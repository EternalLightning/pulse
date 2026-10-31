// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "app_exit.h"
#include "app_internal.h"
#include "exit_confirmation.h"
#include "../common/localization.h"
#include "../ui/file_operation_dialog.h"
#include <thread>

namespace pulse {
namespace {
bool HasPendingWindowOperations(const AppState& s) {
    return s.ops.HasPendingFileOperations() || std::any_of(s.archive_operations.begin(),
        s.archive_operations.end(), [](const AppState::ArchiveOperation& operation) {
            return !operation.done->load(std::memory_order_acquire);
        });
}

bool HideAppWindow(AppState& s, bool require_tray) {
    if (require_tray) {
        AppState* owner = s.core.windows.Lookup(s.core.global_ui_owner);
        if (!owner || (!owner->tray_controller.SetVisible(true) &&
                FindWindowW(L"Shell_TrayWnd", nullptr))) return false;
    }
    SuspendContentSearches(s);
    s.globalSearchWindow.Hide();
    s.quickPreview.Close();
    if (s.operationWindow) s.operationWindow->Hide();
    ShowWindow(s.hwnd, SW_HIDE);
    s.core.windows.Touch(s.hwnd);
    return true;
}

void BeginWindowShutdown(AppState& s) {
    if (s.exit_requested || !s.hwnd) return;
    s.exit_requested = true;
    for (const auto& operation : s.archive_operations)
        operation.cancel->store(true, std::memory_order_release);
    SuspendContentSearches(s);
    s.globalSearchWindow.Hide();
    s.quickPreview.Close();
    s.notification_toast.Show(s.hwnd, l10n::Get(l10n::StringId::ExitExiting),
        l10n::Get(l10n::StringId::ExitStopping), true);
    const HWND hwnd = s.hwnd;
    s.exit_stop_finished = false;
    auto launch = std::make_shared<std::atomic<bool>>(false);
    try {
        s.exit_thread = std::thread([&s, hwnd, launch] {
            launch->wait(false, std::memory_order_acquire);
            s.settings.Stop();
            s.systemIntegration.Stop();
            s.ops.Stop();
            s.exit_stop_finished.store(true, std::memory_order_release);
            PostMessageW(hwnd, WM_EXIT_READY, 0, 0);
        });
        s.ops.BeginShutdown();
        launch->store(true, std::memory_order_release);
        launch->notify_one();
    } catch (...) {
        // No unsafe process termination: keep pumping the UI so this can be retried.
        s.exit_requested = false;
        s.notification_toast.ShowError(s.hwnd, l10n::Get(l10n::StringId::ExitFailed),
            l10n::Get(l10n::StringId::ExitWorkerFailed));
    }
}

bool ConfirmWindowShutdown(AppState& s, bool pending, bool background = false) {
    s.exit_confirming = true;
    const bool accepted = !pending || ui::ShowConfirmDialog(s.hwnd,
        background ? app::BackgroundCloseConfirmationSpec() : app::ExitConfirmationSpec(), s.darkMode, s.accentColor);
    s.exit_confirming = false;
    return accepted;
}
}

void RequestAppWindowClose(AppState& s) {
    if (s.exit_requested || s.exit_confirming || !s.hwnd) return;
    CancelNetworkAuthentication(s);
    if (s.core.exiting) { RequestWindowClose(s); return; }
    if (s.appPrefs.keep_running_on_close || s.appPrefs.global_search_enabled) {
        if (!s.appPrefs.keep_running_on_close &&
            !ConfirmWindowShutdown(s, HasPendingWindowOperations(s), true)) return;
        if (HideAppWindow(s, true)) ResetLastTabToHome(s);
        return;
    }
    bool another_visible_window = false;
    s.core.windows.ForEach([&](HWND hwnd, AppState& window) {
        if (hwnd != s.hwnd && !window.exit_requested && IsWindowVisible(hwnd))
            another_visible_window = true;
    });
    if (!another_visible_window) { RequestApplicationExit(s); return; }
    if (HasPendingWindowOperations(s)) {
        if (ConfirmWindowShutdown(s, true, true)) HideAppWindow(s, false);
    }
    else RequestWindowClose(s);
}

void RequestWindowClose(AppState& s) {
    if (s.exit_requested || s.exit_confirming || !s.hwnd) return;
    if (!s.core.exiting && !ConfirmWindowShutdown(s, HasPendingWindowOperations(s))) return;
    BeginWindowShutdown(s);
}

void RequestApplicationExit(AppState& s) {
    if (s.exit_requested || s.exit_confirming || !s.hwnd) return;
    bool pending = HasPendingWindowOperations(s);
    s.core.windows.ForEach([&](HWND, AppState& window) {
        pending = pending || HasPendingWindowOperations(window);
    });
    if (!ConfirmWindowShutdown(s, pending)) return;
    s.core.exiting = true;
    // Each window drains its own operation callbacks while the shared UI
    // thread keeps pumping. No window bypasses its asynchronous shutdown.
    s.core.windows.ForEach([](HWND, AppState& window) { BeginWindowShutdown(window); });
    BeginWindowShutdown(s);
}

bool CompleteApplicationExit(AppState& s) {
    if (!s.exit_requested || !s.exit_stop_finished.load(std::memory_order_acquire)) return false;
    // Extraction owns a nested message loop. Let it observe cancellation and
    // unwind before destroying its AppState or joining the general I/O worker.
    if (!s.archive_operations.empty()) return false;
    if (s.exit_thread.joinable()) s.exit_thread.join();
    // Apply final metadata/clipboard completions before the teardown snapshot,
    // even when this check ran on a timer ahead of a queued operation notification.
    SendMessageW(s.hwnd, WM_OPS_NOTIFY, 0, 0);
    DestroyWindow(s.hwnd);
    return true;
}
} // namespace pulse
