// CRC-32 (IEEE 802.3, reflected, poly 0xEDB88320) with zlib/PNG semantics.
#pragma once

#include <cstdint>
#include <span>

namespace qz {

/// zlib-compatible: crc32(b, crc32(a)) == crc32(a || b); crc32({}) == 0.
/// Used for RTC state validation, PNG chunks, framebuffer golden CRCs. Pure, reentrant.
[[nodiscard]] std::uint32_t crc32(std::span<const std::uint8_t> data,
                                  std::uint32_t crc = 0) noexcept;

} // namespace qz
