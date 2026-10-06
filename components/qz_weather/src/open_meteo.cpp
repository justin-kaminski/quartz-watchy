// Open-Meteo provider: URL builder and response parser (ARCHITECTURE.md section 12).
// cJSON is the only place floating point appears; values are converted to integers at once.
#include "qz/weather/provider.hpp"
#include "tuning.hpp"

#include <cJSON.h>

#include <cmath>
#include <cstdint>
#include <memory>

namespace qz::weather {
namespace {

struct CJsonDeleter {
    void operator()(cJSON* p) const noexcept { cJSON_Delete(p); }
};
using JsonPtr = std::unique_ptr<cJSON, CJsonDeleter>;

// ---- URL writer: bounded, never overruns, locale-independent (no printf) ----
class UrlWriter {
public:
    explicit UrlWriter(std::span<char> out) : out_(out) {}

    void put(char c) {
        if (len_ < out_.size()) {
            out_[len_] = c;
        }
        ++len_; // keeps counting so overflow is detected exactly
    }
    template<std::size_t N>
    void put(const char (&lit)[N]) {
        for (std::size_t i = 0; i + 1 < N; ++i) {
            put(lit[i]);
        }
    }
    /// Signed 1e-5-degree value as "[-]I.FFFFF" (always 5 fractional digits).
    void put_e5(std::int32_t v_e5) {
        const std::int64_t wide = v_e5;
        const std::int64_t mag = wide < 0 ? -wide : wide;
        if (wide < 0) {
            put('-');
        }
        put_uint(mag / tuning::kE5Scale);
        put('.');
        std::int64_t div = tuning::kE5Scale / 10;
        std::int64_t frac = mag % tuning::kE5Scale;
        for (int i = 0; i < tuning::kE5Digits; ++i) {
            put(static_cast<char>('0' + ((frac / div) % 10)));
            div /= 10;
        }
    }
    /// True if everything fit including a terminating NUL (written for C-string callers).
    [[nodiscard]] bool finish() {
        if (len_ >= out_.size()) {
            return false;
        }
        out_[len_] = '\0';
        return true;
    }
    [[nodiscard]] std::size_t length() const { return len_; }

private:
    void put_uint(std::int64_t v) {
        char digits[20];
        std::size_t n = 0;
        do {
            digits[n++] = static_cast<char>('0' + (v % 10));
            v /= 10;
        } while (v != 0);
        while (n > 0) {
            put(digits[--n]);
        }
    }

    std::span<char> out_;
    std::size_t len_ = 0;
};

bool is_number(const cJSON* item) {
    return cJSON_IsNumber(item) != 0;
}
bool is_array(const cJSON* item) {
    return cJSON_IsArray(item) != 0;
}
bool is_object(const cJSON* item) {
    return cJSON_IsObject(item) != 0;
}

/// JSON number -> deci-degrees C (round half away from zero). False if not a plausible number.
bool to_deci_c(const cJSON* item, std::int16_t& out_dc) {
    if (!is_number(item)) {
        return false;
    }
    const double c = item->valuedouble;
    // NaN is rejected explicitly; +-inf fall outside the window.
    if (std::isnan(c) || c < tuning::kMinPlausibleTempC || c > tuning::kMaxPlausibleTempC) {
        return false;
    }
    out_dc = static_cast<std::int16_t>(std::lround(c * tuning::kDeciPerDegree));
    return true;
}

/// First element of a daily array as deci-C.
bool first_daily_dc(const cJSON* daily, const char* key, std::int16_t& out_dc) {
    const cJSON* arr = cJSON_GetObjectItemCaseSensitive(daily, key);
    if (!is_array(arr)) {
        return false;
    }
    return to_deci_c(cJSON_GetArrayItem(arr, 0), out_dc);
}

} // namespace

std::string_view OpenMeteoProvider::name() const {
    return tuning::kProviderName;
}

Result<std::size_t> OpenMeteoProvider::build_url(const model::Location& loc,
                                                 std::span<char> out) const {
    if (loc.lat_e5 < -tuning::kMaxLatE5 || loc.lat_e5 > tuning::kMaxLatE5 ||
        loc.lon_e5 < -tuning::kMaxLonE5 || loc.lon_e5 > tuning::kMaxLonE5) {
        return Errc::kBadArgs;
    }
    UrlWriter w(out);
    w.put(tuning::kUrlPrefix);
    w.put_e5(loc.lat_e5);
    w.put(tuning::kUrlMiddle);
    w.put_e5(loc.lon_e5);
    w.put(tuning::kUrlSuffix);
    if (!w.finish()) {
        return Errc::kNoSpace;
    }
    return w.length();
}

Result<model::WeatherReport> OpenMeteoProvider::parse(std::string_view body,
                                                      time::UnixSeconds now_utc) const {
    if (body.empty() || body.size() > kMaxBodyBytes) {
        return Errc::kCorrupt;
    }
    const JsonPtr root(cJSON_ParseWithLength(body.data(), body.size()));
    if (!root || !is_object(root.get())) {
        return Errc::kCorrupt;
    }

    model::WeatherReport rep{};
    const cJSON* current = cJSON_GetObjectItemCaseSensitive(root.get(), "current");
    if (!is_object(current)) {
        return Errc::kCorrupt;
    }
    if (!to_deci_c(cJSON_GetObjectItemCaseSensitive(current, "temperature_2m"), rep.temp_dc)) {
        return Errc::kCorrupt;
    }
    const cJSON* code_item = cJSON_GetObjectItemCaseSensitive(current, "weather_code");
    if (!is_number(code_item)) {
        return Errc::kCorrupt;
    }
    const double code_d = code_item->valuedouble;
    if (std::isnan(code_d) || code_d < 0.0 || code_d > static_cast<double>(tuning::kMaxWmoCode)) {
        return Errc::kCorrupt;
    }
    const int code = static_cast<int>(code_d);
    if (static_cast<double>(code) != code_d) {
        return Errc::kCorrupt; // WMO codes are integers
    }
    rep.condition = condition_from_wmo(code);

    // Daily high/low are optional: any problem just drops them (current conditions still show).
    const cJSON* daily = cJSON_GetObjectItemCaseSensitive(root.get(), "daily");
    std::int16_t high = 0;
    std::int16_t low = 0;
    if (is_object(daily) && first_daily_dc(daily, "temperature_2m_max", high) &&
        first_daily_dc(daily, "temperature_2m_min", low)) {
        rep.high_dc = high;
        rep.low_dc = low;
        rep.has_high_low = 1;
    }

    rep.fetched_utc = now_utc;
    rep.valid = 1;
    return rep;
}

} // namespace qz::weather
