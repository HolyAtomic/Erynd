#include <discordcpp/discord_rest_client.hpp>

#include "json_codec.hpp"

#include <algorithm>
#include <cstdint>
#include <exception>
#include <string>
#include <string_view>
#include <type_traits>
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

void redact_secret(std::string& value, std::string_view secret) {
    if (secret.empty()) return;
    constexpr std::string_view replacement = "[REDACTED]";
    std::size_t offset{};
    while ((offset = value.find(secret, offset)) != std::string::npos) {
        value.replace(offset, secret.size(), replacement);
        offset += replacement.size();
    }
}

void redact_headers(Headers& headers, std::string_view secret) {
    Headers sanitized;
    for (const auto& [name, original_value] : headers) {
        auto value = original_value;
        redact_secret(value, secret);
        sanitized.add(name, std::move(value));
    }
    headers = std::move(sanitized);
}

void redact_api_error(DiscordApiError& error, std::string_view secret) {
    redact_secret(error.message, secret);
    redact_secret(error.raw_body, secret);
    redact_headers(error.response_headers, secret);
}

void redact_client_error(DiscordClientError& error, std::string_view secret) {
    std::visit([secret](auto& value) {
        using Error = std::remove_cvref_t<decltype(value)>;
        if constexpr (std::is_same_v<Error, DiscordApiError>) {
            redact_api_error(value, secret);
        } else if constexpr (std::is_same_v<Error, TransportError>) {
            redact_secret(value.message, secret);
        } else if constexpr (std::is_same_v<Error, RateLimitMetadataError>) {
            redact_secret(value.message, secret);
            if (value.response) redact_api_error(*value.response, secret);
        } else if constexpr (std::is_same_v<Error, JsonParseError>) {
            redact_secret(value.path, secret);
            redact_secret(value.message, secret);
            redact_headers(value.response_headers, secret);
        } else if constexpr (std::is_same_v<Error, ModelValidationError>) {
            redact_secret(value.path, secret);
            redact_secret(value.message, secret);
            redact_headers(value.response_headers, secret);
        } else {
            redact_secret(value.message, secret);
        }
    }, error);
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

Result<std::string, LocalValidationError> DiscordApiPathBuilder::finish(const QueryParameters& query) const {
    auto target = finish();
    if (!target.has_value() || query.empty()) return target;
    target.value().push_back('?');
    target.value() += query.serialize();
    return target;
}

DiscordRestClient::DiscordRestClient(RestClient& rest_client, RateLimitCoordinator& coordinator,
    DiscordApiConfig config) noexcept
    : rest_client_{ rest_client }, coordinator_{ coordinator }, config_{ std::move(config) } {}

DiscordRestClient::DiscordRestClient(RestClient& rest_client, RateLimitCoordinator& coordinator,
    DiscordApiConfig config, std::shared_ptr<const BotTokenProvider> token_provider) noexcept
    : rest_client_{ rest_client }, coordinator_{ coordinator }, config_{ std::move(config) },
      token_provider_{ std::move(token_provider) } {}

Result<HttpResponse, DiscordClientError> DiscordRestClient::execute_get(
    const DiscordApiPathBuilder& path, const RouteDescriptor& route,
    std::optional<SteadyClock::time_point> deadline, std::stop_token cancellation) const {
    return execute_request("GET", path, route, {}, {}, deadline, cancellation);
}

Result<HttpResponse, DiscordClientError> DiscordRestClient::execute_request(
    std::string_view method, const DiscordApiPathBuilder& path, const RouteDescriptor& route,
    const Headers& headers, std::string body,
    std::optional<SteadyClock::time_point> deadline, std::stop_token cancellation) const {
    auto request = build_request(method, path, headers, std::move(body), deadline);
    if (!request.has_value()) {
        return Result<HttpResponse, DiscordClientError>::failure(std::move(request).error());
    }

    const auto authorization = request.value().headers.find("Authorization");
    const std::string_view token = authorization && authorization->starts_with("Bot ")
        ? authorization->substr(4) : std::string_view{};
    auto response = [&]() {
        try {
            return rest_client_.execute(request.value(), route, coordinator_, cancellation);
        } catch (...) {
            RestError transport_error{ std::in_place_type<TransportError>, TransportError{
                TransportFailureCategory::io, "HTTP transport raised an exception", true } };
            return RestResult::failure(std::move(transport_error));
        }
    }();
    if (response.state() == ResultState::valueless) {
        return Result<HttpResponse, DiscordClientError>::failure(DiscordClientError{
            std::in_place_type<TransportError>,
            TransportError{ TransportFailureCategory::protocol,
                "REST orchestration returned a valueless result", true }
        });
    }
    if (response.has_error()) {
        auto error = std::move(response).error();
        if (error.valueless_by_exception()) {
            return Result<HttpResponse, DiscordClientError>::failure(DiscordClientError{
                std::in_place_type<TransportError>,
                TransportError{ TransportFailureCategory::protocol,
                    "REST orchestration returned a valueless error", true }
            });
        }
        auto client_error = std::visit([](auto&& value) -> DiscordClientError {
            using Error = std::remove_cvref_t<decltype(value)>;
            return DiscordClientError{ std::in_place_type<Error>, std::forward<decltype(value)>(value) };
        }, std::move(error));
        redact_client_error(client_error, token);
        return Result<HttpResponse, DiscordClientError>::failure(std::move(client_error));
    }
    return Result<HttpResponse, DiscordClientError>::success(std::move(response).value());
}

Result<User, DiscordClientError> DiscordRestClient::get_current_user(
    std::optional<SteadyClock::time_point> deadline, std::stop_token cancellation) const {
    DiscordApiPathBuilder path{ config_.api_version };
    (void)path.append_literal("users");
    (void)path.append_literal("@me");
    const RouteDescriptor route{ "GET", "/users/@me", {}, GlobalLimitClass::ordinary, std::nullopt };
    auto response = execute_get(path, route, deadline, cancellation);
    if (!response.has_value()) return Result<User, DiscordClientError>::failure(std::move(response).error());
    return detail::parse_user_response(response.value());
}

Result<Guild, DiscordClientError> DiscordRestClient::get_guild(Snowflake guild_id,
    std::optional<SteadyClock::time_point> deadline, std::stop_token cancellation) const {
    if (guild_id.value() == 0) {
        return Result<Guild, DiscordClientError>::failure(as_local(local_error(
            LocalValidationIssue::invalid_snowflake, "guild ID must be non-zero")));
    }
    DiscordApiPathBuilder path{ config_.api_version };
    (void)path.append_literal("guilds");
    (void)path.append_snowflake(guild_id);
    const RouteDescriptor route{ "GET", "/guilds/{guild_id}",
        { MajorResource{ MajorResourceKind::guild, guild_id.to_string(), 0 } },
        GlobalLimitClass::ordinary, std::nullopt };
    auto response = execute_get(path, route, deadline, cancellation);
    if (!response.has_value()) return Result<Guild, DiscordClientError>::failure(std::move(response).error());
    return detail::parse_guild_response(response.value());
}

Result<Channel, DiscordClientError> DiscordRestClient::get_channel(Snowflake channel_id,
    std::optional<SteadyClock::time_point> deadline, std::stop_token cancellation) const {
    if (channel_id.value() == 0) {
        return Result<Channel, DiscordClientError>::failure(as_local(local_error(
            LocalValidationIssue::invalid_snowflake, "channel ID must be non-zero")));
    }
    DiscordApiPathBuilder path{ config_.api_version };
    (void)path.append_literal("channels");
    (void)path.append_snowflake(channel_id);
    const RouteDescriptor route{ "GET", "/channels/{channel_id}",
        { MajorResource{ MajorResourceKind::channel, channel_id.to_string(), 0 } },
        GlobalLimitClass::ordinary, std::nullopt };
    auto response = execute_get(path, route, deadline, cancellation);
    if (!response.has_value()) return Result<Channel, DiscordClientError>::failure(std::move(response).error());
    return detail::parse_channel_response(response.value());
}

Result<std::vector<Channel>, DiscordClientError> DiscordRestClient::get_guild_channels(Snowflake guild_id,
    std::optional<SteadyClock::time_point> deadline, std::stop_token cancellation) const {
    if (guild_id.value() == 0) {
        return Result<std::vector<Channel>, DiscordClientError>::failure(as_local(local_error(
            LocalValidationIssue::invalid_snowflake, "guild ID must be non-zero")));
    }
    DiscordApiPathBuilder path{ config_.api_version };
    (void)path.append_literal("guilds");
    (void)path.append_snowflake(guild_id);
    (void)path.append_literal("channels");
    const RouteDescriptor route{ "GET", "/guilds/{guild_id}/channels",
        { MajorResource{ MajorResourceKind::guild, guild_id.to_string(), 0 } },
        GlobalLimitClass::ordinary, std::nullopt };
    auto response = execute_get(path, route, deadline, cancellation);
    if (!response.has_value()) return Result<std::vector<Channel>, DiscordClientError>::failure(std::move(response).error());
    return detail::parse_channel_collection_response(response.value());
}

Result<std::vector<Role>, DiscordClientError> DiscordRestClient::get_guild_roles(Snowflake guild_id,
    std::optional<SteadyClock::time_point> deadline, std::stop_token cancellation) const {
    if (guild_id.value() == 0) {
        return Result<std::vector<Role>, DiscordClientError>::failure(as_local(local_error(
            LocalValidationIssue::invalid_snowflake, "guild ID must be non-zero")));
    }
    DiscordApiPathBuilder path{ config_.api_version };
    (void)path.append_literal("guilds");
    (void)path.append_snowflake(guild_id);
    (void)path.append_literal("roles");
    const RouteDescriptor route{ "GET", "/guilds/{guild_id}/roles",
        { MajorResource{ MajorResourceKind::guild, guild_id.to_string(), 0 } },
        GlobalLimitClass::ordinary, std::nullopt };
    auto response = execute_get(path, route, deadline, cancellation);
    if (!response.has_value()) return Result<std::vector<Role>, DiscordClientError>::failure(std::move(response).error());
    return detail::parse_role_collection_response(response.value());
}

Result<std::vector<Emoji>, DiscordClientError> DiscordRestClient::get_guild_emojis(Snowflake guild_id,
    std::optional<SteadyClock::time_point> deadline, std::stop_token cancellation) const {
    if (guild_id.value() == 0) {
        return Result<std::vector<Emoji>, DiscordClientError>::failure(as_local(local_error(
            LocalValidationIssue::invalid_snowflake, "guild ID must be non-zero")));
    }
    DiscordApiPathBuilder path{ config_.api_version };
    (void)path.append_literal("guilds");
    (void)path.append_snowflake(guild_id);
    (void)path.append_literal("emojis");
    const RouteDescriptor route{ "GET", "/guilds/{guild_id}/emojis",
        { MajorResource{ MajorResourceKind::guild, guild_id.to_string(), 0 } },
        GlobalLimitClass::ordinary, std::nullopt };
    auto response = execute_get(path, route, deadline, cancellation);
    if (!response.has_value()) return Result<std::vector<Emoji>, DiscordClientError>::failure(std::move(response).error());
    return detail::parse_emoji_collection_response(response.value());
}

Result<GuildMember, DiscordClientError> DiscordRestClient::get_guild_member(Snowflake guild_id,
    Snowflake user_id, std::optional<SteadyClock::time_point> deadline, std::stop_token cancellation) const {
    if (guild_id.value() == 0 || user_id.value() == 0) {
        return Result<GuildMember, DiscordClientError>::failure(as_local(local_error(
            LocalValidationIssue::invalid_snowflake, "guild and user IDs must be non-zero")));
    }
    DiscordApiPathBuilder path{ config_.api_version };
    (void)path.append_literal("guilds");
    (void)path.append_snowflake(guild_id);
    (void)path.append_literal("members");
    (void)path.append_snowflake(user_id);
    const RouteDescriptor route{ "GET", "/guilds/{guild_id}/members/{user_id}",
        { MajorResource{ MajorResourceKind::guild, guild_id.to_string(), 0 } },
        GlobalLimitClass::ordinary, std::nullopt };
    auto response = execute_get(path, route, deadline, cancellation);
    if (!response.has_value()) return Result<GuildMember, DiscordClientError>::failure(std::move(response).error());
    return detail::parse_guild_member_response(response.value());
}

Result<Role, DiscordClientError> DiscordRestClient::get_guild_role(Snowflake guild_id,
    Snowflake role_id, std::optional<SteadyClock::time_point> deadline, std::stop_token cancellation) const {
    if (guild_id.value() == 0 || role_id.value() == 0) {
        return Result<Role, DiscordClientError>::failure(as_local(local_error(
            LocalValidationIssue::invalid_snowflake, "guild and role IDs must be non-zero")));
    }
    DiscordApiPathBuilder path{ config_.api_version };
    (void)path.append_literal("guilds");
    (void)path.append_snowflake(guild_id);
    (void)path.append_literal("roles");
    (void)path.append_snowflake(role_id);
    const RouteDescriptor route{ "GET", "/guilds/{guild_id}/roles/{role_id}",
        { MajorResource{ MajorResourceKind::guild, guild_id.to_string(), 0 } },
        GlobalLimitClass::ordinary, std::nullopt };
    auto response = execute_get(path, route, deadline, cancellation);
    if (!response.has_value()) return Result<Role, DiscordClientError>::failure(std::move(response).error());
    return detail::parse_role_response(response.value());
}

Result<void, DiscordClientError> DiscordRestClient::add_guild_member_role(Snowflake guild_id,
    Snowflake user_id, Snowflake role_id, std::optional<SteadyClock::time_point> deadline,
    std::stop_token cancellation) const {
    if (guild_id.value() == 0 || user_id.value() == 0 || role_id.value() == 0) {
        return Result<void, DiscordClientError>::failure(as_local(local_error(
            LocalValidationIssue::invalid_snowflake, "guild, user, and role IDs must be non-zero")));
    }
    DiscordApiPathBuilder path{ config_.api_version };
    (void)path.append_literal("guilds");
    (void)path.append_snowflake(guild_id);
    (void)path.append_literal("members");
    (void)path.append_snowflake(user_id);
    (void)path.append_literal("roles");
    (void)path.append_snowflake(role_id);
    const RouteDescriptor route{ "PUT", "/guilds/{guild_id}/members/{user_id}/roles/{role_id}",
        { MajorResource{ MajorResourceKind::guild, guild_id.to_string(), 0 } },
        GlobalLimitClass::ordinary, std::nullopt };
    auto response = execute_request("PUT", path, route, {}, {}, deadline, cancellation);
    if (!response.has_value()) return Result<void, DiscordClientError>::failure(std::move(response).error());
    if (response.value().status != 204 || !response.value().body.empty()) {
        return Result<void, DiscordClientError>::failure(DiscordClientError{
            std::in_place_type<TransportError>,
            TransportError{ TransportFailureCategory::protocol,
                "Add Guild Member Role requires an empty HTTP 204 response", true }
        });
    }
    return Result<void, DiscordClientError>::success();
}

Result<void, DiscordClientError> DiscordRestClient::remove_guild_member_role(Snowflake guild_id,
    Snowflake user_id, Snowflake role_id, std::optional<SteadyClock::time_point> deadline,
    std::stop_token cancellation) const {
    if (guild_id.value() == 0 || user_id.value() == 0 || role_id.value() == 0) {
        return Result<void, DiscordClientError>::failure(as_local(local_error(
            LocalValidationIssue::invalid_snowflake, "guild, user, and role IDs must be non-zero")));
    }
    DiscordApiPathBuilder path{ config_.api_version };
    (void)path.append_literal("guilds");
    (void)path.append_snowflake(guild_id);
    (void)path.append_literal("members");
    (void)path.append_snowflake(user_id);
    (void)path.append_literal("roles");
    (void)path.append_snowflake(role_id);
    const RouteDescriptor route{ "DELETE", "/guilds/{guild_id}/members/{user_id}/roles/{role_id}",
        { MajorResource{ MajorResourceKind::guild, guild_id.to_string(), 0 } },
        GlobalLimitClass::ordinary, std::nullopt };
    auto response = execute_request("DELETE", path, route, {}, {}, deadline, cancellation);
    if (!response.has_value()) return Result<void, DiscordClientError>::failure(std::move(response).error());
    if (response.value().status != 204 || !response.value().body.empty()) {
        return Result<void, DiscordClientError>::failure(DiscordClientError{
            std::in_place_type<TransportError>,
            TransportError{ TransportFailureCategory::protocol,
                "Remove Guild Member Role requires an empty HTTP 204 response", true }
        });
    }
    return Result<void, DiscordClientError>::success();
}

Result<GuildMember, DiscordClientError> DiscordRestClient::modify_guild_member(Snowflake guild_id,
    Snowflake user_id, const GuildMemberModifyRequest& changes,
    std::optional<SteadyClock::time_point> deadline, std::stop_token cancellation) const {
    if (guild_id.value() == 0 || user_id.value() == 0) {
        return Result<GuildMember, DiscordClientError>::failure(as_local(local_error(
            LocalValidationIssue::invalid_snowflake, "guild and user IDs must be non-zero")));
    }
    if (changes.nick.is_absent() && changes.roles.is_absent()) {
        return Result<GuildMember, DiscordClientError>::failure(as_local(local_error(
            LocalValidationIssue::invalid_request_body, "at least one GuildMember field must be supplied")));
    }
    if (changes.roles.has_value() && std::any_of(changes.roles.value().begin(), changes.roles.value().end(),
        [](Snowflake role_id) { return role_id.value() == 0; })) {
        return Result<GuildMember, DiscordClientError>::failure(as_local(local_error(
            LocalValidationIssue::invalid_snowflake, "role IDs must be non-zero")));
    }

    std::string body;
    try {
        body = detail::encode_guild_member_modify_request(changes);
    } catch (...) {
        return Result<GuildMember, DiscordClientError>::failure(as_local(local_error(
            LocalValidationIssue::invalid_request_body, "GuildMember request body could not be encoded")));
    }
    Headers headers;
    headers.add("Content-Type", "application/json");
    DiscordApiPathBuilder path{ config_.api_version };
    (void)path.append_literal("guilds");
    (void)path.append_snowflake(guild_id);
    (void)path.append_literal("members");
    (void)path.append_snowflake(user_id);
    const RouteDescriptor route{ "PATCH", "/guilds/{guild_id}/members/{user_id}",
        { MajorResource{ MajorResourceKind::guild, guild_id.to_string(), 0 } },
        GlobalLimitClass::ordinary, std::nullopt };
    auto response = execute_request("PATCH", path, route, headers, std::move(body), deadline, cancellation);
    if (!response.has_value()) return Result<GuildMember, DiscordClientError>::failure(std::move(response).error());
    if (response.value().status != 200) {
        return Result<GuildMember, DiscordClientError>::failure(DiscordClientError{
            std::in_place_type<TransportError>,
            TransportError{ TransportFailureCategory::protocol,
                "Modify Guild Member requires an HTTP 200 response with a GuildMember body", true }
        });
    }
    return detail::parse_guild_member_response(response.value());
}

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

Result<HttpRequest, DiscordClientError> DiscordRestClient::build_request_with_query(
    std::string_view method, const DiscordApiPathBuilder& path, const QueryParameters& query,
    const Headers& additional_headers, std::string body,
    std::optional<SteadyClock::time_point> deadline) const {
    auto request = build_request(method, path, additional_headers, std::move(body), deadline);
    if (!request.has_value()) return request;
    if (!query.empty()) {
        request.value().target.push_back('?');
        request.value().target += query.serialize();
    }
    return request;
}

} // namespace discordcpp
