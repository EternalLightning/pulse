#pragma once
#include "protocol.h"
#include <cstdint>
#include <string>

namespace pulse::ipc {
constexpr uint32_t kPreviewMagic = 0x57565250; // PRVW
enum class PreviewContentKind : uint32_t {
    None = 0,
    Bitmap = 1,
    Text = 2,
    Hex = 3,
    Unsupported = 4,
};
enum class PreviewRequestKind : uint32_t {
    Content = 0,
    Properties = 1,
};
constexpr uint32_t kPreviewFlagTruncated = 1u << 0;
constexpr uint32_t kPreviewMaxPropertyCount = 6;
constexpr uint32_t kPreviewMaxPropertyLabelChars = 128;
constexpr uint32_t kPreviewMaxPropertyValueChars = 1024;

inline void TruncatePreviewString(std::wstring& value, size_t limit, bool ellipsis = false) {
    if (value.size() <= limit) return;
    size_t end = limit - (ellipsis && limit ? 1u : 0u);
    if (end && value[end - 1] >= 0xD800 && value[end - 1] <= 0xDBFF) --end;
    value.resize(end);
    if (ellipsis && limit) value.push_back(L'\x2026');
}
constexpr uint32_t kPreviewMinPixelSize = 32;
constexpr uint32_t kPreviewDefaultPixelSize = 512;
constexpr uint32_t kPreviewMaxPixelSize = 1024;
constexpr uint32_t kPreviewGifMaxPixelSize = 512;
constexpr uint32_t kPreviewPixelBucket = 128;

inline uint32_t ClampPreviewPixelSize(uint32_t requested, bool gif) noexcept {
    const uint32_t cap = gif ? kPreviewGifMaxPixelSize : kPreviewMaxPixelSize;
    if (requested < kPreviewMinPixelSize) return kPreviewMinPixelSize;
    if (requested > cap) return cap;
    return requested;
}

inline uint32_t BucketPreviewPixelSize(uint32_t longest_edge) noexcept {
    if (longest_edge <= kPreviewDefaultPixelSize) return kPreviewDefaultPixelSize;
    const uint32_t bucket = ((longest_edge + kPreviewPixelBucket - 1u) /
                             kPreviewPixelBucket) * kPreviewPixelBucket;
    return bucket > kPreviewMaxPixelSize ? kPreviewMaxPixelSize : bucket;
}
struct PreviewRequest {
    uint32_t magic = kPreviewMagic;
    uint32_t request_id = 0;
    uint64_t generation = 0;
    PreviewRequestKind kind = PreviewRequestKind::Content;
    uint32_t pixel_size = 0;
    uint32_t attrs = 0;
    uint32_t path_chars = 0;
    uint32_t frame_index = 0;
};
struct PreviewResponse {
    uint32_t magic = kPreviewMagic;
    uint32_t request_id = 0;
    uint64_t generation = 0;
    int32_t status = 0;
    PreviewContentKind kind = PreviewContentKind::None;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t stride = 0;
    uint32_t mapping_chars = 0;
    uint32_t text_chars = 0;
    uint32_t error_chars = 0;
    uint32_t property_count = 0;
    uint32_t flags = 0;
    uint32_t bytes_read = 0;
    uint32_t frame_count = 1;
    uint32_t frame_delay_ms = 0;
    uint32_t loop_count = 0;
    uint32_t source_width = 0;
    uint32_t source_height = 0;
};
inline std::wstring PreviewPipeName(DWORD pid) {
    return L"\\\\.\\pipe\\PulsePreview-" + std::to_wstring(pid);
}
static_assert(sizeof(PreviewRequest) == 40);
static_assert(sizeof(PreviewResponse) == 80);
inline bool ReadAll(HANDLE h, void* data, DWORD bytes) {
    return PipeRead(h, data, bytes, {.overlapped = false});
}
inline bool WriteAll(HANDLE h, const void* data, DWORD bytes) {
    return PipeWrite(h, data, bytes, {.overlapped = false});
}
} // namespace pulse::ipc
