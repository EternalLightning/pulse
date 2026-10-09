#include "fs/fs_net_cache.h"
#include <windows.h>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <new>
#include <atomic>
#include <thread>

#ifndef PULSE_NET_CACHE_TESTING
#error Build this test with PULSE_NET_CACHE_TESTING to isolate the cache directory.
#endif

namespace {
bool limit_allocations = false;
bool large_allocation_attempted = false;
std::filesystem::path cache_directory;
int failures = 0;

void Check(bool value, const char* name) {
    std::cout << (value ? "[PASS] " : "[FAIL] ") << name << '\n';
    if (!value) ++failures;
}

bool WriteBytes(const std::filesystem::path& path, const std::vector<char>& bytes) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    stream.close();
    return static_cast<bool>(stream);
}

struct Fixture {
    ~Fixture() {
        std::error_code error;
        std::filesystem::remove_all(cache_directory, error);
        Check(!error && !std::filesystem::exists(cache_directory), "isolated cache cleaned up");
    }
};

struct ProbeBlock {
    HANDLE release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::atomic<uint32_t> entered{0};
    ~ProbeBlock() { if (release) CloseHandle(release); }
};

int ProbeCases() {
    using namespace pulse::fs;
    constexpr UINT message = WM_APP + 71;
    auto window = [] {
        return CreateWindowExW(0, L"STATIC", L"UNC probe fixture", 0,
            0, 0, 0, 0, HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
    };
    HWND hwnd = window();
    Check(hwnd != nullptr, "create isolated probe result receiver");
    if (!hwnd) return 1;
    std::vector<UncProbeResult> results;
    const auto drain = [&] {
        MSG msg{};
        while (PeekMessageW(&msg, hwnd, message, message, PM_REMOVE)) {
            std::unique_ptr<UncProbeResult> result(reinterpret_cast<UncProbeResult*>(msg.lParam));
            if (result) results.push_back(std::move(*result));
        }
    };
    const auto await = [&](auto predicate, DWORD budget = 3000) {
        const ULONGLONG deadline = GetTickCount64() + budget;
        while (!predicate() && GetTickCount64() < deadline) { drain(); Sleep(1); }
        drain();
        return predicate();
    };
    auto block = std::make_shared<ProbeBlock>();
    SetUncProbeHookForTest([block](const std::wstring&) {
        ++block->entered;
        WaitForSingleObject(block->release, INFINITE);
        return true;
    }, 30);
    const uint32_t limit = UncProbeResourcesForTest().limit;
    bool admitted = true;
    for (uint32_t i = 0; i < limit; ++i)
        admitted &= StartUncProbe(hwnd, message, L"\\\\probe-fixture.invalid\\share", i + 1);
    Check(admitted && await([&] { return block->entered.load() == limit && results.size() == limit; }),
          "blocked providers each deliver a logical timeout without real UNC access");
    bool offline = results.size() == limit;
    for (const auto& result : results)
        offline &= result.status == NetStatus::Offline && result.probe_id >= 1 && result.probe_id <= limit;
    auto usage = UncProbeResourcesForTest();
    Check(offline && usage.live == limit && usage.retired == limit,
          "timeout keeps all four actual IO owners admitted and retired");
    bool rejected = true;
    for (uint32_t i = 0; i < 32; ++i)
        rejected &= !StartUncProbe(hwnd, message, L"\\\\probe-fixture.invalid\\share", 100 + i);
    Check(rejected && block->entered.load() == limit && UncProbeResourcesForTest().live == limit,
          "repeated probes cannot bypass the physical live plus retired cap");
    drain();
    DestroyWindow(hwnd);
    Check(UncProbeResourcesForTest().live == limit,
          "consumer destruction does not release blocked provider owners");
    SetEvent(block->release);
    Check(await([&] { return UncProbeResourcesForTest().live == 0; }),
          "actual provider return releases all retired admission slots");
    hwnd = window(); results.clear();
    block = std::make_shared<ProbeBlock>();
    SetUncProbeHookForTest([block](const std::wstring&) {
        ++block->entered;
        WaitForSingleObject(block->release, INFINITE);
        return true;
    }, 30);
    Check(StartUncProbe(hwnd, message, L"\\\\late-fixture.invalid\\share", 500) &&
          await([&] { return results.size() == 1; }) && results[0].probe_id == 500 &&
          results[0].status == NetStatus::Offline, "late-result request keeps its original timeout identity");
    SetEvent(block->release);
    Check(await([&] { return results.size() == 2 && UncProbeResourcesForTest().live == 0; }) &&
          results[1].probe_id == 500 && results[1].unc == results[0].unc &&
          (results[1].status == NetStatus::Online || results[1].status == NetStatus::Slow),
          "late success is posted with the same ID after physical IO completes");
    SetUncProbeHookForTest([](const std::wstring&) { return true; }, 1000);
    results.clear();
    Check(StartUncProbe(hwnd, message, L"\\\\fast-fixture.invalid\\share", 600) &&
          await([&] { return results.size() == 1 && UncProbeResourcesForTest().live == 0; }) &&
          results[0].probe_id == 600 && results[0].status == NetStatus::Online,
          "completed provider produces one terminal result and immediately reuses admission");
    SetUncProbeHookForTest({});
    drain();
    DestroyWindow(hwnd);
    return failures ? 1 : 0;
}

int CacheCancellationCases() {
    using namespace pulse::fs;
    cache_directory = std::filesystem::current_path() / L"bench_data" /
        (L"PulseNetCancelTest-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
         std::to_wstring(GetTickCount64()));
    if (!CreateDirectoryW(cache_directory.c_str(), nullptr)) {
        Check(false, "create isolated cache cancellation fixture");
        return 1;
    }
    {
        Fixture fixture;
        const std::wstring unc = L"\\\\cache-cancellation.invalid\\share";
        auto baseline = std::make_shared<std::vector<DirEntry>>(1);
        baseline->front().name = L"original.txt";
        baseline->front().size = 7;
        Check(SaveNetSnapshot(unc, baseline), "publish baseline cache in isolated fixture");
        auto replacement = std::make_shared<std::vector<DirEntry>>(*baseline);
        replacement->front().name = L"replacement.txt";
        replacement->push_back(replacement->front());
        const auto temporary_files = [&] {
            std::vector<std::filesystem::path> files;
            for (const auto& entry : std::filesystem::directory_iterator(cache_directory)) {
                if (entry.path().filename().wstring().find(L".tmp") != std::wstring::npos)
                    files.push_back(entry.path());
            }
            std::sort(files.begin(), files.end());
            return files;
        };
        for (const unsigned cancel_at : {2u, 4u}) {
            unsigned checks = 0;
            Check(!SaveNetSnapshot(unc, replacement, [&] { return ++checks >= cancel_at; }),
                  "cancel during entry writes or before publication reports failure");
            const auto loaded = LoadNetSnapshot(unc);
            Check(checks == cancel_at && loaded && loaded->size() == 1 &&
                  loaded->front().name == L"original.txt" && temporary_files().empty(),
                  "cancelled cache write preserves previous snapshot and removes temporary file");
        }
        const bool published = SaveNetSnapshot(unc, replacement);
        const auto final = LoadNetSnapshot(unc);
        Check(published && final && final->size() == 2,
              "uncancelled cache write still publishes a complete snapshot");

        HANDLE entered = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        HANDLE release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        Check(entered && release, "create controlled concurrent cache writer gates");
        if (entered && release) {
            auto writer_a = std::make_shared<std::vector<DirEntry>>(*replacement);
            writer_a->front().name = L"cancelled-writer-a.txt";
            auto writer_b = std::make_shared<std::vector<DirEntry>>(3);
            for (size_t i = 0; i < writer_b->size(); ++i) {
                auto& entry = (*writer_b)[i];
                entry.name = L"complete-writer-b-" + std::to_wstring(i) + L".txt";
                entry.size = 100 + i;
                entry.attrs = FILE_ATTRIBUTE_ARCHIVE;
            }
            bool result_a = true;
            unsigned checks_a = 0;
            std::thread thread_a([&] {
                result_a = SaveNetSnapshot(unc, writer_a, [&] {
                    if (++checks_a != 2) return false;
                    SetEvent(entered);
                    WaitForSingleObject(release, INFINITE);
                    return true;
                });
            });
            const bool blocked = WaitForSingleObject(entered, 3000) == WAIT_OBJECT_0;
            Check(blocked, "writer A blocks in cancellation predicate during entry writes");
            if (blocked) {
                const auto temporary_a = temporary_files();
                Check(temporary_a.size() == 1,
                      "blocked writer A owns one identifiable temporary file");
                Check(SaveNetSnapshot(unc, writer_b),
                      "writer B publishes a complete snapshot while writer A remains blocked");
                Check(temporary_files() == temporary_a,
                      "writer B publication leaves writer A independent temporary file intact");
            }
            SetEvent(release);
            thread_a.join();
            Check(!result_a && checks_a == 2 && temporary_files().empty(),
                  "released writer A cancels and removes only its own temporary file");
            if (blocked) {
                const auto loaded_b = LoadNetSnapshot(unc);
                bool complete_b = loaded_b && loaded_b->size() == writer_b->size();
                if (complete_b) {
                    for (size_t i = 0; i < writer_b->size(); ++i) {
                        const auto& actual = (*loaded_b)[i];
                        const auto& expected = (*writer_b)[i];
                        complete_b &= actual.name == expected.name && actual.size == expected.size &&
                                      actual.attrs == expected.attrs && !actual.is_dir && !actual.is_reparse;
                    }
                }
                Check(complete_b,
                      "writer A cancellation preserves every entry of writer B published snapshot");
            }
        }
        if (entered) CloseHandle(entered);
        if (release) CloseHandle(release);
        const auto old_ticket = ReserveNetSnapshotWrite(unc);
        const auto new_ticket = ReserveNetSnapshotWrite(unc);
        Check(SaveNetSnapshot(unc, replacement, {}, new_ticket),
              "newer requested enumeration commits before an older result");
        Check(!SaveNetSnapshot(unc, baseline, {}, old_ticket),
              "late older enumeration cannot overwrite a newer committed cache");
        const auto ordered = LoadNetSnapshot(unc);
        Check(ordered && ordered->size() == 2 && ordered->front().name == L"replacement.txt",
              "commit ordering preserves newer snapshot contents");
        Check(!SaveNetSnapshot(L"\\\\cache-cancellation.invalid\\other", baseline, {}, new_ticket),
              "a write ticket cannot commit to another cache identity");
    }
    return failures ? 1 : 0;
}
}

// Bound the corrupt-count test before allocation, so a regression cannot consume
// hundreds of megabytes merely to discover that the cache is truncated.
void* operator new(std::size_t size) {
    if (limit_allocations && size >= 1024 * 1024) {
        large_allocation_attempted = true;
        throw std::bad_alloc();
    }
    if (void* value = std::malloc((std::max)(size, std::size_t{1}))) return value;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* value) noexcept { std::free(value); }
void operator delete[](void* value) noexcept { std::free(value); }
void operator delete(void* value, std::size_t) noexcept { std::free(value); }
void operator delete[](void* value, std::size_t) noexcept { std::free(value); }

namespace pulse::fs {
std::wstring NetCacheTestDirectory() { return cache_directory.wstring(); }
}

int wmain(int argc, wchar_t** argv) {
    if (argc == 2 && wcscmp(argv[1], L"--probes-only") == 0) return ProbeCases();
    if (argc == 2 && wcscmp(argv[1], L"--snapshot-cancel-only") == 0) return CacheCancellationCases();
    wchar_t temporary[32768]{};
    const DWORD length = GetTempPathW(static_cast<DWORD>(std::size(temporary)), temporary);
    if (!length || length >= std::size(temporary)) {
        Check(false, "temporary directory resolved");
        return 1;
    }
    cache_directory = std::filesystem::path(temporary) /
        (L"PulseNetCacheTest-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
         std::to_wstring(GetTickCount64()));
    if (!CreateDirectoryW(cache_directory.c_str(), nullptr)) {
        Check(false, "unique isolated cache created");
        return 1;
    }
    {
        Fixture fixture;
        const std::wstring unc = L"\\\\pulse-cache-test.invalid\\share\\folder";
        auto entries = std::make_shared<std::vector<pulse::fs::DirEntry>>();
        pulse::fs::DirEntry file;
        file.name = L"Unicode-\u4f60\u597d.txt";
        file.size = 0x123456789ull;
        file.attrs = FILE_ATTRIBUTE_ARCHIVE;
        file.mtime = {0x12345678, 0x87654321};
        entries->push_back(file);
        pulse::fs::DirEntry directory;
        directory.name = L"reparse-directory";
        directory.attrs = FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT;
        directory.is_dir = true;
        directory.is_reparse = true;
        entries->push_back(directory);
        const bool saved = pulse::fs::SaveNetSnapshot(unc, entries);
        Check(saved, "save isolated UNC snapshot without network access");
        uint64_t timestamp = 0;
        auto loaded = pulse::fs::LoadNetSnapshot(unc, &timestamp);
        bool same = loaded && loaded->size() == entries->size();
        if (same) {
            for (size_t i = 0; i < entries->size(); ++i) {
                const auto& actual = (*loaded)[i];
                const auto& expected = (*entries)[i];
                same = same && actual.name == expected.name && actual.size == expected.size &&
                    actual.attrs == expected.attrs && actual.is_dir == expected.is_dir &&
                    actual.is_reparse == expected.is_reparse &&
                    actual.mtime.dwLowDateTime == expected.mtime.dwLowDateTime &&
                    actual.mtime.dwHighDateTime == expected.mtime.dwHighDateTime;
            }
        }
        Check(same && timestamp > 0, "snapshot names, attributes, flags, size and timestamp roundtrip");
        std::filesystem::path cache_file;
        for (const auto& entry : std::filesystem::directory_iterator(cache_directory)) {
            if (entry.path().extension() == L".bin") cache_file = entry.path();
        }
        if (saved && !cache_file.empty()) {
            std::ifstream stream(cache_file, std::ios::binary);
            std::vector<char> valid((std::istreambuf_iterator<char>(stream)),
                                     std::istreambuf_iterator<char>());
            stream.close();
            if (valid.size() <= 20) {
                Check(false, "complete saved cache fixture read");
                return 1;
            }
            bool headers_rejected = valid.size() >= 20;
            for (size_t length_bytes = 0; length_bytes < 20; ++length_bytes) {
                auto truncated = std::vector<char>(valid.begin(), valid.begin() + length_bytes);
                timestamp = 77;
                headers_rejected = WriteBytes(cache_file, truncated) &&
                    !pulse::fs::LoadNetSnapshot(unc, &timestamp) && timestamp == 77 && headers_rejected;
            }
            Check(headers_rejected, "every truncated header is rejected without publishing timestamp");
            auto truncated_name = valid;
            truncated_name.pop_back();
            timestamp = 77;
            Check(WriteBytes(cache_file, truncated_name) &&
                  !pulse::fs::LoadNetSnapshot(unc, &timestamp) && timestamp == 77,
                  "truncated final filename is rejected without partial snapshot");
            auto corrupt_count = std::vector<char>(valid.begin(), valid.begin() + 20);
            const uint32_t count = 500000;
            std::memcpy(corrupt_count.data() + 16, &count, sizeof(count));
            const bool written = WriteBytes(cache_file, corrupt_count);
            bool rejected = false;
            limit_allocations = true;
            try {
                rejected = !pulse::fs::LoadNetSnapshot(unc);
            } catch (const std::bad_alloc&) {
            }
            limit_allocations = false;
            Check(written && rejected && !large_allocation_attempted,
                  "malicious in-range count rejected before large allocation");
            Check(WriteBytes(cache_file, valid) && pulse::fs::LoadNetSnapshot(unc) != nullptr,
                  "valid snapshot remains readable after corrupt fixtures");
        } else {
            Check(false, "isolated cache file found for corruption tests");
        }
    }
    return failures == 0 ? 0 : 1;
}
