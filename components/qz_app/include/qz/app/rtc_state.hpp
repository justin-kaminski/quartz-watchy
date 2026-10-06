// RTC-memory state (ARCHITECTURE.md section 6). Layout pinned by a host test; bump
// kRtcStateVersion on ANY change to these structs or the structs they embed.
#pragma once

#include "qz/conn/conn.hpp"
#include "qz/core/containers.hpp"
#include "qz/core/result.hpp"
#include "qz/gfx/framebuffer.hpp"
#include "qz/model/types.hpp"
#include "qz/power/power.hpp"
#include "qz/settings/settings.hpp"
#include "qz/steps/step_tracker.hpp"
#include "qz/time/timekeeper.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <type_traits>

namespace qz::app {

inline constexpr std::uint32_t kRtcStateMagic = 0x53525A51;    ///< "QZRS" little-endian
inline constexpr std::uint32_t kFrameShadowMagic = 0x46525A51; ///< "QZRF"
inline constexpr std::uint16_t kRtcStateVersion = 1;
inline constexpr std::uint16_t kWakeLogCapacity = 32;

struct RtcHeader {
    std::uint32_t magic = 0;
    std::uint16_t version = 0;
    std::uint16_t size = 0;  ///< sizeof(RtcState)
    std::uint32_t crc32 = 0; ///< over boot_count and every byte after the header (RtcStore)
    std::uint32_t boot_count = 0;
};

/// `Error::detail` of the kCorrupt returned by RtcStore::load / load_frame (diagnostics only).
enum class RtcCorruptReason : std::uint8_t {
    kRegionTooSmall = 1,
    kMagic = 2,
    kVersion = 3,
    kSize = 4,
    kCrc = 5,
};

struct DisplayState {
    std::uint16_t partials_since_full = 0;
    std::uint8_t frame_valid = 0; ///< FrameShadow matches the panel
    std::uint8_t last_refresh_full = 0;
    std::int16_t panel_temp_dc = 0;
    std::uint16_t reserved = 0;
    time::UnixSeconds last_full_utc = 0;
};

struct WakeTiming {
    std::int64_t scheduled_wake_rtc_us = 0; ///< raw RTC instant of the pending timer wake, 0 = none
    std::int32_t ewma_latency_us = 350'000; ///< wake-ahead lead (ARCHITECTURE.md 8.4) [TUNE]
    std::uint16_t crash_count_window = 0;   ///< panics within the 10-min window
    std::uint8_t safe_mode = 0;
    std::uint8_t reserved = 0;
    /// Start of the 10-min crash window; while safe_mode != 0 it holds the safe-mode entry time
    /// (the 24 h expiry reference, WakePlanner).
    std::int64_t crash_window_start_rtc_us = 0;
};

struct RtcState {
    RtcHeader header;
    time::TimeKeeperState time;
    steps::StepState steps;
    power::PowerState power;
    conn::ConnState conn;
    model::WeatherReport weather;
    DisplayState display;
    WakeTiming wake;
    settings::Settings settings; ///< cache: NVS is not touched on the minute path
    std::uint8_t settings_valid = 0;
    std::uint8_t face_id = 0;
    std::array<std::uint8_t, 2> reserved{};
    RingBuffer<model::WakeRecord, kWakeLogCapacity> wake_log;
};
static_assert(std::is_trivially_copyable_v<RtcState>, "RtcState lives in RTC_NOINIT memory");

struct FrameShadow {
    std::uint32_t magic = 0;
    std::uint32_t crc32 = 0;
    gfx::Framebuffer frame;
};
static_assert(std::is_trivially_copyable_v<FrameShadow>);

/// Loads/validates/commits the RTC regions. Not thread-safe.
class RtcStore {
public:
    RtcStore(std::span<std::uint8_t> state_region, std::span<std::uint8_t> frame_region) noexcept;
    /// Copies the region into `out` if magic/version/size/CRC are valid; else kCorrupt (out
    /// untouched; Error::detail is an RtcCorruptReason). Byte-wise copies: no alignment assumed.
    Status load(RtcState& out) const noexcept;
    /// Writes payload and a header (magic, version, size, CRC recomputed over the stored bytes);
    /// the caller's `state.header` only contributes boot_count. Call right before sleep.
    void commit(const RtcState& state) noexcept;
    Status load_frame(gfx::Framebuffer& out) const noexcept;
    void commit_frame(const gfx::Framebuffer& frame) noexcept;
    void invalidate() noexcept;

private:
    std::span<std::uint8_t> state_;
    std::span<std::uint8_t> frame_;
};

} // namespace qz::app
