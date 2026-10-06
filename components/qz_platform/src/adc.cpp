// Battery-pin ADC: esp_adc oneshot + curve-fitting calibration, lazily created.
#include "esp_adc/adc_cali_scheme.h"
#include "hal/adc_types.h"
#include "platform_impl.hpp"

namespace qz::platform {
namespace {

/// 12 dB attenuation: calibrated range ends at kAdcCalibratedMaxPinMv (watchy_v3.hpp) [R1 s4].
constexpr adc_atten_t kAtten = ADC_ATTEN_DB_12;
/// ESP32-S3 ADC output is 12 bit [IDF:components/esp_hal_ana_conv/include/hal/adc_types.h].
constexpr adc_bitwidth_t kBitwidth = ADC_BITWIDTH_12;
/// Raw samples averaged per reading to suppress noise. [TUNE] calibrate on hardware (B3).
constexpr int kSamples = 8;

} // namespace

IdfAdc::~IdfAdc() {
    // Teardown failures cannot be reported from a destructor; handles are discarded either way.
    if (cali_ != nullptr) {
        (void)adc_cali_delete_scheme_curve_fitting(cali_);
    }
    if (unit_ != nullptr) {
        (void)adc_oneshot_del_unit(unit_);
    }
}

Status IdfAdc::ensure_init() noexcept {
    if (cali_ != nullptr) {
        return {};
    }
    adc_unit_t unit_id = ADC_UNIT_1;
    adc_channel_t channel = ADC_CHANNEL_0;
    // GPIO9 -> ADC1_CH8 on ESP32-S3 [IDF:components/esp_adc/adc_oneshot.h
    // adc_oneshot_io_to_channel].
    esp_err_t err = adc_oneshot_io_to_channel(board::kBatteryAdc, &unit_id, &channel);
    if (err != ESP_OK) {
        return to_error(err);
    }
    if (unit_ == nullptr) {
        adc_oneshot_unit_init_cfg_t unit_cfg{};
        unit_cfg.unit_id = unit_id;
        unit_cfg.clk_src = static_cast<adc_oneshot_clk_src_t>(0); // 0 = driver default
        unit_cfg.ulp_mode = ADC_ULP_MODE_DISABLE;
        err = adc_oneshot_new_unit(&unit_cfg, &unit_);
        if (err != ESP_OK) {
            unit_ = nullptr;
            return to_error(err);
        }
    }
    adc_oneshot_chan_cfg_t chan_cfg{};
    chan_cfg.atten = kAtten;
    chan_cfg.bitwidth = kBitwidth;
    err = adc_oneshot_config_channel(unit_, channel, &chan_cfg);
    if (err != ESP_OK) {
        return to_error(err);
    }
    adc_cali_curve_fitting_config_t cali_cfg{};
    cali_cfg.unit_id = unit_id;
    cali_cfg.chan = channel;
    cali_cfg.atten = kAtten;
    cali_cfg.bitwidth = kBitwidth;
    // ESP_ERR_NOT_SUPPORTED when the calibration eFuse bits are not burnt; such a chip cannot give
    // a calibrated reading, so it is reported (kUnsupported) rather than guessed
    // [IDF:components/esp_adc/include/esp_adc/adc_cali_scheme.h].
    err = adc_cali_create_scheme_curve_fitting(&cali_cfg, &cali_);
    if (err != ESP_OK) {
        cali_ = nullptr;
        return to_error(err);
    }
    channel_ = channel;
    return {};
}

Result<std::uint16_t> IdfAdc::read_pin_mv() {
    if (const Status s = ensure_init(); !s) {
        return s.error();
    }
    int sum = 0;
    for (int i = 0; i < kSamples; ++i) {
        int raw = 0;
        const esp_err_t err = adc_oneshot_read(unit_, channel_, &raw);
        if (err != ESP_OK) {
            return to_error(err);
        }
        sum += raw;
    }
    int mv = 0;
    const esp_err_t err = adc_cali_raw_to_voltage(cali_, sum / kSamples, &mv);
    if (err != ESP_OK) {
        return to_error(err);
    }
    if (mv < 0) {
        mv = 0;
    }
    if (mv > 0xFFFF) {
        mv = 0xFFFF;
    }
    return static_cast<std::uint16_t>(mv);
}

} // namespace qz::platform
