// RTC time base and libc clock push (ARCHITECTURE.md section 8.1).
#include "esp_rtc_time.h"
#include "platform_impl.hpp"

#include <sys/time.h>

namespace qz::platform {
namespace {
constexpr std::int64_t kUsPerSecond = 1'000'000;
} // namespace

std::int64_t IdfClock::rtc_us() const {
    // Raw RTC counter scaled by the boot-time slow-clock calibration; survives deep sleep and
    // software resets [IDF:components/esp_hw_support/esp_clk.c esp_rtc_get_time_us].
    // [ASSUMED] esp_rtc_get_time_us stays continuous across esp_restart: it keeps a 24-byte
    // retain struct in RTC memory (esp_clk.c retain_mem_t) and re-bases on invalid checksum
    // (hardware bring-up B8 measures this).
    return static_cast<std::int64_t>(esp_rtc_get_time_us());
}

void IdfClock::set_system_utc_us(std::int64_t utc_us) {
    // Floor division so negative (pre-1970) values keep tv_usec in [0, 1e6).
    std::int64_t sec = utc_us / kUsPerSecond;
    std::int64_t rem = utc_us % kUsPerSecond;
    if (rem < 0) {
        rem += kUsPerSecond;
        --sec;
    }
    timeval tv{};
    tv.tv_sec = static_cast<time_t>(sec);
    tv.tv_usec = static_cast<suseconds_t>(rem);
    // settimeofday only fails for an invalid timeval, which the normalization above prevents.
    // It moves the libc clock only; esp_rtc_get_time_us (the raw time base) is unaffected
    // [IDF:components/esp_hw_support/esp_clk.c; esp_libc LIBC_TIME_SYSCALL_USE_RTC_HRT].
    (void)settimeofday(&tv, nullptr);
}

} // namespace qz::platform
