// Console protocol v1 (ARCHITECTURE.md section 16): request tokenizer and response framing.
//
// Request grammar (whitespace = space, tab, CR, LF):
//   line  = ws* [ "#" id ws+ ] token ( ws+ token )* ws*
//   id    = 1-8 of [A-Za-z0-9]
//   token = bare | quoted
//   bare  = any characters but whitespace and '"'     (a backslash is an ordinary character)
//   quoted = '"' ( any char but '"' '\\' | "\\\"" | "\\\\" )* '"'   (followed by whitespace or the
//   end)
// A quoted token may be empty. Anything else is malformed (kBadArgs): an unterminated quote, a
// backslash escape other than \" and \\, a quote inside a bare word, text glued to a closing quote.
// Tokens are decoded in place and are views into the caller's line.
#include "qz/console/protocol.hpp"

#include "protocol_internal.hpp"
#include "tuning.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>

namespace qz::console {
namespace {

static_assert(decltype(Request::id)::capacity() == detail::kMaxIdChars,
              "Request::id must hold a full id");

constexpr std::string_view kNoId = "-";
constexpr std::string_view kOkVerb = " OK ";
constexpr std::string_view kErrVerb = " ERR ";
constexpr std::string_view kEventVerb = "! EVT ";

/// Appends pieces to a fixed buffer; a piece that does not fit is refused whole.
class LineBuilder {
public:
    explicit LineBuilder(std::span<char> out) noexcept : out_(out) {}

    bool append(std::string_view text) noexcept {
        if (text.size() > out_.size() - size_) {
            return false;
        }
        if (!text.empty()) {
            // memmove: `text` may be a view into `out` (a caller re-framing its own output).
            std::memmove(out_.data() + size_, text.data(), text.size());
        }
        size_ += text.size();
        return true;
    }
    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] std::span<char> rest() const noexcept { return out_.subspan(size_); }

private:
    std::span<char> out_;
    std::size_t size_ = 0;
};

/// What goes in the id position: the id, or "-" when there is none or it would break the framing.
std::string_view shown_id(std::string_view id) noexcept {
    return detail::is_valid_id(id) ? id : kNoId;
}

} // namespace

namespace detail {
namespace {

constexpr char kIdMarker = '#';
constexpr char kQuote = '"';
constexpr char kBackslash = '\\';

/// Whitespace between tokens. CR and LF are accepted so that a CRLF terminal works.
constexpr bool is_space(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

constexpr bool is_id_char(char c) noexcept {
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

std::size_t skip_space(std::span<const char> line, std::size_t pos) noexcept {
    while (pos < line.size() && is_space(line[pos])) {
        ++pos;
    }
    return pos;
}

/// "#id" at line[pos]: 1-8 id characters, then whitespace or the end of the line.
ParseFailure parse_id(std::span<const char> line, std::size_t& pos, Request& request) noexcept {
    const std::size_t first = pos + 1;
    std::size_t end = first;
    while (end < line.size() && is_id_char(line[end])) {
        ++end;
    }
    const std::size_t length = end - first;
    const bool terminated = end == line.size() || is_space(line[end]);
    if (length == 0 || length > kMaxIdChars || !terminated) {
        return ParseFailure::kBadId;
    }
    const bool fits = request.id.assign(std::string_view(line.data() + first, length));
    QZ_ASSERT(fits); // length <= kMaxIdChars == the capacity of Request::id
    pos = end;
    return ParseFailure::kNone;
}

/// A quoted token starting at line[pos]. Its text is decoded in place: decoding only ever
/// shortens it, so the write position stays behind the read position.
ParseFailure
parse_quoted(std::span<char> line, std::size_t& pos, std::string_view& token) noexcept {
    const std::size_t start = pos;
    std::size_t write = start;
    std::size_t read = start + 1;
    bool closed = false;
    while (!closed) {
        if (read >= line.size()) {
            return ParseFailure::kUnterminatedQuote;
        }
        const char c = line[read];
        if (c == kQuote) {
            closed = true;
            ++read;
        } else if (c == kBackslash) {
            if (read + 1 >= line.size()) {
                return ParseFailure::kUnterminatedQuote; // the backslash swallows the end
            }
            const char escaped = line[read + 1];
            if (escaped != kQuote && escaped != kBackslash) {
                return ParseFailure::kBadEscape;
            }
            line[write++] = escaped;
            read += 2;
        } else {
            line[write++] = c;
            ++read;
        }
    }
    if (read < line.size() && !is_space(line[read])) {
        return ParseFailure::kStrayQuote;
    }
    token = std::string_view(line.data() + start, write - start);
    pos = read;
    return ParseFailure::kNone;
}

/// A bare word starting at line[pos]: everything up to whitespace, which may not contain a quote.
ParseFailure
parse_bare(std::span<const char> line, std::size_t& pos, std::string_view& token) noexcept {
    std::size_t end = pos;
    while (end < line.size() && !is_space(line[end])) {
        if (line[end] == kQuote) {
            return ParseFailure::kStrayQuote;
        }
        ++end;
    }
    token = std::string_view(line.data() + pos, end - pos);
    pos = end;
    return ParseFailure::kNone;
}

ParseFailure tokenize(std::span<char> line, std::size_t start, Request& request) noexcept {
    std::size_t pos = skip_space(line, start);
    while (pos < line.size()) {
        std::string_view token;
        const ParseFailure failure =
            line[pos] == kQuote ? parse_quoted(line, pos, token) : parse_bare(line, pos, token);
        if (failure != ParseFailure::kNone) {
            return failure;
        }
        if (!request.tokens.push_back(token)) {
            return ParseFailure::kTooManyTokens;
        }
        pos = skip_space(line, pos);
    }
    return ParseFailure::kNone;
}

} // namespace

bool is_valid_id(std::string_view id) noexcept {
    return !id.empty() && id.size() <= kMaxIdChars && std::ranges::all_of(id, is_id_char);
}

ParseFailure parse_line(std::span<char> line, Request& request) noexcept {
    request = Request{};
    if (line.size() > kMaxRequestBytes) {
        return ParseFailure::kTooLong;
    }
    std::size_t pos = skip_space(line, 0);
    if (pos == line.size()) {
        return ParseFailure::kEmpty;
    }
    if (line[pos] == kIdMarker) {
        const ParseFailure id_failure = parse_id(line, pos, request);
        if (id_failure != ParseFailure::kNone) {
            return id_failure;
        }
    }
    const ParseFailure failure = tokenize(line, pos, request);
    if (failure != ParseFailure::kNone) {
        return failure;
    }
    return request.tokens.empty() ? ParseFailure::kNoCommand : ParseFailure::kNone;
}

std::string_view describe(ParseFailure failure) noexcept {
    // No default label on purpose: -Wswitch forces a new reason to get its text.
    switch (failure) {
        case ParseFailure::kNone:
            return "no error";
        case ParseFailure::kEmpty:
            return "empty request";
        case ParseFailure::kNoCommand:
            return "missing command";
        case ParseFailure::kTooLong:
            return "request too long";
        case ParseFailure::kBadId:
            return "bad request id, use 1-8 letters or digits";
        case ParseFailure::kUnterminatedQuote:
            return "unterminated quote";
        case ParseFailure::kBadEscape:
            return R"(bad escape, only \" and \\ exist)";
        case ParseFailure::kStrayQuote:
            return "a quote must open an argument and be followed by a space";
        case ParseFailure::kTooManyTokens:
            return "too many arguments";
    }
    return "malformed request";
}

std::size_t write_ok_header(std::span<char> out, std::string_view id) noexcept {
    LineBuilder line(out);
    if (!line.append(kPrefix) || !line.append(shown_id(id)) || !line.append(kOkVerb)) {
        return 0;
    }
    return line.size();
}

} // namespace detail

Result<Request> parse_request(std::span<char> line) noexcept {
    Request request;
    const detail::ParseFailure failure = detail::parse_line(line, request);
    if (failure != detail::ParseFailure::kNone) {
        return Error{Errc::kBadArgs, static_cast<std::uint16_t>(failure)};
    }
    return request;
}

std::size_t format_ok(std::span<char> out, std::string_view id, std::string_view json) noexcept {
    LineBuilder line(out);
    if (!line.append(kPrefix) || !line.append(shown_id(id)) || !line.append(kOkVerb) ||
        !line.append(json)) {
        return 0;
    }
    return line.size();
}

std::size_t
format_err(std::span<char> out, std::string_view id, Error err, std::string_view msg) noexcept {
    LineBuilder line(out);
    if (!line.append(kPrefix) || !line.append(shown_id(id)) || !line.append(kErrVerb) ||
        !line.append(to_token(err.code)) || !line.append(" ")) {
        return 0;
    }
    JsonWriter json(line.rest());
    json.begin_object().field("msg", msg).end_object();
    return json.overflowed() ? 0 : line.size() + json.view().size();
}

std::size_t format_event(std::span<char> out, std::string_view json) noexcept {
    LineBuilder line(out);
    if (!line.append(kPrefix) || !line.append(kEventVerb) || !line.append(json)) {
        return 0;
    }
    return line.size();
}

} // namespace qz::console
