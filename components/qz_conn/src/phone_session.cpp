// Phone-sync session timing and the shared device name (ARCHITECTURE.md section 13a).
#include "qz/conn/conn.hpp"
#include "tuning.hpp"

#include <array>

namespace qz::conn {

FixedString<11> device_name(std::uint64_t chip_id) noexcept {
    constexpr std::array<char, 16> kHex{
        '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'A', 'B', 'C', 'D', 'E', 'F'};
    std::array<char, 11> name{'Q', 'u', 'a', 'r', 't', 'z', '-', 0, 0, 0, 0};
    for (std::size_t i = 0; i < 4; ++i) {
        name[7 + i] = kHex[(chip_id >> (12U - (4U * i))) & 0xFU];
    }
    FixedString<11> out;
    (void)out.assign(std::string_view{name.data(), name.size()}); // exactly 11 chars: fits
    return out;
}

void PhoneSession::begin(std::int64_t now_rtc_us) noexcept {
    begin_rtc_us_ = now_rtc_us;
    last_activity_us_ = now_rtc_us;
    active_ = true;
    was_secure_ = false;
}

void PhoneSession::end() noexcept {
    active_ = false;
}

PhoneEnd PhoneSession::tick(hal::PhoneLinkState link, std::int64_t now_rtc_us) noexcept {
    if (!active_) {
        return PhoneEnd::kNone;
    }
    if (now_rtc_us - begin_rtc_us_ >= tuning::kPhoneMaxUs) {
        return PhoneEnd::kMaxDuration;
    }
    if (link == hal::PhoneLinkState::kSecure) {
        if (!was_secure_) {
            was_secure_ = true;
            last_activity_us_ = now_rtc_us; // the idle window starts at connection
        }
        return now_rtc_us - last_activity_us_ >= tuning::kPhoneIdleUs ? PhoneEnd::kIdle
                                                                      : PhoneEnd::kNone;
    }
    if (was_secure_) {
        return PhoneEnd::kPhoneLeft; // any non-secure state after a secure one: the phone is gone
    }
    // Waiting for a phone (advertising, or connected but still pairing).
    return now_rtc_us - begin_rtc_us_ >= tuning::kPhoneConnectWindowUs ? PhoneEnd::kNoPhone
                                                                       : PhoneEnd::kNone;
}

void PhoneSession::note_command(std::int64_t now_rtc_us) noexcept {
    last_activity_us_ = now_rtc_us;
}

} // namespace qz::conn
