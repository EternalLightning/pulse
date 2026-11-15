// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "new_item_dialog.h"
#include "batch_rename.h"
#include "../ui/input_dialog.h"
#include "../ui/edit_host.h"
#include "../common/localization.h"

namespace pulse::app {
bool PromptNewItemName(HWND owner, const std::wstring& title, bool dark,
                       COLORREF accent, std::wstring& result, size_t max_length) {
    ui::InputDialogSpec spec;
    spec.window_class = L"PulseNewItemDialog";
    spec.title = title;
    spec.glyph = L"\xE710";
    spec.confirm_text = l10n::Get(l10n::StringId::CreateItemAction);
    const auto prompt = l10n::Get(l10n::StringId::CreateItemNamePrompt);
    spec.fields.push_back({100, prompt, {}, prompt, 100, max_length});
    spec.validate = [](const std::vector<std::wstring>& values) {
        const bool valid = IsValidFileName(values.front());
        return ui::InputDialogValidation{valid, !values.front().empty() && !valid
            ? l10n::Get(l10n::StringId::InvalidName) : std::wstring{}};
    };
    auto input = ui::ShowInputDialog(owner, spec, dark, ui::ChildEditColor(accent));
    if (input.accepted) result = std::move(input.values.front());
    return input.accepted;
}
} // namespace pulse::app
