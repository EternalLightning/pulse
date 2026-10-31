// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

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
    const bool owner_evidence_only = argc == 3 && std::wstring(argv[1]) == L"--filter" &&
        std::wstring(argv[2]) == L"owner-evidence";
    if (argc != 1 && !owner_evidence_only) {
        std::printf("Usage: pulse_file_lock_owner_test.exe [--filter owner-evidence]\n");
        return 2;
    }
    Check(!IsLockFailure(E_ACCESSDENIED) && MayBeLockFailure(E_ACCESSDENIED),
        "Win32 access denial needs owner evidence and is not an ownerless sharing failure");
    Check(!IsLockFailure(static_cast<HRESULT>(0x80270021u)) &&
        !IsLockFailure(static_cast<HRESULT>(0x80270022u)) &&
        MayBeLockFailure(static_cast<HRESULT>(0x80270021u)) &&
        MayBeLockFailure(static_cast<HRESULT>(0x80270022u)),
        "Shell source and destination access denials need owner evidence");
    Check(IsLockFailure(HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION)) &&
        IsLockFailure(HRESULT_FROM_WIN32(ERROR_LOCK_VIOLATION)) &&
        IsLockFailure(static_cast<HRESULT>(0x80270027u)) && IsLockFailure(static_cast<HRESULT>(0x80270028u)),
        "Win32 and Shell explicit sharing failures retain ownerless retry classification");
    Check(!IsLockFailure(HRESULT_FROM_WIN32(ERROR_DIR_NOT_EMPTY)),
        "a nonempty directory alone is not evidence of file occupancy");
    const auto fixture = std::filesystem::current_path() / L"bench_data" /
        (L"pulse_lock_fixture_" + std::to_wstring(GetCurrentProcessId()));
    const auto holder = fixture / (owner_evidence_only ? L"Pulse.exe" : L"FixtureHolder.exe");
    const auto file = fixture / L"locked.txt";
    std::filesystem::create_directories(fixture);
    std::filesystem::copy_file(Executable(), holder, std::filesystem::copy_options::overwrite_existing);
    HANDLE seed = CreateFileW(file.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE |
        FILE_SHARE_DELETE, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    Check(seed != INVALID_HANDLE_VALUE, "create isolated lock fixture");
    if (seed != INVALID_HANDLE_VALUE) CloseHandle(seed);
    HANDLE self_lease = CreateFileW(file.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    Check(self_lease != INVALID_HANDLE_VALUE, "current process holds an isolated file lease");
    if (self_lease != INVALID_HANDLE_VALUE) {
        std::wstring diagnostics;
        const auto denied = ProbeFileLocks(E_ACCESSDENIED, L"locked.txt", {file.wstring()},
            LockProtectedDirectory(), {}, &diagnostics);
        std::printf("[DIAG] self_pid=%lu, owners=%zu\n%ls", GetCurrentProcessId(),
            denied.owners.size(), diagnostics.c_str());
        Check(denied.owners.empty() && denied.path.empty(),
            "a current-process-only lease supplies no access-denial owner evidence");
        const auto shell_denied = ProbeFileLocks(static_cast<HRESULT>(0x80270021u),
            L"locked.txt", {file.wstring()}, LockProtectedDirectory());
        Check(shell_denied.owners.empty() && shell_denied.path.empty(),
            "a current-process-only lease supplies no Shell access-denial owner evidence");
        Check(!DeleteFileW(file.c_str()) && GetLastError() == ERROR_SHARING_VIOLATION,
            "the current-process lease reproduces a real sharing violation");
        const auto sharing_error = HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION);
        const auto sharing = ProbeFileLocks(sharing_error, L"locked.txt", {file.wstring()},
            LockProtectedDirectory());
        Check(sharing.owners.empty() && sharing.path.empty() && IsLockFailure(sharing_error),
            "explicit sharing failure retains ownerless retry after removing self evidence");
        CloseHandle(self_lease);
    }
    // Zero access lets this lease coexist with the child's exclusive data handle.
    self_lease = CreateFileW(file.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    Check(self_lease != INVALID_HANDLE_VALUE,
        "current-process metadata lease permits a simultaneous exclusive child holder");
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
        if (owner_evidence_only) Check(found != report.owners.end(),
            "a different Pulse.exe process remains external owner evidence");
        Check(std::none_of(report.owners.begin(), report.owners.end(), [](const auto& owner) {
                return owner.pid == GetCurrentProcessId();
            }), "external holder reports omit the simultaneous current-process lease");
        const auto denied = ProbeFileLocks(E_ACCESSDENIED, L"locked.txt", {file.wstring()},
            LockProtectedDirectory());
        Check(std::any_of(denied.owners.begin(), denied.owners.end(), [&](const auto& owner) {
                return owner.pid == process.dwProcessId;
            }), "access denial retains real external owner evidence");
        Check(ProbeFileLocks(HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND), L"locked.txt",
            {file.wstring()}, LockProtectedDirectory()).owners.empty(),
            "unrelated errors do not blame a process");
        Check(ProbeFileLocks(HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION), L"locked.txt",
            {file.wstring()}, LockProtectedDirectory(), [] { return true; }).owners.empty(),
            "cancelled lookups publish no delayed report");
        if (self_lease != INVALID_HANDLE_VALUE) {
            CloseHandle(self_lease);
            self_lease = INVALID_HANDLE_VALUE;
        }
        if (!owner_evidence_only && found != report.owners.end()) {
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
    if (self_lease != INVALID_HANDLE_VALUE) CloseHandle(self_lease);
    // Cleanup is limited to the three paths created by this executable.
    DeleteFileW(file.c_str());
    DeleteFileW(holder.c_str());
    RemoveDirectoryW(fixture.c_str());
    return failures ? 1 : 0;
}
