#pragma once

#include <discordcpp/discord_error.hpp>
#include <discordcpp/http_types.hpp>
#include <discordcpp/result.hpp>

#include <cstdint>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <variant>
#include <vector>

namespace discordcpp {

using RateLimitAcquireError = std::variant<TransportError, RateLimitMetadataError>;

/// Opaque, non-secret identifier for one rate-limit identity. Never pass a raw
/// token here. A coordinator is shared by clients using the same identity.
class RateLimitIdentity final {
public:
    explicit RateLimitIdentity(std::uint64_t opaque_id) noexcept : opaque_id_{ opaque_id } {}
    [[nodiscard]] std::uint64_t opaque_id() const noexcept { return opaque_id_; }

private:
    std::uint64_t opaque_id_;
};

enum class MajorResourceKind { channel, guild, webhook };

/// For webhook tokens, opaque_discriminator must be a caller-generated stable
/// non-secret fingerprint. The raw webhook token must never be stored here.
struct MajorResource final {
    MajorResourceKind kind{};
    std::string id;
    std::uint64_t opaque_discriminator{};
};

enum class SharedResourceKind { channel, guild, webhook, other };

/// Explicit resource identity used only for Discord's shared-scope cooldown.
/// For webhook tokens, use opaque_discriminator instead of the raw token.
struct SharedResourceKey final {
    SharedResourceKind kind{};
    std::string id;
    std::uint64_t opaque_discriminator{};
};

enum class GlobalLimitClass { ordinary, interaction_exempt };

/// Caller-supplied route metadata. normalized_path is an endpoint template,
/// e.g. "/channels/{channel_id}/messages/{message_id}"; the coordinator never
/// infers it from HttpRequest::target.
struct RouteDescriptor final {
    std::string method;
    std::string normalized_path;
    std::vector<MajorResource> major_resources;
    GlobalLimitClass global_class{ GlobalLimitClass::ordinary };
    std::optional<SharedResourceKey> shared_resource;
};

/// Injectable monotonic scheduler. wait_for_change returns on notification,
/// cancellation, or reaching wake_at. Implementations must be thread-safe and
/// use no wall-clock time for cooldown scheduling.
class IRateLimitScheduler {
public:
    virtual ~IRateLimitScheduler() = default;
    [[nodiscard]] virtual SteadyClock::time_point now() const noexcept = 0;
    [[nodiscard]] virtual std::uint64_t generation() const noexcept = 0;
    virtual void wait_for_change(std::uint64_t observed_generation,
        std::optional<SteadyClock::time_point> wake_at, std::stop_token cancellation) = 0;
    virtual void notify_all() noexcept = 0;
};

/// Production scheduler based only on steady_clock and condition variables.
class SteadyRateLimitScheduler final : public IRateLimitScheduler {
public:
    [[nodiscard]] SteadyClock::time_point now() const noexcept override;
    [[nodiscard]] std::uint64_t generation() const noexcept override;
    void wait_for_change(std::uint64_t observed_generation,
        std::optional<SteadyClock::time_point> wake_at, std::stop_token cancellation) override;
    void notify_all() noexcept override;

private:
    mutable std::mutex mutex_;
    std::condition_variable_any condition_;
    std::uint64_t generation_{};
};

namespace detail { struct RateLimitState; }

/// Move-only reservation for one request. Destruction releases an undispatched
/// reservation; after mark_dispatched(), abandonment is charged conservatively.
class RateLimitPermit final {
public:
    RateLimitPermit() noexcept = default;
    RateLimitPermit(const RateLimitPermit&) = delete;
    RateLimitPermit& operator=(const RateLimitPermit&) = delete;
    RateLimitPermit(RateLimitPermit&& other) noexcept;
    RateLimitPermit& operator=(RateLimitPermit&& other) noexcept;
    ~RateLimitPermit();

    void mark_dispatched() noexcept;
    [[nodiscard]] bool valid() const noexcept;

private:
    friend class RateLimitCoordinator;
    RateLimitPermit(std::weak_ptr<detail::RateLimitState> state, std::uint64_t reservation_id) noexcept;
    void reset() noexcept;

    std::weak_ptr<detail::RateLimitState> state_;
    std::uint64_t reservation_id_{};
};

/// Thread-safe server-reported Discord rate-limit coordinator. Construct one
/// instance per credential identity and share it across RestClient instances.
/// The scheduler must outlive the coordinator. Raw credentials are neither
/// accepted nor retained. If a response has no
/// usable limit headers, no quota is invented: an unknown route permits at most
/// one in-flight request, then proceeds serially. A 429 without usable timing,
/// a shared-scope 429 without an explicit shared resource, and an exhausted
/// bucket without reset metadata place the affected scope in a metadata-blocked
/// state. Detailed acquisition returns RateLimitMetadataError instead of
/// waiting forever. authorize_single_probe() explicitly permits one serialized
/// probe; no retry or delay is inferred, and a probe that fails to resolve the
/// metadata condition requires a new explicit authorization. A final 2xx global
/// or shared probe is sufficient recovery evidence even without rate-limit
/// headers; the scope then uses conservative one-in-flight serialization until
/// usable metadata arrives. Non-2xx responses, including 1xx, 3xx, 4xx, and 5xx,
/// do not clear quarantine. An exhausted route bucket without reset stays blocked
/// and requires explicit authorization for each later probe.
///
/// The coordinator must outlive every active coordinated call and outstanding
/// permit. Permits hold weak state ownership; destroying a permit after its
/// coordinator has been destroyed is a no-op and cannot release/update state.
/// Before a canonical bucket hash is discovered, distinct provisional routes
/// may each dispatch requests. A later merge preserves conservative accounting
/// but cannot undo requests already dispatched before that shared bucket was known.
class RateLimitCoordinator final {
public:
    RateLimitCoordinator(RateLimitIdentity identity, IRateLimitScheduler& scheduler);
    ~RateLimitCoordinator();
    RateLimitCoordinator(const RateLimitCoordinator&) = delete;
    RateLimitCoordinator& operator=(const RateLimitCoordinator&) = delete;

    [[nodiscard]] Result<RateLimitPermit, TransportError> acquire(
        const RouteDescriptor& route,
        std::optional<SteadyClock::time_point> deadline,
        std::stop_token cancellation = {});

    /// Detailed acquisition reports metadata blocks as a distinct error type.
    [[nodiscard]] Result<RateLimitPermit, RateLimitAcquireError> acquire_detailed(
        const RouteDescriptor& route,
        std::optional<SteadyClock::time_point> deadline,
        std::stop_token cancellation = {});

    /// Authorize one request from a metadata-blocked scope affecting route. For
    /// a global scope, any matching ordinary route may consume that authorization.
    /// This never clears the block by itself and permits at most one concurrent
    /// probe for each affected scope. A failed/uncertain probe does not authorize
    /// an automatic replay; a new caller action is required to authorize again.
    [[nodiscard]] Result<void, RateLimitMetadataError> authorize_single_probe(const RouteDescriptor& route);

    /// Observe every HTTP response, including 429, before API error conversion.
    /// On a 429 with unclassifiable scope, missing retry timing, or missing
    /// shared-resource identity, returns a distinct metadata error and records a
    /// recoverable block. Duplicate rate-limit fields are accepted only when
    /// their values are byte-identical; conflicting values are treated as
    /// absent. Conflicting scope or unconfirmed global-signal values block the
    /// route bucket; conflicting bucket hashes disable bucket discovery;
    /// conflicting timing fields are ignored and other timing sources may still
    /// be used.
    std::optional<RateLimitMetadataError> observe_response(
        RateLimitPermit&& permit, const HttpResponse& response);
    /// Release a dispatched reservation. Capacity is restored only when
    /// delivery is known not to have reached the server.
    void observe_transport_error(RateLimitPermit&& permit, const TransportError& error) noexcept;

    [[nodiscard]] SteadyClock::time_point now() const noexcept;

private:
    std::shared_ptr<detail::RateLimitState> state_;
};

} // namespace discordcpp
