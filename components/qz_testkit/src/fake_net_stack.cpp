// FakeNetStack: scriptable hal::NetStack with virtual-time latency. See fakes.hpp.
#include "qz/testkit/fakes.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <utility>

namespace qz::testkit {

FakeNetStack::FakeNetStack(VirtualClock& clock) : clock_(&clock) {}

FakeNetStack::Step FakeNetStack::next_step(std::deque<Step>& queue, std::optional<Step>& last) {
    if (!queue.empty()) {
        last = std::move(queue.front());
        queue.pop_front();
    }
    return last.has_value() ? *last : Step{};
}

bool FakeNetStack::wait(std::uint32_t latency_ms, std::uint32_t timeout_ms) {
    const std::uint32_t spent = std::min(latency_ms, timeout_ms);
    clock_->advance_us(static_cast<std::int64_t>(spent) * 1000);
    return latency_ms <= timeout_ms;
}

Status FakeNetStack::connect(const hal::WifiCredentials& creds, std::uint32_t timeout_ms) {
    calls_.push_back(NetCall::kConnect);
    ++connects_;
    ++init_count_;
    connect_timeout_ms_ = timeout_ms;
    last_credentials_ = creds;
    if (radio_on_) {
        ++violations_;
        return Errc::kInvalidState;
    }
    radio_on_ = true; // esp_wifi_init happened even if association fails below
    const Step step = next_step(connect_q_, connect_last_);
    if (!wait(step.latency_ms, timeout_ms)) {
        return Errc::kTimeout;
    }
    return step.status;
}

Result<std::int64_t> FakeNetStack::sntp_sync(std::uint32_t timeout_ms,
                                             std::int64_t* rtc_us_at_utc) {
    calls_.push_back(NetCall::kSntp);
    ++sntps_;
    sntp_timeout_ms_ = timeout_ms;
    if (!radio_on_) {
        ++violations_;
        return Errc::kInvalidState;
    }
    const Step step = next_step(sntp_q_, sntp_last_);
    if (!wait(step.latency_ms, timeout_ms)) {
        return Errc::kTimeout;
    }
    if (!step.status) {
        return step.status.error();
    }
    if (rtc_us_at_utc != nullptr) {
        *rtc_us_at_utc = clock_->rtc_us();
    }
    return step.utc_us.value_or(clock_->true_utc_us());
}

Result<hal::HttpResponse>
FakeNetStack::https_get(std::string_view url, std::span<char> body, std::uint32_t timeout_ms) {
    calls_.push_back(NetCall::kHttp);
    ++https_;
    http_timeout_ms_ = timeout_ms;
    last_url_.assign(url);
    if (!radio_on_) {
        ++violations_;
        return Errc::kInvalidState;
    }
    const Step step = next_step(http_q_, http_last_);
    if (!wait(step.latency_ms, timeout_ms)) {
        return Errc::kTimeout;
    }
    if (!step.status) {
        return step.status.error();
    }
    hal::HttpResponse resp;
    resp.status = step.http_status;
    resp.body_len = std::min(step.body.size(), body.size());
    resp.truncated = step.body.size() > body.size();
    if (resp.body_len > 0) {
        std::memcpy(body.data(), step.body.data(), resp.body_len);
    }
    return resp;
}

void FakeNetStack::shutdown() {
    calls_.push_back(NetCall::kShutdown);
    ++shutdowns_;
    radio_on_ = false; // idempotent
}

std::uint32_t FakeNetStack::radio_init_count() const {
    return init_count_;
}

void FakeNetStack::script_connect(Status result, std::uint32_t latency_ms) {
    Step step;
    step.status = result;
    step.latency_ms = latency_ms;
    connect_q_.push_back(std::move(step));
}

void FakeNetStack::script_sntp(Status result, std::uint32_t latency_ms) {
    Step step;
    step.status = result;
    step.latency_ms = latency_ms;
    sntp_q_.push_back(std::move(step));
}

void FakeNetStack::script_sntp_utc(std::int64_t utc_us, std::uint32_t latency_ms) {
    Step step;
    step.latency_ms = latency_ms;
    step.utc_us = utc_us;
    sntp_q_.push_back(std::move(step));
}

void FakeNetStack::script_http(std::uint16_t status, std::string body, std::uint32_t latency_ms) {
    Step step;
    step.latency_ms = latency_ms;
    step.http_status = status;
    step.body = std::move(body);
    http_q_.push_back(std::move(step));
}

void FakeNetStack::script_http_error(Error error, std::uint32_t latency_ms) {
    Step step;
    step.status = error;
    step.latency_ms = latency_ms;
    http_q_.push_back(std::move(step));
}

std::uint32_t FakeNetStack::connect_calls() const {
    return connects_;
}
std::uint32_t FakeNetStack::sntp_calls() const {
    return sntps_;
}
std::uint32_t FakeNetStack::http_calls() const {
    return https_;
}
std::uint32_t FakeNetStack::shutdown_calls() const {
    return shutdowns_;
}
std::uint32_t FakeNetStack::violations() const {
    return violations_;
}
bool FakeNetStack::radio_on() const {
    return radio_on_;
}
const std::vector<NetCall>& FakeNetStack::calls() const {
    return calls_;
}
const hal::WifiCredentials& FakeNetStack::last_credentials() const {
    return last_credentials_;
}
std::uint32_t FakeNetStack::last_connect_timeout_ms() const {
    return connect_timeout_ms_;
}
std::uint32_t FakeNetStack::last_sntp_timeout_ms() const {
    return sntp_timeout_ms_;
}
std::uint32_t FakeNetStack::last_http_timeout_ms() const {
    return http_timeout_ms_;
}
const std::string& FakeNetStack::last_url() const {
    return last_url_;
}

} // namespace qz::testkit
