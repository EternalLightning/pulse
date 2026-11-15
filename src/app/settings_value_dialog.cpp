// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "settings_value_dialog.h"
#include "../ui/input_dialog.h"
#include "../common/localization.h"

namespace pulse::app {
bool PromptSettingsValue(HWND owner, const std::wstring& title,
                         const std::wstring& hint, const std::wstring& initial,
                         std::wstring& result, size_t max_length,
                         const std::wstring& confirm_label, bool dark, D2D1_COLOR_F accent) {
    ui::InputDialogSpec spec;
    spec.window_class = L"PulseSettingsValueDialog";
    spec.title = title;
    spec.glyph = L"\xE713";
    spec.width = 360;
    spec.height = 212;
    spec.confirm_text = confirm_label.empty() ? l10n::Get(l10n::StringId::Apply) : confirm_label;
    spec.select_all = true;
    spec.fields.push_back({100, hint, initial, {}, 96, max_length});
    auto input = ui::ShowInputDialog(owner, spec, dark, accent);
    if (input.accepted) result = std::move(input.values.front());
    return input.accepted;
}
} // namespace pulse::app
