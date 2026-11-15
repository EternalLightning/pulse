// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once

#include <cwctype>
#include <string>

namespace pulse::model {

enum class ViewMode : unsigned char {
    ExtraLargeIcons,
    LargeIcons,
    MediumIcons,
    SmallIcons,
    List,
    Details,
    Tiles,
    Content,
};

inline const wchar_t* ViewModeName(ViewMode mode) noexcept {
    switch (mode) {
    case ViewMode::ExtraLargeIcons: return L"extra-large-icons";
    case ViewMode::LargeIcons: return L"large-icons";
    case ViewMode::MediumIcons: return L"medium-icons";
    case ViewMode::SmallIcons: return L"small-icons";
    case ViewMode::List: return L"list";
    case ViewMode::Details: return L"details";
    case ViewMode::Tiles: return L"tiles";
    case ViewMode::Content: return L"content";
    }
    return L"details";
}

inline int ViewModeIndex(ViewMode mode) noexcept {
    const int value = static_cast<int>(mode);
    return value >= 0 && value < 8 ? value : 5;
}

inline ViewMode ViewModeFromIndex(int index) noexcept {
    return index >= 0 && index < 8 ? static_cast<ViewMode>(index) : ViewMode::Details;
}

inline ViewMode ParseViewMode(const std::wstring& value) noexcept {
    std::wstring lower = value;
    for (auto& c : lower) c = static_cast<wchar_t>(std::towlower(c));
    for (int i = 0; i < 8; ++i) {
        const ViewMode mode = ViewModeFromIndex(i);
        if (lower == ViewModeName(mode)) return mode;
    }
    return ViewMode::Details;
}

} // namespace pulse::model
