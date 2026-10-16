#include "../ops/clipboard.h"
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
    bool corrupt_descriptor_ = false;
    bool cancel_contents_ = false;
};

bool ReadEquals(const std::filesystem::path& path, const std::string& expected) {
    std::ifstream input(path, std::ios::binary);
    const std::string actual((std::istreambuf_iterator<char>(input)), {});
    return input.good() || input.eof() ? actual == expected : false;
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

    error.clear();
    std::filesystem::remove_all(root, error);
    Check(!error, "temporary destination fixture cleaned up");
    std::printf("%d checks, %d failures\n", checks, failures);
    OleUninitialize();
    return failures == 0 ? 0 : 1;
}
