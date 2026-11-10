// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once

#include "protocol.h"
#include "../common/image_open_context.h"

namespace pulse::ipc {

enum class ShellLaunchKind : uint32_t { Execute, Command, Reveal, PickFile, PickFolder, ElevatedWait };
constexpr uint32_t kShellLaunchVersion = 3;
constexpr uint32_t kShellLaunchImageNeighbors = 1;
constexpr HRESULT kShellLaunchOutcomeUnknown = MAKE_HRESULT(SEVERITY_ERROR, FACILITY_ITF, 0x0202);

struct ShellLaunchRequest {
    ShellLaunchKind kind = ShellLaunchKind::Execute;
    uint64_t owner_hwnd = 0;
    uint32_t flags = kShellLaunchImageNeighbors;
    std::vector<std::wstring> paths;
    std::wstring verb;
    std::wstring executable;
    std::wstring parameters;
    std::wstring directory;
    std::wstring title;
    std::vector<std::wstring> filters; // alternating display names and patterns
    ImageOpenContext image_context;
};

inline std::vector<uint8_t> WriteShellLaunch(const ShellLaunchRequest& request) {
    PayloadWriter writer;
    writer.PutU32(kShellLaunchVersion);
    writer.PutU32(static_cast<uint32_t>(request.kind));
    writer.PutU64(request.owner_hwnd);
    writer.PutU32(request.flags);
    writer.PutStringArray(request.paths);
    writer.PutString(request.verb);
    writer.PutString(request.executable);
    writer.PutString(request.parameters);
    writer.PutString(request.directory);
    writer.PutString(request.title);
    writer.PutStringArray(request.filters);
    writer.PutU32(static_cast<uint32_t>(request.image_context.column));
    writer.PutU32(static_cast<uint32_t>(request.image_context.direction));
    writer.PutStringArray(request.image_context.paths);
    return writer.data();
}

inline bool ReadShellLaunch(const uint8_t* data, size_t size, ShellLaunchRequest& request) {
    PayloadReader reader(data, size);
    uint32_t version = 0, kind = 0, column = 0, direction = 0;
    ShellLaunchRequest parsed;
    if (!reader.GetU32(version) || version != kShellLaunchVersion ||
        !reader.GetU32(kind) || kind > static_cast<uint32_t>(ShellLaunchKind::ElevatedWait) ||
        !reader.GetU64(parsed.owner_hwnd) || !reader.GetU32(parsed.flags) ||
        (parsed.flags & ~kShellLaunchImageNeighbors) || !reader.GetStringArray(parsed.paths) ||
        !reader.GetString(parsed.verb) || !reader.GetString(parsed.executable) ||
        !reader.GetString(parsed.parameters) || !reader.GetString(parsed.directory) ||
        !reader.GetString(parsed.title) || !reader.GetStringArray(parsed.filters) ||
        !reader.GetU32(column) || column > static_cast<uint32_t>(model::SortColumn::Path) ||
        !reader.GetU32(direction) || direction > static_cast<uint32_t>(model::SortDirection::Desc) ||
        !reader.GetStringArray(parsed.image_context.paths) || reader.remaining()) return false;
    parsed.image_context.column = static_cast<model::SortColumn>(column);
    parsed.image_context.direction = static_cast<model::SortDirection>(direction);
    parsed.kind = static_cast<ShellLaunchKind>(kind);
    const auto has_nul = [](const std::wstring& value) { return value.find(L'\0') != std::wstring::npos; };
    if (has_nul(parsed.verb) || has_nul(parsed.executable) || has_nul(parsed.parameters) ||
        has_nul(parsed.directory) || has_nul(parsed.title) || parsed.filters.size() > 64 ||
        parsed.filters.size() % 2) return false;
    for (const auto& filter : parsed.filters) if (filter.empty() || has_nul(filter)) return false;
    for (const auto& path : parsed.paths) if (path.empty() || has_nul(path)) return false;
    size_t context_bytes = 0;
    for (const auto& path : parsed.image_context.paths) {
        if (path.empty() || has_nul(path)) return false;
        context_bytes += sizeof(uint32_t) + path.size() * sizeof(wchar_t);
        if (context_bytes > kMaxImageOpenContextBytes) return false;
    }
    if (!parsed.image_context.paths.empty() && (parsed.kind != ShellLaunchKind::Execute ||
        parsed.paths.size() != 1 || !parsed.executable.empty() || !parsed.parameters.empty() ||
        !(parsed.flags & kShellLaunchImageNeighbors) ||
        (!parsed.verb.empty() && _wcsicmp(parsed.verb.c_str(), L"open") != 0))) return false;
    if (parsed.kind == ShellLaunchKind::PickFile || parsed.kind == ShellLaunchKind::PickFolder) {
        if (!parsed.paths.empty() || !parsed.executable.empty() || !parsed.parameters.empty() ||
            !parsed.verb.empty() || parsed.flags ||
            (parsed.kind == ShellLaunchKind::PickFolder && !parsed.filters.empty())) return false;
    } else if (parsed.kind == ShellLaunchKind::Command || parsed.kind == ShellLaunchKind::ElevatedWait) {
        if (parsed.executable.empty() || !parsed.paths.empty()) return false;
    } else if (parsed.kind == ShellLaunchKind::Reveal) {
        if (parsed.paths.size() != 1 || !parsed.executable.empty()) return false;
    } else if (parsed.executable.empty() && parsed.paths.empty()) return false;
    if (parsed.kind != ShellLaunchKind::PickFile && parsed.kind != ShellLaunchKind::PickFolder &&
        (!parsed.title.empty() || !parsed.filters.empty())) return false;
    request = std::move(parsed);
    return true;
}

} // namespace pulse::ipc
