// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "device_navigation.h"
#include "app_internal.h"
#include "../common/localization.h"
#include <algorithm>

namespace pulse::app {
bool IsDeviceTab(const Tab* tab) { return tab && fs::IsDevicePath(tab->current_path); }

bool HasDeviceSelection(const Tab& tab) {
    for (const auto index : tab.SelectedIndices()) {
        if (index >= 0 && index < tab.CountBound() &&
            fs::IsDevicePath(tab.EntryAt(static_cast<size_t>(index)).full_path)) return true;
    }
    return false;
}

std::vector<std::wstring> DeviceSelectedIds(const Tab& tab) {
    std::vector<std::wstring> ids;
    const auto devices = DeviceRegistry::Instance().Devices();
    for (const auto index : tab.SelectedIndices()) {
        if (index < 0 || index >= tab.CountBound()) return {};
        const auto& row = tab.EntryAt(static_cast<size_t>(index));
        if (!fs::IsDevicePath(row.full_path)) return {};
        std::wstring id = row.shell_id;
        if (id.empty() && row.pc_device) {
            const auto path = fs::ParseDevicePath(row.full_path);
            if (path) for (const auto& device : devices)
                if (device.key == path->device) { id = device.id; break; }
        }
        if (id.empty()) return {};
        ids.push_back(std::move(id));
    }
    return ids;
}

std::wstring DeviceTitle(const std::wstring& path) {
    const auto where = fs::ParseDevicePath(path);
    if (!where) return l10n::Get(l10n::StringId::DevicePhone);
    if (!where->parts.empty()) return where->parts.back();
    std::wstring name;
    bool skip = false;
    DeviceRegistry::Instance().Describe(where->device, &name, &skip);
    if (name.empty()) name = l10n::Get(l10n::StringId::DevicePhone);
    if (where->IsDeviceRoot() || skip) return name;
    return where->storage;
}

std::wstring DeviceParent(const std::wstring& path) {
    auto where = fs::ParseDevicePath(path);
    if (!where || where->IsDeviceRoot()) return {};
    bool skip = false;
    DeviceRegistry::Instance().Describe(where->device, nullptr, &skip);
    if (!where->parts.empty()) where->parts.pop_back();
    else if (skip) return {};
    else where->storage.clear();
    return fs::FormatDevicePath(*where);
}

void QueueDeviceOperation(AppState& state, std::wstring verb,
        std::vector<std::wstring> ids, std::wstring name) {
    if (ids.empty()) return;
    if (!state.device_shell_owner) {
        ipc::ShellClient::Callbacks callbacks;
        callbacks.done = [mailbox = state.device_operation_mailbox](uint32_t, uint32_t hr,
                bool cancelled, std::wstring) {
            if (mailbox) mailbox->Publish({static_cast<HRESULT>(hr), cancelled});
        };
        state.device_shell_owner = ipc::ShellClient::Instance().Acquire(std::move(callbacks));
    }
    const auto owner = state.device_shell_owner;
    const auto mailbox = state.device_operation_mailbox;
    const HWND window = state.hwnd;
    const bool queued = state.worker.EnqueueIo([window, owner, mailbox, verb = std::move(verb),
            ids = std::move(ids), name = std::move(name)] {
        if (!ipc::ShellClient::Instance().DeviceOperation(window, verb, ids, name, owner) && mailbox)
            mailbox->Publish({HRESULT_FROM_WIN32(ERROR_NOT_READY), false});
    });
    if (!queued && mailbox) mailbox->Publish({HRESULT_FROM_WIN32(ERROR_NOT_READY), false});
}

void ApplyDeviceOperationResult(AppState& state, const DeviceOperationResult& result) {
    if (result.cancelled) return;
    if (FAILED(result.error)) {
        wchar_t code[32]{};
        swprintf_s(code, L" (0x%08lX)", static_cast<unsigned long>(result.error));
        MessageBoxW(state.hwnd, (l10n::Get(l10n::StringId::DeviceFailedMessage) + code).c_str(),
            l10n::Get(l10n::StringId::DeviceFailed).c_str(), MB_OK | MB_ICONWARNING);
    }
    std::vector<std::wstring> paths;
    ForEachPane(state, [&](Pane& pane) {
        if (const auto* tab = pane.ActiveTab(); tab &&
            (IsDeviceTab(tab) || !fs::IsVirtualPath(tab->current_path))) paths.push_back(tab->current_path);
    });
    for (const auto& path : paths) RefreshPath(state, path, RefreshReason::OperationCompleted);
}

void SetDeviceError(Tab& tab, fs::DeviceError error) {
    using I = l10n::StringId;
    I title = I::DeviceFailed, message = I::DeviceFailedMessage;
    if (error == fs::DeviceError::Disconnected) title = I::DeviceDisconnected;
    else if (error == fs::DeviceError::Locked) { title = I::DeviceLockedTitle; message = I::DeviceLockedMessage; }
    else if (error == fs::DeviceError::TimedOut) { title = I::DeviceTimedOut; message = I::DeviceTimedOutMessage; }
    tab.SetBannerTitle(BannerType::Other, l10n::Get(title));
    tab.banner_message = l10n::Get(message);
}

void DevicesChanged(AppState& state) {
    RefreshSidebarModel(state);
    std::vector<std::wstring> paths;
    ForEachPane(state, [&](Pane& pane) {
        if (auto* tab = pane.ActiveTab(); tab && (tab->current_path.empty() || IsDeviceTab(tab)))
            paths.push_back(tab->current_path);
    });
    std::sort(paths.begin(), paths.end());
    paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
    for (const auto& path : paths) RefreshPath(state, path, RefreshReason::OperationCompleted);
    InvalidateRect(state.hwnd, nullptr, FALSE);
}
}
