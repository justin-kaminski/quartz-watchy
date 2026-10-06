// Error handling as values (ARCHITECTURE.md section 2). No exceptions anywhere.
#pragma once

#include "qz/core/assert.hpp"

#include <cstdint>
#include <expected>
#include <string_view>
#include <utility>

namespace qz {

/// Closed set of error codes. Each maps 1:1 to a console error token (to_token).
/// Adding a code = add the token + a console protocol test.
enum class Errc : std::uint8_t {
    kBadArgs,        ///< "bad_args": invalid argument / out of range / parse failure
    kUnknownCommand, ///< "unknown_cmd"
    kUnsupported,    ///< "unsupported": feature compiled out or not available on this build
    kInvalidState,   ///< "invalid_state": operation not allowed now
    kBusy,           ///< "busy"
    kIo,             ///< "io": bus/flash/peripheral failure (detail = low 16 bits of esp_err_t)
    kTimeout,        ///< "timeout"
    kNotFound,       ///< "not_found"
    kNoTime,         ///< "no_time": requires valid wall-clock time
    kNoCredentials,  ///< "no_creds"
    kBatteryLow,     ///< "battery_low": refused by power policy
    kCorrupt,        ///< "corrupt": CRC/magic/version mismatch
    kNoSpace,        ///< "no_space": buffer or storage too small
    kInternal,       ///< "internal"
};

/// Stable lowercase token for an error code (console protocol v1). Never empty.
[[nodiscard]] std::string_view to_token(Errc code) noexcept;

/// An error value: code plus an optional component-specific detail (e.g. esp_err_t low bits,
/// HTTP status). Trivially copyable, 4 bytes.
struct Error {
    Errc code = Errc::kInternal;
    std::uint16_t detail = 0;

    constexpr Error() noexcept = default;
    constexpr Error(Errc c, std::uint16_t d = 0) noexcept
        : code(c), detail(d) {} // NOLINT(google-explicit-constructor)
    constexpr bool operator==(const Error&) const noexcept = default;
};

/// Value-or-error result. Thin [[nodiscard]] wrapper over std::expected<T, Error> so that
/// ignoring a result is a compile warning (-Werror). Accessing the value of an error result
/// is a programmer error (QZ_ASSERT). Never call std::expected::value() (abort path).
template<class T>
class [[nodiscard]] Result {
public:
    using value_type = T;

    constexpr Result(T value) : v_(std::move(value)) {} // NOLINT(google-explicit-constructor)
    constexpr Result(Error error)
        : v_(std::unexpected(error)) {} // NOLINT(google-explicit-constructor)
    constexpr Result(Errc code)
        : v_(std::unexpected(Error{code})) {} // NOLINT(google-explicit-constructor)

    [[nodiscard]] constexpr bool has_value() const noexcept { return v_.has_value(); }
    constexpr explicit operator bool() const noexcept { return v_.has_value(); }

    constexpr T& operator*() & noexcept {
        QZ_ASSERT(v_.has_value());
        return *v_;
    }
    constexpr const T& operator*() const& noexcept {
        QZ_ASSERT(v_.has_value());
        return *v_;
    }
    constexpr T&& operator*() && noexcept {
        QZ_ASSERT(v_.has_value());
        return std::move(*v_);
    }
    constexpr T* operator->() noexcept {
        QZ_ASSERT(v_.has_value());
        return &*v_;
    }
    constexpr const T* operator->() const noexcept {
        QZ_ASSERT(v_.has_value());
        return &*v_;
    }

    [[nodiscard]] constexpr Error error() const noexcept {
        QZ_ASSERT(!v_.has_value());
        return v_.error();
    }
    [[nodiscard]] constexpr T value_or(T fallback) const& {
        return v_.has_value() ? *v_ : fallback;
    }

private:
    std::expected<T, Error> v_;
};

/// Success-or-error. Default-constructed = success.
template<>
class [[nodiscard]] Result<void> {
public:
    using value_type = void;

    constexpr Result() noexcept = default;
    constexpr Result(Error error) noexcept
        : v_(std::unexpected(error)) {} // NOLINT(google-explicit-constructor)
    constexpr Result(Errc code) noexcept
        : v_(std::unexpected(Error{code})) {} // NOLINT(google-explicit-constructor)

    [[nodiscard]] constexpr bool has_value() const noexcept { return v_.has_value(); }
    constexpr explicit operator bool() const noexcept { return v_.has_value(); }
    [[nodiscard]] constexpr Error error() const noexcept {
        QZ_ASSERT(!v_.has_value());
        return v_.error();
    }

private:
    std::expected<void, Error> v_;
};

using Status = Result<void>;

/// Explicit success value for readability: `return qz::ok();`
[[nodiscard]] constexpr Status ok() noexcept {
    return {};
}

} // namespace qz

/// Propagate an error from any Result/Status expression; continues on success.
#define QZ_RETURN_IF_ERROR(expr)                                                                   \
    do {                                                                                           \
        if (auto qz_status_ = (expr); !qz_status_) {                                               \
            return qz_status_.error();                                                             \
        }                                                                                          \
    } while (false)
