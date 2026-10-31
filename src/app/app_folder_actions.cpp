// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "app_folder_actions.h"
#include "app_internal.h"
#include "new_item_dialog.h"
#include <algorithm>

namespace pulse {
void GroupSelectionIntoFolder(AppState& s) {
    auto* tab = ActiveTab(s);
    if (!CanGroupSelection(tab)) return;
    const auto paths = SelectedFullPaths(*tab);
    const auto parent = tab->current_path;
    const auto tab_id = tab->network_identity;
    std::wstring name;
    const auto channel = [](float value) { return static_cast<int>(std::clamp(value, 0.0f, 1.0f) * 255.0f); };
    if (!app::PromptNewItemName(s.hwnd, l10n::Get(l10n::StringId::NewFolderWithSelection), s.darkMode,
        RGB(channel(s.accentColor.r), channel(s.accentColor.g), channel(s.accentColor.b)), name)) return;
    tab = ActiveTab(s);
    if (!tab || tab->network_identity != tab_id || tab->current_path != parent || !CanGroupSelection(tab) ||
        SelectedFullPaths(*tab) != paths) return;
    ops::OpRequest request;
    request.type = ops::OpType::Move;
    request.folder_action = ops::FolderAction::GroupSelection;
    request.sources = paths; request.dest_dir = parent; request.new_name = name;
    request.undo_label = l10n::Get(l10n::StringId::NewFolderWithSelection);
    request.collision_policy = ops::CollisionPolicy::KeepBoth;
    if (s.ops.Submit(std::move(request))) {
        tab->pending_selected_name = name;
        tab->pending_selected_names = {name};
        tab->pending_ensure_selection_visible = true;
    }
}

void DissolveSelectedFolders(AppState& s) {
    auto* tab = ActiveTab(s);
    if (!CanDissolveSelection(tab)) return;
    ops::OpRequest request;
    request.type = ops::OpType::Move;
    request.folder_action = ops::FolderAction::DissolveFolders;
    request.sources = SelectedFullPaths(*tab); request.dest_dir = tab->current_path;
    request.undo_label = l10n::Get(l10n::StringId::UngroupFolder);
    request.collision_policy = ops::CollisionPolicy::KeepBoth;
    s.ops.Submit(std::move(request));
}

} // namespace pulse
