// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "archive_dialog.h"
#include "../ui/input_dialog.h"
#include "../ui/edit_host.h"
#include "../common/localization.h"
#include "../common/path_utils.h"
#include <filesystem>

namespace pulse::app {
bool ShowArchiveExtractDialog(HWND owner, bool dark, COLORREF accent,
                              const std::wstring& archive_path, bool all,
                              ArchiveExtractOptions& options,
                              const std::function<bool(HWND, std::wstring&)>& pick_folder) {
    const std::filesystem::path archive(archive_path);
    const auto destination = options.destination.empty()
        ? path::StripExtendedPathPrefix(archive.parent_path().wstring()) : options.destination;
    ui::InputDialogSpec spec;
    spec.window_class = L"PulseArchiveDialog";
    spec.title = l10n::Get(l10n::StringId::ArchiveExtractTitle);
    spec.glyph = L"\xE896";
    spec.confirm_text = l10n::Get(l10n::StringId::ArchiveExtract);
    spec.width = 520;
    spec.height = all ? 348.0f : 306.0f;
    spec.close_id = 104;
    spec.fields.push_back({100, l10n::Get(l10n::StringId::ArchiveDestination), destination, {}, 100, 32767});
    spec.fields.push_back({101, l10n::Get(l10n::StringId::ArchiveOptionalPassword), options.password, {}, 192, 4096, true});
    spec.browse_text = l10n::Get(l10n::StringId::DupBrowse);
    spec.browse = [pick_folder](HWND dialog, std::wstring& value) {
        return pick_folder && pick_folder(dialog, value);
    };
    if (all) {
        spec.checkbox_text = l10n::Get(l10n::StringId::ArchiveCreateFolderPrefix) + archive.stem().wstring();
        spec.checked = options.create_folder;
    }
    spec.validate = [](const std::vector<std::wstring>& values) {
        return ui::InputDialogValidation{!values.front().empty(), {}};
    };
    auto input = ui::ShowInputDialog(owner, spec, dark, ui::ChildEditColor(accent));
    if (!input.accepted) return false;
    options.destination = std::move(input.values[0]);
    options.password = std::move(input.values[1]);
    options.create_folder = all && input.checked;
    return true;
}

bool ShowArchivePasswordDialog(HWND owner, bool dark, COLORREF accent, std::wstring& password) {
    ui::InputDialogSpec spec;
    spec.window_class = L"PulseArchiveDialog";
    spec.title = l10n::Get(l10n::StringId::ArchivePassword);
    spec.glyph = L"\xE72E";
    spec.width = 420;
    spec.close_id = 104;
    spec.fields.push_back({101, l10n::Get(l10n::StringId::ArchiveEnterPassword), password, {}, 100, 4096, true});
    auto input = ui::ShowInputDialog(owner, spec, dark, ui::ChildEditColor(accent));
    if (input.accepted) password = std::move(input.values.front());
    return input.accepted;
}
} // namespace pulse::app
