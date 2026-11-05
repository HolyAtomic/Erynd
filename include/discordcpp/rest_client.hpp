#pragma once

#include <discordcpp/discord_error.hpp>
#include <discordcpp/rate_limit.hpp>
#include <discordcpp/transport.hpp>

#include <stop_token>
#include <variant>

namespace discordcpp {

/// REST failures preserve whether the result came from an HTTP/API response or
/// from the transport layer. Coordinated calls can also report invalid or
/// unavailable rate-limit metadata as a separate alternative.
using RestError = std::variant<DiscordApiError, TransportError, RateLimitMetadataError>;
using RestResult = Result<HttpResponse, RestError>;

/// Stateless REST orchestration over a caller-owned synchronous transport.
///
/// RestClient does not own the transport; the transport must outlive the client.
/// It adds no synchronization. Concurrent calls are safe only when the shared
/// transport supports concurrent send() calls. It performs no retries and does
/// not replay requests.
///
/// Error-body parsing is best-effort and extracts only top-level Discord
/// `code` and `message` fields. The parser does not validate raw UTF-8 and the
/// orchestration layer imposes no response-body size limit. A concrete
/// transport should offer a configurable response-size limit; applications
/// handling untrusted responses should account for raw-body retention and
/// invalid UTF-8 in diagnostic strings. A custom clock function must be
/// non-throwing and thread-safe if execute() may be called concurrently.
class RestClient final {
public:
    using ClockNowFunction = SteadyClock::time_point (*)();

    explicit RestClient(IHttpTransport& transport, ClockNowFunction now = &SteadyClock::now) noexcept;

    /// Execute one request. Cancellation is checked before deadline expiration;
    /// if either prevents dispatch, the transport is not called. The request's
    /// absolute steady-clock deadline and stop token are forwarded unchanged.
    /// The transport must enforce them during its synchronous operation; a
    /// synchronous call cannot be forcibly interrupted if it does not cooperate.
    /// A response returned after the deadline is discarded as a timeout, with
    /// delivery marked uncertain. A response completed before the deadline is
    /// accepted even if cancellation races with its return. A transport that
    /// actually cancels must return a cancelled TransportError. Any 2xx status
    /// is success; valid 1xx statuses are protocol TransportErrors because the
    /// transport contract requires final responses only. Other valid statuses
    /// become DiscordApiErrors. Invalid status values and a valueless transport
    /// result become protocol TransportErrors.
    [[nodiscard]] RestResult execute(const HttpRequest& request, std::stop_token cancellation = {}) const;

    /// Opt-in coordinated execution. The caller must supply a route template and
    /// typed major/shared resources; no URL-based inference is performed. The
    /// coordinator observes every response before HTTP errors are converted.
    /// A 429 whose scope cannot be safely scheduled returns
    /// RateLimitMetadataError with the original API response embedded. Call
    /// authorize_single_probe() explicitly to permit one conservative probe.
    [[nodiscard]] RestResult execute(const HttpRequest& request, const RouteDescriptor& route,
        RateLimitCoordinator& coordinator, std::stop_token cancellation = {}) const;

private:
    IHttpTransport& transport_;
    ClockNowFunction now_;
};

} // namespace discordcpp
