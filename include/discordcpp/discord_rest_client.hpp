#pragma once

#include <discordcpp/discord_client_error.hpp>
#include <discordcpp/discord_models.hpp>
#include <discordcpp/rate_limit.hpp>
#include <discordcpp/query_parameters.hpp>
#include <discordcpp/rest_client.hpp>
#include <discordcpp/result.hpp>
#include <discordcpp/snowflake.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <stop_token>

namespace discordcpp {

/// Token access is callback-scoped so a rotating provider can hold a read lock
/// while the client copies the value into the owned Authorization header. A
/// provider must invoke the callback at most once per call and keep the view
/// valid through the callback. Implementations must support concurrent calls
/// and synchronize token rotation against active callbacks. The provider must
/// invoke the callback synchronously, at most once, and must not retain it. The
/// client retains shared ownership of its provider; the provider retains ownership of secrets.
/// The request header and transport may contain transient token copies; secure
/// zeroization is not promised.
class BotTokenProvider {
public:
    using TokenCallback = std::function<void(std::string_view)>;
    virtual ~BotTokenProvider() = default;
    BotTokenProvider() = default;
    BotTokenProvider(const BotTokenProvider&) = delete;
    BotTokenProvider& operator=(const BotTokenProvider&) = delete;

    /// Return false when no token is available. Exceptions are treated as an
    /// unavailable token and are never propagated with provider diagnostics.
    virtual bool with_token(const TokenCallback& callback) const = 0;
};

/// Discord request-construction settings. `api_origin` is the expected HTTPS
/// origin and must match the separately configured transport origin. Generic
/// transports own their origin; when using CurlHttpTransport configure
/// `base_url` to `https://discord.com` (the low-level curl default is unchanged).
/// API versions must be positive; this layer does not pin a version allowlist.
/// The User-Agent defaults to a library-identifying value and is validated
/// against the HTTP product/comment grammar using printable ASCII.
/// No client-level timeout is imposed: callers provide an absolute steady-clock
/// deadline per request, or leave it unset to preserve transport defaults.
struct DiscordApiConfig final {
    std::uint32_t api_version{ 10 };
    std::string api_origin{ "https://discord.com" };
    std::string user_agent{ "Erynd++/0.1.0 (C++ Discord API library)" };

    /// Convert an endpoint suffix such as "/guilds/123" to the origin-form
    /// target "/api/v10/guilds/123". Absolute URLs, empty paths, fragments,
    /// and line breaks are rejected before dispatch.
    [[nodiscard]] Result<std::string, LocalValidationError> make_target(
        std::string_view endpoint_path) const;

    [[nodiscard]] Result<void, LocalValidationError> validate() const;
};

/// Builds an origin-form `/api/vN/...` target from explicit path segments.
/// Literal segments are for fixed route syntax; parameter segments are
/// percent-encoded as a single segment, and Snowflakes use canonical decimals.
class DiscordApiPathBuilder final {
public:
    explicit DiscordApiPathBuilder(std::uint32_t api_version);

    [[nodiscard]] Result<void, LocalValidationError> append_literal(std::string_view segment);
    [[nodiscard]] Result<void, LocalValidationError> append_parameter(std::string_view segment);
    [[nodiscard]] Result<void, LocalValidationError> append_snowflake(Snowflake value);
    [[nodiscard]] Result<std::string, LocalValidationError> finish() const;
    /// Return the same validated origin-form path with encoded query components.
    /// An empty collection leaves the target byte-for-byte unchanged.
    [[nodiscard]] Result<std::string, LocalValidationError> finish(const QueryParameters& query) const;

private:
    std::uint32_t api_version_{};
    std::string path_;
    std::optional<LocalValidationError> error_;
};

/// Discord-specific client shell. It borrows its low-level RestClient and
/// rate-limit coordinator; both objects must outlive this instance. The client
/// adds no synchronization. Concurrent use requires the underlying transport,
/// coordinator, and token provider to support it. It owns shared ownership of
/// the provider. Each endpoint constructs an authenticated owned request and
/// dispatches exactly once through RestClient's coordinated path. Calls are
/// synchronous, do not retry, and return DiscordClientError alternatives for
/// local validation, transport, API/rate-limit metadata, JSON, and model errors.
class DiscordRestClient final {
public:
    DiscordRestClient(RestClient& rest_client, RateLimitCoordinator& coordinator,
        DiscordApiConfig config = {}) noexcept;
    DiscordRestClient(RestClient& rest_client, RateLimitCoordinator& coordinator,
        DiscordApiConfig config, std::shared_ptr<const BotTokenProvider> token_provider) noexcept;

    [[nodiscard]] const DiscordApiConfig& config() const noexcept { return config_; }

    /// Build an owned authenticated request. Caller headers cannot set or
    /// override Authorization, User-Agent, Host, Content-Length, or
    /// Transfer-Encoding. No token is included in returned errors. The request
    /// owns the Authorization header after this function returns.
    [[nodiscard]] Result<HttpRequest, DiscordClientError> build_request(
        std::string_view method, std::string_view endpoint_path,
        const Headers& additional_headers = {}, std::string body = {},
        std::optional<SteadyClock::time_point> deadline = std::nullopt) const;

    /// Construct an authenticated request from an explicit path builder.
    [[nodiscard]] Result<HttpRequest, DiscordClientError> build_request(
        std::string_view method, const DiscordApiPathBuilder& path,
        const Headers& additional_headers = {}, std::string body = {},
        std::optional<SteadyClock::time_point> deadline = std::nullopt) const;

    /// Like build_request(path), with the query serialized after the path.
    [[nodiscard]] Result<HttpRequest, DiscordClientError> build_request_with_query(
        std::string_view method, const DiscordApiPathBuilder& path, const QueryParameters& query,
        const Headers& additional_headers = {}, std::string body = {},
        std::optional<SteadyClock::time_point> deadline = std::nullopt) const;

    /// GET /users/@me. The returned User is the supported partial model; parse
    /// and schema failures retain HTTP response metadata in DiscordClientError.
    [[nodiscard]] Result<User, DiscordClientError> get_current_user(
        std::optional<SteadyClock::time_point> deadline = std::nullopt,
        std::stop_token cancellation = {}) const;

    /// GET /guilds/{guild_id}. Zero is rejected locally because it is not a
    /// usable Discord guild identifier. The route is explicitly keyed by guild.
    [[nodiscard]] Result<Guild, DiscordClientError> get_guild(Snowflake guild_id,
        std::optional<SteadyClock::time_point> deadline = std::nullopt,
        std::stop_token cancellation = {}) const;

    /// GET /channels/{channel_id}. Zero is rejected locally because it is not a
    /// usable Discord channel identifier. The route is explicitly keyed by channel.
    [[nodiscard]] Result<Channel, DiscordClientError> get_channel(Snowflake channel_id,
        std::optional<SteadyClock::time_point> deadline = std::nullopt,
        std::stop_token cancellation = {}) const;

    /// GET /guilds/{guild_id}/channels. Threads are not included by this Discord endpoint.
    [[nodiscard]] Result<std::vector<Channel>, DiscordClientError> get_guild_channels(Snowflake guild_id,
        std::optional<SteadyClock::time_point> deadline = std::nullopt,
        std::stop_token cancellation = {}) const;

    /// GET /guilds/{guild_id}/roles.
    [[nodiscard]] Result<std::vector<Role>, DiscordClientError> get_guild_roles(Snowflake guild_id,
        std::optional<SteadyClock::time_point> deadline = std::nullopt,
        std::stop_token cancellation = {}) const;

    /// GET /guilds/{guild_id}/emojis.
    [[nodiscard]] Result<std::vector<Emoji>, DiscordClientError> get_guild_emojis(Snowflake guild_id,
        std::optional<SteadyClock::time_point> deadline = std::nullopt,
        std::stop_token cancellation = {}) const;

    /// GET /guilds/{guild_id}/members/{user_id}.
    [[nodiscard]] Result<GuildMember, DiscordClientError> get_guild_member(Snowflake guild_id,
        Snowflake user_id, std::optional<SteadyClock::time_point> deadline = std::nullopt,
        std::stop_token cancellation = {}) const;

    /// GET /guilds/{guild_id}/roles/{role_id}. Discord documents this
    /// single-role retrieval endpoint; it returns the existing partial Role.
    [[nodiscard]] Result<Role, DiscordClientError> get_guild_role(Snowflake guild_id,
        Snowflake role_id, std::optional<SteadyClock::time_point> deadline = std::nullopt,
        std::stop_token cancellation = {}) const;

    /// PUT /guilds/{guild_id}/members/{user_id}/roles/{role_id}. Requires
    /// MANAGE_ROLES. Discord returns 204 No Content on success.
    [[nodiscard]] Result<void, DiscordClientError> add_guild_member_role(Snowflake guild_id,
        Snowflake user_id, Snowflake role_id,
        std::optional<SteadyClock::time_point> deadline = std::nullopt,
        std::stop_token cancellation = {}) const;

    /// DELETE /guilds/{guild_id}/members/{user_id}/roles/{role_id}. Requires
    /// MANAGE_ROLES. Discord returns 204 No Content on success.
    [[nodiscard]] Result<void, DiscordClientError> remove_guild_member_role(Snowflake guild_id,
        Snowflake user_id, Snowflake role_id,
        std::optional<SteadyClock::time_point> deadline = std::nullopt,
        std::stop_token cancellation = {}) const;

    /// PATCH /guilds/{guild_id}/members/{user_id}. Discord returns the updated
    /// GuildMember with 200 OK. Supplied `nick` requires MANAGE_NICKNAMES;
    /// supplied `roles` requires MANAGE_ROLES. Discord permission failures are
    /// returned as DiscordApiError and are never bypassed locally.
    [[nodiscard]] Result<GuildMember, DiscordClientError> modify_guild_member(Snowflake guild_id,
        Snowflake user_id, const GuildMemberModifyRequest& changes,
        std::optional<SteadyClock::time_point> deadline = std::nullopt,
        std::stop_token cancellation = {}) const;

    /// POST /channels/{channel_id}/messages. Supports text content only and
    /// returns the created partial Message. In guild channels Discord requires
    /// SEND_MESSAGES. This synchronous operation never retries or replays a POST.
    [[nodiscard]] Result<Message, DiscordClientError> create_message(Snowflake channel_id,
        const CreateMessageRequest& request,
        std::optional<SteadyClock::time_point> deadline = std::nullopt,
        std::stop_token cancellation = {}) const;

    /// GET /channels/{channel_id}/messages/{message_id}. In guild channels,
    /// Discord requires VIEW_CHANNEL and READ_MESSAGE_HISTORY; voice channels
    /// also require CONNECT. Returns the supported partial Message model.
    [[nodiscard]] Result<Message, DiscordClientError> get_message(Snowflake channel_id,
        Snowflake message_id, std::optional<SteadyClock::time_point> deadline = std::nullopt,
        std::stop_token cancellation = {}) const;

private:
    [[nodiscard]] Result<HttpResponse, DiscordClientError> execute_get(
        const DiscordApiPathBuilder& path, const RouteDescriptor& route,
        std::optional<SteadyClock::time_point> deadline, std::stop_token cancellation) const;
    [[nodiscard]] Result<HttpResponse, DiscordClientError> execute_request(
        std::string_view method, const DiscordApiPathBuilder& path, const RouteDescriptor& route,
        const Headers& headers, std::string body,
        std::optional<SteadyClock::time_point> deadline, std::stop_token cancellation) const;

    RestClient& rest_client_;
    RateLimitCoordinator& coordinator_;
    DiscordApiConfig config_;
    std::shared_ptr<const BotTokenProvider> token_provider_;
};

} // namespace discordcpp
