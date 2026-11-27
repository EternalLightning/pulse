// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "ops_manager.h"

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <variant>

namespace pulse::ops {

struct PickerClipboardPaste {
    std::wstring directory;
    uint32_t sequence = 0;
};

struct PickerProperties {
    std::vector<std::wstring> paths;
};
struct PickerClipboardWrite {
    std::vector<std::wstring> paths;
    bool cut = false;
    bool text = false;
};

struct PickerUndo {};

using PickerFileRequest = std::variant<OpRequest, PickerClipboardPaste, PickerProperties, PickerClipboardWrite, PickerUndo>;

class PickerFileOperations {
public:
    OpsManager& Manager() { return manager_; }
    OpsManager& Start(HWND owner, bool verify_copies);
    void Notify();
    void WaitForChange();

private:
    std::mutex mutex_;
    std::condition_variable event_;
    bool changed_ = false;
    bool started_ = false;
    OpsManager manager_;
};

struct PickerFileOperation {
    bool BeginMutation();
    bool Cancel(PickerFileOperations& session);

    std::atomic_bool cancelled{false};
    std::atomic_bool ready{false};
    HRESULT result = E_FAIL;
    bool refresh = false;

private:
    enum class Phase { Preparing, Running, Cancelled };
    std::atomic<Phase> phase_{Phase::Preparing};
};

std::shared_ptr<PickerFileOperations> CreatePickerFileOperations();
bool SubmitPickerFileOperation(const std::shared_ptr<PickerFileOperations>& session,
    const std::shared_ptr<PickerFileOperation>& operation, PickerFileRequest request,
    HWND owner, bool verify_copies);

} // namespace pulse::ops
