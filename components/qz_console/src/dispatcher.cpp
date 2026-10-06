// Request dispatch (registry.hpp): parse, look up, check, run, and frame exactly one response line.
//
// Order of checks: parse -> command lookup -> radio compiled in -> argument count -> run. The OK
// line is built in place (header, then the handler writes its JSON straight after it), so even a
// 16 KiB response is neither copied nor buffered anywhere else. Everything lives in the caller's
// buffers; nothing here allocates.
//
// Logging (ARCHITECTURE.md section 19): one debug line per accepted request, "#id name args".
// Arguments of kFlagSensitive commands are never logged, and neither are the arguments of
// requests that did not resolve to a command (they might be a mistyped sensitive command). The
// request line is zeroed after such requests, too.
#include "protocol_internal.hpp"
#include "qz/console/registry.hpp"
#include "qz/core/log.hpp"
#include "tuning.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace qz::console {
namespace {

using detail::ParseFailure;

constexpr std::string_view kEmptyObject = "{}";
constexpr std::string_view kNoId = "-";

constexpr bool has_flag(const Command& command, CommandFlag flag) noexcept {
    return (command.flags & flag) != 0;
}

/// Fixed-capacity text assembly for messages and log lines. Text that does not fit is cut.
template<std::size_t N>
class TextBuffer {
public:
    void append(std::string_view text) noexcept {
        const std::size_t count = std::min(text.size(), N - size_);
        std::ranges::copy(text.substr(0, count), buf_.data() + size_);
        size_ += count;
    }
    /// Like append(), but control characters (ESC, CR, tab, ...) become '?': a logged argument
    /// must not be able to forge a line or drive the terminal.
    void append_printable(std::string_view text) noexcept {
        constexpr unsigned char kFirstPrintable = 0x20;
        constexpr unsigned char kDelete = 0x7F;
        for (const char c : text) {
            if (size_ == N) {
                return;
            }
            const auto byte = static_cast<unsigned char>(c);
            buf_[size_++] = (byte < kFirstPrintable || byte == kDelete) ? '?' : c;
        }
    }
    void append_number(std::uint32_t value) noexcept {
        std::array<char, 10> digits{}; // UINT32_MAX has 10 digits
        std::size_t pos = digits.size();
        do {
            digits[--pos] = static_cast<char>('0' + (value % 10U));
            value /= 10U;
        } while (value != 0U);
        append(std::string_view(digits.data() + pos, digits.size() - pos));
    }
    [[nodiscard]] std::string_view view() const noexcept { return {buf_.data(), size_}; }

private:
    std::array<char, N> buf_{};
    std::size_t size_ = 0;
};

/// Text for an error that carries no message of its own (handlers return a bare Error).
constexpr std::string_view generic_message(Errc code) noexcept {
    // No default label on purpose: -Wswitch forces a new Errc to get its text.
    switch (code) {
        case Errc::kBadArgs:
            return "bad arguments";
        case Errc::kUnknownCommand:
            return "unknown command";
        case Errc::kUnsupported:
            return "not supported on this build";
        case Errc::kInvalidState:
            return "not allowed in the current state";
        case Errc::kBusy:
            return "busy";
        case Errc::kIo:
            return "i/o failure";
        case Errc::kTimeout:
            return "timed out";
        case Errc::kNotFound:
            return "not found";
        case Errc::kNoTime:
            return "wall-clock time not set";
        case Errc::kNoCredentials:
            return "no credentials stored";
        case Errc::kBatteryLow:
            return "battery too low";
        case Errc::kCorrupt:
            return "stored data corrupt";
        case Errc::kNoSpace:
            return "no space";
        case Errc::kInternal:
            return "internal error";
    }
    return "internal error";
}

/// Zeroes `bytes` through volatile writes, which the optimizer may not drop: the line is dead
/// afterwards and must not keep a credential around.
void scrub(std::span<char> bytes) noexcept {
    volatile char* cursor = bytes.data();
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        cursor[i] = '\0';
    }
}

void log_rejected([[maybe_unused]] std::string_view reason) noexcept {
    QZ_LOGD(detail::kLogTag, "rejected: %.*s", static_cast<int>(reason.size()), reason.data());
}

void log_request([[maybe_unused]] std::string_view id,
                 [[maybe_unused]] const Command& command,
                 [[maybe_unused]] std::span<const std::string_view> args) noexcept {
    if (log_level() < LogLevel::kDebug) {
        return; // do not even assemble the text
    }
    [[maybe_unused]] const std::string_view shown_id = id.empty() ? kNoId : id;
    TextBuffer<detail::kLoggedArgsBytes> text;
    if (has_flag(command, kFlagSensitive)) {
        text.append(" <arguments hidden>");
    } else {
        for (const std::string_view arg : args) {
            text.append(" ");
            text.append_printable(arg);
        }
    }
    QZ_LOGD(detail::kLogTag,
            "#%.*s %.*s%.*s",
            static_cast<int>(shown_id.size()),
            shown_id.data(),
            static_cast<int>(command.name.size()),
            command.name.data(),
            static_cast<int>(text.view().size()),
            text.view().data());
}

void log_failed([[maybe_unused]] const Command& command, [[maybe_unused]] Error error) noexcept {
    [[maybe_unused]] const std::string_view token = to_token(error.code);
    QZ_LOGD(detail::kLogTag,
            "%.*s failed: %.*s",
            static_cast<int>(command.name.size()),
            command.name.data(),
            static_cast<int>(token.size()),
            token.data());
}

/// The error line, or an empty view when `out` cannot hold even that.
std::string_view reply_error(std::span<char> out,
                             std::string_view id,
                             Error error,
                             std::string_view message) noexcept {
    return {out.data(), format_err(out, id, error, message)};
}

std::string_view
reply_usage(std::span<char> out, std::string_view id, const Command& command) noexcept {
    TextBuffer<detail::kErrorMsgBytes> message;
    message.append("usage: ");
    message.append(command.usage);
    return reply_error(out, id, Errc::kBadArgs, message.view());
}

std::string_view reply_failure(std::span<char> out,
                               std::string_view id,
                               const Command& command,
                               Error error) noexcept {
    if (error.code == Errc::kBadArgs) {
        return reply_usage(out, id, command); // the useful hint is how to call it
    }
    TextBuffer<detail::kErrorMsgBytes> message;
    message.append(generic_message(error.code));
    if (error.detail != 0) {
        message.append(" (detail ");
        message.append_number(error.detail);
        message.append(")");
    }
    return reply_error(out, id, error, message.view());
}

/// Checks a resolved command and runs it. Returns the response line (empty if even an error line
/// does not fit `out`).
std::string_view run(DeviceApi& api,
                     const Command& command,
                     std::span<const std::string_view> args,
                     bool radio_compiled,
                     std::string_view id,
                     std::span<char> out) noexcept {
    if (has_flag(command, kFlagNeedsRadio) && !radio_compiled) {
        return reply_error(out, id, Errc::kUnsupported, "radio not compiled in");
    }
    if (args.size() < command.min_args || args.size() > command.max_args) {
        return reply_usage(out, id, command);
    }
    const std::size_t header = detail::write_ok_header(out, id);
    // The shortest success line is the header plus "{}". Refuse before running a command that may
    // have side effects when its answer cannot be delivered at all.
    if (header == 0 || out.size() - header < kEmptyObject.size()) {
        return reply_error(out, id, Errc::kNoSpace, "response buffer too small");
    }
    JsonWriter json(out.subspan(header));
    json.begin_object();
    const Status status = command.handler(api, args, json);
    if (!status) {
        log_failed(command, status.error());
        return reply_failure(out, id, command, status.error()); // partial JSON is discarded
    }
    if (json.overflowed()) {
        return reply_error(out, id, Errc::kNoSpace, "response too large");
    }
    if (json.depth() != 1) {
        return reply_error(out, id, Errc::kInternal, "handler left its JSON unbalanced");
    }
    json.end_object();
    if (json.overflowed()) {
        return reply_error(out, id, Errc::kNoSpace, "response too large");
    }
    return {out.data(), header + json.view().size()};
}

} // namespace

Dispatcher::Dispatcher(const Registry& registry, DeviceApi& api, bool radio_compiled) noexcept
    : registry_(registry), api_(api), radio_compiled_(radio_compiled) {}

std::string_view Dispatcher::handle_line(std::span<char> line, std::span<char> response) noexcept {
    const std::span<char> out = response.first(std::min(response.size(), kMaxResponseBytes));
    Request request;
    const ParseFailure failure = detail::parse_line(line, request);
    const std::string_view id = request.id.view();
    if (failure != ParseFailure::kNone) {
        const std::string_view reason = detail::describe(failure);
        log_rejected(reason);
        scrub(line); // may hold the start of a credential, and nobody can tell which command
        return reply_error(out, id, Errc::kBadArgs, reason);
    }
    std::size_t words = 0;
    const Command* const command = registry_.find(request.tokens.span(), &words);
    if (command == nullptr) {
        log_rejected("unknown command");
        scrub(line);
        return reply_error(out, id, Errc::kUnknownCommand, "unknown command, try help");
    }
    const std::span<const std::string_view> args = request.tokens.span().subspan(words);
    log_request(id, *command, args);
    const std::string_view reply = run(api_, *command, args, radio_compiled_, id, out);
    if (has_flag(*command, kFlagSensitive)) {
        scrub(line);
    }
    return reply;
}

} // namespace qz::console
