#pragma once

#include <discordcpp/discord_error.hpp>
#include <discordcpp/http_types.hpp>
#include <discordcpp/result.hpp>

#include <stop_token>

namespace discordcpp {

/// Result of one synchronous transport operation.
using TransportResult = Result<HttpResponse, TransportError>;

/// Injectable synchronous HTTP transport, independent of any networking or TLS
/// implementation.
///
/// Implementations must not automatically retry or follow redirects. send()
/// must return only a final HTTP response; interim informational responses (1xx)
/// must be handled internally. Implementations must observe cancellation before
/// beginning I/O and while a request is active, and honor HttpRequest::deadline
/// across the full send operation. Cancellation and deadline enforcement are
/// cooperative: a synchronous call cannot be forcibly interrupted by RestClient.
/// A returned TransportError must conservatively set
/// request_may_have_reached_server when delivery cannot be ruled out. If a
/// transport cancels an active operation, it must return a cancelled
/// TransportError rather than an HTTP response.
///
/// Implementations define their own thread-safety. Concurrent calls through one
/// instance are supported only if the implementation explicitly supports them.
class IHttpTransport {
public:
    virtual ~IHttpTransport() = default;

    IHttpTransport() = default;
    IHttpTransport(const IHttpTransport&) = delete;
    IHttpTransport& operator=(const IHttpTransport&) = delete;

    /// Synchronously send one request. The request reference and its contents
    /// need remain valid only until this function returns.
    virtual TransportResult send(const HttpRequest& request, std::stop_token cancellation) = 0;
};

} // namespace discordcpp

