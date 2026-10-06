// Heap-free strings: FixedString for ordinary text, Secret for credentials.
#pragma once

#include <array>
#include <cstddef>
#include <string_view>

namespace qz {

/// NUL-terminated string with inline capacity N (excluding NUL). Trivially copyable.
template<std::size_t N>
class FixedString {
public:
    constexpr FixedString() noexcept = default;
    /// Truncation is a programmer error for literals; use assign() for external input.
    constexpr FixedString(const char* literal) noexcept {
        (void)assign(literal);
    } // NOLINT(google-explicit-constructor)

    /// Copies `s`; returns false (and leaves the string empty) if s.size() > N.
    [[nodiscard]] constexpr bool assign(std::string_view s) noexcept {
        if (s.size() > N) {
            clear();
            return false;
        }
        for (std::size_t i = 0; i < s.size(); ++i) {
            buf_[i] = s[i];
        }
        for (std::size_t i = s.size(); i <= N; ++i) {
            buf_[i] = '\0'; // invariant: every byte after the text is zero (no stale tails)
        }
        size_ = s.size();
        return true;
    }
    constexpr void clear() noexcept {
        for (std::size_t i = 0; i <= N; ++i) {
            buf_[i] = '\0';
        }
        size_ = 0;
    }
    [[nodiscard]] constexpr std::string_view view() const noexcept { return {buf_.data(), size_}; }
    [[nodiscard]] constexpr const char* c_str() const noexcept { return buf_.data(); }
    [[nodiscard]] constexpr std::size_t size() const noexcept { return size_; }
    [[nodiscard]] constexpr bool empty() const noexcept { return size_ == 0; }
    [[nodiscard]] static constexpr std::size_t capacity() noexcept { return N; }
    constexpr bool operator==(const FixedString& o) const noexcept { return view() == o.view(); }

private:
    std::array<char, N + 1> buf_{};
    std::size_t size_ = 0;
};

/// Credential storage (Wi-Fi password, AP password). No implicit conversion, no formatting:
/// the only access is reveal(), which must never reach logs or console output. Zeroed on
/// destruction and clear() (volatile writes). Implemented in qz_core src (zeroize).
template<std::size_t N>
class Secret {
public:
    Secret() noexcept = default;
    Secret(const Secret&) noexcept = default;
    Secret& operator=(const Secret&) noexcept = default;
    /// Moves leave the source wiped, so a moved-from credential never lingers in memory.
    Secret(Secret&& other) noexcept : value_(other.value_) { other.wipe(); }
    Secret& operator=(Secret&& other) noexcept {
        if (this != &other) {
            value_ = other.value_;
            other.wipe();
        }
        return *this;
    }
    ~Secret() { wipe(); }

    [[nodiscard]] bool assign(std::string_view s) noexcept { return value_.assign(s); }
    void clear() noexcept { wipe(); }
    [[nodiscard]] bool empty() const noexcept { return value_.empty(); }
    /// Explicit, greppable access. Callers: NetStack, CredentialStore, provisioning only.
    [[nodiscard]] std::string_view reveal() const noexcept { return value_.view(); }

private:
    void wipe() noexcept {
        auto* p = reinterpret_cast<volatile char*>(
            &value_); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
        for (std::size_t i = 0; i < sizeof(value_); ++i) {
            p[i] = 0;
        }
    }
    FixedString<N> value_;
};

} // namespace qz
