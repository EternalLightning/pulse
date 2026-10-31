// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "shell_interaction_worker.h"
#include <shellapi.h>
#include <shlobj.h>
#include <wrl/client.h>

namespace pulse::shell {
using Microsoft::WRL::ComPtr;

HRESULT PickShellPath(const ipc::ShellLaunchRequest& request, HWND owner, std::wstring& selected) {
    ComPtr<IFileOpenDialog> dialog;
    HRESULT result = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&dialog));
    if (FAILED(result)) return result;
    FILEOPENDIALOGOPTIONS options = 0;
    result = dialog->GetOptions(&options);
    if (FAILED(result)) return result;
    options |= FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_DONTADDTORECENT;
    options |= request.kind == ipc::ShellLaunchKind::PickFolder ? FOS_PICKFOLDERS : FOS_FILEMUSTEXIST;
    result = dialog->SetOptions(options);
    if (FAILED(result)) return result;
    if (!request.title.empty() && FAILED(result = dialog->SetTitle(request.title.c_str()))) return result;
    std::vector<COMDLG_FILTERSPEC> filters;
    for (size_t i = 0; i < request.filters.size(); i += 2)
        filters.push_back({request.filters[i].c_str(), request.filters[i + 1].c_str()});
    if (!filters.empty() && FAILED(result = dialog->SetFileTypes(static_cast<UINT>(filters.size()),
        filters.data()))) return result;
    if (!request.directory.empty()) {
        ComPtr<IShellItem> initial;
        if (SUCCEEDED(SHCreateItemFromParsingName(request.directory.c_str(), nullptr,
                IID_PPV_ARGS(&initial)))) dialog->SetFolder(initial.Get());
    }
    result = dialog->Show(owner);
    if (FAILED(result)) return result;
    ComPtr<IShellItem> item;
    result = dialog->GetResult(&item);
    if (FAILED(result)) return result;
    PWSTR raw = nullptr;
    result = item->GetDisplayName(SIGDN_FILESYSPATH, &raw);
    if (SUCCEEDED(result) && raw) selected = raw;
    CoTaskMemFree(raw);
    return SUCCEEDED(result) && selected.empty() ? E_FAIL : result;
}

HRESULT RunElevated(const ipc::ShellLaunchRequest& request, std::wstring& exit_code,
                    const std::function<bool()>& heartbeat) {
    SHELLEXECUTEINFOW execute{sizeof(execute)};
    execute.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
    execute.lpVerb = L"runas";
    execute.lpFile = request.executable.c_str();
    execute.lpParameters = request.parameters.c_str();
    execute.nShow = SW_HIDE;
    if (!ShellExecuteExW(&execute)) return HRESULT_FROM_WIN32(GetLastError());
    if (!execute.hProcess) return E_FAIL;
    struct Close { HANDLE handle; ~Close() { CloseHandle(handle); } } close{execute.hProcess};
    DWORD wait = WAIT_TIMEOUT;
    while ((wait = WaitForSingleObject(execute.hProcess, 100)) == WAIT_TIMEOUT) {
        if (!heartbeat()) return HRESULT_FROM_WIN32(ERROR_CANCELLED);
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    if (wait != WAIT_OBJECT_0) return HRESULT_FROM_WIN32(GetLastError());
    DWORD code = ERROR_GEN_FAILURE;
    if (!GetExitCodeProcess(execute.hProcess, &code)) return HRESULT_FROM_WIN32(GetLastError());
    exit_code = std::to_wstring(code);
    return S_OK;
}
}
