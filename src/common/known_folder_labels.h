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
    const auto key = KnownFolderPathKey(path);
    if (key.empty()) return nullptr;
    for (const auto& folder : folders) {
        if (!folder.path.empty() && CompareStringOrdinal(key.c_str(), -1,
                folder.path.c_str(), -1, TRUE) == CSTR_EQUAL) return &folder;
    }
    return nullptr;
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
