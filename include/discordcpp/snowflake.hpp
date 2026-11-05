#pragma once

#include <discordcpp/result.hpp>

#include <compare>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

namespace discordcpp {

enum class SnowflakeParseIssue {
    empty_input,
    non_decimal_character,
    overflow
};

struct SnowflakeParseError final {
    SnowflakeParseIssue issue{};
    std::size_t position{};
};

/// Discord's unsigned 64-bit identifier. HTTP API snowflakes are read from and
/// written to decimal strings, never JSON numeric values.
class Snowflake final {
public:
    constexpr explicit Snowflake(std::uint64_t value) noexcept : value_{ value } {}

    [[nodiscard]] static Result<Snowflake, SnowflakeParseError> parse(std::string_view text) {
        if (text.empty()) {
            return Result<Snowflake, SnowflakeParseError>::failure(
                { SnowflakeParseIssue::empty_input, 0 });
        }

        std::uint64_t value{};
        for (std::size_t position = 0; position < text.size(); ++position) {
            const auto character = static_cast<unsigned char>(text[position]);
            if (character < static_cast<unsigned char>('0') || character > static_cast<unsigned char>('9')) {
                return Result<Snowflake, SnowflakeParseError>::failure(
                    { SnowflakeParseIssue::non_decimal_character, position });
            }

            const auto digit = static_cast<std::uint64_t>(character - static_cast<unsigned char>('0'));
            if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 10) {
                return Result<Snowflake, SnowflakeParseError>::failure(
                    { SnowflakeParseIssue::overflow, position });
            }
            value = value * 10 + digit;
        }

        return Result<Snowflake, SnowflakeParseError>::success(Snowflake{ value });
    }

    [[nodiscard]] constexpr std::uint64_t value() const noexcept { return value_; }

    /// Serialize canonically in base ten. Leading zeroes in input are accepted
    /// and omitted here, except that zero is serialized as "0".
    [[nodiscard]] std::string to_string() const { return std::to_string(value_); }

    friend constexpr auto operator<=>(const Snowflake&, const Snowflake&) noexcept = default;

private:
    std::uint64_t value_{};
};

} // namespace discordcpp
