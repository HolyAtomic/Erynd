#include <discordcpp/discord_error.hpp>
#include <discordcpp/http_types.hpp>
#include <discordcpp/result.hpp>
#include <discordcpp/rest_client.hpp>
#include <discordcpp/rate_limit.hpp>
#include "fake_transport.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <condition_variable>
#include <functional>
#include <latch>
#include <mutex>
#include <stdexcept>
#include <stop_token>
#include <system_error>
#include <variant>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <thread>

namespace {

int checks{};
int failures{};
discordcpp::SteadyClock::time_point controlled_time{};

class ManualRateLimitScheduler final : public discordcpp::IRateLimitScheduler {
public:
    explicit ManualRateLimitScheduler(discordcpp::SteadyClock::time_point start = {}) : now_{ start } {}

    discordcpp::SteadyClock::time_point now() const noexcept override {
        std::stop_source* stop_source{};
        discordcpp::SteadyClock::time_point current{};
        bool changed{};
        {
            std::lock_guard lock{ mutex_ };
            if (hook_calls_remaining_ != 0) --hook_calls_remaining_;
            if (hook_calls_remaining_ == 0 && advance_on_next_now_) {
                now_ += *advance_on_next_now_;
                advance_on_next_now_.reset();
                ++generation_;
                changed = true;
            }
            stop_source = std::exchange(stop_on_next_now_, nullptr);
            current = now_;
        }
        if (changed) condition_.notify_all();
        if (stop_source) stop_source->request_stop();
        return current;
    }

    std::uint64_t generation() const noexcept override {
        std::lock_guard lock{ mutex_ };
        return generation_;
    }

    void wait_for_change(std::uint64_t observed, std::optional<discordcpp::SteadyClock::time_point> wake_at,
        std::stop_token cancellation) override {
        std::unique_lock lock{ mutex_ };
        ++waiters_;
        condition_.notify_all();
        std::stop_callback stop_callback{ cancellation, [this] { condition_.notify_all(); } };
        condition_.wait(lock, [&] {
            return generation_ != observed || cancellation.stop_requested() || (wake_at && now_ >= *wake_at);
        });
        --waiters_;
        condition_.notify_all();
    }

    void notify_all() noexcept override {
        {
            std::lock_guard lock{ mutex_ };
            ++generation_;
        }
        condition_.notify_all();
    }

    template<typename Duration>
    void advance(Duration duration) {
        {
            std::lock_guard lock{ mutex_ };
            now_ += std::chrono::duration_cast<discordcpp::SteadyClock::duration>(duration);
            ++generation_;
        }
        condition_.notify_all();
    }

    template<typename Duration>
    void advance_after_now_calls(std::size_t calls, Duration duration) {
        std::lock_guard lock{ mutex_ };
        hook_calls_remaining_ = calls;
        advance_on_next_now_ = std::chrono::duration_cast<discordcpp::SteadyClock::duration>(duration);
    }

    template<typename Duration>
    void advance_and_cancel_after_now_calls(std::size_t calls, Duration duration, std::stop_source& source) {
        std::lock_guard lock{ mutex_ };
        hook_calls_remaining_ = calls;
        advance_on_next_now_ = std::chrono::duration_cast<discordcpp::SteadyClock::duration>(duration);
        stop_on_next_now_ = &source;
    }

    void wait_until_waiting(std::size_t count = 1) {
        std::unique_lock lock{ mutex_ };
        condition_.wait(lock, [&] { return waiters_ >= count; });
    }

    bool wait_until_waiting_or_done(const std::atomic_bool& done) {
        std::unique_lock lock{ mutex_ };
        condition_.wait(lock, [&] { return waiters_ != 0 || done.load(); });
        return waiters_ != 0;
    }

    void notify_test_waiters() noexcept { condition_.notify_all(); }

private:
    mutable std::mutex mutex_;
    mutable std::condition_variable condition_;
    mutable discordcpp::SteadyClock::time_point now_;
    mutable std::optional<discordcpp::SteadyClock::duration> advance_on_next_now_;
    mutable std::stop_source* stop_on_next_now_{};
    mutable std::size_t hook_calls_remaining_{};
    mutable std::uint64_t generation_{};
    std::size_t waiters_{};
};

discordcpp::SteadyClock::time_point controlled_now() {
    return controlled_time;
}

void check(bool condition, std::string_view description) {
    ++checks;
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << description << '\n';
    }
}

template<typename Exception, typename Function>
void check_throws(Function&& function, std::string_view description) {
    bool threw_expected = false;
    try {
        std::forward<Function>(function)();
    } catch (const Exception&) {
        threw_expected = true;
    } catch (...) {
    }
    check(threw_expected, description);
}

struct ThrowingMoveOnly {
    static inline bool throw_on_move{};
    int value{};

    explicit ThrowingMoveOnly(int valueNew) : value(valueNew) {}
    ThrowingMoveOnly(const ThrowingMoveOnly&) = delete;
    ThrowingMoveOnly& operator=(const ThrowingMoveOnly&) = delete;

    ThrowingMoveOnly(ThrowingMoveOnly&& other) : value(other.value) {
        if (throw_on_move) {
            throw std::runtime_error("configured move failure");
        }
    }

    ThrowingMoveOnly& operator=(ThrowingMoveOnly&& other) noexcept {
        value = other.value;
        return *this;
    }
};

void test_result_states_and_access() {
    auto success = discordcpp::Result<std::string, int>::success("ready");
    check(success.state() == discordcpp::ResultState::success, "success state is explicit");
    check(success.has_value() && !success.has_error() && !success.is_valueless(), "success state queries agree");
    check(static_cast<bool>(success), "success converts to true");
    check(success.value() == "ready", "non-const value accessor returns value");
    success.value() += "!";
    check(success.value() == "ready!", "non-const value accessor permits mutation");
    const auto& const_success = success;
    check(const_success.value() == "ready!", "const value accessor returns value");
    check(std::move(success).value() == "ready!", "rvalue value accessor returns movable value");

    auto error = discordcpp::Result<std::string, int>::failure(17);
    check(error.state() == discordcpp::ResultState::error, "error state is explicit");
    check(!error.has_value() && error.has_error() && !error.is_valueless(), "error state queries agree");
    check(!static_cast<bool>(error), "error converts to false");
    check(error.error() == 17, "non-const error accessor returns error");
    error.error() = 23;
    check(error.error() == 23, "non-const error accessor permits mutation");
    const auto& const_error = error;
    check(const_error.error() == 23, "const error accessor returns error");
    check(std::move(error).error() == 23, "rvalue error accessor returns movable error");
}

void test_result_invalid_access_and_same_alternative_types() {
    const auto success = discordcpp::Result<int, std::string>::success(1);
    const auto error = discordcpp::Result<int, std::string>::failure("failure");
    check_throws<std::bad_variant_access>([&] { (void)success.error(); }, "error() on success throws bad_variant_access");
    check_throws<std::bad_variant_access>([&] { (void)error.value(); }, "value() on error throws bad_variant_access");

    auto same_success = discordcpp::Result<int, int>::success(41);
    auto same_error = discordcpp::Result<int, int>::failure(42);
    check(same_success.has_value() && same_success.value() == 41, "same-type alternatives select value by index");
    check(same_error.has_error() && same_error.error() == 42, "same-type alternatives select error by index");
}

void test_result_move_only_support() {
    using MoveOnlyValue = discordcpp::Result<std::unique_ptr<int>, std::string>;
    static_assert(!std::is_copy_constructible_v<MoveOnlyValue>);
    static_assert(std::is_move_constructible_v<MoveOnlyValue>);
    auto value = MoveOnlyValue::success(std::make_unique<int>(7));
    auto moved_value = std::move(value);
    check(moved_value.has_value() && *moved_value.value() == 7, "move-only value is preserved through Result move");

    using MoveOnlyError = discordcpp::Result<int, std::unique_ptr<int>>;
    static_assert(!std::is_copy_constructible_v<MoveOnlyError>);
    static_assert(std::is_move_constructible_v<MoveOnlyError>);
    auto error = MoveOnlyError::failure(std::make_unique<int>(9));
    auto moved_error = std::move(error);
    check(moved_error.has_error() && *moved_error.error() == 9, "move-only error is preserved through Result move");
}

void test_result_void_specialization() {
    auto success = discordcpp::Result<void, std::string>::success();
    check(success.state() == discordcpp::ResultState::success, "void success state is explicit");
    success.value();
    auto error = discordcpp::Result<void, std::string>::failure("failed");
    check(error.has_error() && error.error() == "failed", "void result preserves its error");
    check_throws<std::bad_variant_access>([&] { success.error(); }, "void error() on success throws bad_variant_access");
    check_throws<std::bad_variant_access>([&] { error.value(); }, "void value() on error throws bad_variant_access");
}

void test_result_valueless_assignment_and_throwing_construction() {
    using ThrowingResult = discordcpp::Result<ThrowingMoveOnly, int>;
    auto destination = ThrowingResult::failure(5);
    auto source = ThrowingResult::success(ThrowingMoveOnly{ 12 });

    ThrowingMoveOnly::throw_on_move = true;
    check_throws<std::runtime_error>([&] { destination = std::move(source); }, "throwing alternative assignment propagates exception");
    ThrowingMoveOnly::throw_on_move = false;
    check(destination.state() == discordcpp::ResultState::valueless, "throwing alternative assignment is exposed as valueless");
    check(!destination.has_value() && !destination.has_error() && destination.is_valueless(), "valueless does not imply error");
    check_throws<std::bad_variant_access>([&] { (void)destination.value(); }, "value() on valueless result throws bad_variant_access");
    check_throws<std::bad_variant_access>([&] { (void)destination.error(); }, "error() on valueless result throws bad_variant_access");

    auto recovery = ThrowingResult::success(ThrowingMoveOnly{ 18 });
    destination = std::move(recovery);
    check(destination.has_value() && destination.value().value == 18, "valueless result can recover through assignment");

    ThrowingMoveOnly::throw_on_move = true;
    check_throws<std::runtime_error>(
        [] { (void)ThrowingResult::success(ThrowingMoveOnly{ 3 }); },
        "throwing construction from success factory propagates exception");
    ThrowingMoveOnly::throw_on_move = false;
}

void test_headers_lookup_duplicates_and_set() {
    discordcpp::Headers headers{
        { "X-Trace-Id", "first" },
        { "x-trace-id", "second" },
        { "Set-Cookie", "a=1" },
        { "set-cookie", "b=2" }
    };
    check(headers.find("X-TRACE-ID") == "first", "find returns first ASCII case-insensitive match");
    const auto trace_values = headers.find_all("x-Trace-id");
    check(trace_values.size() == 2, "find_all returns all duplicate fields");
    check(trace_values.size() == 2 && trace_values[0] == "first" && trace_values[1] == "second", "duplicate lookup preserves insertion order");
    check(headers.find_all("SET-cookie").size() == 2, "Set-Cookie duplicates remain separate");

    headers.set("X-TRACE-ID", "replacement");
    check(headers.find_all("x-trace-id").size() == 1, "set removes every matching field");
    check(headers.find("X-Trace-Id") == "replacement", "set appends replacement value");
    check(headers.size() == 3, "set changes only the matching field group");

    headers.add("!#$%&'*+-.^_`|~", "valid token punctuation");
    check(headers.find("!#$%&'*+-.^_`|~") == "valid token punctuation", "HTTP token punctuation is accepted");
}

void test_header_validation_and_set_guarantee() {
    const std::array<std::string_view, 6> invalid_names{
        "", "Bad Name", "Bad:Name", "Bad\tName", "Bad\r\nName", "\xC3\x84"
    };
    for (const auto name : invalid_names) {
        check_throws<std::invalid_argument>([&] {
            discordcpp::Headers headers;
            headers.add(std::string{name}, "value");
        }, "add rejects invalid HTTP token name");
    }

    for (const std::string value : { std::string{"bad\rvalue"}, std::string{"bad\nvalue"}, std::string{"bad\r\nInjected: yes"} }) {
        check_throws<std::invalid_argument>([&] {
            discordcpp::Headers headers;
            headers.add("X-Test", value);
        }, "add rejects CR or LF in value");
    }

    discordcpp::Headers headers{ { "X-Test", "original" }, { "x-test", "duplicate" } };
    check_throws<std::invalid_argument>([&] { headers.set("Bad Name", "replacement"); }, "set rejects invalid field name");
    check_throws<std::invalid_argument>([&] { headers.set("X-Test", "bad\r\nvalue"); }, "set rejects CR or LF in value");
    const auto values_after_rejection = headers.find_all("x-test");
    check(values_after_rejection.size() == 2 && values_after_rejection[0] == "original" && values_after_rejection[1] == "duplicate",
        "invalid set leaves existing fields unchanged");
}

void test_http_types_and_deadline() {
    const auto deadline = discordcpp::SteadyClock::now() + std::chrono::seconds{ 4 };
    discordcpp::HttpRequest request{
        "POST",
        "/channels/123/messages?wait=true",
        { { "Content-Type", "application/json" }, { "X-Trace-Id", "trace-1" } },
        "{\"ok\":true}",
        deadline
    };
    check(request.method == "POST", "request stores method");
    check(request.target == "/channels/123/messages?wait=true", "request stores origin-form target");
    check(request.headers.find("content-type") == "application/json", "request header lookup ignores ASCII case");
    check(request.body == "{\"ok\":true}", "request stores body");
    check(request.deadline == deadline, "request stores steady-clock deadline");

    discordcpp::HttpRequest no_deadline{ "GET", "/channels/123", {}, {}, std::nullopt };
    check(!no_deadline.deadline.has_value(), "request can omit deadline");

    discordcpp::HttpResponse response{
        429,
        { { "Retry-After", "0.275" }, { "Set-Cookie", "a=1" }, { "set-cookie", "b=2" } },
        "{\"retry_after\":0.275}"
    };
    check(response.status == 429, "response stores status");
    check(response.headers.find("retry-after") == "0.275", "response lookup ignores ASCII case");
    check(response.body == "{\"retry_after\":0.275}", "response stores body");
    check(response.headers.find_all("set-cookie").size() == 2, "response preserves duplicate fields");
}

void test_error_models_and_diagnostics() {
    const discordcpp::DiscordApiError with_code{
        400,
        std::int64_t{ 50035 },
        "Invalid Form Body",
        "{\"code\":50035,\"message\":\"Invalid Form Body\"}"
    };
    check(with_code.http_status == 400, "Discord error preserves HTTP status");
    check(with_code.discord_code == 50035, "Discord error preserves optional code");
    check(with_code.message == "Invalid Form Body", "Discord error preserves message");
    check(with_code.raw_body.find("50035") != std::string::npos, "Discord error preserves raw body");
    const discordcpp::DiscordApiError without_code{ 502, std::nullopt, "upstream", "raw" };
    check(!without_code.discord_code.has_value(), "Discord error supports missing Discord code");

    constexpr std::array categories{
        discordcpp::TransportFailureCategory::dns,
        discordcpp::TransportFailureCategory::connect,
        discordcpp::TransportFailureCategory::tls,
        discordcpp::TransportFailureCategory::io,
        discordcpp::TransportFailureCategory::timeout,
        discordcpp::TransportFailureCategory::cancelled,
        discordcpp::TransportFailureCategory::protocol
    };
    for (const auto category : categories) {
        const discordcpp::TransportError error{ category, "transport failure", true };
        check(error.category == category, "transport error retains each failure category");
        check(error.message == "transport failure", "transport error retains diagnostic message");
        check(error.request_may_have_reached_server, "uncertain delivery is recorded conservatively");
    }

    const std::error_code native_error{ 5, std::generic_category() };
    const discordcpp::TransportError with_diagnostic{
        discordcpp::TransportFailureCategory::io, "transport failure", false, native_error
    };
    check(!with_diagnostic.request_may_have_reached_server, "transport error can record known not-sent state");
    check(with_diagnostic.diagnostic_code == native_error, "transport error preserves optional native diagnostic code");
    const discordcpp::TransportError without_diagnostic{
        discordcpp::TransportFailureCategory::timeout, "deadline expired", true
    };
    check(!without_diagnostic.diagnostic_code.has_value(), "transport diagnostic code is optional");
}

void test_rest_success_and_2xx_statuses() {
    test_support::FakeTransport transport;
    transport.queue_response({ 200, { { "X-Result", "one" } }, "ok" });
    transport.queue_response({ 204, {}, {} });
    transport.queue_response({ 205, {}, {} });
    transport.queue_response({ 299, {}, "accepted-range" });
    discordcpp::RestClient client{ transport };

    for (const int status : { 200, 204, 205, 299 }) {
        discordcpp::HttpRequest request{ "GET", "/rest/success/" + std::to_string(status), {}, {}, std::nullopt };
        auto result = client.execute(request);
        check(result.has_value(), "2xx response is REST success");
        check(result.has_value() && result.value().status == status, "successful REST result preserves response");
    }
    check(transport.request_count() == 4, "each successful call is sent once");
    const auto order = transport.request_order();
    check(order.size() == 4 && order[0] == "/rest/success/200" && order[1] == "/rest/success/204" &&
        order[2] == "/rest/success/205" && order[3] == "/rest/success/299",
        "fake transport records request order");
}

void test_rest_error_json_edge_cases() {
    test_support::FakeTransport transport;
    const std::string escaped = R"json({"code":1,"message":"quote: \" slash: \\ line:\n face:\uD83D\uDE00"})json";
    const std::string invalid_surrogate = R"json({"code":2,"message":"bad \uD800"})json";
    const std::string nested_only = R"json({"errors":{"code":3,"message":"nested"}})json";
    const std::string wrong_types = R"json({"code":"4","message":5})json";
    const std::string missing_code = R"json({"message":"message without code"})json";
    transport.queue_response({ 400, { { "X-Diagnostic", "escaped" } }, escaped });
    transport.queue_response({ 400, { { "X-Diagnostic", "surrogate" } }, invalid_surrogate });
    transport.queue_response({ 400, { { "X-Diagnostic", "nested" } }, nested_only });
    transport.queue_response({ 400, { { "X-Diagnostic", "types" } }, wrong_types });
    transport.queue_response({ 400, { { "X-Diagnostic", "missing" } }, missing_code });
    discordcpp::RestClient client{ transport };

    const auto check_api_error = [&](const std::string& target, const std::string& expected_body,
                                     const std::string& expected_message, std::optional<std::int64_t> expected_code,
                                     std::string_view expected_header) {
        auto result = client.execute({ "GET", target, {}, {}, std::nullopt });
        check(result.has_error() && std::holds_alternative<discordcpp::DiscordApiError>(result.error()), "JSON edge case remains an API error");
        if (result.has_error()) {
            const auto* error = std::get_if<discordcpp::DiscordApiError>(&result.error());
            check(error != nullptr && error->raw_body == expected_body, "JSON edge case preserves exact raw body");
            check(error != nullptr && error->message == expected_message, "JSON edge case chooses expected message");
            check(error != nullptr && error->discord_code == expected_code, "JSON edge case chooses expected code");
            check(error != nullptr && error->response_headers.find("x-diagnostic") == expected_header, "JSON edge case preserves response headers");
        }
    };
    const std::string escaped_message = std::string{"quote: \" slash: \\ line:\n face:"} + "\xF0\x9F\x98\x80";
    check_api_error("/rest/json/escaped", escaped, escaped_message, 1, "escaped");
    check_api_error("/rest/json/surrogate", invalid_surrogate, invalid_surrogate, std::nullopt, "surrogate");
    check_api_error("/rest/json/nested", nested_only, nested_only, std::nullopt, "nested");
    check_api_error("/rest/json/types", wrong_types, wrong_types, std::nullopt, "types");
    check_api_error("/rest/json/missing", missing_code, "message without code", std::nullopt, "missing");
}

void test_rest_api_errors_and_body_fallbacks() {
    test_support::FakeTransport transport;
    const std::string json_body = R"json({"errors":{"nested":[1]},"code":50035,"message":"Invalid \"Form\" \u263A"})json";
    transport.queue_response({ 400, { { "X-Request-Id", "req-1" } }, json_body });
    transport.queue_response({ 500, {}, {} });
    transport.queue_response({ 502, {}, "{malformed" });
    discordcpp::RestClient client{ transport };

    auto parsed_result = client.execute({ "POST", "/rest/error/json", {}, {}, std::nullopt });
    check(parsed_result.has_error(), "non-2xx response is an error result");
    if (parsed_result.has_error()) {
        const auto* api_error = std::get_if<discordcpp::DiscordApiError>(&parsed_result.error());
        check(api_error != nullptr, "HTTP error remains distinct as DiscordApiError");
        check(api_error != nullptr && api_error->http_status == 400, "API error preserves HTTP status");
        check(api_error != nullptr && api_error->discord_code == 50035, "API error extracts Discord code");
        const std::string decoded_message = std::string{"Invalid \"Form\" "} + "\xE2\x98\xBA";
        check(api_error != nullptr && api_error->message == decoded_message, "API error decodes escaped JSON message");
        check(api_error != nullptr && api_error->raw_body == json_body, "API error preserves exact raw body");
        check(api_error != nullptr && api_error->response_headers.find("x-request-id") == "req-1", "API error preserves response headers");
    }

    auto empty_result = client.execute({ "GET", "/rest/error/empty", {}, {}, std::nullopt });
    check(empty_result.has_error(), "empty error body remains an HTTP error");
    if (empty_result.has_error()) {
        const auto* api_error = std::get_if<discordcpp::DiscordApiError>(&empty_result.error());
        check(api_error != nullptr && api_error->http_status == 500, "empty-body error preserves status");
        check(api_error != nullptr && api_error->message == "HTTP request failed with status 500", "empty-body error has status fallback");
        check(api_error != nullptr && api_error->raw_body.empty(), "empty-body error preserves empty raw body");
    }

    auto malformed_result = client.execute({ "GET", "/rest/error/malformed", {}, {}, std::nullopt });
    check(malformed_result.has_error(), "malformed error body remains an HTTP error");
    if (malformed_result.has_error()) {
        const auto* api_error = std::get_if<discordcpp::DiscordApiError>(&malformed_result.error());
        check(api_error != nullptr && !api_error->discord_code.has_value(), "malformed body does not invent Discord code");
        check(api_error != nullptr && api_error->message == "{malformed", "malformed body is retained as fallback message");
        check(api_error != nullptr && api_error->raw_body == "{malformed", "malformed body is preserved exactly");
    }
    check(transport.request_count() == 3, "error responses are not automatically replayed");
}

void test_rest_transport_error_propagation() {
    test_support::FakeTransport transport;
    const std::error_code diagnostic{ 111, std::generic_category() };
    transport.queue_error({ discordcpp::TransportFailureCategory::connect, "connection refused", false, diagnostic });
    discordcpp::RestClient client{ transport };
    auto result = client.execute({ "GET", "/rest/transport-error", {}, {}, std::nullopt });
    check(result.has_error(), "transport failure becomes REST error");
    if (result.has_error()) {
        const auto* error = std::get_if<discordcpp::TransportError>(&result.error());
        check(error != nullptr, "transport failure remains distinct from API error");
        check(error != nullptr && error->category == discordcpp::TransportFailureCategory::connect, "transport category propagates");
        check(error != nullptr && error->message == "connection refused", "transport message propagates");
        check(error != nullptr && !error->request_may_have_reached_server, "delivery information propagates");
        check(error != nullptr && error->diagnostic_code == diagnostic, "native diagnostic code propagates");
    }
    check(transport.request_count() == 1, "transport failure is not retried");
}

void test_rest_cancellation_and_expired_deadline_preflight() {
    test_support::FakeTransport transport;
    transport.queue_response({ 200, {}, "must remain queued" });
    discordcpp::RestClient client{ transport };
    std::stop_source cancellation;
    cancellation.request_stop();
    auto cancelled = client.execute({ "GET", "/rest/cancelled", {}, {}, std::nullopt }, cancellation.get_token());
    check(cancelled.has_error(), "pre-cancelled request fails");
    if (cancelled.has_error()) {
        const auto* error = std::get_if<discordcpp::TransportError>(&cancelled.error());
        check(error != nullptr && error->category == discordcpp::TransportFailureCategory::cancelled, "pre-cancelled request reports cancelled");
        check(error != nullptr && !error->request_may_have_reached_server, "pre-cancelled request reports not sent");
    }
    check(transport.request_count() == 0, "pre-cancelled request never reaches transport");

    const auto expired = discordcpp::SteadyClock::now() - std::chrono::milliseconds{ 1 };
    auto timed_out = client.execute({ "GET", "/rest/expired", {}, {}, expired });
    check(timed_out.has_error(), "expired deadline fails before transport");
    if (timed_out.has_error()) {
        const auto* error = std::get_if<discordcpp::TransportError>(&timed_out.error());
        check(error != nullptr && error->category == discordcpp::TransportFailureCategory::timeout, "expired deadline reports timeout");
        check(error != nullptr && !error->request_may_have_reached_server, "expired request reports not sent");
    }
    check(transport.request_count() == 0, "expired request never reaches transport");
}

void test_rest_late_response_and_cancellation_race() {
    test_support::FakeTransport transport;
    const auto deadline = discordcpp::SteadyClock::time_point{} + std::chrono::seconds{ 50 };
    controlled_time = deadline - std::chrono::seconds{ 1 };
    transport.queue_response({ 200, {}, "too late" });
    transport.set_before_next_return([deadline] { controlled_time = deadline + std::chrono::seconds{ 1 }; });
    discordcpp::RestClient client{ transport, &controlled_now };
    auto late = client.execute({ "GET", "/rest/late", {}, {}, deadline });
    check(late.has_error(), "late HTTP response is discarded");
    if (late.has_error()) {
        const auto* error = std::get_if<discordcpp::TransportError>(&late.error());
        check(error != nullptr && error->category == discordcpp::TransportFailureCategory::timeout, "late response reports timeout");
        check(error != nullptr && error->request_may_have_reached_server, "late response conservatively records possible delivery");
    }
    check(transport.request_count() == 1, "late response is not replayed");

    controlled_time = deadline - std::chrono::seconds{ 1 };
    transport.queue_response({ 200, {}, "completed before deadline" });
    std::stop_source cancellation;
    transport.set_before_next_return([&cancellation] { cancellation.request_stop(); });
    auto raced = client.execute({ "GET", "/rest/cancel-race", {}, {}, deadline }, cancellation.get_token());
    check(raced.has_value() && raced.value().body == "completed before deadline", "completed response wins cancellation race before deadline");

    transport.queue_error({ discordcpp::TransportFailureCategory::cancelled, "cancelled by transport", true });
    std::stop_source cancelled_during_send;
    transport.set_before_next_return([&cancelled_during_send] { cancelled_during_send.request_stop(); });
    auto cancelled = client.execute({ "GET", "/rest/cancelled-during-send", {}, {}, deadline }, cancelled_during_send.get_token());
    check(cancelled.has_error() && std::holds_alternative<discordcpp::TransportError>(cancelled.error()), "transport cancellation remains a transport error");
    if (cancelled.has_error()) {
        const auto* error = std::get_if<discordcpp::TransportError>(&cancelled.error());
        check(error != nullptr && error->category == discordcpp::TransportFailureCategory::cancelled, "transport cancellation category propagates");
    }
    check(transport.request_count() == 3, "deadline and cancellation outcomes never retry");
}

void test_rest_deadline_propagation_and_repeated_calls() {
    test_support::FakeTransport transport;
    transport.queue_response({ 200, {}, "first" });
    transport.queue_response({ 200, {}, "second" });
    discordcpp::RestClient client{ transport };
    const auto deadline = discordcpp::SteadyClock::now() + std::chrono::minutes{ 1 };
    discordcpp::HttpRequest first{ "GET", "/rest/first", { { "X-Test", "one" } }, "body", deadline };
    discordcpp::HttpRequest second{ "POST", "/rest/second", {}, "payload", std::nullopt };

    auto first_result = client.execute(first);
    auto second_result = client.execute(second);
    check(first_result.has_value() && second_result.has_value(), "repeated calls both complete");
    const auto requests = transport.requests();
    check(requests.size() == 2, "fake transport records each request");
    check(requests.size() == 2 && requests[0].deadline == deadline, "deadline is propagated unchanged");
    check(requests.size() == 2 && requests[0].method == "GET" && requests[0].headers.find("x-test") == "one" && requests[0].body == "body",
        "request fields are propagated unchanged");
    check(requests.size() == 2 && requests[1].method == "POST" && requests[1].body == "payload" && !requests[1].deadline.has_value(),
        "later request is not contaminated by earlier request");
    const auto order = transport.request_order();
    check(order.size() == 2 && order[0] == "/rest/first" && order[1] == "/rest/second", "request order is deterministic");
}

void test_rest_unexpected_and_invalid_statuses() {
    test_support::FakeTransport transport;
    transport.queue_response({ 100, {}, {} });
    transport.queue_response({ 103, {}, {} });
    transport.queue_response({ 302, { { "Location", "/other" } }, "redirect" });
    transport.queue_response({ 304, {}, {} });
    transport.queue_response({ 99, {}, "below valid range" });
    transport.queue_response({ 600, {}, "invalid status" });
    discordcpp::RestClient client{ transport };

    for (const int status : { 100, 103 }) {
        auto result = client.execute({ "GET", "/rest/interim/" + std::to_string(status), {}, {}, std::nullopt });
        check(result.has_error() && std::holds_alternative<discordcpp::TransportError>(result.error()), "interim status is a protocol transport error");
        if (result.has_error()) {
            const auto* error = std::get_if<discordcpp::TransportError>(&result.error());
            check(error != nullptr && error->category == discordcpp::TransportFailureCategory::protocol, "interim response reports protocol category");
        }
    }
    for (const int status : { 302, 304 }) {
        auto result = client.execute({ "GET", "/rest/unexpected/" + std::to_string(status), {}, {}, std::nullopt });
        check(result.has_error(), "non-2xx status is not reported as success");
        check(result.has_error() && std::holds_alternative<discordcpp::DiscordApiError>(result.error()), "valid non-2xx status is HTTP/API error");
        check(result.has_error() && std::get<discordcpp::DiscordApiError>(result.error()).http_status == status, "unexpected status is preserved");
    }
    for (const int status : { 99, 600 }) {
        auto invalid = client.execute({ "GET", "/rest/invalid-status/" + std::to_string(status), {}, {}, std::nullopt });
        check(invalid.has_error(), "out-of-range status is rejected");
        if (invalid.has_error()) {
            const auto* error = std::get_if<discordcpp::TransportError>(&invalid.error());
            check(error != nullptr && error->category == discordcpp::TransportFailureCategory::protocol, "invalid status is protocol transport error");
            check(error != nullptr && error->request_may_have_reached_server, "invalid returned status conservatively records delivery");
        }
    }
    check(transport.request_count() == 6, "unexpected responses are not followed or replayed");
}

discordcpp::RouteDescriptor make_route(std::string method, std::string path,
    std::string major_id = {}, discordcpp::GlobalLimitClass global_class = discordcpp::GlobalLimitClass::ordinary,
    std::optional<discordcpp::SharedResourceKey> shared = std::nullopt) {
    discordcpp::RouteDescriptor route{ std::move(method), std::move(path), {}, global_class, std::move(shared) };
    if (!major_id.empty()) route.major_resources.push_back({ discordcpp::MajorResourceKind::channel, std::move(major_id), 0 });
    return route;
}

void seed_rate_limit(discordcpp::RateLimitCoordinator& coordinator, const discordcpp::RouteDescriptor& route,
    discordcpp::HttpResponse response) {
    auto acquired = coordinator.acquire(route, std::nullopt);
    check(acquired.has_value(), "seed request obtains a rate-limit permit");
    if (acquired.has_value()) {
        auto permit = std::move(acquired).value();
        permit.mark_dispatched();
        coordinator.observe_response(std::move(permit), response);
    }
}

void test_rate_limit_route_major_and_missing_headers() {
    ManualRateLimitScheduler scheduler;
    discordcpp::RateLimitCoordinator coordinator{ discordcpp::RateLimitIdentity{ 0xA11 }, scheduler };
    const auto channel_a = make_route("POST", "/channels/{channel_id}/messages/{message_id}", "111");
    const auto channel_b = make_route("POST", "/channels/{channel_id}/messages/{message_id}", "222");

    auto first = coordinator.acquire(channel_a, std::nullopt);
    auto other_major = coordinator.acquire(channel_b, std::nullopt);
    check(first.has_value() && other_major.has_value(), "typed major resources use separate provisional buckets");
    if (first.has_value()) coordinator.observe_response(std::move(first).value(), { 200, {}, "no rate headers" });
    if (other_major.has_value()) coordinator.observe_response(std::move(other_major).value(), { 204, {}, {} });

    auto repeated = coordinator.acquire(channel_a, std::nullopt);
    check(repeated.has_value(), "same explicit route template shares state across ordinary message IDs");
    if (repeated.has_value()) coordinator.observe_response(std::move(repeated).value(), { 200, {}, {} });

    auto malformed = coordinator.acquire(make_route("GET", "/invalid"), std::nullopt);
    check(malformed.has_value(), "valid route descriptor is accepted without guessed quota");
    if (malformed.has_value()) coordinator.observe_response(std::move(malformed).value(), { 200, {}, {} });
}

void test_rate_limit_fractional_user_bucket_and_wait_deadline() {
    ManualRateLimitScheduler scheduler;
    discordcpp::RateLimitCoordinator coordinator{ discordcpp::RateLimitIdentity{ 101 }, scheduler };
    const auto route = make_route("GET", "/channels/{channel_id}/messages", "555");
    seed_rate_limit(coordinator, route, { 429,
        { { "X-RateLimit-Scope", "user" }, { "X-RateLimit-Limit", "2" },
          { "X-RateLimit-Remaining", "0" }, { "X-RateLimit-Reset-After", "0.275" } },
        R"json({"retry_after":0.275,"global":false})json" });

    using PermitResult = discordcpp::Result<discordcpp::RateLimitPermit, discordcpp::TransportError>;
    std::optional<PermitResult> waited;
    const auto deadline = scheduler.now() + std::chrono::seconds{ 2 };
    std::jthread worker{ [&] { waited.emplace(coordinator.acquire(route, deadline)); } };
    scheduler.wait_until_waiting();
    scheduler.advance(std::chrono::milliseconds{ 274 });
    scheduler.wait_until_waiting();
    check(!waited.has_value(), "fractional 0.275 second user cooldown is not rounded down");
    scheduler.advance(std::chrono::milliseconds{ 1 });
    worker.join();
    check(waited.has_value() && waited->has_value(), "user bucket opens at its fractional retry_after deadline");
    if (waited && waited->has_value()) coordinator.observe_response(std::move(*waited).value(), { 200, {}, {} });

    const auto verify_unknown_cooldown = [&](const discordcpp::RouteDescriptor& blocked_route, std::string_view description) {
        const auto blocked = coordinator.acquire_detailed(blocked_route, std::nullopt);
        check(blocked.has_error() && std::holds_alternative<discordcpp::RateLimitMetadataError>(blocked.error()) &&
            std::get<discordcpp::RateLimitMetadataError>(blocked.error()).issue ==
                discordcpp::RateLimitMetadataIssue::invalid_retry_timing, description);
    };

    const auto overflow_route = make_route("GET", "/overflow");
    seed_rate_limit(coordinator, overflow_route, { 429, {}, R"json({"retry_after":1e9999})json" });
    verify_unknown_cooldown(overflow_route, "overflowing retry_after does not invent a delay or reopen a 429 scope");

    const auto malformed_route = make_route("GET", "/malformed");
    seed_rate_limit(coordinator, malformed_route, { 429,
        { { "X-RateLimit-Reset-After", "-0.25" } }, R"json({"retry_after":"0.25"})json" });
    verify_unknown_cooldown(malformed_route, "malformed and negative timing leaves the route scope safely blocked");

    const auto reset_route = make_route("GET", "/reset-header");
    seed_rate_limit(coordinator, reset_route, { 429,
        { { "X-RateLimit-Scope", "user" }, { "X-RateLimit-Limit", "1" },
          { "X-RateLimit-Remaining", "0" }, { "X-RateLimit-Reset-After", "0.125" } },
        R"json({"retry_after":"not-a-number"})json" });
    std::optional<PermitResult> reset_wait;
    std::jthread reset_worker{ [&] { reset_wait.emplace(coordinator.acquire(reset_route, scheduler.now() + std::chrono::seconds{ 1 })); } };
    scheduler.wait_until_waiting();
    scheduler.advance(std::chrono::milliseconds{ 124 });
    scheduler.wait_until_waiting();
    check(!reset_wait.has_value(), "fractional reset-after is not shortened when retry_after is malformed");
    scheduler.advance(std::chrono::milliseconds{ 1 });
    reset_worker.join();
    check(reset_wait && reset_wait->has_value(), "fractional X-RateLimit-Reset-After releases at its exact deadline");
    if (reset_wait && reset_wait->has_value()) coordinator.observe_response(std::move(*reset_wait).value(), { 200, {}, {} });
}

void test_rate_limit_global_and_shared_scopes() {
    ManualRateLimitScheduler scheduler;
    discordcpp::RateLimitCoordinator coordinator{ discordcpp::RateLimitIdentity{ 202 }, scheduler };
    const auto ordinary = make_route("GET", "/global-source");
    seed_rate_limit(coordinator, ordinary, { 429,
        { { "X-RateLimit-Scope", "global" }, { "X-RateLimit-Global", "true" } },
        R"json({"retry_after":0.5,"global":true})json" });

    const auto exempt = make_route("POST", "/interactions/{id}", {}, discordcpp::GlobalLimitClass::interaction_exempt);
    auto exempt_permit = coordinator.acquire(exempt, std::nullopt);
    check(exempt_permit.has_value(), "global cooldown does not block explicitly exempt interactions");
    if (exempt_permit.has_value()) coordinator.observe_response(std::move(exempt_permit).value(), { 204, {}, {} });

    using PermitResult = discordcpp::Result<discordcpp::RateLimitPermit, discordcpp::TransportError>;
    std::optional<PermitResult> global_wait;
    std::jthread global_worker{ [&] { global_wait.emplace(coordinator.acquire(make_route("GET", "/global-other"), scheduler.now() + std::chrono::seconds{ 2 })); } };
    scheduler.wait_until_waiting();
    scheduler.advance(std::chrono::milliseconds{ 500 });
    global_worker.join();
    check(global_wait.has_value() && global_wait->has_value(), "global cooldown blocks other ordinary routes until expiry");
    if (global_wait && global_wait->has_value()) coordinator.observe_response(std::move(*global_wait).value(), { 200, {}, {} });

    const discordcpp::SharedResourceKey resource_a{ discordcpp::SharedResourceKind::channel, "700", 0 };
    const discordcpp::SharedResourceKey resource_b{ discordcpp::SharedResourceKind::channel, "701", 0 };
    const auto shared_a = make_route("POST", "/channels/{channel_id}/messages", "700", discordcpp::GlobalLimitClass::ordinary, resource_a);
    const auto shared_b = make_route("POST", "/channels/{channel_id}/messages", "701", discordcpp::GlobalLimitClass::ordinary, resource_b);
    seed_rate_limit(coordinator, shared_a, { 429,
        { { "X-RateLimit-Scope", "shared" } }, R"json({"retry_after":0.25,"global":false})json" });

    auto unrelated_resource = coordinator.acquire(shared_b, std::nullopt);
    check(unrelated_resource.has_value(), "shared cooldown leaves other typed resources available");
    if (unrelated_resource.has_value()) coordinator.observe_response(std::move(unrelated_resource).value(), { 200, {}, {} });

    std::optional<PermitResult> shared_wait;
    std::jthread shared_worker{ [&] { shared_wait.emplace(coordinator.acquire(shared_a, scheduler.now() + std::chrono::seconds{ 2 })); } };
    scheduler.wait_until_waiting();
    scheduler.advance(std::chrono::milliseconds{ 250 });
    shared_worker.join();
    check(shared_wait.has_value() && shared_wait->has_value(), "shared cooldown blocks its explicitly keyed resource");
    if (shared_wait && shared_wait->has_value()) coordinator.observe_response(std::move(*shared_wait).value(), { 200, {}, {} });
}

void test_rate_limit_discovery_merge_stale_response_and_reservations() {
    ManualRateLimitScheduler scheduler;
    discordcpp::RateLimitCoordinator coordinator{ discordcpp::RateLimitIdentity{ 303 }, scheduler };
    const auto route_a = make_route("GET", "/channels/{channel_id}/messages/{message_id}", "900");
    const auto route_b = make_route("GET", "/channels/{channel_id}/pins", "900");
    seed_rate_limit(coordinator, route_a, { 200,
        { { "X-RateLimit-Bucket", "joined" }, { "X-RateLimit-Limit", "2" },
          { "X-RateLimit-Remaining", "1" }, { "X-RateLimit-Reset-After", "1" } }, {} });

    auto older_outstanding = coordinator.acquire(route_a, std::nullopt);
    check(older_outstanding.has_value(), "known bucket grants its remaining reserved permit");
    if (older_outstanding.has_value()) older_outstanding.value().mark_dispatched();

    using PermitResult = discordcpp::Result<discordcpp::RateLimitPermit, discordcpp::TransportError>;
    std::optional<PermitResult> second_provisional;
    std::latch second_acquired{ 1 };
    std::jthread discovery_worker{ [&] {
        second_provisional.emplace(coordinator.acquire(route_b, std::nullopt));
        second_acquired.count_down();
    } };
    second_acquired.wait();
    discovery_worker.join();
    check(second_provisional.has_value(), "unseen route receives one provisional request");
    if (second_provisional.has_value()) {
        auto permit_result = std::move(*second_provisional);
        auto permit = std::move(permit_result).value();
        permit.mark_dispatched();
        coordinator.observe_response(std::move(permit), { 200,
            { { "X-RateLimit-Bucket", "joined" }, { "X-RateLimit-Limit", "2" },
              { "X-RateLimit-Remaining", "1" }, { "X-RateLimit-Reset-After", "1" } }, {} });
    }

    if (older_outstanding.has_value()) {
        coordinator.observe_response(std::move(older_outstanding).value(), { 200,
            { { "X-RateLimit-Bucket", "joined" }, { "X-RateLimit-Limit", "2" },
              { "X-RateLimit-Remaining", "2" }, { "X-RateLimit-Reset-After", "1" } }, {} });
    }

    std::optional<PermitResult> blocked;
    std::jthread worker{ [&] { blocked.emplace(coordinator.acquire(route_a, scheduler.now() + std::chrono::seconds{ 2 })); } };
    scheduler.wait_until_waiting();
    scheduler.advance(std::chrono::milliseconds{ 999 });
    scheduler.wait_until_waiting();
    check(!blocked.has_value(), "merged provisional reservations and stale response cannot loosen remaining quota");
    scheduler.advance(std::chrono::milliseconds{ 1 });
    worker.join();
    check(blocked.has_value() && blocked->has_value(), "merged bucket replenishes only at the shared reset deadline");
    if (blocked && blocked->has_value()) coordinator.observe_response(std::move(*blocked).value(), { 200, {}, {} });
}

void test_rate_limit_concurrent_capacity_reservation() {
    ManualRateLimitScheduler scheduler;
    discordcpp::RateLimitCoordinator coordinator{ discordcpp::RateLimitIdentity{ 350 }, scheduler };
    const auto route = make_route("POST", "/channels/{channel_id}/messages", "3500");
    seed_rate_limit(coordinator, route, { 200,
        { { "X-RateLimit-Bucket", "capacity" }, { "X-RateLimit-Limit", "1" },
          { "X-RateLimit-Remaining", "1" }, { "X-RateLimit-Reset-After", "1" } }, {} });

    using PermitResult = discordcpp::Result<discordcpp::RateLimitPermit, discordcpp::TransportError>;
    std::optional<PermitResult> first;
    std::optional<PermitResult> second;
    std::latch ready{ 2 };
    std::latch start{ 1 };
    const auto deadline = scheduler.now() + std::chrono::seconds{ 2 };
    std::jthread one{ [&] { ready.count_down(); start.wait(); first.emplace(coordinator.acquire(route, deadline)); } };
    std::jthread two{ [&] { ready.count_down(); start.wait(); second.emplace(coordinator.acquire(route, deadline)); } };
    ready.wait();
    start.count_down();
    scheduler.wait_until_waiting();
    check(first.has_value() != second.has_value(), "one known bucket permit is reserved across concurrent callers");
    if (first.has_value()) first.reset();
    else second.reset(); // abandoning the undispatched permit releases capacity
    one.join();
    two.join();
    check((first && first->has_value()) || (second && second->has_value()),
        "waiting concurrent caller obtains capacity only after prior permit release");
}

void test_rate_limit_cancel_deadline_permit_and_transport_outcomes() {
    ManualRateLimitScheduler scheduler;
    discordcpp::RateLimitCoordinator coordinator{ discordcpp::RateLimitIdentity{ 404 }, scheduler };
    const auto route = make_route("POST", "/channels/{channel_id}/messages", "1000");
    seed_rate_limit(coordinator, route, { 429,
        { { "X-RateLimit-Scope", "user" }, { "X-RateLimit-Limit", "1" },
          { "X-RateLimit-Remaining", "0" }, { "X-RateLimit-Reset-After", "1" } },
        R"json({"retry_after":1})json" });

    using PermitResult = discordcpp::Result<discordcpp::RateLimitPermit, discordcpp::TransportError>;
    std::optional<PermitResult> cancelled_result;
    std::stop_source stop;
    std::jthread cancel_worker{ [&] { cancelled_result.emplace(coordinator.acquire(route, std::nullopt, stop.get_token())); } };
    scheduler.wait_until_waiting();
    stop.request_stop();
    cancel_worker.join();
    check(cancelled_result && cancelled_result->has_error() &&
        cancelled_result->error().category == discordcpp::TransportFailureCategory::cancelled,
        "stop token cancels rate-limit waiting without dispatch");

    std::optional<PermitResult> deadline_result;
    std::jthread deadline_worker{ [&] { deadline_result.emplace(coordinator.acquire(route, scheduler.now() + std::chrono::seconds{ 3 })); } };
    scheduler.wait_until_waiting();
    scheduler.advance(std::chrono::seconds{ 3 });
    deadline_worker.join();
    check(deadline_result && deadline_result->has_error() &&
        deadline_result->error().category == discordcpp::TransportFailureCategory::timeout,
        "request deadline expires while waiting for a permit");

    scheduler.advance(std::chrono::seconds{ 1 });
    auto permit = coordinator.acquire(route, std::nullopt);
    check(permit.has_value(), "permit becomes available after cooldown window");
    if (!permit.has_value()) return;
    auto abandoned = std::move(permit).value();
    abandoned = discordcpp::RateLimitPermit{}; // undispatched RAII abandonment restores reservation
    auto after_abandon = coordinator.acquire(route, std::nullopt);
    check(after_abandon.has_value(), "abandoned undispatched permit releases its reservation");
    if (after_abandon.has_value()) {
        auto dispatched = std::move(after_abandon).value();
        dispatched.mark_dispatched();
        coordinator.observe_transport_error(std::move(dispatched),
            { discordcpp::TransportFailureCategory::connect, "known not sent", false });
    }
    auto after_known_failure = coordinator.acquire(route, std::nullopt);
    check(after_known_failure.has_value(), "known-not-sent transport failure restores reserved capacity");
    if (after_known_failure.has_value()) {
        auto dispatched = std::move(after_known_failure).value();
        dispatched.mark_dispatched();
        coordinator.observe_transport_error(std::move(dispatched),
            { discordcpp::TransportFailureCategory::io, "delivery uncertain", true });
    }
    std::optional<PermitResult> uncertain_wait;
    std::stop_source cancel_uncertain;
    std::jthread uncertain_worker{ [&] { uncertain_wait.emplace(coordinator.acquire(route, std::nullopt, cancel_uncertain.get_token())); } };
    scheduler.wait_until_waiting();
    cancel_uncertain.request_stop();
    uncertain_worker.join();
    check(uncertain_wait && uncertain_wait->has_error() &&
        uncertain_wait->error().category == discordcpp::TransportFailureCategory::cancelled,
        "uncertain transport failure remains charged and prevents another permit");
}

bool is_rate_metadata_error(const discordcpp::Result<discordcpp::RateLimitPermit,
    discordcpp::RateLimitAcquireError>& result, discordcpp::RateLimitMetadataIssue issue) {
    return result.has_error() && std::holds_alternative<discordcpp::RateLimitMetadataError>(result.error()) &&
        std::get<discordcpp::RateLimitMetadataError>(result.error()).issue == issue;
}

void test_rate_limit_metadata_errors_and_explicit_recovery() {
    using namespace std::chrono_literals;
    using AcquireResult = discordcpp::Result<discordcpp::RateLimitPermit, discordcpp::RateLimitAcquireError>;
    {
        ManualRateLimitScheduler scheduler;
        discordcpp::RateLimitCoordinator coordinator{ discordcpp::RateLimitIdentity{ 610 }, scheduler };
        const auto route = make_route("GET", "/invalid-user-timing");
        auto permit = coordinator.acquire_detailed(route, std::nullopt);
        check(permit.has_value(), "invalid-timing route can be seeded");
        if (permit) {
            auto p = std::move(permit).value(); p.mark_dispatched();
            const auto metadata = coordinator.observe_response(std::move(p),
                { 429, { { "X-RateLimit-Scope", "user" } }, R"json({"retry_after":"bad"})json" });
            check(metadata && metadata->issue == discordcpp::RateLimitMetadataIssue::invalid_retry_timing,
                "429 without timing returns a distinct metadata error");
        }
        auto blocked = coordinator.acquire_detailed(route, std::nullopt);
        check(is_rate_metadata_error(blocked, discordcpp::RateLimitMetadataIssue::invalid_retry_timing),
            "invalid user timing blocks admission with a recoverable metadata error");
        auto authorized = coordinator.authorize_single_probe(route);
        check(authorized.has_value(), "caller can explicitly authorize one route probe");
        auto known_not_sent_probe = coordinator.acquire_detailed(route, std::nullopt);
        check(known_not_sent_probe.has_value(), "authorized probe is available after known metadata block");
        if (known_not_sent_probe) {
            auto p = std::move(known_not_sent_probe).value(); p.mark_dispatched();
            coordinator.observe_transport_error(std::move(p),
                { discordcpp::TransportFailureCategory::connect, "known not sent", false });
        }
        auto restored_probe = coordinator.acquire_detailed(route, std::nullopt);
        check(restored_probe.has_value(), "known-not-sent probe failure preserves its explicit authorization without refunding quota");
        if (restored_probe) {
            auto p = std::move(restored_probe).value(); p.mark_dispatched();
            coordinator.observe_transport_error(std::move(p),
                { discordcpp::TransportFailureCategory::io, "delivery uncertain", true });
        }
        auto uncertain_block = coordinator.acquire_detailed(route, std::nullopt);
        check(is_rate_metadata_error(uncertain_block, discordcpp::RateLimitMetadataIssue::invalid_retry_timing),
            "uncertain probe delivery consumes authorization and keeps the metadata quarantine");
        check(coordinator.authorize_single_probe(route).has_value(), "caller must explicitly authorize another uncertain probe");
        auto malformed_probe = coordinator.acquire_detailed(route, std::nullopt);
        check(malformed_probe.has_value(), "new explicit authorization enables one retry after uncertain delivery");
        if (malformed_probe) {
            auto p = std::move(malformed_probe).value(); p.mark_dispatched();
            const auto metadata = coordinator.observe_response(std::move(p), { 429,
                { { "X-RateLimit-Scope", "user" } }, R"json({"retry_after":"still-invalid"})json" });
            check(metadata && metadata->issue == discordcpp::RateLimitMetadataIssue::invalid_retry_timing,
                "malformed probe response returns metadata error and re-quarantines the bucket");
        }
        auto reblocked = coordinator.acquire_detailed(route, std::nullopt);
        check(is_rate_metadata_error(reblocked, discordcpp::RateLimitMetadataIssue::invalid_retry_timing),
            "malformed probe cannot reopen the metadata-blocked route");
        check(coordinator.authorize_single_probe(route).has_value(), "valid metadata recovery requires explicit probe authorization");
        auto probe = coordinator.acquire_detailed(route, std::nullopt);
        check(probe.has_value(), "one authorized route probe obtains a permit");
        if (probe) {
            auto p = std::move(probe).value(); p.mark_dispatched();
            coordinator.observe_response(std::move(p), { 200,
                { { "X-RateLimit-Limit", "2" }, { "X-RateLimit-Remaining", "1" },
                  { "X-RateLimit-Reset-After", "0.5" } }, {} });
        }
        auto recovered = coordinator.acquire_detailed(route, std::nullopt);
        check(recovered.has_value(), "valid probe metadata recovers the same coordinator");
    }

    {
        ManualRateLimitScheduler scheduler;
        discordcpp::RateLimitCoordinator coordinator{ discordcpp::RateLimitIdentity{ 611 }, scheduler };
        const auto route = make_route("GET", "/exhausted-no-reset");
        auto permit = coordinator.acquire_detailed(route, std::nullopt);
        if (permit) {
            auto p = std::move(permit).value(); p.mark_dispatched();
            coordinator.observe_response(std::move(p), { 200,
                { { "X-RateLimit-Limit", "1" }, { "X-RateLimit-Remaining", "0" } }, {} });
        }
        auto blocked = coordinator.acquire_detailed(route, std::nullopt);
        check(is_rate_metadata_error(blocked, discordcpp::RateLimitMetadataIssue::exhausted_bucket_without_reset),
            "exhausted bucket without reset returns an explicit metadata error");
        check(coordinator.authorize_single_probe(route).has_value(), "exhausted bucket allows explicit one-shot recovery");
        check(coordinator.authorize_single_probe(route).has_value(), "repeated authorization does not accumulate more than one probe");
        auto probe = coordinator.acquire_detailed(route, std::nullopt);
        check(probe.has_value(), "exhausted bucket grants only the authorized serialized probe");
        if (probe) {
            auto p = std::move(probe).value(); p.mark_dispatched();
            auto concurrent = coordinator.acquire_detailed(route, std::nullopt);
            check(is_rate_metadata_error(concurrent, discordcpp::RateLimitMetadataIssue::exhausted_bucket_without_reset),
                "an outstanding probe prevents a second admission");
            check(!coordinator.authorize_single_probe(route).has_value(),
                "an outstanding probe cannot be reauthorized concurrently");
            coordinator.observe_response(std::move(p), { 200,
                { { "X-RateLimit-Limit", "1" }, { "X-RateLimit-Remaining", "1" },
                  { "X-RateLimit-Reset-After", "1" } }, {} });
        }
        auto recovered = coordinator.acquire_detailed(route, std::nullopt);
        check(recovered.has_value(), "reset metadata recovers exhausted bucket without coordinator recreation");
    }

    {
        ManualRateLimitScheduler scheduler;
        discordcpp::RateLimitCoordinator coordinator{ discordcpp::RateLimitIdentity{ 612 }, scheduler };
        const auto route = make_route("POST", "/shared-without-key");
        auto permit = coordinator.acquire_detailed(route, std::nullopt);
        std::optional<discordcpp::RateLimitMetadataError> metadata;
        if (permit) {
            auto p = std::move(permit).value(); p.mark_dispatched();
            metadata = coordinator.observe_response(std::move(p), { 429,
                { { "X-RateLimit-Scope", "shared" } }, R"json({"retry_after":0.25})json" });
        }
        check(metadata && metadata->issue == discordcpp::RateLimitMetadataIssue::shared_resource_required,
            "shared response without key returns explicit resource metadata error");
        auto same_route = coordinator.acquire_detailed(route, std::nullopt);
        check(is_rate_metadata_error(same_route, discordcpp::RateLimitMetadataIssue::shared_resource_required),
            "missing shared key quarantines only the reporting route until metadata is corrected");
        const discordcpp::SharedResourceKey resource{ discordcpp::SharedResourceKind::channel, "same-channel", 0 };
        const auto other_route = make_route("GET", "/different-route", {}, discordcpp::GlobalLimitClass::ordinary, resource);
        auto other = coordinator.acquire_detailed(other_route, std::nullopt);
        check(other.has_value(), "a different route is not falsely treated as the same shared bucket without identity");
    }

    {
        ManualRateLimitScheduler scheduler;
        discordcpp::RateLimitCoordinator coordinator{ discordcpp::RateLimitIdentity{ 613 }, scheduler };
        test_support::FakeTransport transport;
        transport.queue_response({ 429, { { "X-RateLimit-Scope", "global" } }, R"json({"retry_after":"invalid"})json" });
        discordcpp::RestClient client{ transport };
        const auto route = make_route("GET", "/metadata-rest-error");
        auto result = client.execute({ "GET", "/metadata-rest-error", {}, {}, std::nullopt }, route, coordinator);
        check(result.has_error() && std::holds_alternative<discordcpp::RateLimitMetadataError>(result.error()),
            "coordinated REST call surfaces invalid 429 timing as a distinct metadata error");
        if (result.has_error() && std::holds_alternative<discordcpp::RateLimitMetadataError>(result.error())) {
            const auto& error = std::get<discordcpp::RateLimitMetadataError>(result.error());
            check(error.response && error.response->http_status == 429 &&
                error.response->raw_body == R"json({"retry_after":"invalid"})json" &&
                error.response->response_headers.contains("X-RateLimit-Scope"),
                "metadata error preserves the original API response diagnostics");
        }
    }
}

void test_global_shared_probe_requires_successful_final_response() {
    enum class Scope { global, shared };
    const auto run_case = [&](Scope scope, int probe_status) {
        ManualRateLimitScheduler scheduler;
        const auto identity = static_cast<std::uint64_t>(670 + static_cast<int>(scope) * 10 + probe_status);
        discordcpp::RateLimitCoordinator coordinator{ discordcpp::RateLimitIdentity{ identity }, scheduler };
        const discordcpp::SharedResourceKey resource{ discordcpp::SharedResourceKind::channel,
            "probe-resource-" + std::to_string(identity), 0 };
        const auto route = make_route("GET", "/probe-status", {}, discordcpp::GlobalLimitClass::ordinary,
            scope == Scope::shared ? std::optional{ resource } : std::nullopt);
        const char* scope_name = scope == Scope::global ? "global" : "shared";
        auto seed = coordinator.acquire_detailed(route, std::nullopt);
        check(seed.has_value(), "probe status case seeds a request");
        if (!seed) return;
        auto seed_permit = std::move(seed).value();
        seed_permit.mark_dispatched();
        coordinator.observe_response(std::move(seed_permit), { 429,
            { { "X-RateLimit-Scope", scope_name } }, R"json({"retry_after":"invalid"})json" });
        check(coordinator.authorize_single_probe(route).has_value(), "metadata quarantine authorizes a single probe");
        auto probe = coordinator.acquire_detailed(route, std::nullopt);
        check(probe.has_value(), "authorized probe obtains a permit");
        if (!probe) return;
        auto probe_permit = std::move(probe).value();
        probe_permit.mark_dispatched();
        coordinator.observe_response(std::move(probe_permit), { probe_status, {}, {} });
        auto remains_blocked = coordinator.acquire_detailed(route, std::nullopt);
        check(is_rate_metadata_error(remains_blocked, discordcpp::RateLimitMetadataIssue::invalid_retry_timing),
            "non-2xx probe response retains metadata quarantine");
    };

    for (const auto scope : { Scope::global, Scope::shared }) {
        for (const int status : { 100, 301, 403, 500 }) run_case(scope, status);
    }

    // A 2xx proves the probe was admitted even without rate-limit headers;
    // the coordinator clears quarantine but conservatively serializes requests.
    for (const auto scope : { Scope::global, Scope::shared }) {
        ManualRateLimitScheduler scheduler;
        const auto identity = static_cast<std::uint64_t>(700 + static_cast<int>(scope));
        discordcpp::RateLimitCoordinator coordinator{ discordcpp::RateLimitIdentity{ identity }, scheduler };
        const discordcpp::SharedResourceKey resource{ discordcpp::SharedResourceKind::channel,
            "success-resource-" + std::to_string(identity), 0 };
        const auto route = make_route("GET", "/probe-success", {}, discordcpp::GlobalLimitClass::ordinary,
            scope == Scope::shared ? std::optional{ resource } : std::nullopt);
        auto seed = coordinator.acquire_detailed(route, std::nullopt);
        if (seed) {
            auto p = std::move(seed).value(); p.mark_dispatched();
            coordinator.observe_response(std::move(p), { 429,
                { { "X-RateLimit-Scope", scope == Scope::global ? "global" : "shared" } }, R"json({"retry_after":"bad"})json" });
        }
        check(coordinator.authorize_single_probe(route).has_value(), "successful probe case authorizes recovery");
        auto probe = coordinator.acquire_detailed(route, std::nullopt);
        check(probe.has_value(), "successful probe case obtains permit");
        if (probe) {
            auto p = std::move(probe).value(); p.mark_dispatched();
            coordinator.observe_response(std::move(p), { 204, {}, {} });
        }
        auto recovered = coordinator.acquire_detailed(route, std::nullopt);
        check(recovered.has_value(), "2xx without rate-limit headers clears quarantine into serialized fallback");
    }
}

void test_probe_race_stale_response_and_undispatched_abandonment() {
    using AcquireResult = discordcpp::Result<discordcpp::RateLimitPermit, discordcpp::RateLimitAcquireError>;
    {
        ManualRateLimitScheduler scheduler;
        discordcpp::RateLimitCoordinator coordinator{ discordcpp::RateLimitIdentity{ 720 }, scheduler };
        const auto route = make_route("GET", "/probe-abandonment");
        auto seed = coordinator.acquire_detailed(route, std::nullopt);
        if (seed) {
            auto p = std::move(seed).value(); p.mark_dispatched();
            coordinator.observe_response(std::move(p), { 429,
                { { "X-RateLimit-Scope", "user" } }, R"json({"retry_after":"bad"})json" });
        }
        check(coordinator.authorize_single_probe(route).has_value(), "undispatched-probe case authorizes recovery");
        {
            auto abandoned = coordinator.acquire_detailed(route, std::nullopt);
            check(abandoned.has_value(), "undispatched authorized probe obtains a permit");
        }
        auto restored = coordinator.acquire_detailed(route, std::nullopt);
        check(restored.has_value(), "destroying an undispatched probe restores its authorization");
    }

    {
        ManualRateLimitScheduler scheduler;
        discordcpp::RateLimitCoordinator coordinator{ discordcpp::RateLimitIdentity{ 721 }, scheduler };
        const auto route = make_route("GET", "/probe-contenders");
        auto seed = coordinator.acquire_detailed(route, std::nullopt);
        if (seed) {
            auto p = std::move(seed).value(); p.mark_dispatched();
            coordinator.observe_response(std::move(p), { 429,
                { { "X-RateLimit-Scope", "global" } }, R"json({"retry_after":"bad"})json" });
        }
        std::latch start{ 1 };
        std::optional<AcquireResult> first;
        std::optional<AcquireResult> second;
        const auto contend = [&](std::optional<AcquireResult>& result) {
            start.wait();
            (void)coordinator.authorize_single_probe(route);
            result.emplace(coordinator.acquire_detailed(route, std::nullopt));
        };
        std::jthread one{ [&] { contend(first); } };
        std::jthread two{ [&] { contend(second); } };
        start.count_down();
        one.join();
        two.join();
        const int admitted = static_cast<int>(first && first->has_value()) + static_cast<int>(second && second->has_value());
        check(admitted == 1, "concurrent authorization/acquisition contenders admit exactly one probe");
    }

    {
        ManualRateLimitScheduler scheduler;
        discordcpp::RateLimitCoordinator coordinator{ discordcpp::RateLimitIdentity{ 722 }, scheduler };
        const auto ordinary = make_route("GET", "/stale-probe-global");
        const auto exempt = make_route("GET", "/stale-probe-exempt", {}, discordcpp::GlobalLimitClass::interaction_exempt);
        auto seed = coordinator.acquire_detailed(ordinary, std::nullopt);
        if (seed) {
            auto p = std::move(seed).value(); p.mark_dispatched();
            coordinator.observe_response(std::move(p), { 429,
                { { "X-RateLimit-Scope", "global" } }, R"json({"retry_after":"bad"})json" });
        }
        check(coordinator.authorize_single_probe(ordinary).has_value(), "stale-probe case authorizes an older probe");
        auto older_probe = coordinator.acquire_detailed(ordinary, std::nullopt);
        auto later_exempt = coordinator.acquire_detailed(exempt, std::nullopt);
        check(older_probe.has_value() && later_exempt.has_value(), "older probe and later exempt request can be outstanding");
        if (older_probe && later_exempt) {
            auto old = std::move(older_probe).value(); old.mark_dispatched();
            auto newer = std::move(later_exempt).value(); newer.mark_dispatched();
            coordinator.observe_response(std::move(newer), { 429,
                { { "X-RateLimit-Scope", "global" } }, R"json({"retry_after":"bad"})json" });
            coordinator.observe_response(std::move(old), { 204, {}, {} });
            auto blocked = coordinator.acquire_detailed(ordinary, std::nullopt);
            check(is_rate_metadata_error(blocked, discordcpp::RateLimitMetadataIssue::invalid_retry_timing),
                "older successful probe cannot clear newer global quarantine");
        }
    }
}

void test_rate_limit_stale_cooldown_ordering() {
    using namespace std::chrono_literals;
    using AcquireResult = discordcpp::Result<discordcpp::RateLimitPermit, discordcpp::RateLimitAcquireError>;
    enum class Scope { user, global, shared };
    const auto run_case = [&](Scope scope, bool newer_is_indefinite) {
        ManualRateLimitScheduler scheduler;
        const auto identity = static_cast<std::uint64_t>(620 + static_cast<int>(scope) * 2 + (newer_is_indefinite ? 1 : 0));
        discordcpp::RateLimitCoordinator coordinator{ discordcpp::RateLimitIdentity{ identity }, scheduler };
        const discordcpp::SharedResourceKey resource{ discordcpp::SharedResourceKind::channel, "shared-stale", 0 };
        const auto route = make_route("GET", "/stale-route", "620", discordcpp::GlobalLimitClass::ordinary,
            scope == Scope::shared ? std::optional{ resource } : std::nullopt);
        seed_rate_limit(coordinator, route, { 200,
            { { "X-RateLimit-Limit", "8" }, { "X-RateLimit-Remaining", "8" },
              { "X-RateLimit-Reset-After", "10" } }, {} });
        auto older = coordinator.acquire_detailed(route, std::nullopt);
        auto newer = coordinator.acquire_detailed(route, std::nullopt);
        check(older.has_value() && newer.has_value(), "two sequence-ordered requests reserve one known bucket concurrently");
        if (!older || !newer) return;
        auto old_permit = std::move(older).value(); old_permit.mark_dispatched();
        auto new_permit = std::move(newer).value(); new_permit.mark_dispatched();
        const auto scope_name = scope == Scope::user ? "user" : scope == Scope::global ? "global" : "shared";
        const auto new_response = newer_is_indefinite
            ? discordcpp::HttpResponse{ 429, { { "X-RateLimit-Scope", scope_name } }, R"json({"retry_after":"bad"})json" }
            : discordcpp::HttpResponse{ 429, { { "X-RateLimit-Scope", scope_name } }, R"json({"retry_after":0.5})json" };
        coordinator.observe_response(std::move(new_permit), new_response);
        coordinator.observe_response(std::move(old_permit), { 429,
            { { "X-RateLimit-Scope", scope_name } }, R"json({"retry_after":0.1})json" });

        if (newer_is_indefinite) {
            auto blocked = coordinator.acquire_detailed(route, std::nullopt);
            const auto issue = scope == Scope::shared ? discordcpp::RateLimitMetadataIssue::invalid_retry_timing
                : discordcpp::RateLimitMetadataIssue::invalid_retry_timing;
            check(is_rate_metadata_error(blocked, issue), "stale timed 429 cannot clear a newer indefinite metadata block");
            check(coordinator.authorize_single_probe(route).has_value(), "newer indefinite state can be explicitly probed");
            auto probe = coordinator.acquire_detailed(route, std::nullopt);
            check(probe.has_value(), "explicit probe is available after stale indefinite response");
            if (probe) {
                auto p = std::move(probe).value(); p.mark_dispatched();
                if (scope == Scope::user) coordinator.observe_response(std::move(p), { 200,
                    { { "X-RateLimit-Limit", "8" }, { "X-RateLimit-Remaining", "4" },
                      { "X-RateLimit-Reset-After", "1" } }, {} });
                else coordinator.observe_response(std::move(p), { 200, {}, {} });
            }
            auto recovered = coordinator.acquire_detailed(route, std::nullopt);
            check(recovered.has_value(), "explicit successful probe recovers the blocked metadata state");
            if (recovered && scope != Scope::user) {
                auto first_serial = std::move(recovered).value();
                std::optional<AcquireResult> second_serial;
                std::jthread serial_worker{ [&] {
                    second_serial.emplace(coordinator.acquire_detailed(route, scheduler.now() + 1s));
                } };
                scheduler.wait_until_waiting();
                check(!second_serial.has_value(), "global/shared probe recovery remains serialized without quota metadata");
                coordinator.observe_response(std::move(first_serial), { 200, {}, {} });
                serial_worker.join();
                check(second_serial && second_serial->has_value(), "global/shared serialized route progresses after in-flight completion");
            }
            return;
        }

        std::optional<AcquireResult> waited;
        std::jthread worker{ [&] { waited.emplace(coordinator.acquire_detailed(route, scheduler.now() + 2s)); } };
        scheduler.wait_until_waiting();
        scheduler.advance(499ms);
        scheduler.wait_until_waiting();
        check(!waited.has_value(), "stale timed response cannot shorten a newer cooldown");
        scheduler.advance(1ms);
        worker.join();
        check(waited && waited->has_value(), "newer timed cooldown expires at its own deadline");
    };

    for (const auto scope : { Scope::user, Scope::global, Scope::shared }) {
        run_case(scope, false);
        run_case(scope, true);
    }
}

void test_rate_limit_indefinite_merge_directions() {
    const auto run_case = [&](bool indefinite_is_canonical) {
        ManualRateLimitScheduler scheduler;
        discordcpp::RateLimitCoordinator coordinator{ discordcpp::RateLimitIdentity{ indefinite_is_canonical ? 640u : 641u }, scheduler };
        const auto canonical_route = make_route("GET", "/merge-canonical", "640");
        const auto provisional_route = make_route("GET", "/merge-provisional", "640");
        const auto hash = "same-bucket";
        if (indefinite_is_canonical) {
            seed_rate_limit(coordinator, canonical_route, { 200,
                { { "X-RateLimit-Bucket", hash }, { "X-RateLimit-Limit", "4" },
                  { "X-RateLimit-Remaining", "4" }, { "X-RateLimit-Reset-After", "10" } }, {} });
            seed_rate_limit(coordinator, canonical_route, { 429,
                { { "X-RateLimit-Bucket", hash }, { "X-RateLimit-Scope", "user" } }, R"json({"retry_after":"bad"})json" });
        } else {
            seed_rate_limit(coordinator, canonical_route, { 200,
                { { "X-RateLimit-Bucket", hash }, { "X-RateLimit-Limit", "4" },
                  { "X-RateLimit-Remaining", "4" }, { "X-RateLimit-Reset-After", "10" } }, {} });
            seed_rate_limit(coordinator, provisional_route, { 429,
                { { "X-RateLimit-Scope", "user" } }, R"json({"retry_after":"bad"})json" });
        }
        const auto blocked_route = indefinite_is_canonical ? canonical_route : provisional_route;
        check(coordinator.authorize_single_probe(blocked_route).has_value(), "blocked merge side authorizes one explicit probe");
        auto blocked_side = coordinator.acquire_detailed(blocked_route, std::nullopt);
        auto other_side = coordinator.acquire_detailed(indefinite_is_canonical ? provisional_route : canonical_route, std::nullopt);
        check(blocked_side.has_value() && other_side.has_value(), "both provisional roots hold permits before bucket merge");
        if (!blocked_side || !other_side) return;
        auto blocked_permit = std::move(blocked_side).value(); blocked_permit.mark_dispatched();
        auto other_permit = std::move(other_side).value(); other_permit.mark_dispatched();
        if (indefinite_is_canonical) {
            coordinator.observe_response(std::move(other_permit), { 200,
                { { "X-RateLimit-Bucket", hash } }, {} });
        } else {
            coordinator.observe_response(std::move(blocked_permit), { 429,
                { { "X-RateLimit-Bucket", hash }, { "X-RateLimit-Scope", "user" } }, R"json({"retry_after":"bad"})json" });
            // Keep a permit on the previously canonical root outstanding across the merge.
            auto outstanding = std::move(other_permit);
            auto blocked = coordinator.acquire_detailed(canonical_route, std::nullopt);
            check(is_rate_metadata_error(blocked, discordcpp::RateLimitMetadataIssue::invalid_retry_timing),
                "merge with blocked provisional side preserves its metadata block and outstanding reservation");
            coordinator.observe_response(std::move(outstanding), { 200, {}, {} });
            return;
        }
        auto still_outstanding = std::move(blocked_permit);
        auto after_merge = coordinator.acquire_detailed(canonical_route, std::nullopt);
        check(is_rate_metadata_error(after_merge, discordcpp::RateLimitMetadataIssue::invalid_retry_timing),
            "merge with blocked canonical side cannot admit beyond its outstanding probe");
        coordinator.observe_response(std::move(still_outstanding), { 200, {}, {} });
    };
    run_case(true);
    run_case(false);
}

void test_rate_limit_shared_scope_across_route_templates() {
    using namespace std::chrono_literals;
    using AcquireResult = discordcpp::Result<discordcpp::RateLimitPermit, discordcpp::RateLimitAcquireError>;
    ManualRateLimitScheduler scheduler;
    discordcpp::RateLimitCoordinator coordinator{ discordcpp::RateLimitIdentity{ 645 }, scheduler };
    const discordcpp::SharedResourceKey shared{ discordcpp::SharedResourceKind::channel, "6450", 0 };
    const auto route_a = make_route("POST", "/channels/{channel_id}/messages", "6450",
        discordcpp::GlobalLimitClass::ordinary, shared);
    const auto route_b = make_route("DELETE", "/channels/{channel_id}/messages/{message_id}", "6450",
        discordcpp::GlobalLimitClass::ordinary, shared);
    auto permit = coordinator.acquire_detailed(route_a, std::nullopt);
    if (permit) {
        auto p = std::move(permit).value(); p.mark_dispatched();
        coordinator.observe_response(std::move(p), { 429, { { "X-RateLimit-Scope", "shared" } },
            R"json({"retry_after":0.25})json" });
    }
    std::optional<AcquireResult> waiting;
    std::jthread worker{ [&] { waiting.emplace(coordinator.acquire_detailed(route_b, scheduler.now() + 1s)); } };
    scheduler.wait_until_waiting();
    check(!waiting.has_value(), "shared scope couples different explicit route templates for the same resource");
    scheduler.advance(250ms);
    worker.join();
    check(waiting && waiting->has_value(), "second route proceeds when its shared-resource cooldown expires");
}

void test_rate_limit_duplicate_header_policy() {
    using namespace std::chrono_literals;
    {
        ManualRateLimitScheduler scheduler;
        discordcpp::RateLimitCoordinator coordinator{ discordcpp::RateLimitIdentity{ 650 }, scheduler };
        const discordcpp::SharedResourceKey resource{ discordcpp::SharedResourceKind::channel, "dup", 0 };
        const auto route = make_route("GET", "/duplicate-identical", "650", discordcpp::GlobalLimitClass::ordinary, resource);
        auto permit = coordinator.acquire_detailed(route, std::nullopt);
        std::optional<discordcpp::RateLimitMetadataError> metadata;
        if (permit) {
            auto p = std::move(permit).value(); p.mark_dispatched();
            metadata = coordinator.observe_response(std::move(p), { 429,
                { { "X-RateLimit-Scope", "shared" }, { "x-ratelimit-scope", "shared" },
                  { "X-RateLimit-Global", "false" }, { "x-ratelimit-global", "false" },
                  { "X-RateLimit-Bucket", "dup-hash" }, { "x-ratelimit-bucket", "dup-hash" },
                  { "X-RateLimit-Reset-After", "0.5" }, { "x-ratelimit-reset-after", "0.5" },
                  { "Retry-After", "0.5" }, { "retry-after", "0.5" } }, R"json({"retry_after":0.5})json" });
        }
        check(!metadata, "byte-identical duplicate rate-limit fields are accepted consistently");
        using AcquireResult = discordcpp::Result<discordcpp::RateLimitPermit, discordcpp::RateLimitAcquireError>;
        std::optional<AcquireResult> waiting;
        std::jthread worker{ [&] { waiting.emplace(coordinator.acquire_detailed(route, scheduler.now() + 1s)); } };
        scheduler.wait_until_waiting();
        scheduler.advance(500ms);
        worker.join();
        check(waiting && waiting->has_value(), "identical duplicate shared fields establish their cooldown");
    }
    {
        ManualRateLimitScheduler scheduler;
        discordcpp::RateLimitCoordinator coordinator{ discordcpp::RateLimitIdentity{ 651 }, scheduler };
        const auto route = make_route("GET", "/duplicate-conflict");
        auto permit = coordinator.acquire_detailed(route, std::nullopt);
        std::optional<discordcpp::RateLimitMetadataError> metadata;
        if (permit) {
            auto p = std::move(permit).value(); p.mark_dispatched();
            metadata = coordinator.observe_response(std::move(p), { 429,
                { { "X-RateLimit-Scope", "global" }, { "x-ratelimit-scope", "shared" } }, R"json({"retry_after":0.5})json" });
        }
        check(metadata && metadata->issue == discordcpp::RateLimitMetadataIssue::ambiguous_scope_headers,
            "conflicting scope duplicates return an explicit ambiguous-metadata error");
        auto blocked = coordinator.acquire_detailed(route, std::nullopt);
        check(is_rate_metadata_error(blocked, discordcpp::RateLimitMetadataIssue::ambiguous_scope_headers),
            "conflicting scope duplicates quarantine the route instead of guessing global or shared");
    }
    {
        ManualRateLimitScheduler scheduler;
        discordcpp::RateLimitCoordinator coordinator{ discordcpp::RateLimitIdentity{ 653 }, scheduler };
        auto permit = coordinator.acquire_detailed(make_route("GET", "/duplicate-global-source"), std::nullopt);
        if (permit) {
            auto p = std::move(permit).value(); p.mark_dispatched();
            coordinator.observe_response(std::move(p), { 429,
                { { "X-RateLimit-Global", "true" }, { "x-ratelimit-global", "true" },
                  { "Retry-After", "0.25" }, { "retry-after", "0.25" } }, {} });
        }
        using AcquireResult = discordcpp::Result<discordcpp::RateLimitPermit, discordcpp::RateLimitAcquireError>;
        std::optional<AcquireResult> waiting;
        std::jthread worker{ [&] {
            waiting.emplace(coordinator.acquire_detailed(make_route("GET", "/duplicate-global-other"), scheduler.now() + 1s));
        } };
        scheduler.wait_until_waiting();
        scheduler.advance(250ms);
        worker.join();
        check(waiting && waiting->has_value(), "identical duplicate global and Retry-After fields apply a global cooldown");
    }
    {
        ManualRateLimitScheduler scheduler;
        discordcpp::RateLimitCoordinator coordinator{ discordcpp::RateLimitIdentity{ 654 }, scheduler };
        auto permit = coordinator.acquire_detailed(make_route("GET", "/duplicate-global-conflict"), std::nullopt);
        std::optional<discordcpp::RateLimitMetadataError> metadata;
        if (permit) {
            auto p = std::move(permit).value(); p.mark_dispatched();
            metadata = coordinator.observe_response(std::move(p), { 429,
                { { "X-RateLimit-Global", "true" }, { "x-ratelimit-global", "false" } }, {} });
        }
        check(metadata && metadata->issue == discordcpp::RateLimitMetadataIssue::ambiguous_scope_headers,
            "conflicting global flags without a confirming scope are rejected as ambiguous");
    }
    const auto bucket_duplicate_case = [&](bool identical, bool expect_shared_bucket) {
        ManualRateLimitScheduler scheduler;
        discordcpp::RateLimitCoordinator coordinator{ discordcpp::RateLimitIdentity{ identical ? 655u : 656u }, scheduler };
        const auto route_a = make_route("GET", "/duplicate-bucket-a", "6500");
        const auto route_b = make_route("GET", "/duplicate-bucket-b", "6500");
        auto permit_a = coordinator.acquire_detailed(route_a, std::nullopt);
        if (permit_a) {
            auto p = std::move(permit_a).value(); p.mark_dispatched();
            auto headers = discordcpp::Headers{
                { "X-RateLimit-Bucket", "shared-hash" },
                { "x-ratelimit-bucket", identical ? "shared-hash" : "conflicting-hash" },
                { "X-RateLimit-Limit", "1" }, { "X-RateLimit-Remaining", "0" },
                { "X-RateLimit-Reset-After", "0.5" } };
            coordinator.observe_response(std::move(p), { 200, std::move(headers), {} });
        }
        seed_rate_limit(coordinator, route_b, { 200,
            { { "X-RateLimit-Bucket", "shared-hash" }, { "X-RateLimit-Limit", "1" },
              { "X-RateLimit-Remaining", "1" }, { "X-RateLimit-Reset-After", "1" } }, {} });
        using AcquireResult = discordcpp::Result<discordcpp::RateLimitPermit, discordcpp::RateLimitAcquireError>;
        std::optional<AcquireResult> result;
        std::atomic_bool done{};
        std::jthread worker{ [&] {
            result.emplace(coordinator.acquire_detailed(route_b, scheduler.now() + 3s));
            done.store(true);
            scheduler.notify_test_waiters();
        } };
        const bool blocked = scheduler.wait_until_waiting_or_done(done);
        if (blocked) scheduler.advance(1s);
        worker.join();
        check(blocked == expect_shared_bucket, identical
            ? "identical duplicate bucket fields converge to one conservative bucket"
            : "conflicting duplicate bucket hashes are ignored instead of selecting the first");
        check(result && result->has_value(), "bucket duplicate policy eventually leaves the second route usable");
    };
    bucket_duplicate_case(true, true);
    bucket_duplicate_case(false, false);
    {
        ManualRateLimitScheduler scheduler;
        discordcpp::RateLimitCoordinator coordinator{ discordcpp::RateLimitIdentity{ 652 }, scheduler };
        const auto route = make_route("GET", "/duplicate-malformed-first");
        auto permit = coordinator.acquire_detailed(route, std::nullopt);
        std::optional<discordcpp::RateLimitMetadataError> metadata;
        if (permit) {
            auto p = std::move(permit).value(); p.mark_dispatched();
            metadata = coordinator.observe_response(std::move(p), { 429,
                { { "Retry-After", "nonsense" }, { "retry-after", "0.5" },
                  { "X-RateLimit-Reset-After", "nonsense" }, { "x-ratelimit-reset-after", "0.5" } }, {} });
        }
        check(metadata && metadata->issue == discordcpp::RateLimitMetadataIssue::invalid_retry_timing,
            "malformed-first conflicting Retry-After is not silently replaced by the later value");
    }
}

void test_rest_deadline_and_cancellation_recheck_before_dispatch() {
    using namespace std::chrono_literals;
    ManualRateLimitScheduler scheduler;
    discordcpp::RateLimitCoordinator coordinator{ discordcpp::RateLimitIdentity{ 660 }, scheduler };
    test_support::FakeTransport transport;
    discordcpp::RestClient client{ transport };
    const auto route = make_route("GET", "/pre-dispatch-race");
    const auto deadline = scheduler.now() + 1s;
    auto request = discordcpp::HttpRequest{ "GET", "/pre-dispatch-race", {}, {}, deadline };
    // The first now() is RestClient's preflight, the second is acquire(), and
    // the third is the post-permit pre-dispatch recheck.
    scheduler.advance_after_now_calls(3, 1s);
    auto result = client.execute(request, route, coordinator);
    check(result.has_error() && std::holds_alternative<discordcpp::TransportError>(result.error()) &&
        std::get<discordcpp::TransportError>(result.error()).category == discordcpp::TransportFailureCategory::timeout &&
        !std::get<discordcpp::TransportError>(result.error()).request_may_have_reached_server,
        "deadline expiring after permit acquisition returns known-not-sent timeout");
    check(transport.request_count() == 0, "deadline race before dispatch never calls transport");

    ManualRateLimitScheduler cancellation_scheduler;
    discordcpp::RateLimitCoordinator cancellation_coordinator{ discordcpp::RateLimitIdentity{ 661 }, cancellation_scheduler };
    test_support::FakeTransport cancellation_transport;
    discordcpp::RestClient cancellation_client{ cancellation_transport };
    std::stop_source stop;
    const auto cancellation_deadline = cancellation_scheduler.now() + 1s;
    cancellation_scheduler.advance_and_cancel_after_now_calls(3, 1s, stop);
    auto cancelled = cancellation_client.execute({ "GET", "/cancel-race", {}, {}, cancellation_deadline },
        make_route("GET", "/cancel-race"), cancellation_coordinator, stop.get_token());
    check(cancelled.has_error() && std::holds_alternative<discordcpp::TransportError>(cancelled.error()) &&
        std::get<discordcpp::TransportError>(cancelled.error()).category == discordcpp::TransportFailureCategory::cancelled,
        "post-permit cancellation takes precedence over simultaneous deadline expiry");
    check(cancellation_transport.request_count() == 0, "post-permit cancellation does not dispatch transport");
}

void test_rest_client_rate_limit_integration_and_legacy_path() {
    ManualRateLimitScheduler scheduler;
    discordcpp::RateLimitCoordinator coordinator{ discordcpp::RateLimitIdentity{ 505 }, scheduler };
    test_support::FakeTransport transport;
    transport.queue_response({ 429, { { "X-RateLimit-Scope", "global" } }, R"json({"retry_after":0.5,"global":true})json" });
    discordcpp::RestClient client{ transport };
    const auto route = make_route("GET", "/guilds/{guild_id}");
    auto result = client.execute({ "GET", "/guilds/1", {}, {}, std::nullopt }, route, coordinator);
    check(result.has_error() && std::holds_alternative<discordcpp::DiscordApiError>(result.error()),
        "coordinated RestClient preserves the existing HTTP/API error result");
    check(transport.request_count() == 1, "coordinated execution does not automatically replay a 429");

    transport.queue_response({ 200, {}, "legacy" });
    auto legacy = client.execute({ "GET", "/guilds/2", {}, {}, std::nullopt });
    check(legacy.has_value() && legacy.value().body == "legacy", "existing uncoordinated RestClient overload remains unchanged");
}
} // namespace

int main() {
    test_result_states_and_access();
    test_result_invalid_access_and_same_alternative_types();
    test_result_move_only_support();
    test_result_void_specialization();
    test_result_valueless_assignment_and_throwing_construction();
    test_headers_lookup_duplicates_and_set();
    test_header_validation_and_set_guarantee();
    test_http_types_and_deadline();
    test_error_models_and_diagnostics();
    test_rest_success_and_2xx_statuses();
    test_rest_api_errors_and_body_fallbacks();
    test_rest_error_json_edge_cases();
    test_rest_transport_error_propagation();
    test_rest_cancellation_and_expired_deadline_preflight();
    test_rest_late_response_and_cancellation_race();
    test_rest_deadline_propagation_and_repeated_calls();
    test_rest_unexpected_and_invalid_statuses();
    test_rate_limit_route_major_and_missing_headers();
    test_rate_limit_fractional_user_bucket_and_wait_deadline();
    test_rate_limit_global_and_shared_scopes();
    test_rate_limit_discovery_merge_stale_response_and_reservations();
    test_rate_limit_concurrent_capacity_reservation();
    test_rate_limit_cancel_deadline_permit_and_transport_outcomes();
    test_rate_limit_metadata_errors_and_explicit_recovery();
    test_global_shared_probe_requires_successful_final_response();
    test_probe_race_stale_response_and_undispatched_abandonment();
    test_rate_limit_stale_cooldown_ordering();
    test_rate_limit_indefinite_merge_directions();
    test_rate_limit_shared_scope_across_route_templates();
    test_rate_limit_duplicate_header_policy();
    test_rest_deadline_and_cancellation_recheck_before_dispatch();
    test_rest_client_rate_limit_integration_and_legacy_path();

    if (failures == 0) {
        std::cout << "Passed " << checks << " checks.\n";
        return 0;
    }
    std::cerr << failures << " of " << checks << " checks failed.\n";
    return 1;
}


