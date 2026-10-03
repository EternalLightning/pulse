#include "../ops/file_lock_owner.h"
#include <cstdio>
#include <filesystem>
#include <algorithm>

namespace {
int failures = 0;
void Check(bool ok, const char* text) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", text);
    if (!ok) ++failures;
}
std::wstring Executable() {
    std::wstring path(32768, L'\0');
    const auto count = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    path.resize(count);
    return path;
}
int Hold(const wchar_t* file, const wchar_t* ready_name, const wchar_t* stop_name) {
    HANDLE ready = OpenEventW(EVENT_MODIFY_STATE, FALSE, ready_name);
    HANDLE stop = OpenEventW(SYNCHRONIZE, FALSE, stop_name);
    HANDLE held = CreateFileW(file, GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (!ready || !stop || held == INVALID_HANDLE_VALUE) return 2;
    SetEvent(ready);
    const auto result = WaitForSingleObject(stop, 60000);
    CloseHandle(held);
    CloseHandle(ready);
    CloseHandle(stop);
    return result == WAIT_OBJECT_0 ? 0 : 3;
}
}

int wmain(int argc, wchar_t** argv) {
    using namespace pulse::ops;
    if (argc == 5 && std::wstring(argv[1]) == L"--hold") return Hold(argv[2], argv[3], argv[4]);
    const auto fixture = std::filesystem::current_path() /
        (L"pulse_lock_fixture_" + std::to_wstring(GetCurrentProcessId()));
    const auto holder = fixture / L"FixtureHolder.exe";
    const auto file = fixture / L"locked.txt";
    std::filesystem::create_directory(fixture);
    std::filesystem::copy_file(Executable(), holder, std::filesystem::copy_options::overwrite_existing);
    HANDLE seed = CreateFileW(file.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE |
        FILE_SHARE_DELETE, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    Check(seed != INVALID_HANDLE_VALUE, "create isolated lock fixture");
    if (seed != INVALID_HANDLE_VALUE) CloseHandle(seed);
    const auto identity = std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(GetTickCount64());
    const auto ready_name = L"Local\\PulseLockReady_" + identity;
    const auto stop_name = L"Local\\PulseLockStop_" + identity;
    HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, ready_name.c_str());
    HANDLE stop = CreateEventW(nullptr, TRUE, FALSE, stop_name.c_str());
    std::wstring command = L"\"" + holder.wstring() + L"\" --hold \"" + file.wstring() +
        L"\" \"" + ready_name + L"\" \"" + stop_name + L"\"";
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    const bool spawned = CreateProcessW(holder.c_str(), command.data(), nullptr, nullptr, FALSE,
        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process) != FALSE;
    Check(spawned && WaitForSingleObject(ready, 5000) == WAIT_OBJECT_0,
        "controlled fixture process holds an exclusive file handle");
    if (spawned) {
        Check(!DeleteFileW(file.c_str()) && GetLastError() == ERROR_SHARING_VIOLATION,
            "fixture reproduces a real sharing violation");
        std::wstring diagnostics;
        const auto report = ProbeFileLocks(HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION),
            L"locked.txt", {file.wstring()}, LockProtectedDirectory(), {}, &diagnostics);
        std::printf("[DIAG] expected_pid=%lu, path=%ls, owners=%zu\n%ls", process.dwProcessId,
            report.path.c_str(), report.owners.size(), diagnostics.c_str());
        for (const auto& owner : report.owners) std::printf("[DIAG] PID=%lu, closable=%d, image=%ls\n",
            owner.pid, owner.can_terminate, owner.image_path.c_str());
        const auto found = std::find_if(report.owners.begin(), report.owners.end(), [&](const auto& owner) {
            return owner.pid == process.dwProcessId;
        });
        Check(report.path == file.wstring() && found != report.owners.end(),
            "display-only Shell failure resolves the full file path and owner PID");
        Check(ProbeFileLocks(HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND), L"locked.txt",
            {file.wstring()}, LockProtectedDirectory()).owners.empty(),
            "unrelated errors do not blame a process");
        Check(ProbeFileLocks(HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION), L"locked.txt",
            {file.wstring()}, LockProtectedDirectory(), [] { return true; }).owners.empty(),
            "cancelled lookups publish no delayed report");
        if (found != report.owners.end()) {
            auto stale = *found;
            ++stale.start_time.dwLowDateTime;
            Check(EndLockOwner(stale, LockProtectedDirectory()) == EndLockResult::AlreadyGone &&
                WaitForSingleObject(process.hProcess, 0) == WAIT_TIMEOUT,
                "changed process start time never terminates the live holder");
            Check(EndLockOwner(*found, fixture.wstring()) == EndLockResult::Refused &&
                WaitForSingleObject(process.hProcess, 0) == WAIT_TIMEOUT,
                "termination rechecks protected executable directories");
            Check(EndLockOwner(*found, LockProtectedDirectory(), [] { return true; }) ==
                EndLockResult::Cancelled && WaitForSingleObject(process.hProcess, 0) == WAIT_TIMEOUT,
                "cancel before retry leaves the holder running");
            Check(found->can_terminate && EndLockOwner(*found, LockProtectedDirectory()) == EndLockResult::Ended,
                "end-and-retry terminates only the known fixture instance");
            Check(DeleteFileW(file.c_str()) != FALSE, "delete succeeds after fixture holder exits");
        }
        SetEvent(stop);
        Check(WaitForSingleObject(process.hProcess, 5000) == WAIT_OBJECT_0, "fixture holder exits");
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
    }
    CloseHandle(ready);
    CloseHandle(stop);
    // Cleanup is limited to the three paths created by this executable.
    DeleteFileW(file.c_str());
    DeleteFileW(holder.c_str());
    RemoveDirectoryW(fixture.c_str());
    return failures ? 1 : 0;
}
