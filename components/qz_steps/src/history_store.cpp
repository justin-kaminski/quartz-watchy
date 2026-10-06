// StepHistoryStore: qz_steps NVS namespace (ARCHITECTURE.md section 7). Compare-first writes so a
// repeated flush of unchanged data costs no flash wear. Little-endian blobs, no struct punning.
#include "qz/steps/step_tracker.hpp"
#include "tuning.hpp"

#include <algorithm>

namespace qz::steps {
namespace {

using Hist = std::array<std::uint8_t, model::kStepHistoryDays * tuning::kEntryBytes>;
using Today = std::array<std::uint8_t, tuning::kEntryBytes>;

void put_entry(std::uint8_t* out, std::int32_t day, std::uint32_t steps) noexcept {
    const auto d = static_cast<std::uint32_t>(day);
    for (std::size_t i = 0; i < 4; ++i) {
        out[i] = static_cast<std::uint8_t>((d >> (8U * i)) & 0xFFU);
        out[4 + i] = static_cast<std::uint8_t>((steps >> (8U * i)) & 0xFFU);
    }
}

model::StepDay get_entry(const std::uint8_t* in) noexcept {
    std::uint32_t d = 0;
    std::uint32_t s = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        d |= static_cast<std::uint32_t>(in[i]) << (8U * i);
        s |= static_cast<std::uint32_t>(in[4 + i]) << (8U * i);
    }
    return model::StepDay{static_cast<time::DayNumber>(d), s};
}

Hist encode_history(const StepState& st) noexcept {
    Hist blob{};
    for (std::size_t i = 0; i < st.history.size(); ++i) {
        const bool used = i < st.history_count;
        put_entry(blob.data() + (i * tuning::kEntryBytes),
                  used ? st.history[i].day : tuning::kEmptyDay,
                  used ? st.history[i].steps : 0U);
    }
    return blob;
}

/// Writes `blob` unless the stored value is identical. Sets `wrote` when a write happened.
Status put_blob_if_changed(hal::KvStore& kv,
                           std::string_view key,
                           std::span<const std::uint8_t> blob,
                           bool& wrote) noexcept {
    std::array<std::uint8_t, tuning::kEntryBytes * model::kStepHistoryDays> current{};
    const auto got = kv.get_blob(tuning::kNamespace, key, current);
    if (got && *got == blob.size() && std::equal(blob.begin(), blob.end(), current.begin())) {
        return ok();
    }
    QZ_RETURN_IF_ERROR(kv.set_blob(tuning::kNamespace, key, blob));
    wrote = true;
    return ok();
}

/// Reads a fixed-size blob. kNotFound -> `present` false; a wrong size is corruption.
Status read_blob(hal::KvStore& kv,
                 std::string_view key,
                 std::span<std::uint8_t> out,
                 bool& present) noexcept {
    present = false;
    const auto got = kv.get_blob(tuning::kNamespace, key, out);
    if (!got) {
        if (got.error().code == Errc::kNotFound) {
            return ok();
        }
        if (got.error().code == Errc::kNoSpace) {
            return Errc::kCorrupt; // larger than the format allows
        }
        return got.error();
    }
    if (*got != out.size()) {
        return Errc::kCorrupt;
    }
    present = true;
    return ok();
}

} // namespace

StepHistoryStore::StepHistoryStore(hal::KvStore& kv) noexcept : kv_(kv) {}

Status StepHistoryStore::save(const StepState& state) noexcept {
    bool wrote = false;
    const auto ver = kv_.get_u32(tuning::kNamespace, tuning::kVerKey);
    if (!ver || *ver != tuning::kSchemaVersion) {
        QZ_RETURN_IF_ERROR(
            kv_.set_u32(tuning::kNamespace, tuning::kVerKey, tuning::kSchemaVersion));
        wrote = true;
    }
    const Hist hist = encode_history(state);
    QZ_RETURN_IF_ERROR(put_blob_if_changed(kv_, tuning::kHistKey, hist, wrote));
    if (state.today_valid != 0) {
        Today today{};
        put_entry(today.data(), state.today_day, state.today);
        QZ_RETURN_IF_ERROR(put_blob_if_changed(kv_, tuning::kTodayKey, today, wrote));
    }
    return wrote ? kv_.commit() : ok();
}

Status StepHistoryStore::load(StepState& state) noexcept {
    const auto ver = kv_.get_u32(tuning::kNamespace, tuning::kVerKey);
    if (!ver) {
        return ver.error(); // kNotFound = nothing was ever flushed
    }
    if (*ver != tuning::kSchemaVersion) {
        return Errc::kCorrupt;
    }
    Hist hist{};
    bool have_hist = false;
    QZ_RETURN_IF_ERROR(read_blob(kv_, tuning::kHistKey, hist, have_hist));
    Today today{};
    bool have_today = false;
    QZ_RETURN_IF_ERROR(read_blob(kv_, tuning::kTodayKey, today, have_today));

    // Parse into locals first so a corrupt blob leaves `state` untouched.
    std::array<model::StepDay, model::kStepHistoryDays> history{};
    std::uint8_t count = 0;
    if (have_hist) {
        bool ended = false;
        for (std::size_t i = 0; i < history.size(); ++i) {
            const model::StepDay e = get_entry(hist.data() + (i * tuning::kEntryBytes));
            if (e.day == tuning::kEmptyDay) {
                ended = true;
                continue;
            }
            const bool newest_first = count == 0 || e.day < history[count - 1U].day;
            if (ended || !newest_first) {
                return Errc::kCorrupt;
            }
            history[count] = e;
            ++count;
        }
    }
    const model::StepDay today_entry = have_today ? get_entry(today.data()) : model::StepDay{};
    if (have_today && count > 0 && today_entry.day <= history[0].day) {
        return Errc::kCorrupt;
    }

    state.history = history;
    state.history_count = count;
    if (have_today) {
        state.today_day = today_entry.day;
        state.today = today_entry.steps;
        state.today_valid = 1;
        state.last_flush_day = today_entry.day;
        state.goal_notified = 0;
    }
    return ok();
}

Status StepHistoryStore::erase() noexcept {
    const Status st = kv_.erase_namespace(tuning::kNamespace);
    if (!st && st.error().code != Errc::kNotFound) {
        return st;
    }
    return kv_.commit();
}

} // namespace qz::steps
