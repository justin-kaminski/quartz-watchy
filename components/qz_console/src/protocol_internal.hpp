// Parser and framing internals shared by protocol.cpp and dispatcher.cpp. Private to qz_console.
#pragma once

#include "qz/console/protocol.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace qz::console::detail {

/// Why a request line was rejected. parse_request() reports it as Error::detail of its kBadArgs
/// result; the dispatcher turns it into the `msg` of the error response. The values are private to
/// the component, not part of the wire protocol.
enum class ParseFailure : std::uint8_t {
    kNone = 0,
    kEmpty,             ///< blank line
    kNoCommand,         ///< an id and nothing else
    kTooLong,           ///< more than kMaxRequestBytes
    kBadId,             ///< "#" not followed by 1-8 of [A-Za-z0-9] and whitespace
    kUnterminatedQuote, ///< opening quote without a closing one (a trailing backslash included)
    kBadEscape,         ///< backslash followed by anything but '"' or '\\'
    kStrayQuote,        ///< quote inside a bare word, or text glued to a closing quote
    kTooManyTokens,     ///< more than kMaxArgs + 2 tokens
};

/// parse_request() with the reason. `request` is reset first and filled as far as parsing got, so
/// a valid request id is available even when a later token is malformed.
[[nodiscard]] ParseFailure parse_line(std::span<char> line, Request& request) noexcept;

/// Human-readable reason for the `msg` of the error response. Never empty.
[[nodiscard]] std::string_view describe(ParseFailure failure) noexcept;

/// 1-8 characters of [A-Za-z0-9].
[[nodiscard]] bool is_valid_id(std::string_view id) noexcept;

/// "@QZ1 <id|-> OK " written at the start of `out` (an empty or invalid id is shown as "-").
/// Returns the bytes written, 0 when `out` is too small. The dispatcher continues the line in
/// place after it, so a response of up to 16 KiB is never copied.
[[nodiscard]] std::size_t write_ok_header(std::span<char> out, std::string_view id) noexcept;

} // namespace qz::console::detail
