// qz_bma423 tunables and register addresses (ARCHITECTURE.md section 2: one constexpr table per
// component). Facts: [R1] = docs/research/bma423.md, [ASSUMED] unconfirmed, [TUNE] calibrate on
// hardware (docs/HARDWARE_BRINGUP.md B7).
#pragma once

#include <cstdint>

namespace qz::bma423::tuning {

// --- Registers used directly (everything else goes through the Bosch API) [R1 s3, s4, s6]
inline constexpr std::uint8_t kRegChipId = 0x00;
inline constexpr std::uint8_t kRegEvent = 0x1B;      ///< bit0 por_detected, clear-on-read
inline constexpr std::uint8_t kRegIntStatus0 = 0x1C; ///< feature interrupts, read clears
inline constexpr std::uint8_t kRegInternalStatus = 0x2A;
inline constexpr std::uint8_t kRegPwrConf = 0x7C;             ///< bit0 adv_power_save
inline constexpr std::uint8_t kInternalStatusMask = 0x1F;     ///< bits[4:0] = engine state
inline constexpr std::uint8_t kInternalStatusInitOk = 0x01;   ///< "init_ok"
inline constexpr std::uint8_t kInternalStatusFlagMask = 0xE0; ///< remap error / ODR requirements
inline constexpr std::uint8_t kDoubleTapIntMask = 0x10; ///< INT_STATUS_0 double_tap_out [R1 s10]

// --- Bosch API driving
/// Bytes per config-blob burst. bma423_write_config_file() accepts 2..70 (even) and always
/// transfers whole chunks, so it must divide the 6144-byte blob (else it reads past the array);
/// 64 gives 96 bursts, about 155 ms at 400 kHz [R1 s3 estimate]. The HAL must accept 64-byte
/// writes.
inline constexpr std::uint16_t kReadWriteLenBytes = 64;
inline constexpr std::uint32_t kBoschLowPowerIdleUs = 450; ///< idle the API inserts after a write
/// Idle after a write / before the next access while adv_power_save = 1. The API uses 450 us
/// (datasheet rev 2.0); rev 1.1 and the rev 2.0 power-mode text say 1000 us, and the research
/// decision is to use 1 ms [R1 s1, s11]. Applied to every 450 us the API requests.
inline constexpr std::uint32_t kLowPowerIdleUs = 1000;

// --- Bring-up sequencing
inline constexpr std::uint8_t kSoftResetCommand = 0xB6; ///< CMD (0x7E) [R1 s3]
/// Delay after soft reset before the interface works: UNSPECIFIED, >= 10 ms [ASSUMED] [R1 s3].
inline constexpr std::uint32_t kSoftResetWaitMs = 10;
inline constexpr std::uint8_t kChipIdAttempts = 3; ///< NACK while booting: retry [R1 s4]
inline constexpr std::uint32_t kChipIdRetryMs = 5; ///< [R1 s4]

// --- Sensor configuration [R1 s8, s10]
/// Step-counter-only low-power mode: 50 Hz, averaging 2, low power: ACC_CONF = 0x17, 18 uA.
/// [TUNE] try 0 (no averaging, 13 uA) if a counted walk shows it is accurate enough.
inline constexpr std::uint8_t kStepOdr = 0x07; ///< ODR code 50 Hz
inline constexpr std::uint8_t kStepAveraging = 1;
/// Double-tap needs ODR >= 200 Hz in low-power mode (+29 uA) [R1 s10].
inline constexpr std::uint8_t kTapOdr = 0x09; ///< ODR code 200 Hz
inline constexpr std::uint8_t kTapAveraging = 0;
inline constexpr std::uint8_t kAccelRange4g = 1; ///< ACC_RANGE 0x41: +-4 g, 512 LSB/g [R1 s7]

} // namespace qz::bma423::tuning
