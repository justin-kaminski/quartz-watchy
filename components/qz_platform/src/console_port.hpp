// hal::ConsolePort over the USB-Serial-JTAG driver (ARCHITECTURE.md sections 16, 17). Private to
// qz_platform: includes IDF headers on purpose.
#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "platform_impl.hpp"
#include "qz/hal/system.hpp"

#include <array>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace qz::platform {

/// Line-buffered console on usb_serial_jtag_{read,write}_bytes (no esp_console REPL: the
/// dispatcher already tokenizes lines).
///
/// Lifecycle: start() installs the driver and routes ESP_LOG output through the same output
/// mutex (esp_log_set_vprintf hook), so a log line can never land inside a protocol line and vice
/// versa; stop() flushes (bounded), uninstalls the driver and gives logging back to the default
/// vprintf. While running, IdfSleep is told it is tethered (light sleep would drop the USB link).
///
/// No-host behaviour (never blocks, never keeps the watch awake):
///  - usb_serial_jtag_is_connected() (SOF-based; false once no SOF arrived for ~3 ms) is checked
///    before every write: with no host the data is dropped immediately.
///  - A host that is connected but not reading fills the 4 KiB TX ring; each 256-byte chunk then
///    waits at most kTxTimeoutMs, after which output is dropped for kStallBackoffUs.
///  - Reads wait only for the caller's timeout.
/// The driver and its ISR exist only between start() and stop(); the app only starts it while USB
/// is present (TetherPolicy), so on battery there is no driver, no interrupt and no hook work.
class IdfConsolePort final : public hal::ConsolePort {
public:
    static constexpr std::uint32_t kTxBufferBytes = 4096; ///< driver TX ring (must exceed 1 chunk)
    static constexpr std::uint32_t kRxBufferBytes = 512;  ///< driver RX ring (> 64 required)
    static constexpr std::size_t kChunkBytes = 256;       ///< per usb_serial_jtag_write_bytes call
    static constexpr std::uint32_t kTxTimeoutMs = 100;    ///< wait for ring space, per chunk
    static constexpr std::uint32_t kFlushTimeoutMs = 100; ///< stop(): wait for the host to drain
    static constexpr std::int64_t kStallBackoffUs = 2'000'000;
    static constexpr std::size_t kLineBytes = 512;    ///< accumulator; longer lines overflow
    static constexpr std::size_t kLogLineBytes = 256; ///< stack buffer per ESP_LOG line

    explicit IdfConsolePort(IdfSleep& sleep) noexcept;
    IdfConsolePort(const IdfConsolePort&) = delete;
    IdfConsolePort& operator=(const IdfConsolePort&) = delete;

    Status start() override;
    void stop() override;
    Result<std::size_t> receive_line(std::span<char> out, std::uint32_t timeout_ms) override;
    void send_line(std::string_view line) override;

private:
    static int log_vprintf(const char* fmt, va_list args);
    int log_line(const char* fmt, va_list args);
    /// Mutex held. Writes `body` (+ '\n'); finishes a previous unterminated fragment first.
    bool emit_locked(std::string_view body, bool add_newline);
    /// Mutex held. Chunked write; arms the stall back-off on failure.
    bool write_raw_locked(const char* data, std::size_t len);

    IdfSleep& sleep_;
    StaticSemaphore_t mutex_storage_{};
    SemaphoreHandle_t mutex_ = nullptr; ///< recursive; guards running_, mid_line_, stall_until_us_
    bool running_ = false;
    bool hook_installed_ = false;
    bool mid_line_ = false; ///< last bytes written did not end with '\n'
    std::int64_t stall_until_us_ = 0;

    // Receive side: app task only.
    std::array<char, kLineBytes> line_{};
    std::size_t line_len_ = 0;
    bool overflow_ = false;
    std::array<std::uint8_t, 64> rx_{};
    std::size_t rx_pos_ = 0;
    std::size_t rx_end_ = 0;
};

} // namespace qz::platform
