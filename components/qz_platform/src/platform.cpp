// IdfPlatform: static storage for every IDF-backed HAL object plus the boot sequence.
#include "console_port.hpp"
#include "esp_log.h"
#include "platform_impl.hpp"
#include "qz/platform/idf_platform.hpp"

namespace qz::platform {
namespace {

constexpr char kTag[] = "qz_platform";

/// Declaration order is construction order (and the dependency order): IdfSystem first so its
/// constructor captures the RTC time, reset reason and wake causes before anything else runs.
/// Constructors do no hardware access except IdfSystem's reads and the console's static mutex;
/// hardware is brought up in IdfPlatform::init.
struct Objects {
    IdfSystem system;
    IdfBoardIo io;
    IdfAdc adc;
    IdfDelay delay;
    IdfClock clock;
    IdfRtcMemory rtc;
    IdfKvStore kv;
    IdfSleep sleep{io};
    IdfEpdBus epd{sleep, delay};
    IdfI2cDevice accel;
    IdfConsolePort console{sleep};
};

/// Function-local static: constructed on first use, no heap, never destroyed in practice.
Objects& objects() noexcept {
    static Objects instance;
    return instance;
}

bool g_ready = false;

} // namespace

IdfPlatform& IdfPlatform::instance() noexcept {
    static IdfPlatform platform;
    return platform;
}

Result<IdfPlatform*> IdfPlatform::init() noexcept {
    IdfPlatform& platform = instance();
    Objects& o = objects(); // constructs IdfSystem first (boot RTC time / reset / wake capture)
    if (g_ready) {
        return &platform;
    }
    // 1. Board GPIO: vibration driven low first (a floating pad may run the motor), buttons / USB
    //    detect / charge status as plain inputs.
    if (const Status s = o.io.init(); !s) {
        ESP_LOGE(kTag, "board io init failed (code %u)", static_cast<unsigned>(s.error().code));
        return s.error();
    }
    // 2. EPD bus. IdfEpdBus::init() calls IdfSleep::release_holds() first (drops every deep-sleep
    //    pad hold and re-drives the parked levels) and only then brings up SPI2, as the WP-25 tech
    //    debt requires at every boot.
    if (const Status s = o.epd.init(); !s) {
        ESP_LOGE(kTag, "epd bus init failed (code %u)", static_cast<unsigned>(s.error().code));
        return s.error();
    }
    // 3. Accelerometer bus. Not fatal: without it steps/tap are unavailable but the face works.
    if (const Status s = o.accel.init(); !s) {
        ESP_LOGW(kTag,
                 "accelerometer i2c init failed (code %u); continuing without it",
                 static_cast<unsigned>(s.error().code));
    }
    g_ready = true;
    return &platform;
}

hal::EpdBus& IdfPlatform::epd() noexcept {
    return objects().epd;
}
hal::I2cDevice& IdfPlatform::accel_i2c() noexcept {
    return objects().accel;
}
hal::Delay& IdfPlatform::delay() noexcept {
    return objects().delay;
}
hal::BoardIo& IdfPlatform::io() noexcept {
    return objects().io;
}
hal::Adc& IdfPlatform::battery_adc() noexcept {
    return objects().adc;
}
hal::KvStore& IdfPlatform::kv() noexcept {
    return objects().kv;
}
hal::RtcMemory& IdfPlatform::rtc_memory() noexcept {
    return objects().rtc;
}
hal::Clock& IdfPlatform::clock() noexcept {
    return objects().clock;
}
hal::SleepControl& IdfPlatform::sleep() noexcept {
    return objects().sleep;
}
hal::System& IdfPlatform::system() noexcept {
    return objects().system;
}
hal::ConsolePort& IdfPlatform::console() noexcept {
    return objects().console;
}

void IdfPlatform::set_radio_active(bool active) noexcept {
    objects().sleep.set_radio_active(active);
}

} // namespace qz::platform
