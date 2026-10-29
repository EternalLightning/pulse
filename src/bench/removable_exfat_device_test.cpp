#include "../index/content_config_storage.h"
#include "../index/content_scope.h"
#include "../index/content_search_session.h"
#include "../index/live_filename_search.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <objbase.h>

using namespace pulse::index;
namespace fs = std::filesystem;
namespace {
struct CheckResult { std::string name; bool passed; };
std::vector<CheckResult> checks;
void Check(bool passed, const char* name) {
    checks.push_back({name, passed});
    std::cout << (passed ? "[PASS] " : "[FAIL] ") << name << std::endl;
}
std::string Utf8(const std::wstring& value) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    std::string result(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), n, nullptr, nullptr);
    return result;
}
std::string Json(const std::string& value) {
    std::string result = "\"";
    for (const auto c : value) {
        if (c == '\\' || c == '"') result += '\\';
        if (c == '\n') result += "\\n";
        else if (c == '\r') result += "\\r";
        else result += c;
    }
    return result + '"';
}
std::string Bytes(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
bool Write(const fs::path& path, const std::string& bytes) {
    std::ofstream file(path, std::ios::binary); file << bytes; file.close(); return file.good();
}
struct Device {
    std::wstring root, filesystem, guid;
    DWORD serial = 0, bus_type = 0, descriptor_error = 0;
    bool descriptor_removable = false;
    PersistentDeviceFacts facts;
};
Device FindDevice() {
    const auto mask = GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
        if (!(mask & (1u << i))) continue;
        Device device; device.root = std::wstring(1, static_cast<wchar_t>(L'A' + i)) + L":\\";
        wchar_t filesystem[MAX_PATH]{}, guid[MAX_PATH]{};
        if (!GetVolumeInformationW(device.root.c_str(), nullptr, 0, &device.serial, nullptr, nullptr, filesystem, ARRAYSIZE(filesystem))) continue;
        if (_wcsicmp(filesystem, L"exFAT")) continue;
        device.filesystem = filesystem;
        device.facts = ProbePersistentDevice(device.root);
        if (!KnownRemovableDevice(device.facts)) continue;
        if (GetVolumeNameForVolumeMountPointW(device.root.c_str(), guid, ARRAYSIZE(guid))) device.guid = guid;
        auto path = device.guid; if (!path.empty()) path.pop_back();
        HANDLE handle = CreateFileW(path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (handle != INVALID_HANDLE_VALUE) {
            STORAGE_PROPERTY_QUERY query{}; query.PropertyId = StorageDeviceProperty; query.QueryType = PropertyStandardQuery;
            alignas(STORAGE_DEVICE_DESCRIPTOR) BYTE buffer[4096]{}; DWORD returned = 0;
            if (DeviceIoControl(handle, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query), buffer, sizeof(buffer), &returned, nullptr) && returned >= sizeof(STORAGE_DEVICE_DESCRIPTOR)) {
                const auto* descriptor = reinterpret_cast<const STORAGE_DEVICE_DESCRIPTOR*>(buffer);
                device.bus_type = descriptor->BusType; device.descriptor_removable = descriptor->RemovableMedia != FALSE;
            } else device.descriptor_error = GetLastError();
            CloseHandle(handle);
        } else device.descriptor_error = GetLastError();
        return device;
    }
    return {};
}
}
int wmain(int argc, wchar_t** argv) {
    const auto device = FindDevice();
    std::cout << "[DEVICE] root=" << Utf8(device.root) << " filesystem=" << Utf8(device.filesystem)
        << " serial=" << std::hex << device.serial << std::dec << " guid=" << Utf8(device.guid)
        << " drive_type=" << device.facts.drive_type << " online=" << device.facts.online
        << " bus_known=" << device.facts.bus_known << " removable_bus=" << device.facts.removable_bus
        << " bus_type=" << device.bus_type << " descriptor_error=" << device.descriptor_error << std::endl;
    if (argc > 1 && std::wstring(argv[1]) == L"--probe") return device.root.empty() ? 2 : 0;
    Check(!device.root.empty(), "actual online exFAT removable device discovered with Win32 APIs");
    if (device.root.empty()) return 2;
    Check(device.facts.online && KnownRemovableDevice(device.facts), "production probe classifies actual device as online removable");
    Check(!AllowsPersistentIndex(device.facts) && !PersistentIndexPathAllowed(device.root), "actual exFAT device rejects persistent indexing");
    GUID guid{}; CoCreateGuid(&guid); wchar_t unique[64]{}; StringFromGUID2(guid, unique, ARRAYSIZE(unique));
    const auto name = L"PulseExfatValidation-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::wstring(unique);
    const fs::path fixture = fs::path(device.root) / name;
    const auto private_root = fs::absolute(fs::path(L"bench_data/issue-fixes-removable") / (L"exfat-run-" + std::to_wstring(GetCurrentProcessId())));
    fs::create_directories(private_root);
    SetEnvironmentVariableW(L"LOCALAPPDATA", private_root.c_str());
    const std::string marker = Utf8(name);
    const bool created = CreateDirectoryW(fixture.c_str(), nullptr) != FALSE;
    const auto create_error = created ? ERROR_SUCCESS : GetLastError();
    Check(created, "create unique USB fixture directory without touching existing files");
    bool fixture_ok = false, cleaned = false;
    uint64_t filename_ms = 0, content_ms = 0, content_scanned = 0;
    if (created) {
        fixture_ok = Write(fixture / L".owner", marker) && CreateDirectoryW((fixture / L"nested").c_str(), nullptr);
        for (int i = 0; i < 8; ++i) fixture_ok &= Write(fixture / (L"sample" + std::to_wstring(i) + L".txt"), i == 0 ? "pulse_exfat_needle first line\n" : "unrelated text\n");
        fixture_ok &= Write(fixture / L"nested" / L"nested_match.txt", "second line\npulse_exfat_needle nested\n");
        fixture_ok &= Write(fixture / L"\u4e2d\u6587\u6587\u4ef6.txt", "\xEF\xBB\xBFpulse_exfat_needle UTF8 \xE4\xB8\xAD\xE6\x96\x87\n");
        fixture_ok &= Write(fixture / L"unrelated.log", "not a match\n");
        Check(fixture_ok, "write eleven small data files including nested and Unicode filenames");
    }
    if (fixture_ok) {
        VolumeInfo volume; volume.mount_point = device.root; volume.file_system = device.filesystem;
        volume.kind = VolumeKind::Removable; volume.online = true;
        CacheSearchDevices({volume});
        Check(CachedTransientLocalSearch(fixture.wstring()) && TransientFilenameSearchRoot(fixture.wstring()) == fixture.wstring(), "actual removable subdirectory selects temporary filename route");
        Check(!PersistentIndexPathAllowed(fixture.wstring()), "actual removable subdirectory rejects persistent indexing");
        Check(SharedContentScope({}, {volume}, {}).roots.empty(), "actual removable volume excluded from shared persistent content scope");
        Query query; query.needle = L"ext:txt"; query.path_prefix = fixture.wstring(); query.limit = 3;
        std::atomic<bool> cancelled{false}; SearchResult last; size_t publications = 0;
        const auto filename_start = GetTickCount64();
        ScanLiveFilenames(query, TransientFilenameSearchRoot(query.path_prefix), cancelled, [&](SearchResult result) { last = std::move(result); ++publications; });
        filename_ms = GetTickCount64() - filename_start;
        Check(!last.scanning && !last.incomplete && last.error == 0 && last.total == 10 && last.hits.size() == 3, "production filename scan finds ten txt files with bounded page");
        Check(publications >= 2, "filename scan publishes loading and completion");
        Check(std::all_of(last.hits.begin(), last.hits.end(), [&](const auto& hit) { return FilenameSearchCovered(hit.path, fixture.wstring()); }), "filename results remain inside unique USB fixture");
        query.offset = 9;
        ScanLiveFilenames(query, fixture.wstring() + L"\\", cancelled, [&](SearchResult result) { last = std::move(result); });
        Check(last.total == 10 && last.hits.size() == 1 && !last.incomplete, "USB filename pagination and trailing slash preserve final page");
        query.offset = 0; query.needle = L"\u4e2d\u6587";
        ScanLiveFilenames(query, fixture.wstring(), cancelled, [&](SearchResult result) { last = std::move(result); });
        Check(last.total == 1 && last.hits.size() == 1 && last.hits[0].name == L"\u4e2d\u6587\u6587\u4ef6.txt", "USB filename scan matches Unicode filename");
        publications = 0; bool terminal = false;
        ScanLiveFilenames(query, fixture.wstring(), cancelled, [&](SearchResult result) { ++publications; terminal |= !result.scanning; cancelled = true; });
        Check(publications == 1 && !terminal, "filename cancellation at initial publication prevents final results");
        cancelled = false; CacheSearchDevices({});
        Check(TransientFilenameSearchRoot(fixture.wstring()).empty(), "cleared device cache invalidates old removable route");
        ScanLiveFilenames(query, TransientFilenameSearchRoot(fixture.wstring()), cancelled, [&](SearchResult result) { last = std::move(result); });
        Check(last.error == ERROR_PATH_NOT_FOUND && last.incomplete && !last.scanning, "stale empty removable route fails without scanning current directory");
        ContentSearchRequest request; request.generation = 731; request.root = fixture.wstring(); request.indexed = false; request.subscribe = false;
        request.needle = L"pulse_exfat_needle"; request.filename_query = L"ext:txt";
        ContentSearchProgress progress; std::vector<ContentHit> hits; size_t content_updates = 0;
        const auto content_start = GetTickCount64();
        const bool searched = RunContentSearch(request, cancelled, [&](const auto& state, auto batch) {
            progress = state; ++content_updates; hits.insert(hits.end(), batch.begin(), batch.end()); return true;
        });
        content_ms = GetTickCount64() - content_start; content_scanned = progress.scanned_files;
        Check(searched && progress.done && progress.error == ERROR_SUCCESS && hits.size() == 3, "production temporary content backend returns three real USB matches");
        Check(progress.generation == 731 && !progress.live && !progress.index_revision && !progress.truncated && content_updates >= 2, "temporary content completion keeps generation without indexed subscription");
        Check(std::all_of(hits.begin(), hits.end(), [&](const auto& hit) { return FilenameSearchCovered(hit.path, fixture.wstring()) && !hit.snippet.empty(); }), "content snippets and paths originate only from fixture files");
        Check(std::any_of(hits.begin(), hits.end(), [&](const auto& hit) { return hit.name == L"\u4e2d\u6587\u6587\u4ef6.txt"; }) && std::any_of(hits.begin(), hits.end(), [&](const auto& hit) { return hit.name == L"nested_match.txt" && hit.line == 2; }), "temporary content search handles Unicode file and nested line numbers");
        size_t cancelled_hits = 0;
        const bool cancel_result = RunContentSearch(request, cancelled, [&](const auto& state, auto batch) { progress = state; cancelled_hits += batch.size(); if (!state.done) cancelled = true; return true; });
        Check(!cancel_result && cancelled_hits == 0 && progress.done && progress.error == ERROR_CANCELLED, "temporary content cancellation terminates without stale hits");
        cancelled = false;
        ContentSearchSession session(request, nullptr, 0);
        ContentSearchUpdate stale; stale.progress.generation = request.generation - 1; stale.progress.done = true; stale.hits = hits;
        Check(!session.Accept(std::move(stale)).has_value() && !session.Done(), "production content session rejects previous generation USB results");
        ContentSearchUpdate current; current.progress.generation = request.generation; current.progress.done = true; current.hits = hits;
        const auto accepted = session.Accept(std::move(current));
        Check(accepted.has_value() && accepted->hits.size() == 3 && session.Done(), "current generation accepts actual USB content results");
        const auto database = (private_root / L"private.sqlite").wstring();
        const auto sidecar = fs::path(database + L".config");
        Check(Write(sidecar, "private sentinel\n"), "private persistence rejection fixture created in project");
        const auto before = Bytes(sidecar);
        ContentIndexConfig config; config.roots = {{fixture.wstring()}};
        const bool saved = config_storage::WriteConfigFile(database, config); const auto save_error = GetLastError();
        Check(!saved && save_error == ERROR_NOT_SUPPORTED && Bytes(sidecar) == before && !fs::exists(database), "persistent USB configuration rejected before private database or sidecar mutation");
        pulse::ipc::PayloadWriter payload; content::PutConfig(payload, config);
        Check(config_storage::WriteSidecar(sidecar.wstring(), 1, payload.data()), "private legacy sidecar can represent old removable root");
        const auto legacy = Bytes(sidecar); ContentIndexConfig loaded;
        Check(config_storage::ReadConfigFile(database, loaded) && loaded.roots.empty() && Bytes(sidecar) == legacy, "legacy actual USB root filtered in memory without rewriting sidecar");
        CacheSearchDevices({});
        const bool all_passed = std::all_of(checks.begin(), checks.end(), [](const auto& check) { return check.passed; });
        if (all_passed) {
            wchar_t current_guid[MAX_PATH]{};
            const auto resolved = fs::absolute(fixture).lexically_normal();
            const bool safe = resolved.parent_path() == fs::path(device.root) && resolved.filename() == name &&
                Bytes(resolved / L".owner") == marker && GetVolumeNameForVolumeMountPointW(device.root.c_str(), current_guid, ARRAYSIZE(current_guid)) && device.guid == current_guid;
            Check(safe, "cleanup verifies exact unique fixture parent marker and volume GUID");
            if (safe) { std::error_code error; fs::remove_all(resolved, error); cleaned = !error && !fs::exists(resolved); }
            Check(cleaned, "unique USB fixture removed after passing checks");
        }
    }
    const auto passed = std::count_if(checks.begin(), checks.end(), [](const auto& check) { return check.passed; });
    const auto failed = checks.size() - static_cast<size_t>(passed);
    std::ostringstream evidence;
    evidence << "{\n  \"date\": \"2026-10-23\",\n  \"device\": {\"root\":" << Json(Utf8(device.root)) << ",\"filesystem\":" << Json(Utf8(device.filesystem))
        << ",\"serial_decimal\":" << device.serial << ",\"volume_guid\":" << Json(Utf8(device.guid)) << ",\"drive_type\":" << device.facts.drive_type
        << ",\"online\":" << device.facts.online << ",\"bus_known\":" << device.facts.bus_known << ",\"removable_bus\":" << device.facts.removable_bus
        << ",\"bus_type\":" << device.bus_type << ",\"descriptor_removable\":" << device.descriptor_removable << ",\"descriptor_error\":" << device.descriptor_error << "},\n"
        << "  \"fixture\":" << Json(Utf8(fixture.wstring())) << ",\n  \"private_root\":" << Json(Utf8(private_root.wstring()))
        << ",\n  \"create_error\":" << create_error << ",\n  \"fixture_cleaned\":" << (cleaned ? "true" : "false")
        << ",\n  \"filename_scan_ms\":" << filename_ms << ",\n  \"content_scan_ms\":" << content_ms << ",\n  \"content_scanned_files\":" << content_scanned
        << ",\n  \"passed\":" << passed << ",\n  \"failed\":" << failed << ",\n  \"checks\":[\n";
    for (size_t i = 0; i < checks.size(); ++i) evidence << "    {\"name\":" << Json(checks[i].name) << ",\"passed\":" << (checks[i].passed ? "true" : "false") << "}" << (i + 1 < checks.size() ? "," : "") << '\n';
    evidence << "  ],\n  \"limitations\":[\"Backend and policy validation only; no manual installed UI validation or physical unplug performed\",\"No whole-device scan; only eleven small fixture data files and owner marker\",\"Persistence rejection observed only at private test paths; inaccessible production index storage was not monitored\",\"Cancellation and stale generation verified deterministically at callbacks/session boundary\"]\n}\n";
    fs::create_directories(L"docs/validation/issue-confirm-2026-10-23");
    const bool written = Write(L"docs/validation/issue-confirm-2026-10-23/exfat-device-validation.json", evidence.str());
    std::cout << passed << " PASS / " << failed << " FAIL; evidence_written=" << written << std::endl;
    return failed || !written ? 1 : 0;
}
