#include <discordcpp/query_parameters.hpp>

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

namespace discordcpp {
namespace {

LocalValidationError query_error(std::string message) {
    return { LocalValidationIssue::invalid_query_parameter, std::move(message) };
}

bool valid_utf8(std::string_view input) noexcept {
    std::size_t position{};
    while (position < input.size()) {
        const auto lead = static_cast<unsigned char>(input[position]);
        if (lead <= 0x7f) {
            ++position;
            continue;
        }

        if (lead >= 0xc2 && lead <= 0xdf) {
            if (input.size() - position < 2) return false;
            const auto second = static_cast<unsigned char>(input[position + 1]);
            if (second < 0x80 || second > 0xbf) return false;
            position += 2;
            continue;
        }

        if (lead >= 0xe0 && lead <= 0xef) {
            if (input.size() - position < 3) return false;
            const auto second = static_cast<unsigned char>(input[position + 1]);
            const auto third = static_cast<unsigned char>(input[position + 2]);
            if (third < 0x80 || third > 0xbf) return false;
            if (lead == 0xe0) {
                if (second < 0xa0 || second > 0xbf) return false;
            } else if (lead == 0xed) {
                if (second < 0x80 || second > 0x9f) return false;
            } else if (second < 0x80 || second > 0xbf) {
                return false;
            }
            position += 3;
            continue;
        }

        if (lead >= 0xf0 && lead <= 0xf4) {
            if (input.size() - position < 4) return false;
            const auto second = static_cast<unsigned char>(input[position + 1]);
            const auto third = static_cast<unsigned char>(input[position + 2]);
            const auto fourth = static_cast<unsigned char>(input[position + 3]);
            if (third < 0x80 || third > 0xbf || fourth < 0x80 || fourth > 0xbf) return false;
            if (lead == 0xf0) {
                if (second < 0x90 || second > 0xbf) return false;
            } else if (lead == 0xf4) {
                if (second < 0x80 || second > 0x8f) return false;
            } else if (second < 0x80 || second > 0xbf) {
                return false;
            }
            position += 4;
            continue;
        }
        return false;
    }
    return true;
}

bool unreserved(unsigned char byte) noexcept {
    return (byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') ||
        (byte >= '0' && byte <= '9') || byte == '-' || byte == '.' || byte == '_' || byte == '~';
}

void append_encoded(std::string& output, std::string_view input) {
    constexpr char hex[] = "0123456789ABCDEF";
    for (const unsigned char byte : input) {
        if (unreserved(byte)) {
            output.push_back(static_cast<char>(byte));
        } else {
            output.push_back('%');
            output.push_back(hex[byte >> 4]);
            output.push_back(hex[byte & 0x0f]);
        }
    }
}

} // namespace

Result<void, LocalValidationError> QueryParameters::validate_name(std::string_view name) {
    if (name.empty()) {
        return Result<void, LocalValidationError>::failure(query_error("query parameter name must not be empty"));
    }
    if (!valid_utf8(name)) {
        return Result<void, LocalValidationError>::failure(query_error("query parameter name must be valid UTF-8"));
    }
    return Result<void, LocalValidationError>::success();
}

Result<void, LocalValidationError> QueryParameters::validate_value(std::string_view value) {
    if (!valid_utf8(value)) {
        return Result<void, LocalValidationError>::failure(query_error("query parameter value must be valid UTF-8"));
    }
    return Result<void, LocalValidationError>::success();
}

Result<void, LocalValidationError> QueryParameters::add(std::string_view name, std::string_view value,
    EmptyQueryValuePolicy empty_policy) {
    auto valid_name = validate_name(name);
    if (!valid_name.has_value()) return valid_name;
    if (value.empty() && empty_policy != EmptyQueryValuePolicy::allow) {
        return Result<void, LocalValidationError>::failure(query_error(
            "empty query parameter values require EmptyQueryValuePolicy::allow"));
    }
    auto valid_value = validate_value(value);
    if (!valid_value.has_value()) return valid_value;
    entries_.push_back({ std::string{ name }, std::string{ value } });
    return Result<void, LocalValidationError>::success();
}

Result<void, LocalValidationError> QueryParameters::add(std::string_view name, const char* value,
    EmptyQueryValuePolicy empty_policy) {
    if (value == nullptr) {
        return Result<void, LocalValidationError>::failure(query_error("query parameter value pointer must not be null"));
    }
    return add(name, std::string_view{ value }, empty_policy);
}

Result<void, LocalValidationError> QueryParameters::add(std::string_view name, bool value) {
    return add_integer(name, value ? "true" : "false");
}

Result<void, LocalValidationError> QueryParameters::add_optional(std::string_view name,
    const std::optional<std::string_view>& value, EmptyQueryValuePolicy empty_policy) {
    auto valid_name = validate_name(name);
    if (!valid_name.has_value()) return valid_name;
    if (!value) return Result<void, LocalValidationError>::success();
    return add(name, *value, empty_policy);
}

Result<void, LocalValidationError> QueryParameters::add_optional(std::string_view name,
    const std::optional<std::string>& value, EmptyQueryValuePolicy empty_policy) {
    auto valid_name = validate_name(name);
    if (!valid_name.has_value()) return valid_name;
    if (!value) return Result<void, LocalValidationError>::success();
    return add(name, *value, empty_policy);
}

std::string QueryParameters::serialize() const {
    std::string output;
    for (const auto& entry : entries_) {
        if (!output.empty()) output.push_back('&');
        append_encoded(output, entry.name);
        output.push_back('=');
        append_encoded(output, entry.value);
    }
    return output;
}

Result<void, LocalValidationError> QueryParameters::add_integer(std::string_view name, std::string value) {
    auto valid_name = validate_name(name);
    if (!valid_name.has_value()) return valid_name;
    entries_.push_back({ std::string{ name }, std::move(value) });
    return Result<void, LocalValidationError>::success();
}

} // namespace discordcpp
