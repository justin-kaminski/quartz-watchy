// Single-line JSON writer (protocol.hpp): fixed buffer, escaped strings, latched overflow.
//
// Output is compact RFC 8259 JSON that is always valid UTF-8: valid sequences pass through,
// every byte that is not part of one becomes the escape � (Wi-Fi SSIDs are arbitrary bytes,
// and the protocol says lines are UTF-8). C0 controls and DEL are escaped. Structure misuse (a
// value without its key inside an object, mismatched end calls, nesting past kMaxJsonDepth) is a
// programmer error and trips QZ_ASSERT; running out of buffer is data-dependent and only latches.
#include "qz/console/protocol.hpp"
#include "tuning.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>

namespace qz::console {
namespace {

constexpr std::string_view kBase64Alphabet =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
constexpr std::string_view kHexDigits = "0123456789abcdef";
constexpr std::string_view kReplacementEscape = "\\ufffd"; // U+FFFD REPLACEMENT CHARACTER
constexpr std::string_view kQuote = "\"";
constexpr std::string_view kComma = ",";
constexpr std::string_view kColon = ":";
constexpr std::size_t kInt64Chars = 20; // "-9223372036854775808"
constexpr unsigned kFirstPrintable = 0x20U;
constexpr unsigned kDelete = 0x7FU;
constexpr unsigned kFirstNonAscii = 0x80U;
constexpr unsigned kContinuationLow = 0x80U;
constexpr unsigned kContinuationHigh = 0xBFU;

/// Plain printable ASCII: the bytes that need no escape and no UTF-8 check.
constexpr bool is_plain(char c) noexcept {
    const auto byte = static_cast<unsigned char>(c);
    return byte >= kFirstPrintable && byte < kDelete && c != '"' && c != '\\';
}

/// text[index], or 0 past the end (0 is never a continuation byte).
unsigned byte_at(std::string_view text, std::size_t index) noexcept {
    return index < text.size() ? static_cast<unsigned char>(text[index]) : 0U;
}

bool is_continuation(std::string_view text,
                     std::size_t index,
                     unsigned low = kContinuationLow,
                     unsigned high = kContinuationHigh) noexcept {
    const unsigned byte = byte_at(text, index);
    return byte >= low && byte <= high;
}

/// Length of the well-formed UTF-8 sequence at text[pos] (Unicode table 3-7: no overlong forms, no
/// surrogates, nothing above U+10FFFF), or 0 when the byte there does not start one.
std::size_t utf8_length_at(std::string_view text, std::size_t pos) noexcept {
    const unsigned lead = byte_at(text, pos);
    if (lead >= 0xC2U && lead <= 0xDFU) {
        return is_continuation(text, pos + 1U) ? 2U : 0U;
    }
    if (lead >= 0xE0U && lead <= 0xEFU) {
        const unsigned low = lead == 0xE0U ? 0xA0U : kContinuationLow;
        const unsigned high = lead == 0xEDU ? 0x9FU : kContinuationHigh;
        const bool valid =
            is_continuation(text, pos + 1U, low, high) && is_continuation(text, pos + 2U);
        return valid ? 3U : 0U;
    }
    if (lead >= 0xF0U && lead <= 0xF4U) {
        const unsigned low = lead == 0xF0U ? 0x90U : kContinuationLow;
        const unsigned high = lead == 0xF4U ? 0x8FU : kContinuationHigh;
        const bool valid = is_continuation(text, pos + 1U, low, high) &&
                           is_continuation(text, pos + 2U) && is_continuation(text, pos + 3U);
        return valid ? 4U : 0U;
    }
    return 0U;
}

/// The escape for one ASCII byte that is not plain: a short form where JSON has one, else \u00xx.
std::string_view escape_for(unsigned char byte, std::array<char, 6>& scratch) noexcept {
    switch (byte) {
        case '"':
            return "\\\"";
        case '\\':
            return "\\\\";
        case '\b':
            return "\\b";
        case '\f':
            return "\\f";
        case '\n':
            return "\\n";
        case '\r':
            return "\\r";
        case '\t':
            return "\\t";
        default:
            break;
    }
    scratch[0] = '\\';
    scratch[1] = 'u';
    scratch[2] = '0';
    scratch[3] = '0';
    scratch[4] = kHexDigits[static_cast<std::size_t>(byte >> 4U)];
    scratch[5] = kHexDigits[static_cast<std::size_t>(byte & 0x0FU)];
    return {scratch.data(), scratch.size()};
}

/// Decimal text of `value` inside `storage` (no printf; INT64_MIN safe).
std::string_view format_int64(std::array<char, kInt64Chars>& storage, std::int64_t value) noexcept {
    std::uint64_t magnitude =
        value < 0 ? 0U - static_cast<std::uint64_t>(value) : static_cast<std::uint64_t>(value);
    std::size_t pos = storage.size();
    do {
        storage[--pos] = static_cast<char>('0' + (magnitude % 10U));
        magnitude /= 10U;
    } while (magnitude != 0U);
    if (value < 0) {
        storage[--pos] = '-';
    }
    return {storage.data() + pos, storage.size() - pos};
}

/// `chars` base64 digits of the 24-bit `group`, then '=' up to four characters. Returns the
/// position after the last character written.
char* encode_group(char* out, std::uint32_t group, std::size_t chars) noexcept {
    constexpr std::size_t kGroupChars = 4;
    constexpr std::uint32_t kSixBits = 0x3FU;
    constexpr unsigned kBitsPerDigit = 6;
    constexpr unsigned kTopShift = 18;
    for (std::size_t i = 0; i < kGroupChars; ++i) {
        if (i < chars) {
            const unsigned shift = kTopShift - (kBitsPerDigit * static_cast<unsigned>(i));
            *out++ = kBase64Alphabet[(group >> shift) & kSixBits];
        } else {
            *out++ = '=';
        }
    }
    return out;
}

} // namespace

JsonWriter::JsonWriter(std::span<char> buffer) noexcept : buf_(buffer) {}

bool JsonWriter::overflowed() const noexcept {
    return overflow_;
}

std::string_view JsonWriter::view() const noexcept {
    return {buf_.data(), len_};
}

std::size_t JsonWriter::depth() const noexcept {
    return depth_;
}

void JsonWriter::put(std::string_view text) noexcept {
    if (overflow_ || text.empty()) {
        return;
    }
    if (text.size() > buf_.size() - len_) {
        overflow_ = true;
        return;
    }
    std::memcpy(buf_.data() + len_, text.data(), text.size());
    len_ += text.size();
}

void JsonWriter::put_quoted(std::string_view text) noexcept {
    put(kQuote);
    std::array<char, 6> scratch{};
    std::size_t pos = 0;
    while (pos < text.size() && !overflow_) {
        std::size_t run = pos;
        while (run < text.size() && is_plain(text[run])) {
            ++run;
        }
        put(text.substr(pos, run - pos));
        pos = run;
        if (pos == text.size()) {
            break;
        }
        const auto byte = static_cast<unsigned char>(text[pos]);
        if (byte < kFirstNonAscii) {
            put(escape_for(byte, scratch));
            ++pos;
            continue;
        }
        const std::size_t length = utf8_length_at(text, pos);
        if (length == 0) {
            put(kReplacementEscape);
            ++pos;
        } else {
            put(text.substr(pos, length));
            pos += length;
        }
    }
    put(kQuote);
}

bool JsonWriter::in_array() const noexcept {
    return depth_ > 0 && ((array_bits_ >> depth_) & 1U) != 0U;
}

void JsonWriter::begin_value() noexcept {
    if (after_key_) {
        after_key_ = false; // key() already wrote the separator and "key":
        return;
    }
    // Without a key a value is only legal as the single top-level value or as an array element.
    QZ_ASSERT(depth_ == 0 ? (need_comma_ & 1U) == 0U : in_array());
    if (((need_comma_ >> depth_) & 1U) != 0U) {
        put(kComma);
    }
}

void JsonWriter::end_value() noexcept {
    need_comma_ |= 1U << depth_; // the value is now an element of its container
}

void JsonWriter::open(char bracket, bool is_array) noexcept {
    begin_value();
    QZ_ASSERT(depth_ < detail::kMaxJsonDepth);
    put(std::string_view(&bracket, 1));
    ++depth_;
    const std::uint32_t level = 1U << depth_;
    need_comma_ &= ~level;
    array_bits_ = is_array ? (array_bits_ | level) : (array_bits_ & ~level);
}

void JsonWriter::close(char bracket, bool is_array) noexcept {
    QZ_ASSERT(depth_ > 0 && !after_key_ && in_array() == is_array);
    put(std::string_view(&bracket, 1));
    --depth_;
    end_value();
}

JsonWriter& JsonWriter::begin_object() noexcept {
    open('{', false);
    return *this;
}

JsonWriter& JsonWriter::end_object() noexcept {
    close('}', false);
    return *this;
}

JsonWriter& JsonWriter::begin_array(std::string_view key_name) noexcept {
    if (!key_name.empty()) {
        key(key_name);
    }
    open('[', true);
    return *this;
}

JsonWriter& JsonWriter::end_array() noexcept {
    close(']', true);
    return *this;
}

JsonWriter& JsonWriter::key(std::string_view k) noexcept {
    QZ_ASSERT(depth_ > 0 && !in_array() && !after_key_);
    if (((need_comma_ >> depth_) & 1U) != 0U) {
        put(kComma);
    }
    put_quoted(k);
    put(kColon);
    after_key_ = true;
    return *this;
}

JsonWriter& JsonWriter::str(std::string_view v) noexcept {
    begin_value();
    put_quoted(v);
    end_value();
    return *this;
}

JsonWriter& JsonWriter::num(std::int64_t v) noexcept {
    begin_value();
    std::array<char, kInt64Chars> digits{};
    put(format_int64(digits, v));
    end_value();
    return *this;
}

JsonWriter& JsonWriter::boolean(bool v) noexcept {
    begin_value();
    put(v ? "true" : "false");
    end_value();
    return *this;
}

JsonWriter& JsonWriter::null() noexcept {
    begin_value();
    put("null");
    end_value();
    return *this;
}

JsonWriter& JsonWriter::base64(std::span<const std::uint8_t> bytes) noexcept {
    constexpr std::size_t kGroupBytes = 3;
    constexpr std::size_t kGroupChars = 4;
    begin_value();
    // Reserve the whole string up front: it is written or refused as one piece.
    const std::size_t encoded = (bytes.size() + kGroupBytes - 1U) / kGroupBytes * kGroupChars;
    if (!overflow_ && encoded + (2U * kQuote.size()) > buf_.size() - len_) {
        overflow_ = true;
    }
    if (!overflow_) {
        char* out = buf_.data() + len_;
        *out++ = '"';
        std::size_t i = 0;
        for (; i + kGroupBytes <= bytes.size(); i += kGroupBytes) {
            const std::uint32_t group = (static_cast<std::uint32_t>(bytes[i]) << 16U) |
                                        (static_cast<std::uint32_t>(bytes[i + 1U]) << 8U) |
                                        static_cast<std::uint32_t>(bytes[i + 2U]);
            out = encode_group(out, group, kGroupChars);
        }
        const std::size_t tail = bytes.size() - i;
        if (tail == 1U) {
            out = encode_group(out, static_cast<std::uint32_t>(bytes[i]) << 16U, 2U);
        } else if (tail == 2U) {
            const std::uint32_t group = (static_cast<std::uint32_t>(bytes[i]) << 16U) |
                                        (static_cast<std::uint32_t>(bytes[i + 1U]) << 8U);
            out = encode_group(out, group, 3U);
        }
        *out++ = '"';
        len_ = static_cast<std::size_t>(out - buf_.data());
    }
    end_value();
    return *this;
}

} // namespace qz::console
