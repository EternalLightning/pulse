// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

// clipboard.cpp — CF_HDROP + CFSTR_PREFERREDDROPEFFECT interop.
#include "clipboard.h"
#include "../common/shell_id.h"
#include "../common/path_utils.h"
#include "../common/portable_device_item.h"
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <algorithm>
#include <cstring>
#include <memory>
#include <set>
#include <string_view>
#include <unordered_set>
#include <wrl/client.h>

namespace pulse::ops {

bool PreferredEffectIsCut(uint32_t preferred_effect) {
    return preferred_effect == DROPEFFECT_MOVE;
}

namespace {

HWND ClipboardOwner() {
    if (HWND owner = GetActiveWindow()) return owner;
    return GetConsoleWindow();
}

bool OpenClipboardWithRetry(HWND owner) {
    for (int attempt = 0; attempt < 20; ++attempt) {
        if (OpenClipboard(owner)) return true;
        Sleep(5);
    }
    return false;
}

} // namespace

bool WriteClipboard(const std::vector<std::wstring>& paths, bool cut, HWND owner) {
    if (paths.empty()) return false;

    // DROPFILES + double-null-terminated wide path list.
    size_t chars = 1; // final terminator
    for (const auto& p : paths) chars += p.size() + 1;
    size_t bytes = sizeof(DROPFILES) + chars * sizeof(wchar_t);

    HGLOBAL hdrop = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, bytes);
    if (!hdrop) return false;
    auto* df = static_cast<DROPFILES*>(GlobalLock(hdrop));
    if (!df) {
        GlobalFree(hdrop);
        return false;
    }
    df->pFiles = sizeof(DROPFILES);
    df->fWide = TRUE;
    wchar_t* dst = reinterpret_cast<wchar_t*>(reinterpret_cast<uint8_t*>(df) + sizeof(DROPFILES));
    for (const auto& p : paths) {
        std::memcpy(dst, p.c_str(), p.size() * sizeof(wchar_t));
        dst += p.size() + 1;
    }
    GlobalUnlock(hdrop);

    // EmptyClipboard assigns ownership to the window passed to OpenClipboard;
    // using nullptr can leave no owner and make SetClipboardData fail.
    if (!OpenClipboardWithRetry(owner ? owner : ClipboardOwner())) {
        GlobalFree(hdrop);
        return false;
    }
    if (!EmptyClipboard() || !SetClipboardData(CF_HDROP, hdrop)) {
        CloseClipboard();
        GlobalFree(hdrop);
        return false;
    }
    // Ownership of hdrop transferred to the clipboard.

    UINT fmtEffect = RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT);
    HGLOBAL heffect = fmtEffect ? GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, sizeof(DWORD)) : nullptr;
    bool effect_set = false;
    if (fmtEffect != 0 && heffect) {
        DWORD* effect = static_cast<DWORD*>(GlobalLock(heffect));
        if (effect) {
            *effect = cut ? DROPEFFECT_MOVE : DROPEFFECT_COPY;
            GlobalUnlock(heffect);
            if (SetClipboardData(fmtEffect, heffect)) effect_set = true;
            else GlobalFree(heffect);
        } else {
            GlobalFree(heffect);
        }
    }
    if (!effect_set) {
        // CF_HDROP ownership already transferred; clearing the clipboard lets
        // the system release it without a double-free.
        EmptyClipboard();
        CloseClipboard();
        return false;
    }
    CloseClipboard();
    return true;
}

bool WriteClipboardText(const std::wstring& text, HWND owner) {
    if (text.empty()) return false;
    size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!h) return false;
    void* memory = GlobalLock(h);
    if (!memory) {
        GlobalFree(h);
        return false;
    }
    std::memcpy(memory, text.c_str(), bytes);
    GlobalUnlock(h);
    if (!OpenClipboardWithRetry(owner ? owner : ClipboardOwner())) {
        GlobalFree(h);
        return false;
    }
    if (!EmptyClipboard() || !SetClipboardData(CF_UNICODETEXT, h)) {
        CloseClipboard();
        GlobalFree(h);
        return false;
    }
    // Ownership of h transferred to the clipboard.
    CloseClipboard();
    return true;
}

namespace {
HRESULT CaptureClipboardPaths(ClipboardData& out, uint32_t expected_sequence) {
    out = {};
    if (!OpenClipboardWithRetry(nullptr)) return HRESULT_FROM_WIN32(ERROR_BUSY);
    struct ClipboardLock { ~ClipboardLock() { CloseClipboard(); } } clipboard;
    if (expected_sequence && GetClipboardSequenceNumber() != expected_sequence)
        return HRESULT_FROM_WIN32(ERROR_CANCELLED);

    if (HANDLE h = GetClipboardData(CF_HDROP)) {
        if (HDROP hdrop = static_cast<HDROP>(GlobalLock(h))) {
            struct DropLock { HANDLE data; ~DropLock() { GlobalUnlock(data); } } drop{h};
            UINT count = DragQueryFileW(hdrop, 0xFFFFFFFF, nullptr, 0);
            out.paths.reserve(count);
            for (UINT i = 0; i < count; ++i) {
                UINT len = DragQueryFileW(hdrop, i, nullptr, 0);
                std::wstring path(static_cast<size_t>(len) + 1, L'\0');
                const UINT copied = DragQueryFileW(hdrop, i, path.data(), len + 1);
                if (!copied) continue;
                path.resize(copied);
                out.paths.push_back(std::move(path));
            }
        }
    }
    if (out.paths.empty()) return S_FALSE;
    UINT fmtEffect = RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT);
    if (HANDLE h = GetClipboardData(fmtEffect)) {
        if (DWORD* effect = static_cast<DWORD*>(GlobalLock(h))) {
            out.cut = GlobalSize(h) >= sizeof(DWORD) && PreferredEffectIsCut(*effect);
            GlobalUnlock(h);
        }
    }
    // The same owner can advance the sequence while rendering data under this lock.
    out.sequence = GetClipboardSequenceNumber();
    return S_OK;
}
} // namespace

HRESULT ReadClipboardForPaste(uint32_t expected_sequence, ClipboardData& out) {
    const HRESULT result = CaptureClipboardPaths(out, expected_sequence);
    if (result == S_OK && out.sequence != GetClipboardSequenceNumber()) {
        out = {};
        return HRESULT_FROM_WIN32(ERROR_CANCELLED);
    }
    return result;
}

bool ReadClipboard(ClipboardData& out) {
    if (IsClipboardFormatAvailable(CF_HDROP)) return ReadClipboardForPaste(0, out) == S_OK;
    out = {};
    return false;
}

namespace {
// Hiding async capability makes the destination complete descriptor-only
// transfers before Drop returns, while all provider work stays on the STA worker.
class SynchronousShellData final : public IDataObject {
public:
    SynchronousShellData(IDataObject* data, const std::atomic_bool* cancelled)
        : data_(data), cancelled_(cancelled) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (id != IID_IUnknown && id != IID_IDataObject) return E_NOINTERFACE;
        *out = static_cast<IDataObject*>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refs_); }
    ULONG STDMETHODCALLTYPE Release() override {
        const auto refs = InterlockedDecrement(&refs_);
        if (!refs) delete this;
        return refs;
    }
    HRESULT STDMETHODCALLTYPE GetData(FORMATETC* format, STGMEDIUM* medium) override {
        if (Cancelled()) return HRESULT_FROM_WIN32(ERROR_CANCELLED);
        const HRESULT result = data_->GetData(format, medium);
        if (SUCCEEDED(result) && Cancelled()) {
            ReleaseStgMedium(medium);
            *medium = {};
            return HRESULT_FROM_WIN32(ERROR_CANCELLED);
        }
        return result;
    }
    HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC* format, STGMEDIUM* medium) override {
        return Cancelled() ? HRESULT_FROM_WIN32(ERROR_CANCELLED) : data_->GetDataHere(format, medium);
    }
    HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC* format) override { return data_->QueryGetData(format); }
    HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC* in, FORMATETC* out) override {
        return data_->GetCanonicalFormatEtc(in, out);
    }
    HRESULT STDMETHODCALLTYPE SetData(FORMATETC* format, STGMEDIUM* medium, BOOL release) override {
        return data_->SetData(format, medium, release);
    }
    HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD direction, IEnumFORMATETC** out) override {
        return data_->EnumFormatEtc(direction, out);
    }
    HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC* format, DWORD flags, IAdviseSink* sink, DWORD* token) override {
        return data_->DAdvise(format, flags, sink, token);
    }
    HRESULT STDMETHODCALLTYPE DUnadvise(DWORD token) override { return data_->DUnadvise(token); }
    HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA** out) override { return data_->EnumDAdvise(out); }
private:
    bool Cancelled() const { return cancelled_ && cancelled_->load(); }
    LONG refs_ = 1;
    Microsoft::WRL::ComPtr<IDataObject> data_;
    const std::atomic_bool* cancelled_;
};

class ShellTransferProgress final : public IFileOperationProgressSink {
public:
    ShellTransferProgress(const std::atomic_bool* cancelled, IShellItem* destination,
                          std::vector<std::wstring>* completed_names)
        : cancelled_(cancelled), destination_(destination), completed_names_(completed_names) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (id != IID_IUnknown && id != IID_IFileOperationProgressSink) return E_NOINTERFACE;
        *out = static_cast<IFileOperationProgressSink*>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refs_); }
    ULONG STDMETHODCALLTYPE Release() override {
        const auto refs = InterlockedDecrement(&refs_);
        if (!refs) delete this;
        return refs;
    }
    HRESULT Check() const { return cancelled_ && cancelled_->load() ? HRESULT_FROM_WIN32(ERROR_CANCELLED) : S_OK; }
    HRESULT Result() const { return result_; }
    HRESULT Record(HRESULT result) { if (FAILED(result) && SUCCEEDED(result_)) result_ = result; return Check(); }
    HRESULT Record(HRESULT result, IShellItem* folder, IShellItem* item) {
        int order = 1;
        if (completed_names_ && completed_names_->size() < 1024 && remaining_name_units_ &&
            SUCCEEDED(result) && item && folder && destination_ &&
            SUCCEEDED(folder->Compare(destination_, SICHINT_CANONICAL, &order)) && order == 0) {
            auto name = portable_device::ShellItemName(item);
            if (!name.empty() && name.size() <= remaining_name_units_ && seen_names_.insert(name).second) {
                remaining_name_units_ -= name.size();
                completed_names_->push_back(std::move(name));
            }
        }
        return Record(result);
    }
    HRESULT STDMETHODCALLTYPE StartOperations() override { return Check(); }
    HRESULT STDMETHODCALLTYPE FinishOperations(HRESULT result) override { return Record(result); }
    HRESULT STDMETHODCALLTYPE PreRenameItem(DWORD, IShellItem*, LPCWSTR) override { return Check(); }
    HRESULT STDMETHODCALLTYPE PostRenameItem(DWORD, IShellItem*, LPCWSTR, HRESULT result, IShellItem*) override { return Record(result); }
    HRESULT STDMETHODCALLTYPE PreMoveItem(DWORD, IShellItem*, IShellItem*, LPCWSTR) override { return Check(); }
    HRESULT STDMETHODCALLTYPE PostMoveItem(DWORD, IShellItem*, IShellItem* folder, LPCWSTR, HRESULT result, IShellItem* item) override { return Record(result, folder, item); }
    HRESULT STDMETHODCALLTYPE PreCopyItem(DWORD, IShellItem*, IShellItem*, LPCWSTR) override { return Check(); }
    HRESULT STDMETHODCALLTYPE PostCopyItem(DWORD, IShellItem*, IShellItem* folder, LPCWSTR, HRESULT result, IShellItem* item) override { return Record(result, folder, item); }
    HRESULT STDMETHODCALLTYPE PreDeleteItem(DWORD, IShellItem*) override { return Check(); }
    HRESULT STDMETHODCALLTYPE PostDeleteItem(DWORD, IShellItem*, HRESULT result, IShellItem*) override { return Record(result); }
    HRESULT STDMETHODCALLTYPE PreNewItem(DWORD, IShellItem*, LPCWSTR) override { return Check(); }
    HRESULT STDMETHODCALLTYPE PostNewItem(DWORD, IShellItem* folder, LPCWSTR, LPCWSTR, DWORD, HRESULT result, IShellItem* item) override { return Record(result, folder, item); }
    HRESULT STDMETHODCALLTYPE UpdateProgress(UINT, UINT) override { return Check(); }
    HRESULT STDMETHODCALLTYPE ResetTimer() override { return Check(); }
    HRESULT STDMETHODCALLTYPE PauseTimer() override { return Check(); }
    HRESULT STDMETHODCALLTYPE ResumeTimer() override { return Check(); }
private:
    LONG refs_ = 1;
    const std::atomic_bool* cancelled_;
    IShellItem* destination_;
    std::vector<std::wstring>* completed_names_;
    // These names only select visible results in the device view.
    size_t remaining_name_units_ = 64 * 1024;
    std::unordered_set<std::wstring> seen_names_;
    HRESULT result_ = S_OK;
};
} // namespace

HRESULT PerformShellOperation(IFileOperation* operation, IShellItem* destination, std::vector<std::wstring>* completed_names,
                              const std::atomic_bool* cancelled) {
    Microsoft::WRL::ComPtr<ShellTransferProgress> progress;
    progress.Attach(new ShellTransferProgress(cancelled, destination, completed_names));
    DWORD cookie = 0;
    HRESULT hr = operation->Advise(progress.Get(), &cookie);
    if (FAILED(hr)) return hr;
    hr = operation->PerformOperations();
    BOOL aborted = FALSE;
    if (SUCCEEDED(hr)) hr = operation->GetAnyOperationsAborted(&aborted);
    operation->Unadvise(cookie);
    if (SUCCEEDED(hr) && FAILED(progress->Result())) hr = progress->Result();
    if (aborted || (cancelled && cancelled->load())) return HRESULT_FROM_WIN32(ERROR_CANCELLED);
    return hr;
}

bool HasShellFileData(IDataObject* data) {
    if (!data) return false;
    for (const auto* name : {CFSTR_SHELLIDLIST, CFSTR_FILEDESCRIPTORW}) {
        FORMATETC format{static_cast<CLIPFORMAT>(RegisterClipboardFormatW(name)), nullptr,
            DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
        if (SUCCEEDED(data->QueryGetData(&format))) return true;
    }
    return false;
}

namespace {
HRESULT ResolveShellClipboardItems(IDataObject* data, IShellItemArray** out) {
    const HRESULT hr = SHCreateShellItemArrayFromDataObject(data, IID_PPV_ARGS(out));
    if (SUCCEEDED(hr)) return hr;

    // Pulse publishes filesystem copies as CF_HDROP without a Shell ID list.
    FORMATETC format{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    STGMEDIUM medium{};
    HRESULT result = data->GetData(&format, &medium);
    if (FAILED(result)) return DV_E_FORMATETC;
    const std::unique_ptr<STGMEDIUM, decltype(&ReleaseStgMedium)> release(&medium, ReleaseStgMedium);
    if (medium.tymed != TYMED_HGLOBAL || !medium.hGlobal) return E_INVALIDARG;
    const HDROP drop = static_cast<HDROP>(medium.hGlobal);
    const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
    if (!count) return E_INVALIDARG;
    std::vector<PIDLIST_ABSOLUTE> ids(count);
    struct FreeIds {
        const std::vector<PIDLIST_ABSOLUTE>& ids;
        ~FreeIds() { for (auto id : ids) CoTaskMemFree(id); }
    } free_ids{ids};
    for (UINT i = 0; i < count; ++i) {
        const UINT length = DragQueryFileW(drop, i, nullptr, 0);
        std::wstring source(static_cast<size_t>(length) + 1, L'\0');
        if (!length || DragQueryFileW(drop, i, source.data(), length + 1) != length) return E_INVALIDARG;
        source.resize(length);
        const auto shell_path = path::StripExtendedPathPrefix(source);
        result = SHParseDisplayName(shell_path.c_str(), nullptr, &ids[i], 0, nullptr);
        if (FAILED(result)) return result;
        if (!ids[i]) return E_FAIL;
    }
    return SHCreateShellItemArrayFromIDLists(count, const_cast<PCIDLIST_ABSOLUTE_ARRAY>(ids.data()), out);
}

HRESULT PasteShellDataToFolder(IDataObject* data, IShellItem* destination, DWORD preferred_effect,
                               const std::atomic_bool* cancelled, HWND owner,
                               std::vector<std::wstring>* completed_names = nullptr,
                               const std::function<bool()>& begin_transfer = {}) {
    if (cancelled && cancelled->load()) return HRESULT_FROM_WIN32(ERROR_CANCELLED);
    Microsoft::WRL::ComPtr<IDataObject> synchronous;
    synchronous.Attach(new SynchronousShellData(data, cancelled));
    data = synchronous.Get();
    Microsoft::WRL::ComPtr<IShellItemArray> items;
    const HRESULT resolved = ResolveShellClipboardItems(data, &items);
    if (SUCCEEDED(resolved)) {
        Microsoft::WRL::ComPtr<IFileOperation> operation;
        HRESULT hr = CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&operation));
        DWORD flags = FOF_NOCONFIRMMKDIR;
        DWORD count = 0;
        bool same_folder = SUCCEEDED(hr) && SUCCEEDED(items->GetCount(&count)) && count != 0;
        for (UINT i = 0; same_folder && i < count; ++i) {
            Microsoft::WRL::ComPtr<IShellItem> item, parent;
            int order = 1;
            same_folder = SUCCEEDED(items->GetItemAt(i, &item)) && SUCCEEDED(item->GetParent(&parent)) &&
                SUCCEEDED(parent->Compare(destination, SICHINT_CANONICAL, &order)) && order == 0;
        }
        if (same_folder && !PreferredEffectIsCut(preferred_effect)) flags |= FOF_RENAMEONCOLLISION;
        if (SUCCEEDED(hr) && begin_transfer && !begin_transfer()) return HRESULT_FROM_WIN32(ERROR_CANCELLED);
        if (SUCCEEDED(hr) && owner) hr = operation->SetOwnerWindow(owner);
        if (SUCCEEDED(hr)) hr = operation->SetOperationFlags(flags);
        if (SUCCEEDED(hr)) hr = PreferredEffectIsCut(preferred_effect)
            ? operation->MoveItems(items.Get(), destination)
            : operation->CopyItems(items.Get(), destination);
        // A drop target may return while a provider is still transferring data.
        // Keep the source alive and report only the file operation's terminal result.
        return SUCCEEDED(hr) ? PerformShellOperation(operation.Get(), destination, completed_names, cancelled) : hr;
    }
    if (resolved != DV_E_FORMATETC) return resolved;
    Microsoft::WRL::ComPtr<IDropTarget> drop;
    const HRESULT bound = destination->BindToHandler(nullptr, BHID_SFUIObject, IID_PPV_ARGS(&drop));
    if (FAILED(bound)) return bound;

    HRESULT result = E_NOINTERFACE;
    if (drop) {
        if (begin_transfer && !begin_transfer()) return HRESULT_FROM_WIN32(ERROR_CANCELLED);
        const DWORD wanted = PreferredEffectIsCut(preferred_effect) ? DROPEFFECT_MOVE : DROPEFFECT_COPY;
        DWORD effect = wanted;
        const DWORD keys = MK_LBUTTON | (wanted == DROPEFFECT_MOVE ? MK_SHIFT : MK_CONTROL);
        POINTL point{0, 0};
        if (SUCCEEDED(drop->DragEnter(data, keys, point, &effect))) {
            if ((effect & wanted) && (!cancelled || !cancelled->load())) {
                effect = wanted;
                result = drop->Drop(data, keys, point, &effect);
                if (SUCCEEDED(result) && effect != wanted) result = E_FAIL;
            } else {
                drop->DragLeave();
            }
        }
    }
    return cancelled && cancelled->load() ? HRESULT_FROM_WIN32(ERROR_CANCELLED) : result;
}

DWORD ShellClipboardEffect(IDataObject* data) {
    DWORD preferred_effect = DROPEFFECT_COPY;
    const UINT preferred_format = RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT);
    FORMATETC format{ static_cast<CLIPFORMAT>(preferred_format), nullptr,
                      DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
    STGMEDIUM medium{};
    if (preferred_format && SUCCEEDED(data->GetData(&format, &medium))) {
        if (medium.tymed == TYMED_HGLOBAL && medium.hGlobal &&
            GlobalSize(medium.hGlobal) >= sizeof(DWORD)) {
            if (const auto* preferred = static_cast<const DWORD*>(GlobalLock(medium.hGlobal))) {
                preferred_effect = *preferred;
                GlobalUnlock(medium.hGlobal);
            }
        }
        ReleaseStgMedium(&medium);
    }
    return PreferredEffectIsCut(preferred_effect) ? DROPEFFECT_MOVE : DROPEFFECT_COPY;
}
} // namespace

HRESULT PasteShellData(IDataObject* data, const std::wstring& dir, DWORD preferred_effect,
                       const std::atomic_bool* cancelled) {
    if (!data || (preferred_effect != DROPEFFECT_COPY && preferred_effect != DROPEFFECT_MOVE)) return E_INVALIDARG;
    if (cancelled && cancelled->load()) return HRESULT_FROM_WIN32(ERROR_CANCELLED);
    Microsoft::WRL::ComPtr<IShellItem> destination;
    const HRESULT hr = shell_id::CreateItem(path::StripExtendedPathPrefix(dir), IID_PPV_ARGS(&destination));
    if (FAILED(hr)) return hr;
    return PasteShellDataToFolder(data, destination.Get(), preferred_effect, cancelled, nullptr);
}

namespace {
HRESULT CaptureShellClipboard(uint32_t expected_sequence, Microsoft::WRL::ComPtr<IDataObject>& data) {
    if (!expected_sequence || GetClipboardSequenceNumber() != expected_sequence)
        return HRESULT_FROM_WIN32(ERROR_CANCELLED);
    const HRESULT captured = OleGetClipboard(&data);
    if (FAILED(captured)) return captured;
    if (GetClipboardSequenceNumber() != expected_sequence) return HRESULT_FROM_WIN32(ERROR_CANCELLED);
    return data ? S_OK : DV_E_FORMATETC;
}

HRESULT PasteClipboard(const std::wstring& dir, uint32_t expected_sequence,
                        const std::atomic_bool* cancelled, HWND owner,
                        const std::function<bool()>& begin_transfer) {
    const HRESULT ole = OleInitialize(nullptr);
    if (FAILED(ole)) return ole;
    struct OleDone { ~OleDone() { OleUninitialize(); } } ole_done;

    Microsoft::WRL::ComPtr<IDataObject> data;
    const HRESULT captured = CaptureShellClipboard(expected_sequence, data);
    if (FAILED(captured)) return captured;
    if (cancelled && cancelled->load())
        return HRESULT_FROM_WIN32(ERROR_CANCELLED);
    const auto effect = ShellClipboardEffect(data.Get());
    Microsoft::WRL::ComPtr<IShellItem> folder;
    const HRESULT located = shell_id::CreateItem(path::StripExtendedPathPrefix(dir), IID_PPV_ARGS(&folder));
    if (FAILED(located)) return located;
    return PasteShellDataToFolder(data.Get(), folder.Get(), effect, cancelled, owner, nullptr, begin_transfer);
}
} // namespace

bool PasteShellClipboard(const std::wstring& dir, uint32_t expected_sequence) {
    return SUCCEEDED(PasteClipboard(dir, expected_sequence, nullptr, nullptr, {}));
}

HRESULT PasteShellClipboard(const std::wstring& dir, uint32_t expected_sequence,
                            const std::atomic_bool& cancelled, HWND owner,
                            const std::function<bool()>& begin_transfer) {
    return PasteClipboard(dir, expected_sequence, &cancelled, owner, begin_transfer);
}

HRESULT PasteShellClipboardToFolder(IShellItem* folder, HWND owner, std::vector<std::wstring>& completed_names,
                                    uint32_t expected_sequence) {
    Microsoft::WRL::ComPtr<IDataObject> data;
    const HRESULT hr = CaptureShellClipboard(expected_sequence, data);
    if (FAILED(hr)) return hr;
    return PasteShellDataToFolder(data.Get(), folder, ShellClipboardEffect(data.Get()), nullptr, owner, &completed_names);
}

#ifdef PULSE_CLIPBOARD_TESTING
HRESULT ResolveShellClipboardItemsForTest(IDataObject* data, IShellItemArray** out) {
    return ResolveShellClipboardItems(data, out);
}

bool PasteShellDataForTest(IDataObject* data, const std::wstring& dir, uint32_t preferred_effect) {
    const HRESULT ole = OleInitialize(nullptr);
    if (FAILED(ole)) return false;
    struct OleDone { ~OleDone() { OleUninitialize(); } } ole_done;
    return SUCCEEDED(PasteShellData(data, dir,
        PreferredEffectIsCut(preferred_effect) ? DROPEFFECT_MOVE : DROPEFFECT_COPY));
}
#endif

bool CutPasteMovedAllPaths(const std::vector<std::wstring>& expected_normalized,
                          const std::vector<std::wstring>& moved_normalized) {
    const auto path_order = [](std::wstring_view a, std::wstring_view b) {
        return CompareStringOrdinal(a.data(), static_cast<int>(a.size()),
            b.data(), static_cast<int>(b.size()), TRUE) == CSTR_LESS_THAN;
    };
    const std::set<std::wstring_view, decltype(path_order)> moved(
        moved_normalized.begin(), moved_normalized.end(), path_order);
    return !expected_normalized.empty() && std::all_of(expected_normalized.begin(), expected_normalized.end(),
        [&](const auto& path) { return moved.contains(path); });
}

bool CompleteCutClipboard(uint32_t sequence, const std::vector<std::wstring>& expected_paths, HWND owner) {
    if (sequence == 0 || expected_paths.empty() || GetClipboardSequenceNumber() != sequence)
        return false;
    ClipboardData current;
    if (!ReadClipboard(current) || !current.cut || current.sequence != sequence ||
        current.paths.size() != expected_paths.size()) return false;
    for (size_t i = 0; i < expected_paths.size(); ++i) {
        if (_wcsicmp(current.paths[i].c_str(), expected_paths[i].c_str()) != 0) return false;
    }
    if (!OpenClipboardWithRetry(owner ? owner : ClipboardOwner())) return false;
    if (GetClipboardSequenceNumber() != sequence || !EmptyClipboard()) {
        CloseClipboard();
        return false;
    }
    const UINT format = RegisterClipboardFormatW(CFSTR_PERFORMEDDROPEFFECT);
    HGLOBAL value = format ? GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, sizeof(DWORD)) : nullptr;
    bool ok = false;
    if (format && value) {
        auto* effect = static_cast<DWORD*>(GlobalLock(value));
        if (effect) {
            *effect = DROPEFFECT_MOVE;
            GlobalUnlock(value);
            ok = SetClipboardData(format, value) != nullptr;
        }
        if (!ok) GlobalFree(value);
    }
    CloseClipboard();
    return ok;
}

} // namespace pulse::ops
