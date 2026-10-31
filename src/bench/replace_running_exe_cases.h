// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once

#include "../ops/ops_manager.h"
#include <cstdio>
#include <filesystem>
#include <fstream>

namespace pulse::bench {

inline int RunRunningExeReplacementChecks(const std::wstring& fixture_root) {
    int failures = 0;
    const auto check = [&](bool ok, const char* label) {
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
        if (!ok) ++failures;
    };
    wchar_t executable[32768]{};
    if (!GetModuleFileNameW(nullptr, executable, ARRAYSIZE(executable))) return 2;
    for (const auto type : {ops::OpType::Copy, ops::OpType::Move}) {
        const auto root = std::filesystem::path(fixture_root) / (type == ops::OpType::Copy ? L"copy" : L"move");
        const auto source = root / L"source" / L"running.exe";
        const auto target = root / L"target" / L"running.exe";
        std::filesystem::create_directories(source.parent_path());
        std::filesystem::create_directories(target.parent_path());
        { std::ofstream output(source, std::ios::binary); output << "replacement bytes"; }
        if (!CopyFileW(executable, target.c_str(), TRUE)) return 2;
        auto original = ops::FileObjectReference::Open(target.wstring());
        std::wstring command = L"\"" + target.wstring() + L"\" --replacement-fixture-child";
        STARTUPINFOW startup{sizeof(startup)};
        PROCESS_INFORMATION process{};
        const bool launched = CreateProcessW(target.c_str(), command.data(), nullptr, nullptr, FALSE,
            CREATE_SUSPENDED | CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process) != FALSE;
        check(launched, "isolated child has a real executable image mapped by Windows");
        if (!launched) return 2;
        ops::OpsManager manager;
        const auto journal = (root / L"operations.json").wstring();
        manager.SetJournalPath(journal);
        manager.Start([] {});
        ops::OpRequest request;
        request.type = type;
        request.sources = {source.wstring()};
        request.dest_dir = target.parent_path().wstring();
        request.collision_policy = ops::CollisionPolicy::Replace;
        const auto run = [&](bool retry = false) {
            const auto completed = manager.Status().completed_ops;
            if (retry) check(manager.RetryLockedOperation(manager.Status().task_id, false),
                "occupied replacement can be retried through the operation's retry action");
            else manager.Submit(request);
            const auto deadline = GetTickCount64() + 15000;
            while (GetTickCount64() < deadline &&
                (manager.Status().completed_ops == completed || manager.HasPendingFileOperations())) Sleep(5);
            return manager.Status();
        };
        auto status = run();
        std::printf("[DIAG] type=%d phase=%d hr=0x%08lX path=%ls error=%ls\n", static_cast<int>(type),
            static_cast<int>(status.phase), static_cast<unsigned long>(status.failure_hr),
            status.failed_path.c_str(), status.last_error.c_str());
        check(status.phase == ops::OpPhase::Failed && ops::IsLockFailure(status.failure_hr) &&
            status.failed_path == target.wstring() && status.lock_retry_available,
            "running executable replacement reports the occupied destination and allows retry");
        auto current = ops::FileObjectReference::Open(target.wstring());
        check(original.Matches(current.Get()) && std::filesystem::exists(source),
            "blocked Copy and Move preserve both the original executable and the source");
        current = {};
        bool leftovers = false;
        for (const auto& entry : std::filesystem::directory_iterator(target.parent_path())) {
            const auto name = entry.path().filename().wstring();
            leftovers = leftovers || name.find(L".pulse-backup-") != std::wstring::npos ||
                name.find(L".pulse-copy-") != std::wstring::npos;
        }
        check(!leftovers, "blocked replacement creates no backup or temporary file");
        const auto journal_deadline = GetTickCount64() + 2000;
        while (GetFileAttributesW(journal.c_str()) != INVALID_FILE_ATTRIBUTES &&
            GetTickCount64() < journal_deadline) Sleep(5);
        ops::OpsManager blocked_reopened;
        blocked_reopened.SetJournalPath(journal);
        check(blocked_reopened.PendingRecovery().entries.empty(),
            "blocked replacement already clears its crash journal before retry or shutdown");
        check(ResumeThread(process.hThread) != static_cast<DWORD>(-1) &&
            WaitForSingleObject(process.hProcess, 10000) == WAIT_OBJECT_0,
            "isolated child exits normally after releasing its executable image");
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        original = {};
        status = run(true);
        std::ifstream input(target, std::ios::binary);
        const std::string bytes((std::istreambuf_iterator<char>(input)), {});
        input.close();
        check(status.phase == ops::OpPhase::Completed && bytes == "replacement bytes",
            "replacement succeeds after the executable exits");
        manager.Stop();
        ops::OpsManager reopened;
        reopened.SetJournalPath(journal);
        check(reopened.PendingRecovery().entries.empty(), "finished or blocked replacement leaves no restart recovery task");
    }
    std::error_code error;
    std::filesystem::remove_all(fixture_root, error);
    check(!error, "isolated running executable fixtures are cleaned");
    return failures ? 1 : 0;
}

} // namespace pulse::bench
