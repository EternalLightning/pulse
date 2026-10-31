// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "../common/crash_reporter.h"
#include "../common/diagnostics_exporter.h"

#include <windows.h>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

using namespace pulse;

namespace {

int g_pass = 0;
int g_fail = 0;

void Check(bool condition, const wchar_t* name) {
    if (condition) {
        ++g_pass;
        wprintf(L"[PASS] %s\n", name);
    } else {
        ++g_fail;
        wprintf(L"[FAIL] %s\n", name);
    }
}

std::wstring Quote(const std::wstring& value) {
    return L"\"" + value + L"\"";
}

std::wstring SelfPath() {
    std::wstring value(32768, L'\0');
    const DWORD size = GetModuleFileNameW(nullptr, value.data(),
                                          static_cast<DWORD>(value.size()));
    value.resize(size);
    return value;
}

bool RunChild(const std::wstring& root, const wchar_t* mode) {
    std::wstring command = Quote(SelfPath()) + L" --child " + Quote(root) + L" " + mode;
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                        nullptr, nullptr, &startup, &process)) return false;
    const DWORD wait = WaitForSingleObject(process.hProcess, 15000);
    DWORD code = 0;
    if (wait == WAIT_OBJECT_0) GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return wait == WAIT_OBJECT_0 && code != 0;
}

size_t CountExtension(const std::filesystem::path& path, const wchar_t* extension) {
    size_t count = 0;
    std::error_code error;
    for (const auto& item : std::filesystem::directory_iterator(path, error)) {
        if (item.is_regular_file() && item.path().extension() == extension) ++count;
    }
    return count;
}

std::string FirstJson(const std::filesystem::path& path) {
    std::error_code error;
    for (const auto& item : std::filesystem::directory_iterator(path, error)) {
        if (item.path().extension() != L".json") continue;
        std::ifstream file(item.path(), std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(file), {});
    }
    return {};
}

} // namespace


BOOL WINAPI FailFindNext(HANDLE find, LPWIN32_FIND_DATAW data) {
    const std::wstring_view name(data->cFileName);
    if ((name == L"." || name == L"..") && FindNextFileW(find, data)) return TRUE;
    SetLastError(ERROR_READ_FAULT);
    return FALSE;
}
int EnumerationErrorsTest() {
    namespace fs = std::filesystem;
    const auto root = fs::absolute(fs::path(L"bench_data") /
        (L"diagnostic-enumeration-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64())));
    const auto source = root / L"source", destination = root / L"export";
    const auto crashes = source / L"Diagnostics" / L"Crashes";
    fs::create_directories(crashes);
    fs::create_directories(destination);
    std::ofstream(crashes / L"one.log") << "isolated log";
    diagnostics::ExportOptions options{source.wstring(), destination.wstring()};
    options.require_empty_destination = false;
    diagnostics::SetFindNextForTesting(&FailFindNext);
    std::wstring error;
    Check(!diagnostics::Export(options, &error) && !error.empty(), L"export reports injected enumeration I/O failure");
    Check(fs::exists(destination / L"Crashes" / L"one.log"), L"injected failure follows an actual artifact copy");
    Check(!fs::exists(destination / L"diagnostics-manifest.json"), L"incomplete export never receives success manifest");
    error.clear();
    Check(!diagnostics::ClearCrashReports(source.wstring(), &error) && !error.empty(),
        L"clear reports injected enumeration I/O failure");
    const auto empty_destination = root / L"empty-export";
    fs::create_directories(empty_destination);
    options.destination = empty_destination.wstring();
    options.require_empty_destination = true;
    error.clear();
    Check(!diagnostics::Export(options, &error) && !error.empty(), L"destination emptiness scan rejects enumeration failure");
    diagnostics::SetFindNextForTesting(nullptr);
    const auto complete_destination = root / L"complete-export";
    fs::create_directories(complete_destination);
    options.destination = complete_destination.wstring();
    options.require_empty_destination = true;
    Check(diagnostics::Export(options, &error) && fs::exists(complete_destination / L"diagnostics-manifest.json"),
        L"complete enumeration produces a success manifest");
    Check(diagnostics::ClearCrashReports(source.wstring(), &error), L"complete cleanup enumeration succeeds");
    const auto absent_source = root / L"absent-crashes";
    const auto absent_destination = root / L"absent-export";
    fs::create_directories(absent_source);
    fs::create_directories(absent_destination);
    options.source_root = absent_source.wstring();
    options.destination = absent_destination.wstring();
    Check(diagnostics::Export(options, &error), L"missing optional crash directory remains valid empty export");
    fs::create_directories(absent_source / L"Diagnostics");
    std::ofstream(absent_source / L"Diagnostics" / L"Crashes") << "not a directory";
    const auto invalid_destination = root / L"invalid-export";
    fs::create_directories(invalid_destination);
    options.destination = invalid_destination.wstring();
    error.clear();
    Check(!diagnostics::Export(options, &error) && !error.empty(), L"existing invalid crash directory is not treated as absent");
    error.clear();
    Check(!diagnostics::ClearCrashReports(absent_source.wstring(), &error) && !error.empty(),
        L"clear reports invalid crash directory");
    wprintf(L"%d passed, %d failed; isolated fixture retained\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}

int wmain(int argc, wchar_t** argv) {
    if (argc == 2 && wcscmp(argv[1], L"--enumeration-errors") == 0) return EnumerationErrorsTest();
    if (argc >= 4 && wcscmp(argv[1], L"--child") == 0) {
        crash::Initialize({crash::ProcessRole::Test, false, argv[2]});
        crash::AddBreadcrumb(7, 11, 13);
        if (wcscmp(argv[3], L"terminate") == 0) crash::ReportTerminate("test-terminate");
        RaiseException(0xE0424242u, EXCEPTION_NONCONTINUABLE, 0, nullptr);
        return 0;
    }

    wchar_t temp[MAX_PATH]{};
    GetTempPathW(ARRAYSIZE(temp), temp);
    const std::filesystem::path root = std::filesystem::path(temp) /
        (L"PulseCrashTest-" + std::to_wstring(GetCurrentProcessId()));
    const std::filesystem::path crashes = root / L"Diagnostics" / L"Crashes";
    const std::filesystem::path exported = root / L"Export";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root, error);

    Check(RunChild(root.wstring(), L"seh"), L"unhandled SEH terminates child");
    Check(RunChild(root.wstring(), L"terminate"), L"terminate handler terminates child");
    Check(CountExtension(crashes, L".dmp") == 2, L"crashes create minidumps");
    Check(CountExtension(crashes, L".json") == 2, L"crashes create JSON metadata");
    const std::string json = FirstJson(crashes);
    Check(json.find("\"schema\":1") != std::string::npos &&
          json.find("\"build_id\"") != std::string::npos &&
          json.find("\"breadcrumbs\"") != std::string::npos,
          L"metadata contains stable schema and breadcrumbs");

    for (int i = 0; i < 11; ++i) RunChild(root.wstring(), L"seh");
    crash::Initialize({crash::ProcessRole::Test, false, root.wstring()});
    Check(CountExtension(crashes, L".json") <= 10 &&
          CountExtension(crashes, L".dmp") <= 10,
          L"startup retention keeps at most ten event pairs");

    std::filesystem::create_directories(exported, error);
    diagnostics::ExportOptions options;
    options.source_root = root.wstring();
    options.destination = exported.wstring();
    std::wstring export_error;
    Check(diagnostics::Export(options, &export_error), L"diagnostics export succeeds");
    Check(std::filesystem::exists(exported / L"diagnostics-manifest.json"),
          L"diagnostics export writes manifest");
    Check(CountExtension(exported / L"Crashes", L".dmp") <= 10,
          L"diagnostics export includes retained dumps");

    crash::Shutdown();
    std::filesystem::remove_all(root, error);
    wprintf(L"%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
