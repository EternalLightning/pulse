#pragma once

#include "protocol.h"

namespace pulse::ipc {

enum class ShellLaunchKind : uint32_t { Execute, Command, Reveal, PickFile, PickFolder, ElevatedWait };
constexpr uint32_t kShellLaunchVersion = 2;
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
    return writer.data();
}

inline bool ReadShellLaunch(const uint8_t* data, size_t size, ShellLaunchRequest& request) {
    PayloadReader reader(data, size);
    uint32_t version = 0, kind = 0;
    ShellLaunchRequest parsed;
    if (!reader.GetU32(version) || version != kShellLaunchVersion ||
        !reader.GetU32(kind) || kind > static_cast<uint32_t>(ShellLaunchKind::ElevatedWait) ||
        !reader.GetU64(parsed.owner_hwnd) || !reader.GetU32(parsed.flags) ||
        (parsed.flags & ~kShellLaunchImageNeighbors) || !reader.GetStringArray(parsed.paths) ||
        !reader.GetString(parsed.verb) || !reader.GetString(parsed.executable) ||
        !reader.GetString(parsed.parameters) || !reader.GetString(parsed.directory) ||
        !reader.GetString(parsed.title) || !reader.GetStringArray(parsed.filters) || reader.remaining()) return false;
    parsed.kind = static_cast<ShellLaunchKind>(kind);
    const auto has_nul = [](const std::wstring& value) { return value.find(L'\0') != std::wstring::npos; };
    if (has_nul(parsed.verb) || has_nul(parsed.executable) || has_nul(parsed.parameters) ||
        has_nul(parsed.directory) || has_nul(parsed.title) || parsed.filters.size() > 64 ||
        parsed.filters.size() % 2) return false;
    for (const auto& filter : parsed.filters) if (filter.empty() || has_nul(filter)) return false;
    for (const auto& path : parsed.paths) if (path.empty() || has_nul(path)) return false;
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
