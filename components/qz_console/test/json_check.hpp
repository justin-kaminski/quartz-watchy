// Test helper: a strict JSON (RFC 8259) checker that also validates UTF-8 and decodes strings.
// Written independently of the writer under test (decode-then-range-check, not its table).
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace qz::console::test {

struct JsonDoc {
    bool valid = false;
    std::vector<std::string> strings; ///< every decoded string (keys and values), in order
    std::vector<std::string> numbers; ///< every number token as written, in order
    std::size_t max_depth = 0;        ///< deepest nesting of objects and arrays
};

/// True for well-formed UTF-8: no stray or missing continuation bytes, overlong forms,
/// surrogates or values above U+10FFFF.
inline bool is_valid_utf8(std::string_view text) {
    std::size_t pos = 0;
    while (pos < text.size()) {
        const auto lead = static_cast<unsigned char>(text[pos]);
        if (lead < 0x80U) {
            ++pos;
            continue;
        }
        std::size_t extra = 0;
        std::uint32_t code = 0;
        std::uint32_t minimum = 0;
        if (lead >= 0xC2U && lead <= 0xDFU) {
            extra = 1;
            code = lead & 0x1FU;
            minimum = 0x80U;
        } else if (lead >= 0xE0U && lead <= 0xEFU) {
            extra = 2;
            code = lead & 0x0FU;
            minimum = 0x800U;
        } else if (lead >= 0xF0U && lead <= 0xF4U) {
            extra = 3;
            code = lead & 0x07U;
            minimum = 0x10000U;
        } else {
            return false;
        }
        if (pos + extra >= text.size()) {
            return false;
        }
        for (std::size_t i = 1; i <= extra; ++i) {
            const auto next = static_cast<unsigned char>(text[pos + i]);
            if ((next & 0xC0U) != 0x80U) {
                return false;
            }
            code = (code << 6U) | (next & 0x3FU);
        }
        if (code < minimum || code > 0x10FFFFU || (code >= 0xD800U && code <= 0xDFFFU)) {
            return false;
        }
        pos += extra + 1;
    }
    return true;
}

namespace detail_json {

class Parser {
public:
    explicit Parser(std::string_view text) : text_(text) {}

    JsonDoc run() {
        skip_space();
        const bool parsed = value(0);
        skip_space();
        doc_.valid = parsed && pos_ == text_.size();
        return doc_;
    }

private:
    [[nodiscard]] bool at_end() const { return pos_ >= text_.size(); }
    [[nodiscard]] char peek() const { return at_end() ? '\0' : text_[pos_]; }

    void skip_space() {
        while (!at_end() && (peek() == ' ' || peek() == '\t' || peek() == '\n' || peek() == '\r')) {
            ++pos_;
        }
    }
    bool consume(char c) {
        if (!at_end() && peek() == c) {
            ++pos_;
            return true;
        }
        return false;
    }
    bool literal(std::string_view word) {
        if (text_.substr(pos_, word.size()) != word) {
            return false;
        }
        pos_ += word.size();
        return true;
    }
    bool digits() {
        const std::size_t start = pos_;
        while (!at_end() && peek() >= '0' && peek() <= '9') {
            ++pos_;
        }
        return pos_ > start;
    }
    bool number() {
        const std::size_t start = pos_;
        consume('-');
        if (!consume('0') && !digits()) {
            return false;
        }
        if (consume('.') && !digits()) {
            return false;
        }
        if (consume('e') || consume('E')) {
            if (!consume('+')) {
                consume('-');
            }
            if (!digits()) {
                return false;
            }
        }
        doc_.numbers.emplace_back(text_.substr(start, pos_ - start));
        return true;
    }
    bool hex4(std::uint32_t& out) {
        out = 0;
        for (int i = 0; i < 4; ++i) {
            if (at_end()) {
                return false;
            }
            const char c = text_[pos_++];
            std::uint32_t digit = 0;
            if (c >= '0' && c <= '9') {
                digit = static_cast<std::uint32_t>(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                digit = static_cast<std::uint32_t>(c - 'a' + 10);
            } else if (c >= 'A' && c <= 'F') {
                digit = static_cast<std::uint32_t>(c - 'A' + 10);
            } else {
                return false;
            }
            out = out * 16U + digit;
        }
        return true;
    }
    static void append_utf8(std::string& out, std::uint32_t code) {
        if (code < 0x80U) {
            out.push_back(static_cast<char>(code));
        } else if (code < 0x800U) {
            out.push_back(static_cast<char>(0xC0U | (code >> 6U)));
            out.push_back(static_cast<char>(0x80U | (code & 0x3FU)));
        } else if (code < 0x10000U) {
            out.push_back(static_cast<char>(0xE0U | (code >> 12U)));
            out.push_back(static_cast<char>(0x80U | ((code >> 6U) & 0x3FU)));
            out.push_back(static_cast<char>(0x80U | (code & 0x3FU)));
        } else {
            out.push_back(static_cast<char>(0xF0U | (code >> 18U)));
            out.push_back(static_cast<char>(0x80U | ((code >> 12U) & 0x3FU)));
            out.push_back(static_cast<char>(0x80U | ((code >> 6U) & 0x3FU)));
            out.push_back(static_cast<char>(0x80U | (code & 0x3FU)));
        }
    }
    bool escape(std::string& out) {
        if (at_end()) {
            return false;
        }
        const char c = text_[pos_++];
        switch (c) {
            case '"':
            case '\\':
            case '/':
                out.push_back(c);
                return true;
            case 'b':
                out.push_back('\b');
                return true;
            case 'f':
                out.push_back('\f');
                return true;
            case 'n':
                out.push_back('\n');
                return true;
            case 'r':
                out.push_back('\r');
                return true;
            case 't':
                out.push_back('\t');
                return true;
            case 'u': {
                std::uint32_t code = 0;
                if (!hex4(code)) {
                    return false;
                }
                if (code >= 0xD800U && code <= 0xDBFFU) { // high surrogate: a low one must follow
                    std::uint32_t low = 0;
                    if (!consume('\\') || !consume('u') || !hex4(low) || low < 0xDC00U ||
                        low > 0xDFFFU) {
                        return false;
                    }
                    code = 0x10000U + ((code - 0xD800U) << 10U) + (low - 0xDC00U);
                } else if (code >= 0xDC00U && code <= 0xDFFFU) {
                    return false;
                }
                append_utf8(out, code);
                return true;
            }
            default:
                return false;
        }
    }
    bool raw_utf8(std::string& out) {
        const auto lead = static_cast<unsigned char>(text_[pos_]);
        std::size_t length = 0;
        if (lead >= 0xC2U && lead <= 0xDFU) {
            length = 2;
        } else if (lead >= 0xE0U && lead <= 0xEFU) {
            length = 3;
        } else if (lead >= 0xF0U && lead <= 0xF4U) {
            length = 4;
        } else {
            return false;
        }
        const std::string_view sequence = text_.substr(pos_, length);
        if (sequence.size() != length || !is_valid_utf8(sequence)) {
            return false;
        }
        out.append(sequence);
        pos_ += length;
        return true;
    }
    bool string(std::string& out) {
        ++pos_; // opening quote
        while (!at_end()) {
            const auto byte = static_cast<unsigned char>(text_[pos_]);
            if (byte == '"') {
                ++pos_;
                return true;
            }
            if (byte < 0x20U) {
                return false; // raw control characters are illegal inside strings
            }
            if (byte == '\\') {
                ++pos_;
                if (!escape(out)) {
                    return false;
                }
            } else if (byte < 0x80U) {
                out.push_back(static_cast<char>(byte));
                ++pos_;
            } else if (!raw_utf8(out)) {
                return false;
            }
        }
        return false;
    }
    bool string_value() {
        std::string decoded;
        if (!string(decoded)) {
            return false;
        }
        doc_.strings.push_back(std::move(decoded));
        return true;
    }
    bool object(std::size_t depth) {
        doc_.max_depth = std::max(doc_.max_depth, depth);
        ++pos_; // '{'
        skip_space();
        if (consume('}')) {
            return true;
        }
        while (true) {
            skip_space();
            if (peek() != '"' || !string_value()) {
                return false;
            }
            skip_space();
            if (!consume(':')) {
                return false;
            }
            skip_space();
            if (!value(depth)) {
                return false;
            }
            skip_space();
            if (consume('}')) {
                return true;
            }
            if (!consume(',')) {
                return false;
            }
        }
    }
    bool array(std::size_t depth) {
        doc_.max_depth = std::max(doc_.max_depth, depth);
        ++pos_; // '['
        skip_space();
        if (consume(']')) {
            return true;
        }
        while (true) {
            skip_space();
            if (!value(depth)) {
                return false;
            }
            skip_space();
            if (consume(']')) {
                return true;
            }
            if (!consume(',')) {
                return false;
            }
        }
    }
    bool value(std::size_t depth) {
        switch (peek()) {
            case '{':
                return object(depth + 1);
            case '[':
                return array(depth + 1);
            case '"':
                return string_value();
            case 't':
                return literal("true");
            case 'f':
                return literal("false");
            case 'n':
                return literal("null");
            default:
                return number();
        }
    }

    std::string_view text_;
    std::size_t pos_ = 0;
    JsonDoc doc_;
};

} // namespace detail_json

/// Parses one complete JSON text; `valid` is false for anything RFC 8259 does not allow
/// (trailing commas, raw control characters, bad escapes, invalid UTF-8, trailing garbage).
inline JsonDoc parse_json(std::string_view text) {
    return detail_json::Parser(text).run();
}

} // namespace qz::console::test
