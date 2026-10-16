#include "../ui/drag_drop.h"
#include "../ops/clipboard.h"
#include "../ipc/shell_query_protocol.h"
#include <shlobj.h>
#include <cstdio>
#include <cstring>
#include <atomic>
#include <filesystem>
#include <utility>

namespace {
int passed = 0, failed = 0;
void Check(bool good, const wchar_t* name) {
    wprintf(L"[%s] %s\n", good ? L"PASS" : L"FAIL", name);
    good ? ++passed : ++failed;
}

class PreferringDataObject final : public IDataObject {
public:
    explicit PreferringDataObject(DWORD preferred) : preferred_(preferred) {
        inner_ = pulse::ui::FileDataObject::Create({L"C:\\fixture\\source.txt"});
    }
    ~PreferringDataObject() { inner_->Release(); }
    IFACEMETHODIMP QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (iid != IID_IUnknown && iid != IID_IDataObject) return E_NOINTERFACE;
        *out = static_cast<IDataObject*>(this); AddRef(); return S_OK;
    }
    IFACEMETHODIMP_(ULONG) AddRef() override { return ++refs_; }
    IFACEMETHODIMP_(ULONG) Release() override {
        const ULONG remaining = --refs_;
        if (!remaining) delete this;
        return remaining;
    }
    IFACEMETHODIMP GetData(FORMATETC* fmt, STGMEDIUM* out) override {
        if (fmt && out && fmt->cfFormat == RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT)) {
            *out = {};
            out->tymed = TYMED_HGLOBAL;
            out->hGlobal = GlobalAlloc(GMEM_MOVEABLE, sizeof(DWORD));
            if (!out->hGlobal) return E_OUTOFMEMORY;
            *static_cast<DWORD*>(GlobalLock(out->hGlobal)) = preferred_;
            GlobalUnlock(out->hGlobal);
            return S_OK;
        }
        return inner_->GetData(fmt, out);
    }
    IFACEMETHODIMP GetDataHere(FORMATETC* f, STGMEDIUM* m) override { return inner_->GetDataHere(f, m); }
    IFACEMETHODIMP QueryGetData(FORMATETC* f) override { return inner_->QueryGetData(f); }
    IFACEMETHODIMP GetCanonicalFormatEtc(FORMATETC* f, FORMATETC* o) override { return inner_->GetCanonicalFormatEtc(f, o); }
    IFACEMETHODIMP SetData(FORMATETC* f, STGMEDIUM* m, BOOL r) override { return inner_->SetData(f, m, r); }
    IFACEMETHODIMP EnumFormatEtc(DWORD d, IEnumFORMATETC** o) override { return inner_->EnumFormatEtc(d, o); }
    IFACEMETHODIMP DAdvise(FORMATETC*, DWORD, IAdviseSink*, DWORD*) override { return OLE_E_ADVISENOTSUPPORTED; }
    IFACEMETHODIMP DUnadvise(DWORD) override { return OLE_E_ADVISENOTSUPPORTED; }
    IFACEMETHODIMP EnumDAdvise(IEnumSTATDATA**) override { return OLE_E_ADVISENOTSUPPORTED; }
private:
    ULONG refs_ = 1;
    DWORD preferred_;
    pulse::ui::FileDataObject* inner_ = nullptr;
};

class MediumOwner final : public IUnknown {
public:
    MediumOwner(HGLOBAL data, unsigned& released) : data_(data), released_(released) {}
    IFACEMETHODIMP QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (iid != IID_IUnknown) return E_NOINTERFACE;
        *out = this; AddRef(); return S_OK;
    }
    IFACEMETHODIMP_(ULONG) AddRef() override { return ++refs_; }
    IFACEMETHODIMP_(ULONG) Release() override {
        const ULONG remaining = --refs_;
        if (!remaining) delete this;
        return remaining;
    }
private:
    ~MediumOwner() { if (data_) GlobalFree(data_); ++released_; }
    ULONG refs_ = 1;
    HGLOBAL data_;
    unsigned& released_;
};

class MediumDataObject final : public IDataObject {
public:
    MediumDataObject(DWORD tymed, bool empty, unsigned& released)
        : tymed_(tymed), empty_(empty), released_(released) {}
    IFACEMETHODIMP QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (iid != IID_IUnknown && iid != IID_IDataObject) return E_NOINTERFACE;
        *out = this; AddRef(); return S_OK;
    }
    IFACEMETHODIMP_(ULONG) AddRef() override { return ++refs_; }
    IFACEMETHODIMP_(ULONG) Release() override {
        const ULONG remaining = --refs_;
        if (!remaining) delete this;
        return remaining;
    }
    IFACEMETHODIMP GetData(FORMATETC* fmt, STGMEDIUM* out) override {
        if (!fmt || !out) return E_POINTER;
        *out = {};
        if (fmt->cfFormat != CF_HDROP) return DV_E_FORMATETC;
        HGLOBAL data = nullptr;
        if (!empty_) {
            constexpr wchar_t paths[] = L"C:\\fixture\\one.txt\0C:\\fixture\\two.txt\0";
            data = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, sizeof(DROPFILES) + sizeof(paths));
            if (!data) return E_OUTOFMEMORY;
            auto* header = static_cast<DROPFILES*>(GlobalLock(data));
            if (!header) { GlobalFree(data); return E_OUTOFMEMORY; }
            header->pFiles = sizeof(DROPFILES);
            header->fWide = TRUE;
            memcpy(reinterpret_cast<BYTE*>(header) + sizeof(DROPFILES), paths, sizeof(paths));
            GlobalUnlock(data);
        }
        out->tymed = tymed_;
        out->hGlobal = data;
        // Custom owner also makes a deliberately mismatched medium safe to release.
        out->pUnkForRelease = new MediumOwner(data, released_);
        last_owner_ = out->pUnkForRelease;
        last_owner_->AddRef();
        return S_OK;
    }
    IFACEMETHODIMP GetDataHere(FORMATETC*, STGMEDIUM*) override { return E_NOTIMPL; }
    IFACEMETHODIMP QueryGetData(FORMATETC*) override { return E_NOTIMPL; }
    IFACEMETHODIMP GetCanonicalFormatEtc(FORMATETC*, FORMATETC*) override { return E_NOTIMPL; }
    IFACEMETHODIMP SetData(FORMATETC*, STGMEDIUM*, BOOL) override { return E_NOTIMPL; }
    IFACEMETHODIMP EnumFormatEtc(DWORD, IEnumFORMATETC**) override { return E_NOTIMPL; }
    IFACEMETHODIMP DAdvise(FORMATETC*, DWORD, IAdviseSink*, DWORD*) override { return OLE_E_ADVISENOTSUPPORTED; }
    IFACEMETHODIMP DUnadvise(DWORD) override { return OLE_E_ADVISENOTSUPPORTED; }
    IFACEMETHODIMP EnumDAdvise(IEnumSTATDATA**) override { return OLE_E_ADVISENOTSUPPORTED; }
    void FinishProbe() {
        if (last_owner_) {
            // Retain an observer reference so the old leaking path can be cleaned after checking it.
            const ULONG remaining = last_owner_->Release();
            if (remaining) last_owner_->Release();
            last_owner_ = nullptr;
        }
    }
    ULONG OwnerReferences() {
        last_owner_->AddRef();
        return last_owner_->Release();
    }
private:
    ULONG refs_ = 1;
    DWORD tymed_;
    bool empty_;
    unsigned& released_;
    IUnknown* last_owner_ = nullptr;
};

void TestDropMediumOwnership() {
    using namespace pulse::ui;
    const auto probe = [](DWORD tymed, bool empty, bool expected, const wchar_t* label) {
        unsigned released = 0;
        auto* object = new MediumDataObject(tymed, empty, released);
        std::vector<std::wstring> paths{L"stale"};
        const bool result = ExtractHDropPaths(object, paths);
        Check(result == expected && (expected ? paths == std::vector<std::wstring>{
                  L"C:\\fixture\\one.txt", L"C:\\fixture\\two.txt"} : paths.empty()), label);
        Check(object->OwnerReferences() == 1, L"successful GetData medium relinquishes its owner reference on every exit");
        object->FinishProbe();
        Check(released == 1, L"medium owner frees its resource exactly once");
        object->Release();
    };
    probe(TYMED_HGLOBAL, false, true, L"valid HDROP retains exact Unicode path extraction");
    probe(TYMED_HGLOBAL, true, false, L"empty HDROP is rejected and stale paths cleared");
    probe(TYMED_GDI, false, false, L"wrong medium type is rejected before interpreting its union as HDROP");
}

HANDLE renderer_entered = nullptr, renderer_release = nullptr;
std::atomic_uint renderer_calls = 0;
std::atomic<DWORD> renderer_thread = 0;
DWORD drag_thread = 0;
bool loop_observed = false;

HGLOBAL BlockedRenderer(const std::vector<std::wstring>& paths) {
    ++renderer_calls;
    renderer_thread = GetCurrentThreadId();
    SetEvent(renderer_entered);
    if (WaitForSingleObject(renderer_release, 20000) != WAIT_OBJECT_0) return nullptr;
    HGLOBAL data = GlobalAlloc(GMEM_MOVEABLE, sizeof(DWORD));
    if (data) {
        *static_cast<DWORD*>(GlobalLock(data)) = static_cast<DWORD>(paths.front().size());
        GlobalUnlock(data);
    }
    return data;
}

bool SetEffect(IDataObject* obj, const wchar_t* name, DWORD effect) {
    FORMATETC fmt{static_cast<CLIPFORMAT>(RegisterClipboardFormatW(name)), nullptr,
                  DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    STGMEDIUM medium{};
    medium.tymed = TYMED_HGLOBAL;
    medium.hGlobal = GlobalAlloc(GMEM_MOVEABLE, sizeof(DWORD));
    if (!medium.hGlobal) return false;
    *static_cast<DWORD*>(GlobalLock(medium.hGlobal)) = effect;
    GlobalUnlock(medium.hGlobal);
    const HRESULT hr = obj->SetData(&fmt, &medium, TRUE);
    if (FAILED(hr)) ReleaseStgMedium(&medium);
    return SUCCEEDED(hr);
}

HRESULT WINAPI InspectDragLoop(IDataObject* obj, IDropSource* source, DWORD allowed, DWORD* effect) {
    using namespace pulse::ui;
    bool correct = WaitForSingleObject(renderer_entered, 2000) == WAIT_OBJECT_0;
    correct &= GetCurrentThreadId() == drag_thread && renderer_thread.load() != drag_thread;
    FORMATETC shell{static_cast<CLIPFORMAT>(RegisterClipboardFormatW(CFSTR_SHELLIDLIST)), nullptr,
                    DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    STGMEDIUM medium{};
    correct &= obj->QueryGetData(&shell) == DV_E_FORMATETC && obj->GetData(&shell, &medium) == DV_E_FORMATETC;
    IEnumFORMATETC* formats = nullptr;
    correct &= SUCCEEDED(obj->EnumFormatEtc(DATADIR_GET, &formats));
    if (formats) {
        FORMATETC format{};
        while (formats->Next(1, &format, nullptr) == S_OK) correct &= format.cfFormat != shell.cfFormat;
        formats->Release();
    }
    std::vector<std::wstring> paths;
    correct &= ExtractHDropPaths(obj, paths) && paths.size() == 2 &&
        paths[0] == L"\\\\server\\share\\拖出.txt" && paths[1].size() > MAX_PATH;
    correct &= PreferredDropEffect(obj) == allowed;
    correct &= source->QueryContinueDrag(TRUE, MK_LBUTTON) == DRAGDROP_S_CANCEL &&
        source->QueryContinueDrag(FALSE, 0) == DRAGDROP_S_DROP;
    for (unsigned i = 0; i < 24; ++i) {
        auto* next = FileDataObject::Create({L"C:\\next-drag.txt"});
        correct &= next->QueryGetData(&shell) == DV_E_FORMATETC;
        next->Release();
    }
    correct &= renderer_calls.load() == 1;
    correct &= SetEffect(obj, CFSTR_PERFORMEDDROPEFFECT, DROPEFFECT_MOVE);
    correct &= PreferredDropEffect(obj) == allowed;
    loop_observed = correct;
    *effect = DROPEFFECT_NONE;
    return DRAGDROP_S_DROP;
}

HRESULT WINAPI CancelDragLoop(IDataObject* obj, IDropSource*, DWORD, DWORD* effect) {
    SetEffect(obj, CFSTR_PERFORMEDDROPEFFECT, DROPEFFECT_MOVE);
    *effect = DROPEFFECT_MOVE;
    return DRAGDROP_S_CANCEL;
}

HGLOBAL FailedRenderer(const std::vector<std::wstring>&) { return nullptr; }

HGLOBAL UnavailableBlockedRenderer(const std::vector<std::wstring>& paths) {
    if (HGLOBAL marker = BlockedRenderer(paths)) GlobalFree(marker);
    return nullptr;
}

void TestDragPreparation() {
    using namespace pulse::ui;
    renderer_entered = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    renderer_release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    testing::SetShellIdListRenderer(BlockedRenderer);
    drag_thread = GetCurrentThreadId();
    const std::vector<std::wstring> paths{L"\\\\server\\share\\拖出.txt",
        L"C:\\" + std::wstring(280, L'a') + L".txt"};
    const DWORD effect = testing::RunFileDragDrop(paths, DROPEFFECT_COPY | DROPEFFECT_MOVE, InspectDragLoop);
    Check(loop_observed && effect == DROPEFFECT_MOVE,
          L"blocked Shell preparation enters the drag loop on the UI STA with HDROP and one worker");
    Check(!testing::WaitForShellIdListWorker(0), L"returning from a drag does not join its blocked provider");
    auto* other = FileDataObject::Create({L"C:\\later-drag.txt"});
    SetEvent(renderer_release);
    Check(testing::WaitForShellIdListWorker(2000), L"abandoned provider result finishes without its data object");
    FORMATETC shell{static_cast<CLIPFORMAT>(RegisterClipboardFormatW(CFSTR_SHELLIDLIST)), nullptr,
                    DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    Check(other->QueryGetData(&shell) == DV_E_FORMATETC,
          L"a previous drag's late result never appears in the next drag");
    other->Release();
    ResetEvent(renderer_entered);
    auto* completed = FileDataObject::Create(paths);
    Check(testing::WaitForShellIdListWorker(2000), L"prepared Shell data completes");
    STGMEDIUM medium{};
    bool rendered = completed->QueryGetData(&shell) == S_OK && SUCCEEDED(completed->GetData(&shell, &medium));
    if (rendered) {
        const auto* marker = static_cast<const DWORD*>(GlobalLock(medium.hGlobal));
        rendered = marker && *marker == paths.front().size();
        if (marker) GlobalUnlock(medium.hGlobal);
        ReleaseStgMedium(&medium);
    }
    Check(rendered, L"completed Shell data is advertised and duplicated from its owning drag");
    Check(SetEffect(completed, CFSTR_PREFERREDDROPEFFECT, DROPEFFECT_COPY) &&
          PreferredDropEffect(completed) == DROPEFFECT_COPY && completed->PerformedEffect() == 0,
          L"preferred effect updates never masquerade as performed moves");
    Check(SetEffect(completed, CFSTR_LOGICALPERFORMEDDROPEFFECT, DROPEFFECT_MOVE),
          L"Shell private feedback formats are accepted");
    completed->Release();
    Check(testing::RunFileDragDrop(paths, DROPEFFECT_COPY, CancelDragLoop) == DROPEFFECT_NONE,
          L"cancelled drags discard performed-effect feedback");
    Check(testing::WaitForShellIdListWorker(2000), L"cancelled drag provider completes safely");
    testing::SetShellIdListRenderer(FailedRenderer);
    auto* unavailable = FileDataObject::Create(paths);
    Check(testing::WaitForShellIdListWorker(2000), L"failed optional Shell preparation finishes");
    std::vector<std::wstring> extracted;
    Check(unavailable->QueryGetData(&shell) == DV_E_FORMATETC &&
          ExtractHDropPaths(unavailable, extracted) && extracted == paths,
          L"Shell preparation failure leaves all HDROP paths available without false CIDA support");
    unavailable->Release();
    testing::SetShellIdListRenderer(nullptr);
    CloseHandle(renderer_entered); CloseHandle(renderer_release);
}

void TestCidaPreparation() {
    using namespace pulse::ui;
    namespace fs = std::filesystem;
    const fs::path root = fs::current_path() / L"bench_data" /
        (L"drag_pidl_" + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(GetTickCount64()));
    fs::create_directories(root / L"other");
    const std::vector<std::wstring> all_paths{
        (root / L"中文 file.txt").wstring(), (root / L"second.txt").wstring(),
        (root / L"other" / L"third.txt").wstring()};
    for (const auto& path : all_paths) {
        HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        Check(file != INVALID_HANDLE_VALUE, L"isolated PIDL fixture file exists");
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    }
    auto verify = [&](const std::vector<std::wstring>& paths, bool desktop_parent) {
        auto* data = FileDataObject::Create(paths);
        bool valid = testing::WaitForShellIdListWorker(6000);
        FORMATETC format{static_cast<CLIPFORMAT>(RegisterClipboardFormatW(CFSTR_SHELLIDLIST)), nullptr,
                        DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
        STGMEDIUM medium{};
        valid = valid && data->QueryGetData(&format) == S_OK && SUCCEEDED(data->GetData(&format, &medium));
        if (valid) {
            const auto* cida = static_cast<const CIDA*>(GlobalLock(medium.hGlobal));
            const size_t size = GlobalSize(medium.hGlobal);
            valid = cida && cida->cidl == paths.size();
            if (valid) {
                const auto* bytes = reinterpret_cast<const uint8_t*>(cida);
                const size_t parent_size = cida->aoffset[1] - cida->aoffset[0];
                valid = pulse::ipc::ValidShellPidl(bytes + cida->aoffset[0], parent_size) &&
                    (desktop_parent ? parent_size == 2 : parent_size > 2);
                for (size_t i = 0; valid && i < paths.size(); ++i) {
                    const size_t start = cida->aoffset[i + 1];
                    const size_t end = i + 1 < paths.size() ? cida->aoffset[i + 2] :
                        start + ILGetSize(reinterpret_cast<PCUIDLIST_RELATIVE>(bytes + start));
                    valid = start < end && end <= size && pulse::ipc::ValidShellPidl(bytes + start, end - start);
                    if (!valid) break;
                    PIDLIST_ABSOLUTE item = ILCombine(reinterpret_cast<PCIDLIST_ABSOLUTE>(bytes + cida->aoffset[0]),
                        reinterpret_cast<PCUIDLIST_RELATIVE>(bytes + start));
                    PWSTR path = nullptr;
                    valid = item && SUCCEEDED(SHGetNameFromIDList(item, SIGDN_FILESYSPATH, &path)) &&
                        path && paths[i] == path;
                    CoTaskMemFree(path); CoTaskMemFree(item);
                }
            }
            if (cida) GlobalUnlock(medium.hGlobal);
            ReleaseStgMedium(&medium);
        }
        data->Release();
        return valid;
    };
    Check(verify({all_paths[0]}, false), L"single Unicode drag item roundtrips its original filesystem path");
    Check(verify({all_paths[0], all_paths[1]}, false), L"same-parent CIDA children resolve relative to the shared PIDL");
    Check(verify({all_paths[0], all_paths[2]}, true), L"different-parent CIDA children resolve from the desktop PIDL");
    std::error_code error;
    fs::remove_all(root, error);
    Check(!error, L"isolated CIDA fixtures are cleaned");
}

IDropTarget* ShellDropTarget(const std::wstring& path, bool recycle = false) {
    IShellItem* item = nullptr;
    HRESULT hr;
    if (recycle) {
        PIDLIST_ABSOLUTE pidl = nullptr;
        hr = SHGetKnownFolderIDList(FOLDERID_RecycleBinFolder, 0, nullptr, &pidl);
        if (SUCCEEDED(hr)) hr = SHCreateItemFromIDList(pidl, IID_PPV_ARGS(&item));
        CoTaskMemFree(pidl);
    } else hr = SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&item));
    IDropTarget* target = nullptr;
    if (SUCCEEDED(hr) && item) {
        hr = item->BindToHandler(nullptr, BHID_SFUIObject, IID_PPV_ARGS(&target));
        item->Release();
    }
    if (FAILED(hr)) wprintf(L"Shell target HRESULT: 0x%08lX\n", static_cast<unsigned long>(hr));
    return target;
}

IDataObject* NativeShellDataObject(const std::wstring& path) {
    IShellItem* item = nullptr;
    if (FAILED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&item)))) return nullptr;
    IShellItemArray* array = nullptr;
    const HRESULT hr = SHCreateShellItemArrayFromShellItem(item, IID_PPV_ARGS(&array));
    item->Release();
    IDataObject* data = nullptr;
    if (SUCCEEDED(hr) && array) {
        array->BindToHandler(nullptr, BHID_DataObject, IID_PPV_ARGS(&data));
        array->Release();
    }
    return data;
}

void TestShellCompatibility(bool native_control = false) {
    using namespace pulse::ui;
    namespace fs = std::filesystem;
    HANDLE token = nullptr;
    DWORD integrity = 0;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        DWORD bytes = 0;
        GetTokenInformation(token, TokenIntegrityLevel, nullptr, 0, &bytes);
        std::vector<BYTE> buffer(bytes);
        if (GetTokenInformation(token, TokenIntegrityLevel, buffer.data(), bytes, &bytes)) {
            const auto* label = reinterpret_cast<const TOKEN_MANDATORY_LABEL*>(buffer.data());
            const UCHAR count = *GetSidSubAuthorityCount(label->Label.Sid);
            integrity = *GetSidSubAuthority(label->Label.Sid, count - 1);
        }
        CloseHandle(token);
    }
    wprintf(L"Shell compatibility process integrity RID: %lu\n", integrity);
    wprintf(L"Data object: %s\n", native_control ? L"native Shell comparison" : L"Pulse without CIDA");
    Check(integrity == SECURITY_MANDATORY_MEDIUM_RID,
          L"Shell compatibility test actually runs with a normal Medium token");
    if (integrity != SECURITY_MANDATORY_MEDIUM_RID) return;
    const fs::path root = fs::current_path() / L"bench_data" /
        (L"drag_drop_" + std::to_wstring(GetCurrentProcessId()));
    fs::create_directories(root / L"target");
    const auto source = (root / L"source.txt").wstring();
    HANDLE file = CreateFileW(source.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    Check(file != INVALID_HANDLE_VALUE, L"isolated Shell compatibility fixture is writable");
    if (file == INVALID_HANDLE_VALUE) return;
    CloseHandle(file);
    renderer_entered = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    renderer_release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    testing::SetShellIdListRenderer(UnavailableBlockedRenderer);
    auto* data = FileDataObject::Create({source});
    IDataObject* native_data = native_control ? NativeShellDataObject(source) : nullptr;
    if (native_control) Check(native_data != nullptr, L"native Shell data-object comparison is available");
    IDataObject* offered = native_data ? native_data : data;
    Check(WaitForSingleObject(renderer_entered, 2000) == WAIT_OBJECT_0,
          L"Pulse optional Shell preparation is held during compatibility checks");
    auto* target = ShellDropTarget((root / L"target").wstring());
    DWORD effect = DROPEFFECT_COPY | DROPEFFECT_MOVE;
    HRESULT entered = target ? target->DragEnter(offered, MK_LBUTTON | MK_CONTROL, {}, &effect) : E_FAIL;
    Check(SUCCEEDED(entered) && effect == DROPEFFECT_COPY, native_control ?
          L"Explorer folder accepts native Shell data" : L"Explorer folder accepts immediate HDROP while CIDA is blocked");
    HRESULT dropped = E_FAIL;
    if (target && SUCCEEDED(entered) && effect == DROPEFFECT_COPY) {
        effect = DROPEFFECT_COPY | DROPEFFECT_MOVE;
        dropped = target->Drop(offered, MK_CONTROL, {}, &effect);
    }
    const auto destination = root / L"target" / L"source.txt";
    const ULONGLONG start = GetTickCount64();
    while (!fs::exists(destination) && GetTickCount64() - start < 3000) {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
        Sleep(10);
    }
    wprintf(L"Explorer copy Drop HRESULT: 0x%08lX, effect: %lu, performed: %lu\n",
            static_cast<unsigned long>(dropped), effect, data->PerformedEffect());
    Check(SUCCEEDED(dropped) && fs::exists(source) && fs::exists(destination), native_control ?
          L"Explorer folder actually copies native Shell data" : L"Explorer folder actually copies the isolated HDROP file");
    if (target) target->Release();
    if (native_data) native_data->Release();
    fs::create_directories(root / L"move_target");
    const auto move_source = (root / L"move.txt").wstring();
    file = CreateFileW(move_source.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    auto* moving = FileDataObject::Create({move_source});
    IDataObject* native_moving = native_control ? NativeShellDataObject(move_source) : nullptr;
    offered = native_moving ? native_moving : moving;
    target = ShellDropTarget((root / L"move_target").wstring());
    effect = DROPEFFECT_COPY | DROPEFFECT_MOVE;
    entered = target ? target->DragEnter(offered, MK_LBUTTON, {}, &effect) : E_FAIL;
    dropped = E_FAIL;
    if (target && SUCCEEDED(entered) && effect == DROPEFFECT_MOVE) {
        effect = DROPEFFECT_COPY | DROPEFFECT_MOVE;
        dropped = target->Drop(offered, 0, {}, &effect);
    }
    const auto moved = root / L"move_target" / L"move.txt";
    const ULONGLONG move_start = GetTickCount64();
    while (!fs::exists(moved) && GetTickCount64() - move_start < 3000) {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
        Sleep(10);
    }
    wprintf(L"Explorer move Drop HRESULT: 0x%08lX, effect: %lu, performed: %lu\n",
            static_cast<unsigned long>(dropped), effect, moving->PerformedEffect());
    Check(SUCCEEDED(dropped) && !fs::exists(move_source) && fs::exists(moved), native_control ?
          L"Explorer folder actually moves native Shell data" :
          L"Explorer folder actually moves an isolated HDROP file with CIDA unavailable");
    if (target) target->Release();
    if (native_moving) native_moving->Release();
    moving->Release();
    auto* recycle = ShellDropTarget({}, true);
    effect = DROPEFFECT_COPY | DROPEFFECT_MOVE;
    entered = recycle ? recycle->DragEnter(data, MK_LBUTTON, {}, &effect) : E_FAIL;
    Check(SUCCEEDED(entered) && effect == DROPEFFECT_MOVE,
          L"Recycle Bin accepts immediate HDROP while CIDA is blocked (hover only)");
    if (recycle) { recycle->DragLeave(); recycle->Release(); }
    SetEvent(renderer_release);
    Check(testing::WaitForShellIdListWorker(2000), L"Shell compatibility blocked worker exits");
    data->Release();
    testing::SetShellIdListRenderer(nullptr);
    CloseHandle(renderer_entered); CloseHandle(renderer_release);

    auto* prepared = FileDataObject::Create({source});
    Check(testing::WaitForShellIdListWorker(3000), L"real Shell PIDLs finish in the background");
    FORMATETC shell{static_cast<CLIPFORMAT>(RegisterClipboardFormatW(CFSTR_SHELLIDLIST)), nullptr,
                    DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    STGMEDIUM medium{};
    bool valid = prepared->QueryGetData(&shell) == S_OK && SUCCEEDED(prepared->GetData(&shell, &medium));
    if (valid) {
        const auto* cida = static_cast<const CIDA*>(GlobalLock(medium.hGlobal));
        PIDLIST_ABSOLUTE combined = cida && cida->cidl == 1 ? ILCombine(
            reinterpret_cast<PCIDLIST_ABSOLUTE>(reinterpret_cast<const BYTE*>(cida) + cida->aoffset[0]),
            reinterpret_cast<PCUIDLIST_RELATIVE>(reinterpret_cast<const BYTE*>(cida) + cida->aoffset[1])) : nullptr;
        wchar_t resolved[MAX_PATH]{};
        valid = combined && SHGetPathFromIDListW(combined, resolved) && source == resolved;
        CoTaskMemFree(combined);
        if (cida) GlobalUnlock(medium.hGlobal);
        ReleaseStgMedium(&medium);
    }
    Check(valid, L"real completed CIDA resolves to the original isolated file");
    prepared->Release();
    fs::remove_all(root);
}
}

void TestTargetSession() {
    using namespace pulse::ui;
    testing::SetShellIdListRenderer(FailedRenderer);
    HWND window = CreateWindowExW(0, L"STATIC", L"drop target session fixture", WS_POPUP,
        300, 250, 200, 100, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    Check(window != nullptr, L"isolated hidden drop-target window exists");
    if (!window) { testing::SetShellIdListRenderer(nullptr); return; }
    const std::vector<std::wstring> entering_paths{L"C:\\fixture\\enter.txt"};
    const std::vector<std::wstring> dropped_paths{L"C:\\fixture\\final.txt", L"C:\\fixture\\second.txt"};
    auto* entering = FileDataObject::Create(entering_paths);
    auto* dropped = FileDataObject::Create(dropped_paths);
    Check(SetEffect(entering, CFSTR_PREFERREDDROPEFFECT, DROPEFFECT_MOVE) &&
          SetEffect(dropped, CFSTR_PREFERREDDROPEFFECT, DROPEFFECT_COPY),
          L"owned entering and final data objects carry different source preferences");
    size_t overs = 0, drops = 0, leaves = 0;
    bool metadata_ready = false;
    std::vector<std::wstring> observed_sources;
    POINT observed_point{};
    DWORD observed_keys = 0, observed_allowed = 0, observed_preferred = 0;
    DropTargetCallbacks callbacks;
    const auto observe = [&](const auto& paths, POINT point, DWORD keys, DWORD allowed, DWORD preferred) {
        observed_sources = paths; observed_point = point;
        observed_keys = keys; observed_allowed = allowed; observed_preferred = preferred;
    };
    callbacks.drag_over = [&](const auto& paths, POINT point, DWORD keys, DWORD allowed, DWORD preferred) {
        ++overs; observe(paths, point, keys, allowed, preferred);
        return metadata_ready ? DROPEFFECT_MOVE : DROPEFFECT_COPY;
    };
    callbacks.drag_leave = [&] { ++leaves; };
    callbacks.drop = [&](const auto& paths, POINT point, DWORD keys, DWORD allowed, DWORD preferred) {
        ++drops; observe(paths, point, keys, allowed, preferred); return DROPEFFECT_COPY;
    };
    auto* target = new WindowDropTarget(window, std::move(callbacks));
    const auto screen = [&](LONG x, LONG y) {
        POINT point{x, y}; ClientToScreen(window, &point); return POINTL{point.x, point.y};
    };
    DWORD effect = DROPEFFECT_COPY | DROPEFFECT_MOVE;
    target->DragEnter(entering, 0, screen(5, 7), &effect);
    Check(effect == DROPEFFECT_COPY && overs == 1 && observed_sources == entering_paths,
          L"enter owns the source paths and publishes initial feedback");
    effect = DROPEFFECT_MOVE;
    target->DragOver(MK_SHIFT, screen(23, 31), &effect);
    metadata_ready = true;
    Check(target->RefreshDragFeedback() == DROPEFFECT_MOVE && overs == 3 &&
          observed_sources == entering_paths && observed_point.x == 23 && observed_point.y == 31 &&
          observed_keys == MK_SHIFT && observed_allowed == DROPEFFECT_MOVE && observed_preferred == DROPEFFECT_MOVE,
          L"metadata refresh replays the latest client point, modifiers, allowed mask and source preference");
    target->DragLeave();
    const auto after_leave = overs;
    effect = DROPEFFECT_COPY | DROPEFFECT_MOVE;
    target->DragOver(0, screen(40, 45), &effect);
    Check(target->RefreshDragFeedback() == DROPEFFECT_NONE && effect == DROPEFFECT_NONE &&
          overs == after_leave && leaves == 1,
          L"DragLeave ends the session so metadata completion cannot replay old feedback");
    metadata_ready = false;
    effect = DROPEFFECT_COPY | DROPEFFECT_MOVE;
    target->DragEnter(entering, 0, screen(8, 9), &effect);
    const auto before_drop = overs;
    effect = DROPEFFECT_COPY;
    target->Drop(dropped, MK_CONTROL, screen(42, 43), &effect);
    Check(effect == DROPEFFECT_COPY && drops == 1 && observed_sources == dropped_paths &&
          observed_point.x == 42 && observed_point.y == 43 && observed_keys == MK_CONTROL &&
          observed_allowed == DROPEFFECT_COPY && observed_preferred == DROPEFFECT_COPY &&
          dropped->PerformedEffect() == DROPEFFECT_COPY,
          L"Drop re-extracts the final owned data object and records its performed effect");
    Check(target->RefreshDragFeedback() == DROPEFFECT_NONE && overs == before_drop && leaves == 1,
          L"completed Drop retires hover context without replaying feedback or adding leave callbacks");
    target->Release(); entering->Release(); dropped->Release();
    Check(testing::WaitForShellIdListWorker(2000), L"optional data-object preparation finishes before session fixture cleanup");
    testing::SetShellIdListRenderer(nullptr);
    DestroyWindow(window);
}

int wmain(int argc, wchar_t** argv) {
    using namespace pulse::ui;
    setvbuf(stdout, nullptr, _IONBF, 0);
    OleInitialize(nullptr);
    if (argc == 2 && wcscmp(argv[1], L"--medium-only") == 0) {
        TestDropMediumOwnership();
        OleUninitialize();
        wprintf(L"drag/drop medium: %d passed, %d failed\n", passed, failed);
        return failed ? 1 : 0;
    }
    if (argc == 2 && wcscmp(argv[1], L"--target-session-only") == 0) {
        TestTargetSession();
        OleUninitialize();
        wprintf(L"drag/drop target session: %d passed, %d failed\n", passed, failed);
        return failed ? 1 : 0;
    }
    if (argc > 1 && wcscmp(argv[1], L"--cida") == 0) {
        TestCidaPreparation();
        OleUninitialize();
        wprintf(L"drag/drop CIDA: %d passed, %d failed\n", passed, failed);
        return failed ? 1 : 0;
    }
    if (argc > 1 && (wcscmp(argv[1], L"--shell-compat") == 0 || wcscmp(argv[1], L"--shell-control") == 0)) {
        TestShellCompatibility(wcscmp(argv[1], L"--shell-control") == 0);
        OleUninitialize();
        wprintf(L"drag/drop Shell compatibility: %d passed, %d failed\n", passed, failed);
        return failed ? 1 : 0;
    }
    TestDragPreparation();
    if (argc > 1 && wcscmp(argv[1], L"--drag-out") == 0) {
        OleUninitialize();
        wprintf(L"drag/drop preparation: %d passed, %d failed\n", passed, failed);
        return failed ? 1 : 0;
    }
    const DWORD both = DROPEFFECT_COPY | DROPEFFECT_MOVE;
    for (DWORD preference : {DWORD(0), DWORD(DROPEFFECT_COPY), DWORD(DROPEFFECT_MOVE), both,
                             DWORD(DROPEFFECT_COPY | DROPEFFECT_LINK)}) {
        for (DWORD keys : {DWORD(0), DWORD(MK_CONTROL), DWORD(MK_SHIFT)}) {
            const DWORD expected = keys == MK_CONTROL ? DROPEFFECT_COPY : keys == MK_SHIFT ? DROPEFFECT_MOVE :
                (preference & both) == DROPEFFECT_COPY ? DROPEFFECT_COPY : DROPEFFECT_MOVE;
            auto* data = new PreferringDataObject(preference);
            unsigned overs = 0, drops = 0;
            bool observed = true;
            DropTargetCallbacks callbacks;
            auto compute = [&](const auto& paths, POINT, DWORD state, DWORD allowed, DWORD preferred) {
                observed = observed && allowed == both && preferred == preference;
                return ComputeDropEffect(state, paths.front(), L"C:\\target", allowed, preferred);
            };
            callbacks.drag_over = [&](const auto& p, POINT pt, DWORD k, DWORD a, DWORD pref) {
                ++overs; return compute(p, pt, k, a, pref);
            };
            callbacks.drop = [&](const auto& p, POINT pt, DWORD k, DWORD a, DWORD pref) {
                ++drops; return compute(p, pt, k, a, pref);
            };
            auto* target = new WindowDropTarget(nullptr, std::move(callbacks));
            DWORD effect = both;
            target->DragEnter(data, keys, {}, &effect);
            bool correct = effect == expected;
            effect = both; target->DragOver(keys, {}, &effect); correct &= effect == expected;
            effect = both; target->Drop(data, keys, {}, &effect);
            Check(correct && effect == expected && observed && overs == 2 && drops == 1,
                  L"source preference and modifiers agree across enter, hover and drop");
            target->Release(); data->Release();
        }
    }
    {
        auto* entering = new PreferringDataObject(DROPEFFECT_MOVE);
        auto* dropped = new PreferringDataObject(DROPEFFECT_COPY);
        DWORD seen = 0;
        DropTargetCallbacks callbacks;
        callbacks.drop = [&](const auto&, POINT, DWORD, DWORD, DWORD preferred) {
            seen = preferred; return DROPEFFECT_COPY;
        };
        auto* target = new WindowDropTarget(nullptr, std::move(callbacks));
        DWORD effect = both; target->DragEnter(entering, 0, {}, &effect);
        effect = both; target->Drop(dropped, 0, {}, &effect);
        Check(seen == DROPEFFECT_COPY && effect == DROPEFFECT_COPY, L"Drop reads the final data object's preference");
        effect = 0; seen = 0; target->Drop(dropped, 0, {}, &effect);
        Check(effect == 0 && seen == 0, L"disallowed drop does not submit");
        Check(target->DragEnter(entering, 0, {}, nullptr) == E_POINTER &&
              target->DragOver(0, {}, nullptr) == E_POINTER && target->Drop(dropped, 0, {}, nullptr) == E_POINTER,
              L"drop entry points reject null effect pointers");
        target->Release(); entering->Release(); dropped->Release();
    }
    Check(ComputeDropEffect(0, L"C:\\a", L"D:\\b", both) == DROPEFFECT_COPY, L"cross-volume default copies");
    Check(ComputeDropEffect(MK_CONTROL, L"C:\\a", L"C:\\b", DROPEFFECT_MOVE) == DROPEFFECT_MOVE,
          L"source allowed mask constrains modifiers");
    for (DWORD allowed : {DWORD(DROPEFFECT_COPY), DWORD(DROPEFFECT_MOVE)}) {
        auto* data = new PreferringDataObject(DROPEFFECT_MOVE);
        bool observed = true;
        DropTargetCallbacks callbacks;
        const auto constrained = [&](const auto& paths, POINT, DWORD keys, DWORD mask, DWORD preferred) {
            observed = observed && mask == allowed;
            return ComputeDropEffect(keys, paths.front(), L"C:\\target", mask, preferred);
        };
        callbacks.drag_over = constrained;
        callbacks.drop = constrained;
        auto* target = new WindowDropTarget(nullptr, std::move(callbacks));
        DWORD effect = allowed; target->DragEnter(data, 0, {}, &effect);
        bool correct = effect == allowed;
        effect = allowed; target->DragOver(0, {}, &effect); correct &= effect == allowed;
        effect = allowed; target->Drop(data, 0, {}, &effect);
        Check(correct && effect == allowed && observed, L"source copy-only or move-only mask reaches every callback");
        target->Release(); data->Release();
    }
    Check(pulse::ops::PreferredEffectIsCut(DROPEFFECT_MOVE), L"exact MOVE is a clipboard cut");
    Check(!pulse::ops::PreferredEffectIsCut(both), L"COPY|MOVE clipboard preference never deletes originals");
    Check(!pulse::ops::PreferredEffectIsCut(DROPEFFECT_COPY), L"COPY is not a clipboard cut");
    Check(!pulse::ops::PreferredEffectIsCut(0), L"absent preference is not a clipboard cut");
    OleUninitialize();
    wprintf(L"drag/drop: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
