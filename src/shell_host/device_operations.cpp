// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "device_operations.h"
#include <shellapi.h>
#include "../common/file_name.h"
#include "../common/portable_device_item.h"
#include "../common/shell_id.h"
#include "../ops/clipboard.h"
#include <shlobj.h>
#include <wrl/client.h>

namespace pulse::shell {
using Microsoft::WRL::ComPtr;

namespace {
HRESULT SetDeviceClipboard(IShellItemArray* items, DWORD effect) {
    ComPtr<IDataObject> data;
    HRESULT hr = items->BindToHandler(nullptr, BHID_DataObject, IID_PPV_ARGS(&data));
    if (FAILED(hr)) return hr;
    if (!ops::HasShellFileData(data.Get())) return DV_E_FORMATETC;
    const auto format_id = RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT);
    if (!format_id) return E_FAIL;
    FORMATETC format{static_cast<CLIPFORMAT>(format_id), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    STGMEDIUM medium{TYMED_HGLOBAL};
    medium.hGlobal = GlobalAlloc(GMEM_MOVEABLE, sizeof(DWORD));
    if (!medium.hGlobal) return E_OUTOFMEMORY;
    auto* value = static_cast<DWORD*>(GlobalLock(medium.hGlobal));
    if (!value) {
        ReleaseStgMedium(&medium);
        return E_OUTOFMEMORY;
    }
    *value = effect;
    GlobalUnlock(medium.hGlobal);
    hr = data->SetData(&format, &medium, TRUE);
    if (FAILED(hr)) ReleaseStgMedium(&medium);
    return SUCCEEDED(hr) ? OleSetClipboard(data.Get()) : hr;
}

bool IsDeviceMember(IShellItem* item, IShellItem* computer, SFGAOF capability) {
    SFGAOF attrs = 0;
    if (FAILED(item->GetAttributes(SFGAO_FILESYSTEM | capability, &attrs)) ||
        (attrs & (SFGAO_FILESYSTEM | capability)) != capability) return false;
    ComPtr<IShellItem> node = item;
    for (int depth = 0; depth < 128; ++depth) {
        ComPtr<IShellItem> parent;
        if (FAILED(node->GetParent(&parent)) || !parent) return false;
        int order = 1;
        if (SUCCEEDED(parent->Compare(computer, SICHINT_CANONICAL, &order)) && order == 0) {
            std::wstring parsing;
            return depth >= 2 && portable_device::IsDevice(node.Get(), parsing);
        }
        node = std::move(parent);
    }
    return false;
}
}

HRESULT ExecuteDeviceOperation(const DeviceOperation& request, std::vector<std::wstring>& completed_names) {
    completed_names.clear();
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
        verb != L"properties" && verb != L"newfolder" && verb != L"delete" && verb != L"rename") return E_INVALIDARG;
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
    if (verb == L"delete" || verb == L"rename" || verb == L"cut") {
        const SFGAOF capability = verb == L"delete" ? SFGAO_CANDELETE : verb == L"rename" ? SFGAO_CANRENAME : SFGAO_CANMOVE;
        ComPtr<IShellItem> computer;
        hr = SHGetKnownFolderItem(FOLDERID_ComputerFolder, KF_FLAG_DEFAULT, nullptr, IID_PPV_ARGS(&computer));
        if (FAILED(hr)) return hr;
        for (DWORD index = 0; index < pidls.size(); ++index) {
            ComPtr<IShellItem> item;
            hr = items->GetItemAt(index, &item);
            if (FAILED(hr)) return hr;
            if (!IsDeviceMember(item.Get(), computer.Get(), capability)) return E_ACCESSDENIED;
        }
    }
    if (verb == L"copy" || verb == L"cut") {
        hr = SetDeviceClipboard(items.Get(), verb == L"cut" ? DROPEFFECT_MOVE : DROPEFFECT_COPY);
        if (SUCCEEDED(hr)) return hr;
    }
    if (verb == L"rename") {
        if (pidls.size() != 1 || !path::IsValidFileName(request.name)) return E_INVALIDARG;
        ComPtr<IShellFolder> parent;
        PCUITEMID_CHILD child = nullptr;
        hr = SHBindToParent(pidls.front(), IID_PPV_ARGS(&parent), &child);
        if (FAILED(hr)) return hr;
        PITEMID_CHILD renamed = nullptr;
        hr = parent->SetNameOf(request.owner, child, request.name.c_str(), SHGDN_INFOLDER | SHGDN_FORPARSING, &renamed);
        if (SUCCEEDED(hr)) {
            ComPtr<IShellItem> item;
            if (renamed && SUCCEEDED(SHCreateShellItem(nullptr, parent.Get(), renamed, &item))) {
                const auto name = portable_device::ShellItemName(item.Get());
                if (!name.empty()) completed_names.push_back(name);
            }
            if (completed_names.empty()) completed_names.push_back(request.name);
        }
        CoTaskMemFree(renamed);
        return hr;
    }
    if (verb == L"delete") {
        ComPtr<IFileOperation> operation;
        hr = CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&operation));
        if (SUCCEEDED(hr) && request.owner) hr = operation->SetOwnerWindow(request.owner);
        if (SUCCEEDED(hr)) hr = operation->SetOperationFlags(FOF_NOCONFIRMATION);
        if (SUCCEEDED(hr)) hr = operation->DeleteItems(items.Get());
        return SUCCEEDED(hr) ? ops::PerformShellOperation(operation.Get()) : hr;
    }
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
        if (paste) return ops::PasteShellClipboardToFolder(folder.Get(), request.owner, completed_names);
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
        return SUCCEEDED(hr) ? ops::PerformShellOperation(operation.Get(), folder.Get(), &completed_names) : hr;
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
