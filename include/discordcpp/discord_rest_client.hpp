#pragma once

#include <discordcpp/discord_client_error.hpp>
#include <discordcpp/rate_limit.hpp>
#include <discordcpp/rest_client.hpp>
#include <discordcpp/result.hpp>

#include <cstdint>
#include <string>
#include <string_view>

namespace discordcpp {

/// Minimal settings for the Discord-specific layer. The origin is owned by the
/// injected transport; this type owns only API path versioning. When using
/// CurlHttpTransport with this client, configure its base_url to the origin
/// ("https://discord.com"). Its existing default already includes /api/v10 and
/// is preserved for low-level RestClient callers.
struct DiscordApiConfig final {
    std::uint32_t api_version{ 10 };

    /// Convert an endpoint suffix such as "/guilds/123" to the origin-form
    /// target "/api/v10/guilds/123". Absolute URLs, empty paths, fragments,
    /// and line breaks are rejected before dispatch.
    [[nodiscard]] Result<std::string, LocalValidationError> make_target(
        std::string_view endpoint_path) const;
};

/// Discord-specific client shell. It borrows its low-level RestClient and
/// rate-limit coordinator; both objects must outlive this instance. The client
/// adds no synchronization. Concurrent use requires the underlying transport
/// and coordinator to support it. Authentication and endpoint methods are
/// intentionally introduced in later steps.
///
/// Authentication follow-up contract: the client will own a
/// shared_ptr<const BotTokenProvider>, keeping the provider alive. Providers
/// must support concurrent token access. Token views will be callback-scoped
/// and may not escape request construction, so rotating implementations can
/// synchronize storage without returning dangling views. The client will not
/// retain a separate credential copy; request and transport buffers may hold
/// transient copies as required to send the header.
class DiscordRestClient final {
public:
    DiscordRestClient(RestClient& rest_client, RateLimitCoordinator& coordinator,
        DiscordApiConfig config = {}) noexcept;

    [[nodiscard]] const DiscordApiConfig& config() const noexcept { return config_; }

private:
    RestClient& rest_client_;
    RateLimitCoordinator& coordinator_;
    DiscordApiConfig config_;
};

} // namespace discordcpp
