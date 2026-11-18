// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "device_operations.h"
#include <shellapi.h>
#include "../common/shell_id.h"
#include "../ops/clipboard.h"
#include <shlobj.h>
#include <wrl/client.h>

namespace pulse::shell {
using Microsoft::WRL::ComPtr;

HRESULT ExecuteDeviceOperation(const DeviceOperation& request) {
    // Clipboard providers must remain on an OLE-initialized STA after copy returns.
    struct OleSession {
        HRESULT result = OleInitialize(nullptr);
        ~OleSession() { if (SUCCEEDED(result)) OleUninitialize(); }
    };
    thread_local OleSession ole;
    if (FAILED(ole.result)) return ole.result;
    if (request.ids.empty() || request.ids.size() > 4096) return E_INVALIDARG;
    const auto& verb = request.verb;
    if (verb != L"copy" && verb != L"cut" && verb != L"paste" && verb != L"open" &&
        verb != L"properties" && verb != L"newfolder") return E_INVALIDARG;
    std::vector<PIDLIST_ABSOLUTE> pidls;
    struct FreeIds {
        std::vector<PIDLIST_ABSOLUTE>& ids;
        ~FreeIds() { for (auto id : ids) CoTaskMemFree(id); }
    } free_ids{pidls};
    for (const auto& id : request.ids) {
        PIDLIST_ABSOLUTE pidl = nullptr;
        const HRESULT hr = shell_id::ParseId(id, &pidl);
        if (FAILED(hr)) return hr;
        pidls.push_back(pidl);
    }
    ComPtr<IShellItemArray> items;
    HRESULT hr = SHCreateShellItemArrayFromIDLists(static_cast<UINT>(pidls.size()),
        const_cast<PCIDLIST_ABSOLUTE_ARRAY>(pidls.data()), &items);
    if (FAILED(hr)) return hr;
    if (verb == L"paste" || verb == L"newfolder") {
        if (pidls.size() != 1) return E_INVALIDARG;
        ComPtr<IShellItem> folder;
        hr = items->GetItemAt(0, &folder);
        if (FAILED(hr)) return hr;
        SFGAOF attrs = 0;
        const bool paste = verb == L"paste";
        hr = folder->GetAttributes(paste ? SFGAO_FOLDER : SFGAO_FOLDER | SFGAO_READONLY, &attrs);
        if (FAILED(hr)) return hr;
        if (!(attrs & SFGAO_FOLDER) || (attrs & SFGAO_READONLY)) return E_ACCESSDENIED;
        if (paste) return ops::PasteShellClipboardToFolder(folder.Get(), request.owner);
        if (request.name.empty() || request.name == L"." || request.name == L".." ||
            request.name.find_first_of(L"\\/:*?\"<>|") != std::wstring::npos) return E_INVALIDARG;
        for (const auto ch : request.name) if (ch < 32) return E_INVALIDARG;
        ComPtr<IFileOperation> operation;
        hr = CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&operation));
        if (SUCCEEDED(hr)) hr = operation->SetOwnerWindow(request.owner);
        if (SUCCEEDED(hr)) hr = operation->SetOperationFlags(FOF_NOCONFIRMMKDIR);
        if (SUCCEEDED(hr)) hr = operation->NewItem(folder.Get(), FILE_ATTRIBUTE_DIRECTORY,
            request.name.c_str(), nullptr, nullptr);
        if (SUCCEEDED(hr)) hr = operation->PerformOperations();
        BOOL aborted = FALSE;
        if (SUCCEEDED(hr)) hr = operation->GetAnyOperationsAborted(&aborted);
        return SUCCEEDED(hr) && aborted ? HRESULT_FROM_WIN32(ERROR_CANCELLED) : hr;
    }
    ComPtr<IContextMenu> menu;
    hr = items->BindToHandler(nullptr, BHID_SFUIObject, IID_PPV_ARGS(&menu));
    if (FAILED(hr)) return hr;
    HMENU popup = CreatePopupMenu();
    if (!popup) return HRESULT_FROM_WIN32(GetLastError());
    hr = menu->QueryContextMenu(popup, 0, 1, 0x7fff, CMF_NORMAL);
    if (SUCCEEDED(hr)) {
        std::string narrow;
        for (const wchar_t ch : verb) narrow.push_back(static_cast<char>(ch));
        CMINVOKECOMMANDINFOEX invoke{};
        invoke.cbSize = sizeof(invoke);
        invoke.fMask = CMIC_MASK_UNICODE;
        invoke.hwnd = request.owner;
        invoke.lpVerb = narrow.c_str();
        invoke.lpVerbW = verb.c_str();
        invoke.nShow = SW_SHOWNORMAL;
        hr = menu->InvokeCommand(reinterpret_cast<CMINVOKECOMMANDINFO*>(&invoke));
    }
    DestroyMenu(popup);
    return hr;
}
}
