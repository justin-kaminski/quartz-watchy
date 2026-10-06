// Tether policy (ARCHITECTURE.md section 17). The console exists only in kTethered, which is
// reachable only through on_wake() with two consecutive USB-present reads.
#include "qz/app/app.hpp"

namespace qz::app {

namespace {
constexpr std::uint8_t kAbsentPollsToDetach = 2;
}

TetherState TetherPolicy::on_wake(bool usb_first_read, bool usb_second_read) noexcept {
    absent_polls_ = 0;
    state_ =
        (usb_first_read && usb_second_read) ? TetherState::kTethered : TetherState::kUntethered;
    return state_;
}

TetherState TetherPolicy::on_poll(bool usb_present) noexcept {
    // Polls only matter while tethered; in every other state a "present" poll must not create
    // the console (no debounce) and an absent one has nothing to do.
    if (state_ != TetherState::kTethered) {
        return state_;
    }
    if (usb_present) {
        absent_polls_ = 0;
        return state_;
    }
    ++absent_polls_;
    if (absent_polls_ >= kAbsentPollsToDetach) {
        state_ = TetherState::kDetaching;
    }
    return state_;
}

void TetherPolicy::request_sleep() noexcept {
    // Only a tethered watch is awake because of USB; on battery there is nothing to override.
    if (state_ == TetherState::kTethered) {
        state_ = TetherState::kSleepOnce;
        absent_polls_ = 0;
    }
}

void TetherPolicy::on_detached() noexcept {
    state_ = TetherState::kUntethered;
    absent_polls_ = 0;
}

} // namespace qz::app
