#include <discordcpp/discord_rest_client.hpp>

#include "fake_transport.hpp"

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

class ManualScheduler final : public discordcpp::IRateLimitScheduler {
public:
    [[nodiscard]] discordcpp::SteadyClock::time_point now() const noexcept override {
        std::lock_guard lock{ mutex_ };
        return now_;
    }

    [[nodiscard]] std::uint64_t generation() const noexcept override {
        std::lock_guard lock{ mutex_ };
        return generation_;
    }

    void wait_for_change(std::uint64_t, std::optional<discordcpp::SteadyClock::time_point> wake_at,
        std::stop_token cancellation) override {
        std::lock_guard lock{ mutex_ };
        if (cancellation.stop_requested()) return;
        if (wake_at && *wake_at > now_) now_ = *wake_at;
        ++generation_;
        ++wait_count_;
    }

    void notify_all() noexcept override {
        std::lock_guard lock{ mutex_ };
        ++generation_;
    }

    [[nodiscard]] std::size_t wait_count() const noexcept {
        std::lock_guard lock{ mutex_ };
        return wait_count_;
    }

private:
    mutable std::mutex mutex_;
    discordcpp::SteadyClock::time_point now_{};
    std::uint64_t generation_{};
    std::size_t wait_count_{};
};

class TestTokenProvider final : public discordcpp::BotTokenProvider {
public:
    bool with_token(const TokenCallback& callback) const override {
        callback("endpoint-test-token");
        return true;
    }
};

class ThrowingTransport final : public discordcpp::IHttpTransport {
public:
    discordcpp::TransportResult send(const discordcpp::HttpRequest&, std::stop_token) override {
        throw std::runtime_error("exception echoed endpoint-test-token");
    }
};

struct Environment final {
    ManualScheduler scheduler;
    test_support::FakeTransport transport;
    discordcpp::RateLimitCoordinator coordinator{ discordcpp::RateLimitIdentity{ 77 }, scheduler };
    discordcpp::RestClient rest{ transport, &Environment::clock_now };
    std::shared_ptr<const TestTokenProvider> token{ std::make_shared<TestTokenProvider>() };
    discordcpp::DiscordRestClient client{ rest, coordinator, discordcpp::DiscordApiConfig{}, token };

    static discordcpp::SteadyClock::time_point clock_now() noexcept { return {}; }
};

discordcpp::HttpResponse response(int status, std::string body) {
    discordcpp::HttpResponse value{};
    value.status = status;
    value.body = std::move(body);
    value.headers.add("X-Trace-Id", "endpoint-test");
    return value;
}

void set_bucket_capacity(discordcpp::HttpResponse& value, std::string bucket,
    std::string remaining = "0", std::string reset_after = "1.25") {
    value.headers.add("X-RateLimit-Bucket", std::move(bucket));
    value.headers.add("X-RateLimit-Limit", "1");
    value.headers.add("X-RateLimit-Remaining", std::move(remaining));
    value.headers.add("X-RateLimit-Reset-After", std::move(reset_after));
}

const std::string user_json = R"json({"id":"123456789012345678","username":"erynd","global_name":null,"avatar":"avatar-hash","bot":true})json";
const std::string guild_json = R"json({"id":"223456789012345678","name":"guild","icon":null,"owner_id":"323456789012345678"})json";
const std::string channel_json = R"json({"id":"423456789012345678","type":0,"guild_id":"223456789012345678","name":"general","topic":null})json";

const std::string channel_collection_json = R"json([
    {"id":"423456789012345678","type":0,"guild_id":"223456789012345678","name":"general","topic":null,"position":0},
    {"id":"423456789012345679","type":2,"guild_id":"223456789012345678","name":"voice","position":1}
])json";
const std::string role_collection_json = R"json([
    {"id":"523456789012345678","name":"@everyone","permissions":"1024","position":0,"color":0,"hoist":false,"managed":false,"mentionable":false},
    {"id":"523456789012345679","name":"moderator","permissions":"18446744073709551616","position":1,"color":16711680,"hoist":true,"managed":false,"mentionable":true,"icon":null,"unicode_emoji":"🛡️"}
])json";
const std::string emoji_collection_json = R"json([
    {"id":"623456789012345678","name":"wave","roles":[],"require_colons":true,"managed":false,"animated":false,"available":true},
    {"id":"623456789012345679","name":"dance","roles":["523456789012345679"],"animated":true}
])json";

template<typename Model, typename Call, typename Validate>
void exercise_guild_collection_endpoint(std::string_view endpoint, std::string_view path_suffix,
    std::string_view body, std::string_view malformed_element_body, Call call, Validate validate_models) {
    constexpr std::uint64_t guild_id = 723456789012345678ULL;
    const auto expected_target = "/api/v10/guilds/" + std::to_string(guild_id) + std::string{ path_suffix };

    {
        Environment env;
        env.transport.queue_response(response(200, std::string{ body }));
        const auto result = call(env, discordcpp::Snowflake{ guild_id });
        check(result.has_value() && result.value().size() == 2,
            std::string{ endpoint } + " decodes multiple elements into a complete typed collection");
        if (result.has_value()) validate_models(result.value());
        const auto requests = env.transport.requests();
        check(requests.size() == 1 && requests.front().method == "GET" &&
            requests.front().target == expected_target && requests.front().body.empty(),
            std::string{ endpoint } + " sends GET to the exact versioned origin-form path");
        if (requests.size() == 1) {
            check(requests.front().headers.find("authorization") ==
                std::optional<std::string_view>{ "Bot endpoint-test-token" } &&
                requests.front().headers.find("user-agent").has_value(),
                std::string{ endpoint } + " uses the authenticated request builder");
        }
        check(env.transport.request_order() == std::vector<std::string>{ expected_target },
            std::string{ endpoint } + " preserves its single dispatch order");
    }
    {
        Environment env;
        env.transport.queue_response(response(200, "[]"));
        const auto result = call(env, discordcpp::Snowflake{ guild_id });
        check(result.has_value() && result.value().empty(),
            std::string{ endpoint } + " decodes an empty collection");
    }
    {
        Environment env;
        env.transport.queue_response(response(200, "{"));
        const auto result = call(env, discordcpp::Snowflake{ guild_id });
        check(result.has_error() && std::holds_alternative<discordcpp::JsonParseError>(result.error()),
            std::string{ endpoint } + " reports malformed JSON as JsonParseError");
    }
    {
        Environment env;
        env.transport.queue_response(response(200, std::string{ malformed_element_body }));
        const auto result = call(env, discordcpp::Snowflake{ guild_id });
        check(result.has_error() && std::holds_alternative<discordcpp::ModelValidationError>(result.error()) &&
            std::get<discordcpp::ModelValidationError>(result.error()).path == "/1/id",
            std::string{ endpoint } + " rejects a malformed second element without returning a truncated collection");
    }
    {
        Environment env;
        env.transport.queue_response(response(403, R"json({"code":50001,"message":"Missing Access"})json"));
        const auto result = call(env, discordcpp::Snowflake{ guild_id });
        check(result.has_error() && std::holds_alternative<discordcpp::DiscordApiError>(result.error()) &&
            std::get<discordcpp::DiscordApiError>(result.error()).http_status == 403,
            std::string{ endpoint } + " preserves Discord API errors");
    }
    {
        Environment env;
        env.transport.queue_error({ discordcpp::TransportFailureCategory::connect, "fixture failure", false });
        const auto result = call(env, discordcpp::Snowflake{ guild_id });
        check(result.has_error() && std::holds_alternative<discordcpp::TransportError>(result.error()) &&
            std::get<discordcpp::TransportError>(result.error()).category == discordcpp::TransportFailureCategory::connect,
            std::string{ endpoint } + " preserves transport failures");
    }
    {
        Environment env;
        env.transport.queue_response(response(429, R"json({"retry_after":"bad"})json"));
        const auto result = call(env, discordcpp::Snowflake{ guild_id });
        check(result.has_error() && std::holds_alternative<discordcpp::RateLimitMetadataError>(result.error()),
            std::string{ endpoint } + " preserves rate-limit metadata errors");
    }
    {
        Environment env;
        const auto result = call(env, discordcpp::Snowflake{ 0 });
        check(result.has_error() && std::holds_alternative<discordcpp::LocalValidationError>(result.error()) &&
            std::get<discordcpp::LocalValidationError>(result.error()).issue ==
                discordcpp::LocalValidationIssue::invalid_snowflake && env.transport.request_count() == 0,
            std::string{ endpoint } + " rejects invalid guild IDs before dispatch");
    }
    {
        Environment env;
        auto first = response(200, std::string{ body });
        set_bucket_capacity(first, std::string{ endpoint } + "-major-resource");
        env.transport.queue_response(std::move(first));
        env.transport.queue_response(response(200, std::string{ body }));
        auto same_guild_again = response(200, std::string{ body });
        set_bucket_capacity(same_guild_again, std::string{ endpoint } + "-major-resource");
        env.transport.queue_response(std::move(same_guild_again));
        check(call(env, discordcpp::Snowflake{ 111 }).has_value(),
            std::string{ endpoint } + " observes the guild-scoped route first");
        check(call(env, discordcpp::Snowflake{ 222 }).has_value() && env.scheduler.wait_count() == 0,
            std::string{ endpoint } + " keeps a different guild major resource isolated");
        check(call(env, discordcpp::Snowflake{ 111 }).has_value() && env.scheduler.wait_count() == 1,
            std::string{ endpoint } + " coordinates the same guild major resource with learned bucket metadata");
    }
}

void test_guild_collection_endpoints() {
    exercise_guild_collection_endpoint<discordcpp::Channel>("guild channels", "/channels",
        channel_collection_json,
        R"json([{"id":"1","type":0},{"id":7,"type":0}])json",
        [](Environment& env, discordcpp::Snowflake guild_id) { return env.client.get_guild_channels(guild_id); },
        [](const std::vector<discordcpp::Channel>& channels) {
            check(channels[0].id.value() == 423456789012345678ULL && channels[1].id.value() == 423456789012345679ULL &&
                channels[0].position.value() == 0 && channels[1].name.value() == "voice",
                "channel IDs, optional position, and collection order decode correctly");
        });
    exercise_guild_collection_endpoint<discordcpp::Role>("guild roles", "/roles",
        role_collection_json,
        R"json([{"id":"1","name":"valid","permissions":"0","position":0,"color":0,"hoist":false,"managed":false,"mentionable":false},{"id":7,"name":"bad","permissions":"0","position":0,"color":0,"hoist":false,"managed":false,"mentionable":false}])json",
        [](Environment& env, discordcpp::Snowflake guild_id) { return env.client.get_guild_roles(guild_id); },
        [](const std::vector<discordcpp::Role>& roles) {
            check(roles[0].id.value() == 523456789012345678ULL &&
                roles[1].permissions == "18446744073709551616" && roles[1].icon.is_null() &&
                roles[1].unicode_emoji.value() == "🛡️",
                "role Snowflakes, wide permission strings, and nullable/optional fields decode correctly");
        });
    exercise_guild_collection_endpoint<discordcpp::Emoji>("guild emojis", "/emojis",
        emoji_collection_json,
        R"json([{"id":"1","name":"valid"},{"id":7,"name":"bad"}])json",
        [](Environment& env, discordcpp::Snowflake guild_id) { return env.client.get_guild_emojis(guild_id); },
        [](const std::vector<discordcpp::Emoji>& emojis) {
            check(emojis[0].id.value() == 623456789012345678ULL && emojis[1].roles.value()[0].value() == 523456789012345679ULL &&
                emojis[0].available.value() && emojis[1].animated.value() && emojis[1].managed.is_absent(),
                "emoji Snowflakes, role references, optional flags, and collection order decode correctly");
        });
}

void test_successful_endpoint_requests() {
    Environment env;
    env.transport.queue_response(response(200, user_json));
    env.transport.queue_response(response(200, guild_json));
    env.transport.queue_response(response(200, channel_json));

    const auto user = env.client.get_current_user();
    const auto guild = env.client.get_guild(discordcpp::Snowflake{ 223456789012345678ULL });
    const auto channel = env.client.get_channel(discordcpp::Snowflake{ 423456789012345678ULL });
    check(user.has_value() && user.value().username == "erynd" && user.value().id.value() == 123456789012345678ULL,
        "current bot user endpoint returns the typed User model");
    check(user.has_value() && user.value().global_name.is_null() && user.value().avatar.value() == "avatar-hash" &&
        user.value().bot.value(), "User optional and nullable fields preserve Field state");
    check(guild.has_value() && guild.value().name == "guild" &&
        guild.value().owner_id.value().value() == 323456789012345678ULL && guild.value().icon.is_null(),
        "get guild returns typed Snowflake and nullable Guild fields");
    check(channel.has_value() && channel.value().type == 0 && channel.value().name.value() == "general" &&
        channel.value().topic.is_null(), "get channel returns typed Channel fields and nullable topic");

    const auto requests = env.transport.requests();
    check(requests.size() == 3, "three endpoint methods dispatch exactly one request each");
    if (requests.size() == 3) {
        const std::string_view expected_targets[]{ "/api/v10/users/@me", "/api/v10/guilds/223456789012345678",
            "/api/v10/channels/423456789012345678" };
        for (std::size_t index = 0; index < requests.size(); ++index) {
            check(requests[index].method == "GET" && requests[index].target == expected_targets[index] &&
                requests[index].target.front() == '/' && requests[index].body.empty(),
                "endpoint request uses GET, exact origin-form path, and empty body");
            check(requests[index].headers.size() == 2 &&
                requests[index].headers.find("authorization") ==
                    std::optional<std::string_view>{ "Bot endpoint-test-token" } &&
                requests[index].headers.find("user-agent").has_value(),
                "endpoint request has only Authorization and User-Agent headers");
        }
        check(env.transport.request_order() == std::vector<std::string>{
            "/api/v10/users/@me", "/api/v10/guilds/223456789012345678",
            "/api/v10/channels/423456789012345678" }, "request dispatch order matches endpoint call order");
    }
}

void test_error_propagation_and_local_rejection() {
    {
        Environment env;
        env.transport.queue_error({ discordcpp::TransportFailureCategory::connect,
            "fixture connection failure", false });
        const auto result = env.client.get_current_user();
        check(result.has_error() && std::holds_alternative<discordcpp::TransportError>(result.error()) &&
            std::get<discordcpp::TransportError>(result.error()).category == discordcpp::TransportFailureCategory::connect &&
            !std::get<discordcpp::TransportError>(result.error()).request_may_have_reached_server,
            "transport failures preserve category and delivery uncertainty");
    }
    {
        Environment env;
        env.transport.queue_response(response(403,
            R"json({"code":50001,"message":"Missing Access endpoint-test-token"})json"));
        const auto result = env.client.get_current_user();
        check(result.has_error() && std::holds_alternative<discordcpp::DiscordApiError>(result.error()) &&
            std::get<discordcpp::DiscordApiError>(result.error()).discord_code == 50001 &&
            std::get<discordcpp::DiscordApiError>(result.error()).message == "Missing Access [REDACTED]" &&
            std::get<discordcpp::DiscordApiError>(result.error()).raw_body.find("endpoint-test-token") == std::string::npos &&
            std::get<discordcpp::DiscordApiError>(result.error()).response_headers.contains("x-trace-id"),
            "non-2xx API response retains structured diagnostics while redacting echoed credentials");
    }
    {
        Environment env;
        env.transport.queue_error({ discordcpp::TransportFailureCategory::io,
            "transport diagnostic echoed endpoint-test-token", true });
        const auto result = env.client.get_current_user();
        check(result.has_error() && std::holds_alternative<discordcpp::TransportError>(result.error()) &&
            std::get<discordcpp::TransportError>(result.error()).message.find("endpoint-test-token") == std::string::npos &&
            std::get<discordcpp::TransportError>(result.error()).request_may_have_reached_server,
            "transport diagnostics preserve error classification and uncertainty without exposing the token");
    }
    {
        ThrowingTransport transport;
        ManualScheduler scheduler;
        discordcpp::RateLimitCoordinator coordinator{ discordcpp::RateLimitIdentity{ 78 }, scheduler };
        discordcpp::RestClient rest{ transport };
        auto provider = std::make_shared<TestTokenProvider>();
        discordcpp::DiscordRestClient client{ rest, coordinator, discordcpp::DiscordApiConfig{}, provider };
        const auto result = client.get_current_user();
        check(result.has_error() && std::holds_alternative<discordcpp::TransportError>(result.error()) &&
            std::get<discordcpp::TransportError>(result.error()).message.find("endpoint-test-token") == std::string::npos &&
            std::get<discordcpp::TransportError>(result.error()).request_may_have_reached_server,
            "transport exceptions become sanitized uncertain transport failures");
    }
    {
        Environment env;
        env.transport.queue_response(response(429, R"({"message":"slow down"})"));
        const auto result = env.client.get_current_user();
        check(result.has_error() && std::holds_alternative<discordcpp::RateLimitMetadataError>(result.error()) &&
            std::get<discordcpp::RateLimitMetadataError>(result.error()).response.has_value() &&
            std::get<discordcpp::RateLimitMetadataError>(result.error()).response->http_status == 429,
            "429 with missing timing preserves rate-limit metadata error and original response");
    }
    {
        Environment env;
        env.transport.queue_response(response(200, "{"));
        const auto result = env.client.get_current_user();
        check(result.has_error() && std::holds_alternative<discordcpp::JsonParseError>(result.error()) &&
            std::get<discordcpp::JsonParseError>(result.error()).http_status == 200,
            "malformed successful JSON remains a parse error with HTTP status");
    }
    {
        Environment env;
        env.transport.queue_response(response(200, R"({"id":"1","username":false})"));
        const auto result = env.client.get_current_user();
        check(result.has_error() && std::holds_alternative<discordcpp::ModelValidationError>(result.error()) &&
            std::get<discordcpp::ModelValidationError>(result.error()).path == "/username",
            "valid JSON with an invalid model schema remains a model validation error");
    }
    {
        Environment env;
        const auto result = env.client.get_guild(discordcpp::Snowflake{ 0 });
        check(result.has_error() && std::holds_alternative<discordcpp::LocalValidationError>(result.error()) &&
            std::get<discordcpp::LocalValidationError>(result.error()).issue == discordcpp::LocalValidationIssue::invalid_snowflake &&
            env.transport.request_count() == 0,
            "zero guild ID is rejected locally before any transport dispatch");
    }
    {
        Environment env;
        const auto result = env.client.get_channel(discordcpp::Snowflake{ 0 });
        check(result.has_error() && std::holds_alternative<discordcpp::LocalValidationError>(result.error()) &&
            std::get<discordcpp::LocalValidationError>(result.error()).issue == discordcpp::LocalValidationIssue::invalid_snowflake &&
            env.transport.request_count() == 0,
            "zero channel ID is rejected locally before any transport dispatch");
    }
    {
        Environment env;
        std::stop_source source;
        source.request_stop();
        const auto result = env.client.get_current_user(std::nullopt, source.get_token());
        check(result.has_error() && std::holds_alternative<discordcpp::TransportError>(result.error()) &&
            std::get<discordcpp::TransportError>(result.error()).category == discordcpp::TransportFailureCategory::cancelled &&
            env.transport.request_count() == 0,
            "pre-cancelled endpoint call is rejected before dispatch");
    }
    {
        Environment env;
        const auto result = env.client.get_current_user(discordcpp::SteadyClock::time_point{});
        check(result.has_error() && std::holds_alternative<discordcpp::TransportError>(result.error()) &&
            std::get<discordcpp::TransportError>(result.error()).category == discordcpp::TransportFailureCategory::timeout &&
            !std::get<discordcpp::TransportError>(result.error()).request_may_have_reached_server &&
            env.transport.request_count() == 0,
            "expired endpoint deadline is rejected before dispatch with known-not-sent semantics");
    }
    {
        auto invalid_config = discordcpp::DiscordApiConfig{};
        invalid_config.api_origin = "http://example.invalid";
        Environment env;
        discordcpp::DiscordRestClient invalid_client{ env.rest, env.coordinator, invalid_config, env.token };
        const auto result = invalid_client.get_current_user();
        check(result.has_error() && std::holds_alternative<discordcpp::LocalValidationError>(result.error()) &&
            std::get<discordcpp::LocalValidationError>(result.error()).issue ==
                discordcpp::LocalValidationIssue::invalid_api_origin && env.transport.request_count() == 0,
            "invalid endpoint configuration is rejected before transport dispatch");
    }
}

void test_explicit_route_metadata_and_coordination() {
    {
        Environment env;
        auto guild_one = response(200, guild_json);
        set_bucket_capacity(guild_one, "same-canonical-hash");
        env.transport.queue_response(std::move(guild_one));
        env.transport.queue_response(response(200, guild_json));
        auto guild_one_again = response(200, guild_json);
        set_bucket_capacity(guild_one_again, "same-canonical-hash");
        env.transport.queue_response(std::move(guild_one_again));

        check(env.client.get_guild(discordcpp::Snowflake{ 111 }).has_value(),
            "first guild request teaches canonical bucket metadata");
        check(env.client.get_guild(discordcpp::Snowflake{ 222 }).has_value(),
            "different guild major ID remains separately schedulable");
        check(env.scheduler.wait_count() == 0,
            "canonical bucket mapping preserves typed major-resource separation");
        check(env.client.get_guild(discordcpp::Snowflake{ 111 }).has_value(),
            "same guild route proceeds after deterministic reset advancement");
        check(env.scheduler.wait_count() == 1,
            "same explicit guild route and major ID observe the learned bucket cooldown");
    }
    {
        Environment env;
        auto guild = response(200, guild_json);
        set_bucket_capacity(guild, "same-hash-different-major-kind");
        env.transport.queue_response(std::move(guild));
        auto channel = response(200, channel_json);
        set_bucket_capacity(channel, "same-hash-different-major-kind");
        env.transport.queue_response(std::move(channel));
        check(env.client.get_guild(discordcpp::Snowflake{ 777 }).has_value(),
            "guild call records its guild major-resource kind");
        check(env.client.get_channel(discordcpp::Snowflake{ 777 }).has_value(),
            "channel call records its channel major-resource kind");
        check(env.scheduler.wait_count() == 0,
            "equal snowflake text with different typed major-resource kinds stays isolated");
    }
    {
        Environment env;
        env.transport.queue_response(response(429, R"({"retry_after":1.25,"global":true})"));
        env.transport.queue_response(response(200, guild_json));
        const auto limited = env.client.get_current_user();
        check(limited.has_error() && std::holds_alternative<discordcpp::DiscordApiError>(limited.error()) &&
            std::get<discordcpp::DiscordApiError>(limited.error()).http_status == 429,
            "valid global 429 remains an API error rather than being retried");
        check(env.client.get_guild(discordcpp::Snowflake{ 888 }).has_value(),
            "global cooldown is observed by a different endpoint route");
        check(env.scheduler.wait_count() == 1,
            "explicit ordinary route metadata participates in the shared global cooldown");
        check(env.transport.request_count() == 2,
            "global cooldown waits and dispatches once without replay");
    }
    {
        Environment env;
        auto shared = response(429, R"({"retry_after":1.0})");
        shared.headers.add("X-RateLimit-Scope", "shared");
        env.transport.queue_response(std::move(shared));
        const auto result = env.client.get_channel(discordcpp::Snowflake{ 999 });
        check(result.has_error() && std::holds_alternative<discordcpp::RateLimitMetadataError>(result.error()) &&
            std::get<discordcpp::RateLimitMetadataError>(result.error()).issue ==
                discordcpp::RateLimitMetadataIssue::shared_resource_required &&
            std::get<discordcpp::RateLimitMetadataError>(result.error()).response.has_value(),
            "shared-scope 429 preserves metadata error when endpoint lacks explicit shared-resource key");
    }
}

} // namespace

int main() {
    test_successful_endpoint_requests();
    test_error_propagation_and_local_rejection();
    test_explicit_route_metadata_and_coordination();
    test_guild_collection_endpoints();
    if (failures == 0) {
        std::cout << "Passed " << checks << " Discord REST endpoint checks.\n";
        return 0;
    }
    std::cerr << failures << " of " << checks << " Discord REST endpoint checks failed.\n";
    return 1;
}
