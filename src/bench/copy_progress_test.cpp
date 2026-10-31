// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "ops/copy_progress_report.h"
#include "ops/transfer_rate.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <filesystem>
#include <string>

namespace {
int failures = 0;
constexpr uint64_t mib = 1024ull * 1024;
void Check(bool value, const char* description) {
    std::printf("[%s] %s\n", value ? "PASS" : "FAIL", description);
    failures += !value;
}
COPYFILE2_MESSAGE Chunk(uint64_t bytes, uint64_t total) {
    COPYFILE2_MESSAGE message{};
    message.Type = COPYFILE2_CALLBACK_CHUNK_FINISHED;
    message.Info.ChunkFinished.uliTotalBytesTransferred.QuadPart = bytes;
    message.Info.ChunkFinished.uliTotalFileSize.QuadPart = total;
    return message;
}
int RegressionTests() {
    using namespace pulse::ops;
    for (const auto type : {COPYFILE2_CALLBACK_CHUNK_STARTED, COPYFILE2_CALLBACK_STREAM_STARTED,
                           COPYFILE2_CALLBACK_POLL_CONTINUE, COPYFILE2_CALLBACK_NONE}) {
        CopyProgressReporter reporter;
        COPYFILE2_MESSAGE message{};
        message.Type = type;
        int reports = 0;
        reporter.Observe(message, 100, [&](uint64_t, uint64_t) { ++reports; });
        Check(reports == 0, "notifications without byte progress never publish zero/zero");
        bool real_reported = false;
        reporter.Observe(Chunk(10, 100), 150, [&](uint64_t done, uint64_t total) {
            real_reported = done == 10 && total == 100;
            ++reports;
        });
        Check(reports == 1 && real_reported, "non-byte notifications do not postpone the next real progress report");
    }
    CopyProgressReporter reporter;
    TransferRateEstimator rate;
    rate.Reset(0, 0);
    int progress_reports = 0;
    uint64_t published = 0;
    constexpr uint64_t total = 700 * mib;
    for (ULONGLONG tick = 100; tick <= 5000; tick += 100) {
        const auto publish = [&](uint64_t done, uint64_t) {
            published = (std::max)(published, done);
            rate.Observe(tick, published, total);
            ++progress_reports;
        };
        COPYFILE2_MESSAGE notification{};
        notification.Type = COPYFILE2_CALLBACK_CHUNK_STARTED;
        reporter.Observe(notification, tick - 10, publish);
        reporter.Observe(Chunk(tick * 140 * mib / 1000, total), tick, publish);
        notification.Type = COPYFILE2_CALLBACK_POLL_CONTINUE;
        reporter.Observe(notification, tick + 1, publish);
    }
    Check(progress_reports == 50, "interleaved starts and polls cannot starve real chunk reports");
    Check(published == total && rate.speed() > 139 * mib && rate.speed() < 141 * mib,
          "steady 140 MiB/s chunks retain measured throughput through notification traffic");
    CopyProgressReporter empty;
    COPYFILE2_MESSAGE finished{};
    finished.Type = COPYFILE2_CALLBACK_STREAM_FINISHED;
    int empty_reports = 0;
    empty.Observe(finished, 1, [&](uint64_t done, uint64_t size) { empty_reports += done == 0 && size == 0; });
    Check(empty_reports == 1, "a real empty stream still reports its zero-byte completion");
    CopyProgressReporter terminal;
    terminal.Observe(Chunk(10, 100), 100, [](uint64_t, uint64_t) {});
    int terminal_reports = 0;
    terminal.Observe(Chunk(100, 100), 101, [&](uint64_t done, uint64_t size) { terminal_reports += done == 100 && size == 100; });
    Check(terminal_reports == 1, "completion bypasses the progress throttle");
    std::printf("[RESULT] failures=%d\n", failures);
    return failures ? 1 : 0;
}

struct Probe {
    pulse::ops::CopyProgressReporter reporter;
    pulse::ops::TransferRateEstimator rate;
    ULONGLONG start = 0, last_log = 0;
    uint64_t total = 0, published = 0, reports = 0, false_reports = 0;
    std::array<uint64_t, 16> messages{};
    double peak = 0;
};
COPYFILE2_MESSAGE_ACTION CALLBACK TraceCopy(const COPYFILE2_MESSAGE* message, void* data) {
    auto& probe = *static_cast<Probe*>(data);
    const auto type = static_cast<size_t>(message->Type);
    if (type < probe.messages.size()) ++probe.messages[type];
    const auto tick = GetTickCount64();
    probe.reporter.Observe(*message, tick, [&](uint64_t bytes, uint64_t total) {
        ++probe.reports;
        if (total == 0 && probe.total != 0) ++probe.false_reports;
        probe.published = (std::max)(probe.published, bytes);
        probe.rate.Observe(tick, probe.published, probe.total);
        probe.peak = (std::max)(probe.peak, probe.rate.speed());
        if (tick - probe.last_log >= 250) {
            probe.last_log = tick;
            std::printf("SAMPLE,%llu,%llu,%llu,%.3f,%llu,%u\n", tick - probe.start,
                bytes, total, probe.rate.speed() / mib, probe.rate.eta_seconds(), static_cast<unsigned>(message->Type));
        }
    });
    return COPYFILE2_PROGRESS_CONTINUE;
}
int CopyProbe(const wchar_t* source, const wchar_t* destination, uint64_t bytes) {
    constexpr DWORD block_bytes = 4 * 1024 * 1024;
    if (bytes == 0 || bytes % block_bytes != 0) return 2;
    std::printf("[PROBE] preparing owned source fixture: %llu bytes\n", bytes);
    void* buffer = VirtualAlloc(nullptr, block_bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!buffer) return 2;
    auto* words = static_cast<uint32_t*>(buffer);
    uint32_t pattern = 0x7c123f91;
    for (size_t index = 0; index < block_bytes / sizeof(uint32_t); ++index) {
        pattern = pattern * 1664525u + 1013904223u;
        words[index] = pattern;
    }
    const HANDLE file = CreateFileW(source, GENERIC_WRITE, 0, nullptr, CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_NO_BUFFERING | FILE_FLAG_WRITE_THROUGH, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        std::printf("[PROBE] create source error=%lu\n", GetLastError());
        VirtualFree(buffer, 0, MEM_RELEASE);
        return 2;
    }
    bool prepared = true;
    for (uint64_t offset = 0; offset < bytes; offset += block_bytes) {
        DWORD written = 0;
        if (!WriteFile(file, buffer, block_bytes, &written, nullptr) || written != block_bytes) {
            std::printf("[PROBE] source write error=%lu\n", GetLastError());
            prepared = false;
            break;
        }
    }
    prepared = FlushFileBuffers(file) && prepared;
    CloseHandle(file);
    VirtualFree(buffer, 0, MEM_RELEASE);
    if (!prepared) return 2;
    Probe probe;
    probe.total = bytes;
    probe.start = GetTickCount64();
    probe.rate.Reset(probe.start, 0);
    COPYFILE2_EXTENDED_PARAMETERS parameters{};
    parameters.dwSize = sizeof(parameters);
    parameters.dwCopyFlags = COPY_FILE_COPY_SYMLINK | COPY_FILE_FAIL_IF_EXISTS;
    parameters.pProgressRoutine = TraceCopy;
    parameters.pvCallbackContext = &probe;
    std::printf("[PROBE] starting CopyFile2 with production flags\n");
    const HRESULT result = CopyFile2(source, destination, &parameters);
    const auto elapsed = GetTickCount64() - probe.start;
    std::printf("SUMMARY,%lu,%llu,%llu,%llu,%llu,%.3f,%.3f\n", static_cast<unsigned long>(result),
        elapsed, probe.published, probe.reports, probe.false_reports,
        probe.peak / mib, elapsed ? static_cast<double>(bytes) * 1000.0 / elapsed / mib : 0);
    for (size_t type = 0; type < probe.messages.size(); ++type)
        if (probe.messages[type]) std::printf("CALLBACK,%zu,%llu\n", type, probe.messages[type]);
    std::error_code size_error;
    const auto target_size = std::filesystem::file_size(destination, size_error);
    Check(SUCCEEDED(result) && !size_error && target_size == bytes && probe.published == bytes,
          "real cross-volume CopyFile2 completes the fixture at its exact size");
    return failures ? 1 : 0;
}
}
int wmain(int argc, wchar_t** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc == 5 && std::wstring(argv[1]) == L"--probe")
        return CopyProbe(argv[2], argv[3], std::stoull(argv[4]));
    if (argc != 1) return 2;
    return RegressionTests();
}
