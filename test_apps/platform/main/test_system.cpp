// Smoke tests of the remaining platform pieces that can run unattended.
#include "platform_impl.hpp"
#include "unity.h"
#include "unity_test_runner.h"

#include <sys/time.h>

using qz::platform::IdfClock;
using qz::platform::IdfDelay;
using qz::platform::IdfSystem;

TEST_CASE("Clock: rtc_us is monotonic and settimeofday is applied", "[qz_platform][clock]") {
    IdfClock clock;
    const std::int64_t a = clock.rtc_us();
    IdfDelay().delay_ms(5);
    const std::int64_t b = clock.rtc_us();
    TEST_ASSERT_TRUE(b > a);
    TEST_ASSERT_TRUE(b - a >= 4000); // >= ~5 ms, minus slow-clock granularity

    clock.set_system_utc_us(1'700'000'000'123'456LL);
    timeval tv{};
    TEST_ASSERT_EQUAL_INT(0, gettimeofday(&tv, nullptr));
    TEST_ASSERT_TRUE(tv.tv_sec >= 1'700'000'000LL && tv.tv_sec <= 1'700'000'002LL);
}

TEST_CASE("Delay: waits at least the requested time", "[qz_platform][delay]") {
    IdfClock clock;
    IdfDelay delay;
    const std::int64_t t0 = clock.rtc_us();
    delay.delay_us(500);
    delay.delay_ms(10);
    delay.delay_us(3500);
    TEST_ASSERT_TRUE(clock.rtc_us() - t0 >= 13000);
}

TEST_CASE("System: identity, heap, clock info", "[qz_platform][system]") {
    IdfSystem sys;
    TEST_ASSERT_TRUE(sys.boot_rtc_us() > 0);
    TEST_ASSERT_TRUE(sys.chip_id() != 0);
    const auto heap = sys.heap();
    TEST_ASSERT_TRUE(heap.free_bytes >= heap.min_free_bytes);
    TEST_ASSERT_TRUE(heap.free_bytes > 0);
    const auto fw = sys.firmware();
    TEST_ASSERT_TRUE(!fw.idf_version.empty());
    const auto clk = sys.slow_clock();
    // With the 32.768 kHz crystal fitted the calibrated frequency is within 5000 ppm.
    TEST_ASSERT_TRUE(clk.external_crystal);
    TEST_ASSERT_TRUE(clk.measured_hz > 32600 && clk.measured_hz < 32900);
}
