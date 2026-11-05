#include <discordcpp/query_parameters.hpp>
#include <discordcpp/discord_rest_client.hpp>

#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>

namespace {

int checks{};
int failures{};

void check(bool condition, std::string_view message) {
    ++checks;
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

bool invalid_parameter(const discordcpp::Result<void, discordcpp::LocalValidationError>& result) {
    return result.has_error() && result.error().issue == discordcpp::LocalValidationIssue::invalid_query_parameter;
}

void test_basic_typed_values_and_insertion_order() {
    discordcpp::QueryParameters query;
    check(query.empty() && query.size() == 0 && query.serialize().empty(),
        "empty collection serializes to an empty query string");

    check(query.add("search", "two words").has_value(), "string literal is accepted as a string value");
    check(query.add("limit", 25).has_value(), "integer parameter is accepted");
    check(query.add("offset", std::int64_t{ -12 }).has_value(), "signed integer parameter is accepted");
    check(query.add("include_archived", true).has_value(), "boolean parameter is accepted");
    check(query.add("enabled", false).has_value(), "false boolean parameter is accepted");
    check(query.serialize() ==
        "search=two%20words&limit=25&offset=-12&include_archived=true&enabled=false",
        "typed values serialize deterministically in insertion order");
}

void test_optional_repeated_and_empty_values() {
    discordcpp::QueryParameters query;
    check(query.add_optional("cursor", std::optional<std::string_view>{}).has_value(),
        "absent optional string is omitted");
    check(query.add_optional("empty-name", std::optional<std::string>{ std::string{} },
        discordcpp::EmptyQueryValuePolicy::allow).has_value(), "present optional empty string is allowed explicitly");
    check(query.add_optional("count", std::optional<int>{ 0 }).has_value(),
        "present optional integer serializes including zero");
    check(query.add_optional("flag", std::optional<bool>{ false }).has_value(),
        "present optional false boolean is not treated as absent");
    check(query.add("tag", "first").has_value() && query.add("tag", "second").has_value(),
        "repeated parameter names are preserved");
    check(query.serialize() == "empty-name=&count=0&flag=false&tag=first&tag=second",
        "optional absence, explicit empty, false, and repeated values remain distinct");

    discordcpp::QueryParameters default_empty;
    check(invalid_parameter(default_empty.add("empty", "")),
        "empty string value is rejected by default");
    check(default_empty.add("empty", "", discordcpp::EmptyQueryValuePolicy::allow).has_value() &&
        default_empty.serialize() == "empty=", "empty value is serialized only with explicit permission");
    check(invalid_parameter(default_empty.add_optional("", std::optional<std::string_view>{})),
        "an absent optional does not hide an invalid empty name");
}

void test_percent_encoding_and_utf8() {
    discordcpp::QueryParameters query;
    check(query.add("a&b=c?d#e%f", "x&y=z?more#part%" ).has_value(),
        "reserved delimiter characters are accepted as component data");
    check(query.add("q space", "v space").has_value(), "spaces are accepted");
    check(query.add("café", "你好 😀").has_value(), "valid UTF-8 values are accepted");
    check(query.serialize() ==
        "a%26b%3Dc%3Fd%23e%25f=x%26y%3Dz%3Fmore%23part%25&q%20space=v%20space&caf%C3%A9=%E4%BD%A0%E5%A5%BD%20%F0%9F%98%80",
        "reserved bytes and UTF-8 are percent-encoded with uppercase hex and spaces as %20");

    std::string invalid_value{ "bad\xC3(" };
    check(invalid_parameter(query.add("invalid", invalid_value)), "ill-formed UTF-8 value is rejected");
    std::string invalid_name{ "bad\xED\xA0\x80" };
    check(invalid_parameter(query.add(invalid_name, "value")), "UTF-8 encoding of a surrogate is rejected in names");
    std::string truncated{ "\xF0\x9F\x98", 3 };
    check(invalid_parameter(query.add("truncated", truncated)), "truncated UTF-8 sequence is rejected");
}

void test_long_inputs_and_stable_repeated_serialization() {
    const std::string long_name(4096, 'n');
    const std::string long_value(4096, 'v');
    discordcpp::QueryParameters query;
    check(query.add(long_name, long_value).has_value(), "long valid UTF-8 names and values are accepted without arbitrary limits");
    const auto expected = long_name + "=" + long_value;
    const auto serialized = query.serialize();
    check(serialized == expected && serialized.size() == 8193,
        "long unreserved inputs serialize without expansion or truncation");

    discordcpp::QueryParameters ordered;
    (void)ordered.add("b", "2");
    (void)ordered.add("a", "1");
    (void)ordered.add("b", "3");
    check(ordered.serialize() == "b=2&a=1&b=3" && ordered.serialize() == ordered.serialize(),
        "serialization is stable, insertion-ordered, and keeps repeated keys adjacent to their insertion points");
}

void test_path_builder_integration() {
    discordcpp::DiscordApiPathBuilder path{ 10 };
    check(path.append_literal("channels").has_value() &&
        path.append_snowflake(discordcpp::Snowflake{ 123 }).has_value() &&
        path.append_literal("messages").has_value(), "path setup succeeds");

    discordcpp::QueryParameters empty;
    const auto without_query = path.finish();
    const auto empty_query_target = path.finish(empty);
    check(without_query.has_value() && empty_query_target.has_value() &&
        without_query.value() == empty_query_target.value() && empty_query_target.value() ==
            "/api/v10/channels/123/messages",
        "empty query collection leaves the existing path-builder behavior unchanged");

    discordcpp::QueryParameters query;
    (void)query.add("redirect", "https://evil.example/a?x=1&y=2#frag");
    (void)query.add("next", "a/b%2Fc");
    const auto target = path.finish(query);
    check(target.has_value() && target.value() ==
        "/api/v10/channels/123/messages?redirect=https%3A%2F%2Fevil.example%2Fa%3Fx%3D1%26y%3D2%23frag&next=a%2Fb%252Fc",
        "query delimiters and URL-shaped values cannot alter host or path structure");
    check(target.has_value() && target.value().find("evil.example/a") == std::string::npos,
        "raw URL delimiters never escape query component encoding");
}

} // namespace

int main() {
    test_basic_typed_values_and_insertion_order();
    test_optional_repeated_and_empty_values();
    test_percent_encoding_and_utf8();
    test_long_inputs_and_stable_repeated_serialization();
    test_path_builder_integration();
    if (failures == 0) {
        std::cout << "Passed " << checks << " REST query parameter checks.\n";
        return 0;
    }
    std::cerr << failures << " of " << checks << " REST query parameter checks failed.\n";
    return 1;
}
