// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "shell_interaction_client.h"
#include "shell_client.h"
#include <memory>

namespace pulse::ipc {
namespace {
struct Interaction {
    HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::atomic<uint32_t> request{0};
    std::atomic<bool> cancel{false};
    std::atomic<bool> completed{false};
    ShellInteractionResult result;
    void Complete(HRESULT error, bool cancelled, std::wstring value = {}) {
        if (completed.exchange(true)) return;
        result = {error, cancelled, std::move(value)};
        SetEvent(done);
    }
    ~Interaction() { if (done) CloseHandle(done); }
};

struct ModalOwner {
    HWND window = nullptr;
    bool enabled = false;
    explicit ModalOwner(HWND owner) : window(owner), enabled(owner && IsWindowEnabled(owner)) {
        if (enabled) EnableWindow(window, FALSE);
    }
    ~ModalOwner() {
        if (enabled && IsWindow(window)) {
            EnableWindow(window, TRUE);
            SetActiveWindow(window);
        }
    }
};
}

ShellInteractionResult RunShellInteraction(const ShellLaunchRequest& request) {
    auto state = std::make_shared<Interaction>();
    if (!state->done) return {HRESULT_FROM_WIN32(GetLastError()), false, {}};
    const HWND owner = reinterpret_cast<HWND>(static_cast<uintptr_t>(request.owner_hwnd));
    const bool pump = owner && GetWindowThreadProcessId(owner, nullptr) == GetCurrentThreadId();
    ModalOwner modal(pump ? owner : nullptr);
    try {
        std::thread([state, request] {
            try {
                auto& client = ShellClient::Instance();
                const auto id = client.Launch(request, [state](HRESULT error, bool cancelled, std::wstring value) {
                    state->Complete(error, cancelled, std::move(value));
                });
                state->request.store(id);
                if (!id) {
                    state->Complete(HRESULT_FROM_WIN32(ERROR_NOT_READY), false);
                } else if (state->cancel.load()) client.Cancel(id);
            } catch (...) {
                state->Complete(E_OUTOFMEMORY, false);
            }
        }).detach();
    } catch (...) { return {E_OUTOFMEMORY, false, {}}; }
    for (;;) {
        const DWORD wait = pump ? MsgWaitForMultipleObjectsEx(1, &state->done, 50,
            QS_ALLINPUT, MWMO_INPUTAVAILABLE) : WaitForSingleObject(state->done, 50);
        if (wait == WAIT_OBJECT_0) return std::move(state->result);
        if (wait == WAIT_FAILED || (pump && !IsWindow(owner))) break;
        if (pump && wait == WAIT_OBJECT_0 + 1) {
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                if (message.message == WM_QUIT) {
                    PostQuitMessage(static_cast<int>(message.wParam));
                    state->cancel.store(true);
                    if (const auto id = state->request.load()) ShellClient::Instance().Cancel(id);
                    return {HRESULT_FROM_WIN32(ERROR_CANCELLED), true, {}};
                }
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }
    }
    state->cancel.store(true);
    if (const auto id = state->request.load()) ShellClient::Instance().Cancel(id);
    return {HRESULT_FROM_WIN32(ERROR_CANCELLED), true, {}};
}

bool PickShellPath(HWND owner, bool folder, const std::wstring& title, std::wstring& path,
                   const std::vector<std::wstring>& filters, const std::wstring& initial) {
    ShellLaunchRequest request;
    request.kind = folder ? ShellLaunchKind::PickFolder : ShellLaunchKind::PickFile;
    request.owner_hwnd = reinterpret_cast<uintptr_t>(owner);
    request.flags = 0;
    request.title = title;
    request.filters = filters;
    request.directory = initial;
    const auto result = RunShellInteraction(request);
    if (FAILED(result.error) || result.cancelled || result.value.empty() || result.value.size() > 32768 ||
        result.value.find(L'\0') != std::wstring::npos) return false;
    path = result.value;
    return true;
}
}
