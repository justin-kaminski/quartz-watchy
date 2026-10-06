// CredentialStore: persistence, validation, and the "password only via reveal()" guarantee.
#include "qz/settings/settings.hpp"
#include "qz/testkit/fakes.hpp"
#include "test_support.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <ostream>
#include <string>
#include <string_view>

namespace qz::settings {
namespace {

using test::code_of;
using testkit::FakeKvStore;

constexpr std::string_view kCred = "qz_cred";
constexpr std::string_view kPassword = "correct-horse-battery-staple";

// The secret type has no stream/format operator, so an accidental `log << creds.password` or
// console print cannot compile; the only read path is reveal().
template<class T>
concept Streamable = requires(std::ostream& os, const T& value) { os << value; };
static_assert(!Streamable<Secret<64>>);
static_assert(Streamable<int>); // the concept itself works

hal::WifiCredentials make_creds(std::string_view ssid, std::string_view password) {
    hal::WifiCredentials creds;
    EXPECT_TRUE(creds.ssid.assign(ssid));
    EXPECT_TRUE(creds.password.assign(password));
    return creds;
}

TEST(CredentialStore, AbsentCredentialsReportNoCreds) {
    FakeKvStore kv;
    CredentialStore store(kv);
    EXPECT_EQ(code_of(store.load()), Errc::kNoCredentials);
    EXPECT_EQ(kv.write_count(), 0U);
}

TEST(CredentialStore, SaveLoadRoundTrip) {
    FakeKvStore kv;
    CredentialStore store(kv);
    ASSERT_TRUE(store.save(make_creds("Home WiFi", kPassword)));
    EXPECT_EQ(kv.write_count(), 3U); // ver, ssid, pass
    EXPECT_EQ(kv.commit_count(), 1U);

    const auto loaded = CredentialStore(kv).load();
    ASSERT_TRUE(loaded);
    EXPECT_EQ(loaded->ssid.view(), "Home WiFi");
    EXPECT_EQ(loaded->password.reveal(), kPassword);
    EXPECT_EQ(*kv.get_u32(kCred, "ver"), kSchemaVersion);
}

TEST(CredentialStore, MaximumLengthValuesRoundTrip) {
    FakeKvStore kv;
    CredentialStore store(kv);
    const std::string ssid(32, 's');
    const std::string psk(64, 'a'); // raw hex PSK
    ASSERT_TRUE(store.save(make_creds(ssid, psk)));
    const auto loaded = store.load();
    ASSERT_TRUE(loaded);
    EXPECT_EQ(loaded->ssid.view(), ssid);
    EXPECT_EQ(loaded->password.reveal(), psk);
}

TEST(CredentialStore, OpenNetworkStoresNoPasswordKey) {
    FakeKvStore kv;
    CredentialStore store(kv);
    ASSERT_TRUE(store.save(make_creds("Cafe", kPassword)));
    ASSERT_TRUE(store.save(make_creds("Cafe", ""))); // switch to open: old password must go
    EXPECT_EQ(kv.entry_count(kCred), 2U);            // ver + ssid
    EXPECT_FALSE(kv.contains_text(kPassword));
    const auto loaded = store.load();
    ASSERT_TRUE(loaded);
    EXPECT_TRUE(loaded->password.empty());
    EXPECT_EQ(loaded->ssid.view(), "Cafe");
    ASSERT_TRUE(store.save(make_creds("Cafe", ""))); // erasing an absent key is fine
}

TEST(CredentialStore, ClearRemovesEverything) {
    FakeKvStore kv;
    CredentialStore store(kv);
    ASSERT_TRUE(store.clear()); // nothing stored: still ok
    ASSERT_TRUE(store.save(make_creds("Home", kPassword)));
    ASSERT_TRUE(store.clear());
    EXPECT_EQ(kv.entry_count(kCred), 0U);
    EXPECT_EQ(code_of(store.load()), Errc::kNoCredentials);
    EXPECT_FALSE(kv.contains_text(kPassword));
}

TEST(CredentialStore, ValidationRejectsBadCredentialsWithoutWriting) {
    const std::string long_ssid(33, 'x');
    const std::string long_pass(65, 'x');
    const std::string bad_hex(64, 'g');
    const std::string ok_hex(64, 'F');
    EXPECT_TRUE(validate_credentials(make_creds("a", "12345678")));
    EXPECT_TRUE(validate_credentials(make_creds("a", std::string(63, 'p'))));
    EXPECT_TRUE(validate_credentials(make_creds("a", ok_hex)));
    EXPECT_TRUE(validate_credentials(make_creds("a", "")));                   // open network
    EXPECT_TRUE(validate_credentials(make_creds("caf\xC3\xA9", "12345678"))); // UTF-8 SSID bytes

    FakeKvStore kv;
    CredentialStore store(kv);
    const std::array<hal::WifiCredentials, 7> bad{{
        make_creds("", "12345678"),
        make_creds("tab\there", "12345678"),
        make_creds("nul\x7f", "12345678"),
        make_creds("a", "1234567"), // too short for WPA2
        make_creds("a", "pass\nword!"),
        make_creds("a", bad_hex),
        make_creds("a", long_pass.substr(0, 63) + "\x01"),
    }};
    for (const auto& creds : bad) {
        EXPECT_EQ(code_of(validate_credentials(creds)), Errc::kBadArgs);
        EXPECT_EQ(code_of(store.save(creds)), Errc::kBadArgs);
    }
    // Beyond the field capacity the Secret/FixedString refuse the value up front.
    hal::WifiCredentials too_long;
    EXPECT_FALSE(too_long.ssid.assign(long_ssid));
    EXPECT_EQ(code_of(validate_credentials(too_long)), Errc::kBadArgs); // empty after refusal
    EXPECT_FALSE(too_long.password.assign(long_pass));
    EXPECT_EQ(kv.write_count(), 0U);
}

TEST(CredentialStore, CorruptStoredValuesAreReportedAsCorrupt) {
    {
        FakeKvStore kv;
        ASSERT_TRUE(kv.set_str(kCred, "ssid", std::string(40, 'x'))); // longer than any SSID
        EXPECT_EQ(code_of(CredentialStore(kv).load()), Errc::kCorrupt);
    }
    {
        FakeKvStore kv;
        ASSERT_TRUE(kv.set_u32(kCred, "ssid", 7)); // wrong type
        EXPECT_EQ(code_of(CredentialStore(kv).load()), Errc::kCorrupt);
    }
    {
        FakeKvStore kv;
        ASSERT_TRUE(kv.set_str(kCred, "ssid", "ok"));
        ASSERT_TRUE(kv.set_str(kCred, "pass", std::string(70, 'p')));
        EXPECT_EQ(code_of(CredentialStore(kv).load()), Errc::kCorrupt);
    }
    {
        FakeKvStore kv;
        ASSERT_TRUE(kv.set_str(kCred, "ssid", "ok"));
        ASSERT_TRUE(kv.set_u32(kCred, "pass", 1)); // wrong type
        EXPECT_EQ(code_of(CredentialStore(kv).load()), Errc::kCorrupt);
    }
    {
        FakeKvStore kv;
        ASSERT_TRUE(kv.set_str(kCred, "ssid", "")); // empty SSID = nothing usable
        EXPECT_EQ(code_of(CredentialStore(kv).load()), Errc::kNoCredentials);
    }
}

TEST(CredentialStore, WriteFailuresPropagateWithoutCommit) {
    FakeKvStore kv;
    kv.fail_writes_after(1);
    EXPECT_EQ(code_of(CredentialStore(kv).save(make_creds("Home", kPassword))), Errc::kIo);
    EXPECT_EQ(kv.commit_count(), 0U);
}

TEST(CredentialStore, PasswordNeverLandsOutsideCredNamespaceOrInFormattedOutput) {
    FakeKvStore kv;
    CredentialStore creds(kv);
    SettingsStore settings(kv);

    // Everything the firmware persists, with credentials present.
    Settings all = defaults();
    ASSERT_TRUE(set_from_string(all, Key::kTimeZone, "Europe/Berlin"));
    ASSERT_TRUE(set_from_string(all, Key::kLatitude, "52.52"));
    ASSERT_TRUE(set_from_string(all, Key::kLongitude, "13.405"));
    ASSERT_TRUE(set_from_string(all, Key::kConnectivity, "time+weather"));
    ASSERT_TRUE(creds.save(make_creds("Home", kPassword)));
    ASSERT_TRUE(settings.save(all, defaults()));
    ASSERT_TRUE(kv.set_blob("qz_steps", "hist", std::array<std::uint8_t, 4>{1, 2, 3, 4}));
    ASSERT_TRUE(kv.set_u32("qz_diag", "crashes", 1));

    // Sanity: the checker does see the password where it legitimately lives.
    EXPECT_TRUE(kv.contains_text(kPassword));
    ASSERT_TRUE(kv.erase_namespace(kCred));
    EXPECT_FALSE(kv.contains_text(kPassword)) << "password leaked into another namespace";
    EXPECT_FALSE(kv.contains_text("pass"));

    // Settings round trip and every formatted value stay free of the password.
    ASSERT_TRUE(creds.save(make_creds("Home", kPassword)));
    const auto loaded = settings.load();
    ASSERT_TRUE(loaded);
    for (const KeyInfo& ki : schema()) {
        EXPECT_EQ(test::fmt(*loaded, ki.key).find(kPassword), std::string::npos) << ki.name;
        EXPECT_EQ(ki.help.find(kPassword), std::string_view::npos);
    }
    // Factory reset wipes the password too.
    ASSERT_TRUE(settings.erase_all());
    EXPECT_FALSE(kv.contains_text(kPassword));
    EXPECT_EQ(code_of(creds.load()), Errc::kNoCredentials);
}

TEST(CredentialStore, ErrorResultsCarryNoCredentialText) {
    FakeKvStore kv;
    CredentialStore store(kv);
    const auto bad = make_creds("Home", "short");
    const Status st = store.save(bad);
    ASSERT_FALSE(st);
    EXPECT_EQ(st.error().detail, 0); // detail never encodes input bytes
    EXPECT_FALSE(kv.contains_text("short"));
    EXPECT_FALSE(kv.contains_text("Home"));
}

} // namespace
} // namespace qz::settings
