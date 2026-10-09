// PhoneSession timing table and the shared device name.
#include "qz/conn/conn.hpp"

#include <gtest/gtest.h>

namespace qz::conn {
namespace {

using hal::PhoneLinkState;

constexpr std::int64_t kS = 1'000'000;

TEST(DeviceName, MatchesTheProvisioningScheme) {
    EXPECT_EQ(device_name(0x1234ABCDULL).view(), "Quartz-ABCD");
    EXPECT_EQ(device_name(0).view(), "Quartz-0000");
}

TEST(PhoneSession, InactiveNeverEnds) {
    PhoneSession s;
    EXPECT_EQ(s.tick(PhoneLinkState::kAdvertising, 1000 * kS), PhoneEnd::kNone);
}

TEST(PhoneSession, NoPhoneWithinTheConnectWindowEnds) {
    PhoneSession s;
    s.begin(0);
    EXPECT_EQ(s.tick(PhoneLinkState::kAdvertising, 119 * kS), PhoneEnd::kNone);
    EXPECT_EQ(s.tick(PhoneLinkState::kPairing, 119 * kS), PhoneEnd::kNone);
    EXPECT_EQ(s.tick(PhoneLinkState::kAdvertising, 120 * kS), PhoneEnd::kNoPhone);
    EXPECT_FALSE(s.was_secure());
}

TEST(PhoneSession, SecureLinkIdlesOutWithoutCommands) {
    PhoneSession s;
    s.begin(0);
    EXPECT_EQ(s.tick(PhoneLinkState::kSecure, 100 * kS), PhoneEnd::kNone); // idle window starts
    EXPECT_TRUE(s.was_secure());
    s.note_command(150 * kS);
    EXPECT_EQ(s.tick(PhoneLinkState::kSecure, 269 * kS), PhoneEnd::kNone);
    EXPECT_EQ(s.tick(PhoneLinkState::kSecure, 270 * kS), PhoneEnd::kIdle);
}

TEST(PhoneSession, PhoneLeavingEndsTheSession) {
    PhoneSession s;
    s.begin(0);
    (void)s.tick(PhoneLinkState::kSecure, 5 * kS);
    EXPECT_EQ(s.tick(PhoneLinkState::kAdvertising, 6 * kS), PhoneEnd::kPhoneLeft);
}

TEST(PhoneSession, HardCapWinsOverActivity) {
    PhoneSession s;
    s.begin(0);
    for (std::int64_t t = 0; t < 15 * 60 * kS; t += 60 * kS) {
        s.note_command(t);
        EXPECT_EQ(s.tick(PhoneLinkState::kSecure, t), PhoneEnd::kNone) << t;
    }
    EXPECT_EQ(s.tick(PhoneLinkState::kSecure, 15 * 60 * kS), PhoneEnd::kMaxDuration);
}

TEST(PhoneSession, EndStopsTicking) {
    PhoneSession s;
    s.begin(0);
    s.end();
    EXPECT_FALSE(s.active());
    EXPECT_EQ(s.tick(PhoneLinkState::kAdvertising, 999 * kS), PhoneEnd::kNone);
}

} // namespace
} // namespace qz::conn
