// VirtualClock: the virtual RTC + libc clock behind every time-dependent fake.
//
// All arithmetic is integer and exact. The RTC counts T * (1e9 + ppb) / 1e9 microseconds for T
// microseconds of true time; the sub-microsecond remainder is carried in rtc_remainder_, so the RTC
// after any sequence of advance_us() calls equals the single-step result for the same total.
#include "qz/core/assert.hpp"
#include "qz/testkit/fakes.hpp"

#include <cstdint>
#include <limits>

namespace qz::testkit {
namespace {

constexpr std::int64_t kPpbScale = 1'000'000'000; ///< ppb are parts per kPpbScale
/// +-50 %: far beyond any crystal, and it keeps (1e9 + ppb) >= 5e8 so the conversions below are
/// bounded by a factor of two.
constexpr std::int32_t kMaxCrystalErrorPpb = 500'000'000;
constexpr std::int64_t kInt64Max = std::numeric_limits<std::int64_t>::max();
/// Largest single step, in either time base (about 73,000 years): keeps every product in range.
constexpr std::int64_t kMaxStepUs = kInt64Max / 4;
constexpr std::int64_t kUsPerMs = 1000;

/// n / d rounded toward negative infinity (d > 0).
constexpr std::int64_t floor_div(std::int64_t n, std::int64_t d) noexcept {
    std::int64_t q = n / d;
    if (n % d < 0) {
        --q;
    }
    return q;
}

/// n mod d in [0, d) (d > 0).
constexpr std::int64_t floor_mod(std::int64_t n, std::int64_t d) noexcept {
    const std::int64_t r = n % d;
    return r < 0 ? r + d : r;
}

/// n / d rounded toward positive infinity (d > 0).
constexpr std::int64_t ceil_div(std::int64_t n, std::int64_t d) noexcept {
    std::int64_t q = n / d;
    if (n % d > 0) {
        ++q;
    }
    return q;
}

} // namespace

std::int64_t VirtualClock::rtc_us() const {
    return rtc_us_;
}

void VirtualClock::set_system_utc_us(std::int64_t utc_us) {
    system_utc_us_ = utc_us;
}

void VirtualClock::delay_us(std::uint32_t us) {
    advance_us(static_cast<std::int64_t>(us));
}

void VirtualClock::delay_ms(std::uint32_t ms) {
    advance_us(static_cast<std::int64_t>(ms) * kUsPerMs);
}

void VirtualClock::advance_us(std::int64_t us) {
    QZ_ASSERT(us >= 0);
    QZ_ASSERT(us <= kMaxStepUs);
    // RTC advance = us + floor((us * ppb + remainder) / 1e9). us * ppb can exceed int64 for long
    // steps, so split us = quotient * 1e9 + rest: quotient * ppb is exact and rest * ppb < 5e17.
    const std::int64_t quotient = us / kPpbScale;
    const std::int64_t rest = us % kPpbScale;
    const std::int64_t scaled = (rest * error_ppb_) + rtc_remainder_;
    const std::int64_t rtc_delta = us + (quotient * error_ppb_) + floor_div(scaled, kPpbScale);
    QZ_ASSERT(rtc_delta >= 0); // (1e9 + ppb) >= 5e8 makes the RTC monotonic
    QZ_ASSERT(elapsed_us_ <= kInt64Max - us);
    QZ_ASSERT(true_utc_us_ <= kInt64Max - us);
    QZ_ASSERT(rtc_us_ <= kInt64Max - rtc_delta);
    QZ_ASSERT(system_utc_us_ <= kInt64Max - rtc_delta);

    elapsed_us_ += us;
    true_utc_us_ += us;
    rtc_us_ += rtc_delta;
    system_utc_us_ += rtc_delta;
    rtc_remainder_ = floor_mod(scaled, kPpbScale);
}

void VirtualClock::advance_rtc_us(std::int64_t rtc_delta_us) {
    QZ_ASSERT(rtc_delta_us >= 0);
    QZ_ASSERT(rtc_delta_us <= kMaxStepUs);
    if (rtc_delta_us == 0) {
        return;
    }
    // The RTC reaches +R after T true microseconds iff T * rate + remainder >= R * 1e9, where
    // rate = 1e9 + ppb. The least such T is ceil((R * 1e9 - remainder) / rate); split R = a * rate
    // + b so that no product leaves int64.
    const std::int64_t rate = kPpbScale + error_ppb_;
    const std::int64_t whole = rtc_delta_us / rate;
    const std::int64_t rest = rtc_delta_us % rate;
    const std::int64_t true_us =
        (whole * kPpbScale) + ceil_div((rest * kPpbScale) - rtc_remainder_, rate);
    advance_us(true_us);
}

void VirtualClock::set_true_utc_us(std::int64_t utc_us) {
    true_utc_us_ = utc_us;
}

std::int64_t VirtualClock::true_utc_us() const {
    return true_utc_us_;
}

void VirtualClock::set_crystal_error_ppb(std::int32_t ppb) {
    QZ_ASSERT(ppb >= -kMaxCrystalErrorPpb && ppb <= kMaxCrystalErrorPpb);
    error_ppb_ = ppb;
}

void VirtualClock::power_loss() {
    // The RTC and the libc clock lose their state; the world (true time) and the crystal do not.
    rtc_us_ = 0;
    rtc_remainder_ = 0;
    system_utc_us_ = 0;
}

std::int64_t VirtualClock::elapsed_us() const {
    return elapsed_us_;
}

std::int64_t VirtualClock::system_utc_us() const {
    return system_utc_us_;
}

} // namespace qz::testkit
