#include "app_exit.h"
#include "app_internal.h"
#include "exit_confirmation.h"
#include "../common/localization.h"
#include "../ui/file_operation_dialog.h"
#include <thread>

namespace pulse {
namespace {
const wchar_t* ExitText(const wchar_t* zh, const wchar_t* en) {
    return l10n::effective_language() == l10n::Language::EnUS ? en : zh;
}
}

void RequestApplicationExit(AppState& s) {
    if (s.exit_requested || s.exit_confirming || !s.hwnd) return;
    s.exit_confirming = true;
    if (s.ops.HasPendingFileOperations()) {
        const auto spec = app::ExitConfirmationSpec();
        if (!ui::ShowConfirmDialog(s.hwnd, spec, s.darkMode, s.accentColor)) {
            s.exit_confirming = false;
            return;
        }
    }
    s.exit_confirming = false;
    s.exit_requested = true;
    SuspendContentSearches(s);
    s.globalSearchWindow.Hide();
    s.quickPreview.Close();
    s.notification_toast.Show(s.hwnd, ExitText(L"正在退出", L"Exiting"),
        ExitText(L"正在安全停止任务…", L"Stopping tasks safely…"), true);
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
        s.notification_toast.ShowError(s.hwnd, ExitText(L"无法退出", L"Could not exit"),
            ExitText(L"无法启动停止任务的后台线程。请稍后重试。",
                L"Could not start the shutdown worker. Please retry."));
    }
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
