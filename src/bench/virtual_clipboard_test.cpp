// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "../ops/clipboard.h"
#include "../ui/drag_drop.h"
#include <windows.h>
#include <shlobj.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <wrl/client.h>

namespace {
int failures = 0;
int checks = 0;

void Check(bool passed, const char* name) {
    ++checks;
    std::printf("[%s] %s\n", passed ? "PASS" : "FAIL", name);
    if (!passed) ++failures;
}

class VirtualFileData final : public IDataObject {
public:
    explicit VirtualFileData(std::wstring file_drop) : file_drop_(std::move(file_drop)) {}

    explicit VirtualFileData(std::string contents, bool corrupt_descriptor = false,
                             bool cancel_contents = false)
        : contents_(std::move(contents)), corrupt_descriptor_(corrupt_descriptor),
          cancel_contents_(cancel_contents) {
        descriptor_format_ = static_cast<CLIPFORMAT>(RegisterClipboardFormatW(CFSTR_FILEDESCRIPTORW));
        contents_format_ = static_cast<CLIPFORMAT>(RegisterClipboardFormatW(CFSTR_FILECONTENTS));
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (iid == IID_IUnknown || iid == IID_IDataObject) {
            *out = static_cast<IDataObject*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG refs = --refs_;
        if (!refs) delete this;
        return refs;
    }

    HRESULT STDMETHODCALLTYPE GetData(FORMATETC* format, STGMEDIUM* medium) override {
        if (!format || !medium) return E_POINTER;
        *medium = {};
        if (!file_drop_.empty()) {
            if (FAILED(QueryGetData(format))) return DV_E_FORMATETC;
            const size_t size = sizeof(DROPFILES) + (file_drop_.size() + 2) * sizeof(wchar_t);
            HGLOBAL block = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, size);
            if (!block) return E_OUTOFMEMORY;
            auto* header = static_cast<DROPFILES*>(GlobalLock(block));
            if (!header) { GlobalFree(block); return E_OUTOFMEMORY; }
            header->pFiles = sizeof(DROPFILES);
            header->fWide = TRUE;
            std::memcpy(header + 1, file_drop_.c_str(), (file_drop_.size() + 1) * sizeof(wchar_t));
            GlobalUnlock(block);
            medium->tymed = TYMED_HGLOBAL;
            medium->hGlobal = block;
            return S_OK;
        }
        if (format->cfFormat == descriptor_format_ && (format->tymed & TYMED_HGLOBAL)) {
            if (corrupt_descriptor_) {
                HGLOBAL block = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, sizeof(DWORD));
                if (!block) return E_OUTOFMEMORY;
                medium->tymed = TYMED_HGLOBAL;
                medium->hGlobal = block;
                return S_OK;
            }
            const size_t size = sizeof(FILEGROUPDESCRIPTORW);
            HGLOBAL block = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, size);
            if (!block) return E_OUTOFMEMORY;
            auto* group = static_cast<FILEGROUPDESCRIPTORW*>(GlobalLock(block));
            if (!group) { GlobalFree(block); return E_OUTOFMEMORY; }
            group->cItems = 1;
            auto& descriptor = group->fgd[0];
            descriptor.dwFlags = FD_ATTRIBUTES | FD_FILESIZE;
            descriptor.dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
            descriptor.nFileSizeLow = static_cast<DWORD>(contents_.size());
            wcscpy_s(descriptor.cFileName, L"virtual-fixture.txt");
            GlobalUnlock(block);
            medium->tymed = TYMED_HGLOBAL;
            medium->hGlobal = block;
            return S_OK;
        }
        if (format->cfFormat == contents_format_ && format->lindex == 0) {
            if (cancel_contents_) return DV_E_FORMATETC;
            if ((format->tymed & TYMED_ISTREAM) != 0) {
                HGLOBAL block = GlobalAlloc(GMEM_MOVEABLE, contents_.size());
                if (!block) return E_OUTOFMEMORY;
                if (!contents_.empty()) {
                    void* bytes = GlobalLock(block);
                    if (!bytes) { GlobalFree(block); return E_OUTOFMEMORY; }
                    std::memcpy(bytes, contents_.data(), contents_.size());
                    GlobalUnlock(block);
                }
                IStream* stream = nullptr;
                const HRESULT hr = CreateStreamOnHGlobal(block, TRUE, &stream);
                if (FAILED(hr)) { GlobalFree(block); return hr; }
                medium->tymed = TYMED_ISTREAM;
                medium->pstm = stream;
                return S_OK;
            }
            if ((format->tymed & TYMED_HGLOBAL) != 0) {
                HGLOBAL block = GlobalAlloc(GMEM_MOVEABLE, contents_.size());
                if (!block) return E_OUTOFMEMORY;
                if (!contents_.empty()) {
                    void* bytes = GlobalLock(block);
                    if (!bytes) { GlobalFree(block); return E_OUTOFMEMORY; }
                    std::memcpy(bytes, contents_.data(), contents_.size());
                    GlobalUnlock(block);
                }
                medium->tymed = TYMED_HGLOBAL;
                medium->hGlobal = block;
                return S_OK;
            }
        }
        return DV_E_FORMATETC;
    }
    bool SourceStillReadable() {
        FORMATETC format{contents_format_, nullptr, DVASPECT_CONTENT, 0, TYMED_ISTREAM | TYMED_HGLOBAL};
        STGMEDIUM medium{};
        if (FAILED(GetData(&format, &medium))) return false;
        std::string actual(contents_.size(), '\0');
        ULONG read = 0;
        bool ok = false;
        if (medium.tymed == TYMED_ISTREAM && medium.pstm) {
            ok = SUCCEEDED(medium.pstm->Read(actual.data(), static_cast<ULONG>(actual.size()), &read)) &&
                read == static_cast<ULONG>(actual.size());
        } else if (medium.tymed == TYMED_HGLOBAL && medium.hGlobal &&
                   GlobalSize(medium.hGlobal) >= actual.size()) {
            const void* bytes = GlobalLock(medium.hGlobal);
            if (bytes) {
                if (!actual.empty()) std::memcpy(actual.data(), bytes, actual.size());
                GlobalUnlock(medium.hGlobal);
                read = static_cast<ULONG>(actual.size());
                ok = true;
            }
        }
        ReleaseStgMedium(&medium);
        return ok && read == static_cast<ULONG>(contents_.size()) && actual == contents_;
    }
    HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC*, STGMEDIUM*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC* format) override {
        if (!format) return E_POINTER;
        if (!file_drop_.empty())
            return format->cfFormat == CF_HDROP && (format->tymed & TYMED_HGLOBAL) ? S_OK : DV_E_FORMATETC;
        if (format->cfFormat == descriptor_format_ && (format->tymed & TYMED_HGLOBAL)) return S_OK;
        if (format->cfFormat == contents_format_ && format->lindex == 0 &&
            (format->tymed & (TYMED_ISTREAM | TYMED_HGLOBAL)))
            return cancel_contents_ ? DV_E_FORMATETC : S_OK;
        return DV_E_FORMATETC;
    }
    HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC*, FORMATETC* out) override {
        if (out) out->ptd = nullptr;
        return DATA_S_SAMEFORMATETC;
    }
    HRESULT STDMETHODCALLTYPE SetData(FORMATETC*, STGMEDIUM*, BOOL) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD direction, IEnumFORMATETC** out) override {
        if (out) *out = nullptr;
        if (direction != DATADIR_GET || !out) return E_NOTIMPL;
        if (!file_drop_.empty()) {
            FORMATETC format{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
            return SHCreateStdEnumFmtEtc(1, &format, out);
        }
        FORMATETC formats[] = {
            {descriptor_format_, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL},
            {contents_format_, nullptr, DVASPECT_CONTENT, 0, TYMED_ISTREAM | TYMED_HGLOBAL},
        };
        return SHCreateStdEnumFmtEtc(static_cast<UINT>(std::size(formats)), formats, out);
    }
    HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC*, DWORD, IAdviseSink*, DWORD*) override { return OLE_E_ADVISENOTSUPPORTED; }
    HRESULT STDMETHODCALLTYPE DUnadvise(DWORD) override { return OLE_E_ADVISENOTSUPPORTED; }
    HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA**) override { return OLE_E_ADVISENOTSUPPORTED; }

private:
    ~VirtualFileData() = default;
    ULONG refs_ = 1;
    CLIPFORMAT descriptor_format_ = 0;
    CLIPFORMAT contents_format_ = 0;
    std::string contents_;
    std::wstring file_drop_;
    bool corrupt_descriptor_ = false;
    bool cancel_contents_ = false;
};

bool ReadEquals(const std::filesystem::path& path, const std::string& expected) {
    std::ifstream input(path, std::ios::binary);
    const std::string actual((std::istreambuf_iterator<char>(input)), {});
    return input.good() || input.eof() ? actual == expected : false;
}

void VirtualDrop(const std::filesystem::path& dir, const std::string& payload) {
    constexpr DWORD both = DROPEFFECT_COPY | DROPEFFECT_MOVE;
    const auto destination = dir / L"virtual-fixture.txt";
    HRESULT result = E_UNEXPECTED;
    unsigned completions = 0;
    pulse::ui::DropTargetCallbacks callbacks;
    callbacks.virtual_destination = [&](POINT, DWORD, DWORD allowed, DWORD) {
        return allowed == DROPEFFECT_COPY ? dir.wstring() : std::wstring{};
    };
    callbacks.virtual_complete = [&](HRESULT status) { result = status; ++completions; };
    auto* target = new pulse::ui::WindowDropTarget(nullptr, std::move(callbacks));
    auto* source = new VirtualFileData(payload);
    DWORD effect = both;
    Check(SUCCEEDED(target->DragEnter(source, 0, {}, &effect)) && effect == DROPEFFECT_COPY &&
        !std::filesystem::exists(destination), "virtual hover offers copy without downloading the file");
    effect = both;
    Check(SUCCEEDED(target->DragOver(MK_SHIFT, {}, &effect)) && effect == DROPEFFECT_NONE,
        "virtual files do not advertise move on Shift");
    effect = DROPEFFECT_MOVE;
    Check(SUCCEEDED(target->Drop(source, 0, {}, &effect)) && effect == DROPEFFECT_NONE &&
        !completions && !std::filesystem::exists(destination), "a move-only virtual drop never materializes data");
    effect = both;
    Check(SUCCEEDED(target->Drop(source, 0, {}, &effect)) && effect == DROPEFFECT_COPY &&
        completions == 1 && SUCCEEDED(result) && ReadEquals(destination, payload) && source->SourceStillReadable(),
        "virtual Drop returns copy only after cross-STA materialization and preserves the source");
    source->Release();
    std::filesystem::remove(destination);

    auto* corrupt = new VirtualFileData(payload, true);
    effect = both;
    Check(SUCCEEDED(target->Drop(corrupt, 0, {}, &effect)) && effect == DROPEFFECT_NONE &&
        completions == 2 && FAILED(result) && !std::filesystem::exists(destination),
        "malformed virtual Drop reports failure and no performed effect");
    corrupt->Release();

    auto* cancelled = new VirtualFileData(payload);
    const std::atomic_bool stop = true;
    Check(pulse::ops::PasteShellData(cancelled, dir.wstring(), DROPEFFECT_COPY, &stop) ==
        HRESULT_FROM_WIN32(ERROR_CANCELLED) && !std::filesystem::exists(destination) && cancelled->SourceStillReadable(),
        "cancelled Shell transfer keeps the virtual source and creates no destination");
    cancelled->Release();
    target->Release();
}
}

int wmain() {
    const HRESULT ole = OleInitialize(nullptr);
    if (FAILED(ole)) {
        std::fprintf(stderr, "[FAIL] OleInitialize failed: 0x%08lX\n", static_cast<unsigned long>(ole));
        return 2;
    }
    wchar_t fixture_root[32768]{};
    const DWORD fixture_length = GetEnvironmentVariableW(L"PULSE_TEST_FIXTURE_ROOT",
        fixture_root, static_cast<DWORD>(std::size(fixture_root)));
    if (fixture_length >= std::size(fixture_root)) {
        std::fprintf(stderr, "[FAIL] PULSE_TEST_FIXTURE_ROOT exceeds the supported path length\n");
        OleUninitialize();
        return 2;
    }
    const auto base = fixture_length && fixture_length < std::size(fixture_root)
        ? std::filesystem::path(fixture_root) : std::filesystem::temp_directory_path();
    const auto root = base /
        (L"PulseVirtualClipboard_" + std::to_wstring(GetCurrentProcessId()) + L"_" +
         std::to_wstring(GetTickCount64()));
    std::error_code error;
    std::filesystem::create_directories(root, error);
    if (error) {
        std::fwprintf(stderr, L"[FAIL] Cannot create virtual clipboard fixture at %ls: error %lu\n",
            root.c_str(), error.value());
        OleUninitialize();
        return 2;
    }

    const uint32_t stale_sequence = GetClipboardSequenceNumber() == 1 ? 2 : 1;
    std::vector<std::wstring> device_names;
    Check(pulse::ops::PasteShellClipboardToFolder(nullptr, nullptr, device_names, stale_sequence) ==
        HRESULT_FROM_WIN32(ERROR_CANCELLED) && device_names.empty(),
        "device paste rejects a replaced clipboard before obtaining provider data");
    Check(pulse::ops::CutPasteMovedAllPaths({L"C:\\SRC\\a.txt", L"C:\\src\\A.txt", L"C:\\src\\b.txt"},
            {L"C:\\src\\a.txt", L"C:\\src\\B.txt"}) &&
        !pulse::ops::CutPasteMovedAllPaths({L"C:\\src\\a.txt", L"C:\\src\\b.txt"},
            {L"C:\\src\\a.txt"}),
        "cut completion matches case-insensitively and requires every distinct source");

    constexpr char payload[] = "virtual file from test IDataObject";
    auto* valid = new VirtualFileData(payload);
    const bool pasted = pulse::ops::PasteShellDataForTest(valid, root.wstring(), DROPEFFECT_COPY);
    const auto destination = root / L"virtual-fixture.txt";
    Check(pasted && ReadEquals(destination, payload),
          "shell drop materializes a virtual FileGroupDescriptor/FileContents item");
    Check(valid->SourceStillReadable(), "copy drop leaves the virtual source data readable");
    valid->Release();

    std::filesystem::remove(destination, error);
    auto* cancelled = new VirtualFileData(payload, false, true);
    pulse::ops::PasteShellDataForTest(cancelled, root.wstring(), DROPEFFECT_COPY);
    cancelled->Release();
    Check(!std::filesystem::exists(destination),
          "unavailable virtual contents do not create a partial destination file");

    std::filesystem::remove(destination, error);
    auto* corrupt = new VirtualFileData(payload, true);
    pulse::ops::PasteShellDataForTest(corrupt, root.wstring(), DROPEFFECT_COPY);
    corrupt->Release();
    Check(!std::filesystem::exists(destination),
          "malformed virtual descriptor is rejected without creating a file");

    const auto same_folder_source = root / L"same-folder.txt";
    std::ofstream(same_folder_source, std::ios::binary) << payload;
    Microsoft::WRL::ComPtr<IShellItem> source_item;
    Microsoft::WRL::ComPtr<IShellItemArray> source_array;
    Microsoft::WRL::ComPtr<IDataObject> source_data;
    const bool source_ready = SUCCEEDED(SHCreateItemFromParsingName(same_folder_source.c_str(), nullptr,
        IID_PPV_ARGS(&source_item))) && SUCCEEDED(SHCreateShellItemArrayFromShellItem(source_item.Get(),
        IID_PPV_ARGS(&source_array))) && SUCCEEDED(source_array->BindToHandler(nullptr, BHID_DataObject,
        IID_PPV_ARGS(&source_data)));
    const bool same_folder_copy = source_ready && pulse::ops::PasteShellDataForTest(
        source_data.Get(), root.wstring(), DROPEFFECT_COPY);
    size_t copies = 0;
    for (const auto& file : std::filesystem::directory_iterator(root))
        if (file.is_regular_file() && ReadEquals(file.path(), payload)) ++copies;
    Check(same_folder_copy && copies == 2 && ReadEquals(same_folder_source, payload),
        "Ctrl-copy into the source folder creates a second file and preserves the original");
    const auto copied_folder = root / L"copied";
    std::filesystem::create_directory(copied_folder);
    Check(source_ready && pulse::ops::PasteShellDataForTest(source_data.Get(), copied_folder.wstring(),
        DROPEFFECT_COPY) && ReadEquals(copied_folder / same_folder_source.filename(), payload) &&
        ReadEquals(same_folder_source, payload),
        "Shell item copy returns only after destination contents are complete and keeps the source");
    const auto moved_folder = root / L"moved";
    std::filesystem::create_directory(moved_folder);
    Check(source_ready && pulse::ops::PasteShellDataForTest(source_data.Get(), moved_folder.wstring(),
        DROPEFFECT_MOVE) && ReadEquals(moved_folder / same_folder_source.filename(), payload) &&
        !std::filesystem::exists(same_folder_source),
        "only explicit cut moves a Shell item and waits for destination completion");
    source_data.Reset(); source_array.Reset(); source_item.Reset();

    const auto hdrop_source = root / L"clipboard source 中文.txt";
    std::ofstream(hdrop_source, std::ios::binary) << payload;
    Microsoft::WRL::ComPtr<IDataObject> hdrop_data;
    hdrop_data.Attach(new VirtualFileData(hdrop_source.wstring()));
    const auto hdrop_folder = root / L"hdrop";
    std::filesystem::create_directory(hdrop_folder);
    Microsoft::WRL::ComPtr<IShellItemArray> hdrop_items;
    DWORD hdrop_count = 0;
    Check(!pulse::ops::HasShellFileData(hdrop_data.Get()) &&
        SUCCEEDED(pulse::ops::ResolveShellClipboardItemsForTest(hdrop_data.Get(), &hdrop_items)) &&
        SUCCEEDED(hdrop_items->GetCount(&hdrop_count)) && hdrop_count == 1,
        "the Pulse clipboard's CF_HDROP resolves to a Shell item without a Shell ID list");
    Check(pulse::ops::PasteShellDataForTest(hdrop_data.Get(),
        hdrop_folder.wstring(), DROPEFFECT_COPY) &&
        ReadEquals(hdrop_folder / hdrop_source.filename(), payload) && ReadEquals(hdrop_source, payload),
        "the shared Shell paste copies a CF_HDROP-only source and preserves the original");
    hdrop_items.Reset(); hdrop_data.Reset();

    const auto drag_folder = root / L"drag";
    std::filesystem::create_directory(drag_folder);
    VirtualDrop(drag_folder, payload);

    error.clear();
    std::filesystem::remove_all(root, error);
    Check(!error, "temporary destination fixture cleaned up");
    std::printf("%d checks, %d failures\n", checks, failures);
    OleUninitialize();
    return failures == 0 ? 0 : 1;
}
