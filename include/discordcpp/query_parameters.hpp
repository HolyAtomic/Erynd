#pragma once

#include <discordcpp/discord_client_error.hpp>
#include <discordcpp/result.hpp>

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace discordcpp {

/// Controls whether a string query value may be explicitly empty. An absent
/// optional parameter is omitted independently of this setting.
enum class EmptyQueryValuePolicy {
    reject,
    allow
};

/// Ordered query parameters with strict UTF-8 validation and RFC 3986
/// percent-encoding. Repeated names are preserved in insertion order. Names
/// and values preserve ASCII unreserved bytes and encode every other UTF-8 byte
/// as uppercase `%HH`; spaces use `%20`, never `+`. Query delimiters are encoded
/// inside components, so values cannot add parameters or alter the path/host.
class QueryParameters final {
public:
    QueryParameters() = default;

    /// Add a string value. Empty values are rejected unless explicitly allowed.
    [[nodiscard]] Result<void, LocalValidationError> add(std::string_view name,
        std::string_view value, EmptyQueryValuePolicy empty_policy = EmptyQueryValuePolicy::reject);
    [[nodiscard]] Result<void, LocalValidationError> add(std::string_view name, const char* value,
        EmptyQueryValuePolicy empty_policy = EmptyQueryValuePolicy::reject);

    /// Add a boolean using lowercase `true` or `false`.
    [[nodiscard]] Result<void, LocalValidationError> add(std::string_view name, bool value);

    /// Add an integer using locale-independent base-10 formatting.
    template<std::integral T>
        requires (!std::same_as<std::remove_cv_t<T>, bool>)
    [[nodiscard]] Result<void, LocalValidationError> add(std::string_view name, T value) {
        return add_integer(name, std::to_string(value));
    }

    /// An empty optional is omitted after validating its name. A present empty
    /// string follows empty_policy and is distinct from an absent value.
    [[nodiscard]] Result<void, LocalValidationError> add_optional(std::string_view name,
        const std::optional<std::string_view>& value,
        EmptyQueryValuePolicy empty_policy = EmptyQueryValuePolicy::reject);
    [[nodiscard]] Result<void, LocalValidationError> add_optional(std::string_view name,
        const std::optional<std::string>& value,
        EmptyQueryValuePolicy empty_policy = EmptyQueryValuePolicy::reject);

    template<std::integral T>
    [[nodiscard]] Result<void, LocalValidationError> add_optional(std::string_view name,
        const std::optional<T>& value) {
        if (!value) return validate_name(name);
        return add(name, *value);
    }

    /// Serialize in insertion order as `name=value` pairs joined with `&`.
    /// The empty collection serializes to an empty string.
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }
    [[nodiscard]] bool empty() const noexcept { return entries_.empty(); }

private:
    struct Entry final {
        std::string name;
        std::string value;
    };

    [[nodiscard]] static Result<void, LocalValidationError> validate_name(std::string_view name);
    [[nodiscard]] static Result<void, LocalValidationError> validate_value(std::string_view value);
    [[nodiscard]] Result<void, LocalValidationError> add_integer(std::string_view name, std::string value);

    std::vector<Entry> entries_;
};

} // namespace discordcpp
