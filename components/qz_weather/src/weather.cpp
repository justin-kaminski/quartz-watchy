// Provider-independent weather helpers: WMO mapping, freshness, display units.
#include "qz/weather/provider.hpp"
#include "tuning.hpp"

namespace qz::weather {

using model::WeatherCondition;
using model::WeatherFreshness;

WeatherCondition condition_from_wmo(int code) noexcept {
    // WMO code table 4677 subset used by Open-Meteo (checked 2026-10-06 at open-meteo.com/en/docs).
    switch (code) {
        case 0: // clear sky
        case 1: // mainly clear
            return WeatherCondition::kClear;
        case 2: // partly cloudy
            return WeatherCondition::kPartlyCloudy;
        case 3: // overcast
            return WeatherCondition::kCloudy;
        case 45: // fog
        case 48: // depositing rime fog
            return WeatherCondition::kFog;
        case 51: // drizzle light/moderate/dense
        case 53:
        case 55:
        case 56: // freezing drizzle light/dense
        case 57:
            return WeatherCondition::kDrizzle;
        case 61: // rain slight/moderate/heavy
        case 63:
        case 65:
        case 66: // freezing rain light/heavy
        case 67:
            return WeatherCondition::kRain;
        case 71: // snow fall slight/moderate/heavy
        case 73:
        case 75:
        case 77: // snow grains
        case 85: // snow showers slight/heavy (no separate icon class)
        case 86:
            return WeatherCondition::kSnow;
        case 80: // rain showers slight/moderate/violent
        case 81:
        case 82:
            return WeatherCondition::kShowers;
        case 95: // thunderstorm, with slight/heavy hail
        case 96:
        case 99:
            return WeatherCondition::kThunder;
        default:
            return WeatherCondition::kUnknown;
    }
}

WeatherFreshness freshness(const model::WeatherReport& r,
                           time::UnixSeconds now_utc,
                           std::uint16_t interval_min,
                           bool time_valid) noexcept {
    if (!time_valid || r.valid == 0) {
        return WeatherFreshness::kHidden;
    }
    std::int64_t age_s = now_utc - r.fetched_utc;
    if (age_s < 0) {
        if (-age_s > tuning::kClockSkewToleranceS) {
            return WeatherFreshness::kHidden;
        }
        age_s = 0;
    }
    // The 6 h hide limit dominates even when 2 x interval is longer (interval 360 min).
    if (age_s > tuning::kHideAfterS) {
        return WeatherFreshness::kHidden;
    }
    const std::int64_t fresh_s = tuning::kFreshIntervals * static_cast<std::int64_t>(interval_min) *
                                 tuning::kSecondsPerMinute;
    return age_s <= fresh_s ? WeatherFreshness::kFresh : WeatherFreshness::kStale;
}

std::int16_t display_degrees(std::int16_t temp_dc, model::TempUnit unit) noexcept {
    // Exact integer math: value in 1/den units (den = 10 for C; 50 for F, i.e. deci-F x 5),
    // rounded half away from zero. Integers cannot be -0, so "-0" never reaches the display.
    std::int32_t num = temp_dc;
    std::int32_t den = tuning::kDeciPerDegreeInt;
    if (unit == model::TempUnit::kFahrenheit) {
        num = (num * tuning::kFahrenheitNum) +
              (tuning::kFahrenheitOffsetDeciF * tuning::kFahrenheitDen);
        den = tuning::kDeciPerDegreeInt * tuning::kFahrenheitDen;
    }
    const std::int32_t mag = num < 0 ? -num : num;
    const std::int32_t rounded = (mag + (den / 2)) / den;
    return static_cast<std::int16_t>(num < 0 ? -rounded : rounded);
}

} // namespace qz::weather
