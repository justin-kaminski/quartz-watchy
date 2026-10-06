#include "qz/testkit/fakes.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace qz::testkit {
namespace {

constexpr std::size_t kMaxNameLen = 15; // NVS namespace/key limit (ARCHITECTURE.md section 7)

enum class Tag : std::uint8_t { kU32 = 1, kI32, kI64, kStr, kBlob };

bool names_ok(std::string_view ns, std::string_view key) noexcept {
    return !ns.empty() && !key.empty() && ns.size() <= kMaxNameLen && key.size() <= kMaxNameLen;
}

std::string full_key(std::string_view ns, std::string_view key) {
    std::string k(ns);
    k.push_back('/');
    k.append(key);
    return k;
}

template<class T>
std::vector<std::uint8_t> encode_int(Tag tag, T value) {
    std::vector<std::uint8_t> bytes(1 + sizeof(T));
    bytes[0] = static_cast<std::uint8_t>(tag);
    std::memcpy(bytes.data() + 1, &value, sizeof(T));
    return bytes;
}

std::vector<std::uint8_t> encode_bytes(Tag tag, std::span<const std::uint8_t> data) {
    std::vector<std::uint8_t> bytes;
    bytes.reserve(1 + data.size());
    bytes.push_back(static_cast<std::uint8_t>(tag));
    bytes.insert(bytes.end(), data.begin(), data.end());
    return bytes;
}

/// Fault-injection gate shared by every mutation: false when the injected failure is due.
bool write_allowed(std::int32_t& fail_after) noexcept {
    if (fail_after == 0) {
        return false;
    }
    if (fail_after > 0) {
        --fail_after;
    }
    return true;
}

template<class T>
Result<T> read_int(const std::map<std::string, std::vector<std::uint8_t>>& entries,
                   Tag tag,
                   std::string_view ns,
                   std::string_view key) {
    if (!names_ok(ns, key)) {
        return Errc::kBadArgs;
    }
    const auto it = entries.find(full_key(ns, key));
    if (it == entries.end()) {
        return Errc::kNotFound;
    }
    // NVS reports a type mismatch as an error distinct from "not found".
    if (it->second.size() != 1 + sizeof(T) || it->second[0] != static_cast<std::uint8_t>(tag)) {
        return Errc::kCorrupt;
    }
    T value{};
    std::memcpy(&value, it->second.data() + 1, sizeof(T));
    return value;
}

Result<std::size_t> read_bytes(const std::map<std::string, std::vector<std::uint8_t>>& entries,
                               Tag tag,
                               std::string_view ns,
                               std::string_view key,
                               std::span<std::uint8_t> out) {
    if (!names_ok(ns, key)) {
        return Errc::kBadArgs;
    }
    const auto it = entries.find(full_key(ns, key));
    if (it == entries.end()) {
        return Errc::kNotFound;
    }
    if (it->second.empty() || it->second[0] != static_cast<std::uint8_t>(tag)) {
        return Errc::kCorrupt;
    }
    const std::size_t size = it->second.size() - 1;
    if (out.size() < size) {
        return Errc::kNoSpace;
    }
    std::copy_n(it->second.begin() + 1, size, out.begin());
    return size;
}

} // namespace

Result<std::uint32_t> FakeKvStore::get_u32(std::string_view ns, std::string_view key) {
    return read_int<std::uint32_t>(entries_, Tag::kU32, ns, key);
}

Status FakeKvStore::set_u32(std::string_view ns, std::string_view key, std::uint32_t value) {
    if (!names_ok(ns, key)) {
        return Errc::kBadArgs;
    }
    if (!write_allowed(fail_after_)) {
        return Errc::kIo;
    }
    entries_[full_key(ns, key)] = encode_int(Tag::kU32, value);
    ++writes_;
    return ok();
}

Result<std::int32_t> FakeKvStore::get_i32(std::string_view ns, std::string_view key) {
    return read_int<std::int32_t>(entries_, Tag::kI32, ns, key);
}

Status FakeKvStore::set_i32(std::string_view ns, std::string_view key, std::int32_t value) {
    if (!names_ok(ns, key)) {
        return Errc::kBadArgs;
    }
    if (!write_allowed(fail_after_)) {
        return Errc::kIo;
    }
    entries_[full_key(ns, key)] = encode_int(Tag::kI32, value);
    ++writes_;
    return ok();
}

Result<std::int64_t> FakeKvStore::get_i64(std::string_view ns, std::string_view key) {
    return read_int<std::int64_t>(entries_, Tag::kI64, ns, key);
}

Status FakeKvStore::set_i64(std::string_view ns, std::string_view key, std::int64_t value) {
    if (!names_ok(ns, key)) {
        return Errc::kBadArgs;
    }
    if (!write_allowed(fail_after_)) {
        return Errc::kIo;
    }
    entries_[full_key(ns, key)] = encode_int(Tag::kI64, value);
    ++writes_;
    return ok();
}

Result<std::size_t>
FakeKvStore::get_str(std::string_view ns, std::string_view key, std::span<char> out) {
    // char and uint8_t views of the same bytes; the copy below never reads through the cast type
    // as anything but bytes.
    auto* bytes = reinterpret_cast<std::uint8_t*>(
        out.data()); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
    return read_bytes(entries_, Tag::kStr, ns, key, {bytes, out.size()});
}

Status FakeKvStore::set_str(std::string_view ns, std::string_view key, std::string_view value) {
    if (!names_ok(ns, key)) {
        return Errc::kBadArgs;
    }
    if (!write_allowed(fail_after_)) {
        return Errc::kIo;
    }
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(
        value.data()); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
    entries_[full_key(ns, key)] = encode_bytes(Tag::kStr, {bytes, value.size()});
    ++writes_;
    return ok();
}

Result<std::size_t>
FakeKvStore::get_blob(std::string_view ns, std::string_view key, std::span<std::uint8_t> out) {
    return read_bytes(entries_, Tag::kBlob, ns, key, out);
}

Status FakeKvStore::set_blob(std::string_view ns,
                             std::string_view key,
                             std::span<const std::uint8_t> value) {
    if (!names_ok(ns, key)) {
        return Errc::kBadArgs;
    }
    if (!write_allowed(fail_after_)) {
        return Errc::kIo;
    }
    entries_[full_key(ns, key)] = encode_bytes(Tag::kBlob, value);
    ++writes_;
    return ok();
}

Status FakeKvStore::erase_key(std::string_view ns, std::string_view key) {
    if (!names_ok(ns, key)) {
        return Errc::kBadArgs;
    }
    const auto it = entries_.find(full_key(ns, key));
    if (it == entries_.end()) {
        return Errc::kNotFound;
    }
    if (!write_allowed(fail_after_)) {
        return Errc::kIo;
    }
    entries_.erase(it);
    ++writes_;
    return ok();
}

Status FakeKvStore::erase_namespace(std::string_view ns) {
    if (ns.empty() || ns.size() > kMaxNameLen) {
        return Errc::kBadArgs;
    }
    std::string prefix(ns);
    prefix.push_back('/');
    // Erasing a namespace that does not exist is not an error (factory reset is idempotent).
    for (auto it = entries_.lower_bound(prefix);
         it != entries_.end() && it->first.starts_with(prefix);) {
        if (!write_allowed(fail_after_)) {
        return Errc::kIo;
    }
        it = entries_.erase(it);
        ++writes_;
    }
    return ok();
}

Status FakeKvStore::commit() {
    ++commits_;
    return ok();
}

std::uint32_t FakeKvStore::write_count() const {
    return writes_;
}

std::uint32_t FakeKvStore::commit_count() const {
    return commits_;
}

std::size_t FakeKvStore::entry_count(std::string_view ns) const {
    std::string prefix(ns);
    prefix.push_back('/');
    return static_cast<std::size_t>(
        std::count_if(entries_.begin(), entries_.end(), [&](const auto& entry) {
            return entry.first.starts_with(prefix);
        }));
}

bool FakeKvStore::contains_text(std::string_view needle) const {
    if (needle.empty()) {
        return false;
    }
    const auto contains = [needle](const auto& haystack_begin, const auto& haystack_end) {
        return std::search(haystack_begin,
                           haystack_end,
                           needle.begin(),
                           needle.end(),
                           [](auto a, auto b) { return static_cast<char>(a) == b; }) !=
               haystack_end;
    };
    return std::ranges::any_of(entries_, [&](const auto& entry) {
        return contains(entry.first.begin(), entry.first.end()) ||
               contains(entry.second.begin(), entry.second.end());
    });
}

void FakeKvStore::fail_writes_after(std::int32_t writes) {
    fail_after_ = writes;
}

} // namespace qz::testkit
