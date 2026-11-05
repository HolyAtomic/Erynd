#pragma once

#include <discordcpp/field.hpp>
#include <discordcpp/snowflake.hpp>

#include <cstdint>
#include <string>

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
};

} // namespace discordcpp
