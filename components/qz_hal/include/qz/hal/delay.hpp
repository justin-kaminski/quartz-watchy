// Short blocking delays for driver sequencing (esp_rom_delay_us / vTaskDelay on target;
// virtual-time advance in qz_testkit). Thread-safe on target.
#pragma once

#include <cstdint>

namespace qz::hal {

class Delay {
public:
    virtual ~Delay() = default;
    virtual void delay_us(std::uint32_t us) = 0;
    virtual void delay_ms(std::uint32_t ms) = 0;
};

} // namespace qz::hal
