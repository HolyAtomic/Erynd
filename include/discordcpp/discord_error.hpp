#pragma once

#include <discordcpp/http_types.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <system_error>

namespace discordcpp {

/// A structured error response returned by Discord's HTTP API.
struct DiscordApiError {
    int http_status{};
    std::optional<std::int64_t> discord_code;
    std::string message;
    std::string raw_body;
    Headers response_headers;
};

/// Which server-reported rate-limit state requires explicit recovery.
enum class RateLimitMetadataScope {
    route_bucket,
    global,
    shared_resource
};

/// Why rate-limit metadata was insufficient to safely schedule another request.
enum class RateLimitMetadataIssue {
    invalid_retry_timing,
    ambiguous_scope_headers,
    shared_resource_required,
    exhausted_bucket_without_reset,
    probe_not_authorized,
    no_recoverable_metadata_block
};

/// A rate-limit scheduling failure, distinct from both an HTTP/API error and a
/// transport failure. The response is present when this error was caused by a
/// 429; it preserves the original status, headers, and body for diagnosis.
struct RateLimitMetadataError {
    RateLimitMetadataIssue issue{ RateLimitMetadataIssue::invalid_retry_timing };
    RateLimitMetadataScope scope{ RateLimitMetadataScope::route_bucket };
    std::string message;
    std::optional<DiscordApiError> response;
};

/// The category of a failure while communicating with an HTTP server.
enum class TransportFailureCategory {
    dns,
    connect,
    tls,
    io,
    timeout,
    cancelled,
    protocol
};

/// A transport failure, distinct from an HTTP/API error.
struct TransportError {
    TransportFailureCategory category{ TransportFailureCategory::io };
    std::string message;
    /// Set true whenever delivery cannot be ruled out; false means the
    /// transport knows the request was not sent to the server.
    bool request_may_have_reached_server{};
    /// Optional native diagnostic, for example a socket or resolver error code.
    std::optional<std::error_code> diagnostic_code;
};

} // namespace discordcpp


