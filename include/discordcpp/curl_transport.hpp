#pragma once

#include <discordcpp/transport.hpp>

#include <chrono>
#include <cstddef>
#include <optional>
#include <string>

namespace discordcpp {

/// Capabilities reported by the runtime libcurl build after initialization.
struct CurlHttpTransportCapabilities {
    /// Whether libcurl's global initialization functions are safe to call while
    /// other process threads are active. Initialization is still required to
    /// happen during single-threaded startup for builds that report false.
    bool thread_safe_global_init{};
    /// Whether this build has asynchronous or threaded DNS resolution.
    bool asynchronous_dns{};
};

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
    /// Maximum cumulative response header bytes, including status lines and
    /// separators from interim and final response blocks.
    std::size_t max_response_header_bytes{ 128 * 1024 };
    /// Maximum cumulative number of response fields across interim and final
    /// response blocks. Repeated fields each count separately.
    std::size_t max_response_header_fields{ 256 };
    /// Maximum wire size of one response field line, including its line ending.
    std::size_t max_response_header_field_bytes{ 16 * 1024 };
    /// When true (the default), sends are rejected before dispatch if runtime
    /// libcurl lacks asynchronous/threaded DNS; every send has a connection
    /// timeout. Set false only to accept best-effort timeout behavior during DNS.
    bool require_async_dns_for_timeouts{ true };
};

/// Synchronous libcurl transport, available through the optional
/// discordcpp::curl_transport target. Each send() owns a separate easy handle,
/// so concurrent calls on one instance are supported. The transport does not
/// own or log credentials and does not follow redirects or retry requests.
///
/// Call initialize() once during application startup before concurrent use.
/// It reports the linked runtime's global-init and asynchronous-DNS features.
/// On runtimes without thread-safe global initialization, call it on the
/// single-threaded startup path before any other libcurl use in the process.
/// Builds without asynchronous DNS can be used for timed requests only by
/// setting require_async_dns_for_timeouts=false; in that mode DNS may block
/// past the deadline. Cancellation is cooperative and may also be delayed by
/// blocking system/TLS calls. Deadlines are checked before and after execution
/// and applied to libcurl when representable. Configured timeout durations must
/// fit in libcurl's long-valued millisecond option; larger values are rejected.
/// Far-future request deadlines omit that option and are checked monotonically.
///
/// TLS peer and hostname verification remain enabled. The default HTTPS URL is
/// secure; HTTP URLs are allowed for local fixtures, but credentials sent over
/// non-loopback HTTP are cleartext. Global libcurl state is retained until
/// process exit; this transport never calls curl_global_cleanup(). Each send
/// uses a separate easy handle, which supports concurrent calls but prevents
/// connection reuse between calls. Response bodies are unlimited unless capped;
/// response headers have bounded configurable defaults.
/// Construction throws std::invalid_argument for invalid options. send()
/// rejects malformed methods/targets, caller-supplied Host/Content-Length/
/// Transfer-Encoding fields, and HEAD requests with bodies before dispatch.
class CurlHttpTransport final : public IHttpTransport {
public:
    explicit CurlHttpTransport(CurlHttpTransportOptions options = {});
    ~CurlHttpTransport() override = default;

    /// Initialize libcurl and report runtime capabilities. Call once during
    /// single-threaded application startup before concurrent use.
    /// This library intentionally retains process-global libcurl state and does
    /// not clean it up, because other modules may also use libcurl.
    [[nodiscard]] static Result<CurlHttpTransportCapabilities, TransportError> initialize();

    CurlHttpTransport(const CurlHttpTransport&) = delete;
    CurlHttpTransport& operator=(const CurlHttpTransport&) = delete;
    CurlHttpTransport(CurlHttpTransport&&) = delete;
    CurlHttpTransport& operator=(CurlHttpTransport&&) = delete;

    [[nodiscard]] TransportResult send(const HttpRequest& request, std::stop_token cancellation) override;

private:
    CurlHttpTransportOptions options_;
};

} // namespace discordcpp
