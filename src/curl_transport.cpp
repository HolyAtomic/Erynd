#include <discordcpp/curl_transport.hpp>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <curl/curl.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <climits>
#include <cstdint>
#include <limits>
#include <mutex>
#include <new>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <utility>

namespace discordcpp {
namespace {

class CurlErrorCategory final : public std::error_category {
public:
    const char* name() const noexcept override { return "libcurl"; }
    std::string message(int condition) const override {
        return curl_easy_strerror(static_cast<CURLcode>(condition));
    }
};

const std::error_category& curl_error_category() noexcept {
    static const CurlErrorCategory category;
    return category;
}

struct CurlInitializationState {
    std::once_flag once;
    std::atomic<bool> initialized{};
    CURLcode result{ CURLE_FAILED_INIT };
    CurlHttpTransportCapabilities capabilities{};
};

CurlInitializationState& curl_initialization_state() noexcept {
    // Intentionally no curl_global_cleanup(): this global state belongs to the
    // process and other linked modules may share the same libcurl instance.
    static CurlInitializationState state;
    return state;
}

Result<CurlHttpTransportCapabilities, TransportError> initialize_curl() {
    auto& state = curl_initialization_state();
    std::call_once(state.once, [&state] {
        state.result = curl_global_init(CURL_GLOBAL_DEFAULT);
        if (state.result == CURLE_OK) {
            const auto* version = curl_version_info(CURLVERSION_NOW);
            if (version != nullptr) {
                state.capabilities.thread_safe_global_init = (version->features & CURL_VERSION_THREADSAFE) != 0;
                state.capabilities.asynchronous_dns = (version->features & CURL_VERSION_ASYNCHDNS) != 0;
            }
            state.initialized.store(true, std::memory_order_release);
        }
    });
    if (state.result != CURLE_OK) {
        return Result<CurlHttpTransportCapabilities, TransportError>::failure(
            { TransportFailureCategory::io, "libcurl global initialization failed", false,
                std::error_code{ static_cast<int>(state.result), curl_error_category() } });
    }
    return Result<CurlHttpTransportCapabilities, TransportError>::success(state.capabilities);
}

bool is_token_char(unsigned char byte) noexcept {
    if ((byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') || (byte >= '0' && byte <= '9')) return true;
    switch (byte) {
        case '!': case '#': case '$': case '%': case '&': case '\'': case '*': case '+': case '-':
        case '.': case '^': case '_': case '`': case '|': case '~':
            return true;
        default:
            return false;
    }
}

bool valid_method(std::string_view method) noexcept {
    return !method.empty() && std::all_of(method.begin(), method.end(), [](char ch) {
        return is_token_char(static_cast<unsigned char>(ch));
    });
}

bool valid_target(std::string_view target) noexcept {
    if (target.empty() || target.front() != '/' || target.starts_with("//") || target.find('#') != std::string_view::npos) {
        return false;
    }
    return std::none_of(target.begin(), target.end(), [](unsigned char ch) {
        return ch <= 0x20 || ch == 0x7f;
    });
}

std::string normalized_base_url(std::string base_url) {
    const bool http = base_url.starts_with("http://");
    const bool https = base_url.starts_with("https://");
    if (!http && !https) throw std::invalid_argument("curl transport base_url must use http or https");
    if (base_url.find_first_of("?#") != std::string::npos) {
        throw std::invalid_argument("curl transport base_url must not contain a query or fragment");
    }
    if (std::any_of(base_url.begin(), base_url.end(), [](unsigned char ch) { return ch <= 0x20 || ch == 0x7f; })) {
        throw std::invalid_argument("curl transport base_url must not contain control characters or spaces");
    }
    const auto authority_start = https ? 8u : 7u;
    const auto authority_end = base_url.find('/', authority_start);
    const auto authority = std::string_view{ base_url }.substr(authority_start,
        authority_end == std::string::npos ? std::string::npos : authority_end - authority_start);
    if (authority.empty() || authority.find('@') != std::string_view::npos || authority.find_first_of(" \r\n\t") != std::string_view::npos) {
        throw std::invalid_argument("curl transport base_url must have a host and must not contain user information");
    }
    while (base_url.size() > authority_start && base_url.back() == '/') base_url.pop_back();
    return base_url;
}

std::string joined_url(std::string_view base_url, std::string_view target) {
    std::string url;
    url.reserve(base_url.size() + target.size());
    url.append(base_url);
    url.append(target);
    return url;
}

std::optional<long> duration_ms(std::chrono::milliseconds duration) noexcept {
    if (duration.count() <= 0 || duration.count() > LONG_MAX) return std::nullopt;
    return static_cast<long>(duration.count());
}

std::optional<long> deadline_ms(SteadyClock::time_point deadline, SteadyClock::time_point now) noexcept {
    if (now >= deadline) return 0;
    const auto remaining = deadline - now;
    const auto milliseconds = std::chrono::duration<long double, std::milli>{ remaining }.count();
    // A missing result means a timeout larger than libcurl's long-valued
    // millisecond option; omit that option and rely on the monotonic callback.
    if (!std::isfinite(milliseconds) || milliseconds >= static_cast<long double>(LONG_MAX)) return std::nullopt;
    const auto rounded_up = std::ceil(milliseconds);
    if (rounded_up <= 0.0L || rounded_up > static_cast<long double>(LONG_MAX)) return std::nullopt;
    return static_cast<long>(rounded_up);
}

enum class CallbackFailure {
    none,
    malformed_headers,
    header_allocation,
    header_limit,
    body_allocation,
    body_limit
};

struct TransferContext {
    Headers response_headers;
    std::string response_body;
    std::optional<std::size_t> max_body_bytes;
    std::size_t max_header_bytes{};
    std::size_t max_header_fields{};
    std::size_t max_header_field_bytes{};
    std::size_t response_header_bytes{};
    std::size_t response_header_fields{};
    std::stop_token cancellation;
    std::optional<SteadyClock::time_point> deadline;
    CallbackFailure callback_failure{ CallbackFailure::none };
    bool cancelled{};
    bool timed_out{};
};

std::size_t header_callback(char* data, std::size_t size, std::size_t count, void* opaque) noexcept {
    auto* context = static_cast<TransferContext*>(opaque);
    if (size != 0 && count > std::numeric_limits<std::size_t>::max() / size) {
        context->callback_failure = CallbackFailure::header_limit;
        return 0;
    }
    const auto length = size * count;
    if (length == 0) return 0;
    if (length > context->max_header_bytes ||
        context->response_header_bytes > context->max_header_bytes - length) {
        context->callback_failure = CallbackFailure::header_limit;
        return 0;
    }
    context->response_header_bytes += length;

    const std::string_view line{ data, length };
    if (line.starts_with("HTTP/")) {
        context->response_headers = Headers{};
        context->response_body.clear();
        return length;
    }
    if (line == "\r\n" || line == "\n") return length;
    if (length > context->max_header_field_bytes) {
        context->callback_failure = CallbackFailure::header_limit;
        return 0;
    }
    if (context->response_header_fields >= context->max_header_fields) {
        context->callback_failure = CallbackFailure::header_limit;
        return 0;
    }

    auto field = line;
    while (!field.empty() && (field.back() == '\r' || field.back() == '\n')) field.remove_suffix(1);
    const auto colon = field.find(':');
    if (colon == std::string_view::npos) {
        context->callback_failure = CallbackFailure::malformed_headers;
        return 0;
    }
    auto value = field.substr(colon + 1);
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.remove_prefix(1);
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) value.remove_suffix(1);
    if (std::any_of(value.begin(), value.end(), [](unsigned char ch) {
        return (ch < 0x20 && ch != '\t') || ch == 0x7f;
    })) {
        context->callback_failure = CallbackFailure::malformed_headers;
        return 0;
    }
    try {
        context->response_headers.add(std::string{ field.substr(0, colon) }, std::string{ value });
        ++context->response_header_fields;
    } catch (const std::bad_alloc&) {
        context->callback_failure = CallbackFailure::header_allocation;
        return 0;
    } catch (const std::length_error&) {
        context->callback_failure = CallbackFailure::header_allocation;
        return 0;
    } catch (...) {
        context->callback_failure = CallbackFailure::malformed_headers;
        return 0;
    }
    return length;
}

std::size_t body_callback(char* data, std::size_t size, std::size_t count, void* opaque) noexcept {
    auto* context = static_cast<TransferContext*>(opaque);
    if (size != 0 && count > std::numeric_limits<std::size_t>::max() / size) {
        context->callback_failure = CallbackFailure::body_limit;
        return 0;
    }
    const auto length = size * count;
    if (context->max_body_bytes && (length > *context->max_body_bytes ||
        context->response_body.size() > *context->max_body_bytes - length)) {
        context->callback_failure = CallbackFailure::body_limit;
        return 0;
    }
    try {
        context->response_body.append(data, length);
    } catch (const std::bad_alloc&) {
        context->callback_failure = CallbackFailure::body_allocation;
        return 0;
    } catch (const std::length_error&) {
        context->callback_failure = CallbackFailure::body_allocation;
        return 0;
    } catch (...) {
        context->callback_failure = CallbackFailure::body_allocation;
        return 0;
    }
    return length;
}

int progress_callback(void* opaque, curl_off_t, curl_off_t, curl_off_t, curl_off_t) noexcept {
    auto* context = static_cast<TransferContext*>(opaque);
    if (context->cancellation.stop_requested()) {
        context->cancelled = true;
        return 1;
    }
    if (context->deadline && SteadyClock::now() >= *context->deadline) {
        context->timed_out = true;
        return 1;
    }
    return 0;
}

TransportError transport_error(TransportFailureCategory category, std::string message,
    bool may_have_reached, CURLcode code) {
    return { category, std::move(message), may_have_reached,
        std::error_code{ static_cast<int>(code), curl_error_category() } };
}

TransportResult failure(TransportFailureCategory category, std::string message, bool may_have_reached,
    CURLcode code) {
    return TransportResult::failure(transport_error(category, std::move(message), may_have_reached, code));
}

bool framing_headers_present(const Headers& headers) noexcept {
    return headers.contains("Content-Length") || headers.contains("Transfer-Encoding") || headers.contains("Host");
}

std::optional<TransportResult> callback_error(CallbackFailure failure_reason) {
    switch (failure_reason) {
        case CallbackFailure::malformed_headers:
            return failure(TransportFailureCategory::protocol, "response contained malformed HTTP headers", true,
                CURLE_WEIRD_SERVER_REPLY);
        case CallbackFailure::header_allocation:
            return failure(TransportFailureCategory::io, "memory allocation failed while storing response headers", true,
                CURLE_OUT_OF_MEMORY);
        case CallbackFailure::header_limit:
            return failure(TransportFailureCategory::io, "response headers exceeded configured limits", true,
                CURLE_WRITE_ERROR);
        case CallbackFailure::body_allocation:
            return failure(TransportFailureCategory::io, "memory allocation failed while storing response body", true,
                CURLE_OUT_OF_MEMORY);
        case CallbackFailure::body_limit:
            return failure(TransportFailureCategory::io, "response body exceeded configured limit", true,
                CURLE_WRITE_ERROR);
        case CallbackFailure::none:
            return std::nullopt;
    }
    return std::nullopt;
}

} // namespace

Result<CurlHttpTransportCapabilities, TransportError> CurlHttpTransport::initialize() {
    return initialize_curl();
}

CurlHttpTransport::CurlHttpTransport(CurlHttpTransportOptions options) : options_{ std::move(options) } {
    options_.base_url = normalized_base_url(std::move(options_.base_url));
    const auto connect_ms = duration_ms(options_.connect_timeout);
    const auto default_ms = options_.default_timeout ? duration_ms(*options_.default_timeout) : std::optional<long>{};
    if (!connect_ms) throw std::invalid_argument("curl connect_timeout must be positive and representable in milliseconds");
    if (options_.default_timeout && !default_ms)
        throw std::invalid_argument("curl default_timeout must be positive and representable in milliseconds");
    if (options_.max_response_header_bytes == 0 || options_.max_response_header_fields == 0 ||
        options_.max_response_header_field_bytes == 0) {
        throw std::invalid_argument("curl response header limits must be positive");
    }
}

TransportResult CurlHttpTransport::send(const HttpRequest& request, std::stop_token cancellation) {
    bool transfer_started{};
    try {
        if (cancellation.stop_requested()) {
            return failure(TransportFailureCategory::cancelled, "request cancelled before transport execution", false,
                CURLE_ABORTED_BY_CALLBACK);
        }
        if (request.deadline && SteadyClock::now() >= *request.deadline) {
            return failure(TransportFailureCategory::timeout, "request deadline expired before transport execution", false,
                CURLE_OPERATION_TIMEDOUT);
        }
        if (!valid_method(request.method) || !valid_target(request.target)) {
            return failure(TransportFailureCategory::protocol, "invalid HTTP method or origin-form request target", false,
                CURLE_URL_MALFORMAT);
        }
        if (framing_headers_present(request.headers)) {
            return failure(TransportFailureCategory::protocol,
                "caller-supplied Host, Content-Length, and Transfer-Encoding headers are not supported", false,
                CURLE_BAD_FUNCTION_ARGUMENT);
        }
        for (const auto& [name, value] : request.headers) {
            (void)name;
            if (std::any_of(value.begin(), value.end(), [](unsigned char ch) {
                return (ch < 0x20 && ch != '\t') || ch == 0x7f;
            })) {
                return failure(TransportFailureCategory::protocol, "invalid control character in request header", false,
                    CURLE_BAD_FUNCTION_ARGUMENT);
            }
        }

        auto& init_state = curl_initialization_state();
        if (!init_state.initialized.load(std::memory_order_acquire)) {
            return failure(TransportFailureCategory::io,
                "CurlHttpTransport::initialize() must be called during application startup before send()", false,
                CURLE_FAILED_INIT);
        }
        if (options_.require_async_dns_for_timeouts && !init_state.capabilities.asynchronous_dns) {
            return failure(TransportFailureCategory::protocol,
                "runtime libcurl lacks asynchronous DNS required for bounded request timeouts", false,
                CURLE_NOT_BUILT_IN);
        }

        CURL* handle = curl_easy_init();
        if (!handle) return failure(TransportFailureCategory::io, "libcurl could not allocate a request handle", false,
            CURLE_OUT_OF_MEMORY);
        struct HandleGuard { CURL* value; ~HandleGuard() { curl_easy_cleanup(value); } } handle_guard{ handle };

        const auto url = joined_url(options_.base_url, request.target);
        TransferContext context;
        context.max_body_bytes = options_.max_response_body_bytes;
        context.max_header_bytes = options_.max_response_header_bytes;
        context.max_header_fields = options_.max_response_header_fields;
        context.max_header_field_bytes = options_.max_response_header_field_bytes;
        context.cancellation = cancellation;
        context.deadline = request.deadline;

        curl_slist* header_list{};
        struct HeaderGuard { curl_slist*& value; ~HeaderGuard() { if (value) curl_slist_free_all(value); } } header_guard{ header_list };
        for (const auto& [name, value] : request.headers) {
            std::string field = name + ": " + value;
            auto* appended = curl_slist_append(header_list, field.c_str());
            if (!appended) return failure(TransportFailureCategory::io, "could not allocate request headers", false,
                CURLE_OUT_OF_MEMORY);
            header_list = appended;
        }

        const auto set_option = [handle](CURLoption option, auto value) noexcept {
            return curl_easy_setopt(handle, option, value);
        };
        const auto reject_option = [](CURLcode code, const char* what) {
            return failure(TransportFailureCategory::protocol, what, false, code);
        };
        CURLcode code = set_option(CURLOPT_URL, url.c_str());
        if (code != CURLE_OK) return reject_option(code, "libcurl rejected request URL");
#define DISCORDCPP_SET_LONG(option, value, description) \
        do { code = set_option(option, value); if (code != CURLE_OK) return reject_option(code, description); } while (false)
#define DISCORDCPP_SET_POINTER(option, value, description) \
        do { code = set_option(option, value); if (code != CURLE_OK) return reject_option(code, description); } while (false)
        DISCORDCPP_SET_LONG(CURLOPT_NOSIGNAL, 1L, "libcurl rejected NOSIGNAL configuration");
        DISCORDCPP_SET_LONG(CURLOPT_FOLLOWLOCATION, 0L, "libcurl rejected redirect configuration");
        DISCORDCPP_SET_LONG(CURLOPT_MAXREDIRS, 0L, "libcurl rejected redirect limit configuration");
        DISCORDCPP_SET_LONG(CURLOPT_SSL_VERIFYPEER, 1L, "libcurl rejected TLS peer verification configuration");
        DISCORDCPP_SET_LONG(CURLOPT_SSL_VERIFYHOST, 2L, "libcurl rejected TLS host verification configuration");
        DISCORDCPP_SET_LONG(CURLOPT_PROTOCOLS, CURLPROTO_HTTP | CURLPROTO_HTTPS, "libcurl rejected protocol restriction");
        DISCORDCPP_SET_LONG(CURLOPT_REDIR_PROTOCOLS, CURLPROTO_HTTP | CURLPROTO_HTTPS, "libcurl rejected redirect protocol restriction");
        DISCORDCPP_SET_POINTER(CURLOPT_HEADERFUNCTION, header_callback, "libcurl rejected response header callback");
        DISCORDCPP_SET_POINTER(CURLOPT_HEADERDATA, &context, "libcurl rejected response header context");
        DISCORDCPP_SET_POINTER(CURLOPT_WRITEFUNCTION, body_callback, "libcurl rejected response body callback");
        DISCORDCPP_SET_POINTER(CURLOPT_WRITEDATA, &context, "libcurl rejected response body context");
        DISCORDCPP_SET_POINTER(CURLOPT_XFERINFOFUNCTION, progress_callback, "libcurl rejected progress callback");
        DISCORDCPP_SET_POINTER(CURLOPT_XFERINFODATA, &context, "libcurl rejected progress context");
        DISCORDCPP_SET_LONG(CURLOPT_NOPROGRESS, 0L, "libcurl rejected progress configuration");
#undef DISCORDCPP_SET_LONG
#undef DISCORDCPP_SET_POINTER

        if (header_list) {
            code = set_option(CURLOPT_HTTPHEADER, header_list);
            if (code != CURLE_OK) return reject_option(code, "libcurl rejected request headers");
        }

        const auto connect_timeout = duration_ms(options_.connect_timeout);
        code = set_option(CURLOPT_CONNECTTIMEOUT_MS, *connect_timeout);
        if (code != CURLE_OK) return reject_option(code, "libcurl rejected connection timeout");

        std::optional<long> total_timeout;
        if (request.deadline) total_timeout = deadline_ms(*request.deadline, SteadyClock::now());
        else if (options_.default_timeout) total_timeout = duration_ms(*options_.default_timeout);
        if (request.deadline && total_timeout == 0) {
            return failure(TransportFailureCategory::timeout, "request deadline expired before transport execution", false,
                CURLE_OPERATION_TIMEDOUT);
        }
        if (total_timeout) {
            code = set_option(CURLOPT_TIMEOUT_MS, *total_timeout);
            if (code != CURLE_OK) return reject_option(code, "libcurl rejected request timeout");
        }

        if (request.method == "GET" && request.body.empty()) {
            code = set_option(CURLOPT_HTTPGET, 1L);
            if (code != CURLE_OK) return reject_option(code, "libcurl rejected GET method");
        } else if (request.method == "HEAD") {
            if (!request.body.empty()) return failure(TransportFailureCategory::protocol,
                "request bodies with HEAD are not supported", false, CURLE_BAD_FUNCTION_ARGUMENT);
            code = set_option(CURLOPT_NOBODY, 1L);
            if (code != CURLE_OK) return reject_option(code, "libcurl rejected HEAD method");
        } else if (request.method == "POST") {
            code = set_option(CURLOPT_POST, 1L);
            if (code != CURLE_OK) return reject_option(code, "libcurl rejected POST method");
        } else if (request.method != "GET") {
            code = set_option(CURLOPT_CUSTOMREQUEST, request.method.c_str());
            if (code != CURLE_OK) return reject_option(code, "libcurl rejected custom HTTP method");
        }
        if (!request.body.empty() || request.method == "POST") {
            if (request.body.size() > static_cast<std::size_t>(std::numeric_limits<curl_off_t>::max())) {
                return failure(TransportFailureCategory::protocol, "request body is too large for libcurl", false,
                    CURLE_BAD_FUNCTION_ARGUMENT);
            }
            code = set_option(CURLOPT_POSTFIELDS, request.body.data());
            if (code != CURLE_OK) return reject_option(code, "libcurl rejected request body buffer");
            code = set_option(CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(request.body.size()));
            if (code != CURLE_OK) return reject_option(code, "libcurl rejected request body size");
            if (request.method != "POST") {
                code = set_option(CURLOPT_CUSTOMREQUEST, request.method.c_str());
                if (code != CURLE_OK) return reject_option(code, "libcurl rejected custom HTTP method");
            }
        }

        if (cancellation.stop_requested()) {
            return failure(TransportFailureCategory::cancelled, "request cancelled before transport execution", false,
                CURLE_ABORTED_BY_CALLBACK);
        }
        if (request.deadline && SteadyClock::now() >= *request.deadline) {
            return failure(TransportFailureCategory::timeout, "request deadline expired before transport execution", false,
                CURLE_OPERATION_TIMEDOUT);
        }
        transfer_started = true;
        const auto result = curl_easy_perform(handle);
        long response_status{};
        const auto info_result = curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &response_status);

        if (context.cancelled || (result == CURLE_ABORTED_BY_CALLBACK && cancellation.stop_requested())) {
            return failure(TransportFailureCategory::cancelled, "request cancelled during transport execution", true,
                result == CURLE_OK ? CURLE_ABORTED_BY_CALLBACK : result);
        }
        if (context.timed_out || result == CURLE_OPERATION_TIMEDOUT ||
            (request.deadline && SteadyClock::now() >= *request.deadline)) {
            return failure(TransportFailureCategory::timeout, "request timed out or exceeded its deadline", true,
                result == CURLE_OK ? CURLE_OPERATION_TIMEDOUT : result);
        }
        if (const auto callback_failure = callback_error(context.callback_failure)) return *callback_failure;

        if (result != CURLE_OK) {
            switch (result) {
                case CURLE_COULDNT_RESOLVE_HOST:
                case CURLE_COULDNT_RESOLVE_PROXY:
                    return failure(TransportFailureCategory::dns, "DNS resolution failed", false, result);
                case CURLE_COULDNT_CONNECT:
                    return failure(TransportFailureCategory::connect, "connection could not be established", false, result);
                case CURLE_URL_MALFORMAT:
                case CURLE_BAD_FUNCTION_ARGUMENT:
                    return failure(TransportFailureCategory::protocol, "invalid HTTP transport configuration", false, result);
                case CURLE_WEIRD_SERVER_REPLY:
                case CURLE_HTTP2:
                case CURLE_HTTP2_STREAM:
                case CURLE_HTTP3:
                case CURLE_UNSUPPORTED_PROTOCOL:
                    return failure(TransportFailureCategory::protocol, "server response used an invalid or unsupported HTTP protocol", true, result);
                default:
                    break;
            }
            if (result == CURLE_SSL_CONNECT_ERROR || result == CURLE_PEER_FAILED_VERIFICATION ||
                result == CURLE_SSL_CERTPROBLEM || result == CURLE_SSL_CIPHER || result == CURLE_USE_SSL_FAILED ||
                result == CURLE_SSL_ENGINE_NOTFOUND || result == CURLE_SSL_ENGINE_SETFAILED ||
                result == CURLE_SSL_CACERT_BADFILE || result == CURLE_SSL_CRL_BADFILE ||
                result == CURLE_SSL_ISSUER_ERROR || result == CURLE_SSL_PINNEDPUBKEYNOTMATCH ||
                result == CURLE_SSL_INVALIDCERTSTATUS || result == CURLE_SSL_CLIENTCERT) {
                return failure(TransportFailureCategory::tls, "TLS negotiation or certificate verification failed", false, result);
            }
            return failure(TransportFailureCategory::io, "libcurl HTTP operation failed", true, result);
        }
        if (info_result != CURLE_OK) {
            return failure(TransportFailureCategory::protocol, "libcurl could not retrieve the final HTTP status",
                true, info_result);
        }
        if (response_status < 100 || response_status > 599) {
            return failure(TransportFailureCategory::protocol, "libcurl returned an invalid HTTP response", true,
                CURLE_WEIRD_SERVER_REPLY);
        }
        if (response_status < 200) {
            return failure(TransportFailureCategory::protocol, "libcurl returned a non-final informational response", true,
                CURLE_WEIRD_SERVER_REPLY);
        }
        return TransportResult::success({ static_cast<int>(response_status), std::move(context.response_headers),
            std::move(context.response_body) });
    } catch (const std::bad_alloc&) {
        return failure(TransportFailureCategory::io, "memory allocation failed in libcurl transport",
            transfer_started, CURLE_OUT_OF_MEMORY);
    } catch (...) {
        return failure(TransportFailureCategory::io, "unexpected failure in libcurl transport",
            transfer_started, CURLE_FAILED_INIT);
    }
}

} // namespace discordcpp
