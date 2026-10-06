// Private declarations of the ESP-IDF backed HAL classes implemented by WP-24 (qz_platform).
// IdfPlatform (WP-25/28) owns one instance of each in static storage. This header includes IDF
// headers on purpose and must never be exposed through the component's public include dir.
#pragma once

#include "driver/i2c_master.h"
#include "driver/spi_master.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_err.h"
#include "nvs.h"
#include "qz/board/watchy_v3.hpp"
#include "qz/core/result.hpp"
#include "qz/hal/board_io.hpp"
#include "qz/hal/delay.hpp"
#include "qz/hal/epd_bus.hpp"
#include "qz/hal/i2c.hpp"
#include "qz/hal/kv_store.hpp"
#include "qz/hal/system.hpp"
#include "qz/platform/idf_platform.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace qz::platform {

/// Maps an esp_err_t to a qz::Error (detail = low 16 bits of the esp_err_t).
/// ESP_ERR_NVS_* codes are [IDF:components/nvs_flash/include/nvs.h].
[[nodiscard]] Error to_error(esp_err_t err) noexcept;

/// GPIO-backed BoardIo. Call init() once (IdfPlatform::init) before use.
class IdfBoardIo final : public hal::BoardIo {
public:
    IdfBoardIo() noexcept = default;
    IdfBoardIo(const IdfBoardIo&) = delete;
    IdfBoardIo& operator=(const IdfBoardIo&) = delete;
    ~IdfBoardIo() override;

    /// Drives the vibration pad low and releases any deep-sleep pad hold glitch-free, then
    /// configures buttons, USB detect and charge status as inputs without internal pulls.
    Status init() noexcept;

    [[nodiscard]] std::uint8_t pressed_buttons() const override;
    [[nodiscard]] bool usb_present() const override;
    [[nodiscard]] bool charging() const override;
    void set_vibration(bool on) override;
};

/// Battery-pin ADC (ADC1 oneshot + curve-fitting calibration). Hardware is set up lazily on the
/// first read so wakes that never sample the battery pay nothing. Not thread-safe.
class IdfAdc final : public hal::Adc {
public:
    IdfAdc() noexcept = default;
    IdfAdc(const IdfAdc&) = delete;
    IdfAdc& operator=(const IdfAdc&) = delete;
    ~IdfAdc() override;

    Result<std::uint16_t> read_pin_mv() override;

private:
    Status ensure_init() noexcept;

    adc_oneshot_unit_handle_t unit_ = nullptr;
    adc_cali_handle_t cali_ = nullptr;
    adc_channel_t channel_ = ADC_CHANNEL_0;
};

/// esp_rom_delay_us for short waits, vTaskDelay for long ones.
class IdfDelay final : public hal::Delay {
public:
    void delay_us(std::uint32_t us) override;
    void delay_ms(std::uint32_t ms) override;
};

/// RTC timer time base plus libc clock push.
class IdfClock final : public hal::Clock {
public:
    [[nodiscard]] std::int64_t rtc_us() const override;
    void set_system_utc_us(std::int64_t utc_us) override;
};

// Sizes of the RTC_NOINIT regions (kRtcStateRegionBytes / kRtcFrameRegionBytes) are public
// constants in qz/platform/idf_platform.hpp so main/ can static_assert them against
// sizeof(app::RtcState) / sizeof(app::FrameShadow).

/// Byte regions in RTC_NOINIT memory (survive deep sleep and software resets; undefined after
/// power-on, so the owner validates magic/version/CRC).
class IdfRtcMemory final : public hal::RtcMemory {
public:
    [[nodiscard]] std::span<std::uint8_t> state_region() override;
    [[nodiscard]] std::span<std::uint8_t> frame_region() override;
};

/// Reset reason, wake sources, slow clock info, RNG, heap. The constructor captures the RTC time,
/// reset reason and wake causes, so construct it first thing in app_main.
class IdfSystem final : public hal::System {
public:
    IdfSystem() noexcept;

    [[nodiscard]] hal::ResetReason reset_reason() const override { return reset_reason_; }
    [[nodiscard]] hal::WakeSources wake_sources() const override { return wake_; }
    [[nodiscard]] std::int64_t boot_rtc_us() const override { return boot_rtc_us_; }
    [[nodiscard]] hal::SlowClockInfo slow_clock() const override;
    [[nodiscard]] hal::FirmwareInfo firmware() const override;
    [[nodiscard]] std::uint64_t chip_id() const override;
    std::uint32_t random_u32() override;
    [[nodiscard]] hal::HeapInfo heap() const override;
    void restart() override;

private:
    hal::ResetReason reset_reason_ = hal::ResetReason::kOther;
    hal::WakeSources wake_{};
    std::int64_t boot_rtc_us_ = 0;
};

/// hal::KvStore over the default NVS partition. NVS is initialized lazily on first use.
/// Every operation opens and closes its own handle (no handle cache, no static storage).
class IdfKvStore final : public hal::KvStore {
public:
    /// Longest string (including its terminating NUL) get_str/set_str support.
    static constexpr std::size_t kMaxStrBytes = 256;

    IdfKvStore() noexcept = default;

    /// Initializes NVS if needed. nvs_flash_init is idempotent
    /// [IDF:components/nvs_flash/src/nvs_partition_manager.cpp init_partition]. If the partition
    /// has no free pages or was written by a newer NVS format, it is erased and re-initialized (the
    /// IDF-documented recovery; NVS loss = factory reset, docs/PARTITIONS.md). Call
    /// erased_on_init() afterwards to detect that.
    Status ensure_init() noexcept;
    /// True if ensure_init() had to erase the partition (all stored data lost).
    [[nodiscard]] bool erased_on_init() const noexcept { return erased_; }

    Result<std::uint32_t> get_u32(std::string_view ns, std::string_view key) override;
    Status set_u32(std::string_view ns, std::string_view key, std::uint32_t value) override;
    Result<std::int32_t> get_i32(std::string_view ns, std::string_view key) override;
    Status set_i32(std::string_view ns, std::string_view key, std::int32_t value) override;
    Result<std::int64_t> get_i64(std::string_view ns, std::string_view key) override;
    Status set_i64(std::string_view ns, std::string_view key, std::int64_t value) override;
    Result<std::size_t>
    get_str(std::string_view ns, std::string_view key, std::span<char> out) override;
    Status set_str(std::string_view ns, std::string_view key, std::string_view value) override;
    Result<std::size_t>
    get_blob(std::string_view ns, std::string_view key, std::span<std::uint8_t> out) override;
    Status set_blob(std::string_view ns,
                    std::string_view key,
                    std::span<const std::uint8_t> value) override;
    Status erase_key(std::string_view ns, std::string_view key) override;
    Status erase_namespace(std::string_view ns) override;
    /// NVS writes reach flash inside nvs_set_*; nvs_commit is a documented no-op
    /// [IDF:components/nvs_flash/src/nvs_api.cpp nvs_commit]. Ensures NVS is initialized.
    Status commit() override;

private:
    bool inited_ = false;
    bool erased_ = false;
};

// ---- WP-25: sleep/wake and buses ----------------------------------------------------------------

/// Deep/light sleep control (ARCHITECTURE.md section 5). Single instance, app task only.
class IdfSleep final : public hal::SleepControl {
public:
    /// Shortest timer wake accepted by deep sleep (shorter requests are raised to this value) so
    /// esp_deep_sleep_try_to_start never rejects a plan for a timer that already elapsed.
    static constexpr std::int64_t kMinDeepSleepTimerUs = 2000;
    /// Light sleeps shorter than this are polled instead (sleep entry/exit overhead dominates).
    static constexpr std::int64_t kMinLightSleepUs = 3000;

    explicit IdfSleep(hal::BoardIo& io) noexcept : io_(io) {}
    IdfSleep(const IdfSleep&) = delete;
    IdfSleep& operator=(const IdfSleep&) = delete;

    /// Tethered (USB powered, console up): light_sleep() polls instead of entering light sleep so
    /// the USB-Serial-JTAG link stays up.
    void set_tethered(bool tethered) noexcept { tethered_ = tethered; }
    [[nodiscard]] bool tethered() const noexcept { return tethered_; }

    /// Arms the wake sources of `plan`, parks the pins per ARCHITECTURE.md section 5 and enters
    /// deep sleep. Does not return unless the IDF rejects the sleep request, in which case all
    /// holds are released and the chip restarts (esp_restart).
    void deep_sleep(const hal::SleepPlan& plan) override;
    hal::LightSleepWake light_sleep(const hal::SleepPlan& plan) override;

    /// Undoes everything deep_sleep() latched: drives the parked outputs to their parked level
    /// first, then drops the per-pad holds and the deep-sleep autohold flag, and returns the
    /// battery pad to the analog state. Idempotent; call once at boot BEFORE the SPI bus or any
    /// output driver is initialized (it reconfigures GPIO17/33/34/35/47/48 as plain outputs).
    static Status release_holds() noexcept;

private:
    hal::BoardIo& io_;
    bool tethered_ = false;
    bool outputs_kept_in_light_sleep_ = false;
};

/// SSD1681 bus: SPI2 master (write only, mode 0, board::kSpiHz), DC/CS/RST/BUSY handling.
/// CS is the SPI peripheral's hardware CS; DC is a plain GPIO set before each transfer.
class IdfEpdBus final : public hal::EpdBus {
public:
    /// Reset timing [ASSUMED] generous (ssd1681.md s2): RES# low >= 10 ms, wait >= 10 ms after.
    static constexpr std::uint32_t kResetLowMs = 10;
    static constexpr std::uint32_t kResetHighMs = 10;
    /// SPI bounce buffer (internal DMA-capable RAM). Frames are sent in chunks of this size under
    /// one CS frame.
    static constexpr std::size_t kChunkBytes = 1000;

    IdfEpdBus(hal::SleepControl& sleep, hal::Delay& delay) noexcept
        : sleep_(sleep), delay_(delay) {}
    IdfEpdBus(const IdfEpdBus&) = delete;
    IdfEpdBus& operator=(const IdfEpdBus&) = delete;
    ~IdfEpdBus() override;

    /// Releases deep-sleep pad holds (IdfSleep::release_holds), sets RST/DC idle levels, configures
    /// BUSY as a plain input (no pull) and brings up SPI2 + the display device. Call once.
    Status init() noexcept;

    Status hardware_reset() override;
    Status command(std::uint8_t cmd) override;
    Status data(std::span<const std::uint8_t> bytes) override;
    /// Always kUnsupported: the board has no MISO; the 3-wire read turnaround is not implemented
    /// ([TECH-DEBT] in docs/STATUS.md, needed only for bring-up E1).
    Status read(std::span<std::uint8_t> out) override;
    [[nodiscard]] bool supports_read() const override { return false; }
    [[nodiscard]] bool busy() const override;
    Status wait_idle(std::uint32_t timeout_ms) override;

private:
    Status transfer(bool dc_high, std::span<const std::uint8_t> bytes);

    hal::SleepControl& sleep_;
    hal::Delay& delay_;
    spi_device_handle_t dev_ = nullptr;
    bool bus_ready_ = false;
};

/// BMA423 register access over the `i2c_master` driver (400 kHz, external pull-ups).
class IdfI2cDevice final : public hal::I2cDevice {
public:
    /// Longest payload accepted by write_registers (the BMA423 config blob is written in 64-byte
    /// chunks, STATUS.md WP-08 tech debt); the register byte is sent in its own buffer.
    static constexpr std::size_t kMaxWriteBytes = 256;
    /// Per-transaction timeout. A 65-byte write is ~1.6 ms at 400 kHz [TUNE].
    static constexpr int kXferTimeoutMs = 50;

    IdfI2cDevice() noexcept = default;
    IdfI2cDevice(const IdfI2cDevice&) = delete;
    IdfI2cDevice& operator=(const IdfI2cDevice&) = delete;
    ~IdfI2cDevice() override;

    /// Creates the bus (board::kI2cSda/kI2cScl, no internal pull-ups) and adds the device at
    /// board::kBma423Address, board::kI2cHz. Call once.
    Status init() noexcept;

    Status read_registers(std::uint8_t reg, std::span<std::uint8_t> out) override;
    Status write_registers(std::uint8_t reg, std::span<const std::uint8_t> data) override;

private:
    [[nodiscard]] Status finish(esp_err_t err) noexcept;

    i2c_master_bus_handle_t bus_ = nullptr;
    i2c_master_dev_handle_t dev_ = nullptr;
};

} // namespace qz::platform
