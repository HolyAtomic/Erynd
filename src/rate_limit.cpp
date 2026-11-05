#include <discordcpp/rate_limit.hpp>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace discordcpp {
namespace {

using Nanoseconds = std::chrono::nanoseconds;

std::optional<Nanoseconds> parse_decimal_seconds(std::string_view value) {
    if (value.empty()) return std::nullopt;
    std::size_t position{};
    if (value[position] == '-') return std::nullopt;
    if (value[position] == '+') return std::nullopt;

    std::string digits;
    std::size_t fractional_digits{};
    bool saw_integer_digit{};
    while (position < value.size() && value[position] >= '0' && value[position] <= '9') {
        digits.push_back(value[position++]);
        saw_integer_digit = true;
    }
    if (position < value.size() && value[position] == '.') {
        ++position;
        const auto start = position;
        while (position < value.size() && value[position] >= '0' && value[position] <= '9') {
            digits.push_back(value[position++]);
            ++fractional_digits;
        }
        if (position == start) return std::nullopt;
    }
    if (!saw_integer_digit) return std::nullopt;

    int exponent{};
    if (position < value.size() && (value[position] == 'e' || value[position] == 'E')) {
        ++position;
        bool negative_exponent{};
        if (position < value.size() && (value[position] == '-' || value[position] == '+')) {
            negative_exponent = value[position] == '-';
            ++position;
        }
        const auto start = position;
        int parsed_exponent{};
        while (position < value.size() && value[position] >= '0' && value[position] <= '9') {
            if (parsed_exponent < 10000) {
                parsed_exponent = parsed_exponent * 10 + (value[position] - '0');
            }
            ++position;
        }
        if (position == start) return std::nullopt;
        exponent = negative_exponent ? -parsed_exponent : parsed_exponent;
    }
    if (position != value.size()) return std::nullopt;

    const auto first_nonzero = digits.find_first_not_of('0');
    if (first_nonzero == std::string::npos) return Nanoseconds::zero();
    digits.erase(0, first_nonzero);

    const long long scale = static_cast<long long>(exponent) - static_cast<long long>(fractional_digits) + 9;
    constexpr auto max_count = static_cast<std::uint64_t>(Nanoseconds::max().count());
    std::uint64_t result{};
    if (scale >= 0) {
        if (scale > 19 || digits.size() + static_cast<std::size_t>(scale) > 19) return std::nullopt;
        const auto conversion = std::from_chars(digits.data(), digits.data() + digits.size(), result);
        if (conversion.ec != std::errc{} || conversion.ptr != digits.data() + digits.size()) return std::nullopt;
        for (long long index = 0; index < scale; ++index) {
            if (result > std::numeric_limits<std::uint64_t>::max() / 10) return std::nullopt;
            result *= 10;
        }
    } else {
        const auto discarded_count = static_cast<unsigned long long>(-scale);
        const auto kept_count = discarded_count >= digits.size() ? 0U : digits.size() - static_cast<std::size_t>(discarded_count);
        if (kept_count > 19) return std::nullopt;
        if (kept_count != 0) {
            const auto conversion = std::from_chars(digits.data(), digits.data() + kept_count, result);
            if (conversion.ec != std::errc{} || conversion.ptr != digits.data() + kept_count) return std::nullopt;
        }
        bool remainder_nonzero{};
        for (std::size_t index = kept_count; index < digits.size(); ++index) {
            remainder_nonzero = remainder_nonzero || digits[index] != '0';
        }
        if (remainder_nonzero) {
            if (result == std::numeric_limits<std::uint64_t>::max()) return std::nullopt;
            ++result; // round upward so a cooldown is never shortened
        }
    }
    if (result > max_count) return std::nullopt;
    return Nanoseconds{ static_cast<Nanoseconds::rep>(result) };
}

std::optional<std::uint64_t> parse_unsigned(std::string_view value) noexcept {
    if (value.empty()) return std::nullopt;
    std::uint64_t parsed{};
    const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != value.data() + value.size()) return std::nullopt;
    return parsed;
}

bool append_utf8_agnostic_json_string(std::string_view body, std::size_t& at) noexcept {
    if (at >= body.size() || body[at++] != '"') return false;
    while (at < body.size()) {
        const auto ch = static_cast<unsigned char>(body[at++]);
        if (ch == '"') return true;
        if (ch < 0x20) return false;
        if (ch != '\\') continue;
        if (at >= body.size()) return false;
        const char escape = body[at++];
        if (escape == 'u') {
            if (body.size() - at < 4) return false;
            for (int i = 0; i < 4; ++i) {
                const char digit = body[at++];
                if (!((digit >= '0' && digit <= '9') || (digit >= 'a' && digit <= 'f') || (digit >= 'A' && digit <= 'F'))) return false;
            }
        } else if (escape != '"' && escape != '\\' && escape != '/' && escape != 'b' && escape != 'f' &&
                   escape != 'n' && escape != 'r' && escape != 't') {
            return false;
        }
    }
    return false;
}

void skip_ws(std::string_view body, std::size_t& at) noexcept {
    while (at < body.size() && (body[at] == ' ' || body[at] == '\t' || body[at] == '\r' || body[at] == '\n')) ++at;
}

bool skip_value(std::string_view body, std::size_t& at, unsigned depth) noexcept;

bool skip_compound(std::string_view body, std::size_t& at, unsigned depth, char open, char close) noexcept {
    if (depth > 64 || at >= body.size() || body[at++] != open) return false;
    skip_ws(body, at);
    if (at < body.size() && body[at] == close) { ++at; return true; }
    while (at < body.size()) {
        if (open == '{') {
            if (!append_utf8_agnostic_json_string(body, at)) return false;
            skip_ws(body, at);
            if (at >= body.size() || body[at++] != ':') return false;
            skip_ws(body, at);
        }
        if (!skip_value(body, at, depth + 1)) return false;
        skip_ws(body, at);
        if (at < body.size() && body[at] == close) { ++at; return true; }
        if (at >= body.size() || body[at++] != ',') return false;
        skip_ws(body, at);
    }
    return false;
}

bool skip_value(std::string_view body, std::size_t& at, unsigned depth) noexcept {
    if (depth > 64 || at >= body.size()) return false;
    if (body[at] == '"') return append_utf8_agnostic_json_string(body, at);
    if (body[at] == '{') return skip_compound(body, at, depth, '{', '}');
    if (body[at] == '[') return skip_compound(body, at, depth, '[', ']');
    for (const auto literal : { std::string_view{"true"}, std::string_view{"false"}, std::string_view{"null"} }) {
        if (body.substr(at, literal.size()) == literal) { at += literal.size(); return true; }
    }
    const auto start = at;
    if (body[at] == '-') ++at;
    if (at >= body.size() || body[at] < '0' || body[at] > '9') return false;
    if (body[at] == '0') ++at;
    else while (at < body.size() && body[at] >= '0' && body[at] <= '9') ++at;
    if (at < body.size() && body[at] == '.') {
        ++at; const auto fraction = at;
        while (at < body.size() && body[at] >= '0' && body[at] <= '9') ++at;
        if (at == fraction) return false;
    }
    if (at < body.size() && (body[at] == 'e' || body[at] == 'E')) {
        ++at; if (at < body.size() && (body[at] == '+' || body[at] == '-')) ++at;
        const auto exponent = at;
        while (at < body.size() && body[at] >= '0' && body[at] <= '9') ++at;
        if (at == exponent) return false;
    }
    return at > start;
}

struct BodyRateFields {
    std::optional<Nanoseconds> retry_after;
    std::optional<bool> global;
};

std::optional<BodyRateFields> parse_rate_body(std::string_view body) {
    BodyRateFields fields;
    std::size_t at{};
    skip_ws(body, at);
    if (at >= body.size() || body[at++] != '{') return std::nullopt;
    skip_ws(body, at);
    if (at < body.size() && body[at] == '}') { ++at; skip_ws(body, at); return at == body.size() ? std::optional{fields} : std::nullopt; }
    while (at < body.size()) {
        const auto key_start = at;
        if (!append_utf8_agnostic_json_string(body, at)) return std::nullopt;
        // Discord emits plain ASCII field names. Decode is unnecessary for the
        // recognized wire names; escaped spellings safely remain unrecognized.
        const auto raw_key = body.substr(key_start + 1, at - key_start - 2);
        skip_ws(body, at);
        if (at >= body.size() || body[at++] != ':') return std::nullopt;
        skip_ws(body, at);
        if (raw_key == "retry_after") {
            const auto value_start = at;
            if (!skip_value(body, at, 0)) return std::nullopt;
            const auto token = body.substr(value_start, at - value_start);
            if (const auto parsed = parse_decimal_seconds(token); parsed.has_value()) fields.retry_after = parsed;
        } else if (raw_key == "global" && at < body.size() && (body.substr(at, 4) == "true" || body.substr(at, 5) == "false")) {
            fields.global = body[at] == 't';
            if (!skip_value(body, at, 0)) return std::nullopt;
        } else if (!skip_value(body, at, 0)) {
            return std::nullopt;
        }
        skip_ws(body, at);
        if (at < body.size() && body[at] == '}') { ++at; skip_ws(body, at); return at == body.size() ? std::optional{fields} : std::nullopt; }
        if (at >= body.size() || body[at++] != ',') return std::nullopt;
        skip_ws(body, at);
    }
    return std::nullopt;
}

std::string lower_ascii(std::string_view value) {
    std::string result;
    result.reserve(value.size());
    for (const unsigned char ch : value) result.push_back(static_cast<char>(ch >= 'A' && ch <= 'Z' ? ch + ('a' - 'A') : ch));
    return result;
}

std::string encoded(std::string_view value) {
    return std::to_string(value.size()) + ':' + std::string{ value };
}

std::string major_key(const std::vector<MajorResource>& resources) {
    std::vector<MajorResource> sorted = resources;
    std::sort(sorted.begin(), sorted.end(), [](const auto& left, const auto& right) {
        if (left.kind != right.kind) return left.kind < right.kind;
        if (left.id != right.id) return left.id < right.id;
        return left.opaque_discriminator < right.opaque_discriminator;
    });
    std::string result;
    for (const auto& resource : sorted) {
        result += std::to_string(static_cast<int>(resource.kind)) + ':' + encoded(resource.id) +
            std::to_string(resource.opaque_discriminator) + ';';
    }
    return result;
}

std::string provisional_key(const RouteDescriptor& route) {
    return encoded(route.method) + encoded(route.normalized_path) + major_key(route.major_resources);
}

std::string canonical_key(std::string_view hash, std::string_view majors) {
    return encoded(hash) + std::string{ majors };
}

SteadyClock::time_point add_saturated(SteadyClock::time_point start, Nanoseconds duration) noexcept {
    const auto converted = std::chrono::duration_cast<SteadyClock::duration>(duration);
    if (converted > SteadyClock::time_point::max() - start) return SteadyClock::time_point::max();
    return start + converted;
}

struct HeaderValue final {
    std::optional<std::string_view> value;
    bool conflicting{};
};

/// Duplicate rate-limit fields are accepted only when every value is byte-for-
/// byte identical. Conflicts are ambiguous and treated as absent; this also
/// makes malformed-first/valid-second input ambiguous instead of trusting order.
HeaderValue header(const Headers& headers, std::string_view name) {
    const auto values = headers.find_all(name);
    if (values.empty()) return {};
    for (std::size_t i = 1; i < values.size(); ++i) {
        if (values[i] != values.front()) return { std::nullopt, true };
    }
    return { values.front(), false };
}

bool parse_bool_header(std::optional<std::string_view> value) {
    if (!value.has_value()) return false;
    const auto normalized = lower_ascii(*value);
    return normalized == "true" || normalized == "1";
}

} // namespace

namespace detail {

struct BucketState {
    std::shared_ptr<BucketState> parent;
    std::optional<std::uint64_t> limit;
    std::optional<std::uint64_t> remaining;
    std::size_t in_flight{};
    std::optional<SteadyClock::time_point> reset_at;
    std::optional<SteadyClock::duration> window_duration;
    std::optional<SteadyClock::time_point> cooldown_until;
    bool metadata_blocked{};
    bool probe_available{};
    std::size_t probe_in_flight{};
    RateLimitMetadataIssue metadata_issue{ RateLimitMetadataIssue::invalid_retry_timing };
    std::uint64_t latest_observation{};
    std::uint64_t latest_cooldown_sequence{};
};

struct Reservation {
    std::shared_ptr<BucketState> bucket;
    RouteDescriptor route;
    std::string provisional;
    std::string majors;
    std::uint64_t sequence{};
    bool dispatched{};
    bool bucket_probe{};
    bool global_probe{};
    bool shared_probe{};
    bool ordinary_counted{};
    bool shared_counted{};
};

struct SharedCooldown {
    std::optional<SteadyClock::time_point> until;
    bool metadata_blocked{};
    bool probe_available{};
    bool probe_in_flight{};
    bool unknown_serial{};
    std::size_t in_flight{};
    RateLimitMetadataIssue metadata_issue{ RateLimitMetadataIssue::invalid_retry_timing };
    std::uint64_t latest_sequence{};
};

struct RateLimitState {
    explicit RateLimitState(RateLimitIdentity identityNew, IRateLimitScheduler& schedulerNew)
        : identity{ identityNew }, scheduler{ schedulerNew } {}

    RateLimitIdentity identity;
    IRateLimitScheduler& scheduler;
    mutable std::mutex mutex;
    std::unordered_map<std::string, std::shared_ptr<BucketState>> provisional_buckets;
    std::unordered_map<std::string, std::shared_ptr<BucketState>> canonical_buckets;
    std::unordered_map<std::string, std::uint64_t> route_mapping_sequence;
    std::unordered_map<std::string, SharedCooldown> shared_cooldowns;
    std::unordered_map<std::uint64_t, Reservation> reservations;
    std::size_t ordinary_in_flight{};
    std::optional<SteadyClock::time_point> global_cooldown;
    bool global_metadata_blocked{};
    bool global_probe_available{};
    bool global_probe_in_flight{};
    bool global_unknown_serial{};
    RateLimitMetadataIssue global_metadata_issue{ RateLimitMetadataIssue::invalid_retry_timing };
    std::uint64_t global_latest_sequence{};
    std::uint64_t next_sequence{ 1 };
    std::uint64_t next_reservation{ 1 };

    std::shared_ptr<BucketState> root(std::shared_ptr<BucketState> bucket) {
        while (bucket && bucket->parent) bucket = bucket->parent;
        return bucket;
    }

    static std::uint64_t subtract_saturating(std::uint64_t value, std::size_t amount) noexcept {
        return amount >= value ? 0 : value - static_cast<std::uint64_t>(amount);
    }

    std::shared_ptr<BucketState> merge(std::shared_ptr<BucketState> left, std::shared_ptr<BucketState> right) {
        left = root(std::move(left));
        right = root(std::move(right));
        if (!left || !right || left == right) return left ? left : right;
        const auto left_remaining = left->remaining;
        const auto right_remaining = right->remaining;
        const auto left_flight = left->in_flight;
        const auto right_flight = right->in_flight;
        if (left_remaining && right_remaining) {
            left->remaining = std::min(subtract_saturating(*left_remaining, right_flight),
                                       subtract_saturating(*right_remaining, left_flight));
        } else if (left_remaining) {
            left->remaining = subtract_saturating(*left_remaining, right_flight);
        } else if (right_remaining) {
            left->remaining = subtract_saturating(*right_remaining, left_flight);
        }
        if (left->limit && right->limit) left->limit = std::min(*left->limit, *right->limit);
        else if (!left->limit) left->limit = right->limit;
        left->in_flight = left_flight > std::numeric_limits<std::size_t>::max() - right_flight
            ? std::numeric_limits<std::size_t>::max() : left_flight + right_flight;
        if (right->reset_at && (!left->reset_at || *right->reset_at > *left->reset_at)) left->reset_at = right->reset_at;
        if (right->window_duration && (!left->window_duration || *right->window_duration > *left->window_duration)) left->window_duration = right->window_duration;
        if (right->cooldown_until && (!left->cooldown_until || *right->cooldown_until > *left->cooldown_until)) left->cooldown_until = right->cooldown_until;
        if (left->metadata_blocked || right->metadata_blocked) {
            if (!left->metadata_blocked && right->metadata_blocked) left->metadata_issue = right->metadata_issue;
            left->metadata_blocked = true;
            left->probe_available = false;
        }
        left->probe_in_flight = left->probe_in_flight > std::numeric_limits<std::size_t>::max() - right->probe_in_flight
            ? std::numeric_limits<std::size_t>::max() : left->probe_in_flight + right->probe_in_flight;
        left->latest_observation = std::max(left->latest_observation, right->latest_observation);
        left->latest_cooldown_sequence = std::max(left->latest_cooldown_sequence, right->latest_cooldown_sequence);
        right->parent = left;
        return left;
    }

    void release(std::uint64_t id, std::optional<bool> known_not_sent) noexcept {
        std::lock_guard lock{ mutex };
        const auto found = reservations.find(id);
        if (found == reservations.end()) return;
        auto& reservation = found->second;
        auto bucket = root(reservation.bucket);
        if (bucket && bucket->in_flight != 0) --bucket->in_flight;
        if (known_not_sent.value_or(!reservation.dispatched) && bucket && bucket->remaining && !reservation.bucket_probe) {
            if (*bucket->remaining < std::numeric_limits<std::uint64_t>::max()) ++*bucket->remaining;
        }
        if (bucket && reservation.bucket_probe && bucket->probe_in_flight != 0) {
            --bucket->probe_in_flight;
            if (known_not_sent.value_or(!reservation.dispatched) && bucket->metadata_blocked && bucket->probe_in_flight == 0)
                bucket->probe_available = true;
        }
        if (reservation.global_probe) {
            global_probe_in_flight = false;
            if (known_not_sent.value_or(!reservation.dispatched) && global_metadata_blocked) global_probe_available = true;
        }
        if (reservation.ordinary_counted && ordinary_in_flight != 0) --ordinary_in_flight;
        if (reservation.shared_counted) {
            const auto shared_key = reservation.route.shared_resource
                ? std::to_string(static_cast<int>(reservation.route.shared_resource->kind)) + ':' + encoded(reservation.route.shared_resource->id) +
                    std::to_string(reservation.route.shared_resource->opaque_discriminator)
                : std::string{};
            if (const auto shared = shared_cooldowns.find(shared_key); shared != shared_cooldowns.end()) {
                if (reservation.shared_probe) {
                    shared->second.probe_in_flight = false;
                    if (known_not_sent.value_or(!reservation.dispatched) && shared->second.metadata_blocked)
                        shared->second.probe_available = true;
                }
                if (shared->second.in_flight != 0) --shared->second.in_flight;
            }
        }
        reservations.erase(found);
        scheduler.notify_all();
    }

    void mark_dispatched(std::uint64_t id) noexcept {
        std::lock_guard lock{ mutex };
        const auto found = reservations.find(id);
        if (found != reservations.end()) found->second.dispatched = true;
    }

    void prune_expired(SteadyClock::time_point now) {
        if (next_sequence % 128 != 0) return;
        std::unordered_set<const BucketState*> expired;
        const auto is_expired = [&](const std::shared_ptr<BucketState>& candidate) {
            const auto bucket = root(candidate);
            if (!bucket || bucket->in_flight != 0 || bucket->metadata_blocked) return false;
            if (bucket->cooldown_until && now < *bucket->cooldown_until) return false;
            if (bucket->reset_at && now < *bucket->reset_at) return false;
            if (bucket->remaining && !bucket->reset_at && !bucket->cooldown_until) return false;
            return true;
        };
        for (const auto& entry : provisional_buckets) if (is_expired(entry.second)) expired.insert(root(entry.second).get());
        for (const auto& entry : canonical_buckets) if (is_expired(entry.second)) expired.insert(root(entry.second).get());
        if (expired.empty()) return;
        for (auto it = provisional_buckets.begin(); it != provisional_buckets.end();) {
            if (expired.contains(root(it->second).get())) {
                route_mapping_sequence.erase(it->first);
                it = provisional_buckets.erase(it);
            } else ++it;
        }
        for (auto it = canonical_buckets.begin(); it != canonical_buckets.end();) {
            if (expired.contains(root(it->second).get())) it = canonical_buckets.erase(it);
            else ++it;
        }
        for (auto it = shared_cooldowns.begin(); it != shared_cooldowns.end();) {
            if (!it->second.metadata_blocked && !it->second.unknown_serial && it->second.in_flight == 0 &&
                (!it->second.until || now >= *it->second.until)) it = shared_cooldowns.erase(it);
            else ++it;
        }
        if (global_cooldown && now >= *global_cooldown) global_cooldown.reset();
    }
};

} // namespace detail

SteadyClock::time_point SteadyRateLimitScheduler::now() const noexcept { return SteadyClock::now(); }

std::uint64_t SteadyRateLimitScheduler::generation() const noexcept {
    std::lock_guard lock{ mutex_ };
    return generation_;
}

void SteadyRateLimitScheduler::wait_for_change(std::uint64_t observed_generation,
    std::optional<SteadyClock::time_point> wake_at, std::stop_token cancellation) {
    std::unique_lock lock{ mutex_ };
    const auto changed = [&] { return generation_ != observed_generation; };
    if (wake_at) condition_.wait_until(lock, cancellation, *wake_at, changed);
    else condition_.wait(lock, cancellation, changed);
}

void SteadyRateLimitScheduler::notify_all() noexcept {
    {
        std::lock_guard lock{ mutex_ };
        ++generation_;
    }
    condition_.notify_all();
}

RateLimitPermit::RateLimitPermit(std::weak_ptr<detail::RateLimitState> state, std::uint64_t reservation_id) noexcept
    : state_{ std::move(state) }, reservation_id_{ reservation_id } {}

RateLimitPermit::RateLimitPermit(RateLimitPermit&& other) noexcept
    : state_{ std::move(other.state_) }, reservation_id_{ std::exchange(other.reservation_id_, 0) } {}

RateLimitPermit& RateLimitPermit::operator=(RateLimitPermit&& other) noexcept {
    if (this != &other) {
        reset();
        state_ = std::move(other.state_);
        reservation_id_ = std::exchange(other.reservation_id_, 0);
    }
    return *this;
}

RateLimitPermit::~RateLimitPermit() { reset(); }

void RateLimitPermit::mark_dispatched() noexcept {
    if (reservation_id_ != 0) if (auto state = state_.lock()) state->mark_dispatched(reservation_id_);
}

bool RateLimitPermit::valid() const noexcept { return reservation_id_ != 0 && !state_.expired(); }

void RateLimitPermit::reset() noexcept {
    if (reservation_id_ != 0) {
        if (auto state = state_.lock()) state->release(reservation_id_, std::nullopt);
        reservation_id_ = 0;
        state_.reset();
    }
}

RateLimitCoordinator::RateLimitCoordinator(RateLimitIdentity identity, IRateLimitScheduler& scheduler)
    : state_{ std::make_shared<detail::RateLimitState>(identity, scheduler) } {}

RateLimitCoordinator::~RateLimitCoordinator() = default;

SteadyClock::time_point RateLimitCoordinator::now() const noexcept { return state_->scheduler.now(); }

Result<RateLimitPermit, TransportError> RateLimitCoordinator::acquire(
    const RouteDescriptor& route, std::optional<SteadyClock::time_point> deadline, std::stop_token cancellation) {
    auto detailed = acquire_detailed(route, deadline, cancellation);
    if (detailed.state() == ResultState::valueless) {
        return Result<RateLimitPermit, TransportError>::failure({
            TransportFailureCategory::protocol, "rate-limit acquisition returned a valueless result", false });
    }
    if (detailed.has_value()) return Result<RateLimitPermit, TransportError>::success(std::move(detailed).value());
    auto error = std::move(detailed).error();
    if (std::holds_alternative<TransportError>(error))
        return Result<RateLimitPermit, TransportError>::failure(std::move(std::get<TransportError>(error)));
    const auto& metadata = std::get<RateLimitMetadataError>(error);
    return Result<RateLimitPermit, TransportError>::failure({
        TransportFailureCategory::protocol, metadata.message, false });
}

Result<RateLimitPermit, RateLimitAcquireError> RateLimitCoordinator::acquire_detailed(
    const RouteDescriptor& route, std::optional<SteadyClock::time_point> deadline, std::stop_token cancellation) {
    const auto transport_error = [](TransportFailureCategory category, std::string message) {
        RateLimitAcquireError error{ std::in_place_type<TransportError>, TransportError{ category, std::move(message), false } };
        return Result<RateLimitPermit, RateLimitAcquireError>::failure(std::move(error));
    };
    const auto metadata_error = [](RateLimitMetadataIssue issue, RateLimitMetadataScope scope, std::string message) {
        RateLimitAcquireError error{ std::in_place_type<RateLimitMetadataError>, RateLimitMetadataError{ issue, scope, std::move(message), std::nullopt } };
        return Result<RateLimitPermit, RateLimitAcquireError>::failure(std::move(error));
    };
    if (route.method.empty() || route.normalized_path.empty() || route.normalized_path.front() != '/') {
        return transport_error(TransportFailureCategory::protocol, "invalid explicit rate-limit route descriptor");
    }
    for (const auto& resource : route.major_resources) {
        if (resource.id.empty()) return transport_error(TransportFailureCategory::protocol, "major resource identifier must not be empty");
    }
    if (route.shared_resource && route.shared_resource->id.empty()) {
        return transport_error(TransportFailureCategory::protocol, "shared resource identifier must not be empty");
    }

    const auto provisional = provisional_key(route);
    const auto majors = major_key(route.major_resources);
    const auto shared = route.shared_resource
        ? std::to_string(static_cast<int>(route.shared_resource->kind)) + ':' + encoded(route.shared_resource->id) +
            std::to_string(route.shared_resource->opaque_discriminator)
        : std::string{};

    while (true) {
        if (cancellation.stop_requested()) return transport_error(TransportFailureCategory::cancelled, "cancelled while waiting for rate-limit permit");
        const auto now = state_->scheduler.now();
        if (deadline && now >= *deadline) return transport_error(TransportFailureCategory::timeout, "deadline expired while waiting for rate-limit permit");

        std::optional<SteadyClock::time_point> wake_at = deadline;
        std::uint64_t observed_generation{};
        {
            std::unique_lock lock{ state_->mutex };
            state_->prune_expired(now);
            auto& mapped = state_->provisional_buckets[provisional];
            if (!mapped) mapped = std::make_shared<detail::BucketState>();
            auto bucket = state_->root(mapped);
            mapped = bucket;

            const auto refresh_bucket = [&](detail::BucketState& value) {
                if (value.reset_at && now >= *value.reset_at) {
                    if (value.limit) value.remaining = detail::RateLimitState::subtract_saturating(*value.limit, value.in_flight);
                    else value.remaining.reset();
                    if (value.window_duration && *value.window_duration > SteadyClock::duration::zero()) {
                        // Restart the local observation window from detection.
                        // This can wait slightly longer after a long idle period,
                        // but never schedules from a wall clock or shortens a limit.
                        value.reset_at = add_saturated(now, std::chrono::duration_cast<Nanoseconds>(*value.window_duration));
                    } else {
                        value.reset_at.reset();
                    }
                }
                if (value.cooldown_until && now >= *value.cooldown_until) {
                    value.cooldown_until.reset();
                    if (!value.reset_at && value.remaining && *value.remaining == 0) {
                        if (value.limit) value.remaining = detail::RateLimitState::subtract_saturating(*value.limit, value.in_flight);
                        else value.remaining.reset();
                    }
                }
            };
            refresh_bucket(*bucket);

            bool bucket_probe{};
            bool global_probe{};
            bool shared_probe{};
            if (route.global_class == GlobalLimitClass::ordinary && state_->global_metadata_blocked) {
                if (!state_->global_probe_available || state_->global_probe_in_flight)
                    return metadata_error(state_->global_metadata_issue, RateLimitMetadataScope::global,
                        "global rate-limit metadata is unavailable; explicitly authorize one probe");
                global_probe = true;
            }
            const bool global_serial_gate = route.global_class == GlobalLimitClass::ordinary &&
                (state_->global_unknown_serial || global_probe);
            bool shared_serial_gate{};
            if (shared.size()) {
                if (const auto found = state_->shared_cooldowns.find(shared);
                    found != state_->shared_cooldowns.end() && found->second.metadata_blocked) {
                    if (!found->second.probe_available || found->second.probe_in_flight)
                        return metadata_error(found->second.metadata_issue, RateLimitMetadataScope::shared_resource,
                            "shared-resource rate-limit metadata is unavailable; explicitly authorize one probe");
                    shared_probe = true;
                }
                if (const auto found = state_->shared_cooldowns.find(shared); found != state_->shared_cooldowns.end())
                    shared_serial_gate = found->second.unknown_serial || shared_probe;
            }
            if (bucket->metadata_blocked) {
                if (!bucket->probe_available || bucket->probe_in_flight != 0)
                    return metadata_error(bucket->metadata_issue, RateLimitMetadataScope::route_bucket,
                        "route-bucket rate-limit metadata is unavailable; explicitly authorize one probe");
                bucket_probe = true;
            }

            std::optional<SteadyClock::time_point> blocked_until;
            if (route.global_class == GlobalLimitClass::ordinary && state_->global_cooldown && now < *state_->global_cooldown)
                blocked_until = state_->global_cooldown;
            if (shared.size() && state_->shared_cooldowns.contains(shared)) {
                const auto& cooldown = state_->shared_cooldowns.at(shared);
                if (cooldown.until && now < *cooldown.until && (!blocked_until || *cooldown.until > *blocked_until)) blocked_until = cooldown.until;
            }
            if (bucket->cooldown_until && now < *bucket->cooldown_until && (!blocked_until || *bucket->cooldown_until > *blocked_until))
                blocked_until = bucket->cooldown_until;
            const bool capacity = bucket_probe ? bucket->in_flight == 0
                : (bucket->remaining ? *bucket->remaining > 0 : bucket->in_flight == 0);
            const bool scope_capacity = (!global_serial_gate || state_->ordinary_in_flight == 0) &&
                (!shared_serial_gate || state_->shared_cooldowns.at(shared).in_flight == 0);

            if (!blocked_until && capacity && scope_capacity) {
                const auto sequence = state_->next_sequence++;
                const auto reservation_id = state_->next_reservation++;
                detail::SharedCooldown* shared_state{};
                if (!shared.empty()) shared_state = &state_->shared_cooldowns[shared];
                state_->reservations.emplace(reservation_id, detail::Reservation{
                    bucket, route, provisional, majors, sequence, false, bucket_probe, global_probe, shared_probe,
                    route.global_class == GlobalLimitClass::ordinary, !shared.empty() });
                if (global_probe) { state_->global_probe_available = false; state_->global_probe_in_flight = true; }
                if (route.global_class == GlobalLimitClass::ordinary) ++state_->ordinary_in_flight;
                if (shared_probe) {
                    shared_state->probe_available = false;
                    shared_state->probe_in_flight = true;
                }
                if (shared_state) ++shared_state->in_flight;
                if (bucket_probe) {
                    bucket->probe_available = false;
                    ++bucket->probe_in_flight;
                } else if (bucket->remaining) --*bucket->remaining;
                ++bucket->in_flight;
                state_->prune_expired(now);
                return Result<RateLimitPermit, RateLimitAcquireError>::success(RateLimitPermit{ state_, reservation_id });
            }

            if (blocked_until && (!wake_at || *blocked_until < *wake_at)) wake_at = blocked_until;
            if (bucket->remaining && *bucket->remaining == 0 && bucket->reset_at && (!wake_at || *bucket->reset_at < *wake_at))
                wake_at = bucket->reset_at;
            observed_generation = state_->scheduler.generation();
        }
        state_->scheduler.wait_for_change(observed_generation, wake_at, cancellation);
    }
}

Result<void, RateLimitMetadataError> RateLimitCoordinator::authorize_single_probe(const RouteDescriptor& route) {
    std::lock_guard lock{ state_->mutex };
    const auto provisional = provisional_key(route);
    const auto bucket_entry = state_->provisional_buckets.find(provisional);
    auto bucket = bucket_entry == state_->provisional_buckets.end() ? std::shared_ptr<detail::BucketState>{}
        : state_->root(bucket_entry->second);
    const auto shared = route.shared_resource
        ? std::to_string(static_cast<int>(route.shared_resource->kind)) + ':' + encoded(route.shared_resource->id) +
            std::to_string(route.shared_resource->opaque_discriminator)
        : std::string{};
    bool authorized{};
    if (route.global_class == GlobalLimitClass::ordinary && state_->global_metadata_blocked && !state_->global_probe_in_flight) {
        state_->global_probe_available = true;
        authorized = true;
    }
    if (!shared.empty()) {
        if (auto found = state_->shared_cooldowns.find(shared); found != state_->shared_cooldowns.end() &&
            found->second.metadata_blocked && !found->second.probe_in_flight) {
            found->second.probe_available = true;
            authorized = true;
        }
    }
    if (bucket && bucket->metadata_blocked && bucket->probe_in_flight == 0) {
        bucket->probe_available = true;
        authorized = true;
    }
    if (!authorized) return Result<void, RateLimitMetadataError>::failure({
        RateLimitMetadataIssue::no_recoverable_metadata_block, RateLimitMetadataScope::route_bucket,
        "route has no metadata-blocked scope available for probing", std::nullopt });
    state_->scheduler.notify_all();
    return Result<void, RateLimitMetadataError>::success();
}

std::optional<RateLimitMetadataError> RateLimitCoordinator::observe_response(
    RateLimitPermit&& permit, const HttpResponse& response) {
    auto state = permit.state_.lock();
    if (!state || permit.reservation_id_ == 0) {
        permit.reservation_id_ = 0;
        permit.state_.reset();
        return std::nullopt;
    }
    const auto id = permit.reservation_id_;

    const auto limit = header(response.headers, "X-RateLimit-Limit");
    const auto remaining = header(response.headers, "X-RateLimit-Remaining");
    const auto bucket_header = header(response.headers, "X-RateLimit-Bucket");
    const auto reset_header = header(response.headers, "X-RateLimit-Reset-After");
    const auto scope_header = header(response.headers, "X-RateLimit-Scope");
    const auto global_header = header(response.headers, "X-RateLimit-Global");
    auto parsed_limit = limit.value ? parse_unsigned(*limit.value) : std::nullopt;
    auto parsed_remaining = remaining.value ? parse_unsigned(*remaining.value) : std::nullopt;
    if (parsed_limit && parsed_remaining && *parsed_remaining > *parsed_limit) parsed_remaining = parsed_limit;
    auto parsed_reset = reset_header.value ? parse_decimal_seconds(*reset_header.value) : std::nullopt;
    auto body_fields = response.status == 429 ? parse_rate_body(response.body) : std::nullopt;
    std::optional<Nanoseconds> retry_after = body_fields ? body_fields->retry_after : std::nullopt;
    if (!retry_after) {
        const auto retry_header = header(response.headers, "Retry-After");
        if (retry_header.value) retry_after = parse_decimal_seconds(*retry_header.value);
    }
    if (!retry_after) retry_after = parsed_reset;
    const auto scope = scope_header.value ? lower_ascii(*scope_header.value) : std::string{};
    const bool global_signal = (!global_header.conflicting && parse_bool_header(global_header.value)) ||
        (body_fields && body_fields->global.value_or(false));
    const auto observed_at = state->scheduler.now();

    std::lock_guard lock{ state->mutex };
    const auto reservation_it = state->reservations.find(id);
    if (reservation_it == state->reservations.end()) {
        permit.reservation_id_ = 0;
        permit.state_.reset();
        return std::nullopt;
    }
    auto reservation = std::move(reservation_it->second);
    state->reservations.erase(reservation_it);
    permit.reservation_id_ = 0;
    permit.state_.reset();
    auto bucket = state->root(reservation.bucket);
    if (bucket && bucket->in_flight != 0) --bucket->in_flight;

    if (bucket && reservation.bucket_probe && bucket->probe_in_flight != 0) --bucket->probe_in_flight;
    if (reservation.global_probe) state->global_probe_in_flight = false;
    if (reservation.ordinary_counted && state->ordinary_in_flight != 0) --state->ordinary_in_flight;
    const auto shared_key = reservation.route.shared_resource
        ? std::to_string(static_cast<int>(reservation.route.shared_resource->kind)) + ':' + encoded(reservation.route.shared_resource->id) +
            std::to_string(reservation.route.shared_resource->opaque_discriminator)
        : std::string{};
    if (reservation.shared_counted) {
        if (const auto shared = state->shared_cooldowns.find(shared_key); shared != state->shared_cooldowns.end()) {
            if (reservation.shared_probe) shared->second.probe_in_flight = false;
            if (shared->second.in_flight != 0) --shared->second.in_flight;
        }
    }

    if (bucket_header.value && !bucket_header.value->empty()) {
        const auto binding_sequence = state->route_mapping_sequence[reservation.provisional];
        if (reservation.sequence >= binding_sequence) {
            const auto key = canonical_key(*bucket_header.value, reservation.majors);
            auto& canonical = state->canonical_buckets[key];
            if (!canonical) canonical = bucket;
            else canonical = state->merge(canonical, bucket);
            auto route_binding = state->provisional_buckets.find(reservation.provisional);
            if (route_binding != state->provisional_buckets.end()) route_binding->second = state->merge(route_binding->second, canonical);
            state->route_mapping_sequence[reservation.provisional] = reservation.sequence;
            bucket = state->root(canonical);
        }
    }
    bucket = state->root(bucket);
    if (!bucket) { state->scheduler.notify_all(); return std::nullopt; }

    std::optional<RateLimitMetadataError> metadata_error;
    const auto set_metadata_error = [&](RateLimitMetadataIssue issue, RateLimitMetadataScope error_scope, std::string message) {
        metadata_error = RateLimitMetadataError{ issue, error_scope, std::move(message), std::nullopt };
    };
    const bool stale_bucket_response = reservation.sequence < bucket->latest_observation;
    const bool response_has_independent_scope = response.status == 429 && (scope == "global" || scope == "shared");
    if (!response_has_independent_scope && !scope_header.conflicting && !stale_bucket_response) {
        bucket->latest_observation = reservation.sequence;
        const bool previous_window_active = bucket->reset_at && observed_at < *bucket->reset_at;
        if (parsed_limit) bucket->limit = parsed_limit;
        if (parsed_reset) {
            const auto duration = std::chrono::duration_cast<SteadyClock::duration>(*parsed_reset);
            bucket->window_duration = duration;
            bucket->reset_at = add_saturated(observed_at, *parsed_reset);
        }
        if (parsed_remaining) {
            const auto conservative = detail::RateLimitState::subtract_saturating(*parsed_remaining, bucket->in_flight);
            if (!bucket->remaining || !previous_window_active)
                bucket->remaining = conservative;
            else bucket->remaining = std::min(*bucket->remaining, conservative);
        }
    }

    const bool global_scope = scope == "global" || (scope.empty() && global_signal);
    const bool shared_scope = scope == "shared";
    if (response.status == 429 && scope_header.conflicting) {
        if (reservation.sequence >= bucket->latest_cooldown_sequence) {
            bucket->latest_cooldown_sequence = reservation.sequence;
            bucket->metadata_blocked = true;
            bucket->probe_available = false;
            bucket->metadata_issue = RateLimitMetadataIssue::ambiguous_scope_headers;
        }
        set_metadata_error(RateLimitMetadataIssue::ambiguous_scope_headers, RateLimitMetadataScope::route_bucket,
            "conflicting X-RateLimit-Scope fields make the 429 scope ambiguous");
    } else if (response.status == 429 && global_header.conflicting && scope.empty() && !global_signal) {
        if (reservation.sequence >= bucket->latest_cooldown_sequence) {
            bucket->latest_cooldown_sequence = reservation.sequence;
            bucket->metadata_blocked = true;
            bucket->probe_available = false;
            bucket->metadata_issue = RateLimitMetadataIssue::ambiguous_scope_headers;
        }
        set_metadata_error(RateLimitMetadataIssue::ambiguous_scope_headers, RateLimitMetadataScope::route_bucket,
            "conflicting X-RateLimit-Global fields make the 429 scope ambiguous");
    } else if (response.status == 429 && shared_scope && !reservation.route.shared_resource) {
        if (reservation.sequence >= bucket->latest_cooldown_sequence) {
            bucket->latest_cooldown_sequence = reservation.sequence;
            bucket->metadata_blocked = true;
            bucket->probe_available = false;
            bucket->metadata_issue = RateLimitMetadataIssue::shared_resource_required;
        }
        set_metadata_error(RateLimitMetadataIssue::shared_resource_required, RateLimitMetadataScope::shared_resource,
            "shared-scope 429 requires an explicit shared-resource key");
    } else if (response.status == 429 && global_scope) {
        if (reservation.sequence >= state->global_latest_sequence) {
            state->global_latest_sequence = reservation.sequence;
            state->global_unknown_serial = false;
            state->global_metadata_blocked = !retry_after.has_value();
            state->global_probe_available = false;
            if (retry_after) {
                const auto until = add_saturated(observed_at, *retry_after);
                if (!state->global_cooldown || until > *state->global_cooldown) state->global_cooldown = until;
            } else {
                state->global_metadata_issue = RateLimitMetadataIssue::invalid_retry_timing;
                set_metadata_error(RateLimitMetadataIssue::invalid_retry_timing, RateLimitMetadataScope::global,
                    "global 429 has no valid retry timing; explicitly authorize one probe after review");
            }
        } else if (!retry_after) {
            set_metadata_error(RateLimitMetadataIssue::invalid_retry_timing, RateLimitMetadataScope::global,
                "stale global 429 has no valid retry timing");
        }
    } else if (response.status == 429 && shared_scope) {
        auto& cooldown = state->shared_cooldowns[shared_key];
        if (reservation.sequence >= cooldown.latest_sequence) {
            cooldown.latest_sequence = reservation.sequence;
            cooldown.unknown_serial = false;
            cooldown.metadata_blocked = !retry_after.has_value();
            cooldown.probe_available = false;
            if (retry_after) {
                const auto until = add_saturated(observed_at, *retry_after);
                if (!cooldown.until || until > *cooldown.until) cooldown.until = until;
            } else {
                cooldown.metadata_issue = RateLimitMetadataIssue::invalid_retry_timing;
                set_metadata_error(RateLimitMetadataIssue::invalid_retry_timing, RateLimitMetadataScope::shared_resource,
                    "shared-scope 429 has no valid retry timing; explicitly authorize one probe after review");
            }
        } else if (!retry_after) {
            set_metadata_error(RateLimitMetadataIssue::invalid_retry_timing, RateLimitMetadataScope::shared_resource,
                "stale shared 429 has no valid retry timing");
        }
    } else if (response.status == 429) {
        if (reservation.sequence >= bucket->latest_cooldown_sequence) {
            bucket->latest_cooldown_sequence = reservation.sequence;
            bucket->metadata_blocked = !retry_after.has_value();
            bucket->probe_available = false;
            if (retry_after) {
                const auto until = add_saturated(observed_at, *retry_after);
                if (!bucket->cooldown_until || until > *bucket->cooldown_until) bucket->cooldown_until = until;
                if (!bucket->reset_at) bucket->reset_at = until;
                if (bucket->metadata_issue == RateLimitMetadataIssue::shared_resource_required && shared_scope && reservation.route.shared_resource)
                    bucket->metadata_blocked = false;
            } else {
                bucket->metadata_issue = RateLimitMetadataIssue::invalid_retry_timing;
                set_metadata_error(RateLimitMetadataIssue::invalid_retry_timing, RateLimitMetadataScope::route_bucket,
                    "route-bucket 429 has no valid retry timing; explicitly authorize one probe after review");
            }
        } else if (!retry_after) {
            set_metadata_error(RateLimitMetadataIssue::invalid_retry_timing, RateLimitMetadataScope::route_bucket,
                "stale route-bucket 429 has no valid retry timing");
        }
    }

    // An explicitly authorized probe can resolve an unknown scope only when a
    // response demonstrates progress. A bucket still reporting zero remaining
    // without reset metadata stays blocked and requires another explicit probe.
    // Only a successful final response demonstrates that an explicitly
    // authorized global probe got through. Rate-limit headers are optional:
    // without them the scope remains conservatively serialized below.
    const bool successful_final_response = response.status >= 200 && response.status <= 299;
    if (reservation.sequence >= state->global_latest_sequence && reservation.global_probe && successful_final_response) {
        state->global_metadata_blocked = false;
        state->global_probe_available = false;
        state->global_unknown_serial = true;
        state->global_latest_sequence = reservation.sequence;
    }
    if (reservation.shared_probe && !shared_key.empty()) {
        auto found = state->shared_cooldowns.find(shared_key);
        if (found != state->shared_cooldowns.end() && reservation.sequence >= found->second.latest_sequence && successful_final_response) {
            found->second.metadata_blocked = false;
            found->second.probe_available = false;
            found->second.unknown_serial = true;
            found->second.latest_sequence = reservation.sequence;
        }
    }
    if (reservation.bucket_probe && reservation.sequence >= bucket->latest_cooldown_sequence) {
        const bool resolved = (response.status == 429 && retry_after.has_value() && !shared_scope && !global_scope) ||
            (response.status != 429 && (parsed_reset.has_value() || (parsed_remaining && *parsed_remaining > 0)));
        if (resolved) {
            bucket->metadata_blocked = false;
            bucket->probe_available = false;
        } else if (bucket->metadata_issue == RateLimitMetadataIssue::shared_resource_required &&
            response.status == 429 && shared_scope && reservation.route.shared_resource && retry_after) {
            bucket->metadata_blocked = false;
            bucket->probe_available = false;
        }
        bucket->latest_cooldown_sequence = reservation.sequence;
    }

    if (!response_has_independent_scope && response.status != 429 && !scope_header.conflicting && !stale_bucket_response) {
        const bool exhausted_without_reset = bucket->remaining && *bucket->remaining == 0 && !parsed_reset &&
            !bucket->reset_at && !bucket->cooldown_until;
        if (exhausted_without_reset) {
            bucket->metadata_blocked = true;
            bucket->probe_available = false;
            bucket->metadata_issue = RateLimitMetadataIssue::exhausted_bucket_without_reset;
            bucket->latest_cooldown_sequence = reservation.sequence;
        }
    }

    state->scheduler.notify_all();
    return metadata_error;
}

void RateLimitCoordinator::observe_transport_error(RateLimitPermit&& permit, const TransportError& error) noexcept {
    auto state = permit.state_.lock();
    if (state && permit.reservation_id_ != 0) {
        const auto id = std::exchange(permit.reservation_id_, 0);
        permit.state_.reset();
        state->release(id, !error.request_may_have_reached_server);
    }
}

}
