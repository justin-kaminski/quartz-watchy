// RtcMemory persistence across esp_restart (owner-run; WP-24 acceptance).
#include "esp_rom_sys.h"
#include "platform_impl.hpp"
#include "unity.h"
#include "unity_test_runner.h"

#include <cstdint>

using qz::platform::IdfRtcMemory;
using qz::platform::IdfSystem;

namespace {

constexpr std::uint8_t pattern(std::size_t index, std::uint8_t salt) noexcept {
    return static_cast<std::uint8_t>((index * 131U + salt) & 0xFFU);
}

} // namespace

TEST_CASE("RtcMemory: regions are sized, disjoint, aligned", "[qz_platform][rtc]") {
    IdfRtcMemory mem;
    const auto state = mem.state_region();
    const auto frame = mem.frame_region();
    TEST_ASSERT_EQUAL_UINT32(qz::platform::kRtcStateRegionBytes, state.size());
    TEST_ASSERT_EQUAL_UINT32(qz::platform::kRtcFrameRegionBytes, frame.size());
    TEST_ASSERT_TRUE(state.size() >= 1024); // sizeof(app::RtcState) today
    TEST_ASSERT_TRUE(frame.size() >= 5008); // sizeof(app::FrameShadow)
    TEST_ASSERT_EQUAL_UINT32(0, reinterpret_cast<std::uintptr_t>(state.data()) % 8U);
    TEST_ASSERT_EQUAL_UINT32(0, reinterpret_cast<std::uintptr_t>(frame.data()) % 8U);
    const auto state_end = reinterpret_cast<std::uintptr_t>(state.data()) + state.size();
    const auto frame_end = reinterpret_cast<std::uintptr_t>(frame.data()) + frame.size();
    TEST_ASSERT_TRUE(state_end <= reinterpret_cast<std::uintptr_t>(frame.data()) ||
                     frame_end <= reinterpret_cast<std::uintptr_t>(state.data()));
}

static void rtc_write_then_restart() {
    IdfRtcMemory mem;
    auto state = mem.state_region();
    auto frame = mem.frame_region();
    for (std::size_t i = 0; i < state.size(); ++i) {
        state[i] = pattern(i, 0x11);
    }
    for (std::size_t i = 0; i < frame.size(); ++i) {
        frame[i] = pattern(i, 0x77);
    }
    IdfSystem().restart();
}

static void rtc_verify_after_restart() {
    IdfSystem sys;
    TEST_ASSERT_TRUE(sys.reset_reason() == qz::hal::ResetReason::kSoftware);
    IdfRtcMemory mem;
    const auto state = mem.state_region();
    const auto frame = mem.frame_region();
    for (std::size_t i = 0; i < state.size(); ++i) {
        TEST_ASSERT_EQUAL_UINT8(pattern(i, 0x11), state[i]);
    }
    for (std::size_t i = 0; i < frame.size(); ++i) {
        TEST_ASSERT_EQUAL_UINT8(pattern(i, 0x77), frame[i]);
    }
}

TEST_CASE_MULTIPLE_STAGES("RtcMemory: contents persist across esp_restart",
                          "[qz_platform][rtc]",
                          rtc_write_then_restart,
                          rtc_verify_after_restart);
