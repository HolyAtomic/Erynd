#include <discordcpp/discord_rest_client.hpp>

// Keep the selected JSON library private to the compiled REST API component.
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <exception>
#include <string>
#include <string_view>
#include <utility>

namespace discordcpp {
namespace {

LocalValidationError local_error(LocalValidationIssue issue, std::string message) {
    return { issue, std::move(message) };
}

bool valid_dns_host(std::string_view host) noexcept {
    if (host.empty() || host.size() > 253 || host.front() == '.' || host.back() == '.') return false;
    std::size_t label_start{};
    while (label_start < host.size()) {
        const auto separator = host.find('.', label_start);
        const auto label_end = separator == std::string_view::npos ? host.size() : separator;
        const auto label = host.substr(label_start, label_end - label_start);
        if (label.empty() || label.size() > 63 || label.front() == '-' || label.back() == '-') return false;
        for (const unsigned char ch : label) {
            if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                (ch >= '0' && ch <= '9') || ch == '-')) return false;
        }
        if (separator == std::string_view::npos) break;
        label_start = separator + 1;
    }
    return true;
}

bool valid_origin(std::string_view origin) noexcept {
    constexpr std::string_view scheme = "https://";
    if (!origin.starts_with(scheme)) return false;
    auto authority = origin.substr(scheme.size());
    if (!authority.empty() && authority.back() == '/') authority.remove_suffix(1);
    if (authority.empty() || authority.find_first_of("/@?#\\") != std::string_view::npos) return false;
    const auto colon = authority.rfind(':');
    auto host = authority;
    if (colon != std::string_view::npos) {
        host = authority.substr(0, colon);
        const auto port = authority.substr(colon + 1);
        if (port.empty()) return false;
        unsigned value{};
        for (const unsigned char ch : port) {
            if (ch < '0' || ch > '9') return false;
            const auto digit = static_cast<unsigned>(ch - '0');
            if (value > (65535U - digit) / 10U) return false;
            value = value * 10U + digit;
        }
        if (value == 0) return false;
    }
    return valid_dns_host(host);
}

bool is_token_char(unsigned char ch) noexcept;

bool valid_user_agent(std::string_view user_agent) noexcept {
    std::size_t position{};
    const auto consume_product = [&]() {
        const auto start = position;
        while (position < user_agent.size() && is_token_char(static_cast<unsigned char>(user_agent[position]))) ++position;
        if (position == start) return false;
        if (position < user_agent.size() && user_agent[position] == '/') {
            ++position;
            const auto version_start = position;
            while (position < user_agent.size() && is_token_char(static_cast<unsigned char>(user_agent[position]))) ++position;
            if (position == version_start) return false;
        }
        return true;
    };
    const auto consume_comment = [&]() {
        if (position >= user_agent.size() || user_agent[position] != '(') return false;
        ++position;
        unsigned depth = 1;
        while (position < user_agent.size() && depth != 0) {
            const auto ch = static_cast<unsigned char>(user_agent[position++]);
            if (ch < 0x20 || ch > 0x7e) return false;
            if (ch == '\\') {
                if (position == user_agent.size()) return false;
                const auto escaped = static_cast<unsigned char>(user_agent[position++]);
                if (escaped < 0x20 || escaped > 0x7e) return false;
            } else if (ch == '(') {
                ++depth;
            } else if (ch == ')') {
                --depth;
            }
        }
        return depth == 0;
    };

    if (!consume_product()) return false;
    while (position < user_agent.size()) {
        const auto whitespace_start = position;
        while (position < user_agent.size() && (user_agent[position] == ' ' || user_agent[position] == '\t')) ++position;
        if (position == whitespace_start || position == user_agent.size()) return false;
        if (user_agent[position] == '(') {
            if (!consume_comment()) return false;
        } else if (!consume_product()) {
            return false;
        }
    }
    return true;
}

bool is_token_char(unsigned char ch) noexcept {
    if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9')) return true;
    switch (ch) {
        case '!': case '#': case '$': case '%': case '&': case '\'': case '*': case '+':
        case '-': case '.': case '^': case '_': case '`': case '|': case '~': return true;
        default: return false;
    }
}

bool valid_method(std::string_view method) noexcept {
    return !method.empty() && std::all_of(method.begin(), method.end(), [](unsigned char ch) {
        return is_token_char(ch);
    });
}

bool valid_request_header_value(std::string_view value) noexcept {
    return std::all_of(value.begin(), value.end(), [](unsigned char ch) {
        return ch == '\t' || (ch >= 0x20 && ch != 0x7f);
    });
}

bool reserved_header(std::string_view name) noexcept {
    const auto equal_ascii = [name](std::string_view expected) {
        if (name.size() != expected.size()) return false;
        for (std::size_t index = 0; index < name.size(); ++index) {
            auto ch = static_cast<unsigned char>(name[index]);
            if (ch >= 'A' && ch <= 'Z') ch = static_cast<unsigned char>(ch + ('a' - 'A'));
            if (ch != static_cast<unsigned char>(expected[index])) return false;
        }
        return true;
    };
    return equal_ascii("authorization") || equal_ascii("user-agent") || equal_ascii("host") ||
        equal_ascii("content-length") || equal_ascii("transfer-encoding");
}

DiscordClientError as_local(LocalValidationError error) {
    return DiscordClientError{ std::in_place_type<LocalValidationError>, std::move(error) };
}

} // namespace

Result<void, LocalValidationError> DiscordApiConfig::validate() const {
    if (api_version == 0) {
        return Result<void, LocalValidationError>::failure(local_error(
            LocalValidationIssue::invalid_api_version, "Discord API version must be greater than zero"));
    }
    if (!valid_origin(api_origin)) {
        return Result<void, LocalValidationError>::failure(local_error(
            LocalValidationIssue::invalid_api_origin,
            "API origin must be an HTTPS DNS origin with no credentials, path, query, or fragment"));
    }
    if (!valid_user_agent(user_agent)) {
        return Result<void, LocalValidationError>::failure(local_error(
            LocalValidationIssue::invalid_user_agent,
            "User-Agent must be non-empty printable ASCII without control characters"));
    }
    return Result<void, LocalValidationError>::success();
}

Result<std::string, LocalValidationError> DiscordApiConfig::make_target(
    std::string_view endpoint_path) const {
    auto config_valid = validate();
    if (!config_valid.has_value()) {
        return Result<std::string, LocalValidationError>::failure(std::move(config_valid).error());
    }

    if (endpoint_path.empty() || endpoint_path.front() != '/' ||
        endpoint_path.starts_with("//") || endpoint_path.find('#') != std::string_view::npos ||
        endpoint_path.find('?') != std::string_view::npos || endpoint_path.starts_with("/api/") ||
        endpoint_path.find('\r') != std::string_view::npos ||
        endpoint_path.find('\n') != std::string_view::npos || endpoint_path.find('\\') != std::string_view::npos ||
        endpoint_path.find('%') != std::string_view::npos) {
        return Result<std::string, LocalValidationError>::failure({
            LocalValidationIssue::invalid_endpoint_path,
            "Discord endpoint must be an unescaped origin-form path beginning with one slash and no query"
        });
    }

    std::size_t segment_start = 1;
    while (segment_start <= endpoint_path.size()) {
        const auto separator = endpoint_path.find('/', segment_start);
        const auto segment_end = separator == std::string_view::npos ? endpoint_path.size() : separator;
        const auto segment = endpoint_path.substr(segment_start, segment_end - segment_start);
        if (segment.empty() || segment == "." || segment == "..") {
            return Result<std::string, LocalValidationError>::failure({
                LocalValidationIssue::invalid_endpoint_path, "Discord endpoint contains an invalid path segment" });
        }
        for (const unsigned char ch : segment) {
            if (!is_token_char(ch) && ch != ':' && ch != '@') {
                return Result<std::string, LocalValidationError>::failure({
                    LocalValidationIssue::invalid_endpoint_path, "Discord endpoint contains an invalid path character" });
            }
        }
        if (separator == std::string_view::npos) break;
        segment_start = separator + 1;
    }

    std::string target{ "/api/v" };
    target += std::to_string(api_version);
    target += endpoint_path;
    return Result<std::string, LocalValidationError>::success(std::move(target));
}

DiscordApiPathBuilder::DiscordApiPathBuilder(std::uint32_t api_version)
    : api_version_{ api_version }, path_{ "/api/v" + std::to_string(api_version) } {
    if (api_version == 0) {
        error_ = local_error(LocalValidationIssue::invalid_api_version,
            "Discord API version must be greater than zero");
    }
}

Result<void, LocalValidationError> DiscordApiPathBuilder::append_literal(std::string_view segment) {
    if (error_) return Result<void, LocalValidationError>::failure(*error_);
    if (segment.empty() || segment == "." || segment == ".." ||
        !std::all_of(segment.begin(), segment.end(), [](unsigned char ch) {
            return (is_token_char(ch) && ch != '%' && ch != '#') || ch == ':' || ch == '@';
        })) {
        error_ = local_error(LocalValidationIssue::invalid_path_segment,
            "literal path segment contains invalid or structural characters");
        return Result<void, LocalValidationError>::failure(*error_);
    }
    path_.push_back('/');
    path_.append(segment);
    return Result<void, LocalValidationError>::success();
}

Result<void, LocalValidationError> DiscordApiPathBuilder::append_parameter(std::string_view segment) {
    if (error_) return Result<void, LocalValidationError>::failure(*error_);
    if (segment.empty() || segment == "." || segment == ".." ||
        segment.find_first_of("/\\?#\r\n") != std::string_view::npos ||
        std::any_of(segment.begin(), segment.end(), [](unsigned char ch) { return ch < 0x20 || ch == 0x7f; })) {
        error_ = local_error(LocalValidationIssue::invalid_path_segment,
            "path parameter must be a non-empty single segment without separators or dot traversal");
        return Result<void, LocalValidationError>::failure(*error_);
    }
    constexpr char hex[] = "0123456789ABCDEF";
    path_.push_back('/');
    for (const unsigned char ch : segment) {
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
            (ch >= '0' && ch <= '9') || ch == '-' || ch == '.' || ch == '_' || ch == '~') {
            path_.push_back(static_cast<char>(ch));
        } else {
            path_.push_back('%');
            path_.push_back(hex[ch >> 4]);
            path_.push_back(hex[ch & 0x0f]);
        }
    }
    return Result<void, LocalValidationError>::success();
}

Result<void, LocalValidationError> DiscordApiPathBuilder::append_snowflake(Snowflake value) {
    return append_parameter(value.to_string());
}

Result<std::string, LocalValidationError> DiscordApiPathBuilder::finish() const {
    if (error_) return Result<std::string, LocalValidationError>::failure(*error_);
    if (path_ == "/api/v" + std::to_string(api_version_)) {
        return Result<std::string, LocalValidationError>::failure(local_error(
            LocalValidationIssue::invalid_endpoint_path, "Discord API path must contain an endpoint"));
    }
    return Result<std::string, LocalValidationError>::success(path_);
}

DiscordRestClient::DiscordRestClient(RestClient& rest_client, RateLimitCoordinator& coordinator,
    DiscordApiConfig config) noexcept
    : rest_client_{ rest_client }, coordinator_{ coordinator }, config_{ std::move(config) } {}

DiscordRestClient::DiscordRestClient(RestClient& rest_client, RateLimitCoordinator& coordinator,
    DiscordApiConfig config, std::shared_ptr<const BotTokenProvider> token_provider) noexcept
    : rest_client_{ rest_client }, coordinator_{ coordinator }, config_{ std::move(config) },
      token_provider_{ std::move(token_provider) } {}

Result<HttpRequest, DiscordClientError> DiscordRestClient::build_request(
    std::string_view method, std::string_view endpoint_path, const Headers& additional_headers,
    std::string body, std::optional<SteadyClock::time_point> deadline) const {
    const auto target = config_.make_target(endpoint_path);
    if (!target.has_value()) {
        return Result<HttpRequest, DiscordClientError>::failure(as_local(target.error()));
    }
    DiscordApiPathBuilder builder{ config_.api_version };
    const auto segments = endpoint_path.substr(1);
    std::size_t segment_start{};
    while (segment_start <= segments.size()) {
        const auto separator = segments.find('/', segment_start);
        const auto segment_end = separator == std::string_view::npos ? segments.size() : separator;
        const auto added = builder.append_literal(segments.substr(segment_start, segment_end - segment_start));
        if (!added.has_value()) return Result<HttpRequest, DiscordClientError>::failure(as_local(added.error()));
        if (separator == std::string_view::npos) break;
        segment_start = separator + 1;
    }
    return build_request(method, builder, additional_headers, std::move(body), deadline);
}

Result<HttpRequest, DiscordClientError> DiscordRestClient::build_request(
    std::string_view method, const DiscordApiPathBuilder& path, const Headers& additional_headers,
    std::string body, std::optional<SteadyClock::time_point> deadline) const {
    const auto config_valid = config_.validate();
    if (!config_valid.has_value()) {
        return Result<HttpRequest, DiscordClientError>::failure(as_local(config_valid.error()));
    }
    if (!valid_method(method)) {
        return Result<HttpRequest, DiscordClientError>::failure(as_local(local_error(
            LocalValidationIssue::invalid_http_method, "HTTP method must be a non-empty token")));
    }
    const auto target = path.finish();
    if (!target.has_value()) return Result<HttpRequest, DiscordClientError>::failure(as_local(target.error()));
    const auto expected_prefix = "/api/v" + std::to_string(config_.api_version) + "/";
    if (!target.value().starts_with(expected_prefix)) {
        return Result<HttpRequest, DiscordClientError>::failure(as_local(local_error(
            LocalValidationIssue::invalid_api_version, "path builder API version does not match client configuration")));
    }

    for (const auto& [name, value] : additional_headers) {
        if (reserved_header(name)) {
            return Result<HttpRequest, DiscordClientError>::failure(as_local(local_error(
                LocalValidationIssue::reserved_request_header,
                "caller headers cannot override authentication or transport framing headers")));
        }
        if (!valid_request_header_value(value)) {
            return Result<HttpRequest, DiscordClientError>::failure(as_local(local_error(
                LocalValidationIssue::invalid_request_header, "request header contains an invalid control character")));
        }
    }
    if (!token_provider_) {
        return Result<HttpRequest, DiscordClientError>::failure(as_local(local_error(
            LocalValidationIssue::token_unavailable, "bot token provider is not configured")));
    }

    HttpRequest request{};
    request.method = std::string{ method };
    request.target = target.value();
    request.body = std::move(body);
    request.deadline = deadline;
    for (const auto& [name, value] : additional_headers) request.headers.add(name, value);
    request.headers.add("User-Agent", config_.user_agent);

    bool token_available{};
    bool callback_called{};
    bool callback_violation{};
    try {
        token_available = token_provider_->with_token([&request, &callback_called, &callback_violation](std::string_view token) {
            if (callback_called) {
                callback_violation = true;
                return;
            }
            callback_called = true;
            if (token.empty() || std::any_of(token.begin(), token.end(), [](unsigned char ch) {
                return ch < 0x21 || ch > 0x7e;
            })) {
                return;
            }
            request.headers.add("Authorization", "Bot " + std::string{ token });
        });
    } catch (...) {
        token_available = false;
    }
    if (!token_available || callback_violation || !callback_called || !request.headers.contains("Authorization")) {
        return Result<HttpRequest, DiscordClientError>::failure(as_local(local_error(
            LocalValidationIssue::token_unavailable, "bot token is unavailable or invalid")));
    }
    return Result<HttpRequest, DiscordClientError>::success(std::move(request));
}

} // namespace discordcpp
