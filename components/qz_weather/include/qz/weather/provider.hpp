// Weather provider interface + Open-Meteo implementation (ARCHITECTURE.md section 12).
#pragma once

#include "qz/core/result.hpp"
#include "qz/model/types.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace qz::weather {

inline constexpr std::size_t kMaxBodyBytes = 4096;
inline constexpr std::size_t kMaxUrlBytes = 320;

/// Swappable provider (Open-Meteo free tier is non-commercial). Stateless, reentrant.
class Provider {
public:
    virtual ~Provider() = default;
    [[nodiscard]] virtual std::string_view name() const = 0;
    /// HTTPS URL for the location; returns its length. kNoSpace if `out` is too small.
    [[nodiscard]] virtual Result<std::size_t> build_url(const model::Location& loc,
                                                        std::span<char> out) const = 0;
    /// Parses a response body (heap allowed: radio path). kCorrupt on malformed/missing fields.
    [[nodiscard]] virtual Result<model::WeatherReport> parse(std::string_view body,
                                                             time::UnixSeconds now_utc) const = 0;
};

class OpenMeteoProvider final : public Provider {
public:
    [[nodiscard]] std::string_view name() const override;
    [[nodiscard]] Result<std::size_t> build_url(const model::Location& loc,
                                                std::span<char> out) const override;
    [[nodiscard]] Result<model::WeatherReport> parse(std::string_view body,
                                                     time::UnixSeconds now_utc) const override;
};

/// WMO weather interpretation code -> condition icon class.
[[nodiscard]] model::WeatherCondition condition_from_wmo(int code) noexcept;
/// fresh <= 2 x interval; stale <= 6 h; otherwise (or time invalid / never fetched) hidden.
[[nodiscard]] model::WeatherFreshness freshness(const model::WeatherReport& r,
                                                time::UnixSeconds now_utc,
                                                std::uint16_t interval_min,
                                                bool time_valid) noexcept;
/// Deci-degrees C -> whole degrees in the display unit, rounded half away from zero.
[[nodiscard]] std::int16_t display_degrees(std::int16_t temp_dc, model::TempUnit unit) noexcept;

} // namespace qz::weather
