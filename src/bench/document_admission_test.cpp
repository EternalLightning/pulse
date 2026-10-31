// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "../index/document_admission.h"
#include <array>
#include <atomic>
#include <iostream>
#include <thread>
#include <vector>

using pulse::index::document::AdmissionHandle;
using pulse::index::document::AcquireAdmission;
using pulse::index::document::AdmissionContended;

namespace {
int failures = 0;
void Check(bool ok, const char* name) {
    std::cout << (ok ? "[PASS] " : "[FAIL] ") << name << '\n';
    if (!ok) ++failures;
}
std::wstring Number(HANDLE handle) {
    return std::to_wstring(reinterpret_cast<uintptr_t>(handle));
}
HANDLE Parse(const wchar_t* text) {
    return reinterpret_cast<HANDLE>(static_cast<uintptr_t>(_wcstoui64(text, nullptr, 10)));
}
bool Spawn(const std::wstring& arguments, const std::vector<HANDLE>& inherited,
           HANDLE job, bool suspended, PROCESS_INFORMATION& process) {
    wchar_t path[32768]{};
    if (!GetModuleFileNameW(nullptr, path, ARRAYSIZE(path))) return false;
    std::wstring command = L"\"" + std::wstring(path) + L"\" " + arguments;
    SIZE_T size = 0;
    const DWORD count = job ? 2 : 1;
    InitializeProcThreadAttributeList(nullptr, count, 0, &size);
    std::vector<unsigned char> storage(size);
    auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
    if (!InitializeProcThreadAttributeList(attributes, count, 0, &size)) return false;
    bool ok = UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
        const_cast<HANDLE*>(inherited.data()), inherited.size() * sizeof(HANDLE), nullptr, nullptr) != FALSE;
    if (ok && job) ok = UpdateProcThreadAttribute(attributes, 0, pulse::index::document::kJobListAttribute,
        &job, sizeof(job), nullptr, nullptr) != FALSE;
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.lpAttributeList = attributes;
    if (ok) ok = CreateProcessW(path, command.data(), nullptr, nullptr, TRUE,
        EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW | (suspended ? CREATE_SUSPENDED : 0),
        nullptr, nullptr, &startup.StartupInfo, &process) != FALSE;
    DeleteProcThreadAttributeList(attributes);
    return ok;
}
}

int wmain(int argc, wchar_t** argv) {
    if (argc >= 2 && std::wstring_view(argv[1]) == L"child") {
        if (argc == 3) return WaitForSingleObject(Parse(argv[2]), 10000) == WAIT_OBJECT_0 ? 0 : 2;
        Sleep(10000);
        return 0;
    }
    if (argc == 5 && std::wstring_view(argv[1]) == L"broker") {
        DWORD error = 0;
        AdmissionHandle slot, job;
        const auto deadline = GetTickCount64() + 2000;
        if (!AcquireAdmission(argv[2], slot, [&] { return GetTickCount64() >= deadline; }, error)) return 3;
        job.Reset(CreateJobObjectW(nullptr, nullptr));
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE |
            JOB_OBJECT_LIMIT_PROCESS_MEMORY | JOB_OBJECT_LIMIT_ACTIVE_PROCESS;
        limits.BasicLimitInformation.ActiveProcessLimit = 1;
        limits.ProcessMemoryLimit = 256ull * 1024 * 1024;
        if (!job.value || !SetInformationJobObject(job.value, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) return 4;
        PROCESS_INFORMATION child{};
        if (!Spawn(L"child", {slot.value}, job.value, true, child)) return 5;
        BOOL in_job = FALSE;
        if (!IsProcessInJob(child.hProcess, job.value, &in_job) || !in_job) return 6;
        auto* pid = static_cast<DWORD*>(MapViewOfFile(Parse(argv[4]), FILE_MAP_WRITE, 0, 0, sizeof(DWORD)));
        if (!pid) return 7;
        *pid = child.dwProcessId;
        UnmapViewOfFile(pid);
        CloseHandle(child.hThread);
        CloseHandle(child.hProcess);
        slot.Reset();
        SetEvent(Parse(argv[3]));
        // The test kills only this fixture broker, specifically before the
        // parser's ResumeThread point to exercise the startup crash window.
        Sleep(10000);
        return 8;
    }
    if (argc != 2) return 1;
    const std::wstring directory = argv[1];
    std::array<AdmissionHandle, 4> held;
    DWORD error = 0;
    bool acquired = true;
    for (auto& slot : held) acquired = AcquireAdmission(directory, slot, {}, error) && acquired;
    Check(acquired, "four slots can be held simultaneously");
    AdmissionHandle fifth;
    auto deadline = GetTickCount64() + 100;
    const auto started = GetTickCount64();
    Check(!AcquireAdmission(directory, fifth, [&] { return GetTickCount64() >= deadline; }, error) &&
        error == ERROR_CANCELLED && GetTickCount64() - started < 500,
        "fifth waits and cancellation returns within 500 ms");
    Check(!AdmissionContended(directory), "cancelled waiter releases demand marker");

    std::atomic<bool> waiting_acquired{false};
    std::thread waiter([&] {
        AdmissionHandle next;
        DWORD read_error = 0;
        const auto until = GetTickCount64() + 2000;
        waiting_acquired = AcquireAdmission(directory, next, [&] { return GetTickCount64() >= until; }, read_error);
    });
    deadline = GetTickCount64() + 1000;
    while (!AdmissionContended(directory) && GetTickCount64() < deadline) Sleep(5);
    Check(AdmissionContended(directory), "busy reader can observe waiting demand");
    held[3].Reset();
    waiter.join();
    Check(waiting_acquired, "waiting reader obtains released slot");
    Check(AcquireAdmission(directory, held[3], {}, error), "released slot is reusable");

    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    AdmissionHandle stop;
    stop.Reset(CreateEventW(&security, TRUE, FALSE, nullptr));
    PROCESS_INFORMATION child{};
    const bool spawned = Spawn(L"child " + Number(stop.value), {held[3].value, stop.value}, nullptr, false, child);
    Check(spawned, "fixture parser inherits admission handle");
    if (spawned) {
        held[3].Reset();
        deadline = GetTickCount64() + 100;
        Check(!AcquireAdmission(directory, fifth, [&] { return GetTickCount64() >= deadline; }, error),
            "parent close cannot release slot still held by parser");
        SetEvent(stop.value);
        Check(WaitForSingleObject(child.hProcess, 2000) == WAIT_OBJECT_0, "fixture parser exits normally");
        CloseHandle(child.hThread);
        CloseHandle(child.hProcess);
        deadline = GetTickCount64() + 1000;
        Check(AcquireAdmission(directory, held[3], [&] { return GetTickCount64() >= deadline; }, error),
            "parser exit automatically releases inherited slot");
    }
    held[3].Reset();
    AdmissionHandle ready, mapping;
    ready.Reset(CreateEventW(&security, TRUE, FALSE, nullptr));
    mapping.Reset(CreateFileMappingW(INVALID_HANDLE_VALUE, &security, PAGE_READWRITE, 0, sizeof(DWORD), nullptr));
    PROCESS_INFORMATION broker{};
    const bool broker_started = Spawn(L"broker \"" + directory + L"\" " + Number(ready.value) + L" " + Number(mapping.value),
        {ready.value, mapping.value}, nullptr, false, broker);
    Check(broker_started, "independent fixture instance starts");
    if (broker_started) {
        const bool broker_ready = WaitForSingleObject(ready.value, 3000) == WAIT_OBJECT_0;
        Check(broker_ready, "suspended parser is atomically assigned to kill-on-close job");
        HANDLE parser = nullptr;
        if (broker_ready) {
            auto* pid = static_cast<DWORD*>(MapViewOfFile(mapping.value, FILE_MAP_READ, 0, 0, sizeof(DWORD)));
            if (pid) { parser = OpenProcess(SYNCHRONIZE, FALSE, *pid); UnmapViewOfFile(pid); }
            deadline = GetTickCount64() + 100;
            Check(!AcquireAdmission(directory, fifth, [&] { return GetTickCount64() >= deadline; }, error),
                "independent instances share the four-process cap");
        }
        TerminateProcess(broker.hProcess, 42);
        WaitForSingleObject(broker.hProcess, 2000);
        if (broker_ready) Check(parser && WaitForSingleObject(parser, 2000) == WAIT_OBJECT_0,
            "crashed parent kills parser even before resume");
        if (parser) CloseHandle(parser);
        CloseHandle(broker.hThread);
        CloseHandle(broker.hProcess);
        deadline = GetTickCount64() + 2000;
        Check(AcquireAdmission(directory, held[3], [&] { return GetTickCount64() >= deadline; }, error),
            "parent crash leaves no permanent admission leak");
    }
    AdmissionHandle invalid;
    Check(!AcquireAdmission(directory + L"\\missing", invalid, {}, error) && error == ERROR_PATH_NOT_FOUND,
        "directory failure is reported without indefinite retry");
    return failures ? 1 : 0;
}
