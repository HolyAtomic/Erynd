#include <discordcpp/discord_rest_client.hpp>

// Keep the selected JSON library private to the compiled REST API component.
#include <nlohmann/json.hpp>

#include <utility>

namespace discordcpp {

Result<std::string, LocalValidationError> DiscordApiConfig::make_target(
    std::string_view endpoint_path) const {
    if (api_version == 0) {
        return Result<std::string, LocalValidationError>::failure({
            LocalValidationIssue::invalid_api_version,
            "Discord API version must be greater than zero"
        });
    }

    if (endpoint_path.empty() || endpoint_path.front() != '/' ||
        endpoint_path.starts_with("//") || endpoint_path.find('#') != std::string_view::npos ||
        endpoint_path.starts_with("/api/") ||
        endpoint_path.find('\r') != std::string_view::npos ||
        endpoint_path.find('\n') != std::string_view::npos) {
        return Result<std::string, LocalValidationError>::failure({
            LocalValidationIssue::invalid_endpoint_path,
            "Discord endpoint must be an origin-form path beginning with one slash"
        });
    }

    std::string target{ "/api/v" };
    target += std::to_string(api_version);
    target += endpoint_path;
    return Result<std::string, LocalValidationError>::success(std::move(target));
}

DiscordRestClient::DiscordRestClient(RestClient& rest_client, RateLimitCoordinator& coordinator,
    DiscordApiConfig config) noexcept
    : rest_client_{ rest_client }, coordinator_{ coordinator }, config_{ config } {}

} // namespace discordcpp
