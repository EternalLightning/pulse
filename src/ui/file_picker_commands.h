// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "fluent_menu.h"
#include "../common/localization.h"

#include <array>

namespace pulse::ui {

enum class PickerCommand {
    None, New, Cut, Copy, Paste, Rename, Recycle, Refresh,
    Open, NewFolder, NewFile, NewText, CopyPath, Properties, Undo
};

inline constexpr std::array kPickerToolbar{
    PickerCommand::New, PickerCommand::Cut, PickerCommand::Copy, PickerCommand::Paste,
    PickerCommand::Rename, PickerCommand::Recycle
};

struct PickerCommandInfo {
    l10n::StringId label;
    const wchar_t* glyph;
    const wchar_t* shortcut = L"";
    bool ellipsis = false;
};

struct PickerCommandState {
    bool ready = false;
    bool in_folder = false;
    size_t selected = 0;
    bool has_clipboard = false;
    bool can_undo = false;
};

PickerCommandInfo PickerCommandPresentation(PickerCommand command);
bool PickerCommandEnabled(PickerCommand command, const PickerCommandState& state);
std::vector<FluentMenuItem> PickerNewMenu();
std::vector<FluentMenuItem> PickerContextMenu(const PickerCommandState& state);
std::wstring PromptPickerName(HWND owner, PickerCommand command, bool dark, D2D1_COLOR_F accent);

} // namespace pulse::ui
