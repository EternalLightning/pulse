// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "../index/document_reader.h"
#include "../index/document_protocol.h"
#include <tlhelp32.h>
#include <psapi.h>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>
#include <vector>

namespace {
struct Children { uint64_t count = 0, private_bytes = 0; };
Children Sample() {
    Children result;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32W entry{sizeof(entry)};
    if (snapshot == INVALID_HANDLE_VALUE) return result;
    if (Process32FirstW(snapshot, &entry)) do {
        if (entry.th32ParentProcessID != GetCurrentProcessId() || _wcsicmp(entry.szExeFile, L"Pulse.Document.exe")) continue;
        ++result.count;
        HANDLE child = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, entry.th32ProcessID);
        PROCESS_MEMORY_COUNTERS_EX memory{};
        memory.cb = sizeof(memory);
        if (child && GetProcessMemoryInfo(child, reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory)))
            result.private_bytes += memory.PrivateUsage;
        if (child) CloseHandle(child);
    } while (Process32NextW(snapshot, &entry));
    CloseHandle(snapshot);
    return result;
}
uint64_t Hash(std::wstring_view body) {
    uint64_t hash = 14695981039346656037ull;
    for (const auto character : body) { hash ^= static_cast<uint16_t>(character); hash *= 1099511628211ull; }
    return hash;
}
void Metrics(const pulse::index::DocumentReadMetrics& metrics) {
    std::cout << ",\"reader_queue_us\":" << metrics.reader_queue_us
        << ",\"admission_wait_us\":" << metrics.admission_wait_us
        << ",\"process_start_us\":" << metrics.process_start_us
        << ",\"response_us\":" << metrics.response_us
        << ",\"process_stop_us\":" << metrics.process_stop_us
        << ",\"process_starts\":" << metrics.process_starts;
}
}
int wmain(int argc, wchar_t** argv) {
    if (argc < 5 || argc > 6) return 2;
    const unsigned threads = static_cast<unsigned>(_wtoi(argv[2]));
    const unsigned rounds = static_cast<unsigned>(_wtoi(argv[3]));
    const auto cancel_after = argc == 6 ? _wcstoui64(argv[5], nullptr, 10) * 1000 : 0;
    if (!threads || threads > 16 || !rounds || rounds > 100) return 2;
    std::ifstream input{std::filesystem::path(argv[1])};
    std::vector<std::wstring> paths;
    for (std::string line; std::getline(input, line);) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, line.data(), static_cast<int>(line.size()), nullptr, 0);
        if (count <= 0) return 3;
        std::wstring path(static_cast<size_t>(count), L'\0');
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, line.data(), static_cast<int>(line.size()), path.data(), count);
        paths.push_back(std::move(path));
    }
    if (paths.empty()) return 3;
    struct Result { DWORD error = ERROR_CANCELLED; uint64_t hash = 0, chars = 0, elapsed = 0; pulse::index::DocumentReadMetrics metrics; };
    std::vector<Result> results(paths.size() * rounds);
    std::atomic<size_t> next{0};
    std::atomic<bool> done{false};
    Children peak;
    std::thread monitor([&] {
        while (!done) {
            const auto current = Sample();
            peak.count = (std::max)(peak.count, current.count);
            peak.private_bytes = (std::max)(peak.private_bytes, current.private_bytes);
            Sleep(10);
        }
    });
    const auto started = pulse::index::DocumentMicros();
    auto cancelled = [&] { return cancel_after && pulse::index::DocumentMicros() - started >= cancel_after; };
    std::vector<std::thread> workers;
    auto pool = pulse::index::CreateDocumentReadPool();
    wchar_t option[32]{};
    const bool two_pools = GetEnvironmentVariableW(L"PULSE_POOL_BENCH_SECOND_POOL", option, ARRAYSIZE(option)) && option[0] == L'1';
    auto second_pool = two_pools ? pulse::index::CreateDocumentReadPool() : pool;
    for (unsigned i = 0; i < threads; ++i) workers.emplace_back([&, i] {
        pulse::index::DocumentReadSession session(i % 2 ? second_pool : pool);
        auto reader_cancelled = [&] { return (!two_pools || i % 2 == 0) && cancelled(); };
        for (;;) {
            const auto index = next.fetch_add(1);
            if (index >= results.size()) break;
            auto& result = results[index];
            std::wstring body;
            uint64_t bytes = 0;
            const auto before = pulse::index::DocumentMicros();
            pulse::index::ReadSearchableDocument(paths[index % paths.size()], pulse::index::document::kMaximumFileBytes,
                body, bytes, &result.error, pulse::text::Encoding::Auto, reader_cancelled, argv[4], false, &result.metrics);
            result.elapsed = pulse::index::DocumentMicros() - before;
            result.hash = Hash(body); result.chars = body.size();
            if (reader_cancelled()) break;
        }
    });
    for (auto& worker : workers) worker.join();
    wchar_t ready_path[32768]{};
    if (GetEnvironmentVariableW(L"PULSE_POOL_BENCH_READY_FILE", ready_path, ARRAYSIZE(ready_path))) {
        std::ofstream ready{std::filesystem::path(ready_path)};
        ready << Sample().count;
    }
    if (GetEnvironmentVariableW(L"PULSE_POOL_BENCH_HOLD_MS", option, ARRAYSIZE(option))) Sleep(static_cast<DWORD>(_wtoi(option)));
    const auto release_started = pulse::index::DocumentMicros();
    second_pool.reset();
    pool.reset();
    const auto release_us = pulse::index::DocumentMicros() - release_started;
    const auto elapsed = pulse::index::DocumentMicros() - started;
    done = true;
    monitor.join();
    const auto remaining = Sample();
    pulse::index::DocumentReadMetrics total;
    uint64_t errors = 0;
    for (const auto& result : results) { pulse::index::AddReaderMetrics(total, result.metrics); errors += result.error != ERROR_SUCCESS; }
    std::cout << "{\"elapsed_us\":" << elapsed << ",\"files\":" << results.size() << ",\"errors\":" << errors
        << ",\"peak_children\":" << peak.count << ",\"peak_private_bytes\":" << peak.private_bytes
        << ",\"remaining_children\":" << remaining.count;
    std::cout << ",\"task_release_us\":" << release_us;
    Metrics(total);
    std::cout << ",\"results\":[";
    for (size_t i = 0; i < results.size(); ++i) {
        if (i) std::cout << ',';
        const auto& result = results[i];
        std::cout << "{\"index\":" << i << ",\"error\":" << result.error << ",\"hash\":" << result.hash
            << ",\"chars\":" << result.chars << ",\"elapsed_us\":" << result.elapsed;
        Metrics(result.metrics);
        std::cout << '}';
    }
    std::cout << "]}\n";
    return remaining.count || peak.count > 4 || peak.private_bytes > 1024ull * 1024 * 1024 ? 1 : 0;
}
