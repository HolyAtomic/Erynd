#include <discordcpp/discord_rest_client.hpp>
#include <discordcpp/snowflake.hpp>

#include <chrono>
#include <cstdint>
#include <iostream>
#include <optional>
#include <stop_token>
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

class NullTransport final : public discordcpp::IHttpTransport {
public:
    discordcpp::TransportResult send(const discordcpp::HttpRequest&, std::stop_token) override {
        return discordcpp::TransportResult::failure({});
    }
};

class TestScheduler final : public discordcpp::IRateLimitScheduler {
public:
    [[nodiscard]] discordcpp::SteadyClock::time_point now() const noexcept override { return {}; }
    [[nodiscard]] std::uint64_t generation() const noexcept override { return 0; }
    void wait_for_change(std::uint64_t, std::optional<discordcpp::SteadyClock::time_point>,
        std::stop_token) override {}
    void notify_all() noexcept override {}
};

void test_versioned_origin_form_targets() {
    const discordcpp::DiscordApiConfig config{};
    const auto target = config.make_target("/guilds/123");
    check(target.has_value() && target.value() == "/api/v10/guilds/123",
        "default configuration adds exactly one API version prefix");

    const std::string origin = "https://discord.com";
    check(origin + target.value() == "https://discord.com/api/v10/guilds/123",
        "origin plus request target composes the Discord URL without duplicate prefix");

    const auto v11 = discordcpp::DiscordApiConfig{ 11 }.make_target("/users/@me");
    check(v11.has_value() && v11.value() == "/api/v11/users/@me", "configured API version appears in target path");

    for (const std::string_view invalid : { "", "guilds/123", "//evil.example/path",
             "https://discord.com/api/v10/users/@me", "/api/v10/users/@me",
             "/channels/1#fragment", "/channels/1\r\nX-Test: yes" }) {
        const auto rejected = config.make_target(invalid);
        check(rejected.has_error() && rejected.error().issue == discordcpp::LocalValidationIssue::invalid_endpoint_path,
            "invalid endpoint suffix is rejected locally");
    }

    const auto invalid_version = discordcpp::DiscordApiConfig{ 0 }.make_target("/users/@me");
    check(invalid_version.has_error() &&
        invalid_version.error().issue == discordcpp::LocalValidationIssue::invalid_api_version,
        "zero API version is rejected locally");
}

void test_public_error_model_preserves_existing_errors() {
    discordcpp::DiscordApiError api_error{};
    api_error.http_status = 403;
    api_error.discord_code = 50001;
    api_error.message = "Missing Access";
    api_error.raw_body = R"({"code":50001,"message":"Missing Access"})";
    api_error.response_headers.add("X-Trace", "trace-value");
    discordcpp::DiscordClientError api_variant{ std::in_place_type<discordcpp::DiscordApiError>, api_error };
    check(std::holds_alternative<discordcpp::DiscordApiError>(api_variant) &&
        std::get<discordcpp::DiscordApiError>(api_variant).raw_body == api_error.raw_body &&
        std::get<discordcpp::DiscordApiError>(api_variant).response_headers.find("x-trace").has_value(),
        "API errors remain intact alternatives including raw response diagnostics");

    discordcpp::TransportError transport_error{};
    transport_error.category = discordcpp::TransportFailureCategory::timeout;
    transport_error.request_may_have_reached_server = true;
    discordcpp::DiscordClientError transport_variant{ std::in_place_type<discordcpp::TransportError>, transport_error };
    check(std::holds_alternative<discordcpp::TransportError>(transport_variant) &&
        std::get<discordcpp::TransportError>(transport_variant).request_may_have_reached_server,
        "transport delivery uncertainty remains intact");

    discordcpp::RateLimitMetadataError rate_error{};
    rate_error.scope = discordcpp::RateLimitMetadataScope::global;
    discordcpp::DiscordClientError rate_variant{ std::in_place_type<discordcpp::RateLimitMetadataError>, rate_error };
    check(std::holds_alternative<discordcpp::RateLimitMetadataError>(rate_variant) &&
        std::get<discordcpp::RateLimitMetadataError>(rate_variant).scope == discordcpp::RateLimitMetadataScope::global,
        "rate-limit metadata errors remain a distinct alternative");

    discordcpp::JsonParseError parse_error{};
    parse_error.offset = 12;
    parse_error.path = "/user/id";
    parse_error.http_status = 200;
    parse_error.response_headers.add("X-Trace", "json-response");
    discordcpp::DiscordClientError parse_variant{ std::in_place_type<discordcpp::JsonParseError>, parse_error };
    check(std::get<discordcpp::JsonParseError>(parse_variant).offset == 12 &&
        std::get<discordcpp::JsonParseError>(parse_variant).http_status == 200 &&
        std::get<discordcpp::JsonParseError>(parse_variant).response_headers.contains("x-trace"),
        "JSON parse diagnostics retain location and successful HTTP response metadata");

    discordcpp::ModelValidationError model_error{};
    model_error.path = "/guild/name";
    model_error.http_status = 200;
    discordcpp::DiscordClientError model_variant{ std::in_place_type<discordcpp::ModelValidationError>, model_error };
    check(std::get<discordcpp::ModelValidationError>(model_variant).path == "/guild/name",
        "model validation errors retain field path");

    discordcpp::DiscordClientError local_variant{ std::in_place_type<discordcpp::LocalValidationError>,
        discordcpp::LocalValidationError{ discordcpp::LocalValidationIssue::invalid_endpoint_path, "invalid path" } };
    check(std::get<discordcpp::LocalValidationError>(local_variant).issue ==
        discordcpp::LocalValidationIssue::invalid_endpoint_path,
        "local validation failures remain distinguishable from HTTP errors");
}

void test_client_borrows_existing_orchestration_objects() {
    NullTransport transport;
    discordcpp::RestClient rest_client{ transport };
    TestScheduler scheduler;
    discordcpp::RateLimitCoordinator coordinator{ discordcpp::RateLimitIdentity{ 1 }, scheduler };
    discordcpp::DiscordRestClient client{ rest_client, coordinator, discordcpp::DiscordApiConfig{ 10 } };
    check(client.config().api_version == 10,
        "DiscordRestClient accepts the existing RestClient and coordinator by reference");
}

} // namespace

int main() {
    test_versioned_origin_form_targets();
    test_public_error_model_preserves_existing_errors();
    test_client_borrows_existing_orchestration_objects();

    if (failures == 0) {
        std::cout << "Passed " << checks << " REST API foundation checks.\n";
        return 0;
    }
    std::cerr << failures << " of " << checks << " REST API foundation checks failed.\n";
    return 1;
}
