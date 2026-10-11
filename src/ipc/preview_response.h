#pragma once
#include "preview_protocol.h"

namespace pulse::ipc {
struct PreviewResponseLayout {
    size_t pixel_bytes = 0;
    size_t allocation_budget = 0;
};

inline bool ValidatePreviewResponse(const PreviewRequest& request, const PreviewResponse& response,
                                    PreviewResponseLayout& layout) noexcept {
    layout = {};
    if (response.magic != kPreviewMagic || response.request_id != request.request_id ||
        response.generation != request.generation || response.status < 0 || response.status > 2 ||
        response.mapping_chars > 512 || response.text_chars > 32768 || response.error_chars > 512 ||
        response.property_count > kPreviewMaxPropertyCount || (response.flags & ~kPreviewFlagTruncated) ||
        !response.frame_count || response.frame_delay_ms > 2000 || response.bytes_read > 32768 ||
        (response.source_width == 0) != (response.source_height == 0)) return false;
    const bool bitmap = response.kind == PreviewContentKind::Bitmap;
    const bool text = response.kind == PreviewContentKind::Text || response.kind == PreviewContentKind::Hex;
    if (!bitmap && !text && response.kind != PreviewContentKind::None &&
        response.kind != PreviewContentKind::Unsupported) return false;
    if (request.kind == PreviewRequestKind::Properties) {
        if (response.status != 0 || response.kind != PreviewContentKind::None || response.width || response.height || response.stride ||
            response.mapping_chars || response.text_chars || response.flags || response.bytes_read ||
            response.source_width || response.source_height || response.frame_count != 1 || response.frame_delay_ms)
            return false;
    } else if (request.kind == PreviewRequestKind::Content) {
        if (response.property_count || (response.status == 0 && !bitmap && !text) ||
            (response.status == 1 && response.kind != PreviewContentKind::Unsupported) ||
            (response.status == 2 && !bitmap)) return false;
    } else return false;
    if (bitmap) {
        const uint32_t cap = ClampPreviewPixelSize(request.pixel_size, false);
        if (!response.width || !response.height || response.width > cap || response.height > cap ||
            static_cast<uint64_t>(response.width) * 4 != response.stride || response.text_chars ||
            response.flags || response.bytes_read ||
            (response.status == 0 ? !response.mapping_chars : response.mapping_chars != 0)) return false;
        layout.pixel_bytes = static_cast<size_t>(response.stride) * response.height;
    } else if (response.mapping_chars || response.width || response.height || response.stride ||
               (!text && (response.text_chars || response.flags || response.bytes_read))) return false;
    // Include all property strings and container overhead before any response allocation.
    const size_t body = (static_cast<size_t>(response.mapping_chars) + response.text_chars + response.error_chars) * sizeof(wchar_t);
    const size_t properties = static_cast<size_t>(response.property_count) *
        ((kPreviewMaxPropertyLabelChars + kPreviewMaxPropertyValueChars) * sizeof(wchar_t) + 2 * sizeof(std::wstring));
    layout.allocation_budget = body + properties + layout.pixel_bytes;
    constexpr size_t max_allocation = 4 * 1024 * 1024 + 128 * 1024;
    return layout.allocation_budget <= max_allocation;
}
}
