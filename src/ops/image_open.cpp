#include "image_open.h"

#include "../common/path_utils.h"
#include "../common/preview_extensions.h"
#include <shlobj.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <propkey.h>
#include <wrl.h>
#include <algorithm>
#include <cwctype>
#include <memory>
#include <mutex>
#include <vector>

namespace pulse::ops {
namespace {
using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Make;
using Microsoft::WRL::RuntimeClass;
using Microsoft::WRL::RuntimeClassFlags;
using Microsoft::WRL::ClassicCom;
using Microsoft::WRL::ChainInterfaces;
using Microsoft::WRL::FtmBase;

struct PidlDeleter {
    using pointer = LPITEMIDLIST;
    void operator()(pointer value) const { CoTaskMemFree(value); }
};
using Pidl = std::unique_ptr<ITEMIDLIST, PidlDeleter>;

bool IsImagePath(const std::wstring& path) {
    std::wstring extension = PathFindExtensionW(path.c_str());
    for (auto& ch : extension) ch = static_cast<wchar_t>(std::towlower(ch));
    if (pulse::preview::IsImageExtension(extension)) return true;
    PERCEIVED perceived = PERCEIVED_TYPE_UNSPECIFIED;
    PERCEIVEDFLAG flags = 0;
    return !extension.empty() &&
        SUCCEEDED(AssocGetPerceivedType(extension.c_str(), &perceived, &flags, nullptr)) &&
        perceived == PERCEIVED_TYPE_IMAGE;
}

template <typename Function>
HRESULT ComCall(Function&& function) noexcept {
    try { return function(); }
    catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
    catch (...) { return E_FAIL; }
}

bool SameShellItem(IShellFolder* folder, PCUIDLIST_RELATIVE left, PCUIDLIST_RELATIVE right) {
    if (!left || !right) return false;
    if (ILIsEqual(left, right)) return true;
    const HRESULT compared = folder->CompareIDs(SHCIDS_CANONICALONLY, left, right);
    return SUCCEEDED(compared) && HRESULT_CODE(compared) == 0;
}

struct ViewItem { Pidl pidl; SFGAOF attributes = 0; };
struct FolderContents;
HRESULT ReadContents(PCIDLIST_ABSOLUTE target, FolderContents& contents, HWND owner);
struct FolderContents {
    std::shared_ptr<void> lifetime;
    Pidl folder, selected;
    mutable std::vector<ViewItem> items;
    mutable int focused = -1;
    bool lazy = false;
    mutable std::once_flag read_once;
    mutable HRESULT read_result = E_PENDING;
    HRESULT EnsureContents() const {
        if (!lazy) return S_OK;
        return ComCall([&]() -> HRESULT {
            std::call_once(read_once, [&] {
                read_result = ComCall([&]() -> HRESULT {
                    Pidl target(ILCombine(folder.get(), selected.get()));
                    if (!target) return E_OUTOFMEMORY;
                    FolderContents loaded;
                    const HRESULT hr = ReadContents(target.get(), loaded, nullptr);
                    if (FAILED(hr)) return hr;
                    items = std::move(loaded.items);
                    focused = loaded.focused;
                    return S_OK;
                });
            });
            return read_result;
        });
    }
    // Bind on the calling apartment. No apartment-bound Shell objects are kept
    // in the free-threaded site if the handler retains it after activation.
    HRESULT Bind(IShellFolder** out) const {
        return SHBindToObject(nullptr, folder.get(), nullptr, IID_PPV_ARGS(out));
    }
    bool MatchesFolder(PCIDLIST_ABSOLUTE other) const {
        if (!other) return false;
        if (ILIsEqual(other, folder.get())) return true;
        ComPtr<IShellFolder> desktop;
        return SUCCEEDED(SHGetDesktopFolder(&desktop)) &&
            SameShellItem(desktop.Get(), other, folder.get());
    }
    bool MatchesSelected(PCUITEMID_CHILD other) const {
        if (!other) return false;
        if (ILIsEqual(other, selected.get())) return true;
        ComPtr<IShellFolder> parent;
        return SUCCEEDED(Bind(&parent)) && SameShellItem(parent.Get(), other, selected.get());
    }
    std::vector<PCUITEMID_CHILD> Children(UINT flags) const {
        if ((flags & SVGIO_TYPE_MASK) == SVGIO_SELECTION) return {selected.get()};
        std::vector<PCUITEMID_CHILD> children;
        children.reserve(items.size());
        if (!(flags & SVGIO_FLAG_VIEWORDER)) children.push_back(selected.get());
        for (size_t i = 0; i < items.size(); ++i) {
            if (!(flags & SVGIO_FLAG_VIEWORDER) && static_cast<int>(i) == focused) continue;
            children.push_back(items[i].pidl.get());
        }
        return children;
    }
};

bool PrepareContents(PCIDLIST_ABSOLUTE target, FolderContents& contents) {
    contents.folder.reset(ILCloneFull(target));
    contents.selected.reset(ILClone(ILFindLastID(target)));
    if (!contents.folder || !contents.selected ||
        !ILRemoveLastID(contents.folder.get())) return false;
    contents.lazy = true;
    return true;
}

struct ViewerWindowIdentity {
    HWND window = nullptr;
    DWORD pid = 0;
    bool matching_title = false;
};

class ViewerWindowTracker {
public:
    explicit ViewerWindowTracker(std::wstring name) : name_(std::move(name)) {}
    ~ViewerWindowTracker() { if (launcher_) CloseHandle(launcher_); }
    ViewerWindowTracker(const ViewerWindowTracker&) = delete;
    ViewerWindowTracker& operator=(const ViewerWindowTracker&) = delete;

    bool CaptureBeforeLaunch() noexcept {
        bool captured = true;
        struct Capture { ViewerWindowTracker* tracker; bool* captured; } capture{this, &captured};
        EnumWindows([](HWND window, LPARAM parameter) -> BOOL {
            auto& state = *reinterpret_cast<Capture*>(parameter);
            DWORD pid = 0;
            GetWindowThreadProcessId(window, &pid);
            try {
                state.tracker->before_.push_back({window, pid, state.tracker->MatchesTitle(window)});
            } catch (...) { *state.captured = false; return FALSE; }
            return TRUE;
        }, reinterpret_cast<LPARAM>(&capture));
        return captured;
    }

    void SetLauncher(HANDLE process) noexcept { launcher_ = process; }

    bool Active() noexcept {
        if (window_) {
            DWORD pid = 0;
            return IsWindow(window_) && GetWindowThreadProcessId(window_, &pid) && pid == viewer_pid_;
        }
        EnumWindows([](HWND window, LPARAM parameter) -> BOOL {
            auto& tracker = *reinterpret_cast<ViewerWindowTracker*>(parameter);
            if (!IsWindowVisible(window)) return TRUE;
            DWORD pid = 0;
            GetWindowThreadProcessId(window, &pid);
            const bool matching_title = tracker.MatchesTitle(window);
            if (!matching_title) return TRUE;
            const auto previous = std::find_if(tracker.before_.begin(), tracker.before_.end(),
                [&](const auto& item) { return item.window == window && item.pid == pid; });
            // Association handlers may reuse an existing viewer window. An
            // unchanged background window with the same basename is unrelated.
            if (previous != tracker.before_.end() && previous->matching_title &&
                GetForegroundWindow() != window) return TRUE;
            tracker.window_ = window;
            tracker.viewer_pid_ = pid;
            return FALSE;
        }, reinterpret_cast<LPARAM>(this));
        // Packaged viewers can return a short-lived launcher instead of their
        // actual process. Without an identifiable viewer window, retain the
        // context until host shutdown rather than expire a live media session.
        return true;
    }

private:
    bool MatchesTitle(HWND window) const noexcept {
        wchar_t title[1024]{};
        if (!GetWindowTextW(window, title, ARRAYSIZE(title))) return false;
        const wchar_t* match = wcsstr(title, name_.c_str());
        if (!match) return false;
        const auto boundary = [](wchar_t ch) { return !ch || iswspace(ch) || ch == L'-' || ch == L'\u2014'; };
        return (match == title || boundary(match[-1])) && boundary(match[name_.size()]);
    }

    std::wstring name_;
    std::vector<ViewerWindowIdentity> before_;
    HANDLE launcher_ = nullptr;
    HWND window_ = nullptr;
    DWORD viewer_pid_ = 0;
};

class ChildEnumerator final : public RuntimeClass<RuntimeClassFlags<ClassicCom>, IEnumIDList, FtmBase> {
public:
    ChildEnumerator(std::shared_ptr<const FolderContents> contents,
                    std::vector<PCUITEMID_CHILD> children, size_t cursor = 0)
        : contents_(std::move(contents)), children_(std::move(children)), cursor_(cursor) {}
    HRESULT STDMETHODCALLTYPE Next(ULONG count, PITEMID_CHILD* out, ULONG* fetched) override {
        if (fetched) *fetched = 0;
        if (!out || (!fetched && count != 1)) return E_POINTER;
        std::lock_guard lock(mutex_);
        ULONG produced = 0;
        for (; produced < count && cursor_ < children_.size(); ++produced, ++cursor_) {
            out[produced] = ILClone(children_[cursor_]);
            if (!out[produced]) {
                for (ULONG i = 0; i < produced; ++i) { CoTaskMemFree(out[i]); out[i] = nullptr; }
                cursor_ -= produced;
                return E_OUTOFMEMORY;
            }
        }
        if (fetched) *fetched = produced;
        return produced == count ? S_OK : S_FALSE;
    }
    HRESULT STDMETHODCALLTYPE Skip(ULONG count) override {
        std::lock_guard lock(mutex_);
        const size_t skipped = std::min(static_cast<size_t>(count), children_.size() - cursor_);
        cursor_ += skipped;
        return skipped == count ? S_OK : S_FALSE;
    }
    HRESULT STDMETHODCALLTYPE Reset() override { std::lock_guard lock(mutex_); cursor_ = 0; return S_OK; }
    HRESULT STDMETHODCALLTYPE Clone(IEnumIDList** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        return ComCall([&]() -> HRESULT {
            std::lock_guard lock(mutex_);
            const auto copy = Make<ChildEnumerator>(contents_, children_, cursor_);
            return copy ? copy.CopyTo(out) : E_OUTOFMEMORY;
        });
    }
private:
    std::shared_ptr<const FolderContents> contents_;
    std::vector<PCUITEMID_CHILD> children_;
    size_t cursor_ = 0;
    std::mutex mutex_;
};

class ImageOpenSite final : public RuntimeClass<RuntimeClassFlags<ClassicCom>, IServiceProvider,
    ChainInterfaces<IFolderView2, IFolderView>, IEnumerableView, IOleWindow,
    ChainInterfaces<IPersistIDList, IPersist>, IColumnManager, FtmBase> {
public:
    ImageOpenSite(HWND owner, std::shared_ptr<const FolderContents> contents)
        : owner_(owner), contents_(std::move(contents)) {}
    HRESULT STDMETHODCALLTYPE QueryService(REFGUID service, REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        const HRESULT hr = service == SID_SFolderView || service == SID_EnumerableView
            ? QueryInterface(iid, out) : E_NOINTERFACE;
        return hr;
    }
    HRESULT STDMETHODCALLTYPE GetWindow(HWND* out) override {
        if (!out) return E_POINTER;
        *out = IsWindow(owner_) ? owner_ : nullptr;
        return *out ? S_OK : E_FAIL;
    }
    HRESULT STDMETHODCALLTYPE ContextSensitiveHelp(BOOL) override { return E_NOTIMPL; }
    // Shell serializes the folder identity and visible columns when handing
    // the view to Photos. Missing either interface drops the neighbor context.
    HRESULT STDMETHODCALLTYPE GetClassID(CLSID* out) override {
        if (!out) return E_POINTER;
        *out = GUID_NULL;
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE SetIDList(PCIDLIST_ABSOLUTE) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetIDList(PIDLIST_ABSOLUTE* out) override {
        if (!out) return E_POINTER;
        *out = ILCloneFull(contents_->folder.get());
        return *out ? S_OK : E_OUTOFMEMORY;
    }
    HRESULT STDMETHODCALLTYPE GetColumnCount(CM_ENUM_FLAGS flags, UINT* out) override {
        if (!out) return E_POINTER;
        *out = 0;
        if (flags != CM_ENUM_ALL && flags != CM_ENUM_VISIBLE) return E_INVALIDARG;
        *out = 1;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetColumns(CM_ENUM_FLAGS flags, PROPERTYKEY* out, UINT count) override {
        if (!out) return E_POINTER;
        if ((flags != CM_ENUM_ALL && flags != CM_ENUM_VISIBLE) || count != 1) return E_INVALIDARG;
        *out = PKEY_ItemNameDisplay;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetColumnInfo(REFPROPERTYKEY key, CM_COLUMNINFO* out) override {
        if (!out) return E_POINTER;
        if (out->cbSize != sizeof(*out) || !IsEqualPropertyKey(key, PKEY_ItemNameDisplay)) return E_INVALIDARG;
        if (out->dwMask & CM_MASK_STATE) out->dwState = CM_STATE_VISIBLE | CM_STATE_ALWAYSVISIBLE;
        if (out->dwMask & CM_MASK_WIDTH) out->uWidth = 180;
        if (out->dwMask & CM_MASK_DEFAULTWIDTH) out->uDefaultWidth = 180;
        if (out->dwMask & CM_MASK_IDEALWIDTH) out->uIdealWidth = 180;
        if (out->dwMask & CM_MASK_NAME) wcscpy_s(out->wszName, L"Name");
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetColumnInfo(REFPROPERTYKEY, const CM_COLUMNINFO*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetColumns(const PROPERTYKEY*, UINT) override { return E_NOTIMPL; }

    HRESULT STDMETHODCALLTYPE GetFolder(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (iid == IID_IShellItemArray) {
            PCIDLIST_ABSOLUTE folder_pidl = contents_->folder.get();
            ComPtr<IShellItemArray> array;
            const HRESULT hr = SHCreateShellItemArrayFromIDLists(1, &folder_pidl, &array);
            return SUCCEEDED(hr) ? array.CopyTo(iid, out) : hr;
        }
        ComPtr<IShellFolder> folder;
        HRESULT hr = contents_->Bind(&folder);
        if (SUCCEEDED(hr)) hr = folder.CopyTo(iid, out);
        if (FAILED(hr)) hr = SHCreateItemFromIDList(contents_->folder.get(), iid, out);
        return hr;
    }
    HRESULT STDMETHODCALLTYPE Item(int index, PITEMID_CHILD* out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        const HRESULT ready = contents_->EnsureContents();
        if (FAILED(ready)) return ready;
        if (index < 0 || static_cast<size_t>(index) >= contents_->items.size()) return E_INVALIDARG;
        *out = ILClone(contents_->items[static_cast<size_t>(index)].pidl.get());
        return *out ? S_OK : E_OUTOFMEMORY;
    }
    HRESULT STDMETHODCALLTYPE ItemCount(UINT flags, int* out) override {
        if (!out) return E_POINTER;
        *out = 0;
        const UINT type = flags & SVGIO_TYPE_MASK;
        if (type != SVGIO_SELECTION && type != SVGIO_ALLVIEW && type != SVGIO_BACKGROUND) return E_INVALIDARG;
        if (type != SVGIO_SELECTION) {
            const HRESULT ready = contents_->EnsureContents();
            if (FAILED(ready)) return ready;
        }
        *out = type == SVGIO_SELECTION ? 1 : static_cast<int>(contents_->items.size());
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Items(UINT flags, REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        int count = 0;
        HRESULT hr = ItemCount(flags, &count);
        if (FAILED(hr)) return hr;
        hr = ComCall([&]() -> HRESULT {
            auto children = contents_->Children(flags);
            if (iid == IID_IEnumIDList) {
                const auto enumerator = Make<ChildEnumerator>(contents_, std::move(children));
                return enumerator ? enumerator.CopyTo(iid, out) : E_OUTOFMEMORY;
            }
            ComPtr<IShellFolder> folder;
            HRESULT status = contents_->Bind(&folder);
            if (FAILED(status)) return status;
            if (iid == IID_IDataObject)
                return folder->GetUIObjectOf(owner_, static_cast<UINT>(children.size()), children.data(), iid, nullptr, out);
            ComPtr<IShellItemArray> array;
            status = SHCreateShellItemArray(contents_->folder.get(), folder.Get(),
                static_cast<UINT>(children.size()), children.data(), &array);
            return SUCCEEDED(status) ? array.CopyTo(iid, out) : status;
        });
        return hr;
    }
    HRESULT STDMETHODCALLTYPE SetEnumReadyCallback(IEnumReadyCallback* callback) override {
        return callback ? callback->EnumReady() : S_OK;
    }
    HRESULT STDMETHODCALLTYPE CreateEnumIDListFromContents(PCIDLIST_ABSOLUTE folder, DWORD flags, IEnumIDList** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (!contents_->MatchesFolder(folder)) return E_INVALIDARG;
        const HRESULT ready = contents_->EnsureContents();
        if (FAILED(ready)) return ready;
        const HRESULT hr = ComCall([&]() -> HRESULT {
            std::vector<PCUITEMID_CHILD> children;
            for (const auto& item : contents_->items) {
                if (item.attributes & SFGAO_FOLDER) {
                    if (!(flags & SHCONTF_FOLDERS)) continue;
                } else if (!(flags & SHCONTF_NONFOLDERS)) continue;
                children.push_back(item.pidl.get());
            }
            const auto enumerator = Make<ChildEnumerator>(contents_, std::move(children));
            return enumerator ? enumerator.CopyTo(out) : E_OUTOFMEMORY;
        });
        return hr;
    }
    HRESULT STDMETHODCALLTYPE GetFocusedItem(int* out) override {
        if (!out) return E_POINTER;
        *out = -1;
        const HRESULT ready = contents_->EnsureContents();
        if (FAILED(ready)) return ready;
        *out = contents_->focused;
        return *out >= 0 ? S_OK : E_FAIL;
    }
    HRESULT STDMETHODCALLTYPE GetSelectionMarkedItem(int* out) override { return GetFocusedItem(out); }
    HRESULT STDMETHODCALLTYPE GetItem(int index, REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        const HRESULT ready = contents_->EnsureContents();
        if (FAILED(ready)) return ready;
        if (index < 0 || static_cast<size_t>(index) >= contents_->items.size()) return E_INVALIDARG;
        return SHCreateItemWithParent(contents_->folder.get(), nullptr,
            contents_->items[static_cast<size_t>(index)].pidl.get(), iid, out);
    }
    HRESULT STDMETHODCALLTYPE GetSelectedItem(int start, int* out) override {
        if (!out) return E_POINTER;
        *out = -1;
        const HRESULT ready = contents_->EnsureContents();
        if (FAILED(ready)) return ready;
        // iStart is inclusive; a search from zero must include the first image.
        *out = contents_->focused >= 0 && contents_->focused >= start ? contents_->focused : -1;
        return *out >= 0 ? S_OK : S_FALSE;
    }
    HRESULT STDMETHODCALLTYPE GetSelection(BOOL, IShellItemArray** out) override {
        return Items(SVGIO_SELECTION, IID_PPV_ARGS(out));
    }
    HRESULT STDMETHODCALLTYPE GetSelectionState(PCUITEMID_CHILD pidl, DWORD* out) override {
        if (!out) return E_POINTER;
        *out = contents_->MatchesSelected(pidl) ? SVSI_SELECT | SVSI_FOCUSED : 0;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetSortColumnCount(int* out) override { if (!out) return E_POINTER; *out = 1; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetSortColumns(SORTCOLUMN* out, int count) override {
        if (!out) return E_POINTER;
        if (count != 1) return E_INVALIDARG;
        *out = {PKEY_ItemNameDisplay, SORT_ASCENDING};
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetCurrentViewMode(UINT* out) override { if (!out) return E_POINTER; *out = FVM_DETAILS; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetCurrentFolderFlags(DWORD* out) override { if (!out) return E_POINTER; *out = FWF_SINGLESEL; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetViewModeAndIconSize(FOLDERVIEWMODE* mode, int* size) override {
        if (!mode || !size) return E_POINTER;
        *mode = FVM_DETAILS; *size = 16; return S_OK;
    }
    // This site supplies read-only activation context; it does not host a Shell UI.
    HRESULT STDMETHODCALLTYPE SetCurrentViewMode(UINT) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetItemPosition(PCUITEMID_CHILD, POINT*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetSpacing(POINT*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetDefaultSpacing(POINT*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetAutoArrange() override { return S_FALSE; }
    HRESULT STDMETHODCALLTYPE SelectItem(int, DWORD) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SelectAndPositionItems(UINT, PCUITEMID_CHILD_ARRAY, POINT*, DWORD) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetGroupBy(REFPROPERTYKEY, BOOL) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetGroupBy(PROPERTYKEY* key, BOOL* ascending) override {
        if (!key) return E_POINTER;
        *key = {}; if (ascending) *ascending = TRUE; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetViewProperty(PCUITEMID_CHILD, REFPROPERTYKEY, REFPROPVARIANT) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetViewProperty(PCUITEMID_CHILD, REFPROPERTYKEY, PROPVARIANT*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetTileViewProperties(PCUITEMID_CHILD, LPCWSTR) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetExtendedTileViewProperties(PCUITEMID_CHILD, LPCWSTR) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetText(FVTEXTTYPE, LPCWSTR) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetCurrentFolderFlags(DWORD, DWORD) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetSortColumns(const SORTCOLUMN*, int) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetVisibleItem(int, BOOL, int*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE InvokeVerbOnSelection(LPCSTR) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetViewModeAndIconSize(FOLDERVIEWMODE, int) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetGroupSubsetCount(UINT) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetGroupSubsetCount(UINT*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetRedraw(BOOL) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE IsMoveInSameFolder() override { return S_FALSE; }
    HRESULT STDMETHODCALLTYPE DoRename() override { return E_NOTIMPL; }
private:
    HWND owner_ = nullptr;
    std::shared_ptr<const FolderContents> contents_;
};

HRESULT ReadContents(PCIDLIST_ABSOLUTE target, FolderContents& contents, HWND owner) {
    ComPtr<IShellFolder> folder;
    PCUITEMID_CHILD selected = nullptr;
    HRESULT hr = SHBindToParent(target, IID_PPV_ARGS(&folder), &selected);
    if (FAILED(hr)) return hr;
    contents.folder.reset(ILCloneFull(target));
    contents.selected.reset(ILClone(selected));
    if (!contents.folder || !contents.selected) return E_OUTOFMEMORY;
    if (!ILRemoveLastID(contents.folder.get())) return E_INVALIDARG;
    ComPtr<IEnumIDList> enumerator;
    hr = folder->EnumObjects(owner, SHCONTF_FOLDERS | SHCONTF_NONFOLDERS, &enumerator);
    if (FAILED(hr)) return hr;
    if (enumerator) {
        for (;;) {
            PITEMID_CHILD child = nullptr;
            hr = enumerator->Next(1, &child, nullptr);
            Pidl owned(child);
            if (hr == S_FALSE) break;
            if (FAILED(hr)) return hr;
            if (!owned) return E_FAIL;
            SFGAOF attributes = SFGAO_FOLDER;
            PCUITEMID_CHILD item = owned.get();
            hr = folder->GetAttributesOf(1, &item, &attributes);
            // A vanished or inaccessible sibling must not discard image context.
            if (FAILED(hr)) continue;
            contents.items.push_back({std::move(owned), attributes});
        }
    }
    const auto is_selected = [&](const ViewItem& item) {
        return SameShellItem(folder.Get(), item.pidl.get(), contents.selected.get());
    };
    if (std::none_of(contents.items.begin(), contents.items.end(), is_selected)) {
        Pidl copy(ILClone(contents.selected.get()));
        if (!copy) return E_OUTOFMEMORY;
        contents.items.push_back({std::move(copy), 0});
    }
    std::sort(contents.items.begin(), contents.items.end(), [&](const auto& left, const auto& right) {
        const HRESULT compared = folder->CompareIDs(0, left.pidl.get(), right.pidl.get());
        return SUCCEEDED(compared) && static_cast<short>(HRESULT_CODE(compared)) < 0;
    });
    for (size_t i = 0; i < contents.items.size(); ++i) {
        if (!is_selected(contents.items[i])) continue;
        Pidl selected_view(ILClone(contents.items[i].pidl.get()));
        if (!selected_view) return E_OUTOFMEMORY;
        // Selection and enumeration should return the same representation.
        contents.selected = std::move(selected_view);
        contents.focused = static_cast<int>(i);
        break;
    }
    return S_OK;
}
}

struct ImageOpenSession::State {
    explicit State(std::wstring name) : viewer(std::move(name)) {}
    ViewerWindowTracker viewer;
    std::shared_ptr<void> site;
};

ImageOpenSession::ImageOpenSession() = default;
ImageOpenSession::~ImageOpenSession() = default;

bool ImageOpenSession::ViewerRunning() noexcept {
    return state_ && state_->viewer.Active();
}

bool TryOpenImageWithNeighbors(HWND owner, const std::wstring& path,
                               std::shared_ptr<void> lifetime, ImageOpenSession* session) noexcept {
    try {
        const std::wstring shell_path = pulse::path::StripExtendedPathPrefix(path);
        if (!IsImagePath(shell_path)) return false;
        PIDLIST_ABSOLUTE raw = nullptr;
        HRESULT hr = SHParseDisplayName(shell_path.c_str(), nullptr, &raw, 0, nullptr);
        Pidl target(raw);
        if (FAILED(hr)) return false;
        auto contents = std::make_shared<FolderContents>();
        contents->lifetime = std::move(lifetime);
        // Identity and selection are enough to activate the viewer. Enumerate
        // siblings only if the handler requests the folder view, once per site.
        if (!PrepareContents(target.get(), *contents)) return false;
        const auto site = Make<ImageOpenSite>(owner, contents);
        if (!site) return false;
        std::unique_ptr<ImageOpenSession::State> retained;
        if (session) {
            retained = std::make_unique<ImageOpenSession::State>(PathFindFileNameW(shell_path.c_str()));
            if (!retained->viewer.CaptureBeforeLaunch()) return false;
            retained->site = std::shared_ptr<void>(site.Get(), [keep = site](void*) {});
        }
        SHELLEXECUTEINFOW execute{sizeof(execute)};
        execute.hwnd = owner;
        execute.lpVerb = L"open";
        execute.lpIDList = target.get();
        execute.hInstApp = reinterpret_cast<HINSTANCE>(static_cast<IServiceProvider*>(site.Get()));
        execute.nShow = SW_SHOWNORMAL;
        // Keep the folder-view site without forcing IContextMenu activation,
        // which loads context-menu handlers before opening the default viewer.
        execute.fMask = SEE_MASK_IDLIST | SEE_MASK_FLAG_HINST_IS_SITE |
            SEE_MASK_FLAG_NO_UI | SEE_MASK_NOASYNC | (session ? SEE_MASK_NOCLOSEPROCESS : 0);
        const BOOL opened = ShellExecuteExW(&execute);
        if (opened && session) {
            retained->viewer.SetLauncher(execute.hProcess);
            session->state_ = std::move(retained);
        } else if (execute.hProcess) CloseHandle(execute.hProcess);
        return opened != FALSE;
    } catch (...) {
        return false;
    }
}

} // namespace pulse::ops
