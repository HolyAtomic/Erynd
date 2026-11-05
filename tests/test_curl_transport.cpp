#include <discordcpp/curl_transport.hpp>

#include "local_http_server.hpp"

#include <chrono>
#include <climits>
#include <iostream>
#include <limits>
#include <optional>
#include <stop_token>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <type_traits>
#include <variant>

namespace {

int checks{};
int failures{};

void check(bool condition, const char* description) {
    ++checks;
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << description << '\n';
    }
}

const discordcpp::TransportError* error_of(const discordcpp::TransportResult& result) {
    if (!result.has_error() || result.state() != discordcpp::ResultState::error) return nullptr;
    return &result.error();
}

discordcpp::CurlHttpTransport make_transport(const test_support::LocalHttpServer& server) {
    discordcpp::CurlHttpTransportOptions options;
    options.base_url = server.base_url();
    options.default_timeout = std::chrono::seconds{ 5 };
    options.connect_timeout = std::chrono::seconds{ 2 };
    return discordcpp::CurlHttpTransport{ std::move(options) };
}

void test_get_headers_status_and_duplicate_response_fields() {
    test_support::LocalHttpServer server{ 200, "ok", {
        { "X-Repeat", "first" }, { "x-repeat", "second" }, { "Content-Type", "text/plain" }
    }, false, { 103 } };
    auto transport = make_transport(server);
    discordcpp::HttpRequest request{ "GET", "/channels/7?limit=2", {
        { "X-Client", "erynd-test" }, { "Accept", "application/json" }
    }, {}, std::nullopt };
    auto response = transport.send(request, {});
    check(response.has_value(), "GET transport request succeeds against local server");
    check(server.wait_for_request(), "local mock received GET request");
    const auto captured = server.request();
    check(captured.starts_with("GET /channels/7?limit=2 HTTP/1.1\r\n"), "origin-form target and method are preserved");
    check(captured.find("X-Client: erynd-test\r\n") != std::string::npos, "custom request header is preserved");
    if (response) {
        check(response.value().status == 200, "response status is captured");
        check(response.value().body == "ok", "response body is captured");
        const auto duplicates = response.value().headers.find_all("X-Repeat");
        check(duplicates.size() == 2 && duplicates[0] == "first" && duplicates[1] == "second",
            "duplicate response headers are preserved in order");
        check(response.value().headers.find("content-type") == "text/plain", "response header lookup is case-insensitive");
        check(!response.value().headers.contains("Link"), "interim informational headers do not leak into final response");
    }
}

void test_initialization_and_move_contract() {
    static_assert(!std::is_move_constructible_v<discordcpp::CurlHttpTransport>);
    static_assert(!std::is_move_assignable_v<discordcpp::CurlHttpTransport>);

    discordcpp::CurlHttpTransport transport;
    const auto before_initialization = transport.send({ "GET", "/not-dispatched", {}, {}, std::nullopt }, {});
    const auto* uninitialized_error = error_of(before_initialization);
    check(uninitialized_error && uninitialized_error->category == discordcpp::TransportFailureCategory::io &&
        !uninitialized_error->request_may_have_reached_server,
        "send before explicit libcurl initialization fails without dispatch");

    const auto capabilities = discordcpp::CurlHttpTransport::initialize();
    check(capabilities.has_value(), "explicit libcurl initialization succeeds");
    if (capabilities) {
        if (!capabilities.value().asynchronous_dns) {
            discordcpp::CurlHttpTransport timed;
            const auto rejected = timed.send({ "GET", "/dns-capability", {}, {}, std::nullopt }, {});
            const auto* error = error_of(rejected);
            check(error && error->category == discordcpp::TransportFailureCategory::io &&
                !error->request_may_have_reached_server,
                "timed requests are rejected before dispatch without async DNS");
        }
    }
}

void test_post_body_empty_body_and_redirect_policy() {
    {
        test_support::LocalHttpServer server{ 201, "created" };
        auto transport = make_transport(server);
        discordcpp::HttpRequest request{ "POST", "/messages", { { "Content-Type", "application/json" } }, "{\"x\":1}", std::nullopt };
        auto response = transport.send(request, {});
        check(response.has_value() && response.value().status == 201, "POST preserves method and final status");
        check(server.wait_for_request(), "local mock received POST request");
        const auto captured = server.request();
        check(captured.starts_with("POST /messages HTTP/1.1\r\n"), "POST method is sent");
        check(captured.ends_with("{\"x\":1}"), "POST request body is transmitted");
        check(captured.find("Content-Type: application/json\r\n") != std::string::npos, "POST content type is preserved");
    }
    {
        test_support::LocalHttpServer server{ 204, "" };
        auto transport = make_transport(server);
        auto response = transport.send({ "GET", "/empty", {}, {}, std::nullopt }, {});
        check(response.has_value() && response.value().status == 204 && response.value().body.empty(),
            "empty response body and 204 status are preserved");
    }
    {
        test_support::LocalHttpServer server{ 200, "patched" };
        auto transport = make_transport(server);
        auto response = transport.send({ "PATCH", "/messages/1", { { "Content-Type", "text/plain" } }, "edit", std::nullopt }, {});
        check(response.has_value() && server.wait_for_request(), "custom HTTP method with body succeeds");
        const auto captured = server.request();
        check(captured.starts_with("PATCH /messages/1 HTTP/1.1\r\n") && captured.ends_with("edit"),
            "custom method and body are both preserved");
    }
    {
        test_support::LocalHttpServer server{ 302, "", { { "Location", "/redirect-target" } } };
        auto transport = make_transport(server);
        auto response = transport.send({ "GET", "/redirect-source", {}, {}, std::nullopt }, {});
        check(response.has_value() && response.value().status == 302, "redirect response is returned without following it");
        check(server.wait_for_request() && server.request().find("GET /redirect-source HTTP/1.1") != std::string::npos,
            "redirect policy sends only the original request");
    }
    {
        test_support::LocalHttpServer server{ 500, "server-error" };
        auto transport = make_transport(server);
        auto response = transport.send({ "GET", "/http-error", {}, {}, std::nullopt }, {});
        check(response.has_value() && response.value().status == 500 && response.value().body == "server-error",
            "HTTP error statuses are returned as HTTP responses, not transport failures");
    }
}

void test_invalid_request_and_connection_failure_mapping() {
    test_support::LocalHttpServer server{ 200, "unused" };
    auto transport = make_transport(server);
    const auto invalid = transport.send({ "GET\r\nX-Evil: yes", "/bad", {}, {}, std::nullopt }, {});
    const auto* invalid_error = error_of(invalid);
    check(invalid_error && invalid_error->category == discordcpp::TransportFailureCategory::protocol &&
        !invalid_error->request_may_have_reached_server, "invalid method is rejected before dispatch");
    const auto invalid_target = transport.send({ "GET", "https://example.invalid/path", {}, {}, std::nullopt }, {});
    const auto* target_error = error_of(invalid_target);
    check(target_error && target_error->category == discordcpp::TransportFailureCategory::protocol &&
        !target_error->request_may_have_reached_server, "absolute URL target is rejected before dispatch");

    for (const auto& name : { "Content-Length", "transfer-encoding", "hOsT" }) {
        const discordcpp::Headers forbidden{ { name, "1" } };
        const auto result = transport.send({ "POST", "/framing", forbidden, "x", std::nullopt }, {});
        const auto* error = error_of(result);
        check(error && error->category == discordcpp::TransportFailureCategory::protocol &&
            !error->request_may_have_reached_server, "caller-supplied framing and Host headers are rejected");
    }
    {
        test_support::LocalHttpServer custom_header_server{ 200, "ok" };
        auto custom_transport = make_transport(custom_header_server);
        const auto result = custom_transport.send({ "GET", "/custom-header", { { "X-Allowed", "value" } }, {}, std::nullopt }, {});
        check(result.has_value() && custom_header_server.wait_for_request() &&
            custom_header_server.request().find("X-Allowed: value\r\n") != std::string::npos,
            "legitimate custom request headers remain supported");
    }

    discordcpp::CurlHttpTransportOptions options;
    options.base_url = "http://127.0.0.1:1";
    options.default_timeout = std::chrono::seconds{ 2 };
    discordcpp::CurlHttpTransport refused{ std::move(options) };
    const auto failure = refused.send({ "GET", "/connection-failure", {}, {}, std::nullopt }, {});
    const auto* connection_error = error_of(failure);
    check(connection_error && connection_error->category == discordcpp::TransportFailureCategory::connect &&
        !connection_error->request_may_have_reached_server, "connection refusal maps to known-not-sent connect failure");
    check(connection_error && connection_error->diagnostic_code.has_value(), "libcurl diagnostic code is preserved");
    check(connection_error && connection_error->message.find("127.0.0.1") == std::string::npos,
        "transport error message does not include request URL or credentials");

    test_support::LocalHttpServer limited_server{ 200, "long body" };
    discordcpp::CurlHttpTransportOptions limited_options;
    limited_options.base_url = limited_server.base_url();
    limited_options.max_response_body_bytes = 4;
    discordcpp::CurlHttpTransport limited{ std::move(limited_options) };
    const auto limited_result = limited.send({ "GET", "/bounded", {}, {}, std::nullopt }, {});
    const auto* limited_error = error_of(limited_result);
    check(limited_error && limited_error->category == discordcpp::TransportFailureCategory::io &&
        limited_error->request_may_have_reached_server, "configured response-size limit rejects an oversized response conservatively");
}

void test_response_header_limits_and_malformed_header_classification() {
    {
        test_support::LocalHttpServer server{ 200, "ok", { { "X-One", "1" } } };
        auto options = discordcpp::CurlHttpTransportOptions{};
        options.base_url = server.base_url();
        options.max_response_header_bytes = 20;
        discordcpp::CurlHttpTransport transport{ std::move(options) };
        const auto result = transport.send({ "GET", "/header-byte-limit", {}, {}, std::nullopt }, {});
        const auto* error = error_of(result);
        check(error && error->category == discordcpp::TransportFailureCategory::io && error->request_may_have_reached_server,
            "cumulative response header byte limit is enforced");
    }
    {
        test_support::LocalHttpServer server{ 200, "ok", { { "X-One", "1" }, { "X-Two", "2" } } };
        auto options = discordcpp::CurlHttpTransportOptions{};
        options.base_url = server.base_url();
        options.max_response_header_fields = 2;
        discordcpp::CurlHttpTransport transport{ std::move(options) };
        const auto result = transport.send({ "GET", "/header-field-count-limit", {}, {}, std::nullopt }, {});
        const auto* error = error_of(result);
        check(error && error->category == discordcpp::TransportFailureCategory::io,
            "response header field count limit counts repeated fields");
    }
    {
        test_support::LocalHttpServer server{ 200, "ok", { { "X-Large", "01234567890123456789" } } };
        auto options = discordcpp::CurlHttpTransportOptions{};
        options.base_url = server.base_url();
        options.max_response_header_field_bytes = 16;
        discordcpp::CurlHttpTransport transport{ std::move(options) };
        const auto result = transport.send({ "GET", "/header-field-size-limit", {}, {}, std::nullopt }, {});
        const auto* error = error_of(result);
        check(error && error->category == discordcpp::TransportFailureCategory::io,
            "individual response header field line limit is enforced");
    }
    {
        test_support::LocalHttpServer server{ 200, "ok", { { "Bad Name", "value" } } };
        auto options = discordcpp::CurlHttpTransportOptions{};
        options.base_url = server.base_url();
        discordcpp::CurlHttpTransport transport{ std::move(options) };
        const auto result = transport.send({ "GET", "/malformed-header", {}, {}, std::nullopt }, {});
        const auto* error = error_of(result);
        check(error && error->category == discordcpp::TransportFailureCategory::protocol && error->diagnostic_code,
            "malformed response header is classified as protocol with a diagnostic");
    }
    {
        test_support::LocalHttpServer server{ 200, "ok", { { "X-Bad-Value", std::string{ "bad\0value", 9 } } } };
        auto options = discordcpp::CurlHttpTransportOptions{};
        options.base_url = server.base_url();
        discordcpp::CurlHttpTransport transport{ std::move(options) };
        const auto result = transport.send({ "GET", "/malformed-header-value", {}, {}, std::nullopt }, {});
        const auto* error = error_of(result);
        check(error && error->category == discordcpp::TransportFailureCategory::protocol,
            "response header control bytes are rejected as malformed protocol");
    }
    {
        test_support::LocalHttpServer server{ 200, "ok", { { "X-Final", "yes" } }, false, { 103 }, 1 };
        auto options = discordcpp::CurlHttpTransportOptions{};
        options.base_url = server.base_url();
        options.max_response_header_fields = 3;
        discordcpp::CurlHttpTransport transport{ std::move(options) };
        const auto result = transport.send({ "GET", "/cumulative-interim-limit", {}, {}, std::nullopt }, {});
        const auto* error = error_of(result);
        check(error && error->category == discordcpp::TransportFailureCategory::io,
            "response header field limit applies cumulatively across interim and final blocks");
    }
}

void test_concurrent_calls_and_timeout_boundaries() {
    {
        test_support::LocalHttpServer server{ 200, "concurrent", {}, false, {}, 2 };
        auto transport = make_transport(server);
        std::optional<discordcpp::TransportResult> first;
        std::optional<discordcpp::TransportResult> second;
        std::jthread one{ [&] { first.emplace(transport.send({ "GET", "/one", {}, {}, std::nullopt }, {})); } };
        std::jthread two{ [&] { second.emplace(transport.send({ "GET", "/two", {}, {}, std::nullopt }, {})); } };
        check(server.wait_for_request(2), "one transport instance dispatches concurrent requests");
        one.join();
        two.join();
        if (first && first->has_error()) std::cerr << "concurrent first transport error: " << first->error().message << '\n';
        if (second && second->has_error()) std::cerr << "concurrent second transport error: " << second->error().message << '\n';
        check(first && first->has_value() && second && second->has_value(),
            "concurrent requests both complete successfully");
        check(server.requests().size() == 2, "local fixture received both concurrent requests");
    }
    {
        test_support::LocalHttpServer server{ 200, "far deadline" };
        auto transport = make_transport(server);
        const auto response = transport.send({ "GET", "/far-deadline", {}, {}, discordcpp::SteadyClock::time_point::max() }, {});
        check(response.has_value() && response.value().status == 200,
            "a representable far-future steady deadline is not shortened to LONG_MAX milliseconds");
    }
    if constexpr (std::numeric_limits<std::chrono::milliseconds::rep>::max() > LONG_MAX) {
        discordcpp::CurlHttpTransportOptions options;
        options.default_timeout = std::chrono::milliseconds{ static_cast<std::chrono::milliseconds::rep>(LONG_MAX) + 1 };
        bool rejected{};
        try { discordcpp::CurlHttpTransport invalid{ std::move(options) }; }
        catch (const std::invalid_argument&) { rejected = true; }
        check(rejected, "unrepresentable configured timeout is rejected instead of silently shortened");
    }
}

void test_deadline_and_cancellation() {
    using namespace std::chrono_literals;
    {
        test_support::LocalHttpServer server{ 200, "late", {}, true };
        auto transport = make_transport(server);
        const auto deadline = discordcpp::SteadyClock::now() + 250ms;
        std::optional<discordcpp::TransportResult> result;
        std::jthread request_thread{ [&] {
            result.emplace(transport.send({ "GET", "/deadline", {}, {}, deadline }, {}));
        } };
        check(server.wait_for_request(), "deadline request reaches local fixture before timeout");
        request_thread.join();
        const auto* error = result ? error_of(*result) : nullptr;
        check(error && error->category == discordcpp::TransportFailureCategory::timeout,
            "deadline expiry is reported as timeout");
        check(error && error->request_may_have_reached_server,
            "timeout after dispatch preserves delivery uncertainty");
    }
    {
        test_support::LocalHttpServer server{ 200, "cancelled", {}, true };
        auto transport = make_transport(server);
        std::stop_source stop;
        std::optional<discordcpp::TransportResult> result;
        std::jthread request_thread{ [&] {
            result.emplace(transport.send({ "GET", "/cancel", {}, {}, std::nullopt }, stop.get_token()));
        } };
        check(server.wait_for_request(), "cancellable request reaches local fixture");
        stop.request_stop();
        request_thread.join();
        const auto* error = result ? error_of(*result) : nullptr;
        check(error && error->category == discordcpp::TransportFailureCategory::cancelled,
            "cooperative in-flight cancellation maps to cancelled");
        check(error && error->request_may_have_reached_server,
            "in-flight cancellation conservatively preserves delivery uncertainty");
    }
    {
        test_support::LocalHttpServer server{ 200, "unused" };
        auto transport = make_transport(server);
        std::stop_source stop;
        stop.request_stop();
        const auto cancelled = transport.send({ "GET", "/preflight", {}, {}, std::nullopt }, stop.get_token());
        const auto* error = error_of(cancelled);
        check(error && error->category == discordcpp::TransportFailureCategory::cancelled &&
            !error->request_may_have_reached_server, "pre-dispatch cancellation is known not sent");
    }
}

} // namespace

int main() {
    test_initialization_and_move_contract();
    test_get_headers_status_and_duplicate_response_fields();
    test_post_body_empty_body_and_redirect_policy();
    test_invalid_request_and_connection_failure_mapping();
    test_response_header_limits_and_malformed_header_classification();
    test_deadline_and_cancellation();
    test_concurrent_calls_and_timeout_boundaries();
    if (failures == 0) {
        std::cout << "Passed " << checks << " curl transport checks.\n";
        return 0;
    }
    std::cerr << failures << " of " << checks << " curl transport checks failed.\n";
    return 1;
}
