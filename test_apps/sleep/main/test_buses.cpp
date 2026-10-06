// EpdBus + I2C smoke tests (owner-run; HARDWARE_BRINGUP.md B3/B5). They talk to the real BMA423 and
// the real SSD1681 but never trigger a display refresh.
#include "platform_impl.hpp"
#include "unity.h"
#include "unity_test_runner.h"

#include <array>
#include <cstdio>
#include <span>

using qz::platform::IdfBoardIo;
using qz::platform::IdfClock;
using qz::platform::IdfDelay;
using qz::platform::IdfEpdBus;
using qz::platform::IdfI2cDevice;
using qz::platform::IdfSleep;

TEST_CASE("I2c: BMA423 chip id, and a 64-byte write is accepted", "[qz_sleep][i2c]") {
    IdfI2cDevice i2c;
    TEST_ASSERT_TRUE(i2c.init());
    std::array<std::uint8_t, 1> id{};
    TEST_ASSERT_TRUE(i2c.read_registers(0x00, id)); // CHIP_ID
    TEST_ASSERT_EQUAL_HEX8(0x13, id[0]);            // [R1] bma423.md / HARDWARE_BRINGUP B3

    // Config-stream port write of 64 zero bytes (WP-08 needs 64-byte writes). Harmless: every boot
    // starts the BMA423 sequence with a soft reset, which is repeated here to leave a clean chip.
    IdfDelay delay;
    const std::array<std::uint8_t, 1> soft_reset{0xB6};
    const std::array<std::uint8_t, 1> init_ctrl_off{0x00};
    const std::array<std::uint8_t, 64> blob{};
    TEST_ASSERT_TRUE(i2c.write_registers(0x7E, soft_reset));
    delay.delay_ms(5);
    TEST_ASSERT_TRUE(i2c.write_registers(0x59, init_ctrl_off));
    TEST_ASSERT_TRUE(i2c.write_registers(0x5E, blob));
    TEST_ASSERT_TRUE(i2c.write_registers(0x7E, soft_reset));
    delay.delay_ms(5);
    TEST_ASSERT_TRUE(i2c.read_registers(0x00, id));
    TEST_ASSERT_EQUAL_HEX8(0x13, id[0]);
}

TEST_CASE("I2c: zero-length transfers and NACK mapping", "[qz_sleep][i2c]") {
    IdfI2cDevice i2c;
    TEST_ASSERT_TRUE(i2c.init());
    std::array<std::uint8_t, 0> none{};
    TEST_ASSERT_TRUE(i2c.read_registers(0x00, none));
    std::array<std::uint8_t, 1> id{};
    TEST_ASSERT_TRUE(i2c.read_registers(0x00, id));
    TEST_ASSERT_TRUE(i2c.write_registers(0x7E, std::span<const std::uint8_t>{})); // reg byte only
}

TEST_CASE("EpdBus: reset, SW reset command and BUSY wait via light sleep", "[qz_sleep][epd]") {
    IdfBoardIo io;
    TEST_ASSERT_TRUE(io.init());
    IdfDelay delay;
    IdfSleep sleep(io);
    IdfEpdBus epd(sleep, delay);
    TEST_ASSERT_TRUE(epd.init());
    TEST_ASSERT_TRUE(epd.hardware_reset());
    TEST_ASSERT_TRUE(epd.wait_idle(1000));
    TEST_ASSERT_TRUE(epd.command(0x12)); // SW reset: BUSY high while it runs
    const std::int64_t t0 = IdfClock().rtc_us();
    TEST_ASSERT_TRUE(epd.wait_idle(1000));
    const std::int64_t elapsed_us = IdfClock().rtc_us() - t0;
    std::printf("SW reset BUSY time: %lld us\n", static_cast<long long>(elapsed_us));
    TEST_ASSERT_FALSE(epd.busy());
    // Reads are not supported on this board.
    std::array<std::uint8_t, 1> out{};
    const qz::Status r = epd.read(out);
    TEST_ASSERT_FALSE(r);
    // A zero-length data() is a no-op, and a 5000-byte RAM write crosses the chunk boundary:
    // select the RAM write command only to the point of data, no refresh is triggered.
    TEST_ASSERT_TRUE(epd.data(std::span<const std::uint8_t>{}));
}

TEST_CASE("EpdBus: SPI chunking handles 5000 bytes without an update", "[qz_sleep][epd]") {
    IdfBoardIo io;
    TEST_ASSERT_TRUE(io.init());
    IdfDelay delay;
    IdfSleep sleep(io);
    IdfEpdBus epd(sleep, delay);
    TEST_ASSERT_TRUE(epd.init());
    TEST_ASSERT_TRUE(epd.hardware_reset());
    TEST_ASSERT_TRUE(epd.command(0x12));
    TEST_ASSERT_TRUE(epd.wait_idle(1000));
    static const std::array<std::uint8_t, 5000> white = [] {
        std::array<std::uint8_t, 5000> a{};
        a.fill(0xFF);
        return a;
    }();
    TEST_ASSERT_TRUE(epd.command(0x24)); // write RAM (BW); RAM only, no 0x20 refresh
    TEST_ASSERT_TRUE(epd.data(white));
    TEST_ASSERT_FALSE(epd.busy());       // RAM writes do not raise BUSY
    TEST_ASSERT_TRUE(epd.command(0x10)); // panel deep sleep
    TEST_ASSERT_TRUE(epd.data(std::array<std::uint8_t, 1>{0x01}));
}
