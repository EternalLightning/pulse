#include "app_exit.h"
#include "app_internal.h"
#include "exit_confirmation.h"
#include "../common/localization.h"
#include "../ui/file_operation_dialog.h"
#include <thread>

namespace pulse {
namespace {
void BeginWindowShutdown(AppState& s) {
    if (s.exit_requested || !s.hwnd) return;
    s.exit_requested = true;
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

bool ConfirmWindowShutdown(AppState& s, bool pending) {
    s.exit_confirming = true;
    const bool accepted = !pending || ui::ShowConfirmDialog(s.hwnd,
        app::ExitConfirmationSpec(), s.darkMode, s.accentColor);
    s.exit_confirming = false;
    return accepted;
}
}

void RequestWindowClose(AppState& s) {
    if (s.exit_requested || s.exit_confirming || !s.hwnd) return;
    if (!s.core.exiting && !ConfirmWindowShutdown(s, s.ops.HasPendingFileOperations())) return;
    BeginWindowShutdown(s);
}

void RequestApplicationExit(AppState& s) {
    if (s.exit_requested || s.exit_confirming || !s.hwnd) return;
    bool pending = s.ops.HasPendingFileOperations();
    s.core.windows.ForEach([&](HWND, AppState& window) {
        pending = pending || window.ops.HasPendingFileOperations();
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
    if (s.exit_thread.joinable()) s.exit_thread.join();
    // Apply final metadata/clipboard completions before the teardown snapshot,
    // even when this check ran on a timer ahead of a queued operation notification.
    SendMessageW(s.hwnd, WM_OPS_NOTIFY, 0, 0);
    DestroyWindow(s.hwnd);
    return true;
}
} // namespace pulse
