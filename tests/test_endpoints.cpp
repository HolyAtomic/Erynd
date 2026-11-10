#include <discordcpp/discord_rest_client.hpp>

#include "fake_transport.hpp"

#include <chrono>
#include <array>
#include <cstdint>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
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
const std::string guild_member_json = R"json({
    "user":{"id":"823456789012345678","username":"member","global_name":null,"avatar":"user-avatar","bot":false},
    "nick":null,"avatar":"guild-avatar","roles":["523456789012345679","523456789012345680"],
    "joined_at":"2024-01-02T03:04:05.123Z","premium_since":null,"deaf":false,"mute":true,
    "pending":false,"permissions":"18446744073709551616","future_field":{"ignored":true}
})json";
const std::string guild_role_json = R"json({
    "id":"523456789012345679","name":"moderator","permissions":"18446744073709551616",
    "position":1,"color":16711680,"hoist":true,"managed":false,"mentionable":true,
    "icon":null,"unicode_emoji":"🛡️","future_field":true
})json";
const std::string modified_guild_member_json = R"json({
    "user":{"id":"823456789012345678","username":"member"},"nick":"renamed",
    "roles":["523456789012345679"],"joined_at":null,"deaf":false,"mute":false
})json";
const std::string message_json = R"json({
    "id":"923456789012345678","channel_id":"423456789012345678",
    "author":{"id":"123456789012345678","username":"erynd","bot":true},
    "content":"hello","timestamp":"2025-03-04T05:06:07.123Z",
    "edited_timestamp":null,"tts":false,"mention_everyone":false,
    "mentions":[],"mention_roles":[],"pinned":false,"type":0,"flags":0,
    "future_message_field":{"ignored":true}
})json";

void test_create_message_endpoint() {
    constexpr std::uint64_t channel_id = 423456789012345678ULL;
    const std::string expected_target = "/api/v10/channels/" + std::to_string(channel_id) + "/messages";
    {
        Environment env;
        env.transport.queue_response(response(200, message_json));
        const discordcpp::CreateMessageRequest request{ "hello \"Discord\" 🌠" };
        const auto result = env.client.create_message(discordcpp::Snowflake{ channel_id }, request);
        check(result.has_value() && result.value().id.value() == 923456789012345678ULL &&
            result.value().channel_id.value() == channel_id && result.value().author.username == "erynd" &&
            result.value().content == "hello", "Create Message decodes the returned typed Message");
        const auto requests = env.transport.requests();
        check(requests.size() == 1 && requests[0].method == "POST" && requests[0].target == expected_target,
            "Create Message sends POST to the exact API v10 path");
        if (requests.size() == 1) {
            check(requests[0].headers.find("authorization") ==
                std::optional<std::string_view>{ "Bot endpoint-test-token" } &&
                requests[0].headers.find("user-agent").has_value() &&
                requests[0].headers.find("content-type") ==
                std::optional<std::string_view>{ "application/json" },
                "Create Message uses authenticated headers and JSON content type");
            check(requests[0].body == R"json({"content":"hello \"Discord\" 🌠"})json",
                "Create Message serializes only the supported content field with JSON escaping");
        }
    }
    {
        Environment env;
        env.transport.queue_response(response(200, message_json));
        check(env.client.create_message(discordcpp::Snowflake{ channel_id },
            discordcpp::CreateMessageRequest{ "snowman ☃️, rocket 🚀" }).has_value(),
            "Create Message accepts valid multibyte UTF-8 content");
    }
    {
        Environment env;
        std::string unicode_content;
        for (int index = 0; index < 1000; ++index) unicode_content += "🚀";
        env.transport.queue_response(response(200, message_json));
        check(env.client.create_message(discordcpp::Snowflake{ channel_id },
            discordcpp::CreateMessageRequest{ std::move(unicode_content) }).has_value(),
            "Create Message counts Unicode scalar values rather than UTF-8 bytes");
    }
    {
        Environment env;
        env.transport.queue_response(response(200, message_json));
        check(env.client.create_message(discordcpp::Snowflake{ channel_id },
            discordcpp::CreateMessageRequest{ std::string(2000, 'x') }).has_value(),
            "Create Message accepts content at the 2000 Unicode scalar limit");
    }
    for (const auto& [content, description] : std::vector<std::pair<std::string, std::string>>{
             { "", "empty content" },
             { std::string(2001, 'x'), "content beyond the documented limit" },
             { std::string{ "\xC0\xAF", 2 }, "overlong UTF-8" },
             { std::string{ "\x80", 1 }, "orphan UTF-8 continuation" },
             { std::string{ "\xE2\x82", 2 }, "truncated UTF-8 sequence" },
             { std::string{ "\xED\xA0\x80", 3 }, "UTF-8 encoded surrogate" },
             { std::string{ "\xF4\x90\x80\x80", 4 }, "code point above Unicode range" } }) {
        Environment env;
        const auto result = env.client.create_message(discordcpp::Snowflake{ channel_id },
            discordcpp::CreateMessageRequest{ content });
        check(result.has_error() && std::holds_alternative<discordcpp::LocalValidationError>(result.error()) &&
            std::get<discordcpp::LocalValidationError>(result.error()).issue ==
                discordcpp::LocalValidationIssue::invalid_request_body && env.transport.request_count() == 0,
            "Create Message rejects " + description + " before dispatch");
    }
    {
        Environment env;
        const auto result = env.client.create_message(discordcpp::Snowflake{ 0 },
            discordcpp::CreateMessageRequest{ "hello" });
        check(result.has_error() && std::holds_alternative<discordcpp::LocalValidationError>(result.error()) &&
            std::get<discordcpp::LocalValidationError>(result.error()).issue ==
                discordcpp::LocalValidationIssue::invalid_snowflake && env.transport.request_count() == 0,
            "Create Message rejects zero channel ID before dispatch");
    }
    {
        Environment env;
        env.transport.queue_response(response(200, ""));
        const auto result = env.client.create_message(discordcpp::Snowflake{ channel_id },
            discordcpp::CreateMessageRequest{ "hello" });
        check(result.has_error() && std::holds_alternative<discordcpp::JsonParseError>(result.error()),
            "Create Message rejects an empty successful response instead of returning a partial Message");
    }
    {
        Environment env;
        env.transport.queue_response(response(200, "{"));
        const auto result = env.client.create_message(discordcpp::Snowflake{ channel_id },
            discordcpp::CreateMessageRequest{ "hello" });
        check(result.has_error() && std::holds_alternative<discordcpp::JsonParseError>(result.error()),
            "Create Message preserves malformed JSON response errors");
    }
    {
        Environment env;
        env.transport.queue_response(response(200, R"json({"id":"1"})json"));
        const auto result = env.client.create_message(discordcpp::Snowflake{ channel_id },
            discordcpp::CreateMessageRequest{ "hello" });
        check(result.has_error() && std::holds_alternative<discordcpp::ModelValidationError>(result.error()),
            "Create Message rejects incomplete returned Message schemas");
    }
    {
        Environment env;
        env.transport.queue_response(response(201, message_json));
        const auto result = env.client.create_message(discordcpp::Snowflake{ channel_id },
            discordcpp::CreateMessageRequest{ "hello" });
        check(result.has_error() && std::holds_alternative<discordcpp::TransportError>(result.error()) &&
            std::get<discordcpp::TransportError>(result.error()).category ==
                discordcpp::TransportFailureCategory::protocol,
            "Create Message rejects unexpected successful HTTP status instead of decoding it");
    }
    {
        Environment env;
        env.transport.queue_response(response(403, R"json({"code":50013,"message":"Missing Permissions"})json"));
        const auto result = env.client.create_message(discordcpp::Snowflake{ channel_id },
            discordcpp::CreateMessageRequest{ "hello" });
        check(result.has_error() && std::holds_alternative<discordcpp::DiscordApiError>(result.error()) &&
            std::get<discordcpp::DiscordApiError>(result.error()).http_status == 403,
            "Create Message preserves Discord permission errors as API errors");
    }
    {
        Environment env;
        env.transport.queue_error({ discordcpp::TransportFailureCategory::timeout,
            "fixture timeout", true });
        const auto result = env.client.create_message(discordcpp::Snowflake{ channel_id },
            discordcpp::CreateMessageRequest{ "hello" });
        check(result.has_error() && std::holds_alternative<discordcpp::TransportError>(result.error()) &&
            std::get<discordcpp::TransportError>(result.error()).request_may_have_reached_server,
            "Create Message preserves uncertain transport failure without replay");
        check(env.transport.request_count() == 1, "Create Message dispatches a POST at most once");
    }
    {
        Environment env;
        env.transport.queue_response(response(429, R"json({"retry_after":"invalid"})json"));
        const auto result = env.client.create_message(discordcpp::Snowflake{ channel_id },
            discordcpp::CreateMessageRequest{ "hello" });
        check(result.has_error() && std::holds_alternative<discordcpp::RateLimitMetadataError>(result.error()),
            "Create Message preserves malformed rate-limit metadata errors");
    }
    {
        Environment env;
        auto limited = response(200, message_json);
        set_bucket_capacity(limited, "create-message-channel-bucket");
        env.transport.queue_response(std::move(limited));
        env.transport.queue_response(response(200, message_json));
        auto same_channel = response(200, message_json);
        set_bucket_capacity(same_channel, "create-message-channel-bucket");
        env.transport.queue_response(std::move(same_channel));
        check(env.client.create_message(discordcpp::Snowflake{ 111 },
            discordcpp::CreateMessageRequest{ "first" }).has_value(),
            "Create Message dispatches with explicit channel-major route metadata");
        check(env.client.create_message(discordcpp::Snowflake{ 222 },
            discordcpp::CreateMessageRequest{ "other channel" }).has_value() && env.scheduler.wait_count() == 0,
            "Create Message rate-limit bucket keeps different channel major IDs separate");
        check(env.client.create_message(discordcpp::Snowflake{ 111 },
            discordcpp::CreateMessageRequest{ "same channel" }).has_value() && env.scheduler.wait_count() == 1,
            "Create Message route shares discovered bucket for the same channel major ID");
        check(env.transport.request_order() == std::vector<std::string>{
            "/api/v10/channels/111/messages", "/api/v10/channels/222/messages", "/api/v10/channels/111/messages" },
            "Create Message preserves deterministic dispatch order");
    }
}

void test_get_message_endpoint() {
    constexpr std::uint64_t channel_id = 423456789012345678ULL;
    constexpr std::uint64_t message_id = 923456789012345678ULL;
    const std::string expected_target = "/api/v10/channels/" + std::to_string(channel_id) +
        "/messages/" + std::to_string(message_id);
    {
        Environment env;
        env.transport.queue_response(response(200, message_json));
        const auto result = env.client.get_message(discordcpp::Snowflake{ channel_id },
            discordcpp::Snowflake{ message_id });
        check(result.has_value() && result.value().id.value() == message_id &&
            result.value().channel_id.value() == channel_id &&
            result.value().author.id.value() == 123456789012345678ULL &&
            result.value().edited_timestamp.is_null() && result.value().flags.has_value() &&
            result.value().content == "hello",
            "Get Message decodes Snowflakes and the existing nullable/optional Message fields");
        const auto requests = env.transport.requests();
        check(requests.size() == 1 && requests[0].method == "GET" &&
            requests[0].target == expected_target && requests[0].body.empty(),
            "Get Message sends GET to the exact API v10 path without a request body");
        if (requests.size() == 1) {
            check(requests[0].headers.find("authorization") ==
                std::optional<std::string_view>{ "Bot endpoint-test-token" } &&
                requests[0].headers.find("user-agent") == std::optional<std::string_view>{
                    "Erynd++/0.1.0 (C++ Discord API library)" },
                "Get Message uses the authenticated request builder");
        }
    }
    {
        Environment env;
        env.transport.queue_response(response(201, message_json));
        check(env.client.get_message(discordcpp::Snowflake{ channel_id },
            discordcpp::Snowflake{ message_id }).has_value(),
            "Get Message follows RestClient's 2xx success class because the endpoint docs omit a specific status");
    }
    {
        Environment env;
        auto first = response(200, message_json);
        set_bucket_capacity(first, "get-message-channel-bucket");
        env.transport.queue_response(std::move(first));
        env.transport.queue_response(response(200, message_json));
        env.transport.queue_response(response(200, message_json));
        check(env.client.get_message(discordcpp::Snowflake{ 111 }, discordcpp::Snowflake{ 10 }).has_value(),
            "Get Message first request learns the route bucket for a Channel major resource");
        check(env.client.get_message(discordcpp::Snowflake{ 222 }, discordcpp::Snowflake{ 10 }).has_value() &&
            env.scheduler.wait_count() == 0,
            "Get Message isolates the same route across different Channel major IDs");
        check(env.client.get_message(discordcpp::Snowflake{ 111 }, discordcpp::Snowflake{ 11 }).has_value() &&
            env.scheduler.wait_count() == 1,
            "Get Message treats message ID as a normal route parameter within one Channel bucket");
        check(env.transport.request_order() == std::vector<std::string>{
            "/api/v10/channels/111/messages/10", "/api/v10/channels/222/messages/10",
            "/api/v10/channels/111/messages/11" },
            "Get Message records exact deterministic request ordering");
    }
    {
        auto empty_content = message_json;
        const std::string content_field = "\"content\":\"hello\"";
        const auto content_start = empty_content.find(content_field);
        if (content_start != std::string::npos) empty_content.replace(content_start, content_field.size(),
            "\"content\":\"\"");
        const std::string flags_field = "\"flags\":0,";
        const auto flags_start = empty_content.find(flags_field);
        if (flags_start != std::string::npos) empty_content.erase(flags_start, flags_field.size());
        Environment env;
        env.transport.queue_response(response(200, std::move(empty_content)));
        const auto result = env.client.get_message(discordcpp::Snowflake{ channel_id },
            discordcpp::Snowflake{ message_id });
        check(result.has_value() && result.value().content.empty() &&
            result.value().edited_timestamp.is_null() && result.value().flags.is_absent(),
            "Get Message preserves empty content, nullable timestamp, absent flags, and ignores unknown fields");
    }
    for (const auto [channel, message] : std::vector<std::pair<std::uint64_t, std::uint64_t>>{
             { 0, message_id }, { channel_id, 0 } }) {
        Environment env;
        const auto result = env.client.get_message(discordcpp::Snowflake{ channel },
            discordcpp::Snowflake{ message });
        check(result.has_error() && std::holds_alternative<discordcpp::LocalValidationError>(result.error()) &&
            std::get<discordcpp::LocalValidationError>(result.error()).issue ==
                discordcpp::LocalValidationIssue::invalid_snowflake && env.transport.request_count() == 0,
            "Get Message rejects zero IDs before dispatch");
    }
    {
        Environment env;
        env.transport.queue_response(response(200, ""));
        const auto result = env.client.get_message(discordcpp::Snowflake{ channel_id },
            discordcpp::Snowflake{ message_id });
        check(result.has_error() && std::holds_alternative<discordcpp::JsonParseError>(result.error()),
            "Get Message rejects an empty successful response as malformed JSON");
    }
    {
        Environment env;
        env.transport.queue_response(response(200, "not-json"));
        const auto result = env.client.get_message(discordcpp::Snowflake{ channel_id },
            discordcpp::Snowflake{ message_id });
        check(result.has_error() && std::holds_alternative<discordcpp::JsonParseError>(result.error()),
            "Get Message preserves malformed JSON errors");
    }
    {
        Environment env;
        env.transport.queue_response(response(200, R"json({"id":"1"})json"));
        const auto result = env.client.get_message(discordcpp::Snowflake{ channel_id },
            discordcpp::Snowflake{ message_id });
        check(result.has_error() && std::holds_alternative<discordcpp::ModelValidationError>(result.error()),
            "Get Message rejects missing required Message fields");
    }
    {
        auto wrong_type = message_json;
        const auto roles_start = wrong_type.find("\"mention_roles\":[]");
        if (roles_start != std::string::npos) wrong_type.replace(roles_start, 18, "\"mention_roles\":[17]");
        Environment env;
        env.transport.queue_response(response(200, std::move(wrong_type)));
        const auto result = env.client.get_message(discordcpp::Snowflake{ channel_id },
            discordcpp::Snowflake{ message_id });
        check(result.has_error() && std::holds_alternative<discordcpp::ModelValidationError>(result.error()),
            "Get Message rejects incorrect known field types");
    }
    {
        auto malformed_nested = message_json;
        const auto username_start = malformed_nested.find("\"username\":\"erynd\"");
        if (username_start != std::string::npos) malformed_nested.replace(username_start, 18,
            "\"username\":false");
        Environment env;
        env.transport.queue_response(response(200, std::move(malformed_nested)));
        const auto result = env.client.get_message(discordcpp::Snowflake{ channel_id },
            discordcpp::Snowflake{ message_id });
        check(result.has_error() && std::holds_alternative<discordcpp::ModelValidationError>(result.error()),
            "Get Message rejects malformed nested User objects");
    }
    {
        Environment env;
        env.transport.queue_response(response(404, R"json({"code":10008,"message":"Unknown Message"})json"));
        const auto result = env.client.get_message(discordcpp::Snowflake{ channel_id },
            discordcpp::Snowflake{ message_id });
        check(result.has_error() && std::holds_alternative<discordcpp::DiscordApiError>(result.error()) &&
            std::get<discordcpp::DiscordApiError>(result.error()).http_status == 404,
            "Get Message preserves 404 API errors instead of decoding an error body as Message");
    }
    {
        Environment env;
        env.transport.queue_response(response(403, R"json({"code":50001,"message":"Missing Access"})json"));
        const auto result = env.client.get_message(discordcpp::Snowflake{ channel_id },
            discordcpp::Snowflake{ message_id });
        check(result.has_error() && std::holds_alternative<discordcpp::DiscordApiError>(result.error()) &&
            std::get<discordcpp::DiscordApiError>(result.error()).http_status == 403,
            "Get Message preserves permission errors as Discord API errors");
    }
    {
        Environment env;
        env.transport.queue_error({ discordcpp::TransportFailureCategory::io, "fixture IO failure", true });
        const auto result = env.client.get_message(discordcpp::Snowflake{ channel_id },
            discordcpp::Snowflake{ message_id });
        check(result.has_error() && std::holds_alternative<discordcpp::TransportError>(result.error()) &&
            std::get<discordcpp::TransportError>(result.error()).request_may_have_reached_server,
            "Get Message preserves transport delivery uncertainty");
        check(env.transport.request_count() == 1, "Get Message does not retry transport failures");
    }
    {
        Environment env;
        env.transport.queue_response(response(429, R"json({"retry_after":1.0,"global":false})json"));
        const auto result = env.client.get_message(discordcpp::Snowflake{ channel_id },
            discordcpp::Snowflake{ message_id });
        check(result.has_error() && std::holds_alternative<discordcpp::DiscordApiError>(result.error()) &&
            std::get<discordcpp::DiscordApiError>(result.error()).http_status == 429,
            "Get Message preserves valid rate-limit responses as API errors without replay");
        check(env.transport.request_count() == 1, "Get Message sends a rate-limited request exactly once");
    }
}

void test_get_channel_messages_endpoint() {
    constexpr std::uint64_t channel_id = 423456789012345678ULL;
    constexpr std::uint64_t first_message_id = 923456789012345678ULL;
    constexpr std::uint64_t second_message_id = 923456789012345679ULL;
    const auto message_with_id = [](std::uint64_t id) {
        auto json = message_json;
        const std::string field = "\"id\":\"923456789012345678\"";
        const auto position = json.find(field);
        if (position != std::string::npos) json.replace(position, field.size(),
            "\"id\":\"" + std::to_string(id) + "\"");
        return json;
    };
    const std::string route_target = "/api/v10/channels/" + std::to_string(channel_id) + "/messages";

    {
        Environment env;
        env.transport.queue_response(response(200, "[]"));
        const auto result = env.client.get_channel_messages(discordcpp::Snowflake{ channel_id }, {});
        const auto requests = env.transport.requests();
        check(result.has_value() && result.value().empty(),
            "Get Channel Messages accepts an empty Message array");
        check(requests.size() == 1 && requests[0].method == "GET" &&
            requests[0].target == route_target && requests[0].body.empty(),
            "default Get Channel Messages omits all query parameters and sends exactly one GET");
        if (requests.size() == 1) {
            check(requests[0].headers.find("authorization") ==
                std::optional<std::string_view>{ "Bot endpoint-test-token" } &&
                requests[0].headers.find("user-agent") == std::optional<std::string_view>{
                    "Erynd++/0.1.0 (C++ Discord API library)" },
                "Get Channel Messages uses authenticated Authorization and User-Agent headers");
        }
    }
    {
        const auto first = message_with_id(first_message_id);
        const auto second = message_with_id(second_message_id);
        Environment env;
        env.transport.queue_response(response(200, "[" + first + "," + second + "]"));
        const auto result = env.client.get_channel_messages(discordcpp::Snowflake{ channel_id }, {});
        check(result.has_value() && result.value().size() == 2 &&
            result.value()[0].id.value() == first_message_id && result.value()[1].id.value() == second_message_id &&
            result.value()[0].channel_id.value() == channel_id && result.value()[0].content == "hello",
            "Get Channel Messages fully decodes multiple typed Message elements in response order");
    }
    {
        Environment env;
        env.transport.queue_response(response(200, "[" + message_json + "]"));
        const auto result = env.client.get_channel_messages(discordcpp::Snowflake{ channel_id }, {});
        check(result.has_value() && result.value().size() == 1 &&
            result.value()[0].edited_timestamp.is_null() && result.value()[0].flags.has_value(),
            "Get Channel Messages preserves Message nullable and optional field semantics");
    }
    for (const auto limit : { 1, 100 }) {
        Environment env;
        env.transport.queue_response(response(200, "[]"));
        discordcpp::GetChannelMessagesRequest request;
        request.limit = limit;
        const auto result = env.client.get_channel_messages(discordcpp::Snowflake{ channel_id }, request);
        check(result.has_value() && env.transport.requests().size() == 1 &&
            env.transport.requests()[0].target == route_target + "?limit=" + std::to_string(limit),
            "Get Channel Messages serializes valid limit boundary " + std::to_string(limit));
    }
    for (const auto limit : { -1, 0, 101 }) {
        Environment env;
        discordcpp::GetChannelMessagesRequest request;
        request.limit = limit;
        const auto result = env.client.get_channel_messages(discordcpp::Snowflake{ channel_id }, request);
        check(result.has_error() && std::holds_alternative<discordcpp::LocalValidationError>(result.error()) &&
            std::get<discordcpp::LocalValidationError>(result.error()).issue ==
                discordcpp::LocalValidationIssue::invalid_query_parameter && env.transport.request_count() == 0,
            "Get Channel Messages rejects out-of-range limit before dispatch");
    }
    for (const auto [key, cursor] : std::vector<std::pair<std::string, std::uint64_t>>{
             { "before", 800000000000000001ULL }, { "after", 800000000000000002ULL },
             { "around", 800000000000000003ULL } }) {
        Environment env;
        env.transport.queue_response(response(200, "[]"));
        discordcpp::GetChannelMessagesRequest request;
        if (key == "before") request.before = discordcpp::Snowflake{ cursor };
        else if (key == "after") request.after = discordcpp::Snowflake{ cursor };
        else request.around = discordcpp::Snowflake{ cursor };
        const auto result = env.client.get_channel_messages(discordcpp::Snowflake{ channel_id }, request);
        check(result.has_value() && env.transport.requests().size() == 1 &&
            env.transport.requests()[0].target == route_target + "?" + key + "=" + std::to_string(cursor),
            "Get Channel Messages serializes the typed " + key + " cursor as decimal");
    }
    {
        Environment env;
        env.transport.queue_response(response(200, "[]"));
        discordcpp::GetChannelMessagesRequest request;
        request.limit = 50;
        request.before = discordcpp::Snowflake{ 800000000000000004ULL };
        const auto result = env.client.get_channel_messages(discordcpp::Snowflake{ channel_id }, request);
        check(result.has_value() && env.transport.requests()[0].target == route_target +
            "?limit=50&before=800000000000000004",
            "Get Channel Messages serializes limit then before in stable QueryParameters order");
    }
    for (const auto pair : std::array<std::pair<int, int>, 3>{
             std::pair{ 0, 1 }, std::pair{ 0, 2 }, std::pair{ 1, 2 } }) {
        Environment env;
        discordcpp::GetChannelMessagesRequest request;
        const auto cursor = discordcpp::Snowflake{ 800000000000000005ULL };
        if (pair.first == 0) request.before = cursor;
        else if (pair.first == 1) request.after = cursor;
        else request.around = cursor;
        if (pair.second == 0) request.before = cursor;
        else if (pair.second == 1) request.after = cursor;
        else request.around = cursor;
        const auto result = env.client.get_channel_messages(discordcpp::Snowflake{ channel_id }, request);
        check(result.has_error() && std::holds_alternative<discordcpp::LocalValidationError>(result.error()) &&
            std::get<discordcpp::LocalValidationError>(result.error()).issue ==
                discordcpp::LocalValidationIssue::invalid_query_parameter && env.transport.request_count() == 0,
            "Get Channel Messages rejects each pair of mutually exclusive cursors before dispatch");
    }
    {
        Environment env;
        discordcpp::GetChannelMessagesRequest request;
        request.after = discordcpp::Snowflake{ 0 };
        const auto result = env.client.get_channel_messages(discordcpp::Snowflake{ channel_id }, request);
        check(result.has_error() && std::holds_alternative<discordcpp::LocalValidationError>(result.error()) &&
            std::get<discordcpp::LocalValidationError>(result.error()).issue ==
                discordcpp::LocalValidationIssue::invalid_snowflake && env.transport.request_count() == 0,
            "Get Channel Messages rejects zero pagination Snowflake before dispatch");
    }
    {
        Environment env;
        discordcpp::GetChannelMessagesRequest request;
        const auto result = env.client.get_channel_messages(discordcpp::Snowflake{ 0 }, request);
        check(result.has_error() && std::holds_alternative<discordcpp::LocalValidationError>(result.error()) &&
            std::get<discordcpp::LocalValidationError>(result.error()).issue ==
                discordcpp::LocalValidationIssue::invalid_snowflake && env.transport.request_count() == 0,
            "Get Channel Messages rejects zero channel ID before dispatch");
    }
    for (const auto& [body, description] : std::vector<std::pair<std::string, std::string>>{
             { "{", "malformed JSON" }, { "{}", "a non-array JSON value" },
             { "[17]", "a non-object collection element" },
             { "[{\"id\":\"1\"}]", "a malformed Message element" },
             { "[" + message_json + ",{\"id\":\"1\"}]", "a valid element followed by an invalid element" } }) {
        Environment env;
        env.transport.queue_response(response(200, body));
        const auto result = env.client.get_channel_messages(discordcpp::Snowflake{ channel_id }, {});
        check(result.has_error() && (std::holds_alternative<discordcpp::JsonParseError>(result.error()) ||
            std::holds_alternative<discordcpp::ModelValidationError>(result.error())) &&
            env.transport.request_count() == 1,
            "Get Channel Messages rejects " + description + " without returning a partial collection");
    }
    {
        Environment env;
        env.transport.queue_response(response(404, R"json({"code":10003,"message":"Unknown Channel"})json"));
        const auto result = env.client.get_channel_messages(discordcpp::Snowflake{ channel_id }, {});
        check(result.has_error() && std::holds_alternative<discordcpp::DiscordApiError>(result.error()) &&
            std::get<discordcpp::DiscordApiError>(result.error()).http_status == 404,
            "Get Channel Messages preserves Discord API errors");
    }
    {
        Environment env;
        env.transport.queue_error({ discordcpp::TransportFailureCategory::io, "fixture transport failure", true });
        const auto result = env.client.get_channel_messages(discordcpp::Snowflake{ channel_id }, {});
        check(result.has_error() && std::holds_alternative<discordcpp::TransportError>(result.error()) &&
            std::get<discordcpp::TransportError>(result.error()).request_may_have_reached_server &&
            env.transport.request_count() == 1,
            "Get Channel Messages preserves transport uncertainty and does not retry");
    }
    {
        Environment env;
        env.transport.queue_response(response(429, R"json({"retry_after":1.0,"global":false})json"));
        const auto result = env.client.get_channel_messages(discordcpp::Snowflake{ channel_id }, {});
        check(result.has_error() && std::holds_alternative<discordcpp::DiscordApiError>(result.error()) &&
            std::get<discordcpp::DiscordApiError>(result.error()).http_status == 429 &&
            env.transport.request_count() == 1,
            "Get Channel Messages preserves rate-limit API errors without automatic retry");
    }
    {
        Environment env;
        auto learned = response(200, "[]");
        set_bucket_capacity(learned, "channel-messages-list-bucket");
        env.transport.queue_response(std::move(learned));
        env.transport.queue_response(response(200, "[]"));
        env.transport.queue_response(response(200, "[]"));
        discordcpp::GetChannelMessagesRequest first_request;
        first_request.limit = 1;
        check(env.client.get_channel_messages(discordcpp::Snowflake{ 111 }, first_request).has_value(),
            "Get Channel Messages learns explicit channel-major route metadata");
        discordcpp::GetChannelMessagesRequest other_channel;
        other_channel.limit = 1;
        other_channel.after = discordcpp::Snowflake{ 700 };
        check(env.client.get_channel_messages(discordcpp::Snowflake{ 222 }, other_channel).has_value() &&
            env.scheduler.wait_count() == 0,
            "Get Channel Messages keeps distinct Channel major resources separate");
        discordcpp::GetChannelMessagesRequest same_channel;
        same_channel.limit = 1;
        same_channel.before = discordcpp::Snowflake{ 701 };
        check(env.client.get_channel_messages(discordcpp::Snowflake{ 111 }, same_channel).has_value() &&
            env.scheduler.wait_count() == 1,
            "Get Channel Messages excludes query cursors from route identity and shares bucket per channel");
        check(env.transport.request_count() == 3 && env.transport.request_order() == std::vector<std::string>{
            "/api/v10/channels/111/messages?limit=1",
            "/api/v10/channels/222/messages?limit=1&after=700",
            "/api/v10/channels/111/messages?limit=1&before=701" },
            "Get Channel Messages performs exactly one ordered HTTP request per invocation");
    }
    {
        Environment env;
        env.transport.queue_response(response(200, "[]"));
        env.transport.queue_response(response(200, "[]"));
        discordcpp::GetChannelMessagesRequest before;
        before.before = discordcpp::Snowflake{ 702 };
        discordcpp::GetChannelMessagesRequest after;
        after.after = discordcpp::Snowflake{ 703 };
        check(env.client.get_channel_messages(discordcpp::Snowflake{ channel_id }, before).has_value() &&
            env.client.get_channel_messages(discordcpp::Snowflake{ channel_id }, after).has_value() &&
            env.transport.request_count() == 2 && env.transport.request_order() == std::vector<std::string>{
                route_target + "?before=702", route_target + "?after=703" },
            "Get Channel Messages leaves pagination to callers across repeated independent calls");
    }
}

void test_delete_message_endpoint() {
    constexpr std::uint64_t channel_id = 423456789012345678ULL;
    constexpr std::uint64_t message_id = 923456789012345678ULL;
    const std::string target = "/api/v10/channels/" + std::to_string(channel_id) +
        "/messages/" + std::to_string(message_id);
    {
        Environment env;
        env.transport.queue_response(response(204, ""));
        const auto result = env.client.delete_message(discordcpp::Snowflake{ channel_id },
            discordcpp::Snowflake{ message_id });
        const auto requests = env.transport.requests();
        check(result.has_value(), "Delete Message accepts documented empty HTTP 204 response");
        check(requests.size() == 1 && requests[0].method == "DELETE" &&
            requests[0].target == target && requests[0].body.empty(),
            "Delete Message sends exact API v10 path once without a request body");
        if (requests.size() == 1) {
            check(requests[0].headers.find("authorization") ==
                std::optional<std::string_view>{ "Bot endpoint-test-token" } &&
                requests[0].headers.find("user-agent").has_value() &&
                !requests[0].headers.contains("content-type"),
                "Delete Message uses Authorization and User-Agent without Content-Type");
        }
    }
    for (const auto& [status, body] : std::vector<std::pair<int, std::string>>{
             { 200, "" }, { 202, "" }, { 204, "unexpected body" } }) {
        Environment env;
        env.transport.queue_response(response(status, body));
        const auto result = env.client.delete_message(discordcpp::Snowflake{ channel_id },
            discordcpp::Snowflake{ message_id });
        check(result.has_error() && std::holds_alternative<discordcpp::TransportError>(result.error()) &&
            std::get<discordcpp::TransportError>(result.error()).category ==
                discordcpp::TransportFailureCategory::protocol && env.transport.request_count() == 1,
            "Delete Message rejects unexpected successful status/body as protocol error without replay");
    }
    {
        Environment env;
        env.transport.queue_response(response(403,
            R"json({"code":50013,"message":"Missing Permissions"})json"));
        const auto result = env.client.delete_message(discordcpp::Snowflake{ channel_id },
            discordcpp::Snowflake{ message_id });
        check(result.has_error() && std::holds_alternative<discordcpp::DiscordApiError>(result.error()) &&
            std::get<discordcpp::DiscordApiError>(result.error()).http_status == 403 &&
            env.transport.request_count() == 1,
            "Delete Message preserves Discord permission errors and dispatches once");
    }
    {
        Environment env;
        env.transport.queue_response(response(429, R"json({"retry_after":0.25,"global":false})json"));
        const auto result = env.client.delete_message(discordcpp::Snowflake{ channel_id },
            discordcpp::Snowflake{ message_id });
        check(result.has_error() && std::holds_alternative<discordcpp::DiscordApiError>(result.error()) &&
            std::get<discordcpp::DiscordApiError>(result.error()).http_status == 429 &&
            env.transport.request_count() == 1,
            "Delete Message preserves rate-limit API response without retry or replay");
    }
    {
        Environment env;
        env.transport.queue_error({ discordcpp::TransportFailureCategory::io, "fixture transport failure", true });
        const auto result = env.client.delete_message(discordcpp::Snowflake{ channel_id },
            discordcpp::Snowflake{ message_id });
        check(result.has_error() && std::holds_alternative<discordcpp::TransportError>(result.error()) &&
            std::get<discordcpp::TransportError>(result.error()).request_may_have_reached_server &&
            env.transport.request_count() == 1,
            "Delete Message preserves transport delivery uncertainty without retry");
    }
    for (const auto& [channel, message, label] : std::vector<std::tuple<std::uint64_t,
             std::uint64_t, std::string>>{
             { 0, message_id, "zero channel ID" }, { channel_id, 0, "zero message ID" } }) {
        Environment env;
        const auto result = env.client.delete_message(discordcpp::Snowflake{ channel },
            discordcpp::Snowflake{ message });
        check(result.has_error() && std::holds_alternative<discordcpp::LocalValidationError>(result.error()) &&
            env.transport.request_count() == 0,
            "Delete Message rejects " + label + " before transport dispatch");
    }
    {
        Environment env;
        auto limited = response(204, "");
        set_bucket_capacity(limited, "delete-message-channel-bucket");
        env.transport.queue_response(std::move(limited));
        env.transport.queue_response(response(204, ""));
        auto same_channel = response(204, "");
        set_bucket_capacity(same_channel, "delete-message-channel-bucket");
        env.transport.queue_response(std::move(same_channel));
        constexpr std::uint64_t first_channel = 111;
        constexpr std::uint64_t other_channel = 222;
        check(env.client.delete_message(discordcpp::Snowflake{ first_channel },
            discordcpp::Snowflake{ 333 }).has_value(), "Delete Message observes explicit channel route metadata");
        check(env.client.delete_message(discordcpp::Snowflake{ other_channel },
            discordcpp::Snowflake{ 333 }).has_value() && env.scheduler.wait_count() == 0,
            "Delete Message isolates different typed Channel major resources");
        check(env.client.delete_message(discordcpp::Snowflake{ first_channel },
            discordcpp::Snowflake{ 334 }).has_value() && env.scheduler.wait_count() == 1,
            "Delete Message normalizes message IDs under the Channel major resource");
        const auto requests = env.transport.requests();
        check(requests.size() == 3 && env.transport.request_order() == std::vector<std::string>{
            "/api/v10/channels/111/messages/333", "/api/v10/channels/222/messages/333",
            "/api/v10/channels/111/messages/334" } &&
            std::all_of(requests.begin(), requests.end(), [](const auto& request) {
                return request.method == "DELETE" && request.body.empty();
            }), "Delete Message dispatches exactly once per call using normalized route metadata");
    }
}

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

void test_guild_member_endpoint() {
    constexpr std::uint64_t guild_id = 723456789012345678ULL;
    constexpr std::uint64_t user_id = 823456789012345678ULL;
    const auto expected_target = "/api/v10/guilds/" + std::to_string(guild_id) + "/members/" + std::to_string(user_id);
    {
        Environment env;
        env.transport.queue_response(response(200, guild_member_json));
        const auto result = env.client.get_guild_member(discordcpp::Snowflake{ guild_id }, discordcpp::Snowflake{ user_id });
        check(result.has_value() && result.value().user.value().id.value() == user_id &&
            result.value().nick.is_null() && result.value().avatar.value() == "guild-avatar" &&
            result.value().roles.size() == 2 && result.value().roles[1].value() == 523456789012345680ULL &&
            result.value().joined_at.value() == "2024-01-02T03:04:05.123Z" &&
            result.value().premium_since.is_null() && !result.value().deaf && result.value().mute &&
            result.value().pending.value() == false &&
            result.value().permissions.value() == "18446744073709551616",
            "GuildMember decodes verified fields and preserves nullable/optional states");
        const auto requests = env.transport.requests();
        check(requests.size() == 1 && requests[0].method == "GET" && requests[0].target == expected_target,
            "Get Guild Member sends exact versioned GET path");
        if (requests.size() == 1) {
            check(requests[0].headers.find("authorization") == std::optional<std::string_view>{ "Bot endpoint-test-token" } &&
                requests[0].headers.find("user-agent").has_value(),
                "Get Guild Member uses authenticated User-Agent request construction");
        }
    }
    {
        Environment env;
        env.transport.queue_response(response(200,
            R"json({"roles":[],"joined_at":null,"deaf":false,"mute":false})json"));
        const auto result = env.client.get_guild_member(discordcpp::Snowflake{ guild_id }, discordcpp::Snowflake{ user_id });
        check(result.has_value() && result.value().user.is_absent() && result.value().nick.is_absent() &&
            result.value().joined_at.is_null() && result.value().pending.is_absent(),
            "GuildMember preserves absent user/optional fields and nullable required joined_at");
    }
    {
        Environment env;
        env.transport.queue_response(response(200, R"json({"roles":[],"deaf":false,"mute":false})json"));
        const auto result = env.client.get_guild_member(discordcpp::Snowflake{ guild_id }, discordcpp::Snowflake{ user_id });
        check(result.has_error() && std::holds_alternative<discordcpp::ModelValidationError>(result.error()) &&
            std::get<discordcpp::ModelValidationError>(result.error()).path == "/joined_at",
            "GuildMember rejects a missing required joined_at field");
    }
    for (const auto& [body, path] : std::vector<std::pair<std::string, std::string>>{
             { R"json({"roles":"1","joined_at":null,"deaf":false,"mute":false})json", "/roles" },
             { R"json({"roles":[],"joined_at":null,"deaf":"false","mute":false})json", "/deaf" },
             { R"json({"roles":[],"joined_at":null,"deaf":false,"mute":false,"nick":7})json", "/nick" },
             { R"json({"roles":[],"joined_at":null,"deaf":false,"mute":false,"user":{"id":"1","username":5}})json", "/user/username" },
             { R"json({"roles":[],"joined_at":null,"deaf":false,"mute":false,"permissions":123})json", "/permissions" }
         }) {
        Environment env;
        env.transport.queue_response(response(200, body));
        const auto result = env.client.get_guild_member(discordcpp::Snowflake{ guild_id }, discordcpp::Snowflake{ user_id });
        check(result.has_error() && std::holds_alternative<discordcpp::ModelValidationError>(result.error()) &&
            std::get<discordcpp::ModelValidationError>(result.error()).path == path,
            "GuildMember rejects incorrect known-field type at " + path);
    }
    {
        Environment env;
        env.transport.queue_response(response(200, "{"));
        const auto result = env.client.get_guild_member(discordcpp::Snowflake{ guild_id }, discordcpp::Snowflake{ user_id });
        check(result.has_error() && std::holds_alternative<discordcpp::JsonParseError>(result.error()),
            "Get Guild Member preserves malformed JSON errors");
    }
    {
        Environment env;
        env.transport.queue_response(response(403, R"json({"code":50001,"message":"Missing Access"})json"));
        const auto result = env.client.get_guild_member(discordcpp::Snowflake{ guild_id }, discordcpp::Snowflake{ user_id });
        check(result.has_error() && std::holds_alternative<discordcpp::DiscordApiError>(result.error()),
            "Get Guild Member preserves Discord API errors");
    }
    {
        Environment env;
        env.transport.queue_error({ discordcpp::TransportFailureCategory::connect, "fixture", false });
        const auto result = env.client.get_guild_member(discordcpp::Snowflake{ guild_id }, discordcpp::Snowflake{ user_id });
        check(result.has_error() && std::holds_alternative<discordcpp::TransportError>(result.error()),
            "Get Guild Member preserves transport errors");
    }
    {
        Environment env;
        env.transport.queue_response(response(429, R"json({"retry_after":"invalid"})json"));
        const auto result = env.client.get_guild_member(discordcpp::Snowflake{ guild_id }, discordcpp::Snowflake{ user_id });
        check(result.has_error() && std::holds_alternative<discordcpp::RateLimitMetadataError>(result.error()),
            "Get Guild Member preserves rate-limit metadata failures");
    }
    for (const auto [guild, user] : std::vector<std::pair<std::uint64_t, std::uint64_t>>{
             { 0, user_id }, { guild_id, 0 } }) {
        Environment env;
        const auto result = env.client.get_guild_member(discordcpp::Snowflake{ guild }, discordcpp::Snowflake{ user });
        check(result.has_error() && std::holds_alternative<discordcpp::LocalValidationError>(result.error()) &&
            env.transport.request_count() == 0,
            "Get Guild Member rejects zero IDs before dispatch");
    }
    {
        Environment env;
        auto limited = response(200, guild_member_json);
        set_bucket_capacity(limited, "member-route-guild-major");
        env.transport.queue_response(std::move(limited));
        env.transport.queue_response(response(200, guild_member_json));
        auto same_guild = response(200, guild_member_json);
        set_bucket_capacity(same_guild, "member-route-guild-major");
        env.transport.queue_response(std::move(same_guild));
        check(env.client.get_guild_member(discordcpp::Snowflake{ 111 }, discordcpp::Snowflake{ 333 }).has_value(),
            "Get Guild Member dispatches with explicit guild major resource");
        check(env.client.get_guild_member(discordcpp::Snowflake{ 222 }, discordcpp::Snowflake{ 333 }).has_value() &&
            env.scheduler.wait_count() == 0,
            "Get Guild Member bucket does not mix different guild major resources");
        check(env.client.get_guild_member(discordcpp::Snowflake{ 111 }, discordcpp::Snowflake{ 334 }).has_value() &&
            env.scheduler.wait_count() == 1,
            "Get Guild Member route normalizes user ID and keys its bucket by Guild major resource");
        check(env.transport.requests()[0].target.find("/members/333") != std::string::npos &&
            env.transport.requests()[2].target.find("/members/334") != std::string::npos,
            "Get Guild Member route uses user ID as ordinary route parameter");
    }
}

void test_guild_role_endpoint() {
    constexpr std::uint64_t guild_id = 723456789012345678ULL;
    constexpr std::uint64_t role_id = 523456789012345679ULL;
    const auto expected_target = "/api/v10/guilds/" + std::to_string(guild_id) + "/roles/" + std::to_string(role_id);
    {
        Environment env;
        env.transport.queue_response(response(200, guild_role_json));
        const auto result = env.client.get_guild_role(discordcpp::Snowflake{ guild_id }, discordcpp::Snowflake{ role_id });
        check(result.has_value() && result.value().id.value() == role_id && result.value().name == "moderator" &&
            result.value().permissions == "18446744073709551616" && result.value().icon.is_null() &&
            result.value().unicode_emoji.value() == "🛡️",
            "Get Guild Role decodes the existing partial Role model");
        const auto requests = env.transport.requests();
        check(requests.size() == 1 && requests[0].method == "GET" && requests[0].target == expected_target,
            "Get Guild Role sends exact versioned GET path");
        if (requests.size() == 1) {
            check(requests[0].headers.find("authorization") == std::optional<std::string_view>{ "Bot endpoint-test-token" } &&
                requests[0].headers.find("user-agent").has_value(),
                "Get Guild Role uses authenticated User-Agent request construction");
        }
    }
    {
        Environment env;
        env.transport.queue_response(response(200, R"json({"id":"1","name":"role"})json"));
        const auto result = env.client.get_guild_role(discordcpp::Snowflake{ guild_id }, discordcpp::Snowflake{ role_id });
        check(result.has_error() && std::holds_alternative<discordcpp::ModelValidationError>(result.error()) &&
            std::get<discordcpp::ModelValidationError>(result.error()).path == "/permissions",
            "Get Guild Role rejects missing required Role fields");
    }
    {
        Environment env;
        env.transport.queue_response(response(200, "not-json"));
        const auto result = env.client.get_guild_role(discordcpp::Snowflake{ guild_id }, discordcpp::Snowflake{ role_id });
        check(result.has_error() && std::holds_alternative<discordcpp::JsonParseError>(result.error()),
            "Get Guild Role preserves malformed JSON errors");
    }
    {
        Environment env;
        env.transport.queue_response(response(404, R"json({"code":10011,"message":"Unknown Role"})json"));
        const auto result = env.client.get_guild_role(discordcpp::Snowflake{ guild_id }, discordcpp::Snowflake{ role_id });
        check(result.has_error() && std::holds_alternative<discordcpp::DiscordApiError>(result.error()),
            "Get Guild Role preserves Discord API errors");
    }
    {
        Environment env;
        env.transport.queue_error({ discordcpp::TransportFailureCategory::io, "fixture", true });
        const auto result = env.client.get_guild_role(discordcpp::Snowflake{ guild_id }, discordcpp::Snowflake{ role_id });
        check(result.has_error() && std::holds_alternative<discordcpp::TransportError>(result.error()),
            "Get Guild Role preserves transport errors");
    }
    {
        Environment env;
        env.transport.queue_response(response(429, R"json({"retry_after":"invalid"})json"));
        const auto result = env.client.get_guild_role(discordcpp::Snowflake{ guild_id }, discordcpp::Snowflake{ role_id });
        check(result.has_error() && std::holds_alternative<discordcpp::RateLimitMetadataError>(result.error()),
            "Get Guild Role preserves rate-limit metadata failures");
    }
    for (const auto [guild, role] : std::vector<std::pair<std::uint64_t, std::uint64_t>>{
             { 0, role_id }, { guild_id, 0 } }) {
        Environment env;
        const auto result = env.client.get_guild_role(discordcpp::Snowflake{ guild }, discordcpp::Snowflake{ role });
        check(result.has_error() && std::holds_alternative<discordcpp::LocalValidationError>(result.error()) &&
            env.transport.request_count() == 0,
            "Get Guild Role rejects zero IDs before dispatch");
    }
    {
        Environment env;
        auto limited = response(200, guild_role_json);
        set_bucket_capacity(limited, "role-route-guild-major");
        env.transport.queue_response(std::move(limited));
        env.transport.queue_response(response(200, guild_role_json));
        auto same_guild = response(200, guild_role_json);
        set_bucket_capacity(same_guild, "role-route-guild-major");
        env.transport.queue_response(std::move(same_guild));
        check(env.client.get_guild_role(discordcpp::Snowflake{ 111 }, discordcpp::Snowflake{ 444 }).has_value(),
            "Get Guild Role dispatches with explicit guild major resource");
        check(env.client.get_guild_role(discordcpp::Snowflake{ 222 }, discordcpp::Snowflake{ 444 }).has_value() &&
            env.scheduler.wait_count() == 0,
            "Get Guild Role bucket does not mix different guild major resources");
        check(env.client.get_guild_role(discordcpp::Snowflake{ 111 }, discordcpp::Snowflake{ 445 }).has_value() &&
            env.scheduler.wait_count() == 1,
            "Get Guild Role route normalizes role ID and keys its bucket by Guild major resource");
    }
}

template<typename Invoke>
void exercise_guild_member_role_mutation(std::string_view label, std::string_view method,
    std::string_view route, Invoke invoke) {
    constexpr std::uint64_t guild_id = 723456789012345678ULL;
    constexpr std::uint64_t user_id = 823456789012345678ULL;
    constexpr std::uint64_t role_id = 523456789012345679ULL;
    {
        Environment env;
        env.transport.queue_response(response(204, ""));
        const auto result = invoke(env.client, discordcpp::Snowflake{ guild_id },
            discordcpp::Snowflake{ user_id }, discordcpp::Snowflake{ role_id });
        check(result.has_value(), std::string{ label } + " accepts documented empty 204 response");
        const auto requests = env.transport.requests();
        const auto target = "/api/v10/guilds/" + std::to_string(guild_id) + "/members/" +
            std::to_string(user_id) + "/roles/" + std::to_string(role_id);
        check(requests.size() == 1 && requests[0].method == method && requests[0].target == target &&
            requests[0].body.empty(), std::string{ label } + " sends exact method/path without a request body");
        if (requests.size() == 1) {
            check(requests[0].headers.find("authorization") ==
                std::optional<std::string_view>{ "Bot endpoint-test-token" } &&
                requests[0].headers.find("user-agent").has_value() &&
                !requests[0].headers.contains("content-type"),
                std::string{ label } + " uses auth/User-Agent and omits Content-Type for bodyless request");
        }
    }
    for (const auto& [status, body] : std::vector<std::pair<int, std::string>>{
             { 200, "" }, { 204, "unexpected" } }) {
        Environment env;
        env.transport.queue_response(response(status, body));
        const auto result = invoke(env.client, discordcpp::Snowflake{ guild_id },
            discordcpp::Snowflake{ user_id }, discordcpp::Snowflake{ role_id });
        check(result.has_error() && std::holds_alternative<discordcpp::TransportError>(result.error()) &&
            std::get<discordcpp::TransportError>(result.error()).category == discordcpp::TransportFailureCategory::protocol,
            std::string{ label } + " rejects non-204 or non-empty success response as protocol error");
    }
    {
        Environment env;
        env.transport.queue_response(response(403, R"json({"code":50013,"message":"Missing Permissions"})json"));
        const auto result = invoke(env.client, discordcpp::Snowflake{ guild_id },
            discordcpp::Snowflake{ user_id }, discordcpp::Snowflake{ role_id });
        check(result.has_error() && std::holds_alternative<discordcpp::DiscordApiError>(result.error()) &&
            std::get<discordcpp::DiscordApiError>(result.error()).http_status == 403,
            std::string{ label } + " preserves Discord permission errors without local bypass");
    }
    {
        Environment env;
        env.transport.queue_error({ discordcpp::TransportFailureCategory::io, "fixture", true });
        const auto result = invoke(env.client, discordcpp::Snowflake{ guild_id },
            discordcpp::Snowflake{ user_id }, discordcpp::Snowflake{ role_id });
        check(result.has_error() && std::holds_alternative<discordcpp::TransportError>(result.error()),
            std::string{ label } + " preserves transport failures");
    }
    {
        Environment env;
        env.transport.queue_response(response(429, R"json({"retry_after":"malformed"})json"));
        const auto result = invoke(env.client, discordcpp::Snowflake{ guild_id },
            discordcpp::Snowflake{ user_id }, discordcpp::Snowflake{ role_id });
        check(result.has_error() && std::holds_alternative<discordcpp::RateLimitMetadataError>(result.error()),
            std::string{ label } + " preserves rate-limit metadata failures");
    }
    for (const auto [guild, user, role_id_value] : std::vector<std::array<std::uint64_t, 3>>{
             { 0, user_id, role_id }, { guild_id, 0, role_id }, { guild_id, user_id, 0 } }) {
        Environment env;
        const auto result = invoke(env.client, discordcpp::Snowflake{ guild },
            discordcpp::Snowflake{ user }, discordcpp::Snowflake{ role_id_value });
        check(result.has_error() && std::holds_alternative<discordcpp::LocalValidationError>(result.error()) &&
            env.transport.request_count() == 0,
            std::string{ label } + " rejects zero IDs before dispatch");
    }
    {
        Environment env;
        auto first = response(204, "");
        set_bucket_capacity(first, std::string{ label } + "-mutation-bucket");
        env.transport.queue_response(std::move(first));
        env.transport.queue_response(response(204, ""));
        auto same_guild = response(204, "");
        set_bucket_capacity(same_guild, std::string{ label } + "-mutation-bucket");
        env.transport.queue_response(std::move(same_guild));
        check(invoke(env.client, discordcpp::Snowflake{ 111 }, discordcpp::Snowflake{ 222 },
            discordcpp::Snowflake{ 333 }).has_value(), std::string{ label } + " observes first route bucket");
        check(invoke(env.client, discordcpp::Snowflake{ 112 }, discordcpp::Snowflake{ 222 },
            discordcpp::Snowflake{ 333 }).has_value() && env.scheduler.wait_count() == 0,
            std::string{ label } + " keeps distinct Guild major resources isolated");
        check(invoke(env.client, discordcpp::Snowflake{ 111 }, discordcpp::Snowflake{ 223 },
            discordcpp::Snowflake{ 334 }).has_value() && env.scheduler.wait_count() == 1,
            std::string{ label } + " normalizes user/role IDs under the typed Guild major resource");
        const auto requests = env.transport.requests();
        check(requests.size() == 3 && requests[0].method == method &&
            requests[0].target.find(std::string{ route }) != std::string::npos,
            std::string{ label } + " sends the route with the expected HTTP verb");
    }
}

void test_guild_member_management_endpoints() {
    exercise_guild_member_role_mutation("Add Guild Member Role", "PUT",
        "/members/", [](discordcpp::DiscordRestClient& client, discordcpp::Snowflake guild,
            discordcpp::Snowflake user, discordcpp::Snowflake role) {
            return client.add_guild_member_role(guild, user, role);
        });
    exercise_guild_member_role_mutation("Remove Guild Member Role", "DELETE",
        "/members/", [](discordcpp::DiscordRestClient& client, discordcpp::Snowflake guild,
            discordcpp::Snowflake user, discordcpp::Snowflake role) {
            return client.remove_guild_member_role(guild, user, role);
        });

    constexpr std::uint64_t guild_id = 723456789012345678ULL;
    constexpr std::uint64_t user_id = 823456789012345678ULL;
    {
        Environment env;
        env.transport.queue_response(response(200, modified_guild_member_json));
        discordcpp::GuildMemberModifyRequest changes;
        changes.nick = discordcpp::Field<std::string>::null();
        changes.roles = discordcpp::Field<std::vector<discordcpp::Snowflake>>::with_value({
            discordcpp::Snowflake{ 523456789012345679ULL }, discordcpp::Snowflake{ 523456789012345680ULL } });
        const auto result = env.client.modify_guild_member(discordcpp::Snowflake{ guild_id },
            discordcpp::Snowflake{ user_id }, changes);
        check(result.has_value() && result.value().nick.has_value() &&
            result.value().nick.value() == "renamed" && result.value().roles[0].value() == 523456789012345679ULL,
            "Modify Guild Member decodes the returned 200 GuildMember");
        const auto requests = env.transport.requests();
        const auto expected_target = "/api/v10/guilds/" + std::to_string(guild_id) + "/members/" + std::to_string(user_id);
        check(requests.size() == 1 && requests[0].method == "PATCH" && requests[0].target == expected_target,
            "Modify Guild Member sends exact PATCH path");
        if (requests.size() == 1) {
            check(requests[0].body == R"json({"nick":null,"roles":["523456789012345679","523456789012345680"]})json" &&
                requests[0].headers.find("content-type") == std::optional<std::string_view>{ "application/json" } &&
                requests[0].headers.find("authorization") == std::optional<std::string_view>{ "Bot endpoint-test-token" } &&
                requests[0].headers.find("user-agent").has_value(),
                "Modify Guild Member serializes only supplied fields with JSON Content-Type and auth headers");
        }
    }
    {
        Environment env;
        env.transport.queue_response(response(200, modified_guild_member_json));
        discordcpp::GuildMemberModifyRequest changes;
        changes.roles = discordcpp::Field<std::vector<discordcpp::Snowflake>>::null();
        const auto result = env.client.modify_guild_member(discordcpp::Snowflake{ guild_id },
            discordcpp::Snowflake{ user_id }, changes);
        const auto requests = env.transport.requests();
        check(result.has_value() && requests.size() == 1 && requests[0].body == R"json({"roles":null})json",
            "Modify Guild Member preserves explicit null and omits absent nick field");
    }
    {
        Environment env;
        env.transport.queue_response(response(200, modified_guild_member_json));
        discordcpp::GuildMemberModifyRequest changes;
        changes.nick = discordcpp::Field<std::string>::with_value("new nick");
        changes.roles = discordcpp::Field<std::vector<discordcpp::Snowflake>>::with_value({});
        const auto result = env.client.modify_guild_member(discordcpp::Snowflake{ guild_id },
            discordcpp::Snowflake{ user_id }, changes);
        const auto requests = env.transport.requests();
        check(result.has_value() && requests.size() == 1 &&
            requests[0].body == R"json({"nick":"new nick","roles":[]})json",
            "Modify Guild Member serializes concrete strings and an explicit empty roles array");
    }
    {
        Environment env;
        auto first = response(200, modified_guild_member_json);
        set_bucket_capacity(first, "modify-member-guild-major");
        env.transport.queue_response(std::move(first));
        env.transport.queue_response(response(200, modified_guild_member_json));
        auto same_guild = response(200, modified_guild_member_json);
        set_bucket_capacity(same_guild, "modify-member-guild-major");
        env.transport.queue_response(std::move(same_guild));
        discordcpp::GuildMemberModifyRequest changes;
        changes.nick = discordcpp::Field<std::string>::null();
        check(env.client.modify_guild_member(discordcpp::Snowflake{ 111 }, discordcpp::Snowflake{ 333 }, changes).has_value(),
            "Modify Guild Member observes its first explicit route bucket");
        check(env.client.modify_guild_member(discordcpp::Snowflake{ 112 }, discordcpp::Snowflake{ 333 }, changes).has_value() &&
            env.scheduler.wait_count() == 0,
            "Modify Guild Member keeps different Guild major resources isolated");
        check(env.client.modify_guild_member(discordcpp::Snowflake{ 111 }, discordcpp::Snowflake{ 334 }, changes).has_value() &&
            env.scheduler.wait_count() == 1,
            "Modify Guild Member normalizes user ID under Guild major route metadata");
    }
    {
        Environment env;
        discordcpp::GuildMemberModifyRequest changes;
        const auto result = env.client.modify_guild_member(discordcpp::Snowflake{ guild_id },
            discordcpp::Snowflake{ user_id }, changes);
        check(result.has_error() && std::holds_alternative<discordcpp::LocalValidationError>(result.error()) &&
            std::get<discordcpp::LocalValidationError>(result.error()).issue ==
                discordcpp::LocalValidationIssue::invalid_request_body && env.transport.request_count() == 0,
            "Modify Guild Member rejects an empty patch before dispatch");
    }
    {
        Environment env;
        discordcpp::GuildMemberModifyRequest changes;
        changes.roles = discordcpp::Field<std::vector<discordcpp::Snowflake>>::with_value({ discordcpp::Snowflake{ 0 } });
        const auto result = env.client.modify_guild_member(discordcpp::Snowflake{ guild_id },
            discordcpp::Snowflake{ user_id }, changes);
        check(result.has_error() && std::holds_alternative<discordcpp::LocalValidationError>(result.error()) &&
            env.transport.request_count() == 0,
            "Modify Guild Member rejects zero role IDs before dispatch");
    }
    for (const auto& [status, body] : std::vector<std::pair<int, std::string>>{
             { 204, "" }, { 202, R"json({})json" } }) {
        Environment env;
        env.transport.queue_response(response(status, body));
        discordcpp::GuildMemberModifyRequest changes;
        changes.nick = discordcpp::Field<std::string>::null();
        const auto result = env.client.modify_guild_member(discordcpp::Snowflake{ guild_id },
            discordcpp::Snowflake{ user_id }, changes);
        check(result.has_error() && std::holds_alternative<discordcpp::TransportError>(result.error()) &&
            std::get<discordcpp::TransportError>(result.error()).category == discordcpp::TransportFailureCategory::protocol,
            "Modify Guild Member rejects success statuses other than documented 200");
    }
    {
        Environment env;
        env.transport.queue_response(response(403, R"json({"code":50013,"message":"Missing Permissions"})json"));
        discordcpp::GuildMemberModifyRequest changes;
        changes.nick = discordcpp::Field<std::string>::with_value("new nick");
        const auto result = env.client.modify_guild_member(discordcpp::Snowflake{ guild_id },
            discordcpp::Snowflake{ user_id }, changes);
        check(result.has_error() && std::holds_alternative<discordcpp::DiscordApiError>(result.error()),
            "Modify Guild Member preserves Discord permission errors");
    }
    {
        Environment env;
        env.transport.queue_error({ discordcpp::TransportFailureCategory::connect, "fixture", false });
        discordcpp::GuildMemberModifyRequest changes;
        changes.nick = discordcpp::Field<std::string>::null();
        const auto result = env.client.modify_guild_member(discordcpp::Snowflake{ guild_id },
            discordcpp::Snowflake{ user_id }, changes);
        check(result.has_error() && std::holds_alternative<discordcpp::TransportError>(result.error()),
            "Modify Guild Member preserves transport failures");
    }
    {
        Environment env;
        env.transport.queue_response(response(429, R"json({"retry_after":"bad"})json"));
        discordcpp::GuildMemberModifyRequest changes;
        changes.nick = discordcpp::Field<std::string>::null();
        const auto result = env.client.modify_guild_member(discordcpp::Snowflake{ guild_id },
            discordcpp::Snowflake{ user_id }, changes);
        check(result.has_error() && std::holds_alternative<discordcpp::RateLimitMetadataError>(result.error()),
            "Modify Guild Member preserves rate-limit metadata errors");
    }
    {
        Environment env;
        env.transport.queue_response(response(200, ""));
        discordcpp::GuildMemberModifyRequest changes;
        changes.nick = discordcpp::Field<std::string>::null();
        const auto result = env.client.modify_guild_member(discordcpp::Snowflake{ guild_id },
            discordcpp::Snowflake{ user_id }, changes);
        check(result.has_error() && std::holds_alternative<discordcpp::JsonParseError>(result.error()),
            "Modify Guild Member does not decode an empty 200 body as an empty success");
    }
    for (const auto [guild, user] : std::vector<std::pair<std::uint64_t, std::uint64_t>>{
             { 0, user_id }, { guild_id, 0 } }) {
        Environment env;
        discordcpp::GuildMemberModifyRequest changes;
        changes.nick = discordcpp::Field<std::string>::null();
        const auto result = env.client.modify_guild_member(discordcpp::Snowflake{ guild },
            discordcpp::Snowflake{ user }, changes);
        check(result.has_error() && std::holds_alternative<discordcpp::LocalValidationError>(result.error()) &&
            env.transport.request_count() == 0,
            "Modify Guild Member rejects zero IDs before dispatch");
    }
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
    test_guild_member_endpoint();
    test_guild_role_endpoint();
    test_guild_member_management_endpoints();
    test_create_message_endpoint();
    test_get_message_endpoint();
    test_delete_message_endpoint();
    if (failures == 0) {
        std::cout << "Passed " << checks << " Discord REST endpoint checks.\n";
        return 0;
    }
    std::cerr << failures << " of " << checks << " Discord REST endpoint checks failed.\n";
    return 1;
}
