// FakeConsolePort: scripted request lines in, captured response lines out.
// See the semantics block on the class in fakes.hpp.
#include "qz/core/result.hpp"
#include "qz/testkit/fakes.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace qz::testkit {

Status FakeConsolePort::start() {
    ++starts_;
    if (start_failure_.has_value()) {
        const Error failure = *start_failure_;
        start_failure_.reset();
        return failure;
    }
    if (running_) {
        return Errc::kInvalidState; // the tether policy must stop() before starting again
    }
    running_ = true;
    return ok();
}

void FakeConsolePort::stop() {
    ++stops_;
    running_ = false;
}

Result<std::size_t> FakeConsolePort::receive_line(std::span<char> out, std::uint32_t timeout_ms) {
    if (!running_) {
        return Errc::kInvalidState; // reading from a console that is not up is a policy bug
    }
    if (requests_.empty()) {
        if (clock_ != nullptr) {
            clock_->delay_ms(timeout_ms); // a real blocking read burns its whole timeout
        }
        return std::size_t{0};
    }
    const std::string line = std::move(requests_.front());
    requests_.pop_front();
    if (line.size() > out.size()) {
        return Errc::kNoSpace; // the line is discarded, as on the device
    }
    std::ranges::copy(line, out.begin());
    return line.size();
}

void FakeConsolePort::send_line(std::string_view line) {
    sent_.emplace_back(line);
}

void FakeConsolePort::push_request(std::string line) {
    requests_.push_back(std::move(line));
}

const std::vector<std::string>& FakeConsolePort::sent() const {
    return sent_;
}

std::uint32_t FakeConsolePort::start_count() const {
    return starts_;
}

std::uint32_t FakeConsolePort::stop_count() const {
    return stops_;
}

bool FakeConsolePort::running() const {
    return running_;
}

std::size_t FakeConsolePort::pending_requests() const {
    return requests_.size();
}

void FakeConsolePort::fail_next_start(Error error) {
    start_failure_ = error;
}

} // namespace qz::testkit
