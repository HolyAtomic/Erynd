#pragma once

#include <optional>
#include <utility>

namespace discordcpp {

/// Presence and nullability of a JSON field. A default-constructed Field is
/// absent; null() represents an explicit JSON null; with_value() represents a
/// non-null value. value() follows std::optional::value() and throws
/// std::bad_optional_access unless has_value() is true.
enum class FieldState {
    absent,
    null,
    value
};

template<typename T>
class Field final {
public:
    Field() = default;

    [[nodiscard]] static Field absent() noexcept { return {}; }

    [[nodiscard]] static Field null() noexcept {
        Field field;
        field.null_ = true;
        return field;
    }

    [[nodiscard]] static Field with_value(T value) {
        Field field;
        field.value_.emplace(std::move(value));
        return field;
    }

    [[nodiscard]] FieldState state() const noexcept {
        if (value_.has_value()) return FieldState::value;
        return null_ ? FieldState::null : FieldState::absent;
    }
    [[nodiscard]] bool is_absent() const noexcept { return state() == FieldState::absent; }
    [[nodiscard]] bool is_null() const noexcept { return state() == FieldState::null; }
    [[nodiscard]] bool has_value() const noexcept { return value_.has_value(); }

    T& value() & { return value_.value(); }
    const T& value() const& { return value_.value(); }
    T&& value() && { return std::move(value_).value(); }

    friend bool operator==(const Field&, const Field&) = default;

private:
    bool null_{};
    std::optional<T> value_;
};

} // namespace discordcpp
