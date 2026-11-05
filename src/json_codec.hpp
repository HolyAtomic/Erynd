#pragma once

#include <discordcpp/discord_client_error.hpp>
#include <discordcpp/discord_models.hpp>
#include <discordcpp/http_types.hpp>
#include <discordcpp/result.hpp>

#include <vector>

#include <nlohmann/json.hpp>

namespace discordcpp::detail {

using Json = nlohmann::json;

[[nodiscard]] Result<User, DiscordClientError> parse_user_response(const HttpResponse& response);
[[nodiscard]] Result<Guild, DiscordClientError> parse_guild_response(const HttpResponse& response);
[[nodiscard]] Result<Channel, DiscordClientError> parse_channel_response(const HttpResponse& response);
[[nodiscard]] Result<Role, DiscordClientError> parse_role_response(const HttpResponse& response);
[[nodiscard]] Result<GuildMember, DiscordClientError> parse_guild_member_response(const HttpResponse& response);
[[nodiscard]] Result<std::vector<Channel>, DiscordClientError> parse_channel_collection_response(
    const HttpResponse& response);
[[nodiscard]] Result<std::vector<Role>, DiscordClientError> parse_role_collection_response(
    const HttpResponse& response);
[[nodiscard]] Result<std::vector<Emoji>, DiscordClientError> parse_emoji_collection_response(
    const HttpResponse& response);

/// Internal JSON value conversion used by request serializers in later steps.
[[nodiscard]] Json serialize_snowflake(const Snowflake& value);

} // namespace discordcpp::detail
