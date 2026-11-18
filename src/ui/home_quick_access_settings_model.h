// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "../common/localization.h"
#include "../model/home_quick_access.h"
#include <string>

namespace pulse::ui {
enum HomeQuickAccessSettingsAction {
    HomeAccessToggle, HomeAccessAddFolder, HomeAccessRemove,
    HomeAccessMoveUp, HomeAccessMoveDown, HomeAccessReset
};
struct HomeQuickAccessSettingsRow { std::wstring title, path; };

inline std::wstring HomeQuickAccessItemLabel(int bit) {
    using I = l10n::StringId;
    constexpr I labels[] = {I::Desktop, I::KnownFolderPictures, I::KnownFolderVideos, I::Downloads,
        I::RecycleBin, I::ShellLibraries, I::ShellControlPanel, I::ShellApplications,
        I::KnownFolderDocuments, I::KnownFolderMusic};
    return bit >= 0 && bit < static_cast<int>(std::size(labels)) ? l10n::Get(labels[bit]) : std::wstring();
}
} // namespace pulse::ui
