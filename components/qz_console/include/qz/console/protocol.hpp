// Console line protocol v1 (ARCHITECTURE.md section 16). Pure, heap-free.
#pragma once

#include "qz/core/containers.hpp"
#include "qz/core/fixed_string.hpp"
#include "qz/core/result.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace qz::console {

inline constexpr std::string_view kPrefix = "@QZ1 ";
inline constexpr int kProtocolVersion = 1;
inline constexpr std::size_t kMaxRequestBytes = 256;
inline constexpr std::size_t kMaxResponseBytes = 16'384;
inline constexpr std::size_t kMaxArgs = 12;

/// Parsed request; views point into the caller's line buffer (tokenized in place).
struct Request {
    FixedString<8> id;                                   ///< empty = "-"
    StaticVector<std::string_view, kMaxArgs + 2> tokens; ///< command words + args
};

/// "[#id ]tokens..." with "..." quoting and \" \\ escapes. kBadArgs on malformed input.
Result<Request> parse_request(std::span<char> line) noexcept;

/// Streaming single-line JSON writer into a fixed buffer. Strings are escaped; on overflow the
/// writer latches overflowed() and the dispatcher answers ERR no_space.
class JsonWriter {
public:
    explicit JsonWriter(std::span<char> buffer) noexcept;
    JsonWriter& begin_object() noexcept;
    JsonWriter& end_object() noexcept;
    JsonWriter& begin_array(std::string_view key = {}) noexcept;
    JsonWriter& end_array() noexcept;
    JsonWriter& key(std::string_view k) noexcept;
    JsonWriter& str(std::string_view v) noexcept;
    JsonWriter& num(std::int64_t v) noexcept;
    JsonWriter& boolean(bool v) noexcept;
    JsonWriter& null() noexcept;
    /// Base64 (RFC 4648, no line breaks) string value.
    JsonWriter& base64(std::span<const std::uint8_t> bytes) noexcept;
    // Convenience: key + value.
    JsonWriter& field(std::string_view k, std::string_view v) noexcept { return key(k).str(v); }
    JsonWriter& field(std::string_view k, std::int64_t v) noexcept { return key(k).num(v); }
    JsonWriter& field_bool(std::string_view k, bool v) noexcept { return key(k).boolean(v); }
    [[nodiscard]] bool overflowed() const noexcept;
    [[nodiscard]] std::string_view view() const noexcept;

private:
    std::span<char> buf_;
    std::size_t len_ = 0;
    std::uint32_t need_comma_ = 0; ///< bit per nesting level
    std::uint8_t depth_ = 0;
    bool overflow_ = false;
};

/// "@QZ1 <id|-> OK <json>" ; returns length written (truncated lines are never emitted).
std::size_t format_ok(std::span<char> out, std::string_view id, std::string_view json) noexcept;
/// "@QZ1 <id|-> ERR <token> {"msg":"..."}"
std::size_t
format_err(std::span<char> out, std::string_view id, Error err, std::string_view msg) noexcept;
/// "@QZ1 ! EVT <json>"
std::size_t format_event(std::span<char> out, std::string_view json) noexcept;

} // namespace qz::console
