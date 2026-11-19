// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "new_item_dialog.h"
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
    auto name = ui::PromptFileName(owner, std::move(spec), {}, max_length, dark, ui::ChildEditColor(accent));
    if (name.empty()) return false;
    result = std::move(name);
    return true;
}
} // namespace pulse::app
