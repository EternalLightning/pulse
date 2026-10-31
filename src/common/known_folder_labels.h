// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once
#include "localization.h"
#include <shlobj.h>
#include <array>
#include <span>
#include <mutex>
#include <string>
#include <string_view>

namespace pulse::path {

struct KnownFolderLabel {
    std::wstring path;
    l10n::StringId label;
};

inline std::wstring KnownFolderPathKey(std::wstring_view path) {
    std::wstring key(path);
    for (auto& c : key) if (c == L'/') c = L'\\';
    if (key.starts_with(L"\\\\?\\UNC\\")) key = L"\\\\" + key.substr(8);
    else if (key.starts_with(L"\\\\?\\")) key.erase(0, 4);
    while (key.size() > 3 && key.back() == L'\\') key.pop_back();
    return key;
}

inline const KnownFolderLabel* FindKnownFolderLabel(
    std::wstring_view path, std::span<const KnownFolderLabel> folders) {
    std::wstring storage;
    std::wstring_view key = path;
    if (key.find(L'/') != std::wstring_view::npos || key.starts_with(L"\\\\?\\UNC\\")) {
        storage = KnownFolderPathKey(key);
        key = storage;
    } else {
        if (key.starts_with(L"\\\\?\\")) key.remove_prefix(4);
        while (key.size() > 3 && key.back() == L'\\') key.remove_suffix(1);
    }
    if (key.empty()) return nullptr;
    for (const auto& folder : folders) {
        if (key.size() == folder.path.size() && CompareStringOrdinal(key.data(),
                static_cast<int>(key.size()), folder.path.c_str(),
                static_cast<int>(folder.path.size()), TRUE) == CSTR_EQUAL) return &folder;
    }
    return nullptr;
}

inline const KnownFolderLabel* FindKnownFolderChildLabel(std::wstring_view parent,
    std::wstring_view name, std::span<const KnownFolderLabel> folders) {
    // Ordinary enumerated names need no concatenated path. Unusual separators
    // retain the same normalization as a complete path lookup.
    const auto original_parent = parent;
    if (parent.starts_with(L"\\\\?\\") && !parent.starts_with(L"\\\\?\\UNC\\"))
        parent.remove_prefix(4);
    if (parent.size() >= 3 && (parent[1] == L':' ||
            (parent.starts_with(L"\\\\") && parent[2] != L'?')) &&
        parent.find(L'/') == std::wstring_view::npos &&
        !parent.starts_with(L"\\\\?\\UNC\\") && !name.empty() &&
        name.find_first_of(L"\\/") == std::wstring_view::npos) {
        const bool separator = !parent.empty() && parent.back() != L'\\';
        const size_t prefix_size = parent.size() + (separator ? 1 : 0);
        for (const auto& folder : folders) {
            if (folder.path.size() != prefix_size + name.size()) continue;
            if (separator && folder.path[parent.size()] != L'\\') continue;
            if ((!parent.empty() && CompareStringOrdinal(parent.data(),
                    static_cast<int>(parent.size()), folder.path.data(),
                    static_cast<int>(parent.size()), TRUE) != CSTR_EQUAL) ||
                CompareStringOrdinal(name.data(), static_cast<int>(name.size()),
                    folder.path.data() + prefix_size, static_cast<int>(name.size()), TRUE) != CSTR_EQUAL)
                continue;
            return &folder;
        }
        return nullptr;
    }
    std::wstring full(original_parent);
    if (!full.empty() && full.back() != L'\\' && full.back() != L'/') full += L'\\';
    full += name;
    return FindKnownFolderLabel(full, folders);
}

inline auto& KnownFolderLabels() {
    using l10n::StringId;
    static std::array<KnownFolderLabel, 6> folders{{
        {{}, StringId::Desktop}, {{}, StringId::Downloads},
        {{}, StringId::KnownFolderPictures}, {{}, StringId::KnownFolderDocuments},
        {{}, StringId::KnownFolderMusic}, {{}, StringId::KnownFolderVideos}}};
    return folders;
}

// Call on a startup worker and join it before creating windows.
inline void InitializeKnownFolderLabels() {
    static std::once_flag initialized;
    std::call_once(initialized, [] {
        const KNOWNFOLDERID* ids[] = {&FOLDERID_Desktop, &FOLDERID_Downloads,
            &FOLDERID_Pictures, &FOLDERID_Documents, &FOLDERID_Music, &FOLDERID_Videos};
        auto& result = KnownFolderLabels();
        for (size_t i = 0; i < result.size(); ++i) {
            PWSTR value = nullptr;
            if (SUCCEEDED(SHGetKnownFolderPath(*ids[i], KF_FLAG_DONT_VERIFY, nullptr, &value)) && value)
                result[i].path = KnownFolderPathKey(value);
            CoTaskMemFree(value);
        }
    });
}

inline std::wstring KnownFolderDisplayName(std::wstring_view path, std::wstring_view fallback) {
    if (const auto* folder = FindKnownFolderLabel(path, KnownFolderLabels())) {
        const auto& label = l10n::Get(folder->label);
        if (!label.empty()) return label;
    }
    return std::wstring(fallback);
}

} // namespace pulse::path
