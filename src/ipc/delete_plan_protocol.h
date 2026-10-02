#pragma once
#include "protocol.h"
#include "../common/path_utils.h"
#include <set>
#include <utility>

namespace pulse::ipc {
// Deletion must not turn an extended Win32 name into a different Shell name.
inline bool IsLosslessDeleteShellPath(std::wstring_view path) {
    const std::wstring parsed = pulse::path::StripExtendedPathPrefix(path);
    size_t begin = 0;
    if (parsed.size() >= 3 && ((parsed[0] >= L'A' && parsed[0] <= L'Z') ||
        (parsed[0] >= L'a' && parsed[0] <= L'z')) && parsed[1] == L':' && parsed[2] == L'\\') begin = 3;
    else if (parsed.starts_with(L"\\\\") && !parsed.starts_with(L"\\\\.\\") && !parsed.starts_with(L"\\\\?\\")) begin = 2;
    else return false;
    size_t components = 0;
    while (begin < parsed.size()) {
        const size_t slash = parsed.find(L'\\', begin);
        const size_t end = slash == std::wstring::npos ? parsed.size() : slash;
        const auto part = std::wstring_view(parsed).substr(begin, end - begin);
        if (part.empty() || part == L"." || part == L".." || part.back() == L'.' || part.back() == L' ') return false;
        for (const wchar_t ch : part) if (ch < L' ' || ch == L':' || ch == L'/' || ch == L'*' || ch == L'?' || ch == L'"' || ch == L'<' || ch == L'>' || ch == L'|') return false;
        const auto stem = part.substr(0, part.find(L'.'));
        for (const auto reserved : {L"CON", L"PRN", L"AUX", L"NUL", L"CONIN$", L"CONOUT$"})
            if (pulse::path::EqualInsensitive(stem, reserved)) return false;
        if (stem.size() == 4 && (pulse::path::EqualInsensitive(stem.substr(0, 3), L"COM") ||
            pulse::path::EqualInsensitive(stem.substr(0, 3), L"LPT")) &&
            ((stem[3] >= L'1' && stem[3] <= L'9') || stem[3] == L'\u00b9' || stem[3] == L'\u00b2' || stem[3] == L'\u00b3')) return false;
        ++components;
        if (slash == std::wstring::npos) break;
        begin = slash + 1;
        if (begin == parsed.size()) return false;
    }
    return components > 0;
}

struct DeleteWirePlan {
    uint64_t host_epoch = 0;
    uint64_t token = 0;
    std::vector<std::wstring> paths;
};

inline bool ReadDeleteWirePlan(PayloadReader& reader, DeleteWirePlan& plan, bool require_end = true) {
    uint32_t count = 0;
    DeleteWirePlan parsed;
    if (!reader.GetU64(parsed.host_epoch) || !reader.GetU64(parsed.token) ||
        !reader.GetU32(count) || count == 0 || count > 100000 || count > reader.remaining() / 4) return false;
    for (uint32_t i = 0; i < count; ++i) {
        std::wstring path;
        if (!reader.GetString(path) || !IsLosslessDeleteShellPath(path)) return false;
        parsed.paths.push_back(std::move(path));
    }
    if ((require_end && reader.remaining() != 0) || parsed.host_epoch == 0 || parsed.token == 0) return false;
    plan = std::move(parsed);
    return true;
}

// STA-only host gate. No execution, no retry, no acceptance of a past host epoch.
class DeleteHostAdmission {
public:
    bool Consume(const DeleteWirePlan& plan, uint64_t current_epoch) {
        if (plan.host_epoch != current_epoch || plan.token == 0 || plan.paths.empty()) return false;
        for (const auto& path : plan.paths) if (!IsLosslessDeleteShellPath(path)) return false;
        return consumed_.insert(plan.token).second;
    }
private:
    std::set<uint64_t> consumed_;
};
}
