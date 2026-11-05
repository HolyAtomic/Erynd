#include <discordcpp/discord_rest_client.hpp>
#include <discordcpp/snowflake.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

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

class TestTokenProvider final : public discordcpp::BotTokenProvider {
public:
    explicit TestTokenProvider(std::string token) : token_{ std::move(token) } {}

    bool with_token(const TokenCallback& callback) const override {
        std::lock_guard lock{ mutex_ };
        callback(token_);
        return true;
    }

    void rotate(std::string token) const {
        std::lock_guard lock{ mutex_ };
        token_ = std::move(token);
    }

private:
    mutable std::mutex mutex_;
    mutable std::string token_;
};

class ThrowingTokenProvider final : public discordcpp::BotTokenProvider {
public:
    bool with_token(const TokenCallback&) const override {
        throw std::runtime_error("secret-token-must-not-escape");
    }
};

void test_configuration_validation() {
    discordcpp::DiscordApiConfig config{};
    check(config.validate().has_value(), "default API config is valid");
    config.api_origin = "https://example.test:8443";
    check(config.validate().has_value(), "HTTPS DNS origin and numeric port are accepted");
    config.api_origin = "http://discord.com";
    check(config.validate().has_error() && config.validate().error().issue ==
        discordcpp::LocalValidationIssue::invalid_api_origin, "non-HTTPS origin is rejected");
    config.api_origin = "https://user:password@discord.com";
    check(config.validate().has_error(), "origin credentials are rejected");
    config.api_origin = "https://discord.com/path";
    check(config.validate().has_error(), "origin path is rejected");
    config.api_origin = "https://discord.com:99999";
    check(config.validate().has_error(), "out-of-range origin port is rejected");
    config.api_origin = "https://discord.com";
    config.user_agent = "";
    check(config.validate().has_error() && config.validate().error().issue ==
        discordcpp::LocalValidationIssue::invalid_user_agent, "empty User-Agent is rejected");
    config.user_agent = "agent\r\nX-Injected: yes";
    check(config.validate().has_error(), "User-Agent header injection is rejected");
    config.user_agent = "agent <>";
    check(config.validate().has_error(), "malformed User-Agent product syntax is rejected");
}

void test_path_builder_safety() {
    discordcpp::DiscordApiPathBuilder path{ 10 };
    check(path.append_literal("channels").has_value(), "fixed path segment is accepted");
    check(path.append_snowflake(discordcpp::Snowflake{ 123456789012345678ULL }).has_value(),
        "Snowflake path parameter is accepted");
    check(path.append_literal("messages").has_value(), "additional fixed path segment is accepted");
    const auto built = path.finish();
    check(built.has_value() && built.value() == "/api/v10/channels/123456789012345678/messages",
        "path builder emits canonical versioned origin-form path");

    discordcpp::DiscordApiPathBuilder encoded{ 10 };
    check(encoded.append_literal("widgets").has_value(), "route literal setup succeeds");
    check(encoded.append_parameter("a b%").has_value(), "safe parameter characters are percent-encoded");
    const auto encoded_path = encoded.finish();
    check(encoded_path.has_value() && encoded_path.value() == "/api/v10/widgets/a%20b%25",
        "parameter encoding prevents path structure injection");

    for (const std::string_view invalid : { "", ".", "..", "a/b", "a\\b", "a?b", "a#b", "x\r\ny" }) {
        discordcpp::DiscordApiPathBuilder candidate{ 10 };
        (void)candidate.append_literal("channels");
        const auto added = candidate.append_parameter(invalid);
        check(added.has_error() && added.error().issue == discordcpp::LocalValidationIssue::invalid_path_segment,
            "empty, traversal, separator and control path parameters are rejected");
    }
    for (const std::string_view invalid_literal : { "fragment#x", "encoded%2Fslash" }) {
        discordcpp::DiscordApiPathBuilder candidate{ 10 };
        const auto added = candidate.append_literal(invalid_literal);
        check(added.has_error(), "literal path segments cannot inject fragment or pre-escaped delimiters");
    }
    discordcpp::DiscordApiPathBuilder wrong_version{ 11 };
    (void)wrong_version.append_literal("users");
    (void)wrong_version.append_literal("@me");
    check(wrong_version.finish().has_value(), "independently versioned path builder produces target");
}

void test_authenticated_request_construction_and_ownership() {
    NullTransport transport;
    discordcpp::RestClient rest_client{ transport };
    TestScheduler scheduler;
    discordcpp::RateLimitCoordinator coordinator{ discordcpp::RateLimitIdentity{ 3 }, scheduler };
    auto provider = std::make_shared<TestTokenProvider>("alpha-token");
    std::weak_ptr<const discordcpp::BotTokenProvider> weak_provider = provider;
    discordcpp::DiscordRestClient client{ rest_client, coordinator, discordcpp::DiscordApiConfig{}, provider };
    provider.reset();
    check(!weak_provider.expired(), "client retains provider lifetime through shared ownership");

    discordcpp::DiscordApiPathBuilder path{ 10 };
    (void)path.append_literal("channels");
    (void)path.append_snowflake(discordcpp::Snowflake{ 42 });
    (void)path.append_literal("messages");
    discordcpp::Headers headers;
    headers.add("Content-Type", "application/json");
    const auto deadline = discordcpp::SteadyClock::time_point{} + std::chrono::seconds{ 10 };
    auto request = client.build_request("POST", path, headers, "{}", deadline);
    check(request.has_value(), "authenticated request is constructed");
    if (request.has_value()) {
        check(request.value().headers.find("authorization") == std::optional<std::string_view>{ "Bot alpha-token" },
            "request receives the Discord Bot authorization scheme");
        check(request.value().headers.find("USER-AGENT") ==
            std::optional<std::string_view>{ client.config().user_agent }, "configured User-Agent is installed");
        check(request.value().headers.find("content-type") == std::optional<std::string_view>{ "application/json" },
            "additional non-reserved headers are retained");
        check(request.value().method == "POST" && request.value().target == "/api/v10/channels/42/messages" &&
            request.value().body == "{}" && request.value().deadline == deadline,
            "method, origin-form target, body, and deadline are preserved");
    }

    discordcpp::QueryParameters query;
    check(query.add("limit", 25).has_value() && query.add("before", "a&b=c").has_value(),
        "request query parameters are accepted");
    auto queried_request = client.build_request_with_query("GET", path, query);
    check(queried_request.has_value() && queried_request.value().target ==
        "/api/v10/channels/42/messages?limit=25&before=a%26b%3Dc" &&
        queried_request.value().headers.find("authorization").has_value(),
        "authenticated request construction appends encoded query components after the validated path");
    discordcpp::QueryParameters empty_query;
    auto empty_queried_request = client.build_request_with_query("GET", path, empty_query);
    check(empty_queried_request.has_value() && empty_queried_request.value().target ==
        "/api/v10/channels/42/messages", "empty request query preserves existing target exactly");
    discordcpp::QueryParameters invalid_query;
    check(invalid_query.add("bad", "").has_error(), "invalid query setup is rejected");

    auto provider_shared = std::dynamic_pointer_cast<const TestTokenProvider>(weak_provider.lock());
    provider_shared->rotate("beta-token");
    const auto rotated = client.build_request("GET", "/users/@me");
    check(rotated.has_value() && rotated.value().headers.find("authorization") ==
        std::optional<std::string_view>{ "Bot beta-token" }, "subsequent requests observe synchronized token rotation");
    check(request.has_value() && request.value().headers.find("authorization") ==
        std::optional<std::string_view>{ "Bot alpha-token" }, "previous owned request header remains stable after rotation");

    discordcpp::Headers auth_override;
    auth_override.add("aUtHoRiZaTiOn", "Bot attacker");
    const auto duplicate = client.build_request("GET", "/users/@me", auth_override);
    check(duplicate.has_error() && std::holds_alternative<discordcpp::LocalValidationError>(duplicate.error()) &&
        std::get<discordcpp::LocalValidationError>(duplicate.error()).issue ==
            discordcpp::LocalValidationIssue::reserved_request_header,
        "caller cannot add or override Authorization");
    check(duplicate.has_error() && std::get<discordcpp::LocalValidationError>(duplicate.error()).message.find("beta-token") ==
        std::string::npos, "local request errors do not expose token values");

    for (const auto header_name : { "User-Agent", "Host", "Content-Length", "Transfer-Encoding" }) {
        discordcpp::Headers reserved;
        reserved.add(header_name, "x");
        check(client.build_request("GET", "/users/@me", reserved).has_error(),
            "caller cannot override User-Agent or transport framing headers");
    }

    discordcpp::Headers invalid_control;
    invalid_control.add("X-Test", std::string{ "bad\x01value", 9 });
    check(client.build_request("GET", "/users/@me", invalid_control).has_error(),
        "non-CRLF control characters in caller headers are rejected");

    const auto bad_method = client.build_request("GET\r\nX: yes", "/users/@me");
    check(bad_method.has_error() && std::get<discordcpp::LocalValidationError>(bad_method.error()).issue ==
        discordcpp::LocalValidationIssue::invalid_http_method, "invalid HTTP method is rejected before dispatch");
    const auto bad_path = client.build_request("GET", "https://discord.com/api/v10/users/@me");
    check(bad_path.has_error() && std::get<discordcpp::LocalValidationError>(bad_path.error()).issue ==
        discordcpp::LocalValidationIssue::invalid_endpoint_path, "absolute URL target is rejected");

    discordcpp::DiscordApiPathBuilder mismatched_path{ 11 };
    (void)mismatched_path.append_literal("users");
    (void)mismatched_path.append_literal("@me");
    const auto wrong_version = client.build_request("GET", mismatched_path);
    check(wrong_version.has_error(), "path builder version cannot override the client's configured version");

    std::atomic<int> successful_reads{};
    std::vector<std::thread> readers;
    for (int index = 0; index < 8; ++index) {
        readers.emplace_back([&] {
            const auto concurrent = client.build_request("GET", "/users/@me");
            if (concurrent.has_value() && concurrent.value().headers.contains("authorization")) {
                successful_reads.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    for (auto& reader : readers) reader.join();
    check(successful_reads.load(std::memory_order_relaxed) == 8,
        "provider synchronization supports concurrent request construction");

    ThrowingTokenProvider throwing_provider;
    discordcpp::DiscordRestClient failing_client{ rest_client, coordinator,
        discordcpp::DiscordApiConfig{},
        std::shared_ptr<const discordcpp::BotTokenProvider>{ &throwing_provider, [](const auto*) {} } };
    const auto unavailable = failing_client.build_request("GET", "/users/@me");
    check(unavailable.has_error() && std::holds_alternative<discordcpp::LocalValidationError>(unavailable.error()) &&
        std::get<discordcpp::LocalValidationError>(unavailable.error()).message.find("secret-token-must-not-escape") ==
            std::string::npos, "provider exceptions are suppressed without leaking diagnostic text");
}

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
    discordcpp::DiscordRestClient empty_config_client{ rest_client, coordinator, {} };
    check(empty_config_client.config().api_version == 10,
        "existing empty-brace configuration constructor remains unambiguous");
}

} // namespace

int main() {
    test_versioned_origin_form_targets();
    test_configuration_validation();
    test_path_builder_safety();
    test_authenticated_request_construction_and_ownership();
    test_public_error_model_preserves_existing_errors();
    test_client_borrows_existing_orchestration_objects();

    if (failures == 0) {
        std::cout << "Passed " << checks << " REST API foundation checks.\n";
        return 0;
    }
    std::cerr << failures << " of " << checks << " REST API foundation checks failed.\n";
    return 1;
}
