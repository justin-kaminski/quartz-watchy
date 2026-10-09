// Suite "settings": every key validated, round-tripped through strings and through NVS
// (redirected to the "qz_test" namespace so real settings are never touched).
#include "detail.hpp"
#include "qz/faces/registry.hpp"
#include "qz/hal/kv_store.hpp"
#include "qz/settings/settings.hpp"
#include "tests.hpp"
#include "tuning.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <string_view>

namespace qz::selftest::tests {

namespace {

using settings::Key;
using settings::Settings;

/// Per key: two values that must be rejected and one valid non-default sample.
struct KeyCase {
    Key key;
    std::string_view bad1;
    std::string_view bad2;
    std::string_view sample;
};

constexpr std::array<KeyCase, static_cast<std::size_t>(Key::kCount)> kCases{{
    {Key::kHourFormat, "13h", "", "12h"},
    {Key::kTimeZone, "Nowhere/Land", "", "Europe/Berlin"},
    {Key::kTempUnit, "kelvin", "", "f"},
    {Key::kConnectivity, "wifi", "", "time+weather"},
    {Key::kWeatherHighLow, "maybe", "", "off"},
    {Key::kLatitude, "90.00001", "abc", "41.77"},
    {Key::kLongitude, "180.00001", "", "-88.15"},
    {Key::kSyncIntervalH, "7", "0", "12"},
    {Key::kWeatherIntervalMin, "45", "29", "30"},
    {Key::kStepGoal, "750", "50500", "10000"},
    {Key::kVibration, "2", "", "off"},
    {Key::kFace, "256", "-1", "1"},
    {Key::kTapWake, "x", "", "on"},
    {Key::kPhoneSync, "2", "", "off"},
}};

const KeyCase* case_for(Key key) noexcept {
    for (const KeyCase& c : kCases) {
        if (c.key == key) {
            return &c;
        }
    }
    return nullptr;
}

/// Forwards every call to `inner` with the namespace replaced by "qz_test".
class RedirectKv final : public hal::KvStore {
public:
    explicit RedirectKv(hal::KvStore& inner) noexcept : inner_(inner) {}
    Result<std::uint32_t> get_u32(std::string_view /*ns*/, std::string_view key) override {
        return inner_.get_u32(tuning::kTestNamespace, key);
    }
    Status set_u32(std::string_view /*ns*/, std::string_view key, std::uint32_t value) override {
        return inner_.set_u32(tuning::kTestNamespace, key, value);
    }
    Result<std::int32_t> get_i32(std::string_view /*ns*/, std::string_view key) override {
        return inner_.get_i32(tuning::kTestNamespace, key);
    }
    Status set_i32(std::string_view /*ns*/, std::string_view key, std::int32_t value) override {
        return inner_.set_i32(tuning::kTestNamespace, key, value);
    }
    Result<std::int64_t> get_i64(std::string_view /*ns*/, std::string_view key) override {
        return inner_.get_i64(tuning::kTestNamespace, key);
    }
    Status set_i64(std::string_view /*ns*/, std::string_view key, std::int64_t value) override {
        return inner_.set_i64(tuning::kTestNamespace, key, value);
    }
    Result<std::size_t>
    get_str(std::string_view /*ns*/, std::string_view key, std::span<char> out) override {
        return inner_.get_str(tuning::kTestNamespace, key, out);
    }
    Status set_str(std::string_view /*ns*/, std::string_view key, std::string_view value) override {
        return inner_.set_str(tuning::kTestNamespace, key, value);
    }
    Result<std::size_t>
    get_blob(std::string_view /*ns*/, std::string_view key, std::span<std::uint8_t> out) override {
        return inner_.get_blob(tuning::kTestNamespace, key, out);
    }
    Status set_blob(std::string_view /*ns*/,
                    std::string_view key,
                    std::span<const std::uint8_t> value) override {
        return inner_.set_blob(tuning::kTestNamespace, key, value);
    }
    Status erase_key(std::string_view /*ns*/, std::string_view key) override {
        return inner_.erase_key(tuning::kTestNamespace, key);
    }
    Status erase_namespace(std::string_view /*ns*/) override {
        return inner_.erase_namespace(tuning::kTestNamespace);
    }
    Status commit() override { return inner_.commit(); }

private:
    hal::KvStore& inner_;
};

/// Settings with every key set to its sample value; false if a sample is rejected.
bool make_sample_settings(Settings& out, Detail& d) noexcept {
    out = settings::defaults();
    for (const settings::KeyInfo& info : settings::schema()) {
        const KeyCase* c = case_for(info.key);
        if (c == nullptr) {
            (void)fail(d, Text().put("no case for ").put(info.name).view());
            return false;
        }
        if (const Status s = settings::set_from_string(out, info.key, c->sample); !s) {
            (void)fail_error(d, info.name, s.error());
            return false;
        }
    }
    return true;
}

} // namespace

Outcome settings_defaults(Context& /*ctx*/, Detail& d) noexcept {
    const Settings s = settings::defaults();
    if (const Status v = settings::validate(s, &faces::is_registered); !v) {
        return fail_error(d, "defaults invalid", v.error());
    }
    for (const settings::KeyInfo& info : settings::schema()) {
        std::array<char, 80> buf{};
        if (settings::format_value(s, info.key, buf) == 0) {
            return fail(d, Text().put("no default text: ").put(info.name).view());
        }
        if (settings::find_key(info.name) == nullptr ||
            settings::find_key(info.name)->key != info.key) {
            return fail(d, Text().put("name lookup: ").put(info.name).view());
        }
        if (info.name.size() > 15) {
            return fail(d, Text().put("NVS key too long: ").put(info.name).view());
        }
    }
    Text text;
    text.num(static_cast<std::int64_t>(settings::schema().size())).put(" keys valid");
    return pass(d, text.view());
}

Outcome settings_reject(Context& /*ctx*/, Detail& d) noexcept {
    const Settings before = settings::defaults();
    for (const settings::KeyInfo& info : settings::schema()) {
        const KeyCase* c = case_for(info.key);
        if (c == nullptr) {
            return fail(d, Text().put("no case for ").put(info.name).view());
        }
        for (const std::string_view bad : {c->bad1, c->bad2}) {
            Settings s = before;
            if (settings::set_from_string(s, info.key, bad)) {
                return fail(d, Text().put(info.name).put(" accepted bad value").view());
            }
            if (!(s == before)) {
                return fail(d, Text().put(info.name).put(" changed on error").view());
            }
        }
    }
    // Full-object validation catches out-of-range fields set directly.
    Settings bad_interval = before;
    bad_interval.sync_interval_h = 7;
    Settings bad_goal = before;
    bad_goal.step_goal = 750;
    Settings bad_face = before;
    bad_face.face_id = 200; // not registered
    for (const Settings* s : {&bad_interval, &bad_goal, &bad_face}) {
        if (settings::validate(*s, &faces::is_registered)) {
            return fail(d, "validate() accepted an invalid object");
        }
    }
    return pass(d, "all keys reject bad values");
}

Outcome settings_strings(Context& /*ctx*/, Detail& d) noexcept {
    const Settings defaults = settings::defaults();
    for (const settings::KeyInfo& info : settings::schema()) {
        const KeyCase* c = case_for(info.key);
        if (c == nullptr) {
            return fail(d, Text().put("no case for ").put(info.name).view());
        }
        // default -> text -> default
        std::array<char, 80> buf{};
        const std::size_t n = settings::format_value(defaults, info.key, buf);
        Settings again = defaults;
        if (const Status s = settings::set_from_string(again, info.key, {buf.data(), n}); !s) {
            return fail_error(d, info.name, s.error());
        }
        if (!(again == defaults)) {
            return fail(d, Text().put(info.name).put(" default not stable").view());
        }
        // sample -> text -> sample
        Settings sample = defaults;
        if (const Status s = settings::set_from_string(sample, info.key, c->sample); !s) {
            return fail_error(d, info.name, s.error());
        }
        if (sample == defaults) {
            return fail(d, Text().put(info.name).put(" sample equals default").view());
        }
        const std::size_t m = settings::format_value(sample, info.key, buf);
        Settings copy = defaults;
        if (const Status s = settings::set_from_string(copy, info.key, {buf.data(), m}); !s) {
            return fail_error(d, info.name, s.error());
        }
        if (!(copy == sample)) {
            return fail(d, Text().put(info.name).put(" text round trip differs").view());
        }
    }
    return pass(d, "text round trip for every key");
}

Outcome settings_persist(Context& ctx, Detail& d) noexcept {
    if (ctx.kv == nullptr) {
        return skip(d, "no kv store");
    }
    RedirectKv kv(*ctx.kv);
    Settings sample;
    if (!make_sample_settings(sample, d)) {
        return Outcome::kFail;
    }
    (void)kv.erase_namespace(tuning::kTestNamespace); // clean slate; absent namespace is fine
    settings::SettingsStore store(kv);
    if (const Status s = store.save(sample, settings::defaults()); !s) {
        return fail_error(d, "save", s.error());
    }
    // A second store instance proves the values came from the KV, not from cached state.
    settings::SettingsStore reader(kv);
    const Result<Settings> loaded = reader.load();
    (void)kv.erase_namespace(tuning::kTestNamespace);
    (void)kv.commit();
    if (!loaded) {
        return fail_error(d, "load", loaded.error());
    }
    if (!(*loaded == sample)) {
        return fail(d, "loaded settings differ from saved");
    }
    return pass(d, "save/load in qz_test");
}

Outcome settings_restore(Context& ctx, Detail& d) noexcept {
    if (ctx.kv == nullptr) {
        return skip(d, "no kv store");
    }
    RedirectKv kv(*ctx.kv);
    Settings sample;
    if (!make_sample_settings(sample, d)) {
        return Outcome::kFail;
    }
    settings::SettingsStore store(kv);
    if (const Status s = store.save(sample, settings::defaults()); !s) {
        return fail_error(d, "save", s.error());
    }
    const Status erased = store.erase_all();
    const Result<Settings> loaded = store.load();
    (void)kv.erase_namespace(tuning::kTestNamespace);
    (void)kv.commit();
    if (!erased) {
        return fail_error(d, "erase_all", erased.error());
    }
    if (!loaded) {
        return fail_error(d, "load", loaded.error());
    }
    if (!(*loaded == settings::defaults())) {
        return fail(d, "defaults not restored after erase");
    }
    return pass(d, "erase restores defaults");
}

} // namespace qz::selftest::tests
