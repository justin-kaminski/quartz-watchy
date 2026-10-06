// Helpers to fill the 64-byte test detail string without heap or snprintf.
#pragma once

#include "qz/core/fixed_string.hpp"
#include "qz/core/result.hpp"
#include "qz/selftest/selftest.hpp"

#include <array>
#include <cstdint>
#include <string_view>

namespace qz::selftest {

using Detail = FixedString<64>;

/// Bounded text builder (truncates).
class Text {
public:
    Text& put(std::string_view s) noexcept {
        for (const char c : s) {
            if (len_ < buf_.size()) {
                buf_[len_++] = c;
            }
        }
        return *this;
    }
    Text& num(std::int64_t v) noexcept {
        std::array<char, 20> tmp{};
        std::size_t n = 0;
        const bool neg = v < 0;
        auto mag = static_cast<std::uint64_t>(neg ? -(v + 1) : v) + (neg ? 1U : 0U);
        do {
            tmp[n++] = static_cast<char>('0' + (mag % 10U));
            mag /= 10U;
        } while (mag != 0 && n < tmp.size());
        if (neg) {
            put("-");
        }
        while (n > 0) {
            put(std::string_view(&tmp[--n], 1));
        }
        return *this;
    }
    /// 8 uppercase hex digits.
    Text& hex32(std::uint32_t v) noexcept {
        constexpr std::string_view kDigits = "0123456789ABCDEF";
        for (int shift = 28; shift >= 0; shift -= 4) {
            put(kDigits.substr((v >> static_cast<unsigned>(shift)) & 0xFU, 1));
        }
        return *this;
    }
    /// Deci-units as "12.3".
    Text& deci(std::int64_t dv) noexcept {
        const std::int64_t a = dv < 0 ? -dv : dv;
        if (dv < 0) {
            put("-");
        }
        return num(a / 10).put(".").num(a % 10);
    }
    [[nodiscard]] std::string_view view() const noexcept { return {buf_.data(), len_}; }

private:
    std::array<char, 64> buf_{};
    std::size_t len_ = 0;
};

inline Outcome finish(Detail& d, Outcome o, std::string_view text) noexcept {
    // assign() only fails above capacity; the substr below makes that impossible.
    (void)d.assign(text.substr(0, Detail::capacity()));
    return o;
}
inline Outcome pass(Detail& d, std::string_view text = {}) noexcept {
    return finish(d, Outcome::kPass, text);
}
inline Outcome fail(Detail& d, std::string_view text) noexcept {
    return finish(d, Outcome::kFail, text);
}
inline Outcome skip(Detail& d, std::string_view text) noexcept {
    return finish(d, Outcome::kSkip, text);
}
/// "<what>: <error token>"
inline Outcome fail_error(Detail& d, std::string_view what, Error e) noexcept {
    Text t;
    t.put(what).put(": ").put(to_token(e.code));
    return finish(d, Outcome::kFail, t.view());
}

} // namespace qz::selftest
