// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "app_drop_shortcuts.h"
#include "app_internal.h"
#include "../common/localization.h"
#include "../ops/clipboard.h"

namespace pulse {
namespace {
void ShowShortcutDropError(AppState& state, const app::ShortcutDropResult& result) {
    wchar_t error[128]{};
    swprintf_s(error, l10n::Get(l10n::StringId::ErrorCodeFormat).c_str(),
        static_cast<DWORD>(result.error));
    state.notification_toast.ShowError(state.hwnd, l10n::Get(l10n::StringId::CannotCreateShortcut),
        result.failed_source + L"\n" + error);
}
bool QueueShortcuts(AppState& state, const std::vector<std::wstring>& sources,
    const std::wstring& destination, uint32_t clipboard_sequence = 0) {
    if (!state.shortcut_drop_mailbox) return false;
    const auto mailbox = state.shortcut_drop_mailbox;
    const auto format = l10n::Get(l10n::StringId::ShortcutNameFormat);
    bool accepted = false;
    try {
        accepted = state.worker.EnqueueIo([mailbox, sources, destination, format, clipboard_sequence] {
            ops::ClipboardData clipboard;
            if (clipboard_sequence &&
                (ops::ReadClipboardForPaste(clipboard_sequence, clipboard) != S_OK || clipboard.cut))
                return;
            mailbox->Publish(app::CreateDropShortcuts(
                clipboard_sequence ? clipboard.paths : sources, destination, format));
        });
    } catch (...) {}
    if (!accepted) {
        app::ShortcutDropResult failure;
        failure.failed_source = destination;
        failure.error = HRESULT_FROM_WIN32(ERROR_NOT_ENOUGH_MEMORY);
        ShowShortcutDropError(state, failure);
    }
    return accepted;
}
} // namespace

bool QueueDropShortcuts(AppState& state, const std::vector<std::wstring>& sources,
    const std::wstring& destination) {
    return app::CanCreateDropShortcuts(sources, destination) && QueueShortcuts(state, sources, destination);
}

bool QueuePasteShortcuts(AppState& state, const std::wstring& destination, uint32_t sequence) {
    return sequence && app::IsShortcutFileSystemPath(destination) &&
        QueueShortcuts(state, {}, destination, sequence);
}

void HandleDropShortcutsResult(AppState& state, WPARAM identity) {
    if (!state.shortcut_drop_mailbox) return;
    for (const auto& result : state.shortcut_drop_mailbox->Take(identity)) {
        if (result.failed) ShowShortcutDropError(state, result);
        if (result.created) RefreshPath(state, result.destination);
    }
}
} // namespace pulse
