// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once
#include "../ops/clipboard.h"

namespace pulse::app {

enum class ClipboardPasteSource { None, Paths, Shell, Tray };

inline ClipboardPasteSource ChooseClipboardPasteSource(bool paths_read,
    const ops::ClipboardData& clipboard, bool shell_format, bool tray_present,
    uint32_t observed_sequence, uint32_t current_sequence) {
    if (observed_sequence != current_sequence) return ClipboardPasteSource::None;
    const bool has_paths = paths_read && !clipboard.paths.empty();
    if (has_paths && clipboard.sequence != observed_sequence) return ClipboardPasteSource::None;
    // A failed/empty HDROP must not mask virtual contents or release an old tray.
    if (!has_paths && shell_format) return ClipboardPasteSource::Shell;
    if (tray_present) return ClipboardPasteSource::Tray;
    return has_paths ? ClipboardPasteSource::Paths : ClipboardPasteSource::None;
}

} // namespace pulse::app
