#include "app_drop_shortcuts.h"
#include "app_internal.h"
#include "../common/localization.h"

namespace pulse {
namespace {
void ShowShortcutDropError(AppState& state, const app::ShortcutDropResult& result) {
    wchar_t error[128]{};
    swprintf_s(error, l10n::Get(l10n::StringId::ErrorCodeFormat).c_str(),
        static_cast<DWORD>(result.error));
    state.notification_toast.ShowError(state.hwnd, l10n::Get(l10n::StringId::CannotCreateShortcut),
        result.failed_source + L"\n" + error);
}
} // namespace

bool QueueDropShortcuts(AppState& state, const std::vector<std::wstring>& sources,
    const std::wstring& destination) {
    if (!app::CanCreateDropShortcuts(sources, destination) || !state.shortcut_drop_mailbox) return false;
    const auto mailbox = state.shortcut_drop_mailbox;
    const auto format = l10n::Get(l10n::StringId::ShortcutNameFormat);
    bool accepted = false;
    try {
        accepted = state.worker.EnqueueIo([mailbox, sources, destination, format] {
            mailbox->Publish(app::CreateDropShortcuts(sources, destination, format));
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

void HandleDropShortcutsResult(AppState& state, WPARAM identity) {
    if (!state.shortcut_drop_mailbox) return;
    for (const auto& result : state.shortcut_drop_mailbox->Take(identity)) {
        if (result.failed) ShowShortcutDropError(state, result);
        if (result.created) RefreshPath(state, result.destination);
    }
}
} // namespace pulse
