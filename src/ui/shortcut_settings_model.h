// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <string>

namespace pulse::ui {

enum ShortcutSettingsAction { ShortcutEdit, ShortcutRemove, ShortcutReset, ShortcutResetAll,
                              ShortcutFilter, ShortcutClearFilter };

struct ShortcutSettingsRow {
    int command = 0;
    std::wstring title;
    std::wstring binding;
    std::wstring group;
    bool customized = false;
};

} // namespace pulse::ui
