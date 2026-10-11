#pragma once
#include "../app/app_worker.h"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace pulse::bench::worker_net_cache {
inline std::wstring directory;
inline std::function<void()> cache_access;

struct Case {
    app::WorkerPool pool;
    std::atomic<size_t> enumerations{0}, projections{0}, rejected{0};
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<app::WorkResult> results;
    DWORD cache_thread = 0;
    size_t cache_reads = 0, checkpoints = 0;
    bool block_disk = false, disk_entered = false, release_disk = false;

    bool Start() {
        cache_access = [&] {
            std::unique_lock lock(mutex);
            cache_thread = GetCurrentThreadId();
            ++cache_reads;
            if (block_disk && !disk_entered) {
                disk_entered = true;
                changed.notify_all();
                changed.wait(lock, [&] { return release_disk; });
            }
        };
        if (!pool.Start([&](app::WorkResult result) {
            std::lock_guard lock(mutex);
            results.push_back(std::move(result));
            changed.notify_all();
        })) return false;
        app::WorkerCancellationTestProbe::SetCounters(pool, &enumerations, &projections);
        app::WorkerCancellationTestProbe::SetCheckpoint(pool, [&] {
            {
                std::lock_guard lock(mutex);
                ++checkpoints;
                changed.notify_all();
            }
            auto previous = rejected.load();
            if (enumerations.load() > previous && rejected.compare_exchange_strong(previous, previous + 1))
                throw fs::EnumerationError("isolated UNC provider failure", ERROR_BAD_NETPATH);
        });
        return true;
    }
    bool Wait(const std::function<bool()>& condition) {
        std::unique_lock lock(mutex);
        return changed.wait_for(lock, std::chrono::seconds(5), condition);
    }
    bool WaitFinal(uint64_t generation, size_t count = 1) {
        return Wait([&] {
            return static_cast<size_t>(std::count_if(results.begin(), results.end(), [&](const auto& result) {
                return result.final && (generation == 0 || result.generation == generation);
            })) >= count;
        });
    }
    void Release() {
        { std::lock_guard lock(mutex); release_disk = true; }
        changed.notify_all();
    }
    void Stop() {
        Release();
        pool.Stop();
        cache_access = {};
    }
    ~Case() { Stop(); }
};
}

inline int RunWorkerNetCacheCases() {
    using namespace pulse;
    using namespace pulse::app;
    using namespace pulse::bench::worker_net_cache;
    int failures = 0;
    const auto check = [&](bool ok, const char* label) {
        std::cout << (ok ? "[PASS] " : "[FAIL] ") << label << '\n';
        failures += !ok;
    };
    const auto root = std::filesystem::current_path() / L"bench_data" /
        (L"worker-net-cache-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    std::filesystem::create_directories(root);
    directory = root.wstring();
    const auto path = fs::NormalizePath(L"\\\\worker-net-cache.invalid\\share");
    auto disk = std::make_shared<std::vector<fs::DirEntry>>();
    disk->push_back(fs::DirEntry{L"zeta.bin"}); disk->back().size = 7;
    disk->push_back(fs::DirEntry{L"folder"}); disk->back().is_dir = true;
    disk->push_back(fs::DirEntry{L"alpha.bin"}); disk->back().size = 19;
    check(fs::SaveNetSnapshot(path, disk), "isolated UNC disk facts saved under bench_data");
    uint64_t timestamp = 0;
    check(fs::LoadNetSnapshot(path, &timestamp) && timestamp,
          "fixture stores a real disk-cache timestamp");
    const auto sorted_cache = [&](const WorkResult& result, fs::SnapshotPtr facts, uint64_t age) {
        return !result.final && !result.error && !result.cancelled && result.projection_only &&
            result.snapshot && result.facts && result.facts->size() == facts->size() &&
            result.snapshot->size() == facts->size() && result.cache_unix == age &&
            std::is_sorted(result.snapshot->begin(), result.snapshot->end(), [&](const auto& a, const auto& b) {
                return EntryLess(a, b, result.sort_column, result.sort_direction, result.folder_sort);
            });
    };
    {
        Case test;
        test.block_disk = true;
        check(test.Start(), "disk cache worker starts");
        const DWORD submitting_thread = GetCurrentThreadId();
        const auto generation = test.pool.Refresh(path, ui::SortColumn::Size, ui::SortDirection::Desc);
        check(generation && test.Wait([&] { return test.disk_entered; }) &&
              test.cache_thread != submitting_thread && !test.enumerations && test.results.empty(),
              "Refresh returns while the provider worker is blocked on disk-cache access");
        test.Release();
        check(test.WaitFinal(generation), "disk provisional is followed by a same-generation final");
        test.Stop();
        check(test.cache_reads == 1 && test.enumerations == 1 && test.results.size() == 2 &&
              test.results[0].generation == generation && sorted_cache(test.results[0], disk, timestamp) &&
              test.results[0].snapshot->front().name == L"folder" &&
              test.results[0].snapshot->at(1).name == L"alpha.bin" &&
              test.results[0].facts->front().name == L"zeta.bin" &&
              test.results[0].facts->front().size == 7 && test.results[1].generation == generation &&
              test.results[1].final && test.results[1].error_code == ERROR_BAD_NETPATH &&
              !test.results[1].snapshot && !test.results[1].cache_unix,
              "disk facts preserve metadata and timestamp, sort separately, then yield the typed final error");
    }
    auto memory = std::make_shared<std::vector<fs::DirEntry>>();
    memory->push_back(fs::DirEntry{L"current-memory.txt"});
    {
        Case test;
        check(test.Start(), "memory cache worker starts");
        const auto generation = test.pool.Refresh(path, ui::SortColumn::Name, ui::SortDirection::Asc,
                                                  FolderSortMode::FoldersFirst, memory);
        check(test.WaitFinal(generation), "memory cache refresh reaches its final");
        test.Stop();
        check(!test.cache_reads && test.results.size() == 2 &&
              sorted_cache(test.results[0], memory, 0) && test.results[0].facts == memory &&
              test.results[0].snapshot->front().name == L"current-memory.txt",
              "memory facts take priority without opening the older disk cache");
    }
    {
        Case test;
        check(test.Start(), "shared cache worker starts");
        const auto owners = WorkerCancellationTestProbe::ProviderOwners(test.pool);
        std::mutex gate_mutex;
        std::condition_variable gate_changed;
        size_t entered = 0;
        bool release = false, accepted = true;
        for (size_t i = 0; i < owners; ++i) accepted &= test.pool.EnqueueIo([&] {
            std::unique_lock lock(gate_mutex);
            ++entered; gate_changed.notify_all();
            gate_changed.wait(lock, [&] { return release; });
        });
        {
            std::unique_lock lock(gate_mutex);
            check(accepted && gate_changed.wait_for(lock, std::chrono::seconds(5), [&] { return entered == owners; }),
                  "provider owners pause before mixed cache subscribers are queued");
        }
        const auto first = test.pool.Refresh(path, ui::SortColumn::Name, ui::SortDirection::Asc);
        const auto second = test.pool.Refresh(path, ui::SortColumn::Name, ui::SortDirection::Desc,
                                              FolderSortMode::FoldersFirst, memory);
        { std::lock_guard lock(gate_mutex); release = true; }
        gate_changed.notify_all();
        check(first && second && test.WaitFinal(0, 2), "mixed subscribers finish through their shared producer");
        test.Stop();
        check(!test.cache_reads && test.enumerations == 1 && test.results.size() == 4 &&
              sorted_cache(test.results[0], memory, 0) && sorted_cache(test.results[1], memory, 0) &&
              test.results[0].facts == memory && test.results[1].facts == memory &&
              test.results[0].generation == first && test.results[1].generation == second,
              "a current memory subscriber supplies both sorts without reading disk or duplicating enumeration");
    }
    for (int scenario = 0; scenario < 3; ++scenario) {
        std::filesystem::path corrupt_file;
        if (scenario == 1) {
            for (const auto& entry : std::filesystem::directory_iterator(root))
                if (entry.path().extension() == L".bin") corrupt_file = entry.path();
            std::ofstream stream(corrupt_file, std::ios::binary | std::ios::trunc);
            stream << "broken cache";
        }
        Case test;
        check(test.Start(), "optional disk-cache failure worker starts");
        if (scenario == 2) cache_access = [] { throw std::runtime_error("isolated cache access failure"); };
        const auto requested_path = scenario == 0 ? path + L"\\missing-cache" : path;
        const auto generation = test.pool.Refresh(requested_path, ui::SortColumn::Name, ui::SortDirection::Asc);
        check(test.WaitFinal(generation), "optional cache failure still reaches directory enumeration final");
        test.Stop();
        check(test.enumerations == 1 && test.results.size() == 1 && test.results.front().final &&
              test.results.front().generation == generation && test.results.front().error &&
              test.results.front().error_code == ERROR_BAD_NETPATH && !test.results.front().cache_unix,
              scenario == 0 ? "missing disk cache publishes only the final directory error" :
              scenario == 1 ? "malformed disk cache publishes only the final directory error" :
                              "cache-access exception cannot prevent normal enumeration and its final error");
    }
    check(fs::SaveNetSnapshot(path, disk), "disk fixture restored for in-flight cancellation");
    for (int scenario = 0; scenario < 2; ++scenario) {
        Case test;
        test.block_disk = true;
        check(test.Start(), "obsolete disk-read worker starts");
        const auto old = test.pool.Refresh(path, ui::SortColumn::Name, ui::SortDirection::Asc);
        check(test.Wait([&] { return test.disk_entered; }), "old generation pauses before loading disk facts");
        uint64_t replacement = 0;
        if (scenario == 0) test.pool.Cancel(path, old);
        else {
            replacement = test.pool.Refresh(path, ui::SortColumn::Name, ui::SortDirection::Asc);
            check(replacement > old && test.WaitFinal(replacement), "new generation completes while old disk read is held");
        }
        size_t before_release = 0;
        { std::lock_guard lock(test.mutex); before_release = test.checkpoints; }
        test.Release();
        check(test.Wait([&] { return test.checkpoints > before_release; }), "released old disk read observes request cancellation");
        test.Stop();
        check(std::none_of(test.results.begin(), test.results.end(), [&](const auto& result) {
                  return result.generation == old;
              }) && (scenario == 0 ? test.results.empty() && !test.enumerations :
                  test.results.size() == 2 && test.enumerations == 1 &&
                  test.results[0].generation == replacement && !test.results[0].final),
              scenario == 0 ? "cancelled disk read publishes neither provisional nor final" :
                              "superseded disk read cannot overwrite the replacement provisional or final");
    }
    directory.clear();
    std::error_code error;
    std::filesystem::remove_all(root, error);
    check(!error && !std::filesystem::exists(root), "all workers stop before the isolated disk-cache fixture is removed");
    return failures ? 1 : 0;
}
