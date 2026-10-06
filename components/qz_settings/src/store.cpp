// NVS persistence for settings (namespace qz_set) and Wi-Fi credentials (namespace qz_cred).
// Wear policy (ARCHITECTURE.md section 7): only keys that differ are written, one commit.
#include "internal.hpp"
#include "migrate.hpp"
#include "qz/settings/settings.hpp"
#include "qz/time/tz.hpp"
#include "tuning.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string_view>

namespace qz::settings {
namespace {

// Stored string buffers: one byte per FixedString capacity.
constexpr std::size_t kTzNameCap = FixedString<40>::capacity();
constexpr std::size_t kTzPosixCap = FixedString<64>::capacity();

/// Errors that mean "this key holds nothing usable" (fall back to the default). Anything else
/// (flash I/O failure) is propagated so the caller can decide.
bool is_unusable_value(Error e) noexcept {
    return e.code == Errc::kNotFound || e.code == Errc::kCorrupt || e.code == Errc::kNoSpace ||
           e.code == Errc::kBadArgs;
}

template<class T>
Status tolerate(const Result<T>& r) noexcept {
    if (r || is_unusable_value(r.error())) {
        return ok();
    }
    return r.error();
}

/// True if reading produced a value or a present-but-unusable entry.
template<class T>
bool key_exists(const Result<T>& r) noexcept {
    return r || r.error().code != Errc::kNotFound;
}

Result<std::int64_t> read_numeric(hal::KvStore& kv, Key key) noexcept {
    const KeyInfo& ki = info(key);
    if (ki.type == ValueType::kDegrees) {
        const auto v = kv.get_i32(kNamespace, ki.name);
        if (!v) {
            return v.error();
        }
        return static_cast<std::int64_t>(*v);
    }
    const auto v = kv.get_u32(kNamespace, ki.name);
    if (!v) {
        return v.error();
    }
    return static_cast<std::int64_t>(*v);
}

Status write_numeric(hal::KvStore& kv, Key key, std::int64_t value) noexcept {
    const KeyInfo& ki = info(key);
    if (ki.type == ValueType::kDegrees) {
        return kv.set_i32(kNamespace, ki.name, static_cast<std::int32_t>(value));
    }
    return kv.set_u32(kNamespace, ki.name, static_cast<std::uint32_t>(value));
}

/// Did any qz_set key (any type) exist before versioning? Distinguishes a fresh install from v0.
bool namespace_has_keys(hal::KvStore& kv) noexcept {
    std::array<char, 1> probe{};
    for (const KeyInfo& ki : schema()) {
        if (ki.type == ValueType::kZoneName) {
            if (key_exists(kv.get_str(kNamespace, ki.name, probe))) {
                return true;
            }
        } else if (key_exists(read_numeric(kv, ki.key))) {
            return true;
        }
    }
    return key_exists(kv.get_str(kNamespace, tuning::kTzPosixKey, probe));
}

/// Loads the zone: the built-in table wins when the stored name is still listed (so rule updates
/// in a new tzdata take effect); a removed name keeps working through its stored POSIX string.
Status load_zone(hal::KvStore& kv, Settings& s) noexcept {
    std::array<char, kTzNameCap> name_buf{};
    const auto name_len = kv.get_str(kNamespace, info(Key::kTimeZone).name, name_buf);
    QZ_RETURN_IF_ERROR(tolerate(name_len));
    if (!name_len || *name_len == 0) {
        return ok();
    }
    const std::string_view name{name_buf.data(), *name_len};
    Settings candidate = s;
    if (detail::apply_zone_name(candidate, name)) {
        s = candidate;
        return ok();
    }
    std::array<char, kTzPosixCap> posix_buf{};
    const auto posix_len = kv.get_str(kNamespace, tuning::kTzPosixKey, posix_buf);
    QZ_RETURN_IF_ERROR(tolerate(posix_len));
    if (!posix_len) {
        return ok();
    }
    const std::string_view posix{posix_buf.data(), *posix_len};
    if (time::TimeZone::parse(posix) && candidate.tz_name.assign(name) &&
        candidate.tz_posix.assign(posix)) {
        s = candidate;
    }
    return ok();
}

/// Latitude and longitude only count as a location when both are present and valid.
Status load_location(hal::KvStore& kv, Settings& s) noexcept {
    const auto lat = read_numeric(kv, Key::kLatitude);
    const auto lon = read_numeric(kv, Key::kLongitude);
    QZ_RETURN_IF_ERROR(tolerate(lat));
    QZ_RETURN_IF_ERROR(tolerate(lon));
    if (!lat || !lon) {
        return ok();
    }
    Settings candidate = s;
    if (detail::apply_number(candidate, Key::kLatitude, *lat) &&
        detail::apply_number(candidate, Key::kLongitude, *lon)) {
        s = candidate;
    }
    return ok();
}

/// One integer key; an invalid or mistyped value keeps the default already in `s`.
Status load_numeric(hal::KvStore& kv, Settings& s, Key key) noexcept {
    const auto value = read_numeric(kv, key);
    QZ_RETURN_IF_ERROR(tolerate(value));
    if (value) {
        Settings candidate = s;
        if (detail::apply_number(candidate, key, *value)) {
            s = candidate;
        }
    }
    return ok();
}

/// Reads the namespace version and migrates older data. `fresh` = nothing stored at all.
/// Missing/garbled version with keys present = v0. Newer-than-known data is read best effort
/// (unknown keys are ignored, `ver` is never downgraded).
Status open_namespace(hal::KvStore& kv, bool& fresh) noexcept {
    const auto ver = kv.get_u32(kNamespace, tuning::kVerKey);
    QZ_RETURN_IF_ERROR(tolerate(ver));
    fresh = !ver && !namespace_has_keys(kv);
    if (fresh) {
        return ok();
    }
    const std::uint32_t stored = ver ? *ver : 0;
    if (stored >= kSchemaVersion) {
        return ok();
    }
    return run_migrations(
        kv, kNamespace, static_cast<std::uint16_t>(stored), kSchemaVersion, settings_migrations());
}

bool zone_differs(const Settings& a, const Settings& b) noexcept {
    return !(a.tz_name == b.tz_name);
}

bool posix_differs(const Settings& a, const Settings& b) noexcept {
    return !(a.tz_posix == b.tz_posix);
}

/// Location is persisted as the pair lat/lon; absent keys mean "not set".
bool location_differs(const Settings& cur, const Settings& prev) noexcept {
    if (cur.location_set != prev.location_set) {
        return true;
    }
    return cur.location_set && !(cur.location == prev.location);
}

bool key_differs(const Settings& cur, const Settings& prev, Key key) noexcept {
    switch (key) {
        case Key::kTimeZone:
            return zone_differs(cur, prev) || posix_differs(cur, prev);
        case Key::kLatitude:
        case Key::kLongitude:
            return location_differs(cur, prev);
        case Key::kCount:
            return false;
        default:
            return detail::to_number(cur, key) != detail::to_number(prev, key);
    }
}

/// Writes `ver` if the namespace has none (or an older one); never downgrades newer data.
Status ensure_version(hal::KvStore& kv, std::string_view ns) noexcept {
    const auto ver = kv.get_u32(ns, tuning::kVerKey);
    if (ver) {
        if (*ver >= kSchemaVersion) {
            return ok();
        }
    } else if (!is_unusable_value(ver.error())) {
        return ver.error();
    }
    return kv.set_u32(ns, tuning::kVerKey, kSchemaVersion);
}

/// Erases a key that may already be absent.
Status erase_if_present(hal::KvStore& kv, std::string_view ns, std::string_view key) noexcept {
    const Status st = kv.erase_key(ns, key);
    if (!st && st.error().code != Errc::kNotFound) {
        return st;
    }
    return ok();
}

/// Persists one changed key (the caller checked key_differs).
Status
write_key(hal::KvStore& kv, const Settings& cur, const Settings& prev, const KeyInfo& ki) noexcept {
    switch (ki.key) {
        case Key::kTimeZone:
            if (zone_differs(cur, prev)) {
                QZ_RETURN_IF_ERROR(kv.set_str(kNamespace, ki.name, cur.tz_name.view()));
            }
            if (posix_differs(cur, prev)) {
                QZ_RETURN_IF_ERROR(
                    kv.set_str(kNamespace, tuning::kTzPosixKey, cur.tz_posix.view()));
            }
            return ok();
        case Key::kLatitude:
        case Key::kLongitude:
            if (!cur.location_set) {
                return erase_if_present(kv, kNamespace, ki.name);
            }
            return write_numeric(kv, ki.key, detail::to_number(cur, ki.key));
        default:
            return write_numeric(kv, ki.key, detail::to_number(cur, ki.key));
    }
}

void wipe(std::span<char> buf) noexcept {
    volatile char* p = buf.data();
    for (std::size_t i = 0; i < buf.size(); ++i) {
        p[i] = 0;
    }
}

bool is_printable(char c) noexcept {
    const auto u = static_cast<unsigned char>(c);
    return u >= 0x20U && u != 0x7FU;
}

bool is_hex_digit(char c) noexcept {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

} // namespace

// ---- SettingsStore ----

SettingsStore::SettingsStore(hal::KvStore& kv) noexcept : kv_(kv) {}

Result<Settings> SettingsStore::load() noexcept {
    Settings s = defaults();
    bool fresh = false;
    QZ_RETURN_IF_ERROR(open_namespace(kv_, fresh));
    if (fresh) {
        return s; // fresh install: defaults, no flash access beyond reads
    }
    QZ_RETURN_IF_ERROR(load_zone(kv_, s));
    QZ_RETURN_IF_ERROR(load_location(kv_, s));
    for (const KeyInfo& ki : schema()) {
        if (detail::is_numeric_key(ki.key) && ki.type != ValueType::kDegrees) {
            QZ_RETURN_IF_ERROR(load_numeric(kv_, s, ki.key));
        }
    }
    return s;
}

Status SettingsStore::save(const Settings& current, const Settings& previous) noexcept {
    QZ_RETURN_IF_ERROR(validate(current, nullptr));

    bool any = false;
    for (const KeyInfo& ki : schema()) {
        any = any || key_differs(current, previous, ki.key);
    }
    if (!any) {
        return ok(); // nothing to write, nothing to commit
    }
    QZ_RETURN_IF_ERROR(ensure_version(kv_, kNamespace));
    for (const KeyInfo& ki : schema()) {
        if (key_differs(current, previous, ki.key)) {
            QZ_RETURN_IF_ERROR(write_key(kv_, current, previous, ki));
        }
    }
    return kv_.commit();
}

Status SettingsStore::erase_all() noexcept {
    for (const std::string_view ns : tuning::kAllNamespaces) {
        QZ_RETURN_IF_ERROR(kv_.erase_namespace(ns));
    }
    return kv_.commit();
}

// ---- credentials ----

Status validate_credentials(const hal::WifiCredentials& creds) noexcept {
    const std::string_view ssid = creds.ssid.view();
    if (ssid.empty() || ssid.size() > tuning::kSsidMax) {
        return Errc::kBadArgs;
    }
    for (const char c : ssid) {
        if (!is_printable(c)) {
            return Errc::kBadArgs;
        }
    }
    const std::string_view pass = creds.password.reveal();
    if (pass.empty()) {
        return ok(); // open network
    }
    if (pass.size() == tuning::kPskHexLen) {
        for (const char c : pass) {
            if (!is_hex_digit(c)) {
                return Errc::kBadArgs;
            }
        }
        return ok();
    }
    if (pass.size() < tuning::kPassphraseMin || pass.size() > tuning::kPassphraseMax) {
        return Errc::kBadArgs;
    }
    for (const char c : pass) {
        if (!is_printable(c)) {
            return Errc::kBadArgs;
        }
    }
    return ok();
}

CredentialStore::CredentialStore(hal::KvStore& kv) noexcept : kv_(kv) {}

Result<hal::WifiCredentials> CredentialStore::load() noexcept {
    std::array<char, tuning::kSsidMax> ssid_buf{};
    const auto ssid_len = kv_.get_str(kCredNamespace, tuning::kSsidKey, ssid_buf);
    if (!ssid_len) {
        const Errc code = ssid_len.error().code;
        if (code == Errc::kNotFound) {
            return Errc::kNoCredentials;
        }
        if (code == Errc::kCorrupt || code == Errc::kNoSpace) {
            return Errc::kCorrupt;
        }
        return ssid_len.error();
    }
    if (*ssid_len == 0) {
        return Errc::kNoCredentials;
    }

    hal::WifiCredentials creds;
    if (!creds.ssid.assign({ssid_buf.data(), *ssid_len})) {
        return Errc::kCorrupt;
    }

    std::array<char, tuning::kPskHexLen> pass_buf{}; // 64 bytes: the longest valid password
    const auto pass_len = kv_.get_str(kCredNamespace, tuning::kPassKey, pass_buf);
    Result<hal::WifiCredentials> result = creds; // password still empty (open network)
    if (pass_len) {
        if (!creds.password.assign({pass_buf.data(), *pass_len})) {
            wipe(pass_buf);
            return Errc::kCorrupt;
        }
        result = creds;
    } else if (pass_len.error().code != Errc::kNotFound) {
        const Errc code = pass_len.error().code;
        wipe(pass_buf);
        if (code == Errc::kCorrupt || code == Errc::kNoSpace) {
            return Errc::kCorrupt;
        }
        return pass_len.error();
    }
    wipe(pass_buf);
    return result;
}

Status CredentialStore::save(const hal::WifiCredentials& creds) noexcept {
    QZ_RETURN_IF_ERROR(validate_credentials(creds));
    QZ_RETURN_IF_ERROR(ensure_version(kv_, kCredNamespace));
    QZ_RETURN_IF_ERROR(kv_.set_str(kCredNamespace, tuning::kSsidKey, creds.ssid.view()));
    if (creds.password.empty()) {
        QZ_RETURN_IF_ERROR(erase_if_present(kv_, kCredNamespace, tuning::kPassKey));
    } else {
        QZ_RETURN_IF_ERROR(kv_.set_str(kCredNamespace, tuning::kPassKey, creds.password.reveal()));
    }
    return kv_.commit();
}

Status CredentialStore::clear() noexcept {
    QZ_RETURN_IF_ERROR(kv_.erase_namespace(kCredNamespace));
    return kv_.commit();
}

} // namespace qz::settings
