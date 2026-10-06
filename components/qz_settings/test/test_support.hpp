// Shared helpers for the qz_settings host tests.
#pragma once

#include "qz/core/result.hpp"
#include "qz/settings/settings.hpp"

#include <array>
#include <cstddef>
#include <optional>
#include <string>

namespace qz::settings::test {

/// Error code of a failed Result/Status, nullopt on success.
template<class R>
std::optional<Errc> code_of(const R& r) {
    if (r) {
        return std::nullopt;
    }
    return r.error().code;
}

inline std::string fmt(const Settings& s, Key key) {
    std::array<char, 64> buf{};
    const std::size_t n = format_value(s, key, buf);
    return std::string(buf.data(), n);
}

inline bool face_none(std::uint8_t /*id*/) {
    return false;
}

inline bool face_below_four(std::uint8_t id) {
    return id < 4;
}

} // namespace qz::settings::test
