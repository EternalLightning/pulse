// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once

#include "context_menu_prefs.h"
#include "system_ctx_model.h"
#include "../common/localization.h"

#include <utility>

namespace pulse {
using SystemContextPrefsSave = bool (*)(const app::ContextMenuPrefs& prefs);

// Save a staged copy before consuming the Pulse queue or starting registry writes.
// The optional saver lets isolated tests exercise a persistence failure.
inline bool ApplySystemContextChanges(app::SystemCtxModel& model, app::ContextMenuPrefs& prefs,
                               SystemContextPrefsSave save = nullptr) {
    const auto pulse = model.PendingPulseChanges();
    if (!pulse.empty()) {
        auto staged = prefs;
        for (const auto& [key, hidden] : pulse.hidden) staged.SetItemEnabled(key, !hidden);
        for (const auto& clsid : pulse.skipped) staged.SetComDisabled(ipc::HandlerCatalogKey(clsid), true);
        if (!(save ? save(staged) : staged.Save())) {
            model.ReportError(l10n::Pick(L"Pulse 菜单设置保存失败，待应用更改已保留；未开始写入注册表。",
                                         L"Could not save Pulse menu settings. Pending changes are kept; registry changes have not started."));
            return false;
        }
        prefs = std::move(staged);
        model.CommitPulseChanges();
    }
    model.ApplyPending();
    return true;
}

} // namespace pulse
