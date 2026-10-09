#include "../ops/drop_staging.h"
#include <filesystem>
#include <fstream>
#include <cstdio>
#include <string>

namespace {
int passed = 0, failed = 0;
DWORD enumeration_thread = 0;
DWORD ui_thread = 0;
HWND paint_window = nullptr;
unsigned closes = 0, paints = 0;
void Check(bool good, const wchar_t* name) {
    wprintf(L"[%s] %s\n", good ? L"PASS" : L"FAIL", name);
    good ? ++passed : ++failed;
}
std::string Read(const std::filesystem::path& file) {
    std::ifstream input(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
LRESULT CALLBACK PaintWindowProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (msg == WM_CLOSE) { ++closes; return 0; }
    if (msg == WM_PAINT) { ++paints; }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}
HANDLE WINAPI ObserveEnumeration(LPCWSTR path, FINDEX_INFO_LEVELS level, LPVOID data,
    FINDEX_SEARCH_OPS search, LPVOID filter, DWORD flags) {
    enumeration_thread = GetCurrentThreadId();
    if (paint_window) {
        InvalidateRect(paint_window, nullptr, FALSE);
        PostMessageW(paint_window, WM_CLOSE, 0, 0);
        Sleep(40); // Leave a deterministic paint opportunity before the worker completes.
    }
    return FindFirstFileExW(path, level, data, search, filter, flags);
}
HANDLE WINAPI FailEnumeration(LPCWSTR, FINDEX_INFO_LEVELS, LPVOID, FINDEX_SEARCH_OPS, LPVOID, DWORD) {
    SetLastError(ERROR_ACCESS_DENIED);
    return INVALID_HANDLE_VALUE;
}
BOOL WINAPI FailNext(HANDLE, LPWIN32_FIND_DATAW) {
    SetLastError(ERROR_ACCESS_DENIED);
    return FALSE;
}
}

int wmain() {
    namespace fs = std::filesystem;
    using namespace pulse::ops;
    ui_thread = GetCurrentThreadId();
    const fs::path fixture = fs::absolute(fs::path(L"bench_data") /
        (L"drop_stage_" + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(GetTickCount64())));
    const fs::path temp = fixture / L"temp", stages = temp / L"PulseDrop";
    const fs::path original = temp / L"7zFixture";
    fs::create_directories(original / L"tree" / L"empty");
    fs::create_directories(fixture / L"destination");
    std::ofstream(original / L"file.txt") << "archive bytes";
    std::ofstream(original / L"tree" / L"nested.txt") << "nested bytes";
    const std::vector<std::wstring> sources{(original / L"file.txt").wstring(), (original / L"tree").wstring()};
    Check(IsTemporaryDropSource(sources[0], temp.wstring(), stages.wstring()), L"archive extraction inside TEMP is staged");
    Check(!IsTemporaryDropSource((fixture / L"temp-other" / L"file.txt").wstring(), temp.wstring(), stages.wstring()),
          L"prefix-sharing sibling is not TEMP");
    Check(!IsTemporaryDropSource((stages / L"owned" / L"file.txt").wstring(), temp.wstring(), stages.wstring()),
          L"already staged sources pass through");
    Check(!IsTemporaryDropSource(L"C:\\ordinary\\file.txt", temp.wstring(), stages.wstring()), L"ordinary files pass through");
    Check(IsTemporaryDropSource(L"\\\\?\\" + sources[0], temp.wstring(), stages.wstring()), L"long path prefix is normalized");

    WNDCLASSW wc{};
    wc.lpfnWndProc = PaintWindowProc; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"PulseDropStageFixture";
    RegisterClassW(&wc);
    paint_window = CreateWindowW(wc.lpszClassName, L"", WS_OVERLAPPED, 0, 0, 100, 100, nullptr, nullptr, wc.hInstance, nullptr);
    DropEnumerationApi observing; observing.first = ObserveEnumeration;
    std::vector<std::wstring> prepared;
    const auto staged = StageDropSourcesOnWorker(paint_window, sources, temp.wstring(), stages.wstring(), prepared, observing);
    Check(staged.any && staged.error == ERROR_SUCCESS && prepared.size() == sources.size() && prepared != sources,
          L"worker fully stages file and directory before returning");
    Check(enumeration_thread != 0 && enumeration_thread != ui_thread, L"directory scanning runs off the UI thread");
    Check(closes == 0 && IsWindow(paint_window) && IsWindowEnabled(paint_window), L"staging defers close messages and restores owner input");
    MSG message{};
    bool queued_close = PeekMessageW(&message, paint_window, WM_CLOSE, WM_CLOSE, PM_REMOVE) != FALSE;
    while (PeekMessageW(&message, paint_window, WM_CLOSE, WM_CLOSE, PM_REMOVE)) {}
    Check(queued_close, L"close request remains queued until staging finishes");
    DestroyWindow(paint_window); paint_window = nullptr;
    fs::remove_all(original);
    Check(Read(prepared[0]) == "archive bytes" && Read(fs::path(prepared[1]) / L"nested.txt") == "nested bytes",
          L"archive source deletion after Drop preserves staged bytes");
    Check(fs::is_directory(fs::path(prepared[1]) / L"empty"), L"empty extracted directories survive staging");
    const auto destination = fixture / L"destination" / L"file.txt";
    std::ofstream(destination) << "old conflict";
    Check(CopyFileW(prepared[0].c_str(), destination.c_str(), FALSE) && Read(destination) == "archive bytes",
          L"delayed conflict replacement succeeds after archive manager cleanup");
    SweepDropStages(stages.wstring(), false);
    Check(fs::exists(prepared[0]), L"startup sweep preserves stages belonging to the running process");
    SweepDropStages(stages.wstring(), true);
    Check(!fs::exists(prepared[0]) && fs::is_empty(stages), L"exit sweep removes owned stages");

    fs::create_directories(original / L"tree");
    std::ofstream(original / L"file.txt") << "preserve original";
    std::ofstream(original / L"tree" / L"nested.txt") << "nested";
    std::vector<std::wstring> failing{sources[0], (original / L"missing.txt").wstring()};
    const auto missing = StageDropSources(failing, temp.wstring(), stages.wstring(), prepared);
    Check(missing.error == ERROR_FILE_NOT_FOUND && !missing.any && prepared == failing && fs::is_empty(stages),
          L"partial staging failure rolls back and keeps the original source list");
    unsigned submits = 0;
    const auto refused = SubmitStagedDrop(failing, temp.wstring(), stages.wstring(), [&](const auto&) { ++submits; });
    Check(refused.error && submits == 0, L"failed staging never submits an operation");
    DropEnumerationApi denied; denied.first = FailEnumeration;
    DropStageError error;
    Check(!StageDropSources({sources[1]}, temp.wstring(), stages.wstring(), prepared, &error, denied) &&
          error.code == ERROR_ACCESS_DENIED && error.source == sources[1] && fs::is_empty(stages),
          L"initial enumeration failure is reported and cleaned up");
    denied.first = FindFirstFileExW; denied.next = FailNext;
    Check(!StageDropSources({sources[1]}, temp.wstring(), stages.wstring(), prepared, &error, denied) &&
          error.code == ERROR_ACCESS_DENIED && fs::is_empty(stages), L"mid-enumeration failure cannot become a partial success");
    SetFileAttributesW(sources[0].c_str(), FILE_ATTRIBUTE_READONLY);
    const auto readonly = StageDropSources({sources[0]}, temp.wstring(), stages.wstring(), prepared);
    Check(readonly.any && !readonly.error, L"read-only archive files use a durable copy");
    SweepDropStages(stages.wstring(), true);
    Check((GetFileAttributesW(sources[0].c_str()) & FILE_ATTRIBUTE_READONLY) != 0 && Read(sources[0]) == "preserve original",
          L"stage cleanup preserves read-only source attributes and data");
    SetFileAttributesW(sources[0].c_str(), FILE_ATTRIBUTE_NORMAL);
    fs::remove_all(fixture);
    Check(!fs::exists(fixture), L"isolated fixture cleanup completes");
    wprintf(L"drop staging: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
