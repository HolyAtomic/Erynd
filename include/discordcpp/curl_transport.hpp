#pragma once

#include <discordcpp/transport.hpp>

#include <chrono>
#include <cstddef>
#include <optional>
#include <string>

namespace discordcpp {

/// Configuration for the optional libcurl-backed HTTP transport.
struct CurlHttpTransportOptions {
    /// Base URL to which origin-form request targets are appended. The default
    /// targets Discord API v10; local servers can supply an http:// loopback URL.
    std::string base_url{ "https://discord.com/api/v10" };
    /// Fallback operation timeout when HttpRequest::deadline is absent. Set to
    /// nullopt to allow requests without an explicit deadline to run indefinitely.
    std::optional<std::chrono::milliseconds> default_timeout{ std::chrono::seconds{ 60 } };
    /// Connection and name-resolution timeout, further bounded by the total
    /// request timeout. Must be positive.
    std::chrono::milliseconds connect_timeout{ std::chrono::seconds{ 10 } };
    /// Optional cap for response body memory use. No cap is imposed by default.
    std::optional<std::size_t> max_response_body_bytes;
};

/// Synchronous libcurl transport, available through the optional
/// discordcpp::curl_transport target. Each send() owns a separate easy handle,
/// so concurrent calls on one instance are supported. The transport does not
/// own or log credentials and does not follow redirects or retry requests.
///
/// Cancellation is cooperative through libcurl's progress callback and may
/// not interrupt every blocking resolver/TLS backend immediately. A request
/// deadline is also passed to libcurl as a total operation timeout and checked
/// before and after the operation. TLS peer and host verification remain on.
/// libcurl global state is initialized once and retained until process exit so
/// this library does not call curl_global_cleanup() while other libcurl users
/// may still be active in the process.
/// Response size is unlimited unless max_response_body_bytes is configured.
/// Construction throws std::invalid_argument for an invalid base URL or
/// non-positive timeout. send() rejects malformed methods/targets and HEAD
/// requests with bodies as protocol errors before dispatch.
class CurlHttpTransport final : public IHttpTransport {
public:
    explicit CurlHttpTransport(CurlHttpTransportOptions options = {});
    ~CurlHttpTransport() override = default;

    CurlHttpTransport(const CurlHttpTransport&) = delete;
    CurlHttpTransport& operator=(const CurlHttpTransport&) = delete;
    CurlHttpTransport(CurlHttpTransport&&) noexcept = default;
    CurlHttpTransport& operator=(CurlHttpTransport&&) noexcept = default;

    [[nodiscard]] TransportResult send(const HttpRequest& request, std::stop_token cancellation) override;

private:
    CurlHttpTransportOptions options_;
};

} // namespace discordcpp
