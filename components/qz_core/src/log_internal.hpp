// Private seam between log.cpp and assert.cpp (not part of the public contract).
#pragma once

#include "qz/core/log.hpp"

namespace qz::detail {

/// The currently installed sink, or nullptr when none is installed. assert_fail() uses it to
/// pick the destination of its report (sink if installed, stderr otherwise).
[[nodiscard]] LogSink installed_log_sink() noexcept;

} // namespace qz::detail
