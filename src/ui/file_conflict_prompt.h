// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "batch_conflict_dialog.h"
#include "file_operation_dialog.h"

namespace pulse::ui {

inline void ResolveFileConflict(HWND owner, ops::OpsManager& manager, const ops::ConflictItemInfo& conflict,
                                bool dark, D2D1_COLOR_F accent) {
    if (conflict.rows.size() > 1 && !conflict.link_confirmation) {
        BatchConflictLabels labels;
        labels.source = l10n::Get(l10n::StringId::OpSource);
        labels.replace = l10n::Get(l10n::StringId::OpReplaceFile);
        if (const auto result = ShowBatchFileConflictDialog(owner, conflict, labels, dark, accent))
            manager.ResolveConflicts(conflict.token, *result);
        else
            manager.ResolveConflict(conflict.token, ops::ConflictChoice::Cancel, false);
    } else {
        const auto result = ShowFileConflictDialog(owner, conflict, dark, accent);
        manager.ResolveConflict(conflict.token, result.choice, result.apply_to_all);
    }
}

} // namespace pulse::ui
