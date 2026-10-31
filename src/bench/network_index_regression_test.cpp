// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "../index/network_index.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <cstdio>
#include <winnetwk.h>

namespace pulse::index::testing {
struct NetworkIndexAccess {
    static void Seed(NetworkIndex& index, const std::wstring& path) {
        index.running_ = true;
        NetworkIndex::RootState root;
        root.info.path = path;
        index.roots_.push_back(std::move(root));
    }
    static bool LoadConfiguration(NetworkIndex& index) {
        std::vector<std::wstring> configured;
        return index.LoadConfiguration(configured);
    }
    static void Build(NetworkIndex& index, const std::wstring& path) {
        index.BuildRoot(path, index.generation_);
    }
    static const void* Shard(const NetworkIndex& index) { return index.roots_[0].shard.get(); }
    static void Unavailable(NetworkIndex& index, const std::wstring& path, bool building = false) {
        std::lock_guard lock(index.mu_);
        NetworkIndex::RootState root;
        root.info.path = path; root.info.building = building;
        index.roots_.push_back(std::move(root));
    }
    static void Quality(NetworkIndex& index, bool online, bool building, bool failed) {
        std::lock_guard lock(index.mu_);
        auto& info = index.roots_[0].info;
        info.online = online; info.building = building;
        info.error = failed ? L"injected synchronization failure" : L"";
    }
    static SearchResult Search(NetworkIndex& index, const std::wstring& prefix) {
        if (!index.search_thread_.joinable())
            index.search_thread_ = std::thread([&] { index.SearchLoop(); });
        static uint32_t id = 100;
        Query query; query.path_prefix = prefix; query.limit = 100;
        index.SearchAsync(query, ++id);
        SearchResult result;
        for (unsigned retry = 0; retry < 1000; ++retry) {
            if (index.TakeResult(id, result)) return result;
            Sleep(1);
        }
        result.error = ERROR_TIMEOUT; result.incomplete = true;
        return result;
    }
};
}

namespace {
using namespace pulse::index;
enum class Scan { Complete, ChildDenied, NextFailed, EmptyRoot, EmptyChild, MissingChild, RootDenied };
Scan scan = Scan::Complete;
unsigned root_next = 0;
unsigned universal_calls = 0;
int failures = 0;

void Check(bool ok, const wchar_t* name) {
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, name, -1, nullptr, 0, nullptr, nullptr);
    std::string label(static_cast<size_t>(std::max(1, bytes)), '\0');
    if (bytes > 0) WideCharToMultiByte(CP_UTF8, 0, name, -1, label.data(), bytes, nullptr, nullptr);
    std::fprintf(stdout, "[%s] %s\n", ok ? "PASS" : "FAIL", label.c_str());
    std::fflush(stdout);
    if (!ok) ++failures;
}

void Fill(WIN32_FIND_DATAW* find, const wchar_t* name, bool directory = false) {
    *find = {};
    wcscpy_s(find->cFileName, name);
    find->dwFileAttributes = directory ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
}

HANDLE WINAPI First(LPCWSTR pattern, FINDEX_INFO_LEVELS, LPVOID data,
                    FINDEX_SEARCH_OPS, LPVOID, DWORD) {
    auto* find = static_cast<WIN32_FIND_DATAW*>(data);
    const bool child = std::wstring(pattern).find(L"\\child\\*") != std::wstring::npos;
    if ((!child && (scan == Scan::EmptyRoot || scan == Scan::RootDenied)) ||
        (child && (scan == Scan::ChildDenied || scan == Scan::EmptyChild || scan == Scan::MissingChild))) {
        SetLastError(scan == Scan::EmptyRoot || scan == Scan::EmptyChild ? ERROR_FILE_NOT_FOUND :
                     scan == Scan::MissingChild ? ERROR_PATH_NOT_FOUND : ERROR_ACCESS_DENIED);
        return INVALID_HANDLE_VALUE;
    }
    if (!child) root_next = 0;
    Fill(find, child ? L"child.txt" : L"kept.txt");
    return reinterpret_cast<HANDLE>(static_cast<uintptr_t>(child ? 2 : 1));
}

BOOL WINAPI Next(HANDLE handle, LPWIN32_FIND_DATAW find) {
    if (handle == reinterpret_cast<HANDLE>(static_cast<uintptr_t>(1))) {
        if (scan == Scan::NextFailed) {
            SetLastError(ERROR_BAD_NETPATH);
            return FALSE;
        }
        if (root_next++ == 0) {
            Fill(find, L"child", true);
            return TRUE;
        }
    }
    SetLastError(ERROR_NO_MORE_FILES);
    return FALSE;
}

BOOL WINAPI Close(HANDLE) {
    // Ensure the scan captures the enumeration error before closing its handle.
    SetLastError(ERROR_SUCCESS);
    return TRUE;
}

DWORD WINAPI Universal(LPCWSTR, DWORD, LPVOID data, LPDWORD bytes) {
    ++universal_calls;
    const wchar_t* unc = L"\\\\server\\share\\folder";
    const DWORD required = static_cast<DWORD>(sizeof(UNIVERSAL_NAME_INFOW) +
                                             (wcslen(unc) + 1) * sizeof(wchar_t));
    SetLastError(ERROR_SUCCESS);
    if (!data) { *bytes = required; return ERROR_MORE_DATA; }
    if (*bytes < required) return ERROR_INSUFFICIENT_BUFFER;
    auto* info = static_cast<UNIVERSAL_NAME_INFOW*>(data);
    info->lpUniversalName = reinterpret_cast<LPWSTR>(info + 1);
    wcscpy_s(info->lpUniversalName, wcslen(unc) + 1, unc);
    return NO_ERROR;
}

DWORD WINAPI RejectUniversal(LPCWSTR, DWORD, LPVOID, LPDWORD) {
    return ERROR_NOT_CONNECTED;
}

std::vector<char> Read(const std::wstring& path) {
    std::ifstream stream(std::filesystem::path(path), std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}
}

int RunNetworkIndexRegressionTests() {
    using namespace pulse::index::testing;
    const auto directory = std::filesystem::absolute(std::filesystem::path(L"bench_data")) /
        (L"pulse-network-scan-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
         std::to_wstring(GetTickCount64()));
    if (!std::filesystem::create_directory(directory)) return 1;
    NetworkIndexHooks hooks;
    hooks.find_first = First;
    hooks.find_next = Next;
    hooks.find_close = Close;
    hooks.universal_name = Universal;
    hooks.shard_base = (directory / L"shard").wstring();
    SetNetworkIndexHooks(&hooks);
    {
        NetworkIndex index;
        const std::wstring root = L"\\\\fixture.invalid\\share";
        NetworkIndexAccess::Seed(index, root);
        NetworkIndexAccess::Build(index, root);
        const void* original = NetworkIndexAccess::Shard(index);
        auto old_path = hooks.shard_base + L"-0.bin";
        auto original_bytes = Read(old_path);
        Check(original && index.Roots()[0].indexed_items == 3 && !original_bytes.empty(),
              L"complete injected scan publishes three records");
        auto result = NetworkIndexAccess::Search(index, root);
        Check(result.total == 3 && !result.incomplete && !result.error,
              L"healthy root produces a complete query response");
        NetworkIndexAccess::Unavailable(index, L"\\\\fixture.invalid\\offline");
        result = NetworkIndexAccess::Search(index, L"");
        Check(result.total == 3 && result.incomplete && result.error == ERROR_NETWORK_UNREACHABLE,
              L"global query reports omitted offline coverage while retaining healthy hits");
        result = NetworkIndexAccess::Search(index, root);
        Check(result.total == 3 && !result.incomplete && !result.error,
              L"healthy scoped query ignores unrelated offline root");
        NetworkIndexAccess::Unavailable(index, root + L"2");
        result = NetworkIndexAccess::Search(index, root);
        Check(result.total == 3 && !result.incomplete && !result.error,
              L"scope overlap respects the root path component boundary");
        NetworkIndexAccess::Unavailable(index, L"\\\\fixture.invalid\\building", true);
        result = NetworkIndexAccess::Search(index, L"\\\\fixture.invalid\\building");
        Check(result.total == 0 && result.incomplete && result.error == ERROR_NOT_READY,
              L"first build without a shard reports incomplete coverage instead of complete empty");
        NetworkIndexAccess::Quality(index, true, true, false);
        result = NetworkIndexAccess::Search(index, root);
        Check(result.total == 3 && result.incomplete && result.error == ERROR_NOT_READY,
              L"rebuilding root retains old hits and reports in-progress coverage");
        NetworkIndexAccess::Quality(index, true, false, true);
        result = NetworkIndexAccess::Search(index, root);
        Check(result.total == 3 && result.incomplete && result.error == ERROR_READ_FAULT,
              L"failed synchronization marks retained stale shard results incomplete");
        NetworkIndexAccess::Quality(index, false, false, true);
        result = NetworkIndexAccess::Search(index, root);
        Check(result.total == 0 && result.incomplete && result.error == ERROR_NETWORK_UNREACHABLE,
              L"offline retained shard reports its missing query coverage");
        NetworkIndexAccess::Quality(index, true, false, false);
        result = NetworkIndexAccess::Search(index, root);
        Check(result.total == 3 && !result.incomplete && !result.error,
              L"successful recovery restores complete scoped coverage");
        for (const auto mode : {Scan::ChildDenied, Scan::NextFailed, Scan::MissingChild, Scan::RootDenied}) {
            scan = Scan::Complete;
            NetworkIndexAccess::Build(index, root);
            original = NetworkIndexAccess::Shard(index);
            const bool slot_zero = std::filesystem::exists(hooks.shard_base + L"-0.bin");
            old_path = hooks.shard_base + (slot_zero ? L"-0.bin" : L"-1.bin");
            original_bytes = Read(old_path);
            scan = mode;
            NetworkIndexAccess::Build(index, root);
            const auto info = index.Roots()[0];
            Check(NetworkIndexAccess::Shard(index) == original && info.indexed_items == 3 &&
                  !info.building && info.progress == 0 && !info.error.empty() &&
                  info.online == (mode != Scan::RootDenied) &&
                  (mode == Scan::RootDenied || info.state.find(L"同步失败") != std::wstring::npos) &&
                  Read(old_path) == original_bytes &&
                  !std::filesystem::exists(hooks.shard_base + (slot_zero ? L"-1.bin" : L"-0.bin")),
                  mode == Scan::ChildDenied ? L"child access denied retains old shard and disk bytes" :
                  mode == Scan::NextFailed ? L"FindNext network failure retains old shard and disk bytes" :
                  mode == Scan::MissingChild ? L"disappeared child retains old shard and disk bytes" :
                  L"root access denied retains old shard and disk bytes");
        }
        scan = Scan::EmptyChild;
        NetworkIndexAccess::Build(index, root);
        Check(index.Roots()[0].indexed_items == 2 && index.Roots()[0].progress == 100 &&
              index.Roots()[0].error.empty(), L"empty child is a complete successful scan");
        scan = Scan::EmptyRoot;
        NetworkIndexAccess::Build(index, root);
        Check(index.Roots()[0].indexed_items == 0 && index.Roots()[0].online &&
              index.Roots()[0].progress == 100 && index.Roots()[0].error.empty(),
              L"empty root publishes an online empty shard");
    }
    Check(NormalizeNetworkRoot(L"Z:\\folder") == L"\\\\server\\share\\folder" &&
          universal_calls == 2, L"mapped path uses WNet status despite unrelated LastError");
    Check(NormalizeNetworkRoot(L"\\\\?\\C:\\fixture").empty(), L"reject extended local device root");
    Check(NormalizeNetworkRoot(L"\\\\.\\C:\\fixture").empty(), L"reject Win32 local device root");
    Check(NormalizeNetworkRoot(L"\\\\?\\UNC\\server\\share\\folder") ==
          L"\\\\server\\share\\folder", L"preserve extended UNC normalization");
    SetNetworkIndexHooks(nullptr);
    std::error_code cleanup;
    std::filesystem::remove_all(directory, cleanup);
    Check(!cleanup && !std::filesystem::exists(directory), L"remove and enumerate isolated network scan fixtures");
    if (cleanup) std::fprintf(stderr, "[FAIL] network scan cleanup: error=%d %s\n",
        cleanup.value(), cleanup.message().c_str());
    return failures ? 1 : 0;
}

int RunNetworkConfigRegressionTests() {
    using namespace pulse::index;
    using namespace pulse::index::testing;
    std::fputs("[DIAG] network-config entered fixture\n", stderr);
    std::fflush(stderr);
    wchar_t executable[32768]{};
    const DWORD length = GetModuleFileNameW(nullptr, executable, ARRAYSIZE(executable));
    if (!length || length >= ARRAYSIZE(executable)) {
        std::fprintf(stderr, "[FAIL] fixture executable path: win32=%lu\n", GetLastError());
        return 1;
    }
    const auto directory = std::filesystem::path(executable).parent_path().parent_path() / L"bench_data" /
        (L"pulse-network-config-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
         std::to_wstring(GetTickCount64()));
    std::error_code directory_error;
    if (!std::filesystem::create_directories(directory, directory_error)) {
        std::fprintf(stderr, "[FAIL] create isolated network-config fixture: error=%d %s\n",
            directory_error.value(), directory_error.message().c_str());
        return 1;
    }
    const auto directory_utf8 = directory.u8string();
    std::fprintf(stderr, "[DIAG] network-config directory=%s\n",
        reinterpret_cast<const char*>(directory_utf8.c_str()));
    std::fflush(stderr);
    const auto path = (directory / L"network-index.json").wstring();
    NetworkIndexHooks hooks;
    hooks.config_path = path;
    hooks.universal_name = RejectUniversal;
    SetNetworkIndexHooks(&hooks);
    struct HookReset {
        ~HookReset() { SetNetworkIndexHooks(nullptr); }
    } reset_hooks;
    const auto write = [&](const std::string& bytes) {
        std::ofstream stream(std::filesystem::path(path), std::ios::binary | std::ios::trunc);
        stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        return stream.good();
    };
    std::vector<std::wstring> roots;
    std::wstring error = L"stale";
    NetworkConfigLoadStatus status = NetworkConfigLoadStatus::Invalid;
    Check(LoadNetworkRootsFile(path, roots, &error, &status) && roots.empty() && error.empty() &&
          status == NetworkConfigLoadStatus::MissingFile, L"absent configuration is distinct from an empty roots array");
    Check(write(R"({"roots":[]})") && LoadNetworkRootsFile(path, roots, &error, &status) &&
          roots.empty() && error.empty() && status == NetworkConfigLoadStatus::Empty,
          L"explicit empty roots loads successfully");
    const std::string unicode = R"({"version":1,"roots":["\\\\server\\share\\\u4E2D\u6587\uD83D\uDE00"]})";
    Check(write(unicode) && LoadNetworkRootsFile(path, roots, &error, &status) &&
          status == NetworkConfigLoadStatus::Loaded && roots.size() == 1 &&
          roots[0] == L"\\\\server\\share\\中文\U0001F600",
          L"Unicode escapes and surrogate pairs decode to the actual UNC path");
    for (const auto& invalid : std::vector<std::string>{
            R"({"roots":[123]})", R"({"roots":["\\server\share"])",
            R"({"roots":[])", R"({"roots":[] "other":1})", R"({"roots":["x",]})",
            R"({"roots":null})", R"({"roots":[] ,"roots":[]})", R"({"roots":[]} garbage)",
            R"({"roots":["\uD800"]})", R"({"roots":["\q"]})", R"({"roots":["\uZZZZ"]})",
            R"({"roots":["\\server\share\u0000hidden"]})", R"({"roots":["C:\local"]})",
            "", std::string("{\"roots\":[\"") + static_cast<char>(0xFF) + "\"]}"}) {
        Check(write(invalid), L"write isolated malformed configuration fixture");
        const auto before = Read(path);
        roots = {L"sentinel"};
        Check(!LoadNetworkRootsFile(path, roots, &error, &status) && roots.empty() &&
              !error.empty() && status == NetworkConfigLoadStatus::Invalid,
              L"malformed configuration fails without publishing partial roots");
        Check(!SaveNetworkRootsFile(path, {L"\\\\server\\new"}, &error) &&
              !error.empty() && Read(path) == before,
              L"direct save refuses to overwrite malformed original bytes");
    }
    Check(write(R"({"version":1})") && !LoadNetworkRootsFile(path, roots, &error, &status) &&
          status == NetworkConfigLoadStatus::MissingRoots && !error.empty(),
          L"missing roots key is reported separately");
    Check(write(R"({"roots":[123]})"), L"write failed-start configuration fixture");
    const auto before = Read(path);
    {
        NetworkIndex index;
        Check(!NetworkIndexAccess::LoadConfiguration(index) && !index.ConfigurationError().empty(),
              L"startup loader retains and exposes configuration failure");
        NetworkIndexAccess::Seed(index, L"\\\\server\\existing");
        Check(!index.AddRoot(L"\\\\server\\new", &error) && !error.empty() &&
              index.Roots().size() == 1 && Read(path) == before,
              L"add after failed load preserves original file and in-memory roots");
        Check(!index.RemoveRoot(L"\\\\server\\existing", &error) && !error.empty() &&
              index.Roots().size() == 1 && Read(path) == before,
              L"remove after failed load preserves original file and in-memory roots");
    }
    Check(write(R"({"roots":[]})"), L"write I/O denial fixture");
    const auto io_before = Read(path);
    HANDLE locked = CreateFileW(path.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    Check(locked != INVALID_HANDLE_VALUE, L"hold isolated configuration with sharing denied");
    Check(!LoadNetworkRootsFile(path, roots, &error, &status) &&
          status == NetworkConfigLoadStatus::IoError && !error.empty(), L"I/O failure is distinct from malformed JSON");
    Check(!SaveNetworkRootsFile(path, {L"\\\\server\\new"}, &error) && !error.empty(),
          L"save refuses configuration that cannot be read");
    if (locked != INVALID_HANDLE_VALUE) CloseHandle(locked);
    Check(Read(path) == io_before, L"I/O failure preserves original file bytes");
    {
        NetworkIndex index;
        Check(NetworkIndexAccess::LoadConfiguration(index) && index.ConfigurationError().empty() &&
              index.AddRoot(L"\\\\server\\new", &error) &&
              LoadNetworkRootsFile(path, roots, &error, &status) && roots.size() == 1 &&
              roots[0] == L"\\\\server\\new", L"valid empty load still permits add and persisted roundtrip");
    }
    SetNetworkIndexHooks(nullptr);
    std::error_code cleanup_error;
    std::filesystem::remove_all(directory, cleanup_error);
    Check(!cleanup_error, L"remove isolated network configuration fixtures");
    if (cleanup_error) std::fprintf(stderr, "[FAIL] fixture cleanup: error=%d %s\n",
        cleanup_error.value(), cleanup_error.message().c_str());
    std::fprintf(stderr, "[DIAG] network-config completed failures=%d\n", failures);
    std::fflush(stderr);
    return failures ? 1 : 0;
}
