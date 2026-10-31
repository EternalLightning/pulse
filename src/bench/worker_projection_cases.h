// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once
#include "../app/app_worker.h"
#include "../app/directory_projection.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

inline int RunWorkerProjectionCases() {
    using namespace pulse;
    using namespace pulse::app;
    int failures = 0;
    const auto check = [&](bool ok, const char* label) {
        std::cout << (ok ? "[PASS] " : "[FAIL] ") << label << '\n';
        failures += !ok;
    };
    const auto root = std::filesystem::current_path() / L"bench_data" /
        (L"worker-projection-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    std::filesystem::create_directories(root);
    constexpr size_t count = 180;
    for (size_t i = 0; i < count; ++i) {
        std::ofstream file(root / (std::to_wstring(i) + L".bin"), std::ios::binary);
        file << std::string(i + 1, 'x');
    }
    std::atomic<size_t> enumerations{0}, projections{0};
    fs::SnapshotPtr shared_facts;
    for (int scenario = 0; scenario < 4; ++scenario) {
        std::mutex mutex;
        std::condition_variable changed;
        bool reached = false, release = false;
        size_t enumeration_checks = 0;
        std::vector<WorkResult> results;
        WorkerPool pool;
        enumerations = 0; projections = 0;
        check(pool.Start([&](WorkResult result) {
            std::lock_guard lock(mutex);
            results.push_back(std::move(result));
            changed.notify_all();
        }), "projection pool starts with existing owner lanes");
        WorkerCancellationTestProbe::SetCounters(pool, &enumerations, &projections);
        WorkerCancellationTestProbe::SetCheckpoint(pool, [&] {
            if (scenario == 2 || !enumerations.load()) return;
            std::unique_lock lock(mutex);
            if (reached || ++enumeration_checks < 40) return;
            reached = true;
            changed.notify_all();
            changed.wait(lock, [&] { return release; });
        });
        if (scenario == 2) WorkerCancellationTestProbe::SetFactsCheckpoint(pool, [&] {
            std::unique_lock lock(mutex);
            if (reached) return;
            reached = true;
            changed.notify_all();
            changed.wait(lock, [&] { return release; });
        });
        const auto first = pool.Refresh(root.wstring(), ui::SortColumn::Name, ui::SortDirection::Asc);
        {
            std::unique_lock lock(mutex);
            check(changed.wait_for(lock, std::chrono::seconds(5), [&] { return reached; }),
                  "real directory enumeration reaches an in-flight checkpoint");
        }
        const auto second = pool.Refresh(root.wstring(), ui::SortColumn::Name, ui::SortDirection::Desc);
        uint64_t replacement = 0;
        if (scenario == 1) pool.Cancel(root.wstring(), first);
        if (scenario == 2) {
            { std::ofstream file(root / L"refreshed.bin", std::ios::binary); file << "new facts"; }
            replacement = pool.Refresh(root.wstring(), ui::SortColumn::Name, ui::SortDirection::Asc);
        }
        if (scenario == 3) {
            pool.Cancel(root.wstring(), first);
            pool.Cancel(root.wstring(), second);
            replacement = pool.Refresh(root.wstring(), ui::SortColumn::Name, ui::SortDirection::Asc);
        }
        {
            std::lock_guard lock(mutex);
            release = true;
        }
        changed.notify_all();
        {
            std::unique_lock lock(mutex);
            const size_t expected = scenario == 1 || scenario == 3 ? 1 : 2;
            check(changed.wait_for(lock, std::chrono::seconds(5), [&] { return results.size() == expected; }),
                  "all current projections finish from the shared producer");
        }
        pool.Stop();
        check(enumerations == (scenario >= 2 ? 2u : 1u) &&
              projections == (scenario == 1 || scenario == 3 ? 1u : 2u),
              scenario == 3 ? "abandoned group is replaced by one fresh producer and one current projection" :
              scenario == 2 ? "same-key Refresh starts a second enumeration while the other old sort finishes" :
                              "in-flight same-path requests enumerate once and project each current sort once");
        bool correct = first && second && first != second;
        for (const auto& result : results) {
            const auto expected_count = count + (scenario == 2 && result.generation == replacement ? 1u : 0u);
            correct = correct && result.final && !result.projection_only && !result.error && !result.cancelled &&
                result.snapshot && result.snapshot->size() == expected_count && result.facts &&
                result.facts->size() == expected_count &&
                std::is_sorted(result.snapshot->begin(), result.snapshot->end(), [&](const auto& a, const auto& b) {
                    return EntryLess(a, b, result.sort_column, result.sort_direction, result.folder_sort, result.path);
                });
            if (scenario) correct = correct && result.generation != first;
            if (result.sort_direction == ui::SortDirection::Desc) correct = correct && result.generation == second;
            if (scenario >= 2 && result.sort_direction == ui::SortDirection::Asc)
                correct = correct && result.generation == replacement;
        }
        if (scenario == 0 && results.size() == 2) {
            check(results[0].facts == results[1].facts,
                  "both independently sorted results retain the same immutable enumeration facts");
            shared_facts = results[0].facts;
        }
        if (scenario == 2) {
            check(results.size() == 2 && results[0].facts != results[1].facts &&
                std::any_of(results.begin(), results.end(), [&](const WorkResult& result) {
                    return result.generation == replacement && result.facts &&
                        std::any_of(result.facts->begin(), result.facts->end(),
                            [](const fs::DirEntry& entry) { return entry.name == L"refreshed.bin"; });
                }) && std::any_of(results.begin(), results.end(), [&](const WorkResult& result) {
                    return result.generation == second && result.facts &&
                        std::none_of(result.facts->begin(), result.facts->end(),
                            [](const fs::DirEntry& entry) { return entry.name == L"refreshed.bin"; });
                }), "replacement observes new filesystem contents while the old subscriber keeps its old facts");
            std::filesystem::remove(root / L"refreshed.bin");
        }
        check(correct, scenario == 1 ? "cancelling one sort leaves the other sort current" :
              scenario == 2 ? "superseding one sort suppresses its stale generation only" :
              scenario == 3 ? "cancelling all subscribers closes their group before a fresh enumeration" :
                              "different sorts keep distinct generations and correct orders");
    }
    {
        { std::ofstream file(root / L"project-fresh.bin", std::ios::binary); file << "fresh facts"; }
        std::mutex mutex;
        std::condition_variable changed;
        std::vector<WorkResult> results;
        bool reached = false, release = false;
        WorkerPool pool;
        enumerations = 0; projections = 0;
        check(pool.Start([&](WorkResult result) {
            std::lock_guard lock(mutex);
            results.push_back(std::move(result));
            changed.notify_all();
        }), "active enumeration projection pool starts");
        WorkerCancellationTestProbe::SetCounters(pool, &enumerations, &projections);
        WorkerCancellationTestProbe::SetFactsCheckpoint(pool, [&] {
            std::unique_lock lock(mutex);
            if (reached) return;
            reached = true;
            changed.notify_all();
            changed.wait(lock, [&] { return release; });
        });
        const auto refresh = pool.Refresh(root.wstring(), ui::SortColumn::Name, ui::SortDirection::Asc);
        {
            std::unique_lock lock(mutex);
            check(changed.wait_for(lock, std::chrono::seconds(5), [&] { return reached; }),
                  "fresh enumeration finishes capturing facts before the sort changes");
        }
        const auto descending = pool.Project(root.wstring(), shared_facts, ui::SortColumn::Name, ui::SortDirection::Desc);
        const auto shared_descending = pool.Project(root.wstring(), shared_facts, ui::SortColumn::Name, ui::SortDirection::Desc);
        check(shared_descending == descending,
              "identical Project target borrows the live projection generation without stranding its original subscriber");
        pool.Cancel(root.wstring(), refresh);
        const auto ascending = pool.Project(root.wstring(), shared_facts, ui::SortColumn::Name, ui::SortDirection::Asc);
        pool.Cancel(root.wstring(), descending);
        {
            std::lock_guard lock(mutex);
            release = true;
        }
        changed.notify_all();
        {
            std::unique_lock lock(mutex);
            check(changed.wait_for(lock, std::chrono::seconds(5), [&] { return results.size() == 1; }),
                  "latest sort completes after previous refresh and sort subscriptions are cancelled");
        }
        pool.Stop();
        check(refresh && descending > refresh && ascending > descending && enumerations == 1 && projections == 1 &&
            results.size() == 1 && results[0].generation == ascending && results[0].final &&
            !results[0].projection_only && !results[0].error && results[0].snapshot &&
            results[0].snapshot->size() == count + 1 && results[0].facts != shared_facts &&
            std::any_of(results[0].snapshot->begin(), results[0].snapshot->end(),
                [](const fs::DirEntry& entry) { return entry.name == L"project-fresh.bin"; }),
            "rapid Project subscriptions keep the active producer alive and publish fresh facts instead of stale input");
        std::filesystem::remove(root / L"project-fresh.bin");
    }
    for (int scenario = 0; scenario < 3; ++scenario) {
        std::mutex mutex;
        std::condition_variable changed;
        bool reached = false, release = false;
        std::vector<WorkResult> results;
        WorkerPool pool;
        check(pool.Start([&](WorkResult result) {
            if (!result.final) return;
            std::lock_guard lock(mutex);
            results.push_back(std::move(result));
            changed.notify_all();
        }), "running subscription reclamation pool starts");
        WorkerCancellationTestProbe::SetFactsCheckpoint(pool, [&] {
            std::unique_lock lock(mutex);
            if (reached) return;
            reached = true;
            changed.notify_all();
            changed.wait(lock, [&] { return release; });
        });
        auto cached = std::make_shared<std::vector<fs::DirEntry>>();
        cached->push_back(fs::DirEntry{L"obsolete running subscription cache"});
        std::weak_ptr<const std::vector<fs::DirEntry>> retained = cached;
        const auto first = pool.Refresh(root.wstring(), ui::SortColumn::Name, ui::SortDirection::Asc,
                                        FolderSortMode::FoldersFirst, cached);
        cached.reset();
        {
            std::unique_lock lock(mutex);
            check(changed.wait_for(lock, std::chrono::seconds(5), [&] { return reached; }),
                  "running producer pauses after provisional cache use and fresh enumeration");
        }
        check(!retained.expired(), "current running subscription still owns its cached facts");
        uint64_t other = 0, replacement = 0;
        if (scenario != 0)
            other = pool.Project(root.wstring(), shared_facts, ui::SortColumn::Name, ui::SortDirection::Desc);
        if (scenario == 2)
            replacement = pool.Refresh(root.wstring(), ui::SortColumn::Name, ui::SortDirection::Asc);
        else pool.Cancel(root.wstring(), first);
        check(retained.expired(), scenario == 2 ?
              "refresh replacement releases obsolete running subscriber cache before producer resumes" :
              "cancellation releases obsolete running subscriber cache before producer resumes");
        {
            std::lock_guard lock(mutex);
            release = true;
        }
        changed.notify_all();
        const size_t expected = scenario == 0 ? 0 : scenario == 1 ? 1 : 2;
        if (expected) {
            std::unique_lock lock(mutex);
            check(changed.wait_for(lock, std::chrono::seconds(5), [&] { return results.size() >= expected; }),
                  "remaining running subscriptions finish after obsolete cache reclamation");
        }
        pool.Stop();
        bool correct = first && results.size() == expected;
        for (const auto& result : results)
            correct = correct && (result.generation == other || result.generation == replacement) &&
                result.generation != first && !result.error && !result.cancelled && result.snapshot &&
                result.snapshot->size() == count && result.facts != shared_facts;
        if (scenario != 0)
            correct = correct && std::count_if(results.begin(), results.end(), [&](const WorkResult& result) {
                return result.generation == other;
            }) == 1;
        check(correct, "running cache reclamation preserves valid Project subscribers and suppresses obsolete generations");
    }
    {
        std::mutex mutex;
        std::condition_variable changed;
        size_t entered = 0, finished = 0;
        bool release = false;
        WorkerPool pool;
        check(pool.Start([](WorkResult) {}), "queue reclamation pool starts");
        const auto owners = WorkerCancellationTestProbe::ProviderOwners(pool);
        bool accepted = owners != 0;
        for (size_t i = 0; i < owners; ++i) accepted = pool.EnqueueIo([&] {
            std::unique_lock lock(mutex);
            ++entered;
            changed.notify_all();
            changed.wait(lock, [&] { return release; });
            ++finished;
            changed.notify_all();
        }) && accepted;
        {
            std::unique_lock lock(mutex);
            check(accepted && changed.wait_for(lock, std::chrono::seconds(5), [&] { return entered == owners; }),
                  "all provider owners block before synthetic UNC navigation requests");
        }
        bool bounded = true;
        for (size_t i = 0; i < 64; ++i) {
            auto cached = std::make_shared<std::vector<fs::DirEntry>>();
            cached->push_back(fs::DirEntry{L"cancelled cached facts"});
            std::weak_ptr<const std::vector<fs::DirEntry>> retained = cached;
            const auto path = L"\\\\pulse-never-enumerate\\share\\queued-" + std::to_wstring(i);
            const auto generation = pool.Refresh(path, ui::SortColumn::Name, ui::SortDirection::Asc,
                                                 FolderSortMode::FoldersFirst, cached);
            cached.reset();
            bounded = bounded && generation && WorkerCancellationTestProbe::PendingDirectoryGroups(pool) == 1;
            pool.Cancel(path, generation);
            bounded = bounded && WorkerCancellationTestProbe::PendingDirectoryGroups(pool) == 0 && retained.expired();
        }
        check(bounded, "64 cancelled queued navigation groups and cached fact objects are reclaimed immediately");
        const std::wstring path = L"\\\\pulse-never-enumerate\\share\\active";
        auto cached = std::make_shared<std::vector<fs::DirEntry>>();
        std::weak_ptr<const std::vector<fs::DirEntry>> retained = cached;
        const auto old = pool.Refresh(path, ui::SortColumn::Name, ui::SortDirection::Asc,
                                      FolderSortMode::FoldersFirst, cached);
        cached.reset();
        const auto other = pool.Refresh(path, ui::SortColumn::Name, ui::SortDirection::Desc);
        pool.Cancel(path, old);
        check(retained.expired() && WorkerCancellationTestProbe::PendingDirectoryGroups(pool) == 1,
              "cancelling cached subscriber releases its object while retaining the active shared producer");
        pool.Cancel(path, other);
        check(WorkerCancellationTestProbe::PendingDirectoryGroups(pool) == 0,
              "cancelling the last queued subscriber removes its producer");
        const auto first_group_asc = pool.Refresh(path, ui::SortColumn::Name, ui::SortDirection::Asc);
        const auto first_group_desc = pool.Refresh(path, ui::SortColumn::Name, ui::SortDirection::Desc);
        const auto second_group_asc = pool.Refresh(path, ui::SortColumn::Name, ui::SortDirection::Asc);
        bool separate_groups = first_group_asc && first_group_desc && second_group_asc &&
            WorkerCancellationTestProbe::PendingDirectoryGroups(pool) == 2;
        const auto second_group_desc = pool.Refresh(path, ui::SortColumn::Name, ui::SortDirection::Desc);
        separate_groups = separate_groups && second_group_desc &&
            WorkerCancellationTestProbe::PendingDirectoryGroups(pool) == 1;
        check(separate_groups,
              "fresh producer retains the active old sort and accepts new different sorts as one group");
        pool.Cancel(path, second_group_asc);
        pool.Cancel(path, second_group_desc);
        bool superseded = true;
        uint64_t current = 0;
        for (size_t i = 0; i < 64; ++i) {
            auto source = std::make_shared<std::vector<fs::DirEntry>>();
            std::weak_ptr<const std::vector<fs::DirEntry>> stale_source = source;
            current = pool.Refresh(path, ui::SortColumn::Name, ui::SortDirection::Asc,
                                   FolderSortMode::FoldersFirst, source);
            source.reset();
            const auto fresh = pool.Refresh(path, ui::SortColumn::Name, ui::SortDirection::Asc);
            superseded = superseded && fresh > current && stale_source.expired() &&
                WorkerCancellationTestProbe::PendingDirectoryGroups(pool) == 1;
            current = fresh;
        }
        check(superseded, "same-key queued refreshes replace producers without retaining superseded cached objects");
        pool.Cancel(path, current);
        {
            std::lock_guard lock(mutex);
            release = true;
        }
        changed.notify_all();
        {
            std::unique_lock lock(mutex);
            check(changed.wait_for(lock, std::chrono::seconds(5), [&] { return finished == owners; }),
                  "blocked provider fixtures release with no uncancelled synthetic UNC producer");
        }
        pool.Stop();
    }
    {
        std::mutex mutex;
        std::condition_variable changed;
        std::vector<WorkResult> results;
        WorkerPool pool;
        enumerations = 0; projections = 0;
        check(pool.Start([&](WorkResult result) {
            std::lock_guard lock(mutex);
            results.push_back(std::move(result));
            changed.notify_all();
        }), "source projection pool starts");
        WorkerCancellationTestProbe::SetCounters(pool, &enumerations, &projections);
        const auto asc = pool.Project(root.wstring(), shared_facts, ui::SortColumn::Name, ui::SortDirection::Asc);
        const auto desc = pool.Project(root.wstring(), shared_facts, ui::SortColumn::Name, ui::SortDirection::Desc);
        {
            std::unique_lock lock(mutex);
            check(changed.wait_for(lock, std::chrono::seconds(5), [&] { return results.size() == 2; }),
                  "Project produces both independent background orders");
        }
        pool.Stop();
        check(asc && desc && asc != desc && !enumerations && projections == 2 &&
              results.size() == 2 && results[0].projection_only && results[1].projection_only &&
              results[0].facts == shared_facts && results[1].facts == shared_facts,
              "Project owns supplied facts and performs no enumeration");
        const auto watermark = pool.ResultWatermark();
        check(!pool.Project(root.wstring(), shared_facts, ui::SortColumn::Name, ui::SortDirection::Asc) &&
              pool.ResultWatermark() == watermark + 1,
              "rejected Project advances the watermark");
    }
    for (int scenario = 0; scenario < 3; ++scenario) {
        std::mutex mutex;
        std::condition_variable changed;
        std::vector<WorkResult> results;
        bool reached = false, release = false;
        std::atomic<bool> throw_once{false};
        WorkerPool pool;
        enumerations = 0; projections = 0;
        check(pool.Start([&](WorkResult result) {
            std::lock_guard lock(mutex);
            results.push_back(std::move(result));
            changed.notify_all();
        }), "cached projection pool starts");
        WorkerCancellationTestProbe::SetCounters(pool, &enumerations, &projections);
        WorkerCancellationTestProbe::SetCheckpoint(pool, [&] {
            if (!enumerations.load()) return;
            if (scenario == 2 && !throw_once.exchange(true)) throw std::runtime_error("isolated provider failure");
            if (scenario) return;
            std::unique_lock lock(mutex);
            if (reached) return;
            reached = true;
            changed.notify_all();
            changed.wait(lock, [&] { return release; });
        });
        const auto path = scenario == 1 ? (root / L"missing" / L"child").wstring() : root.wstring();
        const auto generation = pool.Refresh(path, ui::SortColumn::Name, ui::SortDirection::Desc,
                                             FolderSortMode::FoldersFirst, shared_facts);
        if (scenario == 0) {
            std::unique_lock lock(mutex);
            check(changed.wait_for(lock, std::chrono::seconds(5), [&] { return reached; }) &&
                  results.size() == 1 && results[0].generation == generation && !results[0].final &&
                  results[0].snapshot && results[0].snapshot->size() == count,
                  "sorted same-generation cache publishes before blocked real enumeration completes");
            release = true;
            changed.notify_all();
        }
        {
            std::unique_lock lock(mutex);
            check(changed.wait_for(lock, std::chrono::seconds(5), [&] { return results.size() == 2; }),
                  "cached provisional result is followed by a final result");
        }
        pool.Stop();
        bool correct = results.size() == 2 && !results[0].final && results[1].final &&
            results[0].generation == generation && results[1].generation == generation && !results[0].error;
        if (correct && scenario == 0) correct = !results[1].error && results[1].facts->size() == count;
        if (correct && scenario == 1) correct = results[1].error && results[1].error_code == ERROR_PATH_NOT_FOUND;
        if (correct && scenario == 2) correct = results[1].error && results[1].error_code == ERROR_GEN_FAILURE;
        check(correct, scenario == 0 ? "provisional does not retire the generation before the final listing" :
              scenario == 1 ? "final filesystem error preserves its numeric code after cached publication" :
                              "unknown enumeration failure publishes a final general failure after cache");
    }
    WorkResult cache{};
    cache.path = L"\\\\pulse-no-cache-write\\share";
    cache.generation = 1;
    cache.snapshot = shared_facts;
    cache.final = false;
    size_t callbacks = 0, writes = 0;
    WorkerCancellationTestProbe::Publish(cache, false, false, &callbacks, &writes);
    check(callbacks == 1 && !writes, "provisional UNC publication never writes the disk cache");
    std::error_code error;
    std::filesystem::remove_all(root, error);
    check(!error, "isolated projection fixture cleaned");
    return failures ? 1 : 0;
}
