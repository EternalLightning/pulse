#include "archive_operation_ui.h"
#include "../common/localization.h"
#include "../ui/file_operation_dialog.h"
#include "../ui/dialog_lifecycle.h"

namespace pulse::app {

ops::OpStatus ArchiveOperationStatus(const ops::ArchiveProgress& progress,
    const std::wstring& archive, const std::wstring& destination, bool cancelling) {
    ops::OpStatus status;
    status.active = progress.phase != ops::ArchivePhase::Completed;
    status.phase = cancelling ? ops::OpPhase::Cancelling
        : progress.phase == ops::ArchivePhase::Preparing ? ops::OpPhase::Scanning
        : progress.phase == ops::ArchivePhase::Completed ? ops::OpPhase::Completed : ops::OpPhase::Running;
    status.summary = l10n::Get(cancelling ? l10n::StringId::ArchiveCancelling
        : progress.phase == ops::ArchivePhase::Preparing ? l10n::StringId::OpPreparingList
        : progress.phase == ops::ArchivePhase::Completed ? l10n::StringId::OpCompleted
        : l10n::StringId::ArchiveExtracting);
    status.percent = progress.phase == ops::ArchivePhase::Preparing ? -1.0f : static_cast<float>(progress.percent);
    status.source_label = archive;
    status.destination_label = destination;
    status.total_bytes = progress.total_bytes;
    status.transferred_bytes = progress.processed_bytes;
    status.total_items = progress.total_items;
    status.completed_items = progress.completed_items;
    status.bytes_per_second = cancelling || !status.active ? 0.0 : progress.bytes_per_second;
    status.eta_seconds = cancelling || !status.active ? 0 : progress.eta_seconds;
    return status;
}

ops::OpStatus ArchiveFailureStatus(const ops::ArchiveOperationResult& result,
    const std::wstring& archive, const std::wstring& destination) {
    ops::OpStatus status;
    status.phase = ops::OpPhase::Failed;
    status.summary = l10n::Get(l10n::StringId::ArchiveExtractFailed);
    status.source_label = archive;
    status.destination_label = destination;
    status.last_error = result.error.empty() ? status.summary : result.error;
    return status;
}

bool ShowArchiveProgressWindow(HWND owner, bool dark, COLORREF accent,
    std::atomic_bool& done, std::atomic_bool& cancel,
    const std::function<ops::OpStatus()>& read_status) {
    ui::FileOperationWindow window;
    ui::FileOperationCallbacks callbacks;
    callbacks.cancel = [&] { cancel.store(true, std::memory_order_release); };
    const bool owner_enabled = IsWindow(owner) && IsWindowEnabled(owner);
    const bool created = window.Create(owner, std::move(callbacks),
        {.use_summary_title = true, .estimated_bytes = true, .item_progress_known = false,
            .modal_owner = owner_enabled});
    if (!created) cancel.store(true, std::memory_order_release);
    window.SetTheme(dark, D2D1::ColorF(GetRValue(accent) / 255.0f,
        GetGValue(accent) / 255.0f, GetBValue(accent) / 255.0f));
    window.Update(read_status());
    ui::OwnerDimScope owner_dim(created ? owner : nullptr);
    if (created && owner_enabled) EnableWindow(owner, FALSE);
    if (created) window.Show();
    bool quitting = false;
    int quit_code = 0;
    auto pump_messages = [&] {
        MsgWaitForMultipleObjectsEx(0, nullptr, 100, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) {
                quitting = true;
                quit_code = static_cast<int>(message.wParam);
                cancel.store(true, std::memory_order_release);
            } else {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }
    };
    while (!done.load(std::memory_order_acquire)) {
        if (created) window.Update(read_status());
        if (!IsWindow(owner)) cancel.store(true, std::memory_order_release);
        pump_messages();
    }
    if (created && !quitting && !cancel.load(std::memory_order_acquire) && IsWindow(owner)) {
        const auto final_status = read_status();
        if (final_status.phase == ops::OpPhase::Failed) {
            window.Update(final_status);
            while (window.IsVisible() && IsWindow(owner) && !quitting)
                pump_messages();
        }
    }
    window.Destroy();
    if (created && owner_enabled && IsWindow(owner)) EnableWindow(owner, TRUE);
    if (quitting) PostQuitMessage(quit_code);
    return created && !cancel.load(std::memory_order_acquire);
}

} // namespace pulse::app
