// Private declarations of the ESP-IDF backed HAL classes implemented by WP-24 (qz_platform).
// IdfPlatform (WP-25/28) owns one instance of each in static storage. This header includes IDF
// headers on purpose and must never be exposed through the component's public include dir.
#pragma once

#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_err.h"
#include "nvs.h"
#include "qz/board/watchy_v3.hpp"
#include "qz/core/result.hpp"
#include "qz/hal/board_io.hpp"
#include "qz/hal/delay.hpp"
#include "qz/hal/kv_store.hpp"
#include "qz/hal/system.hpp"

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

/// Sizes of the RTC_NOINIT regions. The platform cannot include qz_app (layering), so they are
/// fixed here with headroom; main/ static_asserts sizeof(app::RtcState) / sizeof(app::FrameShadow)
/// against them. Budget (ARCHITECTURE.md section 6): total RTC use <= 7.5 KiB of 8 KiB.
inline constexpr std::size_t kRtcStateRegionBytes = 2048; ///< sizeof(RtcState) is 1024 today
inline constexpr std::size_t kRtcFrameRegionBytes = 5024; ///< FrameShadow = 8 + 5000 = 5008
static_assert(kRtcStateRegionBytes + kRtcFrameRegionBytes <= 7680, "RTC slow memory budget");

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

} // namespace qz::platform
