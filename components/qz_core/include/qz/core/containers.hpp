// Fixed-capacity, heap-free containers. Element types must be default-constructible and
// copyable; containers of trivially copyable T are trivially copyable (usable in RTC memory).
#pragma once

#include "qz/core/assert.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace qz {

/// Vector with inline storage of N elements. push_back on a full vector returns false.
/// Not thread-safe.
template<class T, std::size_t N>
class StaticVector {
public:
    [[nodiscard]] constexpr std::size_t size() const noexcept { return size_; }
    [[nodiscard]] static constexpr std::size_t capacity() noexcept { return N; }
    [[nodiscard]] constexpr bool empty() const noexcept { return size_ == 0; }
    [[nodiscard]] constexpr bool full() const noexcept { return size_ == N; }

    [[nodiscard]] constexpr bool push_back(const T& v) noexcept {
        if (size_ == N) {
            return false;
        }
        items_[size_++] = v;
        return true;
    }
    constexpr void pop_back() noexcept {
        QZ_ASSERT(size_ > 0);
        --size_;
    }
    constexpr void clear() noexcept { size_ = 0; }

    constexpr T& operator[](std::size_t i) noexcept {
        QZ_ASSERT(i < size_);
        return items_[i];
    }
    constexpr const T& operator[](std::size_t i) const noexcept {
        QZ_ASSERT(i < size_);
        return items_[i];
    }
    [[nodiscard]] constexpr T* begin() noexcept { return items_.data(); }
    [[nodiscard]] constexpr T* end() noexcept { return items_.data() + size_; }
    [[nodiscard]] constexpr const T* begin() const noexcept { return items_.data(); }
    [[nodiscard]] constexpr const T* end() const noexcept { return items_.data() + size_; }
    [[nodiscard]] constexpr std::span<const T> span() const noexcept {
        return {items_.data(), size_};
    }

private:
    std::array<T, N> items_{};
    std::size_t size_ = 0;
};

/// Ring buffer of the last N items; push overwrites the oldest. Index 0 = oldest.
/// Layout (array + two uint16_t) is stable for RTC memory. Not thread-safe.
template<class T, std::uint16_t N>
class RingBuffer {
    static_assert(N > 0);

public:
    constexpr void push(const T& v) noexcept {
        items_[head_] = v;
        head_ = static_cast<std::uint16_t>((head_ + 1U) % N);
        if (count_ < N) {
            ++count_;
        }
    }
    [[nodiscard]] constexpr std::size_t size() const noexcept { return count_; }
    [[nodiscard]] static constexpr std::size_t capacity() noexcept { return N; }
    [[nodiscard]] constexpr bool empty() const noexcept { return count_ == 0; }
    constexpr void clear() noexcept {
        head_ = 0;
        count_ = 0;
    }
    /// i-th oldest element (0 = oldest), i < size().
    constexpr const T& operator[](std::size_t i) const noexcept {
        QZ_ASSERT(i < count_);
        return items_[(static_cast<std::size_t>(head_) + N - count_ + i) % N];
    }
    [[nodiscard]] constexpr const T& newest() const noexcept { return (*this)[count_ - 1U]; }

private:
    std::array<T, N> items_{};
    std::uint16_t head_ = 0;
    std::uint16_t count_ = 0;
};

} // namespace qz
