// RTC-memory regions: validation and commit (ARCHITECTURE.md section 6).
//
// Image = bytes of RtcState exactly as stored. The header fields magic/version/size are checked
// directly; the CRC covers every other byte (boot_count and the whole payload), so any single
// corrupted byte of the image is detected. All copies are byte-wise (no alignment assumed).
#include "qz/app/rtc_state.hpp"
#include "qz/core/crc32.hpp"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <type_traits>

namespace qz::app {

namespace {

constexpr std::size_t kStateBytes = sizeof(RtcState);
constexpr std::size_t kFrameShadowBytes = sizeof(FrameShadow);
/// First byte covered by the state CRC: boot_count (magic/version/size/crc are checked/skipped).
constexpr std::size_t kStateCrcStart = offsetof(RtcHeader, boot_count);
/// Frame shadow CRC covers exactly the framebuffer bytes.
constexpr std::size_t kFrameCrcStart = offsetof(FrameShadow, frame);

static_assert(std::is_standard_layout_v<RtcHeader> && std::is_standard_layout_v<FrameShadow>);
static_assert(offsetof(RtcState, header) == 0, "the header leads the image");
static_assert(sizeof(RtcHeader) == 16 && kStateCrcStart == 12, "crc skips only magic/ver/size/crc");
static_assert(offsetof(RtcHeader, magic) == 0 && offsetof(RtcHeader, version) == 4 &&
              offsetof(RtcHeader, size) == 6 && offsetof(RtcHeader, crc32) == 8);
static_assert(kStateBytes <= 0xFFFFU, "RtcHeader::size is 16 bits");
static_assert(kFrameCrcStart == 8 && kFrameShadowBytes == 8 + gfx::kFrameBytes);

[[nodiscard]] Error corrupt(RtcCorruptReason reason) noexcept {
    return Error{Errc::kCorrupt, static_cast<std::uint16_t>(reason)};
}

[[nodiscard]] std::uint32_t state_crc(std::span<const std::uint8_t> region) noexcept {
    return crc32(region.subspan(kStateCrcStart, kStateBytes - kStateCrcStart));
}

[[nodiscard]] std::uint32_t frame_crc(std::span<const std::uint8_t> region) noexcept {
    return crc32(region.subspan(kFrameCrcStart, gfx::kFrameBytes));
}

template<class T>
[[nodiscard]] T read_at(std::span<const std::uint8_t> region, std::size_t offset) noexcept {
    static_assert(std::is_trivially_copyable_v<T>);
    T value{};
    std::memcpy(&value, region.data() + offset, sizeof(T));
    return value;
}

template<class T>
void write_at(std::span<std::uint8_t> region, std::size_t offset, const T& value) noexcept {
    static_assert(std::is_trivially_copyable_v<T>);
    std::memcpy(region.data() + offset, &value, sizeof(T));
}

} // namespace

RtcStore::RtcStore(std::span<std::uint8_t> state_region,
                   std::span<std::uint8_t> frame_region) noexcept
    : state_(state_region), frame_(frame_region) {}

Status RtcStore::load(RtcState& out) const noexcept {
    if (state_.size() < kStateBytes) {
        return corrupt(RtcCorruptReason::kRegionTooSmall);
    }
    const std::span<const std::uint8_t> image = state_.first(kStateBytes);
    if (read_at<std::uint32_t>(image, offsetof(RtcHeader, magic)) != kRtcStateMagic) {
        return corrupt(RtcCorruptReason::kMagic);
    }
    if (read_at<std::uint16_t>(image, offsetof(RtcHeader, version)) != kRtcStateVersion) {
        return corrupt(RtcCorruptReason::kVersion);
    }
    if (read_at<std::uint16_t>(image, offsetof(RtcHeader, size)) != kStateBytes) {
        return corrupt(RtcCorruptReason::kSize);
    }
    if (read_at<std::uint32_t>(image, offsetof(RtcHeader, crc32)) != state_crc(image)) {
        return corrupt(RtcCorruptReason::kCrc);
    }
    std::memcpy(static_cast<void*>(&out), image.data(), kStateBytes);
    return ok();
}

void RtcStore::commit(const RtcState& state) noexcept {
    QZ_ASSERT(state_.size() >= kStateBytes);
    const std::span<std::uint8_t> image = state_.first(kStateBytes);
    // Invalidate first: a commit cut short by a brownout leaves a magic-less image, not a torn
    // one that happens to carry the old header.
    write_at<std::uint32_t>(image, offsetof(RtcHeader, magic), 0);
    std::memcpy(image.data(), static_cast<const void*>(&state), kStateBytes);
    write_at<std::uint16_t>(image, offsetof(RtcHeader, version), kRtcStateVersion);
    write_at<std::uint16_t>(
        image, offsetof(RtcHeader, size), static_cast<std::uint16_t>(kStateBytes));
    write_at<std::uint32_t>(image, offsetof(RtcHeader, crc32), state_crc(image));
    write_at<std::uint32_t>(image, offsetof(RtcHeader, magic), kRtcStateMagic);
}

Status RtcStore::load_frame(gfx::Framebuffer& out) const noexcept {
    if (frame_.size() < kFrameShadowBytes) {
        return corrupt(RtcCorruptReason::kRegionTooSmall);
    }
    const std::span<const std::uint8_t> image = frame_.first(kFrameShadowBytes);
    if (read_at<std::uint32_t>(image, offsetof(FrameShadow, magic)) != kFrameShadowMagic) {
        return corrupt(RtcCorruptReason::kMagic);
    }
    if (read_at<std::uint32_t>(image, offsetof(FrameShadow, crc32)) != frame_crc(image)) {
        return corrupt(RtcCorruptReason::kCrc);
    }
    std::memcpy(out.bits.data(), image.data() + kFrameCrcStart, gfx::kFrameBytes);
    return ok();
}

void RtcStore::commit_frame(const gfx::Framebuffer& frame) noexcept {
    QZ_ASSERT(frame_.size() >= kFrameShadowBytes);
    const std::span<std::uint8_t> image = frame_.first(kFrameShadowBytes);
    write_at<std::uint32_t>(image, offsetof(FrameShadow, magic), 0);
    std::memcpy(image.data() + kFrameCrcStart, frame.bits.data(), gfx::kFrameBytes);
    write_at<std::uint32_t>(image, offsetof(FrameShadow, crc32), frame_crc(image));
    write_at<std::uint32_t>(image, offsetof(FrameShadow, magic), kFrameShadowMagic);
}

void RtcStore::invalidate() noexcept {
    // Zero the headers (magic, crc, ...): both images fail validation, payload bytes stay.
    const std::size_t state_n = std::min(state_.size(), sizeof(RtcHeader));
    const std::size_t frame_n = std::min(frame_.size(), kFrameCrcStart);
    if (state_n > 0) {
        std::memset(state_.data(), 0, state_n);
    }
    if (frame_n > 0) {
        std::memset(frame_.data(), 0, frame_n);
    }
}

} // namespace qz::app
