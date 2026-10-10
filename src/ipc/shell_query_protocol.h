#pragma once

#include "protocol.h"

namespace pulse::ipc {

enum class ShellQueryKind : uint32_t {
    Icon = 1, TypeName = 2, Shortcut = 3, DisplayName = 4,
    ParsePidl = 5, PidlName = 6, FolderIdentity = 7, InitializePreviewItem = 8
};

struct ShellQueryRequest {
    ShellQueryKind kind = ShellQueryKind::TypeName;
    std::wstring path;
    uint32_t attributes = 0;
    bool use_attributes = false;
    std::vector<uint8_t> pidl;
    // Only InitializePreviewItem accepts COM's opaque marshal format. Its
    // table-strong owner releases the data on the original preview STA.
    std::vector<uint8_t> marshal_blob;
};

// list_id is a Shell image-list SIZE (0/1/2/4), never an image index or handle.
// Pixels are premultiplied BGRA, including normalization of padded jumbo icons.
struct ShellIconPixels {
    uint32_t list_id = 0;
    uint32_t width = 0, height = 0, stride = 0;
    std::vector<uint8_t> pixels;
};

struct ShellQueryResult {
    HRESULT error = E_FAIL;
    std::wstring text;
    bool target_is_dir = false;
    uint64_t target_size = 0;
    FILETIME target_mtime{};
    std::vector<ShellIconPixels> icons;
    std::vector<uint8_t> pidl;
    std::wstring filesystem_path;
};

constexpr uint32_t kShellQueryVersion = 3;
constexpr uint32_t kShellQueryRequestFrame = 0x7fff0201;
constexpr uint32_t kShellQueryResponseFrame = 0x7fff0202;
constexpr uint32_t kShellQueryPixelLimit = 512;
constexpr size_t kShellQueryPidlLimit = 64 * 1024;
constexpr size_t kShellQueryMarshalLimit = 64 * 1024;

// A PIDL is a bounded sequence of SHITEMID byte records, ending with exactly
// one zero USHORT. No pointer, handle, or COM object crosses this boundary.
inline bool ValidShellPidl(const uint8_t* bytes, size_t size) {
    if (!bytes || size < sizeof(uint16_t) || size > kShellQueryPidlLimit) return false;
    size_t offset = 0;
    while (size - offset >= sizeof(uint16_t)) {
        uint16_t record_size = 0;
        std::memcpy(&record_size, bytes + offset, sizeof(record_size));
        if (!record_size) return offset + sizeof(record_size) == size;
        if (record_size < sizeof(record_size) || record_size > size - offset) return false;
        offset += record_size;
    }
    return false;
}

inline bool ValidShellPidl(const std::vector<uint8_t>& bytes) {
    return ValidShellPidl(bytes.data(), bytes.size());
}

inline bool ValidShellQueryText(const std::wstring& text) {
    return text.size() <= 32768 && text.find(L'\0') == std::wstring::npos;
}

inline std::vector<uint8_t> WriteShellQuery(const ShellQueryRequest& request) {
    PayloadWriter writer;
    writer.PutU32(kShellQueryVersion);
    writer.PutU32(static_cast<uint32_t>(request.kind));
    writer.PutU32(request.attributes);
    writer.PutU32(request.use_attributes ? 1u : 0u);
    writer.PutString(request.path);
    writer.PutU32(static_cast<uint32_t>(request.pidl.size()));
    writer.PutU32(static_cast<uint32_t>(request.marshal_blob.size()));
    std::vector<uint8_t> bytes = writer.data();
    bytes.insert(bytes.end(), request.pidl.begin(), request.pidl.end());
    bytes.insert(bytes.end(), request.marshal_blob.begin(), request.marshal_blob.end());
    return bytes;
}

inline bool ReadShellQuery(const uint8_t* bytes, size_t size, ShellQueryRequest& request) {
    if (size > kMaxPayload) return false;
    PayloadReader reader(bytes, size);
    ShellQueryRequest decoded;
    uint32_t version = 0, kind = 0, flags = 0, pidl_size = 0, marshal_size = 0;
    if (!reader.GetU32(version) || version != kShellQueryVersion || !reader.GetU32(kind) ||
        kind < 1 || kind > 8 || !reader.GetU32(decoded.attributes) ||
        !reader.GetU32(flags) || flags > 1 || !reader.GetString(decoded.path) ||
        !ValidShellQueryText(decoded.path) || !reader.GetU32(pidl_size) || !reader.GetU32(marshal_size) ||
        pidl_size > kShellQueryPidlLimit || marshal_size > kShellQueryMarshalLimit ||
        static_cast<size_t>(pidl_size) + marshal_size != reader.remaining()) return false;
    decoded.kind = static_cast<ShellQueryKind>(kind);
    decoded.use_attributes = flags != 0;
    if (decoded.kind != ShellQueryKind::Icon && (flags || decoded.attributes)) return false;
    if (decoded.kind == ShellQueryKind::PidlName) {
        if (marshal_size || !decoded.path.empty() || !ValidShellPidl(bytes + size - pidl_size, pidl_size)) return false;
        try { decoded.pidl.assign(bytes + size - pidl_size, bytes + size); }
        catch (const std::bad_alloc&) { return false; }
    } else if (decoded.kind == ShellQueryKind::InitializePreviewItem) {
        if (decoded.path.empty() || pidl_size || !marshal_size) return false;
        try { decoded.marshal_blob.assign(bytes + size - marshal_size, bytes + size); }
        catch (const std::bad_alloc&) { return false; }
    } else if (decoded.path.empty() || pidl_size || marshal_size) return false;
    request = std::move(decoded);
    return true;
}

inline bool ValidShellIconPixels(const ShellIconPixels& icon) {
    return (icon.list_id == 0 || icon.list_id == 1 || icon.list_id == 2 || icon.list_id == 4) &&
        icon.width && icon.height && icon.width <= kShellQueryPixelLimit &&
        icon.height <= kShellQueryPixelLimit && icon.stride == icon.width * 4 &&
        icon.pixels.size() == static_cast<size_t>(icon.stride) * icon.height;
}

inline std::vector<uint8_t> WriteShellQueryResult(const ShellQueryResult& result) {
    PayloadWriter writer;
    writer.PutU32(kShellQueryVersion);
    writer.PutU32(static_cast<uint32_t>(result.error));
    writer.PutString(result.text);
    writer.PutU32(result.target_is_dir ? 1u : 0u);
    writer.PutU64(result.target_size);
    writer.PutU32(result.target_mtime.dwLowDateTime);
    writer.PutU32(result.target_mtime.dwHighDateTime);
    writer.PutString(result.filesystem_path);
    writer.PutU32(static_cast<uint32_t>(result.pidl.size()));
    writer.PutU32(static_cast<uint32_t>(result.icons.size()));
    std::vector<uint8_t> bytes = writer.data();
    bytes.insert(bytes.end(), result.pidl.begin(), result.pidl.end());
    for (const auto& icon : result.icons) {
        PayloadWriter fields;
        fields.PutU32(icon.list_id);
        fields.PutU32(icon.width);
        fields.PutU32(icon.height);
        fields.PutU32(icon.stride);
        fields.PutU32(static_cast<uint32_t>(icon.pixels.size()));
        bytes.insert(bytes.end(), fields.data().begin(), fields.data().end());
        bytes.insert(bytes.end(), icon.pixels.begin(), icon.pixels.end());
    }
    return bytes;
}

inline bool ReadShellQueryResult(const uint8_t* bytes, size_t size, ShellQueryResult& result) {
    if (size > kMaxPayload) return false;
    PayloadReader reader(bytes, size);
    ShellQueryResult decoded;
    uint32_t version = 0, error = 0, is_dir = 0, low = 0, high = 0, count = 0, pidl_size = 0;
    if (!reader.GetU32(version) || version != kShellQueryVersion || !reader.GetU32(error) ||
        !reader.GetString(decoded.text) || !ValidShellQueryText(decoded.text) ||
        !reader.GetU32(is_dir) || is_dir > 1 || !reader.GetU64(decoded.target_size) ||
        !reader.GetU32(low) || !reader.GetU32(high) || !reader.GetString(decoded.filesystem_path) ||
        !ValidShellQueryText(decoded.filesystem_path) || !reader.GetU32(pidl_size) ||
        pidl_size > kShellQueryPidlLimit || !reader.GetU32(count) || count > 4 ||
        pidl_size > reader.remaining()) return false;
    decoded.error = static_cast<HRESULT>(error);
    decoded.target_is_dir = is_dir != 0;
    decoded.target_mtime = {low, high};
    size_t offset = size - reader.remaining();
    uint32_t seen = 0;
    try {
        if (pidl_size) {
            if (!ValidShellPidl(bytes + offset, pidl_size)) return false;
            decoded.pidl.assign(bytes + offset, bytes + offset + pidl_size);
            offset += pidl_size;
        }
        for (uint32_t i = 0; i < count; ++i) {
            PayloadReader fields(bytes + offset, size - offset);
            ShellIconPixels icon;
            uint32_t pixel_bytes = 0;
            if (!fields.GetU32(icon.list_id) || !fields.GetU32(icon.width) ||
                !fields.GetU32(icon.height) || !fields.GetU32(icon.stride) ||
                !fields.GetU32(pixel_bytes) || pixel_bytes > fields.remaining() ||
                icon.list_id > 4 || (seen & (1u << icon.list_id)) ||
                !icon.width || !icon.height || icon.width > kShellQueryPixelLimit ||
                icon.height > kShellQueryPixelLimit || icon.stride != icon.width * 4 ||
                pixel_bytes != static_cast<uint64_t>(icon.stride) * icon.height) return false;
            offset += 20;
            icon.pixels.assign(bytes + offset, bytes + offset + pixel_bytes);
            offset += pixel_bytes;
            if (!ValidShellIconPixels(icon)) return false;
            seen |= 1u << icon.list_id;
            decoded.icons.push_back(std::move(icon));
        }
    } catch (const std::bad_alloc&) { return false; }
    if (offset != size) return false;
    result = std::move(decoded);
    return true;
}

inline std::wstring ShellQueryPipeName(DWORD parent, uint32_t session) {
    return PipeNameFor(parent) + L"_query_" + std::to_wstring(session);
}

} // namespace pulse::ipc
