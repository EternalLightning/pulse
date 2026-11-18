// SPDX-License-Identifier: Apache-2.0
#include "home_quick_access.h"
#include "../common/path_utils.h"
#include <algorithm>

namespace pulse::model {
std::wstring NormalizeHomeQuickAccessFolder(std::wstring_view input) {
    if (input.empty() || input.size() > 32760) return {};
    std::wstring folder(input);
    std::replace(folder.begin(), folder.end(), L'/', L'\\');
    folder = path::StripExtendedPathPrefix(folder);
    const bool drive = folder.size() >= 3 && folder[1] == L':' && folder[2] == L'\\' &&
        ((folder[0] >= L'A' && folder[0] <= L'Z') || (folder[0] >= L'a' && folder[0] <= L'z'));
    const bool unc = folder.starts_with(L"\\\\") && !folder.starts_with(L"\\\\.\\") && !folder.starts_with(L"\\\\?\\");
    if (!drive && !unc) return {};
    for (size_t i = 0; i < folder.size(); ++i) {
        const auto c = folder[i];
        if (c < 0x20 || std::wstring_view(L"<>\"|*?").find(c) != std::wstring_view::npos ||
            (c == L':' && !(drive && i == 1))) return {};
    }
    if (unc) {
        const auto share = folder.find(L'\\', 2);
        if (share == std::wstring::npos || share == 2 || share + 1 >= folder.size() || folder[share + 1] == L'\\') return {};
    }
    wchar_t full[32768]{};
    const DWORD length = GetFullPathNameW(folder.c_str(), ARRAYSIZE(full), full, nullptr);
    if (!length || length >= ARRAYSIZE(full)) return {};
    folder.assign(full, length);
    while (folder.size() > 3 && folder.back() == L'\\') folder.pop_back();
    return folder;
}
bool HomeQuickAccessPrefs::AddFolder(std::wstring folder) {
    folder = NormalizeHomeQuickAccessFolder(folder);
    if (folder.empty() || std::any_of(folders.begin(), folders.end(), [&](const auto& existing) {
        return path::EqualInsensitive(NormalizeHomeQuickAccessFolder(existing), folder);
    })) return false;
    folders.push_back(std::move(folder));
    return true;
}
bool HomeQuickAccessPrefs::RemoveFolder(std::wstring_view folder) {
    const auto normalized = NormalizeHomeQuickAccessFolder(folder);
    if (normalized.empty()) return false;
    return std::erase_if(folders, [&](const auto& existing) {
        return path::EqualInsensitive(NormalizeHomeQuickAccessFolder(existing), normalized);
    }) != 0;
}
bool HomeQuickAccessPrefs::MoveFolder(std::wstring_view folder, int direction) {
    if (direction != -1 && direction != 1) return false;
    const auto normalized = NormalizeHomeQuickAccessFolder(folder);
    if (normalized.empty()) return false;
    const auto current = std::find_if(folders.begin(), folders.end(), [&](const auto& existing) {
        return path::EqualInsensitive(NormalizeHomeQuickAccessFolder(existing), normalized);
    });
    if (current == folders.end() || (direction == -1 && current == folders.begin()) ||
        (direction == 1 && current + 1 == folders.end())) return false;
    std::iter_swap(current, current + direction);
    return true;
}
} // namespace pulse::model
