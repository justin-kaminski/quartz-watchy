// Persistent key-value store (NVS on target, in-memory with write counters in qz_testkit).
// Namespaces/keys <= 15 chars (ARCHITECTURE.md section 7). Not thread-safe.
// Writes are durable after commit(). Missing key -> Errc::kNotFound.
#pragma once

#include "qz/core/result.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace qz::hal {

class KvStore {
public:
    virtual ~KvStore() = default;

    virtual Result<std::uint32_t> get_u32(std::string_view ns, std::string_view key) = 0;
    virtual Status set_u32(std::string_view ns, std::string_view key, std::uint32_t value) = 0;
    virtual Result<std::int32_t> get_i32(std::string_view ns, std::string_view key) = 0;
    virtual Status set_i32(std::string_view ns, std::string_view key, std::int32_t value) = 0;
    virtual Result<std::int64_t> get_i64(std::string_view ns, std::string_view key) = 0;
    virtual Status set_i64(std::string_view ns, std::string_view key, std::int64_t value) = 0;
    /// Copies the string (no NUL) into `out`; returns its length. kNoSpace if out is too small.
    virtual Result<std::size_t>
    get_str(std::string_view ns, std::string_view key, std::span<char> out) = 0;
    virtual Status set_str(std::string_view ns, std::string_view key, std::string_view value) = 0;
    /// Copies the blob into `out`; returns its size. kNoSpace if out is too small.
    virtual Result<std::size_t>
    get_blob(std::string_view ns, std::string_view key, std::span<std::uint8_t> out) = 0;
    virtual Status
    set_blob(std::string_view ns, std::string_view key, std::span<const std::uint8_t> value) = 0;
    virtual Status erase_key(std::string_view ns, std::string_view key) = 0;
    virtual Status erase_namespace(std::string_view ns) = 0;
    /// Durability point (nvs_commit). Implementations may initialize NVS lazily on first use so
    /// that wakes without persistence never pay the NVS init cost.
    virtual Status commit() = 0;
};

} // namespace qz::hal
