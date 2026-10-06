// qz_weather tunables and protocol constants (one table per component; ARCHITECTURE.md section 2).
#pragma once

#include <cstdint>

namespace qz::weather::tuning {

// ---- Open-Meteo request (docs checked 2026-10-06 against https://open-meteo.com/en/docs) ----
// Parameters verified there: latitude, longitude, current (temperature_2m, weather_code, is_day),
// daily (temperature_2m_max/_min), timezone ("auto" -> location's own zone; default is GMT),
// forecast_days (0-16), temperature_unit ("celsius" default). is_day is not requested: the model
// has no slot for it, so it would only cost response bytes.
// "timezone=auto" so the daily high/low cover the *local* calendar day, not the UTC day.
inline constexpr char kUrlPrefix[] = "https://api.open-meteo.com/v1/forecast?latitude=";
inline constexpr char kUrlMiddle[] = "&longitude=";
inline constexpr char kUrlSuffix[] =
    "&current=temperature_2m,weather_code&daily=temperature_2m_max,temperature_2m_min"
    "&timezone=auto&forecast_days=1&temperature_unit=celsius";

inline constexpr char kProviderName[] = "open-meteo";

// ---- coordinates ----
inline constexpr std::int32_t kMaxLatE5 = 9'000'000;
inline constexpr std::int32_t kMaxLonE5 = 18'000'000;
inline constexpr std::int32_t kE5Scale = 100'000;
inline constexpr int kE5Digits = 5;

// ---- response sanity ----
/// Plausible air temperature window (deg C); outside it the field is treated as corrupt.
inline constexpr double kMinPlausibleTempC = -100.0;
inline constexpr double kMaxPlausibleTempC = 100.0;
inline constexpr double kDeciPerDegree = 10.0;
inline constexpr int kMaxWmoCode = 999;

// ---- freshness ----
inline constexpr std::int64_t kSecondsPerMinute = 60;
/// Fresh while age <= 2 x interval (ARCHITECTURE section 12).
inline constexpr std::int64_t kFreshIntervals = 2;
/// Beyond this the report is hidden regardless of interval (section 12: 6 h).
inline constexpr std::int64_t kHideAfterS = std::int64_t{6} * 3600;
/// A fetch stamp ahead of "now" by up to this much is SNTP-correction noise (counts as age 0);
/// further ahead means the clock was wrong when it was stamped -> hidden. [TUNE]
inline constexpr std::int64_t kClockSkewToleranceS = 300;

// ---- temperature conversion ----
inline constexpr std::int32_t kDeciPerDegreeInt = 10;
inline constexpr std::int32_t kFahrenheitNum = 9;
inline constexpr std::int32_t kFahrenheitDen = 5;
inline constexpr std::int32_t kFahrenheitOffsetDeciF = 320; ///< 32 F in deci-F

} // namespace qz::weather::tuning
