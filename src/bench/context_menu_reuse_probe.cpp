// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "../shell_host/ctx_session_process.h"

#include <tlhelp32.h>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>

namespace {
std::string Quote(const std::wstring& value) {
    const int size = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    std::string text(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), text.data(), size, nullptr, nullptr);
    std::string out = "\"";
    for (const unsigned char c : text) {
        if (c == '\\' || c == '"') out += '\\';
        if (c < 32) { char escape[7]{}; sprintf_s(escape, "\\u%04x", c); out += escape; }
        else out += static_cast<char>(c);
    }
    return out + '"';
}

DWORD WorkerPid() {
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W entry{sizeof(entry)};
    DWORD pid = 0;
    if (Process32FirstW(snapshot, &entry)) do {
        if (entry.th32ParentProcessID == GetCurrentProcessId() &&
                _wcsicmp(entry.szExeFile, L"Pulse.Shell.exe") == 0) pid = entry.th32ProcessID;
    } while (Process32NextW(snapshot, &entry));
    CloseHandle(snapshot);
    return pid;
}
}

// Query/close only: never invokes a registered third-party menu command.
int wmain(int argc, wchar_t** argv) {
    if (argc < 5) return 2;
    const bool warm = wcscmp(argv[3], L"warm") == 0;
    std::ofstream output(std::filesystem::path(argv[2]), std::ios::binary);
    if (!output) return 3;
    pulse::ipc::PayloadWriter query;
    query.PutU32(0); query.PutU32(0);
    std::vector<std::wstring> paths;
    for (int i = 4; i < argc; ++i) paths.emplace_back(argv[i]);
    query.PutStringArray(paths); query.PutStringArray({});
    std::mutex mutex;
    std::condition_variable condition;
    uint32_t completed = 0;
    ULONGLONG started = 0;
    bool malformed = false;
    pulse::shell::ContextSessionProcesses processes(argv[1], [&](uint32_t type, uint32_t id, const auto& payload) {
        std::lock_guard lock(mutex);
        pulse::ipc::PayloadReader reader(payload.data(), payload.size());
        uint32_t session = 0, flags = 0, count = 0;
        if (type != pulse::ipc::RSP_CTX_ITEMS || !reader.GetU32(session) || !reader.GetU32(flags) || !reader.GetU32(count)) {
            malformed = true; condition.notify_all(); return;
        }
        output << "{\"event\":\"items\",\"id\":" << id << ",\"ms\":" << GetTickCount64() - started
            << ",\"partial\":" << ((flags & pulse::ipc::CTX_ITEMS_PARTIAL) ? "true" : "false")
            << ",\"count\":" << count << ",\"items\":[";
        for (uint32_t i = 0; i < count; ++i) {
            uint32_t item = 0, item_flags = 0;
            std::wstring verb, text, clsid, handler;
            if (!reader.GetU32(item) || !reader.GetU32(item_flags) || !reader.GetString(verb) ||
                    !reader.GetString(text) || !reader.GetString(clsid) || !reader.GetString(handler)) {
                malformed = true; break;
            }
            output << (i ? "," : "") << "{\"id\":" << item << ",\"flags\":" << item_flags
                << ",\"verb\":" << Quote(verb) << ",\"text\":" << Quote(text)
                << ",\"clsid\":" << Quote(clsid) << ",\"handler\":" << Quote(handler) << '}';
        }
        output << "]}\n";
        output.flush();
        if (!(flags & pulse::ipc::CTX_ITEMS_PARTIAL)) completed = id;
        condition.notify_all();
    }, warm ? pulse::shell::ContextSessionProcesses::ReusePolicy{} : pulse::shell::ContextSessionProcesses::ReusePolicy{0, 0});
    bool failed = false;
    for (uint32_t id = 1; id <= 4 && !failed; ++id) {
        { std::lock_guard lock(mutex); started = GetTickCount64(); }
        processes.Query(id, query.data());
        {
            std::unique_lock lock(mutex);
            failed = !condition.wait_for(lock, std::chrono::seconds(15), [&] { return completed == id || malformed; }) || malformed;
            output << "{\"event\":\"query-finished\",\"id\":" << id << ",\"pid\":" << WorkerPid()
                << ",\"ms\":" << GetTickCount64() - started << ",\"failed\":" << (failed ? "true" : "false") << "}\n";
        }
        processes.Close(id);
        const auto deadline = GetTickCount64() + 3000;
        while (processes.ActiveProcesses() && !processes.IdleProcesses() && GetTickCount64() < deadline) Sleep(10);
        const auto idle = processes.IdleProcesses();
        const auto active = processes.ActiveProcesses();
        {
            std::lock_guard lock(mutex);
            output << "{\"event\":\"closed\",\"id\":" << id << ",\"idle\":" << idle
                << ",\"processes\":" << active << "}\n";
        }
    }
    processes.Stop();
    const auto deadline = GetTickCount64() + 3000;
    while (processes.ActiveProcesses() && GetTickCount64() < deadline) Sleep(10);
    output << "{\"event\":\"cleanup\",\"processes\":" << processes.ActiveProcesses() << "}\n";
    return failed || processes.ActiveProcesses() ? 1 : 0;
}
