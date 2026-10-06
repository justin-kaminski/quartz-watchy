// hal::KvStore over NVS (ARCHITECTURE.md section 7): lazy init, one handle per operation.
#include "nvs_flash.h"
#include "platform_impl.hpp"
#include "qz/core/log.hpp"

#include <algorithm>
#include <array>
#include <cstring>

namespace qz::platform {
namespace {

constexpr const char* kTag = "kv";
/// NVS namespace and key names are at most 15 characters (NVS_KEY_NAME_MAX_SIZE - 1)
/// [IDF:components/nvs_flash/include/nvs.h].
constexpr std::size_t kMaxNameChars = 15;

struct Names {
    std::array<char, kMaxNameChars + 1> ns{};
    std::array<char, kMaxNameChars + 1> key{};
};

bool copy_name(std::string_view in, std::array<char, kMaxNameChars + 1>& out) noexcept {
    if (in.empty() || in.size() > kMaxNameChars) {
        return false;
    }
    std::copy(in.begin(), in.end(), out.begin());
    out[in.size()] = '\0';
    return true;
}

/// Zeroes a buffer that may have held a credential; volatile stops the store being elided.
void wipe(char* data, std::size_t size) noexcept {
    volatile char* p = data;
    for (std::size_t i = 0; i < size; ++i) {
        p[i] = 0;
    }
}

/// RAII nvs handle.
class Handle {
public:
    Handle() noexcept = default;
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    ~Handle() {
        if (open_) {
            nvs_close(handle_);
        }
    }
    esp_err_t open(const char* ns, nvs_open_mode_t mode) noexcept {
        const esp_err_t err = nvs_open(ns, mode, &handle_);
        open_ = (err == ESP_OK);
        return err;
    }
    [[nodiscard]] nvs_handle_t get() const noexcept { return handle_; }

private:
    nvs_handle_t handle_ = 0;
    bool open_ = false;
};

Status prepare(IdfKvStore& kv,
               std::string_view ns,
               std::string_view key,
               Names& names,
               Handle& handle,
               nvs_open_mode_t mode) noexcept {
    if (!copy_name(ns, names.ns) || !copy_name(key, names.key)) {
        return Errc::kBadArgs;
    }
    if (const Status s = kv.ensure_init(); !s) {
        return s;
    }
    const esp_err_t err = handle.open(names.ns.data(), mode);
    return err == ESP_OK ? Status{} : Status{to_error(err)};
}

template<class T>
Result<T> get_scalar(IdfKvStore& kv,
                     std::string_view ns,
                     std::string_view key,
                     esp_err_t (*getter)(nvs_handle_t, const char*, T*)) noexcept {
    Names names;
    Handle handle;
    if (const Status s = prepare(kv, ns, key, names, handle, NVS_READONLY); !s) {
        return s.error();
    }
    T value{};
    const esp_err_t err = getter(handle.get(), names.key.data(), &value);
    if (err != ESP_OK) {
        return to_error(err);
    }
    return value;
}

template<class T>
Status set_scalar(IdfKvStore& kv,
                  std::string_view ns,
                  std::string_view key,
                  T value,
                  esp_err_t (*setter)(nvs_handle_t, const char*, T)) noexcept {
    Names names;
    Handle handle;
    if (const Status s = prepare(kv, ns, key, names, handle, NVS_READWRITE); !s) {
        return s;
    }
    esp_err_t err = setter(handle.get(), names.key.data(), value);
    if (err == ESP_OK) {
        err = nvs_commit(handle.get()); // no-op in IDF today; keeps the contract if that changes
    }
    return err == ESP_OK ? Status{} : Status{to_error(err)};
}

} // namespace

Error to_error(esp_err_t err) noexcept {
    const auto detail = static_cast<std::uint16_t>(static_cast<std::uint32_t>(err) & 0xFFFFU);
    switch (err) {
        case ESP_ERR_NVS_NOT_FOUND:
            return Error{Errc::kNotFound, detail};
        case ESP_ERR_NVS_INVALID_LENGTH:
        case ESP_ERR_NVS_NOT_ENOUGH_SPACE:
        case ESP_ERR_NVS_VALUE_TOO_LONG:
        case ESP_ERR_NVS_NO_FREE_PAGES:
            return Error{Errc::kNoSpace, detail};
        case ESP_ERR_INVALID_ARG:
        case ESP_ERR_NVS_INVALID_NAME:
        case ESP_ERR_NVS_KEY_TOO_LONG:
            return Error{Errc::kBadArgs, detail};
        case ESP_ERR_NVS_TYPE_MISMATCH:
            return Error{Errc::kCorrupt, detail};
        case ESP_ERR_TIMEOUT:
            return Error{Errc::kTimeout, detail};
        case ESP_ERR_NOT_SUPPORTED:
            return Error{Errc::kUnsupported, detail};
        case ESP_ERR_INVALID_STATE:
        case ESP_ERR_NVS_NOT_INITIALIZED:
        case ESP_ERR_NVS_READ_ONLY:
        case ESP_ERR_NVS_INVALID_HANDLE:
            return Error{Errc::kInvalidState, detail};
        case ESP_ERR_NO_MEM:
            return Error{Errc::kInternal, detail};
        default:
            return Error{Errc::kIo, detail};
    }
}

Status IdfKvStore::ensure_init() noexcept {
    if (inited_) {
        return {};
    }
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        // IDF-documented recovery for a full/foreign-format partition
        // [IDF:components/nvs_flash/include/nvs_flash.h nvs_flash_init]. The data is lost: this
        // equals a factory reset (docs/PARTITIONS.md). The app restores defaults when it sees
        // erased_on_init().
        QZ_LOGW(kTag, "nvs partition unusable (0x%x), erasing", static_cast<unsigned>(err));
        err = nvs_flash_erase();
        if (err == ESP_OK) {
            erased_ = true;
            err = nvs_flash_init();
        }
    }
    if (err != ESP_OK) {
        return to_error(err);
    }
    inited_ = true;
    return {};
}

Result<std::uint32_t> IdfKvStore::get_u32(std::string_view ns, std::string_view key) {
    return get_scalar<std::uint32_t>(*this, ns, key, &nvs_get_u32);
}

Status IdfKvStore::set_u32(std::string_view ns, std::string_view key, std::uint32_t value) {
    return set_scalar<std::uint32_t>(*this, ns, key, value, &nvs_set_u32);
}

Result<std::int32_t> IdfKvStore::get_i32(std::string_view ns, std::string_view key) {
    return get_scalar<std::int32_t>(*this, ns, key, &nvs_get_i32);
}

Status IdfKvStore::set_i32(std::string_view ns, std::string_view key, std::int32_t value) {
    return set_scalar<std::int32_t>(*this, ns, key, value, &nvs_set_i32);
}

Result<std::int64_t> IdfKvStore::get_i64(std::string_view ns, std::string_view key) {
    return get_scalar<std::int64_t>(*this, ns, key, &nvs_get_i64);
}

Status IdfKvStore::set_i64(std::string_view ns, std::string_view key, std::int64_t value) {
    return set_scalar<std::int64_t>(*this, ns, key, value, &nvs_set_i64);
}

Result<std::size_t>
IdfKvStore::get_str(std::string_view ns, std::string_view key, std::span<char> out) {
    Names names;
    Handle handle;
    if (const Status s = prepare(*this, ns, key, names, handle, NVS_READONLY); !s) {
        return s.error();
    }
    // Stack staging buffer so the caller's buffer needs no room for the NUL. Strings longer than
    // kMaxStrBytes cannot be written through set_str, so a longer stored string means foreign
    // data: reported as kNoSpace by the length query below.
    std::array<char, kMaxStrBytes> staging{};
    std::size_t length = staging.size(); // in: capacity incl. NUL; out: length incl. NUL
    const esp_err_t err = nvs_get_str(handle.get(), names.key.data(), staging.data(), &length);
    if (err != ESP_OK) {
        wipe(staging.data(), staging.size());
        return to_error(err);
    }
    const std::size_t chars = length == 0 ? 0 : length - 1;
    Result<std::size_t> result = chars;
    if (chars > out.size()) {
        result = Errc::kNoSpace;
    } else {
        std::copy_n(staging.begin(), chars, out.begin());
    }
    wipe(staging.data(), staging.size());
    return result;
}

Status IdfKvStore::set_str(std::string_view ns, std::string_view key, std::string_view value) {
    if (value.size() >= kMaxStrBytes) {
        return Errc::kBadArgs;
    }
    Names names;
    Handle handle;
    if (const Status s = prepare(*this, ns, key, names, handle, NVS_READWRITE); !s) {
        return s;
    }
    std::array<char, kMaxStrBytes> staging{};
    std::copy(value.begin(), value.end(), staging.begin()); // NUL from value-initialization
    esp_err_t err = nvs_set_str(handle.get(), names.key.data(), staging.data());
    wipe(staging.data(), staging.size());
    if (err == ESP_OK) {
        err = nvs_commit(handle.get());
    }
    return err == ESP_OK ? Status{} : Status{to_error(err)};
}

Result<std::size_t>
IdfKvStore::get_blob(std::string_view ns, std::string_view key, std::span<std::uint8_t> out) {
    Names names;
    Handle handle;
    if (const Status s = prepare(*this, ns, key, names, handle, NVS_READONLY); !s) {
        return s.error();
    }
    std::size_t length = 0;
    // nullptr out_value queries the stored size [IDF:components/nvs_flash/include/nvs.h
    // nvs_get_blob].
    esp_err_t err = nvs_get_blob(handle.get(), names.key.data(), nullptr, &length);
    if (err != ESP_OK) {
        return to_error(err);
    }
    if (length > out.size()) {
        return Errc::kNoSpace;
    }
    if (length == 0) {
        return std::size_t{0};
    }
    err = nvs_get_blob(handle.get(), names.key.data(), out.data(), &length);
    if (err != ESP_OK) {
        return to_error(err);
    }
    return length;
}

Status IdfKvStore::set_blob(std::string_view ns,
                            std::string_view key,
                            std::span<const std::uint8_t> value) {
    Names names;
    Handle handle;
    if (const Status s = prepare(*this, ns, key, names, handle, NVS_READWRITE); !s) {
        return s;
    }
    esp_err_t err = nvs_set_blob(handle.get(), names.key.data(), value.data(), value.size());
    if (err == ESP_OK) {
        err = nvs_commit(handle.get());
    }
    return err == ESP_OK ? Status{} : Status{to_error(err)};
}

Status IdfKvStore::erase_key(std::string_view ns, std::string_view key) {
    Names names;
    {
        // Probe read-only first: opening read-write would create a missing namespace (a flash
        // write). A missing namespace or key is kNotFound, matching the testkit fake.
        Handle probe;
        if (const Status s = prepare(*this, ns, key, names, probe, NVS_READONLY); !s) {
            return s;
        }
    }
    Handle handle;
    esp_err_t err = handle.open(names.ns.data(), NVS_READWRITE);
    if (err != ESP_OK) {
        return to_error(err);
    }
    err = nvs_erase_key(handle.get(), names.key.data());
    if (err == ESP_OK) {
        err = nvs_commit(handle.get());
    }
    return err == ESP_OK ? Status{} : Status{to_error(err)};
}

Status IdfKvStore::erase_namespace(std::string_view ns) {
    Names names;
    if (!copy_name(ns, names.ns)) {
        return Errc::kBadArgs;
    }
    if (const Status s = ensure_init(); !s) {
        return s;
    }
    {
        // Probe read-only first: opening read-write would create the namespace (a flash write).
        Handle probe;
        const esp_err_t err = probe.open(names.ns.data(), NVS_READONLY);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            return {}; // nothing to erase
        }
        if (err != ESP_OK) {
            return to_error(err);
        }
    }
    Handle handle;
    esp_err_t err = handle.open(names.ns.data(), NVS_READWRITE);
    if (err != ESP_OK) {
        return to_error(err);
    }
    err = nvs_erase_all(handle.get());
    if (err == ESP_OK) {
        err = nvs_commit(handle.get());
    }
    return err == ESP_OK ? Status{} : Status{to_error(err)};
}

Status IdfKvStore::commit() {
    return ensure_init();
}

} // namespace qz::platform
