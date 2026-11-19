// SPDX-License-Identifier: Apache-2.0
#include "picker_file_operations.h"

#include "clipboard.h"
#include "../fs/fs_enum.h"

#include <algorithm>
#include <iterator>
#include <utility>

namespace pulse::ops {
namespace {

HRESULT RunRequest(PickerFileOperations& session, PickerFileOperation& operation,
                   OpRequest request, HWND owner, bool verify_copies, const ClipboardData* clipboard = nullptr) {
    if (operation.cancelled.load()) return HRESULT_FROM_WIN32(ERROR_CANCELLED);
    for (auto& path : request.sources) path = fs::NormalizePath(std::move(path));
    auto& manager = session.Start(owner, verify_copies);
    const auto path_order = [](const std::wstring& a, const std::wstring& b) {
        return _wcsicmp(a.c_str(), b.c_str()) < 0;
    };
    std::vector<std::wstring> cut_sources;
    const bool cut = clipboard && clipboard->cut;
    if (cut) {
        cut_sources = request.sources;
        std::sort(cut_sources.begin(), cut_sources.end(), path_order);
    }
    if ((clipboard && clipboard->sequence != GetClipboardSequenceNumber()) ||
        !operation.BeginMutation() || operation.cancelled.load())
        return HRESULT_FROM_WIN32(ERROR_CANCELLED);
    const auto task = manager.Submit(std::move(request));
    if (!task) return E_FAIL;

    bool cancel_sent = false;
    bool permanent_declined = false;
    for (;;) {
        const auto status = manager.Status();
        if (status.task_id == task && !status.active) {
            auto completed = manager.DrainCompletions();
            manager.DrainTransferOutcomes();
            manager.DrainDeleteOutcomes();
            if (cut && status.phase == OpPhase::Completed) {
                std::vector<std::wstring> moved;
                for (auto& entry : completed) if (entry.type == OpType::Move)
                    std::move(entry.sources.begin(), entry.sources.end(), std::back_inserter(moved));
                std::sort(moved.begin(), moved.end(), path_order);
                if (std::includes(moved.begin(), moved.end(), cut_sources.begin(), cut_sources.end(), path_order))
                    CompleteCutClipboard(clipboard->sequence, clipboard->paths, owner);
            }
            if (permanent_declined) return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
            if (status.phase == OpPhase::Completed) return S_OK;
            if (status.phase == OpPhase::Cancelled) return HRESULT_FROM_WIN32(ERROR_CANCELLED);
            return FAILED(status.failure_hr) ? status.failure_hr : E_FAIL;
        }
        if (status.task_id == task && status.active && operation.cancelled.load() && !cancel_sent) {
            cancel_sent = true;
            manager.CancelCurrent();
        }
        if (const auto pending = manager.PendingDeleteConfirmation()) {
            // Only recycle requests are offered here. Never approve a permanent fallback.
            permanent_declined = true;
            manager.ResolveDeleteConfirmation(pending->token, false);
        }
        session.WaitForChange();
    }
}

HRESULT Paste(PickerFileOperations& session, PickerFileOperation& operation,
              PickerClipboardPaste paste, HWND owner, bool verify_copies) {
    if (operation.cancelled.load() || !paste.sequence) return HRESULT_FROM_WIN32(ERROR_CANCELLED);
    ClipboardData clipboard;
    const HRESULT captured = ReadClipboardForPaste(paste.sequence, clipboard);
    if (operation.cancelled.load()) return HRESULT_FROM_WIN32(ERROR_CANCELLED);
    if (captured == S_FALSE) {
        return PasteShellClipboard(paste.directory, paste.sequence, operation.cancelled, owner,
            [&] { return operation.BeginMutation(); });
    }
    if (FAILED(captured)) return captured;
    OpRequest request;
    request.type = clipboard.cut ? OpType::Move : OpType::Copy;
    request.dest_dir = fs::NormalizePath(std::move(paste.directory));
    request.sources = clipboard.cut ? clipboard.paths : std::move(clipboard.paths);
    return RunRequest(session, operation, std::move(request), owner, verify_copies, &clipboard);
}

struct Work {
    std::shared_ptr<PickerFileOperations> session;
    std::shared_ptr<PickerFileOperation> operation;
    PickerFileRequest request;
    HWND owner;
    bool verify_copies;
};

void CALLBACK Run(PTP_CALLBACK_INSTANCE, void* argument) {
    const std::unique_ptr<Work> work(static_cast<Work*>(argument));
    auto& session = *work->session;
    auto& operation = *work->operation;
    try {
        if (auto* paste = std::get_if<PickerClipboardPaste>(&work->request)) {
            operation.result = Paste(session, operation, std::move(*paste), work->owner, work->verify_copies);
        } else if (auto* properties = std::get_if<PickerProperties>(&work->request)) {
            auto& manager = session.Start(work->owner, work->verify_copies);
            if (operation.BeginMutation()) {
                manager.ShowProperties(properties->paths);
                operation.result = S_OK;
            }
        } else {
            operation.result = RunRequest(session, operation, std::move(std::get<OpRequest>(work->request)),
                work->owner, work->verify_copies);
        }
    } catch (const std::exception&) {
        operation.result = E_FAIL;
    }
    operation.ready.store(true, std::memory_order_release);
}

} // namespace

OpsManager& PickerFileOperations::Start(HWND owner, bool verify_copies) {
    if (!started_) {
        manager_.SetUiWindow(owner);
        manager_.SetVerifyCopies(verify_copies);
        started_ = true;
        manager_.Start([this] { Notify(); });
    }
    return manager_;
}

void PickerFileOperations::Notify() {
    {
        std::lock_guard lock(mutex_);
        changed_ = true;
    }
    event_.notify_one();
}

void PickerFileOperations::WaitForChange() {
    std::unique_lock lock(mutex_);
    event_.wait(lock, [&] { return std::exchange(changed_, false); });
}

bool PickerFileOperation::BeginMutation() {
    auto preparing = Phase::Preparing;
    return phase_.compare_exchange_strong(preparing, Phase::Running);
}

bool PickerFileOperation::Cancel(PickerFileOperations& session) {
    cancelled.store(true);
    auto preparing = Phase::Preparing;
    const bool detached = phase_.compare_exchange_strong(preparing, Phase::Cancelled);
    session.Notify();
    return detached;
}

std::shared_ptr<PickerFileOperations> CreatePickerFileOperations() {
    return std::shared_ptr<PickerFileOperations>(new PickerFileOperations, [](PickerFileOperations* session) {
        // The last owner may be the dialog thread. Stop the broker off that thread.
        if (!TrySubmitThreadpoolCallback([](PTP_CALLBACK_INSTANCE, void* argument) {
                delete static_cast<PickerFileOperations*>(argument);
            }, session, nullptr)) delete session;
    });
}

bool SubmitPickerFileOperation(const std::shared_ptr<PickerFileOperations>& session,
    const std::shared_ptr<PickerFileOperation>& operation, PickerFileRequest request,
    HWND owner, bool verify_copies) {
    auto work = std::make_unique<Work>(Work{session, operation, std::move(request), owner, verify_copies});
    auto* argument = work.release();
    if (TrySubmitThreadpoolCallback(Run, argument, nullptr)) return true;
    delete argument;
    return false;
}

} // namespace pulse::ops
