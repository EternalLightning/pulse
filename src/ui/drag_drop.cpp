// drag_drop.cpp — See drag_drop.h for the contract.
#include "drag_drop.h"
#include "../ipc/shell_query_client.h"
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shlguid.h>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <thread>
#include <vector>

namespace pulse::ui {

struct ShellIdListState {
    std::atomic<HGLOBAL> data = nullptr;
    ~ShellIdListState() {
        if (HGLOBAL value = data.load()) GlobalFree(value);
    }
};

namespace {

HGLOBAL BuildShellIdList(const std::vector<std::wstring>& paths);
std::atomic_bool shell_worker_busy = false;
using ShellIdListRenderer = HGLOBAL (*)(const std::vector<std::wstring>&);
#ifdef PULSE_DRAG_DROP_TESTING
ShellIdListRenderer shell_renderer = BuildShellIdList;
#endif

std::shared_ptr<ShellIdListState> PrepareShellIdList(const std::vector<std::wstring>& paths) {
    auto state = std::make_shared<ShellIdListState>();
    // Keep preparation bounded to one worker; optional Shell data arrives
    // asynchronously, while every drag continues to offer HDROP immediately.
    if (paths.empty() || shell_worker_busy.exchange(true)) return state;
#ifdef PULSE_DRAG_DROP_TESTING
    const auto renderer = shell_renderer;
#else
    const auto renderer = BuildShellIdList;
#endif
    try {
        std::thread([state, paths, renderer] {
            const HRESULT initialized = OleInitialize(nullptr);
            if (SUCCEEDED(initialized)) {
                try {
                    state->data.store(renderer(paths));
                } catch (...) {
                    // Shell formats are optional; the paths remain available.
                }
                OleUninitialize();
            }
            shell_worker_busy.store(false);
        }).detach();
    } catch (...) {
        shell_worker_busy.store(false);
    }
    return state;
}

UINT PreferredEffectFormat() {
    static UINT fmt = RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT);
    return fmt;
}

UINT PerformedEffectFormat() {
    static UINT fmt = RegisterClipboardFormatW(CFSTR_PERFORMEDDROPEFFECT);
    return fmt;
}

UINT FileNameWFormat() {
    static UINT fmt = RegisterClipboardFormatW(CFSTR_FILENAMEW);
    return fmt;
}

UINT FileNameFormat() {
    static UINT fmt = RegisterClipboardFormatW(CFSTR_FILENAME);
    return fmt;
}

UINT ShellIdListFormat() {
    static UINT fmt = RegisterClipboardFormatW(CFSTR_SHELLIDLIST);
    return fmt;
}

DWORD ReadDwordFormat(IDataObject* obj, CLIPFORMAT format) {
    if (!obj) return 0;
    FORMATETC fmt{ format, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
    STGMEDIUM medium{};
    if (FAILED(obj->GetData(&fmt, &medium))) return 0;
    if (medium.tymed != TYMED_HGLOBAL || !medium.hGlobal ||
        GlobalSize(medium.hGlobal) < sizeof(DWORD)) {
        ReleaseStgMedium(&medium);
        return 0;
    }
    DWORD value = 0;
    if (DWORD* p = static_cast<DWORD*>(GlobalLock(medium.hGlobal))) {
        value = *p;
        GlobalUnlock(medium.hGlobal);
    }
    ReleaseStgMedium(&medium);
    return value;
}

} // namespace

// ---------------------------------------------------------------------------
// Pure helpers
// ---------------------------------------------------------------------------
std::wstring VolumeRoot(const std::wstring& path) {
    std::wstring p = path;
    if (p.starts_with(L"\\\\?\\UNC\\")) p = L"\\\\" + p.substr(8);
    else if (p.starts_with(L"\\\\?\\")) p = p.substr(4);
    if (p.size() >= 3 && std::iswalpha(p[0]) && p[1] == L':') return p.substr(0, 3);
    if (p.size() >= 5 && p[0] == L'\\' && p[1] == L'\\') {
        auto s3 = p.find(L'\\', 2);
        if (s3 == std::wstring::npos || s3 + 1 >= p.size()) return L"";
        auto s4 = p.find(L'\\', s3 + 1);
        return p.substr(0, s4); // \\server\share
    }
    return L"";
}

DWORD ComputeDropEffect(DWORD key_state, const std::wstring& source_sample,
                        const std::wstring& dest_dir, DWORD allowed,
                        DWORD preferred_effect) {
    const bool ctrl = (key_state & MK_CONTROL) != 0;
    const bool shift = (key_state & MK_SHIFT) != 0;
    DWORD want;
    if (ctrl && !shift) {
        want = DROPEFFECT_COPY;
    } else if (shift && !ctrl) {
        want = DROPEFFECT_MOVE;
    } else if (ctrl && shift) {
        want = DROPEFFECT_LINK;
    } else {
        const DWORD preferred = preferred_effect & (DROPEFFECT_COPY | DROPEFFECT_MOVE);
        if (preferred == DROPEFFECT_COPY || preferred == DROPEFFECT_MOVE) {
            want = preferred;
        } else {
            std::wstring sv = VolumeRoot(source_sample);
            std::wstring dv = VolumeRoot(dest_dir);
            want = (!sv.empty() && sv == dv) ? DROPEFFECT_MOVE : DROPEFFECT_COPY;
        }
    }
    if (want & allowed) return want;
    // Fall back to whatever the source allows.
    if (allowed & DROPEFFECT_COPY) return DROPEFFECT_COPY;
    if (allowed & DROPEFFECT_MOVE) return DROPEFFECT_MOVE;
    if (allowed & DROPEFFECT_LINK) return DROPEFFECT_LINK;
    return DROPEFFECT_NONE;
}

std::wstring FirstDroppableFolder(const std::vector<std::wstring>& sources) {
    for (const auto& path : sources) {
        if (path.empty()) continue;
        const DWORD attrs = GetFileAttributesW(path.c_str());
        if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY))
            return path;
    }
    return {};
}

bool LooksLikeFolderShortcut(const std::wstring& path) {
    if (path.size() < 4) return false;
    return _wcsicmp(path.c_str() + (path.size() - 4), L".lnk") == 0;
}

// ---------------------------------------------------------------------------
// IDataObject inspection
// ---------------------------------------------------------------------------
bool ExtractHDropPaths(IDataObject* obj, std::vector<std::wstring>& out) {
    out.clear();
    if (!obj) return false;
    FORMATETC fmt{ CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
    STGMEDIUM medium{};
    if (FAILED(obj->GetData(&fmt, &medium)) || !medium.hGlobal) return false;
    HDROP hdrop = static_cast<HDROP>(medium.hGlobal);
    UINT count = DragQueryFileW(hdrop, 0xFFFFFFFF, nullptr, 0);
    for (UINT i = 0; i < count; ++i) {
        UINT len = DragQueryFileW(hdrop, i, nullptr, 0);
        std::wstring path(static_cast<size_t>(len) + 1, L'\0');
        const UINT copied = DragQueryFileW(hdrop, i, path.data(), len + 1);
        if (copied != len) continue;
        path.resize(len);
        out.push_back(std::move(path));
    }
    ReleaseStgMedium(&medium);
    return !out.empty();
}

DWORD PreferredDropEffect(IDataObject* obj) {
    return ReadDwordFormat(obj, static_cast<CLIPFORMAT>(PreferredEffectFormat()));
}

// ---------------------------------------------------------------------------
// Format enumerator for FileDataObject.
// ---------------------------------------------------------------------------
namespace {

class FormatEnumerator final : public IEnumFORMATETC {
public:
    explicit FormatEnumerator(bool shell_id_list) : count_(shell_id_list ? 6 : 5) {}
    IFACEMETHODIMP QueryInterface(REFIID riid, void** out) override {
        if (!out) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IEnumFORMATETC) {
            *out = static_cast<IEnumFORMATETC*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    IFACEMETHODIMP_(ULONG) AddRef() override { return ++ref_; }
    IFACEMETHODIMP_(ULONG) Release() override {
        LONG r = --ref_;
        if (r == 0) delete this;
        return r;
    }
    IFACEMETHODIMP Next(ULONG count, FORMATETC* out, ULONG* fetched) override {
        ULONG n = 0;
        while (n < count && pos_ < count_) {
            out[n] = Format(pos_);
            ++pos_;
            ++n;
        }
        if (fetched) *fetched = n;
        return n == count ? S_OK : S_FALSE;
    }
    IFACEMETHODIMP Skip(ULONG count) override {
        const ULONG remaining = pos_ < count_ ? count_ - pos_ : 0;
        ULONG skipped = (std::min)(count, remaining);
        pos_ += skipped;
        return skipped == count ? S_OK : S_FALSE;
    }
    IFACEMETHODIMP Reset() override { pos_ = 0; return S_OK; }
    IFACEMETHODIMP Clone(IEnumFORMATETC** out) override {
        if (!out) return E_POINTER;
        auto* clone = new FormatEnumerator(count_ == 6);
        clone->pos_ = pos_;
        *out = clone;
        return S_OK;
    }

private:
    static FORMATETC Format(ULONG i) {
        FORMATETC f{};
        f.dwAspect = DVASPECT_CONTENT;
        f.lindex = -1;
        f.tymed = TYMED_HGLOBAL;
        switch (i) {
        case 0: f.cfFormat = CF_HDROP; break;
        case 1: f.cfFormat = (CLIPFORMAT)PreferredEffectFormat(); break;
        case 2: f.cfFormat = (CLIPFORMAT)PerformedEffectFormat(); break;
        case 3: f.cfFormat = (CLIPFORMAT)FileNameWFormat(); break;
        case 4: f.cfFormat = (CLIPFORMAT)FileNameFormat(); break;
        default: f.cfFormat = (CLIPFORMAT)ShellIdListFormat(); break;
        }
        return f;
    }
    LONG ref_ = 1;
    ULONG pos_ = 0;
    ULONG count_ = 0;
};

} // namespace

// ---------------------------------------------------------------------------
// FileDataObject
// ---------------------------------------------------------------------------
FileDataObject::FileDataObject(const std::vector<std::wstring>& paths)
    : paths_(paths), shell_id_list_(PrepareShellIdList(paths)) {}

FileDataObject::~FileDataObject() {
    for (const auto& format : stored_formats_) GlobalFree(format.data);
}

FileDataObject* FileDataObject::Create(const std::vector<std::wstring>& paths) {
    return new FileDataObject(paths);
}

HRESULT FileDataObject::QueryInterface(REFIID riid, void** out) {
    if (!out) return E_POINTER;
    if (riid == IID_IUnknown || riid == IID_IDataObject) {
        *out = static_cast<IDataObject*>(this);
        AddRef();
        return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
}

ULONG FileDataObject::AddRef() {
    return ++ref_;
}

ULONG FileDataObject::Release() {
    LONG r = --ref_;
    if (r == 0) delete this;
    return r;
}

HGLOBAL FileDataObject::RenderHDrop() const {
    size_t chars = 1;
    for (const auto& p : paths_) chars += p.size() + 1;
    size_t bytes = sizeof(DROPFILES) + chars * sizeof(wchar_t);
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, bytes);
    if (!h) return nullptr;
    auto* df = static_cast<DROPFILES*>(GlobalLock(h));
    df->pFiles = sizeof(DROPFILES);
    df->fWide = TRUE;
    wchar_t* dst = reinterpret_cast<wchar_t*>(reinterpret_cast<uint8_t*>(df) + sizeof(DROPFILES));
    for (const auto& p : paths_) {
        std::memcpy(dst, p.c_str(), p.size() * sizeof(wchar_t));
        dst += p.size() + 1;
    }
    GlobalUnlock(h);
    return h;
}

HGLOBAL FileDataObject::RenderDword(DWORD v) const {
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, sizeof(DWORD));
    if (!h) return nullptr;
    *static_cast<DWORD*>(GlobalLock(h)) = v;
    GlobalUnlock(h);
    return h;
}

HGLOBAL FileDataObject::RenderFileNameW() const {
    if (paths_.empty()) return nullptr;
    const std::wstring& w = paths_[0];
    const SIZE_T bytes = (w.size() + 1) * sizeof(wchar_t);
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, bytes);
    if (!h) return nullptr;
    std::memcpy(GlobalLock(h), w.c_str(), bytes);
    GlobalUnlock(h);
    return h;
}

HGLOBAL FileDataObject::RenderFileNameA() const {
    if (paths_.empty()) return nullptr;
    const int n = WideCharToMultiByte(CP_ACP, 0, paths_[0].c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return nullptr;
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, static_cast<SIZE_T>(n));
    if (!h) return nullptr;
    WideCharToMultiByte(CP_ACP, 0, paths_[0].c_str(), -1,
                        static_cast<char*>(GlobalLock(h)), n, nullptr, nullptr);
    GlobalUnlock(h);
    return h;
}

HGLOBAL FileDataObject::RenderShellIdList() const {
    const HGLOBAL prepared = shell_id_list_->data.load();
    return prepared ? OleDuplicateData(prepared, static_cast<CLIPFORMAT>(ShellIdListFormat()), 0) : nullptr;
}

namespace {

HGLOBAL BuildShellIdList(const std::vector<std::wstring>& paths) {
    if (paths.empty() || paths.size() > ipc::kMaxPayload / sizeof(UINT) - 2) return nullptr;
    std::vector<std::vector<uint8_t>> pidls;
    pidls.reserve(paths.size());
    const auto deadline = GetTickCount64() + 4000;
    for (const auto& path : paths) {
        const auto now = GetTickCount64();
        if (now >= deadline) return nullptr;
        ipc::ShellQueryRequest request;
        request.kind = ipc::ShellQueryKind::ParsePidl;
        request.path = path;
        ipc::ShellQueryResult result;
        if (!ipc::QueryShell(request, result, {}, static_cast<DWORD>(deadline - now)) ||
            FAILED(result.error) || !ipc::ValidShellPidl(result.pidl)) return nullptr;
        pidls.push_back(std::move(result.pidl));
    }
    auto last_offset = [](const std::vector<uint8_t>& pidl) {
        size_t offset = 0, last = 0;
        while (offset + sizeof(uint16_t) < pidl.size()) {
            uint16_t size = 0;
            std::memcpy(&size, pidl.data() + offset, sizeof(size));
            if (!size) break;
            last = offset;
            offset += size;
        }
        return last;
    };
    const size_t parent_prefix = last_offset(pidls.front());
    std::vector<uint8_t> parent(pidls.front().begin(), pidls.front().begin() + parent_prefix);
    parent.resize(parent_prefix + sizeof(uint16_t), 0);
    bool same_parent = true;
    for (size_t i = 1; same_parent && i < pidls.size(); ++i) {
        same_parent = last_offset(pidls[i]) == parent_prefix &&
            std::equal(parent.begin(), parent.begin() + parent_prefix, pidls[i].begin());
    }
    // ILIsEqual can bind a namespace provider. Binary-identical parents use
    // relative children; otherwise the desktop parent accepts absolute PIDLs.
    const UINT cidl = static_cast<UINT>(pidls.size());
    const UINT header = static_cast<UINT>(sizeof(UINT) * (cidl + 2));
    const size_t parent_size = same_parent ? parent.size() : sizeof(uint16_t);
    if (parent_size > ipc::kMaxPayload - header) return nullptr;
    size_t total = header + parent_size;
    for (const auto& pidl : pidls) {
        const size_t size = pidl.size() - (same_parent ? parent_prefix : 0);
        if (size > ipc::kMaxPayload - total) return nullptr;
        total += size;
    }
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, total);
    if (!h) return nullptr;
    auto* base = static_cast<BYTE*>(GlobalLock(h));
    if (!base) { GlobalFree(h); return nullptr; }
    auto* ida = reinterpret_cast<CIDA*>(base);
    ida->cidl = cidl;
    UINT offset = header;
    ida->aoffset[0] = offset;
    if (same_parent) {
        const UINT sz = static_cast<UINT>(parent.size());
        std::memcpy(base + offset, parent.data(), sz);
        offset += sz;
    } else {
        offset += 2;
    }
    for (UINT i = 0; i < cidl; ++i) {
        ida->aoffset[i + 1] = offset;
        const size_t prefix = same_parent ? parent_prefix : 0;
        const UINT sz = static_cast<UINT>(pidls[i].size() - prefix);
        std::memcpy(base + offset, pidls[i].data() + prefix, sz);
        offset += sz;
    }
    GlobalUnlock(h);
    return h;
}

} // namespace

bool FileDataObject::IsSupportedFormat(const FORMATETC* fmt) const {
    if (!fmt || fmt->dwAspect != DVASPECT_CONTENT) return false;
    if ((fmt->tymed & TYMED_HGLOBAL) == 0) return false;
    return fmt->cfFormat == CF_HDROP ||
           fmt->cfFormat == (CLIPFORMAT)PreferredEffectFormat() ||
           fmt->cfFormat == (CLIPFORMAT)PerformedEffectFormat() ||
           fmt->cfFormat == (CLIPFORMAT)FileNameWFormat() ||
           fmt->cfFormat == (CLIPFORMAT)FileNameFormat() ||
           (fmt->cfFormat == (CLIPFORMAT)ShellIdListFormat() && shell_id_list_->data.load()) ||
           std::any_of(stored_formats_.begin(), stored_formats_.end(), [&](const auto& stored) {
               return stored.format == fmt->cfFormat;
           });
}

HRESULT FileDataObject::QueryGetData(FORMATETC* fmt) {
    return IsSupportedFormat(fmt) ? S_OK : DV_E_FORMATETC;
}

HRESULT FileDataObject::GetData(FORMATETC* fmt, STGMEDIUM* out) {
    if (!out) return E_POINTER;
    std::memset(out, 0, sizeof(*out));
    if (!IsSupportedFormat(fmt)) return DV_E_FORMATETC;
    HGLOBAL h = nullptr;
    if (fmt->cfFormat == CF_HDROP) {
        h = RenderHDrop();
    } else if (fmt->cfFormat == (CLIPFORMAT)PreferredEffectFormat()) {
        h = RenderDword(preferred_effect_);
    } else if (fmt->cfFormat == (CLIPFORMAT)PerformedEffectFormat()) {
        h = RenderDword(performed_effect_);
    } else if (fmt->cfFormat == (CLIPFORMAT)FileNameWFormat()) {
        h = RenderFileNameW();
    } else if (fmt->cfFormat == (CLIPFORMAT)FileNameFormat()) {
        h = RenderFileNameA();
    } else if (fmt->cfFormat == (CLIPFORMAT)ShellIdListFormat()) {
        h = RenderShellIdList();
        if (!h) return E_FAIL;
    } else {
        for (const auto& stored : stored_formats_) {
            if (stored.format == fmt->cfFormat) {
                h = OleDuplicateData(stored.data, stored.format, 0);
                break;
            }
        }
    }
    if (!h) return E_OUTOFMEMORY;
    out->tymed = TYMED_HGLOBAL;
    out->hGlobal = h;
    return S_OK;
}

HRESULT FileDataObject::SetData(FORMATETC* fmt, STGMEDIUM* medium, BOOL release) {
    if (!fmt || !medium) return E_INVALIDARG;
    if (medium->tymed != TYMED_HGLOBAL || !medium->hGlobal) return DV_E_TYMED;
    if (fmt->cfFormat == (CLIPFORMAT)PerformedEffectFormat() ||
        fmt->cfFormat == (CLIPFORMAT)PreferredEffectFormat()) {
        if (GlobalSize(medium->hGlobal) < sizeof(DWORD)) return DV_E_FORMATETC;
        auto* p = static_cast<DWORD*>(GlobalLock(medium->hGlobal));
        if (!p) return E_FAIL;
        if (fmt->cfFormat == (CLIPFORMAT)PerformedEffectFormat()) performed_effect_ = *p;
        else preferred_effect_ = *p;
        GlobalUnlock(medium->hGlobal);
    } else {
        // Explorer and the drag-image helper write private feedback formats.
        HGLOBAL copy = OleDuplicateData(medium->hGlobal, fmt->cfFormat, 0);
        if (!copy) return E_OUTOFMEMORY;
        auto found = std::find_if(stored_formats_.begin(), stored_formats_.end(), [&](const auto& stored) {
            return stored.format == fmt->cfFormat;
        });
        if (found == stored_formats_.end()) stored_formats_.push_back({fmt->cfFormat, copy});
        else {
            GlobalFree(found->data);
            found->data = copy;
        }
    }
    if (release) ReleaseStgMedium(medium);
    return S_OK;
}

HRESULT FileDataObject::EnumFormatEtc(DWORD dir, IEnumFORMATETC** out) {
    if (!out) return E_POINTER;
    if (dir != DATADIR_GET) return E_NOTIMPL;
    *out = new FormatEnumerator(shell_id_list_->data.load() != nullptr);
    return S_OK;
}

// ---------------------------------------------------------------------------
// ListDropSource
// ---------------------------------------------------------------------------
HRESULT ListDropSource::QueryInterface(REFIID riid, void** out) {
    if (!out) return E_POINTER;
    if (riid == IID_IUnknown || riid == IID_IDropSource) {
        *out = static_cast<IDropSource*>(this);
        AddRef();
        return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
}

ULONG ListDropSource::Release() {
    LONG r = --ref_;
    if (r == 0) delete this;
    return r;
}

HRESULT ListDropSource::QueryContinueDrag(BOOL escape_pressed, DWORD key_state) {
    if (escape_pressed) {
        if (esc_hook_ && esc_hook_()) return S_OK; // target consumed Esc (spring-back)
        return DRAGDROP_S_CANCEL;
    }
    if (!(key_state & (MK_LBUTTON | MK_RBUTTON))) return DRAGDROP_S_DROP;
    return S_OK;
}

namespace {

DWORD RunFileDragDrop(const std::vector<std::wstring>& paths, DWORD allowed_effects,
                      std::function<bool()> esc_consumed,
                      HRESULT (WINAPI *loop)(IDataObject*, IDropSource*, DWORD, DWORD*)) {
    if (paths.empty()) return DROPEFFECT_NONE;

    auto* data = FileDataObject::Create(paths);
    FORMATETC fmt{ (CLIPFORMAT)PreferredEffectFormat(), nullptr, DVASPECT_CONTENT, -1,
                   TYMED_HGLOBAL };
    STGMEDIUM medium{};
    medium.tymed = TYMED_HGLOBAL;
    medium.hGlobal = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, sizeof(DWORD));
    if (medium.hGlobal) {
        if (auto* value = static_cast<DWORD*>(GlobalLock(medium.hGlobal))) {
            *value = allowed_effects;
            GlobalUnlock(medium.hGlobal);
            if (FAILED(data->SetData(&fmt, &medium, TRUE))) ReleaseStgMedium(&medium);
        } else ReleaseStgMedium(&medium);
    }

    auto* source = new ListDropSource();
    source->SetEscHook(std::move(esc_consumed));
    DWORD effect = DROPEFFECT_NONE;
    const HRESULT result = loop(data, source, allowed_effects, &effect);
    if (result != DRAGDROP_S_DROP) effect = DROPEFFECT_NONE;
    else if (effect == DROPEFFECT_NONE) effect = data->PerformedEffect() & allowed_effects;
    source->Release();
    data->Release();
    return effect;
}

} // namespace

DWORD DoFileDragDrop(const std::vector<std::wstring>& paths, DWORD allowed_effects,
                     std::function<bool()> esc_consumed) {
    return RunFileDragDrop(paths, allowed_effects, std::move(esc_consumed), ::DoDragDrop);
}

#ifdef PULSE_DRAG_DROP_TESTING
namespace testing {

void SetShellIdListRenderer(ShellIdListRenderer renderer) {
    shell_renderer = renderer ? renderer : BuildShellIdList;
}

bool WaitForShellIdListWorker(DWORD timeout_ms) {
    const ULONGLONG start = GetTickCount64();
    while (shell_worker_busy.load()) {
        if (GetTickCount64() - start >= timeout_ms) return false;
        Sleep(1);
    }
    return true;
}

DWORD RunFileDragDrop(const std::vector<std::wstring>& paths, DWORD allowed, DragLoop loop) {
    return pulse::ui::RunFileDragDrop(paths, allowed, {}, loop);
}

} // namespace testing
#endif

// ---------------------------------------------------------------------------
// WindowDropTarget
// ---------------------------------------------------------------------------
WindowDropTarget::WindowDropTarget(HWND hwnd, DropTargetCallbacks cb)
    : hwnd_(hwnd), cb_(std::move(cb)) {}

HRESULT WindowDropTarget::QueryInterface(REFIID riid, void** out) {
    if (!out) return E_POINTER;
    if (riid == IID_IUnknown || riid == IID_IDropTarget) {
        *out = static_cast<IDropTarget*>(this);
        AddRef();
        return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
}

ULONG WindowDropTarget::Release() {
    LONG r = --ref_;
    if (r == 0) delete this;
    return r;
}

POINT WindowDropTarget::ClientPoint(POINTL pt) const {
    POINT p{ pt.x, pt.y };
    ScreenToClient(hwnd_, &p);
    return p;
}

HRESULT WindowDropTarget::DragEnter(IDataObject* obj, DWORD key_state, POINTL pt,
                                    DWORD* effect) {
    if (!effect) return E_POINTER;
    sources_.clear();
    preferred_ = 0;
    if (!ExtractHDropPaths(obj, sources_)) {
        *effect = DROPEFFECT_NONE;
        return S_OK;
    }
    preferred_ = PreferredDropEffect(obj);
    const DWORD allowed = *effect & (DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK);
    *effect = allowed && cb_.drag_over ? cb_.drag_over(sources_, ClientPoint(pt), key_state, allowed, preferred_) & allowed
                            : DROPEFFECT_NONE;
    return S_OK;
}

HRESULT WindowDropTarget::DragOver(DWORD key_state, POINTL pt, DWORD* effect) {
    if (!effect) return E_POINTER;
    if (sources_.empty()) {
        *effect = DROPEFFECT_NONE;
        return S_OK;
    }
    const DWORD allowed = *effect & (DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK);
    *effect = allowed && cb_.drag_over ? cb_.drag_over(sources_, ClientPoint(pt), key_state, allowed, preferred_) & allowed
                            : DROPEFFECT_NONE;
    return S_OK;
}

HRESULT WindowDropTarget::DragLeave() {
    sources_.clear();
    preferred_ = 0;
    if (cb_.drag_leave) cb_.drag_leave();
    return S_OK;
}

HRESULT WindowDropTarget::Drop(IDataObject* obj, DWORD key_state, POINTL pt, DWORD* effect) {
    if (!effect) return E_POINTER;
    const DWORD allowed = *effect & (DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK);
    std::vector<std::wstring> sources;
    if (!allowed || !ExtractHDropPaths(obj, sources)) {
        *effect = DROPEFFECT_NONE;
        DragLeave();
        return S_OK;
    }
    // The dropped object may differ from the object passed to DragEnter.
    const DWORD preferred = PreferredDropEffect(obj);
    DWORD performed = cb_.drop ? cb_.drop(sources, ClientPoint(pt), key_state, allowed, preferred) & allowed
                               : DROPEFFECT_NONE;
    *effect = performed;
    // Tell the source what we did (Explorer uses this for move semantics).
    FORMATETC fmt{ (CLIPFORMAT)PerformedEffectFormat(), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
    STGMEDIUM medium{};
    medium.tymed = TYMED_HGLOBAL;
    medium.hGlobal = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, sizeof(DWORD));
    if (medium.hGlobal) {
        if (DWORD* value = static_cast<DWORD*>(GlobalLock(medium.hGlobal))) {
            *value = performed;
            GlobalUnlock(medium.hGlobal);
            if (FAILED(obj->SetData(&fmt, &medium, TRUE))) ReleaseStgMedium(&medium);
        } else ReleaseStgMedium(&medium);
    }
    sources_.clear();
    preferred_ = 0;
    return S_OK;
}

} // namespace pulse::ui
