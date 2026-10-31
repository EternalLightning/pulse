// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "shell_query_metadata.h"
#include "../fs/cloud_state.h"
#include "../common/path_utils.h"
#include <shlobj.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <wrl/client.h>
#include <array>

namespace pulse::shell {
namespace {
using Microsoft::WRL::ComPtr;
struct Pidl {
    PIDLIST_ABSOLUTE value = nullptr;
    ~Pidl() { CoTaskMemFree(value); }
};
HRESULT LastError() {
    const DWORD error = GetLastError();
    return HRESULT_FROM_WIN32(error ? error : ERROR_GEN_FAILURE);
}
}

void QueryShellName(const ipc::ShellQueryRequest& request, ipc::ShellQueryResult& result) {
    auto path = pulse::path::StripExtendedPathPrefix(request.path);
    LPCWSTR query = path.c_str();
    UINT flags = request.kind == ipc::ShellQueryKind::TypeName ? SHGFI_TYPENAME : SHGFI_DISPLAYNAME;
    DWORD attributes = 0;
    Pidl pidl;
    if (path.starts_with(L"::") || path.starts_with(L"shell:")) {
        result.error = SHParseDisplayName(path.c_str(), nullptr, &pidl.value, 0, nullptr);
        if (FAILED(result.error)) return;
        query = reinterpret_cast<LPCWSTR>(pidl.value);
        flags |= SHGFI_PIDL;
    } else if (request.kind == ipc::ShellQueryKind::TypeName) {
        attributes = request.attributes;
        if (!fs::IsCloudPlaceholder(attributes)) {
            const DWORD current = GetFileAttributesW(request.path.c_str());
            if (current != INVALID_FILE_ATTRIBUTES) attributes |= current;
        }
        if (fs::IsCloudPlaceholder(attributes)) {
            // A selected online-only item still needs a type label. Ask by
            // extension and attributes so Shell never opens the actual item.
            const size_t dot = path.find_last_of(L'.');
            const size_t slash = path.find_last_of(L"\\/");
            path = L"dummy" + (dot != std::wstring::npos &&
                (slash == std::wstring::npos || dot > slash) ? path.substr(dot) : L"");
            attributes = attributes & FILE_ATTRIBUTE_DIRECTORY ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
            query = path.c_str();
            flags |= SHGFI_USEFILEATTRIBUTES;
        }
    }
    SHFILEINFOW info{};
    if (!SHGetFileInfoW(query, attributes, &info, sizeof(info), flags)) {
        result.error = HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND); return;
    }
    result.text = request.kind == ipc::ShellQueryKind::TypeName ? info.szTypeName : info.szDisplayName;
    result.error = result.text.empty() ? E_FAIL : S_OK;
}

void QueryShellNamespace(const ipc::ShellQueryRequest& request, ipc::ShellQueryResult& result) {
    Pidl pidl;
    if (request.kind == ipc::ShellQueryKind::PidlName) {
        if (!ipc::ValidShellPidl(request.pidl)) { result.error = E_INVALIDARG; return; }
        pidl.value = static_cast<PIDLIST_ABSOLUTE>(CoTaskMemAlloc(request.pidl.size()));
        if (!pidl.value) { result.error = E_OUTOFMEMORY; return; }
        std::memcpy(pidl.value, request.pidl.data(), request.pidl.size());
    } else {
        const auto path = pulse::path::StripExtendedPathPrefix(request.path);
        result.error = SHParseDisplayName(path.c_str(), nullptr, &pidl.value, 0, nullptr);
        if (FAILED(result.error) || !pidl.value) {
            if (SUCCEEDED(result.error)) result.error = E_FAIL;
            return;
        }
        const size_t size = ILGetSize(pidl.value);
        const auto* bytes = reinterpret_cast<const uint8_t*>(pidl.value);
        if (!ipc::ValidShellPidl(bytes, size)) { result.error = E_INVALIDARG; return; }
        result.pidl.assign(bytes, bytes + size);
        if (request.kind == ipc::ShellQueryKind::ParsePidl) { result.error = S_OK; return; }
    }
    auto name = [&](SIGDN kind, std::wstring& value) {
        PWSTR text = nullptr;
        const HRESULT error = SHGetNameFromIDList(pidl.value, kind, &text);
        if (SUCCEEDED(error) && text) value = text;
        CoTaskMemFree(text);
        return error;
    };
    if (request.kind == ipc::ShellQueryKind::PidlName) {
        result.error = name(SIGDN_DESKTOPABSOLUTEPARSING, result.text);
        if (SUCCEEDED(result.error) && result.text.empty()) result.error = E_FAIL;
    } else {
        // Virtual folders may have no filesystem path; getters preserve the
        // original empty-name behavior without calling a provider in Pulse.
        name(SIGDN_NORMALDISPLAY, result.text);
        name(SIGDN_FILESYSPATH, result.filesystem_path);
        result.error = S_OK;
    }
}

void InitializePreviewItem(const ipc::ShellQueryRequest& request, ipc::ShellQueryResult& result) {
    if (request.marshal_blob.empty() || request.marshal_blob.size() > ipc::kShellQueryMarshalLimit) {
        result.error = E_INVALIDARG; return;
    }
    ComPtr<IStream> stream;
    result.error = CreateStreamOnHGlobal(nullptr, TRUE, &stream);
    if (FAILED(result.error)) return;
    ULONG written = 0;
    result.error = stream->Write(request.marshal_blob.data(), static_cast<ULONG>(request.marshal_blob.size()), &written);
    if (FAILED(result.error) || written != request.marshal_blob.size()) { result.error = E_FAIL; return; }
    result.error = stream->Seek({}, STREAM_SEEK_SET, nullptr);
    if (FAILED(result.error)) return;
    ComPtr<IUnknown> handler;
    result.error = CoUnmarshalInterface(stream.Get(), IID_PPV_ARGS(&handler));
    if (FAILED(result.error)) return;
    ComPtr<IInitializeWithItem> initialize;
    result.error = handler.As(&initialize);
    if (FAILED(result.error)) return;
    ComPtr<IShellItem> item;
    const auto path = pulse::path::StripExtendedPathPrefix(request.path);
    result.error = SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&item));
    if (FAILED(result.error)) return;
    result.error = initialize->Initialize(item.Get(), STGM_READ);
}

void QueryShellShortcut(const ipc::ShellQueryRequest& request, ipc::ShellQueryResult& result) {
    const DWORD attrs = GetFileAttributesW(request.path.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) { result.error = LastError(); return; }
    if (fs::IsCloudPlaceholder(attrs)) {
        result.error = HRESULT_FROM_WIN32(ERROR_FILE_OFFLINE);
        return;
    }
    ComPtr<IShellLinkW> link;
    result.error = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link));
    if (FAILED(result.error)) return;
    ComPtr<IPersistFile> file;
    result.error = link.As(&file);
    if (FAILED(result.error)) return;
    result.error = file->Load(request.path.c_str(), STGM_READ);
    if (FAILED(result.error)) {
        const auto plain = pulse::path::StripExtendedPathPrefix(request.path);
        if (plain != request.path) result.error = file->Load(plain.c_str(), STGM_READ);
    }
    if (FAILED(result.error) && request.path.size() >= MAX_PATH) {
        // IPersistFile's Shell parser can reject paths that the filesystem supports.
        ComPtr<IStream> stream;
        ComPtr<IPersistStream> persisted;
        result.error = SHCreateStreamOnFileEx(request.path.c_str(), STGM_READ | STGM_SHARE_DENY_WRITE,
            FILE_ATTRIBUTE_NORMAL, FALSE, nullptr, &stream);
        if (SUCCEEDED(result.error)) result.error = link.As(&persisted);
        if (SUCCEEDED(result.error)) result.error = persisted->Load(stream.Get());
    }
    if (FAILED(result.error)) return;
    std::array<wchar_t, 32768> raw{};
    WIN32_FIND_DATAW ignored{};
    // Do not call Resolve: parsing must never track, repair or prompt for targets.
    result.error = link->GetPath(raw.data(), static_cast<int>(raw.size()), &ignored, SLGP_RAWPATH);
    if (FAILED(result.error)) return;
    if (!raw[0]) { result.error = HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND); return; }
    std::wstring target(raw.data());
    std::wstring probe = target;
    if (probe.size() >= MAX_PATH && !probe.starts_with(L"\\\\?\\")) {
        if (probe.starts_with(L"\\\\")) probe = L"\\\\?\\UNC\\" + probe.substr(2);
        else if (probe.size() > 2 && probe[1] == L':') probe = L"\\\\?\\" + probe;
    }
    WIN32_FILE_ATTRIBUTE_DATA attributes{};
    if (!GetFileAttributesExW(probe.c_str(), GetFileExInfoStandard, &attributes)) {
        result.error = LastError(); return;
    }
    result.text = std::move(target);
    result.target_is_dir = (attributes.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    result.target_size = (static_cast<uint64_t>(attributes.nFileSizeHigh) << 32) | attributes.nFileSizeLow;
    result.target_mtime = attributes.ftLastWriteTime;
    result.error = S_OK;
}

} // namespace pulse::shell
