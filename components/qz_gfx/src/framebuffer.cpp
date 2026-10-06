// Framebuffer: bit-level access to the 1-bpp frame (framebuffer.hpp). Integer-only.
//
// Layout: row-major, 25 bytes per row, MSB = leftmost pixel, bit 1 = black ink. 200 is a multiple
// of 8, so rows have no padding bits.
#include "qz/gfx/framebuffer.hpp"

#include "qz/core/crc32.hpp"

#include <cstddef>
#include <cstdint>

namespace qz::gfx {
namespace {

constexpr std::uint8_t kInkByte = 0xFF;   // eight black pixels
constexpr std::uint8_t kPaperByte = 0x00; // eight white pixels

[[nodiscard]] bool in_frame(std::int16_t x, std::int16_t y) noexcept {
    return x >= 0 && x < kWidth && y >= 0 && y < kHeight;
}

/// Byte holding pixel (x, y). Precondition: in_frame(x, y).
[[nodiscard]] std::size_t byte_index(std::int16_t x, std::int16_t y) noexcept {
    return (static_cast<std::size_t>(y) * kStride) + (static_cast<std::size_t>(x) >> 3U);
}

/// Bit of pixel x inside its byte (MSB = leftmost). Precondition: x >= 0.
[[nodiscard]] std::uint8_t bit_mask(std::int16_t x) noexcept {
    return static_cast<std::uint8_t>(0x80U >> (static_cast<unsigned>(x) & 7U));
}

} // namespace

void Framebuffer::clear(Color c) noexcept {
    bits.fill(c == Color::kBlack ? kInkByte : kPaperByte);
}

Color Framebuffer::get(std::int16_t x, std::int16_t y) const noexcept {
    if (!in_frame(x, y)) {
        return Color::kWhite;
    }
    return (bits[byte_index(x, y)] & bit_mask(x)) != 0U ? Color::kBlack : Color::kWhite;
}

void Framebuffer::set(std::int16_t x, std::int16_t y, Color c) noexcept {
    if (!in_frame(x, y)) {
        return;
    }
    std::uint8_t& byte = bits[byte_index(x, y)];
    const std::uint8_t mask = bit_mask(x);
    if (c == Color::kBlack) {
        byte = static_cast<std::uint8_t>(byte | mask);
    } else {
        byte = static_cast<std::uint8_t>(byte & static_cast<std::uint8_t>(~mask));
    }
}

std::uint32_t Framebuffer::crc32() const noexcept {
    return qz::crc32(bytes());
}

} // namespace qz::gfx
