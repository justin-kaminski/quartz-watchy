// CRC-32 (IEEE 802.3, reflected, polynomial 0xEDB88320), zlib/PNG semantics.
#include "qz/core/crc32.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace qz {
namespace {

constexpr std::uint32_t kReflectedPolynomial = 0xEDB88320U;
constexpr std::size_t kTableSize = 256;
constexpr unsigned kBitsPerByte = 8;

/// One entry per byte value, computed at compile time (1 KiB of flash, no startup cost).
constexpr std::array<std::uint32_t, kTableSize> make_table() noexcept {
    std::array<std::uint32_t, kTableSize> table{};
    for (std::uint32_t value = 0; value < kTableSize; ++value) {
        std::uint32_t crc = value;
        for (unsigned bit = 0; bit < kBitsPerByte; ++bit) {
            crc = ((crc & 1U) != 0U) ? ((crc >> 1U) ^ kReflectedPolynomial) : (crc >> 1U);
        }
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        table[value] = crc; // value < kTableSize
    }
    return table;
}

constexpr std::array<std::uint32_t, kTableSize> kTable = make_table();

constexpr std::uint32_t update(std::uint32_t crc, std::span<const std::uint8_t> data) noexcept {
    // zlib keeps the running value inverted between calls, which is what makes
    // crc32(b, crc32(a)) == crc32(a || b) and crc32({}, c) == c.
    std::uint32_t state = ~crc;
    for (const std::uint8_t byte : data) {
        // The index is masked to 0..255, so it is always inside the table.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        state = kTable[(state ^ byte) & 0xFFU] ^ (state >> kBitsPerByte);
    }
    return ~state;
}

// The standard check value, verified at compile time: a wrong table can never be built.
constexpr std::array<std::uint8_t, 9> kCheckInput = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
static_assert(update(0, kCheckInput) == 0xCBF43926U, "CRC-32 table does not match zlib");

} // namespace

std::uint32_t crc32(std::span<const std::uint8_t> data, std::uint32_t crc) noexcept {
    return update(crc, data);
}

} // namespace qz
