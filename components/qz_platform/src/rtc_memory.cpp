// RTC_NOINIT storage for RtcState and FrameShadow (ARCHITECTURE.md section 6).
#include "esp_attr.h"
#include "platform_impl.hpp"

namespace qz::platform {
namespace {

// RTC_NOINIT_ATTR places the arrays in .rtc_noinit: kept across deep sleep and software
// resets, NOT zeroed at boot [IDF:components/esp_common/include/esp_attr.h RTC_NOINIT_ATTR].
// After power-on the content is undefined, which is why the owner validates magic/version/CRC.
// Region placement/size budget is checked from the map file by owner-run WP-24 acceptance.
RTC_NOINIT_ATTR alignas(8) std::uint8_t g_state_region[kRtcStateRegionBytes];
RTC_NOINIT_ATTR alignas(8) std::uint8_t g_frame_region[kRtcFrameRegionBytes];

} // namespace

std::span<std::uint8_t> IdfRtcMemory::state_region() {
    return {g_state_region, kRtcStateRegionBytes};
}

std::span<std::uint8_t> IdfRtcMemory::frame_region() {
    return {g_frame_region, kRtcFrameRegionBytes};
}

} // namespace qz::platform
