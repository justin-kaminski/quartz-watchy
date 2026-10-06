// Deterministic PNG encoder (framebuffer.hpp): 1-bit grayscale, one IDAT chunk holding a zlib
// stream of stored (uncompressed) deflate blocks, fixed chunk order IHDR, IDAT, IEND, no
// ancillary chunks, no timestamps. The output is a pure function of the framebuffer bytes, so it
// is identical on every platform and run. Integer-only; streams into the sink through a small
// staging buffer (no heap, ~300 bytes of stack).
//
// Layout of the 5268-byte file:
//   signature (8) | IHDR (4 len + 4 tag + 13 data + 4 crc) | IDAT (4 + 4 + 5211 + 4) | IEND (12)
// IDAT data: zlib header (2) | stored block header (5) | 200 x (filter byte 0 + 25 pixel bytes)
//            | Adler-32 of the 5200 raw bytes (4, big endian).
// PNG grayscale black is 0 while the framebuffer's black ink is 1, so every pixel byte is inverted.
#include "qz/core/crc32.hpp"
#include "qz/core/result.hpp"
#include "qz/gfx/framebuffer.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace qz::gfx {
namespace {

using Bytes = std::span<const std::uint8_t>;

constexpr std::array<std::uint8_t, 8> kSignature = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
constexpr std::array<std::uint8_t, 4> kTagIhdr = {0x49, 0x48, 0x44, 0x52}; // "IHDR"
constexpr std::array<std::uint8_t, 4> kTagIdat = {0x49, 0x44, 0x41, 0x54}; // "IDAT"
constexpr std::array<std::uint8_t, 4> kTagIend = {0x49, 0x45, 0x4E, 0x44}; // "IEND"

constexpr std::size_t kRows = static_cast<std::size_t>(kHeight);
constexpr std::size_t kRowBytes = kStride + 1U; // filter byte + packed pixels
constexpr std::size_t kRawBytes = kRowBytes * kRows;
constexpr std::size_t kIhdrDataBytes = 13;
constexpr std::size_t kZlibHeaderBytes = 2;
constexpr std::size_t kStoredHeaderBytes = 5;
constexpr std::size_t kAdlerBytes = 4;
constexpr std::uint32_t kIdatDataBytes =
    static_cast<std::uint32_t>(kZlibHeaderBytes + kStoredHeaderBytes + kRawBytes + kAdlerBytes);
/// A single stored deflate block holds at most 65535 bytes (16-bit LEN).
static_assert(kRawBytes <= 0xFFFFU, "the image must fit one stored deflate block");

/// zlib header + the header of the one final stored block.
constexpr std::array<std::uint8_t, kZlibHeaderBytes + kStoredHeaderBytes> kStreamHeader = {
    0x78, // CMF: deflate, 32 KiB window
    0x01, // FLG: no dictionary, fastest level; (0x78 * 256 + 0x01) is a multiple of 31
    0x01, // BFINAL = 1, BTYPE = 00 (stored)
    static_cast<std::uint8_t>(kRawBytes & 0xFFU),
    static_cast<std::uint8_t>((kRawBytes >> 8U) & 0xFFU), // LEN, little endian
    static_cast<std::uint8_t>(~kRawBytes & 0xFFU),
    static_cast<std::uint8_t>((~kRawBytes >> 8U) & 0xFFU), // NLEN = one's complement of LEN
};

/// Bytes handed to the sink per write (rows are 26 bytes; a few dozen sink calls per image).
constexpr std::size_t kStageBytes = 256;

[[nodiscard]] std::array<std::uint8_t, 4> be32(std::uint32_t v) noexcept {
    return {static_cast<std::uint8_t>(v >> 24U),
            static_cast<std::uint8_t>((v >> 16U) & 0xFFU),
            static_cast<std::uint8_t>((v >> 8U) & 0xFFU),
            static_cast<std::uint8_t>(v & 0xFFU)};
}

/// Adler-32 (RFC 1950): two running sums modulo 65521, updated lazily.
class Adler32 {
public:
    void update(Bytes bytes) noexcept {
        while (!bytes.empty()) {
            const std::size_t count = std::min(bytes.size(), kBlock);
            for (const std::uint8_t b : bytes.first(count)) {
                sum1_ += b;
                sum2_ += sum1_;
            }
            sum1_ %= kModulus;
            sum2_ %= kModulus;
            bytes = bytes.subspan(count);
        }
    }
    [[nodiscard]] std::uint32_t value() const noexcept { return (sum2_ << 16U) | sum1_; }

private:
    static constexpr std::uint32_t kModulus = 65521;
    /// Largest n for which sum2 cannot overflow 32 bits between reductions (zlib's NMAX).
    static constexpr std::size_t kBlock = 5552;
    std::uint32_t sum1_ = 1;
    std::uint32_t sum2_ = 0;
};

/// Forwards bytes to the sink in kStageBytes writes and computes each chunk's CRC on the way.
class ChunkStream {
public:
    explicit ChunkStream(ByteSink& sink) noexcept : sink_(sink) {}

    /// Bytes outside any chunk (the signature).
    [[nodiscard]] Status raw(Bytes bytes) noexcept { return append(bytes); }

    [[nodiscard]] Status begin_chunk(const std::array<std::uint8_t, 4>& tag,
                                     std::uint32_t length) noexcept {
        QZ_RETURN_IF_ERROR(append(be32(length)));
        QZ_RETURN_IF_ERROR(append(tag));
        crc_ = qz::crc32(tag);
        return ok();
    }

    [[nodiscard]] Status data(Bytes bytes) noexcept {
        crc_ = qz::crc32(bytes, crc_);
        return append(bytes);
    }

    [[nodiscard]] Status end_chunk() noexcept { return append(be32(crc_)); }

    [[nodiscard]] Status flush() noexcept {
        if (used_ == 0) {
            return ok();
        }
        const std::size_t count = used_;
        used_ = 0;
        return sink_.write(Bytes{stage_.data(), count});
    }

private:
    [[nodiscard]] Status append(Bytes bytes) noexcept {
        while (!bytes.empty()) {
            const std::size_t count = std::min(stage_.size() - used_, bytes.size());
            std::ranges::copy(bytes.first(count),
                              stage_.begin() + static_cast<std::ptrdiff_t>(used_));
            used_ += count;
            bytes = bytes.subspan(count);
            if (used_ == stage_.size()) {
                QZ_RETURN_IF_ERROR(flush());
            }
        }
        return ok();
    }

    ByteSink& sink_;
    std::array<std::uint8_t, kStageBytes> stage_{};
    std::size_t used_ = 0;
    std::uint32_t crc_ = 0;
};

[[nodiscard]] std::array<std::uint8_t, kIhdrDataBytes> ihdr_data() noexcept {
    const auto width = be32(static_cast<std::uint32_t>(kWidth));
    const auto height = be32(static_cast<std::uint32_t>(kHeight));
    return {width[0],
            width[1],
            width[2],
            width[3],
            height[0],
            height[1],
            height[2],
            height[3],
            1,  // bit depth
            0,  // colour type 0: grayscale
            0,  // compression method 0: deflate
            0,  // filter method 0 (every row uses filter type None)
            0}; // interlace method 0: none
}

[[nodiscard]] Status
write_image_rows(ChunkStream& stream, const Framebuffer& fb, Adler32& adler) noexcept {
    std::array<std::uint8_t, kRowBytes> row{}; // row[0] = 0: filter type None
    for (std::size_t y = 0; y < kRows; ++y) {
        const Bytes pixels = fb.bytes().subspan(y * kStride, kStride);
        std::ranges::transform(pixels, row.begin() + 1, [](std::uint8_t b) {
            return static_cast<std::uint8_t>(~b); // ink 1 (framebuffer) -> 0 (PNG black)
        });
        adler.update(row);
        QZ_RETURN_IF_ERROR(stream.data(row));
    }
    return ok();
}

} // namespace

namespace {

[[nodiscard]] Status write_header_chunk(ChunkStream& stream) noexcept {
    QZ_RETURN_IF_ERROR(stream.begin_chunk(kTagIhdr, static_cast<std::uint32_t>(kIhdrDataBytes)));
    QZ_RETURN_IF_ERROR(stream.data(ihdr_data()));
    return stream.end_chunk();
}

[[nodiscard]] Status write_image_chunk(ChunkStream& stream, const Framebuffer& fb) noexcept {
    QZ_RETURN_IF_ERROR(stream.begin_chunk(kTagIdat, kIdatDataBytes));
    QZ_RETURN_IF_ERROR(stream.data(kStreamHeader));
    Adler32 adler;
    QZ_RETURN_IF_ERROR(write_image_rows(stream, fb, adler));
    QZ_RETURN_IF_ERROR(stream.data(be32(adler.value())));
    return stream.end_chunk();
}

[[nodiscard]] Status write_end_chunk(ChunkStream& stream) noexcept {
    QZ_RETURN_IF_ERROR(stream.begin_chunk(kTagIend, 0));
    return stream.end_chunk();
}

} // namespace

Status encode_png(const Framebuffer& fb, ByteSink& out) noexcept {
    ChunkStream stream(out);
    QZ_RETURN_IF_ERROR(stream.raw(kSignature));
    QZ_RETURN_IF_ERROR(write_header_chunk(stream));
    QZ_RETURN_IF_ERROR(write_image_chunk(stream, fb));
    QZ_RETURN_IF_ERROR(write_end_chunk(stream));
    return stream.flush();
}

} // namespace qz::gfx
