// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "../ipc/shell_query_client.cpp"
#include "../shell_host/shell_query_metadata.h"
#include "../common/path_utils.h"
#include "../fs/cloud_state.h"
#include <shlobj.h>
#include <shobjidl.h>
#include <shlwapi.h>
#include <shellapi.h>
#include <wrl/client.h>
#include <tlhelp32.h>
#include <filesystem>
#include <fstream>
#include <thread>
#include <iostream>
#include <set>
#include <propidl.h>

using namespace pulse::ipc;

namespace {
int failures = 0;
constexpr CLSID kPreviewFixtureClass = {
    0x467ee8c0, 0x7849, 0x41c3, {0xb7, 0x27, 0x23, 0x94, 0x79, 0x1f, 0xb1, 0x6d}
};
struct PreviewFixtureStatus {
    volatile LONG calls = 0;
    volatile LONG item_resolved = 0;
    volatile LONG objects = 0;
    DWORD provider_pid = 0;
};

class PreviewFixtureHandler final : public IInitializeWithItem {
public:
    explicit PreviewFixtureHandler(PreviewFixtureStatus* status) : status_(status) {
        InterlockedIncrement(&status_->objects);
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (iid != IID_IUnknown && iid != __uuidof(IInitializeWithItem)) return E_NOINTERFACE;
        *out = static_cast<IInitializeWithItem*>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG remaining = --references_;
        if (!remaining) delete this;
        return remaining;
    }
    HRESULT STDMETHODCALLTYPE Initialize(IShellItem* item, DWORD mode) override {
        InterlockedIncrement(&status_->calls);
        PWSTR name = nullptr;
        const HRESULT error = item && mode == STGM_READ ? item->GetDisplayName(SIGDN_FILESYSPATH, &name) : E_INVALIDARG;
        if (SUCCEEDED(error) && name && *name) InterlockedIncrement(&status_->item_resolved);
        CoTaskMemFree(name);
        return error;
    }
private:
    ~PreviewFixtureHandler() { InterlockedDecrement(&status_->objects); }
    ULONG references_ = 1;
    PreviewFixtureStatus* status_;
};

class PreviewFixtureFactory final : public IClassFactory {
public:
    explicit PreviewFixtureFactory(PreviewFixtureStatus* status) : status_(status) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (iid != IID_IUnknown && iid != IID_IClassFactory) return E_NOINTERFACE;
        *out = static_cast<IClassFactory*>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG remaining = --references_;
        if (!remaining) delete this;
        return remaining;
    }
    HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown* outer, REFIID iid, void** out) override {
        if (outer) return CLASS_E_NOAGGREGATION;
        auto* handler = new PreviewFixtureHandler(status_);
        const HRESULT result = handler->QueryInterface(iid, out);
        handler->Release();
        return result;
    }
    HRESULT STDMETHODCALLTYPE LockServer(BOOL) override { return S_OK; }
private:
    ULONG references_ = 1;
    PreviewFixtureStatus* status_;
};

int RunPreviewFixture(const std::wstring& name) {
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(initialized)) return 3;
    HANDLE mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, name.c_str());
    auto* status = mapping ? static_cast<PreviewFixtureStatus*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(PreviewFixtureStatus))) : nullptr;
    HANDLE ready = OpenEventW(EVENT_MODIFY_STATE, FALSE, (name + L"-ready").c_str());
    HANDLE stop = OpenEventW(SYNCHRONIZE, FALSE, (name + L"-stop").c_str());
    DWORD cookie = 0;
    Microsoft::WRL::ComPtr<IClassFactory> factory;
    if (status && ready && stop) {
        status->provider_pid = GetCurrentProcessId();
        factory.Attach(new PreviewFixtureFactory(status));
        if (SUCCEEDED(CoRegisterClassObject(kPreviewFixtureClass, factory.Get(), CLSCTX_LOCAL_SERVER,
            REGCLS_MULTIPLEUSE, &cookie))) {
            SetEvent(ready);
            while (MsgWaitForMultipleObjects(1, &stop, FALSE, INFINITE, QS_ALLINPUT) != WAIT_OBJECT_0) {
                MSG message{};
                while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                    TranslateMessage(&message); DispatchMessageW(&message);
                }
            }
            CoRevokeClassObject(cookie);
        }
    }
    factory.Reset();
    CoUninitialize();
    if (status) UnmapViewOfFile(status);
    if (mapping) CloseHandle(mapping);
    if (ready) CloseHandle(ready);
    if (stop) CloseHandle(stop);
    return cookie ? 0 : 4;
}
void Check(bool passed, const char* label) {
    std::cout << (passed ? "[PASS] " : "[FAIL] ") << label << '\n';
    if (!passed) ++failures;
}

void ResetHelpers(const std::wstring& executable) {
    auto& pool = Pool();
    std::lock_guard lock(pool.mutex);
    for (auto& slot : pool.slots) Retire(slot);
    pool.executable = executable;
}

size_t Helpers() {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return SIZE_MAX;
    PROCESSENTRY32W entry{sizeof(entry)};
    size_t count = 0;
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (entry.th32ParentProcessID == GetCurrentProcessId() &&
                _wcsicmp(entry.szExeFile, L"pulse_shell_query_fixture.exe") == 0) ++count;
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return count;
}

ShellQueryRequest Request(ShellQueryKind kind, std::wstring path) {
    ShellQueryRequest request;
    request.kind = kind;
    request.path = std::move(path);
    return request;
}

bool SaveLink(const std::wstring& link_path, const std::wstring& target) {
    Microsoft::WRL::ComPtr<IShellLinkW> link;
    Microsoft::WRL::ComPtr<IPersistFile> file;
    return SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link))) &&
        SUCCEEDED(link->SetPath(target.c_str())) && SUCCEEDED(link.As(&file)) &&
        SUCCEEDED(file->Save(link_path.c_str(), TRUE));
}

void ProtocolTests() {
    ShellQueryRequest parsed;
    for (const DWORD attributes : {DWORD(FILE_ATTRIBUTE_NORMAL), DWORD(FILE_ATTRIBUTE_DIRECTORY),
            DWORD(FILE_ATTRIBUTE_OFFLINE), pulse::fs::kAttrRecallOnOpen,
            pulse::fs::kAttrRecallOnDataAccess | pulse::fs::kAttrPinned}) {
        auto request = Request(ShellQueryKind::TypeName, L"C:\\fixture.txt");
        request.attributes = attributes;
        auto bytes = WriteShellQuery(request);
        Check(ReadShellQuery(bytes.data(), bytes.size(), parsed) &&
            parsed.kind == ShellQueryKind::TypeName && parsed.attributes == attributes && !parsed.use_attributes,
            "type query preserves real file and cloud attributes through protocol validation");
        request.use_attributes = true;
        bytes = WriteShellQuery(request);
        Check(!ReadShellQuery(bytes.data(), bytes.size(), parsed),
            "type query still rejects the icon-only attribute mode flag");
    }
    for (const auto kind : {ShellQueryKind::Shortcut, ShellQueryKind::DisplayName, ShellQueryKind::ParsePidl,
            ShellQueryKind::PidlName, ShellQueryKind::FolderIdentity, ShellQueryKind::InitializePreviewItem}) {
        auto request = Request(kind, L"C:\\fixture.txt");
        if (kind == ShellQueryKind::PidlName) { request.path.clear(); request.pidl = {0, 0}; }
        if (kind == ShellQueryKind::InitializePreviewItem) request.marshal_blob = {1};
        auto bytes = WriteShellQuery(request);
        Check(ReadShellQuery(bytes.data(), bytes.size(), parsed), "non-attribute query fixture is otherwise valid");
        request.attributes = FILE_ATTRIBUTE_NORMAL;
        bytes = WriteShellQuery(request);
        Check(!ReadShellQuery(bytes.data(), bytes.size(), parsed),
            "non-attribute query kinds still reject injected attributes");
        request.attributes = 0;
        request.use_attributes = true;
        bytes = WriteShellQuery(request);
        Check(!ReadShellQuery(bytes.data(), bytes.size(), parsed),
            "non-icon query kinds still reject the attribute mode flag");
    }
    auto valid = WriteShellQuery(Request(ShellQueryKind::Shortcut, L"\\\\?\\C:\\long path\\sample.lnk"));
    Check(ReadShellQuery(valid.data(), valid.size(), parsed) && parsed.kind == ShellQueryKind::Shortcut,
        "versioned shortcut query roundtrips an extended path");
    valid.push_back(0);
    Check(!ReadShellQuery(valid.data(), valid.size(), parsed), "reject trailing query bytes");
    auto invalid = Request(ShellQueryKind::TypeName, std::wstring(L"a\0b", 3));
    valid = WriteShellQuery(invalid);
    Check(!ReadShellQuery(valid.data(), valid.size(), parsed), "reject embedded NUL in query path");
    invalid = Request(ShellQueryKind::Shortcut, L"example.lnk");
    invalid.use_attributes = true;
    valid = WriteShellQuery(invalid);
    Check(!ReadShellQuery(valid.data(), valid.size(), parsed), "attribute-only mode cannot bypass shortcut parsing");
    ShellQueryResult pixels;
    pixels.error = S_OK;
    pixels.icons.push_back({1, 1, 1, 4, {1, 2, 3, 255}});
    auto payload = WriteShellQueryResult(pixels);
    ShellQueryResult decoded;
    Check(ReadShellQueryResult(payload.data(), payload.size(), decoded) && decoded.icons[0].pixels[3] == 255,
        "premultiplied pixel payload roundtrips without handles or native indices");
    pixels.icons.push_back(pixels.icons.front());
    payload = WriteShellQueryResult(pixels);
    Check(!ReadShellQueryResult(payload.data(), payload.size(), decoded), "reject duplicate icon size payloads");
    pixels.icons.resize(1); pixels.icons[0].width = UINT32_MAX;
    payload = WriteShellQueryResult(pixels);
    Check(!ReadShellQueryResult(payload.data(), payload.size(), decoded), "reject overflowing icon dimensions before allocation");
    pixels.icons[0].width = 1; pixels.icons[0].stride = 8;
    payload = WriteShellQueryResult(pixels);
    Check(!ReadShellQueryResult(payload.data(), payload.size(), decoded), "reject malformed icon stride and byte length");
}

void NamespaceProtocolTests() {
    const std::vector<uint8_t> valid_pidl{3, 0, 9, 4, 0, 8, 7, 0, 0};
    Check(ValidShellPidl(valid_pidl) && ValidShellPidl(std::vector<uint8_t>{0, 0}),
        "PIDL records support unaligned lengths and the desktop terminator");
    for (const auto& invalid : std::vector<std::vector<uint8_t>>{
        {}, {0}, {1, 0, 0, 0}, {3, 0, 9}, {4, 0, 9, 0, 0}, {0, 0, 0, 0}, {255, 255, 0, 0}}) {
        Check(!ValidShellPidl(invalid), "reject malformed, unterminated, truncated or trailing PIDL records");
    }
    std::vector<uint8_t> oversized(kShellQueryPidlLimit + 1, 0);
    Check(!ValidShellPidl(oversized), "reject PIDLs larger than the namespace byte limit");
    ShellQueryRequest request;
    request.kind = ShellQueryKind::PidlName;
    request.pidl = valid_pidl;
    ShellQueryRequest decoded_request;
    auto bytes = WriteShellQuery(request);
    Check(ReadShellQuery(bytes.data(), bytes.size(), decoded_request) && decoded_request.pidl == valid_pidl,
        "PIDL name request roundtrips only length-delimited binary data");
    request.path = L"unexpected";
    bytes = WriteShellQuery(request);
    Check(!ReadShellQuery(bytes.data(), bytes.size(), decoded_request), "reject ambiguous PIDL plus path name requests");
    request.kind = ShellQueryKind::ParsePidl;
    bytes = WriteShellQuery(request);
    Check(!ReadShellQuery(bytes.data(), bytes.size(), decoded_request), "path parser cannot accept injected PIDL data");
    ShellQueryResult result;
    result.error = S_OK;
    result.pidl = valid_pidl;
    result.text = L"cached display";
    result.filesystem_path = L"C:\\folder";
    bytes = WriteShellQueryResult(result);
    ShellQueryResult decoded;
    Check(ReadShellQueryResult(bytes.data(), bytes.size(), decoded) && decoded.pidl == valid_pidl &&
        decoded.filesystem_path == result.filesystem_path, "folder identity roundtrips cached names and validated PIDL bytes");
    bytes.pop_back();
    Check(!ReadShellQueryResult(bytes.data(), bytes.size(), decoded), "reject a truncated PIDL result before publishing it");
    result.pidl = {3, 0, 9};
    bytes = WriteShellQueryResult(result);
    Check(!ReadShellQueryResult(bytes.data(), bytes.size(), decoded), "reject missing PIDL terminator in a helper response");
    request = Request(ShellQueryKind::InitializePreviewItem, L"C:\\fixture.txt");
    bytes = WriteShellQuery(request);
    Check(!ReadShellQuery(bytes.data(), bytes.size(), decoded_request), "preview initializer requires its dedicated marshal blob");
    request.marshal_blob = {1, 2, 3};
    bytes = WriteShellQuery(request);
    Check(ReadShellQuery(bytes.data(), bytes.size(), decoded_request) && decoded_request.marshal_blob == request.marshal_blob,
        "preview marshal format uses its own bounded field independently of PIDL records");
    request.pidl = valid_pidl;
    bytes = WriteShellQuery(request);
    Check(!ReadShellQuery(bytes.data(), bytes.size(), decoded_request), "preview initializer cannot accept PIDL data in place of marshaling");
    request.pidl.clear();
    request.marshal_blob.resize(kShellQueryMarshalLimit + 1);
    bytes = WriteShellQuery(request);
    Check(!ReadShellQuery(bytes.data(), bytes.size(), decoded_request), "oversized preview marshal blobs fail before unmarshaling");
}

void NamespaceQueries(const std::filesystem::path& directory) {
    const auto file = directory / L"中文 namespace.txt";
    std::ofstream(file, std::ios::binary) << "namespace fixture";
    ShellQueryResult identity;
    const bool queried = QueryShell(Request(ShellQueryKind::FolderIdentity, directory.wstring()), identity);
    if (!queried || FAILED(identity.error))
        std::printf("folder namespace HRESULT=0x%08lX\n", static_cast<unsigned long>(identity.error));
    Check(queried &&
        SUCCEEDED(identity.error) && ValidShellPidl(identity.pidl) && !identity.text.empty() &&
        identity.filesystem_path == directory.wstring(), "helper parses folder identity and caches both COM getter names");
    ShellQueryResult extended;
    Check(QueryShell(Request(ShellQueryKind::FolderIdentity, L"\\\\?\\" + directory.wstring()), extended) &&
        SUCCEEDED(extended.error) && extended.pidl == identity.pidl &&
        extended.filesystem_path == identity.filesystem_path,
        "P14 extended folder prefix resolves the same Shell identity and filesystem name");
    ShellQueryResult item;
    Check(QueryShell(Request(ShellQueryKind::ParsePidl, file.wstring()), item) &&
        SUCCEEDED(item.error) && ValidShellPidl(item.pidl), "helper parses an actual Unicode filesystem item into PIDL bytes");
    Check(QueryShell(Request(ShellQueryKind::ParsePidl, L"\\\\?\\" + file.wstring()), extended) &&
        SUCCEEDED(extended.error) && extended.pidl == item.pidl,
        "P14 extended file prefix resolves the same Unicode item PIDL");
    Check(pulse::path::StripExtendedPathPrefix(L"\\\\?\\UNC\\server\\share\\folder") ==
        L"\\\\server\\share\\folder", "P14 extended UNC prefix retains the server and share parsing path");
    const auto long_directory = directory / std::wstring(90, L'a') / std::wstring(90, L'b') / std::wstring(70, L'c');
    const auto long_path = L"\\\\?\\" + long_directory.wstring();
    std::filesystem::create_directories(long_path);
    Check(QueryShell(Request(ShellQueryKind::FolderIdentity, long_path), extended) &&
        SUCCEEDED(extended.error) && ValidShellPidl(extended.pidl) &&
        pulse::path::StripExtendedPathPrefix(extended.filesystem_path) == long_directory.wstring(),
        "P14 existing folder beyond MAX_PATH resolves through the namespace helper");
    ShellQueryRequest name;
    name.kind = ShellQueryKind::PidlName;
    name.pidl = item.pidl;
    ShellQueryResult result;
    Check(QueryShell(name, result) && SUCCEEDED(result.error) && result.text == file.wstring(),
        "Shell selection PIDL resolves its parsing name in the helper process");
    Check(QueryShell(Request(ShellQueryKind::FolderIdentity,
        L"::{20D04FE0-3AEA-1069-A2D8-08002B30309D}"), result) && SUCCEEDED(result.error) &&
        ValidShellPidl(result.pidl) && !result.text.empty() && result.filesystem_path.empty(),
        "This PC identity preserves virtual-folder empty filesystem URL behavior");
    Check(QueryShell(Request(ShellQueryKind::ParsePidl, (directory / L"missing-item").wstring()), result) &&
        FAILED(result.error) && result.pidl.empty(), "missing namespace item stays a parse error without fabricated PIDL data");
    Check(!QueryShell(Request(ShellQueryKind::ParsePidl, L":pulse-shell-query-invalid-pidl:"), result) &&
        result.error == HRESULT_FROM_WIN32(ERROR_INVALID_DATA) && result.pidl.empty(),
        "malformed helper PIDL is rejected and the process retired");
    Check(QueryShell(name, result) && SUCCEEDED(result.error), "namespace query recovers after an invalid helper response");
    const auto started = GetTickCount64();
    Check(!QueryShell(Request(ShellQueryKind::ParsePidl, L":pulse-shell-query-hang:"), result, {}, 200) &&
        result.error == HRESULT_FROM_WIN32(ERROR_TIMEOUT) && GetTickCount64() - started < 1700,
        "hung namespace parser is terminated within the whole-request deadline");
    Check(QueryShell(name, result) && SUCCEEDED(result.error), "namespace helper recovers after a provider timeout");
    Check(!QueryShell(name, result, [] { return true; }) &&
        result.error == HRESULT_FROM_WIN32(ERROR_CANCELLED) && result.text.empty(),
        "cancelled namespace query cannot publish stale name or PIDL data");
}

void PreviewItemQueries(const std::filesystem::path& directory) {
    const auto name = L"Local\\PulsePreviewItemFixture-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
        std::to_wstring(GetTickCount64());
    struct Fixture {
        HANDLE mapping = nullptr, ready = nullptr, stop = nullptr, process = nullptr;
        PreviewFixtureStatus* status = nullptr;
        ~Fixture() {
            if (stop) SetEvent(stop);
            if (process) {
                if (WaitForSingleObject(process, 2000) != WAIT_OBJECT_0) {
                    TerminateProcess(process, ERROR_CANCELLED); WaitForSingleObject(process, 1000);
                }
                CloseHandle(process);
            }
            if (status) UnmapViewOfFile(status);
            if (mapping) CloseHandle(mapping);
            if (ready) CloseHandle(ready);
            if (stop) CloseHandle(stop);
        }
    } fixture;
    fixture.mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
        sizeof(PreviewFixtureStatus), name.c_str());
    fixture.ready = CreateEventW(nullptr, TRUE, FALSE, (name + L"-ready").c_str());
    fixture.stop = CreateEventW(nullptr, TRUE, FALSE, (name + L"-stop").c_str());
    fixture.status = fixture.mapping ? static_cast<PreviewFixtureStatus*>(MapViewOfFile(fixture.mapping,
        FILE_MAP_ALL_ACCESS, 0, 0, sizeof(PreviewFixtureStatus))) : nullptr;
    wchar_t module[32768]{};
    const DWORD length = GetModuleFileNameW(nullptr, module, ARRAYSIZE(module));
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    auto command = L"\"" + std::wstring(module, length) + L"\" --preview-provider \"" + name + L"\"";
    const bool launched = fixture.status && fixture.ready && fixture.stop && length &&
        CreateProcessW(module, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);
    if (launched) { fixture.process = process.hProcess; CloseHandle(process.hThread); }
    Check(launched && WaitForSingleObject(fixture.ready, 5000) == WAIT_OBJECT_0 &&
        fixture.status->provider_pid != GetCurrentProcessId(), "preview fixture supplies a real separate-process LOCAL_SERVER COM object");
    if (!launched || WaitForSingleObject(fixture.ready, 0) != WAIT_OBJECT_0) return;
    Microsoft::WRL::ComPtr<IUnknown> handler;
    const HRESULT created = CoCreateInstance(kPreviewFixtureClass, nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&handler));
    if (FAILED(created)) std::printf("preview fixture activation HRESULT=0x%08lX\n", static_cast<unsigned long>(created));
    Check(SUCCEEDED(created) && handler, "activate only the fixture's existing LOCAL_SERVER handler");
    if (!handler) return;
    const auto file = directory / L"preview item.txt";
    std::ofstream(file) << "preview fixture";
    {
        Microsoft::WRL::ComPtr<IStream> stream;
        HRESULT error = CreateStreamOnHGlobal(nullptr, TRUE, &stream);
        if (SUCCEEDED(error)) error = CoMarshalInterface(stream.Get(), IID_IUnknown, handler.Get(), MSHCTX_LOCAL,
            nullptr, MSHLFLAGS_TABLESTRONG);
        Check(SUCCEEDED(error), "marshal an existing preview proxy with one table-strong ownership record");
        if (FAILED(error)) return;
        struct TableRelease {
            IStream* stream;
            ~TableRelease() { stream->Seek({}, STREAM_SEEK_SET, nullptr); CoReleaseMarshalData(stream); }
        } release{stream.Get()};
        STATSTG stat{};
        error = stream->Stat(&stat, STATFLAG_NONAME);
        if (FAILED(error) || !stat.cbSize.QuadPart || stat.cbSize.QuadPart > kShellQueryMarshalLimit) {
            Check(false, "preview marshal size fits its dedicated IPC limit"); return;
        }
        ShellQueryRequest request;
        request.kind = ShellQueryKind::InitializePreviewItem;
        request.path = file.wstring();
        request.marshal_blob.resize(static_cast<size_t>(stat.cbSize.QuadPart));
        stream->Seek({}, STREAM_SEEK_SET, nullptr);
        ULONG read = 0;
        error = stream->Read(request.marshal_blob.data(), static_cast<ULONG>(request.marshal_blob.size()), &read);
        Check(SUCCEEDED(error) && read == request.marshal_blob.size(), "copy only bounded COM marshal bytes into the request");
        if (FAILED(error)) return;
        handler.Reset();
        Check(fixture.status->objects == 1, "table-strong owner retains the provider after the original proxy is released");
        ShellQueryResult result;
        const bool initialized = QueryShell(request, result);
        if (!initialized || FAILED(result.error))
            std::printf("preview item query HRESULT=0x%08lX\n", static_cast<unsigned long>(result.error));
        Check(initialized && SUCCEEDED(result.error) && result.text.empty() && result.pidl.empty() &&
            fixture.status->calls == 1 && fixture.status->item_resolved == 1,
            "helper creates the Shell item and initializes the external provider with an HRESULT-only reply");
        request.path = L"\\\\?\\" + file.wstring();
        Check(QueryShell(request, result) && SUCCEEDED(result.error) && fixture.status->calls == 2 &&
            fixture.status->item_resolved == 2,
            "P14 preview initialization resolves an extended-prefixed file in the external provider");
        const auto long_directory = directory / std::wstring(90, L'a') / std::wstring(90, L'b') / std::wstring(70, L'c');
        std::filesystem::create_directories(L"\\\\?\\" + long_directory.wstring());
        request.path = L"\\\\?\\" + (long_directory / L"preview item.txt").wstring();
        std::ofstream(std::filesystem::path(request.path)) << "preview fixture";
        Check(QueryShell(request, result) && SUCCEEDED(result.error) && fixture.status->calls == 3 &&
            fixture.status->item_resolved == 3,
            "P14 preview initialization resolves an existing file beyond MAX_PATH");
        const LONG before = fixture.status->calls;
        request.path = L":pulse-shell-preview-lost:" + file.wstring();
        Check(!QueryShell(request, result) && fixture.status->calls == before + 1,
            "lost preview reply never replays initialization after the helper crashes");
        request.path = file.wstring();
        Check(QueryShell(request, result) && SUCCEEDED(result.error) && fixture.status->calls == before + 2,
            "preview marshal remains valid for recovery until its original STA releases the table");
        const LONG cancelled_before = fixture.status->calls;
        Check(!QueryShell(request, result, [] { return true; }) &&
            result.error == HRESULT_FROM_WIN32(ERROR_CANCELLED) && fixture.status->calls == cancelled_before,
            "cancelled preview initialization cannot invoke the provider");
    }
    const auto deadline = GetTickCount64() + 2000;
    while (fixture.status->objects && GetTickCount64() < deadline) Sleep(10);
    Check(fixture.status->objects == 0, "releasing marshal table data reclaims the external preview object exactly once");
}

void LongShortcutBaseline(const std::filesystem::path& directory) {
    const auto target = directory / L"target.txt";
    std::ofstream(target) << "fixture";
    const auto folder = directory / std::wstring(90, L'a') / std::wstring(90, L'b') / std::wstring(70, L'c');
    std::filesystem::create_directories(L"\\\\?\\" + folder.wstring());
    const auto shortcut = L"\\\\?\\" + (folder / L"extended.lnk").wstring();
    const auto short_link = directory / L"baseline.lnk";
    const bool saved = SaveLink(short_link.wstring(), target.wstring());
    Check(saved && CopyFileW(short_link.c_str(), shortcut.c_str(), TRUE),
        "copy a valid shortcut fixture to a long filesystem path");
    Check(GetFileAttributesW(shortcut.c_str()) != INVALID_FILE_ATTRIBUTES, "long-path shortcut fixture exists on disk");
    ShellQueryResult result;
    pulse::shell::QueryShellShortcut(Request(ShellQueryKind::Shortcut, shortcut), result);
    if (FAILED(result.error))
        std::printf("native long shortcut HRESULT=0x%08lX\n", static_cast<unsigned long>(result.error));
    Check(SUCCEEDED(result.error) && result.text == target.wstring() && result.target_size == 7 &&
        !result.target_is_dir, "production shortcut parser resolves long path and actual target metadata");
}

void CloudTypeQueries() {
    const auto missing = std::filesystem::absolute(std::filesystem::path(L"bench_data") /
        (L"missing-cloud-type-" + std::to_wstring(GetCurrentProcessId())));
    Check(!std::filesystem::exists(missing), "cloud metadata fixture has no file content to open");
    for (const DWORD attrs : {DWORD(FILE_ATTRIBUTE_OFFLINE), pulse::fs::kAttrRecallOnOpen,
            pulse::fs::kAttrRecallOnDataAccess, pulse::fs::kAttrPinned | pulse::fs::kAttrRecallOnDataAccess}) {
        for (const bool directory : {false, true}) {
            ShellQueryRequest request = Request(ShellQueryKind::TypeName,
                (missing / L"parent.with.dot" / (directory ? L"folder" : L"online.txt")).wstring());
            request.attributes = attrs | (directory ? FILE_ATTRIBUTE_DIRECTORY : 0);
            ShellQueryResult result;
            pulse::shell::QueryShellName(request, result);
            SHFILEINFOW expected{};
            const auto reference = SHGetFileInfoW(directory ? L"dummy" : L"dummy.txt",
                directory ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL, &expected,
                sizeof(expected), SHGFI_TYPENAME | SHGFI_USEFILEATTRIBUTES);
            Check(reference && SUCCEEDED(result.error) && result.text == expected.szTypeName,
                "cloud type metadata uses attributes without requiring a real item for every recall flag");
        }
    }
}

void RealQueries(const std::filesystem::path& directory) {
    const auto file = directory / L"中文 sample.txt";
    std::ofstream(file, std::ios::binary) << "query fixture";
    ShellQueryResult result;
    Check(QueryShell(Request(ShellQueryKind::TypeName, file.wstring()), result) &&
        SUCCEEDED(result.error) && !result.text.empty(), "real host reads file type name");
    Check(QueryShell(Request(ShellQueryKind::DisplayName, file.wstring()), result) &&
        SUCCEEDED(result.error) && result.text.find(L"中文 sample") != std::wstring::npos,
        "real host reads Unicode display name");
    Check(QueryShell(Request(ShellQueryKind::Icon, file.wstring()), result) &&
        SUCCEEDED(result.error) && result.icons.size() == 4 &&
        std::all_of(result.icons.begin(), result.icons.end(), ValidShellIconPixels),
        "real host extracts four valid native icon resolutions");
    Check(QueryShell(Request(ShellQueryKind::Icon, L"::{645FF040-5081-101B-9F08-00AA002F954E}"), result) &&
        SUCCEEDED(result.error) && !result.icons.empty(), "virtual recycle icon resolves in query host");
    auto generic = Request(ShellQueryKind::Icon, L"dummy.unknown-pulse-query");
    generic.use_attributes = true; generic.attributes = FILE_ATTRIBUTE_NORMAL;
    Check(QueryShell(generic, result) && SUCCEEDED(result.error) && !result.icons.empty(),
        "unknown extension uses attribute-only generic icon without a file");
    const auto file_link = directory / L"file.lnk";
    const auto folder_link = directory / L"folder.lnk";
    const auto dead_link = directory / L"missing.lnk";
    const bool saved = SaveLink(file_link.wstring(), file.wstring()) &&
        SaveLink(folder_link.wstring(), directory.wstring()) &&
        SaveLink(dead_link.wstring(), (directory / L"missing-target").wstring());
    Check(saved, "create isolated file, folder and missing-target shortcuts");
    if (saved) {
        Check(QueryShell(Request(ShellQueryKind::Shortcut, file_link.wstring()), result) &&
            SUCCEEDED(result.error) && result.text == file.wstring() && !result.target_is_dir && result.target_size == 13,
            "shortcut host returns actual target and native size");
        Check(QueryShell(Request(ShellQueryKind::Shortcut, folder_link.wstring()), result) &&
            SUCCEEDED(result.error) && result.target_is_dir, "shortcut host classifies folder targets");
        Check(QueryShell(Request(ShellQueryKind::Shortcut, dead_link.wstring()), result) && FAILED(result.error) &&
            result.text.empty(), "missing shortcut target is a native error without tracking or UI");
        const auto long_directory = directory / std::wstring(90, L'a') / std::wstring(90, L'b') / std::wstring(70, L'c');
        std::filesystem::create_directories(L"\\\\?\\" + long_directory.wstring());
        const auto long_link = L"\\\\?\\" + (long_directory / L"extended.lnk").wstring();
        // IPersistFile::Save can report success without creating a long-path file.
        const bool long_saved = CopyFileW(file_link.c_str(), long_link.c_str(), TRUE) != FALSE;
        Check(long_saved && GetFileAttributesW(long_link.c_str()) != INVALID_FILE_ATTRIBUTES,
            "valid long-path shortcut fixture exists on disk");
        const bool long_queried = long_saved && QueryShell(Request(ShellQueryKind::Shortcut, long_link), result);
        if (!long_saved || !long_queried || FAILED(result.error) || result.text != file.wstring())
            std::printf("long shortcut: saved=%d queried=%d HRESULT=0x%08lX path length=%zu\n",
                long_saved, long_queried, static_cast<unsigned long>(result.error), long_link.size());
        Check(long_saved && long_queried && SUCCEEDED(result.error) && result.text == file.wstring(),
            "extended long shortcut path resolves through the host");
    }
    Check(QueryShell(Request(ShellQueryKind::Icon, (directory / L"absent.exe").wstring()), result) &&
        FAILED(result.error) && result.icons.empty(), "missing exact icon does not become a generic cached success");
    const auto started = GetTickCount64();
    const bool remote = QueryShell(Request(ShellQueryKind::TypeName,
        L"\\\\localhost\\PulseQueryMissingShare\\missing.txt"), result, {}, 300);
    Check((!remote || FAILED(result.error)) && GetTickCount64() - started < 1800,
        "unavailable UNC provider is bounded by the request deadline");
}

void LifecycleTests() {
    ShellQueryResult result;
    const auto ready = Request(ShellQueryKind::TypeName, L":pulse-shell-query-ready:");
    Check(QueryShell(ready, result) && SUCCEEDED(result.error) &&
        std::stoul(result.text) != GetCurrentProcessId(), "query executes outside the client process");
    const auto pid = result.text;
    Check(QueryShell(ready, result) && result.text == pid, "idle helper is reused for successive read-only queries");
    const auto before = Pool().next_session.load();
    Check(!QueryShell(Request(ShellQueryKind::TypeName, L":pulse-shell-query-crash:"), result) &&
        Pool().next_session.load() - before <= 2, "crashing provider retries at most once and returns failure");
    Check(QueryShell(ready, result) && SUCCEEDED(result.error), "query pool recovers after a provider crash");
    auto started = GetTickCount64();
    Check(!QueryShell(Request(ShellQueryKind::TypeName, L":pulse-shell-query-fragment:"), result, {}, 200) &&
        result.error == HRESULT_FROM_WIN32(ERROR_TIMEOUT) && GetTickCount64() - started < 1700,
        "fragmented responses cannot extend the whole-request deadline");
    Check(QueryShell(ready, result), "pool recovers after partial response timeout");

    std::array<HANDLE, 4> events{};
    std::array<std::atomic<bool>, 4> cancelled{};
    std::array<ShellQueryResult, 4> replies;
    std::array<std::thread, 4> queries;
    for (size_t i = 0; i < events.size(); ++i) {
        const auto name = L"Local\\PulseQueryReady-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(i);
        events[i] = CreateEventW(nullptr, TRUE, FALSE, name.c_str());
        queries[i] = std::thread([&, i, name] {
            QueryShell(Request(ShellQueryKind::TypeName, L":pulse-shell-query-hang:" + name), replies[i],
                [&, i] { return cancelled[i].load(); }, 10000);
        });
    }
    const bool all_ready = WaitForMultipleObjects(static_cast<DWORD>(events.size()), events.data(), TRUE, 5000) == WAIT_OBJECT_0;
    Check(all_ready && Helpers() == 4, "four hung providers occupy exactly four physical helpers");
    started = GetTickCount64();
    Check(!QueryShell(ready, result, {}, 200) && result.error == HRESULT_FROM_WIN32(ERROR_TIMEOUT) &&
        GetTickCount64() - started < 1000 && Helpers() == 4, "fifth query waits with a deadline without creating a fifth helper");
    cancelled[0].store(true);
    queries[0].join();
    Check(replies[0].error == HRESULT_FROM_WIN32(ERROR_CANCELLED), "cancellation retires only the leased hung helper");
    Check(QueryShell(ready, result) && SUCCEEDED(result.error) && Helpers() <= 4,
        "a cancelled slot recovers while sibling providers remain hung");
    for (size_t i = 1; i < queries.size(); ++i) { cancelled[i].store(true); queries[i].join(); }
    for (const auto event : events) if (event) CloseHandle(event);
    Check(std::all_of(replies.begin(), replies.end(), [](const auto& reply) {
        return reply.error == HRESULT_FROM_WIN32(ERROR_CANCELLED);
    }), "all cancelled queries complete without publishing stale payloads");
    Check(!QueryShell(ready, result, []() -> bool { throw 1; }) &&
        result.error == HRESULT_FROM_WIN32(ERROR_CANCELLED), "throwing cancellation cannot abandon an OVERLAPPED operation");
}
}

int wmain(int argc, wchar_t** argv) {
    if (argc == 2 && std::wstring_view(argv[1]) == L"--protocol-only") {
        ProtocolTests();
        NamespaceProtocolTests();
        return failures ? 1 : 0;
    }
    if (argc == 3 && std::wstring_view(argv[1]) == L"--preview-provider") return RunPreviewFixture(argv[2]);
    if (argc == 2 && std::wstring_view(argv[1]) == L"--cloud-metadata-only") {
        const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (FAILED(com)) return 3;
        CloudTypeQueries();
        CoUninitialize();
        return failures ? 1 : 0;
    }
    if (argc != 2 && argc != 3) return 2;
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(com)) return 3;
    const bool baseline = std::wstring_view(argv[1]) == L"--long-fixture";
    const bool namespace_only = argc == 3 && std::wstring_view(argv[2]) == L"--namespace";
    const bool preview_only = argc == 3 && std::wstring_view(argv[2]) == L"--preview-item";
    const std::wstring executable = baseline ? L"" : std::filesystem::absolute(argv[1]).wstring();
    if (!baseline) ResetHelpers(executable);
    const auto directory = std::filesystem::temp_directory_path() /
        (L"PulseShellQueryTest-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    std::filesystem::create_directories(directory);
    try {
        if (baseline) LongShortcutBaseline(directory);
        else if (preview_only) {
            PreviewItemQueries(directory);
        } else if (namespace_only) {
            NamespaceProtocolTests();
            NamespaceQueries(directory);
        } else {
            ProtocolTests();
            NamespaceProtocolTests();
            RealQueries(directory);
            NamespaceQueries(directory);
            PreviewItemQueries(directory);
            LifecycleTests();
        }
    } catch (const std::exception& error) {
        ++failures; std::cerr << "[FAIL] fixture exception: " << error.what() << '\n';
    }
    if (!baseline) {
        ResetHelpers(executable);
        Check(Helpers() == 0, "all query helpers are physically reclaimed at test cleanup");
    }
    std::error_code error;
    std::filesystem::remove_all(L"\\\\?\\" + directory.wstring(), error);
    Check(!error, "isolated query fixtures are cleaned");
    CoUninitialize();
    return failures ? 1 : 0;
}
