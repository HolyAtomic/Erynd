#include <discordcpp/curl_transport.hpp>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <curl/curl.h>

#include <algorithm>
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

CURLcode initialize_curl() noexcept {
    static std::once_flag once;
    static CURLcode result = CURLE_FAILED_INIT;
    std::call_once(once, [] { result = curl_global_init(CURL_GLOBAL_DEFAULT); });
    return result;
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
    if (duration.count() <= 0) return std::nullopt;
    return static_cast<long>(std::min<std::int64_t>(duration.count(), LONG_MAX));
}

std::optional<long> deadline_ms(SteadyClock::time_point deadline, SteadyClock::time_point now) noexcept {
    if (now >= deadline) return 0;
    const auto remaining = deadline - now;
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(remaining);
    auto count = ms.count();
    if (std::chrono::duration_cast<SteadyClock::duration>(ms) < remaining) ++count;
    return static_cast<long>(std::min<std::int64_t>(count, LONG_MAX));
}

struct TransferContext {
    Headers response_headers;
    std::string response_body;
    std::optional<std::size_t> max_body_bytes;
    std::stop_token cancellation;
    std::optional<SteadyClock::time_point> deadline;
    bool cancelled{};
    bool timed_out{};
    bool malformed_headers{};
    bool response_too_large{};
};

std::size_t header_callback(char* data, std::size_t size, std::size_t count, void* opaque) noexcept {
    auto* context = static_cast<TransferContext*>(opaque);
    if (size != 0 && count > std::numeric_limits<std::size_t>::max() / size) return 0;
    const auto length = size * count;
    if (length == 0) return 0;
    const std::string_view line{ data, length };
    if (line.starts_with("HTTP/")) {
        context->response_headers = Headers{};
        context->response_body.clear();
        return length;
    }
    if (line == "\r\n" || line == "\n") return length;
    auto field = line;
    while (!field.empty() && (field.back() == '\r' || field.back() == '\n')) field.remove_suffix(1);
    const auto colon = field.find(':');
    if (colon == std::string_view::npos) {
        context->malformed_headers = true;
        return 0;
    }
    auto value = field.substr(colon + 1);
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.remove_prefix(1);
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) value.remove_suffix(1);
    try {
        context->response_headers.add(std::string{ field.substr(0, colon) }, std::string{ value });
    } catch (...) {
        context->malformed_headers = true;
        return 0;
    }
    return length;
}

std::size_t body_callback(char* data, std::size_t size, std::size_t count, void* opaque) noexcept {
    auto* context = static_cast<TransferContext*>(opaque);
    if (size != 0 && count > std::numeric_limits<std::size_t>::max() / size) return 0;
    const auto length = size * count;
    if (context->max_body_bytes && (length > *context->max_body_bytes ||
        context->response_body.size() > *context->max_body_bytes - length)) {
        context->response_too_large = true;
        return 0;
    }
    try {
        context->response_body.append(data, length);
    } catch (...) {
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

bool setopt(CURL* handle, CURLoption option, long value) noexcept {
    return curl_easy_setopt(handle, option, value) == CURLE_OK;
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

} // namespace

CurlHttpTransport::CurlHttpTransport(CurlHttpTransportOptions options) : options_{ std::move(options) } {
    options_.base_url = normalized_base_url(std::move(options_.base_url));
    if (options_.connect_timeout.count() <= 0) throw std::invalid_argument("curl connect_timeout must be positive");
    if (options_.default_timeout && options_.default_timeout->count() <= 0)
        throw std::invalid_argument("curl default_timeout must be positive when set");
}

TransportResult CurlHttpTransport::send(const HttpRequest& request, std::stop_token cancellation) {
    bool transfer_started{};
    try {
    if (cancellation.stop_requested()) {
        return failure(TransportFailureCategory::cancelled, "request cancelled before transport execution", false, CURLE_ABORTED_BY_CALLBACK);
    }
    if (request.deadline && SteadyClock::now() >= *request.deadline) {
        return failure(TransportFailureCategory::timeout, "request deadline expired before transport execution", false, CURLE_OPERATION_TIMEDOUT);
    }
    if (!valid_method(request.method) || !valid_target(request.target)) {
        return failure(TransportFailureCategory::protocol, "invalid HTTP method or origin-form request target", false, CURLE_URL_MALFORMAT);
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

    const auto initialized = initialize_curl();
    if (initialized != CURLE_OK) return failure(TransportFailureCategory::io, "libcurl initialization failed", false, initialized);

    CURL* handle = curl_easy_init();
    if (!handle) return failure(TransportFailureCategory::io, "libcurl could not allocate a request handle", false, CURLE_OUT_OF_MEMORY);
    struct HandleGuard { CURL* value; ~HandleGuard() { curl_easy_cleanup(value); } } handle_guard{ handle };

    const auto url = joined_url(options_.base_url, request.target);
    TransferContext context;
    context.max_body_bytes = options_.max_response_body_bytes;
    context.cancellation = cancellation;
    context.deadline = request.deadline;

    curl_slist* header_list{};
    struct HeaderGuard { curl_slist*& value; ~HeaderGuard() { if (value) curl_slist_free_all(value); } } header_guard{ header_list };
    for (const auto& [name, value] : request.headers) {
        std::string field = name + ": " + value;
        auto* appended = curl_slist_append(header_list, field.c_str());
        if (!appended) return failure(TransportFailureCategory::io, "could not allocate request headers", false, CURLE_OUT_OF_MEMORY);
        header_list = appended;
    }

    if (curl_easy_setopt(handle, CURLOPT_URL, url.c_str()) != CURLE_OK ||
        !setopt(handle, CURLOPT_NOSIGNAL, 1L) || !setopt(handle, CURLOPT_FOLLOWLOCATION, 0L) ||
        !setopt(handle, CURLOPT_MAXREDIRS, 0L) || !setopt(handle, CURLOPT_SSL_VERIFYPEER, 1L) ||
        !setopt(handle, CURLOPT_SSL_VERIFYHOST, 2L) ||
        curl_easy_setopt(handle, CURLOPT_PROTOCOLS, CURLPROTO_HTTP | CURLPROTO_HTTPS) != CURLE_OK ||
        curl_easy_setopt(handle, CURLOPT_REDIR_PROTOCOLS, CURLPROTO_HTTP | CURLPROTO_HTTPS) != CURLE_OK ||
        curl_easy_setopt(handle, CURLOPT_HEADERFUNCTION, header_callback) != CURLE_OK ||
        curl_easy_setopt(handle, CURLOPT_HEADERDATA, &context) != CURLE_OK ||
        curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, body_callback) != CURLE_OK ||
        curl_easy_setopt(handle, CURLOPT_WRITEDATA, &context) != CURLE_OK ||
        curl_easy_setopt(handle, CURLOPT_XFERINFOFUNCTION, progress_callback) != CURLE_OK ||
        curl_easy_setopt(handle, CURLOPT_XFERINFODATA, &context) != CURLE_OK || !setopt(handle, CURLOPT_NOPROGRESS, 0L)) {
        return failure(TransportFailureCategory::protocol, "libcurl rejected request configuration", false, CURLE_UNKNOWN_OPTION);
    }

    if (header_list && curl_easy_setopt(handle, CURLOPT_HTTPHEADER, header_list) != CURLE_OK) {
        return failure(TransportFailureCategory::protocol, "libcurl rejected request headers", false, CURLE_UNKNOWN_OPTION);
    }

    const auto connect_timeout = duration_ms(options_.connect_timeout);
    if (!connect_timeout || curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT_MS, *connect_timeout) != CURLE_OK) {
        return failure(TransportFailureCategory::protocol, "libcurl rejected connection timeout", false, CURLE_UNKNOWN_OPTION);
    }

    std::optional<long> total_timeout;
    if (request.deadline) total_timeout = deadline_ms(*request.deadline, SteadyClock::now());
    else if (options_.default_timeout) total_timeout = duration_ms(*options_.default_timeout);
    if (request.deadline && total_timeout == 0) {
        return failure(TransportFailureCategory::timeout, "request deadline expired before transport execution", false, CURLE_OPERATION_TIMEDOUT);
    }
    if (total_timeout && curl_easy_setopt(handle, CURLOPT_TIMEOUT_MS, *total_timeout) != CURLE_OK) {
        return failure(TransportFailureCategory::protocol, "libcurl rejected request timeout", false, CURLE_UNKNOWN_OPTION);
    }

    if (request.method == "GET" && request.body.empty()) {
        if (curl_easy_setopt(handle, CURLOPT_HTTPGET, 1L) != CURLE_OK)
            return failure(TransportFailureCategory::protocol, "libcurl rejected GET method", false, CURLE_UNKNOWN_OPTION);
    } else if (request.method == "HEAD") {
        if (!request.body.empty()) return failure(TransportFailureCategory::protocol,
            "request bodies with HEAD are not supported", false, CURLE_BAD_FUNCTION_ARGUMENT);
        if (curl_easy_setopt(handle, CURLOPT_NOBODY, 1L) != CURLE_OK)
            return failure(TransportFailureCategory::protocol, "libcurl rejected HEAD method", false, CURLE_UNKNOWN_OPTION);
    } else if (request.method == "POST") {
        if (curl_easy_setopt(handle, CURLOPT_POST, 1L) != CURLE_OK)
            return failure(TransportFailureCategory::protocol, "libcurl rejected POST method", false, CURLE_UNKNOWN_OPTION);
    } else if (request.method != "GET") {
        if (curl_easy_setopt(handle, CURLOPT_CUSTOMREQUEST, request.method.c_str()) != CURLE_OK)
            return failure(TransportFailureCategory::protocol, "libcurl rejected custom HTTP method", false, CURLE_UNKNOWN_OPTION);
    }
    if (!request.body.empty() || request.method == "POST") {
        if (request.body.size() > static_cast<std::size_t>(std::numeric_limits<curl_off_t>::max()) ||
            curl_easy_setopt(handle, CURLOPT_POSTFIELDS, request.body.data()) != CURLE_OK ||
            curl_easy_setopt(handle, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(request.body.size())) != CURLE_OK) {
            return failure(TransportFailureCategory::protocol, "libcurl rejected request body", false, CURLE_BAD_FUNCTION_ARGUMENT);
        }
        if (request.method != "POST" && curl_easy_setopt(handle, CURLOPT_CUSTOMREQUEST, request.method.c_str()) != CURLE_OK) {
            return failure(TransportFailureCategory::protocol, "libcurl rejected custom HTTP method", false, CURLE_UNKNOWN_OPTION);
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
    curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &response_status);
    if (context.cancelled || (result == CURLE_ABORTED_BY_CALLBACK && cancellation.stop_requested())) {
        return failure(TransportFailureCategory::cancelled, "request cancelled during transport execution", true, result);
    }
    if (context.timed_out || result == CURLE_OPERATION_TIMEDOUT ||
        (request.deadline && SteadyClock::now() >= *request.deadline)) {
        return failure(TransportFailureCategory::timeout, "request timed out or exceeded its deadline", true,
            result == CURLE_OK ? CURLE_OPERATION_TIMEDOUT : result);
    }
    if (context.response_too_large) return failure(TransportFailureCategory::io,
        "response body exceeded configured limit", true, CURLE_WRITE_ERROR);
    if (context.malformed_headers) return failure(TransportFailureCategory::protocol,
        "response contained malformed HTTP headers", true, CURLE_WRITE_ERROR);

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

    if (response_status < 100 || response_status > 599) {
        return failure(TransportFailureCategory::protocol, "libcurl returned an invalid HTTP response", true, CURLE_WEIRD_SERVER_REPLY);
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
