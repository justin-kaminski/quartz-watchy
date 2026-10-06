// FakeRtcMemory: the two RTC_NOINIT byte regions of the device, plus power-loss noise.
#include "qz/testkit/fakes.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

namespace qz::testkit {
namespace {

constexpr std::size_t kBytesPerWord = 8;

/// splitmix64 (public-domain constants): small, fast, and every output bit is well mixed, so a
/// scrambled region looks like power-on RAM garbage to a CRC check.
std::uint64_t next_word(std::uint64_t& state) noexcept {
    state += 0x9E3779B97F4A7C15ULL;
    std::uint64_t z = state;
    z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31U);
}

void fill_with_noise(std::span<std::uint8_t> region, std::uint64_t& state) noexcept {
    std::uint64_t word = 0;
    std::size_t bytes_left = 0; // unused bytes of `word`
    for (std::uint8_t& byte : region) {
        if (bytes_left == 0) {
            word = next_word(state);
            bytes_left = kBytesPerWord;
        }
        byte = static_cast<std::uint8_t>(word & 0xFFU);
        word >>= 8U;
        --bytes_left;
    }
}

} // namespace

std::span<std::uint8_t> FakeRtcMemory::state_region() {
    return state_;
}

std::span<std::uint8_t> FakeRtcMemory::frame_region() {
    return frame_;
}

void FakeRtcMemory::scramble() {
    fill_with_noise(state_, scramble_state_);
    fill_with_noise(frame_, scramble_state_);
}

} // namespace qz::testkit
