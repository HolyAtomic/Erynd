#pragma once

#include <discordcpp/field.hpp>
#include <discordcpp/snowflake.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace discordcpp {

/// Deliberately partial subset of Discord's User object. Unknown API fields are
/// ignored by the internal decoder. A Field distinguishes omitted and null
/// nullable fields; bot is optional but non-nullable when present.
struct User final {
    Snowflake id{ 0 };
    std::string username;
    Field<std::string> global_name;
    Field<std::string> avatar;
    Field<bool> bot;
};

/// Deliberately partial subset of Discord's Guild object. owner_id is
/// non-nullable in Discord's schema; this partial model permits it to be
/// absent, but a present JSON null is rejected by the decoder.
struct Guild final {
    Snowflake id{ 0 };
    std::string name;
    Field<std::string> icon;
    Field<Snowflake> owner_id;
};

/// Deliberately partial subset of Discord's Channel object. Unknown numeric
/// channel types are preserved in type for forward compatibility. guild_id is
/// optional and non-nullable; name and topic are optional and nullable.
struct Channel final {
    Snowflake id{ 0 };
    std::int32_t type{};
    Field<Snowflake> guild_id;
    Field<std::string> name;
    Field<std::string> topic;
    Field<std::int32_t> position;
};

/// Deliberately partial subset of a Discord guild Role. Permission bits remain
/// a decimal string so their wire representation is not narrowed to an integer.
struct Role final {
    Snowflake id{ 0 };
    std::string name;
    std::string permissions;
    std::int32_t position{};
    std::int32_t color{};
    bool hoist{};
    bool managed{};
    bool mentionable{};
    Field<std::string> icon;
    Field<std::string> unicode_emoji;
};

/// Deliberately partial subset of a custom guild Emoji. The API may omit the
/// optional roles and capability flags depending on the returned representation.
struct Emoji final {
    Snowflake id{ 0 };
    std::string name;
    Field<std::vector<Snowflake>> roles;
    Field<bool> require_colons;
    Field<bool> managed;
    Field<bool> animated;
    Field<bool> available;
};

/// Deliberately partial subset of Discord's Guild Member object. The user is
/// optional; roles and joined_at (which may be null), deaf, and mute are
/// required by the member schema. Timestamp strings retain their ISO 8601 wire
/// form so this model does not impose a date-time dependency.
struct GuildMember final {
    Field<User> user;
    Field<std::string> nick;
    Field<std::string> avatar;
    std::vector<Snowflake> roles;
    Field<std::string> joined_at;
    Field<std::string> premium_since;
    bool deaf{};
    bool mute{};
    Field<bool> pending;
    Field<std::string> permissions;
};

/// Focused request subset for PATCH /guilds/{guild_id}/members/{user_id}.
/// `nick` and `roles` preserve omitted, explicit null, and concrete values.
/// Discord requires MANAGE_NICKNAMES for `nick` and MANAGE_ROLES for `roles`.
/// A concrete roles vector replaces the member's role assignments.
struct GuildMemberModifyRequest final {
    Field<std::string> nick;
    Field<std::vector<Snowflake>> roles;
};

/// Deliberately partial subset of Discord's Message object, decoded from
/// REST responses. `edited_timestamp` is required but nullable in the response;
/// `flags` is optional and non-nullable. Timestamps retain their wire strings
/// after strict RFC 3339 validation. Discord does not document `guild_id` as a
/// Message object field, so it is intentionally not modeled here.
///
/// Attachments, embeds, components, stickers, polls, references, and other
/// message-specific structures are currently omitted.
struct Message final {
    Snowflake id{ 0 };
    Snowflake channel_id{ 0 };
    User author;
    std::string content;
    std::string timestamp;
    Field<std::string> edited_timestamp;
    bool tts{};
    bool mention_everyone{};
    std::vector<User> mentions;
    std::vector<Snowflake> mention_roles;
    bool pinned{};
    std::uint64_t type{};
    Field<std::uint64_t> flags;
};

/// Focused text-only request for POST /channels/{channel_id}/messages.
/// Content must be non-empty, valid UTF-8, and at most 2,000 Unicode scalar
/// values. Other Discord message payload fields are intentionally unsupported.
struct CreateMessageRequest final {
    std::string content;
};

/// Query for GET /channels/{channel_id}/messages. The server defaults `limit`
/// to 50; values must be in [1, 100]. At most one cursor may be supplied.
/// Omitted fields stay absent from the serialized query string.
struct GetChannelMessagesRequest final {
    std::optional<std::int32_t> limit;
    std::optional<Snowflake> before;
    std::optional<Snowflake> after;
    std::optional<Snowflake> around;
};

} // namespace discordcpp
