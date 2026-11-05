#include <discordcpp/discord_models.hpp>

#include "json_codec.hpp"

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>

namespace {

int checks{};
int failures{};

void check(bool condition, std::string_view description) {
    ++checks;
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << description << '\n';
    }
}

template<typename Exception, typename Function>
void check_throws(Function&& function, std::string_view description) {
    bool threw_expected{};
    try {
        function();
    } catch (const Exception&) {
        threw_expected = true;
    } catch (...) {
    }
    check(threw_expected, description);
}

discordcpp::HttpResponse response(std::string body) {
    discordcpp::HttpResponse result{};
    result.status = 200;
    result.headers.add("X-Request-Id", "codec-test");
    result.body = std::move(body);
    return result;
}

template<typename T>
bool is_model_error(const discordcpp::Result<T, discordcpp::DiscordClientError>& result,
    std::string_view path = {}) {
    if (!result.has_error() || !std::holds_alternative<discordcpp::ModelValidationError>(result.error())) {
        return false;
    }
    return path.empty() || std::get<discordcpp::ModelValidationError>(result.error()).path == path;
}

template<typename T>
bool is_json_error(const discordcpp::Result<T, discordcpp::DiscordClientError>& result) {
    return result.has_error() && std::holds_alternative<discordcpp::JsonParseError>(result.error());
}

void test_field_states_and_value_semantics() {
    discordcpp::Field<std::string> absent;
    check(absent.state() == discordcpp::FieldState::absent && absent.is_absent() &&
        !absent.is_null() && !absent.has_value(), "default Field represents absence");

    auto explicit_null = discordcpp::Field<std::string>::null();
    check(explicit_null.state() == discordcpp::FieldState::null && explicit_null.is_null() &&
        !explicit_null.is_absent() && !explicit_null.has_value(), "Field::null represents explicit JSON null");

    auto present = discordcpp::Field<std::string>::with_value("present");
    check(present.state() == discordcpp::FieldState::value && present.has_value() &&
        present.value() == "present", "Field::with_value represents a value");
    present.value() += " value";
    check(present.value() == "present value", "Field offers mutable value access");
    const auto copied = present;
    check(copied == present && copied.value() == "present value", "Field has ordinary copy value semantics");
    auto moved = std::move(present);
    check(moved.has_value() && moved.value() == "present value", "Field has ordinary move value semantics");

    const auto snowflake_field = discordcpp::Field<discordcpp::Snowflake>::with_value(
        discordcpp::Snowflake{ 18446744073709551615ULL });
    check(snowflake_field.has_value() && snowflake_field.value().to_string() == "18446744073709551615",
        "Field supports Snowflake values");

    check_throws<std::bad_optional_access>([&] { (void)absent.value(); },
        "value() on an absent Field throws predictably");
    check_throws<std::bad_optional_access>([&] { (void)explicit_null.value(); },
        "value() on a null Field throws predictably");
}

void test_user_model() {
    const auto minimal = discordcpp::detail::parse_user_response(response(
        R"json({"id":"1","username":"bot","future":{"new":true}})json"));
    check(minimal.has_value() && minimal.value().id.value() == 1 && minimal.value().username == "bot",
        "User parses required ID and username");
    if (minimal.has_value()) {
        check(minimal.value().global_name.is_absent() && minimal.value().avatar.is_absent() &&
            minimal.value().bot.is_absent(), "omitted User fields remain absent");
    }

    const auto all_optional = discordcpp::detail::parse_user_response(response(
        R"json({"id":"42","username":"a \"name\" \\ path\nline","global_name":"display","avatar":"hash","bot":true})json"));
    check(all_optional.has_value() && all_optional.value().username == "a \"name\" \\ path\nline" &&
        all_optional.value().global_name.value() == "display" && all_optional.value().avatar.value() == "hash" &&
        all_optional.value().bot.value(), "User parses optional fields and escaped strings");

    const auto nullable = discordcpp::detail::parse_user_response(response(
        R"json({"id":"43","username":"bot","global_name":null,"avatar":null,"bot":false})json"));
    check(nullable.has_value() && nullable.value().global_name.is_null() && nullable.value().avatar.is_null() &&
        nullable.value().bot.has_value() && !nullable.value().bot.value(),
        "User preserves null nullable fields and false bot value");

    const auto unicode = discordcpp::detail::parse_user_response(response(
        R"json({"id":"44","username":"\u4F60\u597D \uD83D\uDE00"})json"));
    check(unicode.has_value() && unicode.value().username == "你好 \xF0\x9F\x98\x80",
        "User decodes BMP escapes and a valid UTF-16 surrogate pair to UTF-8");

    check(is_model_error(discordcpp::detail::parse_user_response(response(R"({"username":"x"})")), "/id"),
        "missing User ID is a model validation error");
    check(is_model_error(discordcpp::detail::parse_user_response(response(R"({"id":"1"})")), "/username"),
        "missing User username is a model validation error");
    check(is_model_error(discordcpp::detail::parse_user_response(response(
        R"({"id":1,"username":"x"})")), "/id"), "numeric JSON User ID is rejected");
    check(is_model_error(discordcpp::detail::parse_user_response(response(
        R"({"id":"1","username":5})")), "/username"), "wrong User username type is rejected");
    check(is_model_error(discordcpp::detail::parse_user_response(response(
        R"({"id":"1","username":"x","bot":"true"})")), "/bot"),
        "wrong optional User bot type is rejected");
    check(is_model_error(discordcpp::detail::parse_user_response(response(
        R"({"id":"1","username":"x","bot":null})")), "/bot"),
        "null is rejected for non-nullable User bot");
}

void test_guild_model() {
    const auto minimal = discordcpp::detail::parse_guild_response(response(
        R"json({"id":"100","name":"guild","icon":null})json"));
    check(minimal.has_value() && minimal.value().id.value() == 100 && minimal.value().name == "guild" &&
        minimal.value().icon.is_null() && minimal.value().owner_id.is_absent(),
        "Guild parses required fields and preserves absent owner and null icon");

    const auto with_owner = discordcpp::detail::parse_guild_response(response(
        R"json({"id":"101","name":"guild","icon":"icon-hash","owner_id":"18446744073709551615","future":1})json"));
    check(with_owner.has_value() && with_owner.value().icon.value() == "icon-hash" &&
        with_owner.value().owner_id.value().to_string() == "18446744073709551615",
        "Guild parses optional owner ID and ignores unknown fields");

    const auto omitted_icon = discordcpp::detail::parse_guild_response(response(
        R"({"id":"102","name":"guild"})"));
    check(omitted_icon.has_value() && omitted_icon.value().icon.is_absent(),
        "omitted nullable Guild icon remains absent");

    check(is_model_error(discordcpp::detail::parse_guild_response(response(
        R"({"id":"103"})")), "/name"), "missing Guild name is rejected");
    check(is_model_error(discordcpp::detail::parse_guild_response(response(
        R"({"id":"104","name":"guild","owner_id":104})")), "/owner_id"),
        "numeric JSON Guild owner ID is rejected");
    check(is_model_error(discordcpp::detail::parse_guild_response(response(
        R"({"id":"105","name":"guild","owner_id":null})")), "/owner_id"),
        "null Guild owner ID is rejected because Discord defines it as non-nullable");
    check(is_model_error(discordcpp::detail::parse_guild_response(response(
        R"({"id":"18446744073709551616","name":"guild"})")), "/id"),
        "overflowing Guild snowflake is rejected");
    check(is_model_error(discordcpp::detail::parse_guild_response(response(
        R"({"id":"106","name":false})")), "/name"), "wrong Guild name type is rejected");
}

void test_channel_model() {
    const auto guild_channel = discordcpp::detail::parse_channel_response(response(
        R"json({"id":"200","type":0,"guild_id":"100","name":"general","topic":null,"new_field":[]})json"));
    check(guild_channel.has_value() && guild_channel.value().type == 0 &&
        guild_channel.value().guild_id.value().value() == 100 && guild_channel.value().name.value() == "general" &&
        guild_channel.value().topic.is_null(), "Channel parses guild payload and nullable topic");

    const auto dm_channel = discordcpp::detail::parse_channel_response(response(
        R"json({"id":"201","type":1,"name":null})json"));
    check(dm_channel.has_value() && dm_channel.value().guild_id.is_absent() &&
        dm_channel.value().name.is_null() && dm_channel.value().topic.is_absent(),
        "Channel parses DM-like payload and distinguishes absent from null");

    const auto future_type = discordcpp::detail::parse_channel_response(response(
        R"({"id":"202","type":9876})"));
    check(future_type.has_value() && future_type.value().type == 9876,
        "unknown future channel type is preserved numerically");

    check(is_model_error(discordcpp::detail::parse_channel_response(response(
        R"({"type":0})")), "/id"), "missing Channel ID is rejected");
    check(is_model_error(discordcpp::detail::parse_channel_response(response(
        R"({"id":"203"})")), "/type"), "missing Channel type is rejected");
    check(is_model_error(discordcpp::detail::parse_channel_response(response(
        R"({"id":"204","type":"text"})")), "/type"), "string Channel type is rejected");
    check(is_model_error(discordcpp::detail::parse_channel_response(response(
        R"({"id":"205","type":1.5})")), "/type"), "fractional Channel type is rejected");
    check(is_model_error(discordcpp::detail::parse_channel_response(response(
        R"({"id":"206","type":1,"guild_id":null})")), "/guild_id"),
        "null Channel guild ID is rejected because Discord defines it as optional non-nullable");
    check(is_model_error(discordcpp::detail::parse_channel_response(response(
        R"({"id":"207","type":1,"name":false})")), "/name"),
        "wrong nullable Channel name type is rejected");
    check(is_model_error(discordcpp::detail::parse_channel_response(response(
        R"({"id":"208","type":2147483648})")), "/type"),
        "out-of-range Channel type is rejected");
}

void test_guild_collection_models() {
    const auto channels = discordcpp::detail::parse_channel_collection_response(response(
        R"json([{"id":"300","type":0,"position":0,"future":{"x":1}},{"id":"301","type":2}])json"));
    check(channels.has_value() && channels.value().size() == 2 && channels.value()[0].position.value() == 0 &&
        channels.value()[1].position.is_absent(),
        "channel collection preserves optional position, element order, and ignores unknown fields");
    check(is_model_error(discordcpp::detail::parse_channel_collection_response(response(
        R"json([{"id":"300","type":0},{"id":7,"type":2}])json")), "/1/id"),
        "channel collection identifies an invalid later element without returning a partial vector");

    const auto roles = discordcpp::detail::parse_role_collection_response(response(
        R"json([{"id":"400","name":"@everyone","permissions":"0","position":0,"color":0,"hoist":false,"managed":false,"mentionable":false},{"id":"401","name":"admin","permissions":"18446744073709551616","position":1,"color":16777215,"hoist":true,"managed":false,"mentionable":true,"icon":null,"future":true}])json"));
    check(roles.has_value() && roles.value().size() == 2 &&
        roles.value()[1].permissions == "18446744073709551616" && roles.value()[1].icon.is_null(),
        "role collection preserves wide decimal permission strings and nullable fields");
    check(is_model_error(discordcpp::detail::parse_role_collection_response(response(
        R"json([{"id":"400","name":"role","permissions":0,"position":0,"color":0,"hoist":false,"managed":false,"mentionable":false}])json")),
        "/0/permissions"), "role permissions must retain Discord's decimal string representation");

    const auto emojis = discordcpp::detail::parse_emoji_collection_response(response(
        R"json([{"id":"500","name":"wave","roles":[],"require_colons":true,"managed":false,"animated":false,"available":true},{"id":"501","name":"dance","roles":["401"],"animated":true,"future":null}])json"));
    check(emojis.has_value() && emojis.value().size() == 2 && emojis.value()[1].roles.value()[0].value() == 401 &&
        emojis.value()[1].animated.value() && emojis.value()[1].available.is_absent(),
        "emoji collection preserves optional role references and capability flags");
    check(is_model_error(discordcpp::detail::parse_emoji_collection_response(response(
        R"json([{"id":"500","name":"wave","roles":"401"}])json")), "/0/roles"),
        "emoji role references must be an array of decimal Snowflake strings");
    check(is_model_error(discordcpp::detail::parse_emoji_collection_response(response("{}")), ""),
        "guild collection endpoints reject an object root instead of treating it as an empty list");
    check(discordcpp::detail::parse_emoji_collection_response(response("[]")).has_value() &&
        discordcpp::detail::parse_emoji_collection_response(response("[]")).value().empty(),
        "empty Discord collection payloads decode to empty vectors");
}

void test_guild_member_model() {
    const auto decoded = discordcpp::detail::parse_guild_member_response(response(
        R"json({"user":{"id":"600","username":"member"},"nick":null,"avatar":"guild-avatar","roles":["601","602"],"joined_at":"2025-01-02T03:04:05Z","premium_since":null,"deaf":false,"mute":true,"pending":false,"permissions":"18446744073709551616","unknown":true})json"));
    check(decoded.has_value() && decoded.value().user.value().id.value() == 600 &&
        decoded.value().nick.is_null() && decoded.value().roles.size() == 2 &&
        decoded.value().joined_at.value() == "2025-01-02T03:04:05Z" &&
        decoded.value().premium_since.is_null() && decoded.value().mute &&
        decoded.value().permissions.value() == "18446744073709551616",
        "GuildMember decodes user, snowflake roles, timestamps, flags, and string permissions");

    const auto minimal = discordcpp::detail::parse_guild_member_response(response(
        R"json({"roles":[],"joined_at":null,"deaf":false,"mute":false})json"));
    check(minimal.has_value() && minimal.value().user.is_absent() && minimal.value().nick.is_absent() &&
        minimal.value().avatar.is_absent() && minimal.value().joined_at.is_null() &&
        minimal.value().premium_since.is_absent() && minimal.value().pending.is_absent(),
        "GuildMember distinguishes absent optional fields from null values");

    check(is_model_error(discordcpp::detail::parse_guild_member_response(response(
        R"json({"roles":[],"deaf":false,"mute":false})json")), "/joined_at"),
        "GuildMember requires joined_at while allowing explicit null");
    check(is_model_error(discordcpp::detail::parse_guild_member_response(response(
        R"json({"roles":[],"joined_at":null,"deaf":false})json")), "/mute"),
        "GuildMember requires mute");
    check(is_model_error(discordcpp::detail::parse_guild_member_response(response(
        R"json({"roles":[1],"joined_at":null,"deaf":false,"mute":false})json")), "/roles/0"),
        "GuildMember role IDs must be decimal Snowflake strings");
    check(is_model_error(discordcpp::detail::parse_guild_member_response(response(
        R"json({"roles":[],"joined_at":7,"deaf":false,"mute":false})json")), "/joined_at"),
        "GuildMember timestamp must be a string or null");
    check(is_model_error(discordcpp::detail::parse_guild_member_response(response(
        R"json({"roles":[],"joined_at":null,"deaf":false,"mute":false,"user":{"id":"1","username":false}})json")),
        "/user/username"), "GuildMember validates its nested partial User object");
    check(is_model_error(discordcpp::detail::parse_guild_member_response(response(
        R"json({"roles":[],"joined_at":null,"deaf":false,"mute":false,"permissions":"1x"})json")),
        "/permissions"), "GuildMember permissions must be a decimal string when present");
}

void test_json_errors_and_snowflake_json_representation() {
    auto malformed_response = response(R"({"id":"1","username":)");
    malformed_response.status = 200;
    const auto malformed = discordcpp::detail::parse_user_response(malformed_response);
    check(is_json_error(malformed), "malformed JSON becomes JsonParseError");
    if (is_json_error(malformed)) {
        const auto& error = std::get<discordcpp::JsonParseError>(malformed.error());
        check(error.offset.has_value() && error.http_status == 200 && error.response_headers.contains("x-request-id") &&
            error.message == "malformed JSON", "JSON parse error retains offset, status and response headers safely");
    }

    check(is_json_error(discordcpp::detail::parse_user_response(response(""))),
        "empty JSON response is rejected");
    check(is_json_error(discordcpp::detail::parse_user_response(response(
        R"({"id":"1","username":"\uD800"})"))), "unpaired high surrogate is rejected");
    check(is_json_error(discordcpp::detail::parse_user_response(response(
        R"({"id":"1","username":"\uDC00"})"))), "unpaired low surrogate is rejected");

    std::string invalid_utf8 = R"({"id":"1","username":")";
    invalid_utf8 += "\xC3(";
    invalid_utf8 += R"("})";
    const auto invalid_encoding = discordcpp::detail::parse_user_response(response(std::move(invalid_utf8)));
    check(is_json_error(invalid_encoding), "ill-formed raw UTF-8 is rejected");
    if (is_json_error(invalid_encoding)) {
        check(std::get<discordcpp::JsonParseError>(invalid_encoding.error()).category ==
            discordcpp::JsonDiagnosticCategory::invalid_encoding,
            "invalid UTF-8 is classified as an encoding diagnostic");
    }

    const auto numeric_id = discordcpp::detail::parse_user_response(response(
        R"({"id":18446744073709551615,"username":"max"})"));
    check(is_model_error(numeric_id, "/id"), "numeric JSON snowflake is rejected even at uint64 maximum");

    const auto maximum_id = discordcpp::detail::parse_user_response(response(
        R"({"id":"18446744073709551615","username":"max"})"));
    check(maximum_id.has_value() && maximum_id.value().id.value() == UINT64_MAX,
        "maximum uint64 snowflake parses when represented as a JSON string");

    const auto encoded = discordcpp::detail::serialize_snowflake(discordcpp::Snowflake{ UINT64_MAX });
    check(encoded.is_string() && encoded.get<std::string>() == "18446744073709551615",
        "Snowflake JSON serializer emits a decimal string, never a JSON number");

    auto wrong_root = response("[]");
    const auto non_object = discordcpp::detail::parse_guild_response(wrong_root);
    check(is_model_error(non_object, ""), "non-object resource JSON is a model validation error");
}

} // namespace

int main() {
    test_field_states_and_value_semantics();
    test_user_model();
    test_guild_model();
    test_channel_model();
    test_guild_collection_models();
    test_guild_member_model();
    test_json_errors_and_snowflake_json_representation();

    if (failures == 0) {
        std::cout << "Passed " << checks << " model and JSON codec checks.\n";
        return 0;
    }
    std::cerr << failures << " of " << checks << " model and JSON codec checks failed.\n";
    return 1;
}
