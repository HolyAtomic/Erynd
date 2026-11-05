#pragma once

#include <discordcpp/discord_error.hpp>

#include <cstddef>
#include <optional>
#include <string>
#include <variant>

namespace discordcpp {

enum class JsonDiagnosticCategory {
    syntax,
    invalid_encoding,
    resource_limit,
    processing_failure
};

/// A JSON decoding failure. offset is zero-based when available. For a
/// successful HTTP response, status and headers identify the response without
/// duplicating its potentially large body.
struct JsonParseError final {
    JsonDiagnosticCategory category{ JsonDiagnosticCategory::syntax };
    std::optional<std::size_t> offset;
    std::string path;
    std::optional<int> http_status;
    Headers response_headers;
    std::string message;
};

/// A decoded JSON value did not satisfy the subset of a Discord model consumed
/// by this client. Unknown fields are not this error's concern.
struct ModelValidationError final {
    std::string path;
    std::string message;
    std::optional<int> http_status;
    Headers response_headers;
};

enum class LocalValidationIssue {
    invalid_api_version,
    invalid_endpoint_path
};

/// Local input was rejected before transport dispatch.
struct LocalValidationError final {
    LocalValidationIssue issue{ LocalValidationIssue::invalid_endpoint_path };
    std::string message;
};

/// API-facing errors retain the existing error types as distinct alternatives.
using DiscordClientError = std::variant<
    DiscordApiError,
    TransportError,
    RateLimitMetadataError,
    JsonParseError,
    ModelValidationError,
    LocalValidationError>;

} // namespace discordcpp
