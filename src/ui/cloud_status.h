// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once
#include "../fs/cloud_state.h"
#include "../common/localization.h"
#include <algorithm>

namespace pulse::ui {
inline float CloudStatusColumnWidth(float available, float scale) {
    return scale > 0.0f && available >= 200.0f * scale ? 64.0f * scale : 0.0f;
}

inline l10n::StringId CloudStatusLabel(DWORD attrs, fs::CloudSync sync) {
    if (sync == fs::CloudSync::Error) return l10n::StringId::CloudSyncError;
    if (sync == fs::CloudSync::Syncing) return l10n::StringId::CloudSyncing;
    // A pinned file can still be awaiting its download. Recall attributes
    // describe whether preview is safe; pinning describes the provider's intent.
    if (fs::IsCloudPlaceholder(attrs)) return l10n::StringId::CloudOnlineOnly;
    return fs::CloudStateFromAttrs(attrs) == fs::CloudState::Pinned
        ? l10n::StringId::CloudKept : l10n::StringId::CloudAvailableLocal;
}
} // namespace pulse::ui
