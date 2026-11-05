#include "json_codec.hpp"

#include <discordcpp/discord_client_error.hpp>

#include <cstdint>
#include <limits>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace discordcpp::detail {
namespace {

ModelValidationError model_error(const HttpResponse& response, std::string path, std::string message) {
    return { std::move(path), std::move(message), response.status, response.headers };
}

DiscordClientError as_client_error(ModelValidationError error) {
    return DiscordClientError{ std::in_place_type<ModelValidationError>, std::move(error) };
}

JsonParseError parse_error(const HttpResponse& response, JsonDiagnosticCategory category,
    std::optional<std::size_t> offset, std::string message) {
    return { category, offset, {}, response.status, response.headers, std::move(message) };
}

Result<Json, DiscordClientError> parse_document(const HttpResponse& response) {
    try {
        return Result<Json, DiscordClientError>::success(Json::parse(response.body));
    } catch (const Json::parse_error& error) {
        const std::string_view diagnostic{ error.what() };
        const bool invalid_encoding = diagnostic.find("invalid string") != std::string_view::npos ||
            diagnostic.find("surrogate") != std::string_view::npos;
        const auto offset = error.byte == 0 ? std::size_t{ 0 } : error.byte - 1;
        return Result<Json, DiscordClientError>::failure(DiscordClientError{
            std::in_place_type<JsonParseError>,
            parse_error(response,
                invalid_encoding ? JsonDiagnosticCategory::invalid_encoding : JsonDiagnosticCategory::syntax,
                offset,
                invalid_encoding ? "JSON contains invalid UTF-8 or a Unicode surrogate sequence" : "malformed JSON")
        });
    } catch (const std::bad_alloc&) {
        return Result<Json, DiscordClientError>::failure(DiscordClientError{
            std::in_place_type<JsonParseError>,
            parse_error(response, JsonDiagnosticCategory::resource_limit, std::nullopt,
                "JSON processing exceeded available memory")
        });
    } catch (const std::exception&) {
        return Result<Json, DiscordClientError>::failure(DiscordClientError{
            std::in_place_type<JsonParseError>,
            parse_error(response, JsonDiagnosticCategory::processing_failure, std::nullopt,
                "JSON processing failed")
        });
    } catch (...) {
        return Result<Json, DiscordClientError>::failure(DiscordClientError{
            std::in_place_type<JsonParseError>,
            parse_error(response, JsonDiagnosticCategory::processing_failure, std::nullopt,
                "JSON processing failed")
        });
    }
}

template<typename T, typename Convert>
Result<Field<T>, ModelValidationError> read_field(const Json& object, const HttpResponse& response,
    std::string_view key, std::string path, bool nullable, Convert&& convert) {
    const auto found = object.find(std::string{ key });
    if (found == object.end()) {
        return Result<Field<T>, ModelValidationError>::success(Field<T>::absent());
    }
    if (found->is_null()) {
        if (nullable) {
            return Result<Field<T>, ModelValidationError>::success(Field<T>::null());
        }
        return Result<Field<T>, ModelValidationError>::failure(
            model_error(response, std::move(path), "field must not be null"));
    }
    auto converted = convert(*found, response, path);
    if (!converted.has_value()) {
        return Result<Field<T>, ModelValidationError>::failure(std::move(converted).error());
    }
    return Result<Field<T>, ModelValidationError>::success(
        Field<T>::with_value(std::move(converted).value()));
}

template<typename T, typename Convert>
Result<T, ModelValidationError> read_required(const Json& object, const HttpResponse& response,
    std::string_view key, std::string path, Convert&& convert) {
    const auto found = object.find(std::string{ key });
    if (found == object.end()) {
        return Result<T, ModelValidationError>::failure(
            model_error(response, std::move(path), "required field is missing"));
    }
    return convert(*found, response, path);
}

Result<std::string, ModelValidationError> as_string(const Json& value, const HttpResponse& response,
    const std::string& path) {
    if (!value.is_string()) {
        return Result<std::string, ModelValidationError>::failure(
            model_error(response, path, "field must be a string"));
    }
    return Result<std::string, ModelValidationError>::success(value.get<std::string>());
}

Result<Snowflake, ModelValidationError> as_snowflake(const Json& value, const HttpResponse& response,
    const std::string& path) {
    if (!value.is_string()) {
        return Result<Snowflake, ModelValidationError>::failure(
            model_error(response, path, "snowflake must be a decimal JSON string"));
    }
    const auto parsed = Snowflake::parse(value.get_ref<const std::string&>());
    if (!parsed.has_value()) {
        std::string message = "snowflake string is invalid";
        if (parsed.error().issue == SnowflakeParseIssue::overflow) {
            message = "snowflake string exceeds uint64 range";
        }
        return Result<Snowflake, ModelValidationError>::failure(
            model_error(response, path, std::move(message)));
    }
    return Result<Snowflake, ModelValidationError>::success(parsed.value());
}

Result<bool, ModelValidationError> as_bool(const Json& value, const HttpResponse& response,
    const std::string& path) {
    if (!value.is_boolean()) {
        return Result<bool, ModelValidationError>::failure(
            model_error(response, path, "field must be a boolean"));
    }
    return Result<bool, ModelValidationError>::success(value.get<bool>());
}

Result<std::int32_t, ModelValidationError> as_channel_type(const Json& value, const HttpResponse& response,
    const std::string& path) {
    if (!value.is_number_integer()) {
        return Result<std::int32_t, ModelValidationError>::failure(
            model_error(response, path, "channel type must be an integer"));
    }
    if (value.is_number_unsigned()) {
        const auto number = value.get<std::uint64_t>();
        if (number > static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max())) {
            return Result<std::int32_t, ModelValidationError>::failure(
                model_error(response, path, "channel type is outside int32 range"));
        }
        return Result<std::int32_t, ModelValidationError>::success(static_cast<std::int32_t>(number));
    }
    const auto number = value.get<std::int64_t>();
    if (number < std::numeric_limits<std::int32_t>::min() ||
        number > std::numeric_limits<std::int32_t>::max()) {
        return Result<std::int32_t, ModelValidationError>::failure(
            model_error(response, path, "channel type is outside int32 range"));
    }
    return Result<std::int32_t, ModelValidationError>::success(static_cast<std::int32_t>(number));
}

Result<std::int32_t, ModelValidationError> as_int32(const Json& value, const HttpResponse& response,
    const std::string& path, std::string_view field_name) {
    if (!value.is_number_integer()) {
        return Result<std::int32_t, ModelValidationError>::failure(
            model_error(response, path, std::string{ field_name } + " must be an integer"));
    }
    if (value.is_number_unsigned()) {
        const auto number = value.get<std::uint64_t>();
        if (number > static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max())) {
            return Result<std::int32_t, ModelValidationError>::failure(
                model_error(response, path, std::string{ field_name } + " is outside int32 range"));
        }
        return Result<std::int32_t, ModelValidationError>::success(static_cast<std::int32_t>(number));
    }
    const auto number = value.get<std::int64_t>();
    if (number < std::numeric_limits<std::int32_t>::min() ||
        number > std::numeric_limits<std::int32_t>::max()) {
        return Result<std::int32_t, ModelValidationError>::failure(
            model_error(response, path, std::string{ field_name } + " is outside int32 range"));
    }
    return Result<std::int32_t, ModelValidationError>::success(static_cast<std::int32_t>(number));
}

Result<std::string, ModelValidationError> as_permission_bits(const Json& value, const HttpResponse& response,
    const std::string& path) {
    if (!value.is_string()) {
        return Result<std::string, ModelValidationError>::failure(
            model_error(response, path, "permissions must be a decimal JSON string"));
    }
    const auto& text = value.get_ref<const std::string&>();
    if (text.empty() || text.find_first_not_of("0123456789") != std::string::npos) {
        return Result<std::string, ModelValidationError>::failure(
            model_error(response, path, "permissions must contain decimal digits"));
    }
    return Result<std::string, ModelValidationError>::success(text);
}

Result<std::vector<Snowflake>, ModelValidationError> as_snowflake_array(const Json& value,
    const HttpResponse& response, const std::string& path) {
    if (!value.is_array()) {
        return Result<std::vector<Snowflake>, ModelValidationError>::failure(
            model_error(response, path, "field must be an array of snowflake strings"));
    }
    std::vector<Snowflake> result;
    result.reserve(value.size());
    for (std::size_t index = 0; index < value.size(); ++index) {
        auto parsed = as_snowflake(value[index], response, path + "/" + std::to_string(index));
        if (!parsed.has_value()) {
            return Result<std::vector<Snowflake>, ModelValidationError>::failure(std::move(parsed).error());
        }
        result.push_back(std::move(parsed).value());
    }
    return Result<std::vector<Snowflake>, ModelValidationError>::success(std::move(result));
}

template<typename T, typename Convert>
Result<Field<T>, ModelValidationError> read_required_field(const Json& object, const HttpResponse& response,
    std::string_view key, std::string path, bool nullable, Convert&& convert) {
    auto field = read_field<T>(object, response, key, path, nullable, std::forward<Convert>(convert));
    if (!field.has_value()) return field;
    if (field.value().is_absent()) {
        return Result<Field<T>, ModelValidationError>::failure(
            model_error(response, std::move(path), "required field is missing"));
    }
    return field;
}

Result<User, ModelValidationError> as_user(const Json& value, const HttpResponse& response,
    const std::string& path) {
    if (!value.is_object()) {
        return Result<User, ModelValidationError>::failure(model_error(response, path, "user must be an object"));
    }
    User user{};
    auto id = read_required<Snowflake>(value, response, "id", path + "/id", as_snowflake);
    if (!id.has_value()) return Result<User, ModelValidationError>::failure(std::move(id).error());
    auto username = read_required<std::string>(value, response, "username", path + "/username", as_string);
    if (!username.has_value()) return Result<User, ModelValidationError>::failure(std::move(username).error());
    auto global_name = read_field<std::string>(value, response, "global_name", path + "/global_name", true, as_string);
    if (!global_name.has_value()) return Result<User, ModelValidationError>::failure(std::move(global_name).error());
    auto avatar = read_field<std::string>(value, response, "avatar", path + "/avatar", true, as_string);
    if (!avatar.has_value()) return Result<User, ModelValidationError>::failure(std::move(avatar).error());
    auto bot = read_field<bool>(value, response, "bot", path + "/bot", false, as_bool);
    if (!bot.has_value()) return Result<User, ModelValidationError>::failure(std::move(bot).error());
    user.id = std::move(id).value();
    user.username = std::move(username).value();
    user.global_name = std::move(global_name).value();
    user.avatar = std::move(avatar).value();
    user.bot = std::move(bot).value();
    return Result<User, ModelValidationError>::success(std::move(user));
}

template<typename Model, typename Populate>
Result<Model, DiscordClientError> parse_model(const HttpResponse& response, Populate&& populate) {
    auto parsed = parse_document(response);
    if (!parsed.has_value()) {
        return Result<Model, DiscordClientError>::failure(std::move(parsed).error());
    }
    try {
        const auto& object = parsed.value();
        if (!object.is_object()) {
            return Result<Model, DiscordClientError>::failure(as_client_error(
                model_error(response, "", "JSON response must be an object")));
        }
        return populate(object, response);
    } catch (const std::bad_alloc&) {
        return Result<Model, DiscordClientError>::failure(DiscordClientError{
            std::in_place_type<JsonParseError>,
            parse_error(response, JsonDiagnosticCategory::resource_limit, std::nullopt,
                "JSON model decoding exceeded available memory")
        });
    } catch (const std::exception&) {
        return Result<Model, DiscordClientError>::failure(DiscordClientError{
            std::in_place_type<JsonParseError>,
            parse_error(response, JsonDiagnosticCategory::processing_failure, std::nullopt,
                "JSON model decoding failed")
        });
    } catch (...) {
        return Result<Model, DiscordClientError>::failure(DiscordClientError{
            std::in_place_type<JsonParseError>,
            parse_error(response, JsonDiagnosticCategory::processing_failure, std::nullopt,
                "JSON model decoding failed")
        });
    }
}

template<typename Model, typename ParseItem>
Result<std::vector<Model>, DiscordClientError> parse_collection_response(
    const HttpResponse& response, ParseItem&& parse_item) {
    auto parsed = parse_document(response);
    if (!parsed.has_value()) {
        return Result<std::vector<Model>, DiscordClientError>::failure(std::move(parsed).error());
    }
    if (!parsed.value().is_array()) {
        return Result<std::vector<Model>, DiscordClientError>::failure(as_client_error(
            model_error(response, "", "JSON response must be an array")));
    }

    try {
        std::vector<Model> models;
        models.reserve(parsed.value().size());
        for (std::size_t index = 0; index < parsed.value().size(); ++index) {
            if (!parsed.value()[index].is_object()) {
                return Result<std::vector<Model>, DiscordClientError>::failure(as_client_error(
                    model_error(response, "/" + std::to_string(index), "collection element must be an object")));
            }
            HttpResponse element_response = response;
            element_response.body = parsed.value()[index].dump();
            auto element = parse_item(element_response);
            if (!element.has_value()) {
                auto error = std::move(element).error();
                if (auto* validation = std::get_if<ModelValidationError>(&error)) {
                    const auto suffix = validation->path;
                    validation->path = "/" + std::to_string(index) + suffix;
                }
                return Result<std::vector<Model>, DiscordClientError>::failure(std::move(error));
            }
            models.push_back(std::move(element).value());
        }
        return Result<std::vector<Model>, DiscordClientError>::success(std::move(models));
    } catch (const std::bad_alloc&) {
        return Result<std::vector<Model>, DiscordClientError>::failure(DiscordClientError{
            std::in_place_type<JsonParseError>,
            parse_error(response, JsonDiagnosticCategory::resource_limit, std::nullopt,
                "JSON collection decoding exceeded available memory")
        });
    } catch (const std::exception&) {
        return Result<std::vector<Model>, DiscordClientError>::failure(DiscordClientError{
            std::in_place_type<JsonParseError>,
            parse_error(response, JsonDiagnosticCategory::processing_failure, std::nullopt,
                "JSON collection decoding failed")
        });
    } catch (...) {
        return Result<std::vector<Model>, DiscordClientError>::failure(DiscordClientError{
            std::in_place_type<JsonParseError>,
            parse_error(response, JsonDiagnosticCategory::processing_failure, std::nullopt,
                "JSON collection decoding failed")
        });
    }
}

template<typename Model, typename T>
bool failed(const Result<T, ModelValidationError>& result) {
    return !result.has_value();
}

} // namespace

Result<User, DiscordClientError> parse_user_response(const HttpResponse& response) {
    return parse_model<User>(response, [](const Json& object, const HttpResponse& responseNew) {
        User model{};
        auto id = read_required<Snowflake>(object, responseNew, "id", "/id", as_snowflake);
        if (failed<User>(id)) return Result<User, DiscordClientError>::failure(as_client_error(std::move(id).error()));
        auto username = read_required<std::string>(object, responseNew, "username", "/username", as_string);
        if (failed<User>(username)) return Result<User, DiscordClientError>::failure(as_client_error(std::move(username).error()));
        auto global_name = read_field<std::string>(object, responseNew, "global_name", "/global_name", true, as_string);
        if (failed<User>(global_name)) return Result<User, DiscordClientError>::failure(as_client_error(std::move(global_name).error()));
        auto avatar = read_field<std::string>(object, responseNew, "avatar", "/avatar", true, as_string);
        if (failed<User>(avatar)) return Result<User, DiscordClientError>::failure(as_client_error(std::move(avatar).error()));
        auto bot = read_field<bool>(object, responseNew, "bot", "/bot", false, as_bool);
        if (failed<User>(bot)) return Result<User, DiscordClientError>::failure(as_client_error(std::move(bot).error()));
        model.id = std::move(id).value();
        model.username = std::move(username).value();
        model.global_name = std::move(global_name).value();
        model.avatar = std::move(avatar).value();
        model.bot = std::move(bot).value();
        return Result<User, DiscordClientError>::success(std::move(model));
    });
}

Result<Guild, DiscordClientError> parse_guild_response(const HttpResponse& response) {
    return parse_model<Guild>(response, [](const Json& object, const HttpResponse& responseNew) {
        Guild model{};
        auto id = read_required<Snowflake>(object, responseNew, "id", "/id", as_snowflake);
        if (failed<Guild>(id)) return Result<Guild, DiscordClientError>::failure(as_client_error(std::move(id).error()));
        auto name = read_required<std::string>(object, responseNew, "name", "/name", as_string);
        if (failed<Guild>(name)) return Result<Guild, DiscordClientError>::failure(as_client_error(std::move(name).error()));
        auto icon = read_field<std::string>(object, responseNew, "icon", "/icon", true, as_string);
        if (failed<Guild>(icon)) return Result<Guild, DiscordClientError>::failure(as_client_error(std::move(icon).error()));
        auto owner_id = read_field<Snowflake>(object, responseNew, "owner_id", "/owner_id", false, as_snowflake);
        if (failed<Guild>(owner_id)) return Result<Guild, DiscordClientError>::failure(as_client_error(std::move(owner_id).error()));
        model.id = std::move(id).value();
        model.name = std::move(name).value();
        model.icon = std::move(icon).value();
        model.owner_id = std::move(owner_id).value();
        return Result<Guild, DiscordClientError>::success(std::move(model));
    });
}

Result<Channel, DiscordClientError> parse_channel_response(const HttpResponse& response) {
    return parse_model<Channel>(response, [](const Json& object, const HttpResponse& responseNew) {
        Channel model{};
        auto id = read_required<Snowflake>(object, responseNew, "id", "/id", as_snowflake);
        if (failed<Channel>(id)) return Result<Channel, DiscordClientError>::failure(as_client_error(std::move(id).error()));
        auto type = read_required<std::int32_t>(object, responseNew, "type", "/type", as_channel_type);
        if (failed<Channel>(type)) return Result<Channel, DiscordClientError>::failure(as_client_error(std::move(type).error()));
        auto guild_id = read_field<Snowflake>(object, responseNew, "guild_id", "/guild_id", false, as_snowflake);
        if (failed<Channel>(guild_id)) return Result<Channel, DiscordClientError>::failure(as_client_error(std::move(guild_id).error()));
        auto name = read_field<std::string>(object, responseNew, "name", "/name", true, as_string);
        if (failed<Channel>(name)) return Result<Channel, DiscordClientError>::failure(as_client_error(std::move(name).error()));
        auto topic = read_field<std::string>(object, responseNew, "topic", "/topic", true, as_string);
        if (failed<Channel>(topic)) return Result<Channel, DiscordClientError>::failure(as_client_error(std::move(topic).error()));
        auto position = read_field<std::int32_t>(object, responseNew, "position", "/position", false,
            [](const Json& value, const HttpResponse& responseValue, const std::string& path) {
                return as_int32(value, responseValue, path, "channel position");
            });
        if (failed<Channel>(position)) return Result<Channel, DiscordClientError>::failure(as_client_error(std::move(position).error()));
        model.id = std::move(id).value();
        model.type = std::move(type).value();
        model.guild_id = std::move(guild_id).value();
        model.name = std::move(name).value();
        model.topic = std::move(topic).value();
        model.position = std::move(position).value();
        return Result<Channel, DiscordClientError>::success(std::move(model));
    });
}

Result<Role, DiscordClientError> parse_role_response(const HttpResponse& response) {
    return parse_model<Role>(response, [](const Json& object, const HttpResponse& responseNew) {
        Role model{};
        auto id = read_required<Snowflake>(object, responseNew, "id", "/id", as_snowflake);
        if (failed<Role>(id)) return Result<Role, DiscordClientError>::failure(as_client_error(std::move(id).error()));
        auto name = read_required<std::string>(object, responseNew, "name", "/name", as_string);
        if (failed<Role>(name)) return Result<Role, DiscordClientError>::failure(as_client_error(std::move(name).error()));
        auto permissions = read_required<std::string>(object, responseNew, "permissions", "/permissions", as_permission_bits);
        if (failed<Role>(permissions)) return Result<Role, DiscordClientError>::failure(as_client_error(std::move(permissions).error()));
        auto position = read_required<std::int32_t>(object, responseNew, "position", "/position",
            [](const Json& value, const HttpResponse& responseValue, const std::string& path) {
                return as_int32(value, responseValue, path, "role position");
            });
        if (failed<Role>(position)) return Result<Role, DiscordClientError>::failure(as_client_error(std::move(position).error()));
        auto color = read_required<std::int32_t>(object, responseNew, "color", "/color",
            [](const Json& value, const HttpResponse& responseValue, const std::string& path) {
                return as_int32(value, responseValue, path, "role color");
            });
        if (failed<Role>(color)) return Result<Role, DiscordClientError>::failure(as_client_error(std::move(color).error()));
        auto hoist = read_required<bool>(object, responseNew, "hoist", "/hoist", as_bool);
        if (failed<Role>(hoist)) return Result<Role, DiscordClientError>::failure(as_client_error(std::move(hoist).error()));
        auto managed = read_required<bool>(object, responseNew, "managed", "/managed", as_bool);
        if (failed<Role>(managed)) return Result<Role, DiscordClientError>::failure(as_client_error(std::move(managed).error()));
        auto mentionable = read_required<bool>(object, responseNew, "mentionable", "/mentionable", as_bool);
        if (failed<Role>(mentionable)) return Result<Role, DiscordClientError>::failure(as_client_error(std::move(mentionable).error()));
        auto icon = read_field<std::string>(object, responseNew, "icon", "/icon", true, as_string);
        if (failed<Role>(icon)) return Result<Role, DiscordClientError>::failure(as_client_error(std::move(icon).error()));
        auto unicode_emoji = read_field<std::string>(object, responseNew, "unicode_emoji", "/unicode_emoji", true, as_string);
        if (failed<Role>(unicode_emoji)) return Result<Role, DiscordClientError>::failure(as_client_error(std::move(unicode_emoji).error()));
        model.id = std::move(id).value();
        model.name = std::move(name).value();
        model.permissions = std::move(permissions).value();
        model.position = std::move(position).value();
        model.color = std::move(color).value();
        model.hoist = std::move(hoist).value();
        model.managed = std::move(managed).value();
        model.mentionable = std::move(mentionable).value();
        model.icon = std::move(icon).value();
        model.unicode_emoji = std::move(unicode_emoji).value();
        return Result<Role, DiscordClientError>::success(std::move(model));
    });
}

Result<GuildMember, DiscordClientError> parse_guild_member_response(const HttpResponse& response) {
    return parse_model<GuildMember>(response, [](const Json& object, const HttpResponse& responseNew) {
        GuildMember model{};
        auto user = read_field<User>(object, responseNew, "user", "/user", false, as_user);
        if (failed<GuildMember>(user)) return Result<GuildMember, DiscordClientError>::failure(as_client_error(std::move(user).error()));
        auto nick = read_field<std::string>(object, responseNew, "nick", "/nick", true, as_string);
        if (failed<GuildMember>(nick)) return Result<GuildMember, DiscordClientError>::failure(as_client_error(std::move(nick).error()));
        auto avatar = read_field<std::string>(object, responseNew, "avatar", "/avatar", true, as_string);
        if (failed<GuildMember>(avatar)) return Result<GuildMember, DiscordClientError>::failure(as_client_error(std::move(avatar).error()));
        auto roles = read_required<std::vector<Snowflake>>(object, responseNew, "roles", "/roles", as_snowflake_array);
        if (failed<GuildMember>(roles)) return Result<GuildMember, DiscordClientError>::failure(as_client_error(std::move(roles).error()));
        auto joined_at = read_required_field<std::string>(object, responseNew, "joined_at", "/joined_at", true, as_string);
        if (failed<GuildMember>(joined_at)) return Result<GuildMember, DiscordClientError>::failure(as_client_error(std::move(joined_at).error()));
        auto premium_since = read_field<std::string>(object, responseNew, "premium_since", "/premium_since", true, as_string);
        if (failed<GuildMember>(premium_since)) return Result<GuildMember, DiscordClientError>::failure(as_client_error(std::move(premium_since).error()));
        auto deaf = read_required<bool>(object, responseNew, "deaf", "/deaf", as_bool);
        if (failed<GuildMember>(deaf)) return Result<GuildMember, DiscordClientError>::failure(as_client_error(std::move(deaf).error()));
        auto mute = read_required<bool>(object, responseNew, "mute", "/mute", as_bool);
        if (failed<GuildMember>(mute)) return Result<GuildMember, DiscordClientError>::failure(as_client_error(std::move(mute).error()));
        auto pending = read_field<bool>(object, responseNew, "pending", "/pending", false, as_bool);
        if (failed<GuildMember>(pending)) return Result<GuildMember, DiscordClientError>::failure(as_client_error(std::move(pending).error()));
        auto permissions = read_field<std::string>(object, responseNew, "permissions", "/permissions", false,
            as_permission_bits);
        if (failed<GuildMember>(permissions)) return Result<GuildMember, DiscordClientError>::failure(as_client_error(std::move(permissions).error()));
        model.user = std::move(user).value();
        model.nick = std::move(nick).value();
        model.avatar = std::move(avatar).value();
        model.roles = std::move(roles).value();
        model.joined_at = std::move(joined_at).value();
        model.premium_since = std::move(premium_since).value();
        model.deaf = std::move(deaf).value();
        model.mute = std::move(mute).value();
        model.pending = std::move(pending).value();
        model.permissions = std::move(permissions).value();
        return Result<GuildMember, DiscordClientError>::success(std::move(model));
    });
}

std::string encode_guild_member_modify_request(const GuildMemberModifyRequest& request) {
    Json object = Json::object();
    const auto encode_string_field = [&object](std::string_view key, const Field<std::string>& field) {
        switch (field.state()) {
            case FieldState::absent: return;
            case FieldState::null: object[std::string{ key }] = nullptr; return;
            case FieldState::value: object[std::string{ key }] = field.value(); return;
        }
    };
    encode_string_field("nick", request.nick);
    switch (request.roles.state()) {
        case FieldState::absent:
            break;
        case FieldState::null:
            object["roles"] = nullptr;
            break;
        case FieldState::value: {
            Json roles = Json::array();
            for (const auto role_id : request.roles.value()) roles.push_back(role_id.to_string());
            object["roles"] = std::move(roles);
            break;
        }
    }
    return object.dump();
}

namespace {

Result<Emoji, DiscordClientError> parse_emoji_response(const HttpResponse& response) {
    return parse_model<Emoji>(response, [](const Json& object, const HttpResponse& responseNew) {
        Emoji model{};
        auto id = read_required<Snowflake>(object, responseNew, "id", "/id", as_snowflake);
        if (failed<Emoji>(id)) return Result<Emoji, DiscordClientError>::failure(as_client_error(std::move(id).error()));
        auto name = read_required<std::string>(object, responseNew, "name", "/name", as_string);
        if (failed<Emoji>(name)) return Result<Emoji, DiscordClientError>::failure(as_client_error(std::move(name).error()));
        auto roles = read_field<std::vector<Snowflake>>(object, responseNew, "roles", "/roles", false, as_snowflake_array);
        if (failed<Emoji>(roles)) return Result<Emoji, DiscordClientError>::failure(as_client_error(std::move(roles).error()));
        auto require_colons = read_field<bool>(object, responseNew, "require_colons", "/require_colons", false, as_bool);
        if (failed<Emoji>(require_colons)) return Result<Emoji, DiscordClientError>::failure(as_client_error(std::move(require_colons).error()));
        auto managed = read_field<bool>(object, responseNew, "managed", "/managed", false, as_bool);
        if (failed<Emoji>(managed)) return Result<Emoji, DiscordClientError>::failure(as_client_error(std::move(managed).error()));
        auto animated = read_field<bool>(object, responseNew, "animated", "/animated", false, as_bool);
        if (failed<Emoji>(animated)) return Result<Emoji, DiscordClientError>::failure(as_client_error(std::move(animated).error()));
        auto available = read_field<bool>(object, responseNew, "available", "/available", false, as_bool);
        if (failed<Emoji>(available)) return Result<Emoji, DiscordClientError>::failure(as_client_error(std::move(available).error()));
        model.id = std::move(id).value();
        model.name = std::move(name).value();
        model.roles = std::move(roles).value();
        model.require_colons = std::move(require_colons).value();
        model.managed = std::move(managed).value();
        model.animated = std::move(animated).value();
        model.available = std::move(available).value();
        return Result<Emoji, DiscordClientError>::success(std::move(model));
    });
}

} // namespace

Result<std::vector<Channel>, DiscordClientError> parse_channel_collection_response(const HttpResponse& response) {
    return parse_collection_response<Channel>(response, parse_channel_response);
}

Result<std::vector<Role>, DiscordClientError> parse_role_collection_response(const HttpResponse& response) {
    return parse_collection_response<Role>(response, parse_role_response);
}

Result<std::vector<Emoji>, DiscordClientError> parse_emoji_collection_response(const HttpResponse& response) {
    return parse_collection_response<Emoji>(response, parse_emoji_response);
}

Json serialize_snowflake(const Snowflake& value) {
    return Json(value.to_string());
}

} // namespace discordcpp::detail
