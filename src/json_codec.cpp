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
        model.id = std::move(id).value();
        model.type = std::move(type).value();
        model.guild_id = std::move(guild_id).value();
        model.name = std::move(name).value();
        model.topic = std::move(topic).value();
        return Result<Channel, DiscordClientError>::success(std::move(model));
    });
}

Json serialize_snowflake(const Snowflake& value) {
    return Json(value.to_string());
}

} // namespace discordcpp::detail
