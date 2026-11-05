#pragma once

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <initializer_list>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace discordcpp {

/// A sequence of HTTP fields. Names use ASCII case-insensitive comparison and
/// duplicate fields are preserved. Names must be non-empty HTTP tokens; values
/// containing CR or LF are rejected to prevent header-line injection.
class Headers final {
public:
    using value_type = std::pair<std::string, std::string>;
    using const_iterator = std::vector<value_type>::const_iterator;

    Headers() = default;

    Headers(std::initializer_list<value_type> fields) {
        for (const auto& field : fields) {
            add(field.first, field.second);
        }
    }

    /// Append a field. Throws std::invalid_argument for an invalid name or value.
    void add(std::string name, std::string value) {
        validate_field(name, value);
        fields_.emplace_back(std::move(name), std::move(value));
    }

    /// Replace every case-insensitive match with one field at the end.
    /// Provides the strong exception guarantee: on failure, fields_ is unchanged.
    /// Throws std::invalid_argument for an invalid name or value.
    void set(std::string name, std::string value) {
        validate_field(name, value);
        auto replacement = fields_;
        std::erase_if(replacement, [&name](const value_type& field) {
            return names_equal(field.first, name);
        });
        replacement.emplace_back(std::move(name), std::move(value));
        fields_.swap(replacement);
    }

    /// Return the first matching value, or nullopt when absent. The returned
    /// string_view refers to this Headers object and is invalidated if this
    /// object is modified or destroyed.
    [[nodiscard]] std::optional<std::string_view> find(std::string_view name) const noexcept {
        const auto found = std::find_if(fields_.begin(), fields_.end(), [name](const value_type& field) {
            return names_equal(field.first, name);
        });
        if (found == fields_.end()) {
            return std::nullopt;
        }
        return std::string_view{ found->second };
    }

    /// Return all matching values in insertion order. Views remain valid until
    /// this Headers object is modified or destroyed.
    [[nodiscard]] std::vector<std::string_view> find_all(std::string_view name) const {
        std::vector<std::string_view> matches;
        for (const auto& field : fields_) {
            if (names_equal(field.first, name)) {
                matches.emplace_back(field.second);
            }
        }
        return matches;
    }

    [[nodiscard]] bool contains(std::string_view name) const noexcept {
        return find(name).has_value();
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return fields_.size();
    }

    [[nodiscard]] bool empty() const noexcept {
        return fields_.empty();
    }

    [[nodiscard]] const_iterator begin() const noexcept {
        return fields_.begin();
    }

    [[nodiscard]] const_iterator end() const noexcept {
        return fields_.end();
    }

private:
    static constexpr unsigned char ascii_lower(unsigned char character) noexcept {
        if (character >= static_cast<unsigned char>('A') && character <= static_cast<unsigned char>('Z')) {
            return static_cast<unsigned char>(character + ('a' - 'A'));
        }
        return character;
    }

    static constexpr bool is_ascii_alpha(unsigned char character) noexcept {
        return (character >= static_cast<unsigned char>('A') && character <= static_cast<unsigned char>('Z')) ||
            (character >= static_cast<unsigned char>('a') && character <= static_cast<unsigned char>('z'));
    }

    static constexpr bool is_ascii_digit(unsigned char character) noexcept {
        return character >= static_cast<unsigned char>('0') && character <= static_cast<unsigned char>('9');
    }

    static constexpr bool is_token_punctuation(unsigned char character) noexcept {
        switch (character) {
            case '!': case '#': case '$': case '%': case '&': case '\'': case '*': case '+':
            case '-': case '.': case '^': case '_': case '`': case '|': case '~':
                return true;
            default:
                return false;
        }
    }

    static bool valid_name(std::string_view name) noexcept {
        if (name.empty()) {
            return false;
        }
        return std::all_of(name.begin(), name.end(), [](char character) {
            const auto byte = static_cast<unsigned char>(character);
            return is_ascii_alpha(byte) || is_ascii_digit(byte) || is_token_punctuation(byte);
        });
    }

    static bool valid_value(std::string_view value) noexcept {
        return value.find('\r') == std::string_view::npos && value.find('\n') == std::string_view::npos;
    }

    static void validate_field(std::string_view name, std::string_view value) {
        if (!valid_name(name)) {
            throw std::invalid_argument("HTTP header name must be a non-empty token");
        }
        if (!valid_value(value)) {
            throw std::invalid_argument("HTTP header value must not contain CR or LF");
        }
    }

    static bool names_equal(std::string_view left, std::string_view right) noexcept {
        if (left.size() != right.size()) {
            return false;
        }
        for (std::size_t index = 0; index < left.size(); ++index) {
            if (ascii_lower(static_cast<unsigned char>(left[index])) !=
                ascii_lower(static_cast<unsigned char>(right[index]))) {
                return false;
            }
        }
        return true;
    }

    std::vector<value_type> fields_;
};

using SteadyClock = std::chrono::steady_clock;

/// An HTTP request description independent of a concrete transport.
struct HttpRequest {
    std::string method;
    /// Origin-form path and optional query, e.g. "/channels/123/messages";
    /// absolute URLs are not valid targets.
    std::string target;
    Headers headers;
    std::string body;
    /// Absolute monotonic deadline for the whole request lifecycle, including
    /// any future scheduling/rate-limit wait and transport execution. No value
    /// means the request has no deadline.
    std::optional<SteadyClock::time_point> deadline;
};

/// An HTTP response description independent of a concrete transport.
struct HttpResponse {
    int status{};
    Headers headers;
    std::string body;
};

} // namespace discordcpp
