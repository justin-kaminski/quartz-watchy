// hal::ConsolePort over the USB-Serial-JTAG driver. See console_port.hpp for the design.
//
// Verified in the IDF source:
//  - usb_serial_jtag_driver_install requires rx_buffer_size > 64 and tx_buffer_size > 0, allocates
//    ring buffers and the ISR, and fails with ESP_ERR_INVALID_STATE if already installed
//    [IDF:components/esp_driver_usb_serial_jtag/src/usb_serial_jtag.c].
//  - usb_serial_jtag_write_bytes copies into the TX ring and returns 0 if no space appeared within
//    ticks_to_wait; with no host the ring never drains, so the wait is the only blocking point
//    (same file). usb_serial_jtag_read_bytes returns 0 after ticks_to_wait without data.
//  - usb_serial_jtag_is_connected() is the SOF-watching tick-hook status, initially true
//    [IDF:components/esp_driver_usb_serial_jtag/src/usb_serial_jtag_connection_monitor.c].
//  - The stdout VFS path (used by the default vprintf) drops fast when not connected
//    [IDF:components/esp_driver_usb_serial_jtag/src/usb_serial_jtag_vfs.c usb_serial_jtag_write].
//  - CONFIG_LOG_VERSION_1 (this project's setting): esp_log_va makes ONE vprintf call per log line,
//    prefix and trailing newline included [IDF:components/log/src/log.c esp_log_va]. (Log
//    version 2 would split a line into prefix / message / newline calls; emit_locked's mid_line_
//    handling still keeps protocol lines intact then, only the log line could be split.)
//  - esp_log_set_vprintf returns the previous handler [IDF:components/log/include/esp_log_write.h].
#include "console_port.hpp"

#include "driver/usb_serial_jtag.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/task.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace qz::platform {
namespace {

IdfConsolePort* g_port = nullptr;
/// Log handler to fall back to while the console is not running. Default IDF handler is vprintf.
vprintf_like_t g_default_vprintf = &vprintf;

constexpr TickType_t to_ticks(std::uint32_t ms) noexcept {
    return pdMS_TO_TICKS(ms);
}

} // namespace

IdfConsolePort::IdfConsolePort(IdfSleep& sleep) noexcept : sleep_(sleep) {
    mutex_ = xSemaphoreCreateRecursiveMutexStatic(&mutex_storage_);
    g_port = this;
}

Status IdfConsolePort::start() {
    xSemaphoreTakeRecursive(mutex_, portMAX_DELAY);
    if (running_) {
        xSemaphoreGiveRecursive(mutex_);
        return Status{};
    }
    usb_serial_jtag_driver_config_t cfg{};
    cfg.tx_buffer_size = kTxBufferBytes;
    cfg.rx_buffer_size = kRxBufferBytes;
    const esp_err_t err = usb_serial_jtag_driver_install(&cfg);
    if (err != ESP_OK) {
        xSemaphoreGiveRecursive(mutex_);
        return to_error(err);
    }
    line_len_ = 0;
    overflow_ = false;
    rx_pos_ = 0;
    rx_end_ = 0;
    mid_line_ = false;
    stall_until_us_ = 0;
    running_ = true;
    sleep_.set_tethered(true);
    xSemaphoreGiveRecursive(mutex_);
    if (!hook_installed_) {
        // The hook stays installed for good (no race with other tasks logging); while the port
        // is stopped it forwards to the previous handler. g_default_vprintf already holds
        // vprintf, so a call arriving before the assignment below is still routed correctly.
        g_default_vprintf = esp_log_set_vprintf(&IdfConsolePort::log_vprintf);
        hook_installed_ = true;
    }
    return Status{};
}

void IdfConsolePort::stop() {
    xSemaphoreTakeRecursive(mutex_, portMAX_DELAY);
    if (running_) {
        running_ = false;
        if (usb_serial_jtag_is_connected()) {
            // Bounded wait so the last response/event leaves before the driver disappears.
            (void)usb_serial_jtag_wait_tx_done(to_ticks(kFlushTimeoutMs));
        }
        (void)usb_serial_jtag_driver_uninstall();
        sleep_.set_tethered(false);
    }
    xSemaphoreGiveRecursive(mutex_);
}

bool IdfConsolePort::write_raw_locked(const char* data, std::size_t len) {
    while (len > 0) {
        const std::size_t n = std::min(len, kChunkBytes);
        if (usb_serial_jtag_write_bytes(data, n, to_ticks(kTxTimeoutMs)) <= 0) {
            stall_until_us_ = esp_timer_get_time() + kStallBackoffUs;
            return false;
        }
        data += n;
        len -= n;
    }
    return true;
}

bool IdfConsolePort::emit_locked(std::string_view body, bool add_newline) {
    if (!running_ || !usb_serial_jtag_is_connected() || esp_timer_get_time() < stall_until_us_) {
        return false; // no host / host not draining: drop, never wait
    }
    if (mid_line_) {
        // An earlier fragment (truncated log line, aborted write) never got its '\n': end it so
        // protocol lines always start at column 0.
        mid_line_ = false;
        if (!write_raw_locked("\n", 1)) {
            mid_line_ = true;
            return false;
        }
    }
    mid_line_ = true;
    if (!body.empty() && !write_raw_locked(body.data(), body.size())) {
        return false;
    }
    if (add_newline) {
        if (!write_raw_locked("\n", 1)) {
            return false;
        }
        mid_line_ = false;
    } else {
        mid_line_ = !body.empty() && body.back() != '\n';
    }
    return true;
}

void IdfConsolePort::send_line(std::string_view line) {
    xSemaphoreTakeRecursive(mutex_, portMAX_DELAY);
    (void)emit_locked(line, true);
    xSemaphoreGiveRecursive(mutex_);
}

int IdfConsolePort::log_vprintf(const char* fmt, va_list args) {
    IdfConsolePort* const port = g_port;
    if (port == nullptr || xPortInIsrContext() != 0 ||
        xTaskGetSchedulerState() != taskSCHEDULER_RUNNING) {
        return g_default_vprintf(fmt, args);
    }
    return port->log_line(fmt, args);
}

int IdfConsolePort::log_line(const char* fmt, va_list args) {
    xSemaphoreTakeRecursive(mutex_, portMAX_DELAY);
    int result = 0;
    if (!running_) {
        result = g_default_vprintf(fmt, args);
    } else {
        char buf[kLogLineBytes];
        const int n = vsnprintf(buf, sizeof(buf), fmt, args);
        result = n < 0 ? 0 : n;
        std::size_t len = static_cast<std::size_t>(result);
        if (len >= sizeof(buf)) {
            len = sizeof(buf) - 1;
            buf[len - 1] = '\n'; // truncated: still terminate the line
        }
        (void)emit_locked(std::string_view(buf, len), false);
    }
    xSemaphoreGiveRecursive(mutex_);
    return result;
}

Result<std::size_t> IdfConsolePort::receive_line(std::span<char> out, std::uint32_t timeout_ms) {
    if (!running_) {
        vTaskDelay(to_ticks(std::min<std::uint32_t>(timeout_ms, 100)) + 1);
        return std::size_t{0};
    }
    const std::int64_t deadline_us =
        esp_timer_get_time() + static_cast<std::int64_t>(timeout_ms) * 1000;
    for (;;) {
        while (rx_pos_ < rx_end_) {
            const char c = static_cast<char>(rx_[rx_pos_++]);
            if (c == '\n' || c == '\r') {
                if (overflow_) {
                    overflow_ = false;
                    line_len_ = 0;
                    return Error{Errc::kNoSpace};
                }
                if (line_len_ == 0) {
                    continue; // empty line or the '\n' of a CRLF pair
                }
                const std::size_t n = line_len_;
                line_len_ = 0;
                if (n > out.size()) {
                    return Error{Errc::kNoSpace};
                }
                std::memcpy(out.data(), line_.data(), n);
                return n;
            }
            if (line_len_ < line_.size()) {
                line_[line_len_++] = c;
            } else {
                overflow_ = true;
            }
        }
        const std::int64_t remaining_us = deadline_us - esp_timer_get_time();
        if (remaining_us <= 0) {
            return std::size_t{0};
        }
        const auto remaining_ms = static_cast<std::uint32_t>((remaining_us + 999) / 1000);
        const TickType_t ticks = std::max<TickType_t>(to_ticks(remaining_ms), 1);
        const int got = usb_serial_jtag_read_bytes(rx_.data(), rx_.size(), ticks);
        if (got > 0) {
            rx_pos_ = 0;
            rx_end_ = static_cast<std::size_t>(got);
        }
    }
}

} // namespace qz::platform
