#pragma once

#include <cstddef>
#include <type_traits>
#include <utility>
#include <variant>

namespace discordcpp {

/// The active state of a Result. Assignment may produce valueless when an
/// alternative's construction throws; callers can distinguish that state from
/// an active error with state(), has_error(), and is_valueless().
enum class ResultState {
    success,
    error,
    valueless
};

/// A C++20 result type containing either a value or an error.
///
/// Construct results with success() or failure(). Assignment is supported
/// when the alternatives support it and follows std::variant's exception
/// behavior. If assignment throws while changing alternatives, state() reports
/// valueless. Calling value() or error() without that active alternative throws
/// std::bad_variant_access.
template<typename T, typename E>
class Result final {
    static_assert(!std::is_reference_v<T>, "Result value type cannot be a reference");
    static_assert(!std::is_reference_v<E>, "Result error type cannot be a reference");

public:
    using value_type = T;
    using error_type = E;

    [[nodiscard]] static Result success(T value) {
        return Result(std::in_place_index<0>, std::move(value));
    }

    [[nodiscard]] static Result failure(E error) {
        return Result(std::in_place_index<1>, std::move(error));
    }

    [[nodiscard]] ResultState state() const noexcept {
        if (storage_.valueless_by_exception()) {
            return ResultState::valueless;
        }
        return storage_.index() == 0 ? ResultState::success : ResultState::error;
    }

    [[nodiscard]] bool has_value() const noexcept {
        return state() == ResultState::success;
    }

    [[nodiscard]] bool has_error() const noexcept {
        return state() == ResultState::error;
    }

    [[nodiscard]] bool is_valueless() const noexcept {
        return state() == ResultState::valueless;
    }

    /// True only when the result contains a value. False also covers valueless.
    [[nodiscard]] explicit operator bool() const noexcept {
        return has_value();
    }

    T& value() & {
        return std::get<0>(storage_);
    }

    const T& value() const& {
        return std::get<0>(storage_);
    }

    T&& value() && {
        return std::get<0>(std::move(storage_));
    }

    E& error() & {
        return std::get<1>(storage_);
    }

    const E& error() const& {
        return std::get<1>(storage_);
    }

    E&& error() && {
        return std::get<1>(std::move(storage_));
    }

private:
    template<std::size_t Index, typename U>
    explicit Result(std::in_place_index_t<Index> tag, U&& item)
        : storage_(tag, std::forward<U>(item)) {}

    std::variant<T, E> storage_;
};

/// A result type for operations that succeed without returning a value.
template<typename E>
class Result<void, E> final {
    static_assert(!std::is_reference_v<E>, "Result error type cannot be a reference");

public:
    using value_type = void;
    using error_type = E;

    [[nodiscard]] static Result success() {
        return Result(std::in_place_index<0>);
    }

    [[nodiscard]] static Result failure(E error) {
        return Result(std::in_place_index<1>, std::move(error));
    }

    [[nodiscard]] ResultState state() const noexcept {
        if (storage_.valueless_by_exception()) {
            return ResultState::valueless;
        }
        return storage_.index() == 0 ? ResultState::success : ResultState::error;
    }

    [[nodiscard]] bool has_value() const noexcept {
        return state() == ResultState::success;
    }

    [[nodiscard]] bool has_error() const noexcept {
        return state() == ResultState::error;
    }

    [[nodiscard]] bool is_valueless() const noexcept {
        return state() == ResultState::valueless;
    }

    /// True only when the result succeeded. False also covers valueless.
    [[nodiscard]] explicit operator bool() const noexcept {
        return has_value();
    }

    void value() const {
        (void)std::get<0>(storage_);
    }

    E& error() & {
        return std::get<1>(storage_);
    }

    const E& error() const& {
        return std::get<1>(storage_);
    }

    E&& error() && {
        return std::get<1>(std::move(storage_));
    }

private:
    explicit Result(std::in_place_index_t<0> tag)
        : storage_(tag) {}

    template<typename U>
    explicit Result(std::in_place_index_t<1> tag, U&& error)
        : storage_(tag, std::forward<U>(error)) {}

    std::variant<std::monostate, E> storage_;
};

} // namespace discordcpp
