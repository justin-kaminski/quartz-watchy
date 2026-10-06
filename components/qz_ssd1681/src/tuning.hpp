// SSD1681 command bytes, parameter values and tunables, in one place. Every line cites
// docs/research/ssd1681.md (R1) section numbers. Tags: [R1 sN] documented, [ASSUMED], [TUNE].
#pragma once

#include <cstddef>
#include <cstdint>

namespace qz::ssd1681::tuning {

// --- command bytes [R1 s4] --------------------------------------------------------------------
inline constexpr std::uint8_t kCmdDriverOutput = 0x01;    ///< 3 params
inline constexpr std::uint8_t kCmdGateVoltage = 0x03;     ///< 1 param (custom waveform only)
inline constexpr std::uint8_t kCmdSourceVoltage = 0x04;   ///< 3 params (custom waveform only)
inline constexpr std::uint8_t kCmdSoftStart = 0x0C;       ///< 4 params
inline constexpr std::uint8_t kCmdDeepSleep = 0x10;       ///< 1 param
inline constexpr std::uint8_t kCmdDataEntry = 0x11;       ///< 1 param
inline constexpr std::uint8_t kCmdSoftReset = 0x12;       ///< no params, BUSY
inline constexpr std::uint8_t kCmdTempSensor = 0x18;      ///< 1 param
inline constexpr std::uint8_t kCmdReadTemperature = 0x1B; ///< read 2 bytes
inline constexpr std::uint8_t kCmdActivate = 0x20;        ///< no params, BUSY
inline constexpr std::uint8_t kCmdUpdateControl2 = 0x22;  ///< 1 param
inline constexpr std::uint8_t kCmdWriteRamBw = 0x24;      ///< data stream, 1 = white
inline constexpr std::uint8_t kCmdWriteRamRed = 0x26;     ///< data stream, previous image (partial)
inline constexpr std::uint8_t kCmdWriteVcom = 0x2C;       ///< 1 param (custom waveform only)
inline constexpr std::uint8_t kCmdReadOtpOption = 0x2D;   ///< read 11 bytes
inline constexpr std::uint8_t kCmdWriteLut = 0x32;        ///< 153 params (custom waveform only)
inline constexpr std::uint8_t kCmdBorder = 0x3C;          ///< 1 param
inline constexpr std::uint8_t kCmdEndOption = 0x3F;       ///< 1 param (custom waveform only)
inline constexpr std::uint8_t kCmdRamXWindow = 0x44;      ///< 2 params
inline constexpr std::uint8_t kCmdRamYWindow = 0x45;      ///< 4 params
inline constexpr std::uint8_t kCmdRamXCounter = 0x4E;     ///< 1 param
inline constexpr std::uint8_t kCmdRamYCounter = 0x4F;     ///< 2 params

// --- parameter values --------------------------------------------------------------------------
inline constexpr std::uint8_t kGateCountMinus1 = 0xC7;    ///< 200 gates [R1 s4 "send C7 00 00"]
inline constexpr std::uint8_t kDataEntryXincYinc = 0x03;  ///< POR; orientation pending E3 [R1 s3]
inline constexpr std::uint8_t kRamXLast = 0x18;           ///< 25 bytes per row [R1 s3]
inline constexpr std::uint8_t kRamYLastLo = 0xC7;         ///< 200 rows [R1 s3]
inline constexpr std::uint8_t kTempSensorInternal = 0x80; ///< [R1 s6: TSCL/TSDA unconnected]
inline constexpr std::uint8_t kSoftStart0 = 0x8B; ///< POR written explicitly [R1 s9.1 step 5]
inline constexpr std::uint8_t kSoftStart1 = 0x9C;
inline constexpr std::uint8_t kSoftStart2 = 0x96;
inline constexpr std::uint8_t kSoftStart3 = 0x0F;
inline constexpr std::uint8_t kBorderFull = 0x05;       ///< follow LUT1 (white) [R1 s6; verify E4]
inline constexpr std::uint8_t kBorderPartial = 0x80;    ///< held at VCOM [R1 s6; verify E4]
inline constexpr std::uint8_t kUpdateFull = 0xF7;       ///< display mode 1 [R1 s5]
inline constexpr std::uint8_t kUpdatePartialOtp = 0xFF; ///< display mode 2 + temp + LUT [R1 s5]
inline constexpr std::uint8_t kUpdatePartialCustom = 0xCF; ///< mode 2, LUT/temp NOT loaded [R1 s5]
inline constexpr std::uint8_t kUpdateLoadTemperature = 0xB1; ///< temp + LUT load only [R1 s5]
inline constexpr std::uint8_t kDeepSleepMode1 = 0x01;        ///< RAM retained, 1 uA [R1 s4, s7]

// --- structure ---------------------------------------------------------------------------------
inline constexpr std::size_t kTemperatureBytes = 2;   ///< [R1 s4 0x1B]
inline constexpr std::size_t kOtpOptionBytes = 11;    ///< [R1 s4 0x2D]
inline constexpr std::size_t kOtpModeFirst = 2;       ///< bytes C..G hold WS0..35 mode bits
inline constexpr std::size_t kOtpModeLast = 6;        ///< inclusive [R1 s4 0x2D, s11 E1]
inline constexpr std::int32_t kDeciPerSixteenth = 10; ///< 0.0625 C/LSB = 10/16 dC
inline constexpr std::int32_t kSixteenths = 16;
inline constexpr std::uint16_t kTemp12Mask = 0x0FFF;
inline constexpr std::uint16_t kTemp12Sign = 0x0800;
inline constexpr std::uint16_t kTemp12Range = 0x1000;

/// Bytes per bus->data() call when streaming RAM: 10 rows; keeps the stack buffer small
/// (no heap on the wake path) while limiting per-transaction overhead.
inline constexpr std::size_t kRamChunkBytes = 250;

} // namespace qz::ssd1681::tuning
