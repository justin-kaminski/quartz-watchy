#include "qz/testkit/fakes.hpp"

#include <algorithm>

namespace qz::testkit {

Status FakePhoneLink::start(std::string_view name) {
    if (start_failure_) {
        const Error e = *start_failure_;
        start_failure_.reset();
        return e;
    }
    ++inits_;
    name_ = std::string(name);
    state_ = hal::PhoneLinkState::kAdvertising;
    return ok();
}

void FakePhoneLink::stop() {
    state_ = hal::PhoneLinkState::kOff;
    passkey_ = 0;
}

hal::PhoneLinkState FakePhoneLink::state() const {
    return state_;
}

std::uint32_t FakePhoneLink::passkey() const {
    return passkey_;
}

Result<std::size_t> FakePhoneLink::receive_line(std::span<char> out, std::uint32_t timeout_ms) {
    ++receives_;
    if (hook) {
        hook(receives_);
    }
    if (state_ != hal::PhoneLinkState::kSecure || requests_.empty()) {
        if (clock_ != nullptr) {
            clock_->delay_ms(timeout_ms);
        }
        return std::size_t{0};
    }
    const std::string line = std::move(requests_.front());
    requests_.pop_front();
    if (line.size() > out.size()) {
        return Errc::kNoSpace;
    }
    std::ranges::copy(line, out.begin());
    return line.size();
}

void FakePhoneLink::send_line(std::string_view line) {
    if (state_ == hal::PhoneLinkState::kSecure) {
        sent_.emplace_back(line);
    }
}

Status FakePhoneLink::forget_bonds() {
    ++forgets_;
    return ok();
}

std::uint32_t FakePhoneLink::radio_init_count() const {
    return inits_;
}

void FakePhoneLink::set_state(hal::PhoneLinkState state) {
    if (state_ != hal::PhoneLinkState::kOff) {
        state_ = state;
    }
}

void FakePhoneLink::set_passkey(std::uint32_t passkey) {
    passkey_ = passkey;
}

void FakePhoneLink::push_request(std::string line) {
    requests_.push_back(std::move(line));
}

void FakePhoneLink::fail_next_start(Error error) {
    start_failure_ = error;
}

const std::vector<std::string>& FakePhoneLink::sent() const {
    return sent_;
}

bool FakePhoneLink::running() const {
    return state_ != hal::PhoneLinkState::kOff;
}

std::string_view FakePhoneLink::name() const {
    return name_;
}

std::uint32_t FakePhoneLink::forget_count() const {
    return forgets_;
}

std::uint32_t FakePhoneLink::receive_count() const {
    return receives_;
}

} // namespace qz::testkit
