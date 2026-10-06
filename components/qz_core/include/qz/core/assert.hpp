// Assertion policy (ARCHITECTURE.md section 2): QZ_ASSERT is for programmer errors only,
// never for I/O failures or user input. It stays enabled in release builds.
#pragma once

namespace qz {

/// Reports a failed assertion and terminates. Firmware: logs file:line, then abort() (panic,
/// reboot, wake log records the reset). Host: std::abort() (usable with death tests).
/// Defined in qz_core (src/assert.cpp); thread-safe.
[[noreturn]] void assert_fail(const char* expr, const char* file, int line) noexcept;

} // namespace qz

#define QZ_ASSERT(cond)                                                                            \
    ((cond) ? static_cast<void>(0) : ::qz::assert_fail(#cond, __FILE__, __LINE__))
