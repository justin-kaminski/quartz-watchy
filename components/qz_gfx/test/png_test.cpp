// encode_png (framebuffer.hpp).
//
// The output is decoded with a minimal PNG + zlib stored-block reader that checks every chunk CRC
// and the Adler-32 using reference implementations independent of qz::crc32 and of the encoder.
// The reader itself is validated by corrupting a good file in every region. On top: structure pins,
// byte-determinism, pins computed by an independent Python encoder (struct + zlib.crc32 +
// zlib.adler32, round-tripped through zlib.decompress), and sink error propagation.
#include "qz/core/crc32.hpp"
#include "qz/core/result.hpp"
#include "qz/gfx/framebuffer.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace qz::gfx {
namespace {

using Bytes = std::vector<std::uint8_t>;
using ByteView = std::span<const std::uint8_t>;

// Layout of every file the encoder produces (a 200 x 200 1-bit image, stored deflate, one IDAT).
constexpr std::size_t kFileBytes = 5268;
constexpr std::size_t kIhdrDataOffset = 16;
constexpr std::size_t kIdatTypeOffset = 37;
constexpr std::size_t kIdatDataOffset = 41;
constexpr std::size_t kStoredHeaderOffset = 43;
constexpr std::size_t kRawOffset = 48;
constexpr std::size_t kRawBytes = 5200; // 200 rows x (1 filter byte + 25 pixel bytes)
constexpr std::size_t kAdlerOffset = kRawOffset + kRawBytes;
constexpr std::size_t kIdatCrcOffset = kAdlerOffset + 4;
constexpr std::size_t kIendOffset = kIdatCrcOffset + 4;
static_assert(kIendOffset + 12 == kFileBytes);

// --- Reference checksums (deliberately naive; independent of production code)
// -----------------------

std::uint32_t reference_crc32(ByteView data) {
    std::uint32_t state = 0xFFFFFFFFU;
    for (const std::uint8_t byte : data) {
        state ^= byte;
        for (int bit = 0; bit < 8; ++bit) {
            state = ((state & 1U) != 0U) ? ((state >> 1U) ^ 0xEDB88320U) : (state >> 1U);
        }
    }
    return ~state;
}

std::uint32_t reference_adler32(ByteView data) {
    std::uint32_t a = 1;
    std::uint32_t b = 0;
    for (const std::uint8_t byte : data) {
        a = (a + byte) % 65521U;
        b = (b + a) % 65521U;
    }
    return (b << 16U) | a;
}

std::uint32_t read_be32(ByteView b) {
    return (static_cast<std::uint32_t>(b[0]) << 24U) | (static_cast<std::uint32_t>(b[1]) << 16U) |
           (static_cast<std::uint32_t>(b[2]) << 8U) | static_cast<std::uint32_t>(b[3]);
}

unsigned read_le16(ByteView b) {
    return static_cast<unsigned>(b[0]) | (static_cast<unsigned>(b[1]) << 8U);
}

void store_be32(Bytes& bytes, std::size_t offset, std::uint32_t v) {
    bytes[offset] = static_cast<std::uint8_t>(v >> 24U);
    bytes[offset + 1] = static_cast<std::uint8_t>((v >> 16U) & 0xFFU);
    bytes[offset + 2] = static_cast<std::uint8_t>((v >> 8U) & 0xFFU);
    bytes[offset + 3] = static_cast<std::uint8_t>(v & 0xFFU);
}

// --- Minimal PNG reader
// ------------------------------------------------------------------------------

struct Decoded {
    std::string error; ///< empty = the file is well formed
    std::vector<std::string> chunks;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint8_t bit_depth = 0;
    std::uint8_t color_type = 0;
    std::uint8_t compression = 0;
    std::uint8_t filter = 0;
    std::uint8_t interlace = 0;
    Bytes inflated; ///< scanlines with their filter byte
    Bytes pixels;   ///< unfiltered rows, PNG polarity (bit 1 = white)
};

bool fail(Decoded& d, std::string message) {
    d.error = std::move(message);
    return false;
}

bool parse_ihdr(ByteView body, Decoded& d) {
    if (!d.chunks.empty()) {
        return fail(d, "IHDR is not the first chunk");
    }
    if (body.size() != 13) {
        return fail(d, "IHDR must be 13 bytes");
    }
    d.width = read_be32(body.subspan(0, 4));
    d.height = read_be32(body.subspan(4, 4));
    d.bit_depth = body[8];
    d.color_type = body[9];
    d.compression = body[10];
    d.filter = body[11];
    d.interlace = body[12];
    return true;
}

bool parse_chunks(ByteView png, Decoded& d, Bytes& idat) {
    constexpr std::array<std::uint8_t, 8> kSignature = {
        0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    if (png.size() < kSignature.size() ||
        !std::ranges::equal(png.first(kSignature.size()), kSignature)) {
        return fail(d, "bad PNG signature");
    }
    std::size_t pos = kSignature.size();
    bool seen_iend = false;
    while (pos < png.size()) {
        if (seen_iend) {
            return fail(d, "data after IEND");
        }
        if (png.size() - pos < 12) {
            return fail(d, "truncated chunk header");
        }
        const std::uint32_t length = read_be32(png.subspan(pos));
        if (length > png.size() - pos - 12) {
            return fail(d, "truncated chunk: length exceeds the file");
        }
        const ByteView tagged = png.subspan(pos + 4, std::size_t{4} + length); // type + data
        const std::string type(reinterpret_cast<const char*>(tagged.data()), 4);
        if (reference_crc32(tagged) != read_be32(png.subspan(pos + 8 + length))) {
            return fail(d, "CRC mismatch in chunk " + type);
        }
        const ByteView body = tagged.subspan(4);
        if (type == "IHDR") {
            if (!parse_ihdr(body, d)) {
                return false;
            }
        } else if (type == "IDAT") {
            idat.insert(idat.end(), body.begin(), body.end());
        } else if (type == "IEND") {
            if (!body.empty()) {
                return fail(d, "IEND must be empty");
            }
            seen_iend = true;
        } else {
            return fail(d, "unexpected chunk " + type);
        }
        d.chunks.push_back(type);
        pos += std::size_t{12} + length;
    }
    if (!seen_iend) {
        return fail(d, "missing IEND");
    }
    if (d.chunks.empty() || d.chunks.front() != "IHDR") {
        return fail(d, "IHDR must come first");
    }
    return true;
}

/// zlib stream made only of stored blocks (what a minimal reader has to handle for this encoder).
bool inflate_stored(const Bytes& z, Decoded& d) {
    if (z.size() < 6) {
        return fail(d, "zlib stream too short");
    }
    const unsigned cmf = z[0];
    const unsigned flg = z[1];
    if ((cmf & 0x0FU) != 8U || (cmf >> 4U) > 7U) {
        return fail(d, "zlib header: not deflate with a valid window");
    }
    if ((((cmf << 8U) | flg) % 31U) != 0U) {
        return fail(d, "zlib header check bits");
    }
    if ((flg & 0x20U) != 0U) {
        return fail(d, "zlib header: preset dictionary");
    }
    std::size_t pos = 2;
    bool final_block = false;
    while (!final_block) {
        if (z.size() - pos < 5) {
            return fail(d, "truncated stored block header");
        }
        const unsigned head = z[pos];
        final_block = (head & 1U) != 0U;
        if (((head >> 1U) & 3U) != 0U) {
            return fail(d, "not a stored deflate block");
        }
        const unsigned len = read_le16(ByteView{z}.subspan(pos + 1, 2));
        const unsigned nlen = read_le16(ByteView{z}.subspan(pos + 3, 2));
        if ((~len & 0xFFFFU) != nlen) {
            return fail(d, "stored block LEN/NLEN mismatch");
        }
        pos += 5;
        if (z.size() - pos < len) {
            return fail(d, "stored block overruns the stream");
        }
        const ByteView block = ByteView{z}.subspan(pos, len);
        d.inflated.insert(d.inflated.end(), block.begin(), block.end());
        pos += len;
    }
    if (z.size() - pos != 4) {
        return fail(d, "the final block must be followed by exactly the 4-byte Adler-32");
    }
    if (read_be32(ByteView{z}.subspan(pos)) != reference_adler32(d.inflated)) {
        return fail(d, "Adler-32 mismatch");
    }
    return true;
}

bool unfilter(Decoded& d) {
    const std::size_t row_bytes =
        1U + (((std::size_t{d.width} * static_cast<std::size_t>(d.bit_depth)) + 7U) / 8U);
    if (d.inflated.size() != row_bytes * d.height) {
        return fail(d, "inflated size does not match IHDR");
    }
    for (std::size_t y = 0; y < d.height; ++y) {
        const std::size_t start = y * row_bytes;
        if (d.inflated[start] != 0) {
            return fail(d, "filter type is not None in row " + std::to_string(y));
        }
        d.pixels.insert(d.pixels.end(),
                        d.inflated.begin() + static_cast<std::ptrdiff_t>(start + 1),
                        d.inflated.begin() + static_cast<std::ptrdiff_t>(start + row_bytes));
    }
    return true;
}

Decoded decode(ByteView png) {
    Decoded d;
    Bytes idat;
    if (!parse_chunks(png, d, idat)) {
        return d;
    }
    if (!inflate_stored(idat, d)) {
        return d;
    }
    (void)unfilter(d); // records its own error
    return d;
}

// --- Sinks and frames
// -----------------------------------------------------------------------------------

class VectorSink final : public ByteSink {
public:
    Status write(ByteView bytes) override {
        ++writes_;
        EXPECT_FALSE(bytes.empty()) << "the encoder must never issue an empty write";
        data_.insert(data_.end(), bytes.begin(), bytes.end());
        return ok();
    }
    [[nodiscard]] const Bytes& data() const noexcept { return data_; }
    [[nodiscard]] std::size_t writes() const noexcept { return writes_; }

private:
    Bytes data_;
    std::size_t writes_ = 0;
};

/// Fails the write with index `fail_at` and records anything the encoder still does afterwards.
class FailingSink final : public ByteSink {
public:
    static constexpr Error kInjected{Errc::kIo, 0x1234};

    explicit FailingSink(std::size_t fail_at) noexcept : fail_at_(fail_at) {}

    Status write(ByteView /*bytes*/) override {
        if (failed_) {
            ++writes_after_failure_;
            return kInjected;
        }
        if (writes_ == fail_at_) {
            failed_ = true;
            ++writes_;
            return kInjected;
        }
        ++writes_;
        return ok();
    }
    [[nodiscard]] std::size_t writes() const noexcept { return writes_; }
    [[nodiscard]] std::size_t writes_after_failure() const noexcept {
        return writes_after_failure_;
    }

private:
    std::size_t fail_at_;
    std::size_t writes_ = 0;
    std::size_t writes_after_failure_ = 0;
    bool failed_ = false;
};

Bytes encode(const Framebuffer& fb) {
    VectorSink sink;
    const Status status = encode_png(fb, sink);
    EXPECT_TRUE(status.has_value());
    return sink.data();
}

Framebuffer black_frame() {
    Framebuffer fb;
    fb.clear(Color::kBlack);
    return fb;
}

/// bits[i] = (i * 37 + 11) mod 256; the same formula builds the frame in the Python reference.
Framebuffer pattern_frame() {
    Framebuffer fb;
    for (std::size_t i = 0; i < fb.bits.size(); ++i) {
        fb.bits[i] = static_cast<std::uint8_t>(((i * 37U) + 11U) & 0xFFU);
    }
    return fb;
}

Framebuffer noise_frame(std::uint32_t seed) {
    Framebuffer fb;
    std::uint32_t x = seed;
    for (std::uint8_t& b : fb.bits) {
        x ^= x << 13U;
        x ^= x >> 17U;
        x ^= x << 5U;
        b = static_cast<std::uint8_t>(x & 0xFFU);
    }
    return fb;
}

/// What a decoder must see for `fb`: every byte inverted (PNG black is 0, ink is 1).
Bytes inverted(const Framebuffer& fb) {
    Bytes out;
    out.reserve(fb.bits.size());
    for (const std::uint8_t b : fb.bits) {
        out.push_back(static_cast<std::uint8_t>(~b));
    }
    return out;
}

// --- Tests
// -----------------------------------------------------------------------------------------------

TEST(EncodePng, WhiteFrameDecodesToWhitePixels) {
    const Decoded d = decode(encode(Framebuffer{}));
    ASSERT_TRUE(d.error.empty()) << d.error;
    EXPECT_EQ(d.pixels.size(), kFrameBytes);
    EXPECT_TRUE(std::ranges::all_of(d.pixels, [](std::uint8_t b) { return b == 0xFF; }));
}

TEST(EncodePng, BlackFrameDecodesToBlackPixels) {
    const Decoded d = decode(encode(black_frame()));
    ASSERT_TRUE(d.error.empty()) << d.error;
    EXPECT_EQ(d.pixels.size(), kFrameBytes);
    EXPECT_TRUE(std::ranges::all_of(d.pixels, [](std::uint8_t b) { return b == 0x00; }));
}

TEST(EncodePng, EveryPixelMapsToItsOwnBit) {
    for (const std::pair<int, int>& p :
         {std::pair{0, 0}, {7, 0}, {8, 0}, {199, 0}, {0, 199}, {199, 199}, {100, 100}, {13, 57}}) {
        Framebuffer fb;
        fb.set(
            static_cast<std::int16_t>(p.first), static_cast<std::int16_t>(p.second), Color::kBlack);
        const Decoded d = decode(encode(fb));
        ASSERT_TRUE(d.error.empty()) << d.error;
        Bytes expected(kFrameBytes, 0xFF);
        const std::size_t index = (static_cast<std::size_t>(p.second) * kStride) +
                                  (static_cast<std::size_t>(p.first) / 8U);
        expected[index] =
            static_cast<std::uint8_t>(~(0x80U >> (static_cast<unsigned>(p.first) % 8U)));
        EXPECT_TRUE(d.pixels == expected) << "pixel (" << p.first << "," << p.second << ")";
    }
}

TEST(EncodePng, FramesRoundTripBitExact) {
    for (const std::uint32_t seed : {1U, 2463534242U, 0xDEADBEEFU, 7U}) {
        const Framebuffer fb = noise_frame(seed);
        const Decoded d = decode(encode(fb));
        ASSERT_TRUE(d.error.empty()) << d.error;
        EXPECT_TRUE(d.pixels == inverted(fb)) << "seed " << seed;
    }
    const Framebuffer pattern = pattern_frame();
    const Decoded d = decode(encode(pattern));
    ASSERT_TRUE(d.error.empty()) << d.error;
    EXPECT_TRUE(d.pixels == inverted(pattern));
}

TEST(EncodePng, FileStructureIsFixed) {
    VectorSink sink;
    ASSERT_TRUE(encode_png(pattern_frame(), sink).has_value());
    const Bytes& png = sink.data();
    ASSERT_EQ(png.size(), kFileBytes);

    const Decoded d = decode(png);
    ASSERT_TRUE(d.error.empty()) << d.error;
    EXPECT_EQ(d.chunks, (std::vector<std::string>{"IHDR", "IDAT", "IEND"}));
    EXPECT_EQ(d.width, 200U);
    EXPECT_EQ(d.height, 200U);
    EXPECT_EQ(d.bit_depth, 1);
    EXPECT_EQ(d.color_type, 0); // grayscale
    EXPECT_EQ(d.compression, 0);
    EXPECT_EQ(d.filter, 0);
    EXPECT_EQ(d.interlace, 0);
    EXPECT_EQ(d.inflated.size(), kRawBytes);

    // Offsets of the pieces (a PNG viewer-independent reading of the bytes).
    EXPECT_EQ(read_be32(ByteView{png}.subspan(8, 4)), 13U);                           // IHDR length
    EXPECT_EQ(read_be32(ByteView{png}.subspan(kIhdrDataOffset - 4, 4)), 0x49484452U); // "IHDR"
    EXPECT_EQ(read_be32(ByteView{png}.subspan(33, 4)), 5211U);                        // IDAT length
    EXPECT_EQ(read_be32(ByteView{png}.subspan(kIdatTypeOffset, 4)), 0x49444154U);     // "IDAT"
    EXPECT_EQ(png[kIdatDataOffset], 0x78);                                            // zlib CMF
    EXPECT_EQ(png[kIdatDataOffset + 1], 0x01);                                        // zlib FLG
    // One final stored block of 5200 = 0x1450 bytes: BFINAL|BTYPE, LEN, NLEN (little endian).
    const std::array<std::uint8_t, 5> stored = {0x01, 0x50, 0x14, 0xAF, 0xEB};
    EXPECT_TRUE(std::ranges::equal(ByteView{png}.subspan(kStoredHeaderOffset, 5), stored));
    // IEND is the constant 12-byte chunk.
    const std::array<std::uint8_t, 12> iend = {
        0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82};
    EXPECT_TRUE(std::ranges::equal(ByteView{png}.subspan(kIendOffset, 12), iend));
    // The encoder delivers the file in a handful of large writes, never an empty one.
    EXPECT_LE(sink.writes(), 32U);
    EXPECT_GE(sink.writes(), 2U);
}

TEST(EncodePng, OutputIsByteIdenticalAcrossRunsAndCopies) {
    const Framebuffer fb = noise_frame(99);
    const Bytes first = encode(fb);
    const Bytes second = encode(fb);
    const Framebuffer copy = fb;
    EXPECT_TRUE(first == second);
    EXPECT_TRUE(first == encode(copy));
    EXPECT_FALSE(first == encode(noise_frame(100))); // and it does depend on the pixels
    EXPECT_FALSE(first == encode(Framebuffer{}));
}

// Reference values from an independent encoder (Python 3: struct, zlib.crc32, zlib.adler32; the
// IDAT payload round-trips through zlib.decompress; `file`/ImageMagick accept the files).
TEST(EncodePng, MatchesAnIndependentReferenceEncoder) {
    struct Pin {
        std::string_view name;
        Framebuffer frame;
        std::uint32_t crc;
    };
    const std::array<Pin, 3> pins = {{
        {.name = "white", .frame = Framebuffer{}, .crc = 0xF3330E23U},
        {.name = "black", .frame = black_frame(), .crc = 0x8B637A00U},
        {.name = "pattern", .frame = pattern_frame(), .crc = 0x70459CBCU},
    }};
    for (const Pin& pin : pins) {
        const Bytes png = encode(pin.frame);
        EXPECT_EQ(png.size(), kFileBytes) << pin.name;
        EXPECT_EQ(qz::crc32(png), pin.crc) << pin.name;
        EXPECT_EQ(reference_crc32(png), pin.crc) << pin.name;
    }
}

TEST(EncodePng, SinkErrorsPropagateAndStopTheEncoder) {
    const Framebuffer fb = pattern_frame();
    VectorSink reference;
    ASSERT_TRUE(encode_png(fb, reference).has_value());
    const std::size_t total = reference.writes();
    ASSERT_GE(total, 2U);
    for (std::size_t k = 0; k < total; ++k) {
        FailingSink sink(k);
        const Status status = encode_png(fb, sink);
        ASSERT_FALSE(status.has_value()) << "failing write " << k;
        EXPECT_EQ(status.error(), FailingSink::kInjected) << "failing write " << k;
        EXPECT_EQ(sink.writes(), k + 1) << "failing write " << k;
        EXPECT_EQ(sink.writes_after_failure(), 0U) << "failing write " << k;
    }
    FailingSink never(total); // fails only on a write that does not exist
    EXPECT_TRUE(encode_png(fb, never).has_value());
    EXPECT_EQ(never.writes(), total);
}

// --- The reader must actually reject broken files (otherwise the tests above prove nothing)
// -------

enum class Repair : std::uint8_t { kNone, kChunkCrc, kAdlerAndChunkCrc };

void repair(Bytes& png, Repair mode) {
    if (mode == Repair::kAdlerAndChunkCrc) {
        store_be32(
            png, kAdlerOffset, reference_adler32(ByteView{png}.subspan(kRawOffset, kRawBytes)));
    }
    if (mode != Repair::kNone) {
        const std::size_t covered = kAdlerOffset + 4 - kIdatTypeOffset; // IDAT type + data
        store_be32(
            png, kIdatCrcOffset, reference_crc32(ByteView{png}.subspan(kIdatTypeOffset, covered)));
    }
}

TEST(PngReader, AcceptsTheGoodFileAndRejectsEveryKindOfCorruption) {
    const Bytes good = encode(pattern_frame());
    ASSERT_TRUE(decode(good).error.empty());

    struct Corruption {
        std::string_view what;
        std::size_t offset;
        std::uint8_t flip;
        Repair mode;
        std::string_view expected_error;
    };
    const std::array<Corruption, 10> corruptions = {{
        {.what = "signature",
         .offset = 1,
         .flip = 0x01,
         .mode = Repair::kNone,
         .expected_error = "signature"},
        {.what = "IHDR bit depth, CRC stale",
         .offset = 24,
         .flip = 0x01,
         .mode = Repair::kNone,
         .expected_error = "CRC mismatch in chunk IHDR"},
        {.what = "pixel byte, CRC stale",
         .offset = 100,
         .flip = 0x10,
         .mode = Repair::kNone,
         .expected_error = "CRC mismatch in chunk IDAT"},
        {.what = "pixel byte, chunk CRC repaired but Adler-32 stale",
         .offset = 100,
         .flip = 0x10,
         .mode = Repair::kChunkCrc,
         .expected_error = "Adler-32"},
        {.what = "Adler-32 byte, chunk CRC repaired",
         .offset = kAdlerOffset + 2,
         .flip = 0x80,
         .mode = Repair::kChunkCrc,
         .expected_error = "Adler-32"},
        {.what = "stored block LEN",
         .offset = kStoredHeaderOffset + 1,
         .flip = 0x01,
         .mode = Repair::kChunkCrc,
         .expected_error = "LEN/NLEN"},
        {.what = "zlib FLG check bits",
         .offset = kIdatDataOffset + 1,
         .flip = 0x01,
         .mode = Repair::kChunkCrc,
         .expected_error = "zlib header check bits"},
        {.what = "deflate block type",
         .offset = kStoredHeaderOffset,
         .flip = 0x02,
         .mode = Repair::kChunkCrc,
         .expected_error = "not a stored"},
        {.what = "row 0 filter type, checksums repaired",
         .offset = kRawOffset,
         .flip = 0x01,
         .mode = Repair::kAdlerAndChunkCrc,
         .expected_error = "filter type"},
        {.what = "IEND CRC",
         .offset = kIendOffset + 8,
         .flip = 0x01,
         .mode = Repair::kNone,
         .expected_error = "CRC mismatch in chunk IEND"},
    }};
    for (const Corruption& c : corruptions) {
        Bytes bad = good;
        bad[c.offset] = static_cast<std::uint8_t>(bad[c.offset] ^ c.flip);
        repair(bad, c.mode);
        const Decoded d = decode(bad);
        EXPECT_NE(d.error.find(c.expected_error), std::string::npos)
            << c.what << ": got \"" << d.error << "\"";
    }

    Bytes truncated = good;
    truncated.resize(truncated.size() - 5);
    EXPECT_NE(decode(truncated).error.find("truncated"), std::string::npos);

    Bytes no_iend = good;
    no_iend.resize(no_iend.size() - 12);
    EXPECT_NE(decode(no_iend).error.find("missing IEND"), std::string::npos);

    Bytes trailing = good;
    trailing.push_back(0);
    EXPECT_NE(decode(trailing).error.find("after IEND"), std::string::npos);

    EXPECT_FALSE(decode(ByteView{}).error.empty());
}

} // namespace
} // namespace qz::gfx
