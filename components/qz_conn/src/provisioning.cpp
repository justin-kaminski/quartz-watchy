// conn::Provisioning: SoftAP provisioning session logic (ARCHITECTURE.md section 13).
// Secrets (AP password, form password, token) never reach logs; scratch buffers holding
// decoded secrets are wiped before returning.
#include "qz/conn/conn.hpp"
#include "qz/core/log.hpp"
#include "tuning.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <string_view>

namespace qz::conn {
namespace {

constexpr const char* kTag = "conn";

constexpr std::string_view kSavedPage =
    "<!doctype html><html><head><meta charset=utf-8>"
    "<meta name=viewport content=\"width=device-width,initial-scale=1\"><title>Quartz</title>"
    "</head><body><h1>Saved</h1><p>The watch is connecting. You can close this page.</p>"
    "</body></html>";

constexpr const char* kPageFormat =
    "<!doctype html><html><head><meta charset=utf-8>"
    "<meta name=viewport content=\"width=device-width,initial-scale=1\"><title>Quartz setup</title>"
    "</head><body><h1>Quartz setup</h1><form method=post action=/save>"
    "<input type=hidden name=token value=\"%s\">"
    "<p><label>Wi-Fi name <input name=ssid maxlength=32 required></label></p>"
    "<p><label>Wi-Fi password <input name=pass type=password maxlength=63></label></p>"
    "<p><label>Time zone <input name=tz maxlength=40 value=UTC required></label></p>"
    "<p><label>Latitude <input name=lat maxlength=15></label></p>"
    "<p><label>Longitude <input name=lon maxlength=15></label></p>"
    "<p><label>Units <select name=units><option value=c>Celsius</option>"
    "<option value=f>Fahrenheit</option></select></label></p>"
    "<p><label>Mode <select name=mode><option value=off>Off</option>"
    "<option value=time>Time only</option>"
    "<option value=time+weather selected>Time and weather</option></select></label></p>"
    "<p><button type=submit>Save</button></p></form></body></html>";

void log_rejection(const char* what, Errc code) noexcept {
    const std::string_view token = to_token(code);
    QZ_LOGW(kTag, "%s: %.*s", what, static_cast<int>(token.size()), token.data());
}

/// Zeroes memory in a way the optimizer may not elide.
void wipe(std::span<char> buf) noexcept {
    auto* p = reinterpret_cast<volatile char*>(
        buf.data()); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
    for (std::size_t i = 0; i < buf.size(); ++i) {
        p[i] = 0;
    }
}

constexpr int hex_value(char c) noexcept {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

constexpr bool is_control(char c) noexcept {
    const auto u = static_cast<unsigned char>(c);
    return u < 0x20U || u == 0x7FU;
}

/// application/x-www-form-urlencoded value -> bytes. '+' = space, %XX = byte. Rejects malformed
/// escapes, control characters (including %00) and results longer than dst.
Result<std::size_t> url_decode(std::string_view src, std::span<char> dst) noexcept {
    std::size_t n = 0;
    for (std::size_t i = 0; i < src.size(); ++i) {
        char c = src[i];
        if (c == '+') {
            c = ' ';
        } else if (c == '%') {
            if (i + 2 >= src.size()) {
                return Errc::kBadArgs;
            }
            const int hi = hex_value(src[i + 1]);
            const int lo = hex_value(src[i + 2]);
            if (hi < 0 || lo < 0) {
                return Errc::kBadArgs;
            }
            c = static_cast<char>((hi << 4) | lo);
            i += 2;
        }
        if (is_control(c) || n >= dst.size()) {
            return Errc::kBadArgs;
        }
        dst[n++] = c;
    }
    return n;
}

bool constant_time_equal(std::string_view a, std::string_view b) noexcept {
    unsigned diff = a.size() == b.size() ? 0U : 1U;
    const std::size_t n = a.size() < b.size() ? a.size() : b.size();
    for (std::size_t i = 0; i < n; ++i) {
        diff |= static_cast<unsigned>(static_cast<unsigned char>(a[i]) ^
                                      static_cast<unsigned char>(b[i]));
    }
    return diff == 0U;
}

enum class Field : std::uint8_t { kSsid, kPass, kTz, kLat, kLon, kUnits, kMode, kToken, kCount };
constexpr std::size_t kFieldCount = static_cast<std::size_t>(Field::kCount);

struct FieldName {
    std::string_view key;
    Field field;
};
constexpr std::array<FieldName, kFieldCount> kFieldNames{{
    {"ssid", Field::kSsid},
    {"pass", Field::kPass},
    {"tz", Field::kTz},
    {"lat", Field::kLat},
    {"lon", Field::kLon},
    {"units", Field::kUnits},
    {"mode", Field::kMode},
    {"token", Field::kToken},
}};

template<std::size_t N>
bool store(FixedString<N>& dst, std::string_view v) noexcept {
    return dst.assign(v);
}
bool store(Secret<64>& dst, std::string_view v) noexcept {
    return dst.assign(v);
}

/// Draws one alphabet character without modulo bias (rejection sampling, bounded).
char random_symbol(hal::System& system) noexcept {
    constexpr auto kSymbols = static_cast<unsigned>(kPasswordAlphabet.size());
    constexpr unsigned kAcceptBelow = (256U / kSymbols) * kSymbols;
    constexpr int kMaxDraws = 64; // fallback keeps the loop bounded if the RNG is stuck
    for (int draw = 0;; ++draw) {
        std::uint32_t w = system.random_u32();
        for (int b = 0; b < 4; ++b) {
            const unsigned byte = w & 0xFFU;
            w >>= 8U;
            if (byte < kAcceptBelow || draw >= kMaxDraws) {
                return kPasswordAlphabet[byte % kSymbols];
            }
        }
    }
}

struct ParseState {
    ProvisioningForm form;
    FixedString<16> token;
    bool token_fits = true;
    std::array<bool, kFieldCount> seen{};

    [[nodiscard]] bool has(Field f) const noexcept { return seen[static_cast<std::size_t>(f)]; }
    /// `pass`, `lat`, `lon` must be present (possibly empty); the rest non-empty.
    [[nodiscard]] bool required_present() const noexcept {
        return has(Field::kPass) && has(Field::kLat) && has(Field::kLon) && !form.ssid.empty() &&
               !form.tz_name.empty() && !form.units.empty() && !form.mode.empty();
    }
};

/// Stores a decoded value in its field; false if it does not fit.
bool store_field(Field field, std::string_view value, ParseState& st) noexcept {
    switch (field) {
        case Field::kSsid:
            return store(st.form.ssid, value);
        case Field::kPass:
            return store(st.form.password, value);
        case Field::kTz:
            return store(st.form.tz_name, value);
        case Field::kLat:
            return store(st.form.lat, value);
        case Field::kLon:
            return store(st.form.lon, value);
        case Field::kUnits:
            return store(st.form.units, value);
        case Field::kMode:
            return store(st.form.mode, value);
        case Field::kToken:
            st.token_fits = store(st.token, value);
            return true;
        case Field::kCount:
            break;
    }
    return false;
}

/// Handles one `key=value` pair. Unknown keys are ignored; duplicates, bad escapes and
/// over-long values are kBadArgs. `scratch` is wiped before returning.
Status parse_pair(std::string_view pair, ParseState& st, std::span<char> scratch) noexcept {
    if (pair.empty()) {
        return ok();
    }
    const std::size_t eq = pair.find('=');
    const std::string_view key = pair.substr(0, eq);
    const std::string_view raw =
        eq == std::string_view::npos ? std::string_view{} : pair.substr(eq + 1);
    const FieldName* match = nullptr;
    for (const FieldName& f : kFieldNames) {
        if (f.key == key) {
            match = &f;
            break;
        }
    }
    if (match == nullptr) {
        return ok();
    }
    if (st.has(match->field)) {
        return Errc::kBadArgs; // duplicate key: ambiguous
    }
    st.seen[static_cast<std::size_t>(match->field)] = true;
    const Result<std::size_t> len = url_decode(raw, scratch);
    if (!len) {
        wipe(scratch);
        return len.error();
    }
    const bool fits = store_field(match->field, std::string_view{scratch.data(), *len}, st);
    wipe(scratch);
    return fits ? ok() : Status{Errc::kBadArgs};
}

} // namespace

Provisioning::Provisioning(hal::System& system, FormSink& sink) noexcept
    : system_(system), sink_(sink) {}

void Provisioning::begin(std::int64_t now_rtc_us) noexcept {
    end();
    constexpr std::array<char, 16> kHex{
        '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'A', 'B', 'C', 'D', 'E', 'F'};
    const std::uint64_t id = system_.chip_id();
    std::array<char, 12> ssid{'Q', 'u', 'a', 'r', 't', 'z', '-', 0, 0, 0, 0, 0};
    for (std::size_t i = 0; i < 4; ++i) {
        ssid[7 + i] = kHex[(id >> (12U - (4U * i))) & 0xFU];
    }
    (void)ssid_.assign(std::string_view{ssid.data(), 11}); // 11 chars always fit FixedString<11>

    std::array<char, kPasswordLength> pw{};
    for (char& c : pw) {
        c = random_symbol(system_);
    }
    (void)password_.assign(std::string_view{pw.data(), pw.size()}); // 12 <= 64
    wipe(pw);

    std::array<char, tuning::kTokenLength> tok{};
    for (char& c : tok) {
        c = random_symbol(system_);
    }
    (void)token_.assign(std::string_view{tok.data(), tok.size()}); // 10 <= 16
    wipe(tok);

    begin_rtc_us_ = now_rtc_us;
    active_ = true;
    completed_ = false;
    QZ_LOGI(kTag, "provisioning session started");
}

std::string_view Provisioning::ssid() const noexcept {
    return ssid_.view();
}

const Secret<64>& Provisioning::password() const noexcept {
    return password_;
}

bool Provisioning::expired(std::int64_t now_rtc_us) const noexcept {
    return !active_ || now_rtc_us - begin_rtc_us_ >= tuning::kProvisionTimeoutUs;
}

bool Provisioning::tick(std::int64_t now_rtc_us) noexcept {
    const bool is_expired = expired(now_rtc_us);
    if (is_expired && active_) {
        end();
        QZ_LOGI(kTag, "provisioning session expired");
    }
    return is_expired;
}

void Provisioning::end() noexcept {
    password_.clear();
    token_.clear();
    active_ = false;
}

bool Provisioning::completed() const noexcept {
    return completed_;
}

std::string_view Provisioning::page() {
    if (!active_) {
        page_[0] = '\0';
        return {};
    }
    const int n = std::snprintf(page_.data(), page_.size(), kPageFormat, token_.c_str());
    if (n <= 0 || static_cast<std::size_t>(n) >= page_.size()) {
        page_[0] = '\0';
        return {};
    }
    return {page_.data(), static_cast<std::size_t>(n)};
}

Result<std::string_view> Provisioning::submit(std::string_view form_body) {
    if (!active_ || completed_) {
        return Errc::kInvalidState;
    }
    auto form = parse_form(form_body, token_.view());
    if (!form) {
        log_rejection("provisioning form rejected", form.error().code);
        return form.error();
    }
    const Status applied = sink_.apply(*form);
    if (!applied) {
        log_rejection("provisioning form not applied", applied.error().code);
        return applied.error();
    }
    completed_ = true;
    password_.clear(); // the AP is about to go away; the page secret is no longer needed
    token_.clear();    // one-time token consumed
    QZ_LOGI(kTag, "provisioning form accepted");
    return kSavedPage;
}

Result<ProvisioningForm> Provisioning::parse_form(std::string_view body,
                                                  std::string_view expected_token) noexcept {
    if (body.size() > kMaxFormBytes) {
        return Errc::kNoSpace;
    }
    ParseState st;
    std::array<char, tuning::kDecodeBufBytes> scratch{};
    Status status = ok();
    std::size_t pos = 0;
    while (pos <= body.size() && status) {
        std::size_t amp = body.find('&', pos);
        if (amp == std::string_view::npos) {
            amp = body.size();
        }
        status = parse_pair(body.substr(pos, amp - pos), st, scratch);
        pos = amp + 1;
    }
    wipe(scratch);
    if (!status) {
        return status.error();
    }
    // Authenticate before judging the rest: a missing, oversize or wrong token is the same answer.
    if (expected_token.empty() || !st.seen[static_cast<std::size_t>(Field::kToken)] ||
        !st.token_fits || !constant_time_equal(st.token.view(), expected_token)) {
        return Errc::kInvalidState;
    }
    if (!st.required_present()) {
        return Errc::kBadArgs;
    }
    return std::move(st.form);
}

} // namespace qz::conn
