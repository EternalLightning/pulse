// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

    static bool ExclusionsConcurrencyFixture() {
        bool ok = true;
        const auto check = [&](bool value, const char* label) {
            std::cout << (value ? "[PASS] " : "[FAIL] ") << label << '\n'; ok &= value;
        };
        const auto base = std::filesystem::absolute(std::filesystem::path(L"bench_data") /
            (L"exclusions-concurrency-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64())));
        const auto root = base / L"fixture", cache = base / L"cache";
        std::filesystem::create_directories(root / L"child");
        std::filesystem::create_directories(cache);
        { std::ofstream stream(root / L"keep.txt"); stream << "fixture"; }
        { std::ofstream stream(root / L"child" / L"nested.txt"); stream << "fixture"; }
        const bool previous_scope = MachineIndexScope();
        SetMachineIndexScope(false);
        SetActiveIndexDirectory(cache.wstring());
        const ULONGLONG started = GetTickCount64(), deadline = started + 30000;
        std::atomic<bool> done{false}, failed{false};
        std::atomic<unsigned> reader_rounds{0}, completed{0};
        {
            Engine engine;
            engine.fixture_root_ = root.wstring();
            engine.running_ = true;
            engine.AddForTest((root / L"keep.txt").wstring(), L"keep.txt", false, 7);
            engine.ready_ = true;
            // A substantial allocation makes a concurrent clear/free observable to
            // readers traversing the old vector. Refill is always under the lock.
            const auto fill = [&] {
                std::vector<std::wstring> paths;
                paths.reserve(32768);
                for (unsigned i = 0; i < 32768; ++i)
                    paths.push_back((base / (L"excluded-neighbor-" + std::to_wstring(i))).wstring());
                std::unique_lock lock(engine.mutex_);
                engine.excluded_paths_ = std::move(paths);
            };
            fill();
            const auto covered = [&](std::wstring_view path) {
                const auto scope = root.wstring();
                return path.size() > scope.size() && path.substr(0, scope.size()) == scope && path[scope.size()] == L'\\';
            };
            std::thread feed_reader([&] {
                while (!done && GetTickCount64() < deadline) {
                    const auto page = engine.ReadFeed(false, root.wstring(), 0, 0);
                    for (const auto& record : page.records) if (!covered(record.path)) failed = true;
                    ++reader_rounds;
                    Sleep(1);
                }
            });
            std::thread coverage_reader([&] {
                while (!done && GetTickCount64() < deadline) {
                    engine.ChangeCoverage((root / L"keep.txt").wstring());
                    {
                        std::shared_lock lock(engine.mutex_);
                        const auto count = engine.excluded_paths_.size();
                        if (count != 0 && count != 32768) failed = true;
                        if (engine.IsExcludedPath((root / L"keep.txt").wstring())) failed = true;
                        if (count && (engine.excluded_paths_.front() != (base / L"excluded-neighbor-0").wstring() ||
                                      engine.excluded_paths_.back() != (base / L"excluded-neighbor-32767").wstring())) failed = true;
                    }
                    ++reader_rounds;
                    Sleep(1);
                }
            });
            std::thread search_reader([&] {
                Query query; query.path_prefix = root.wstring(); query.needle = L"*.txt";
                query.rank = false; query.limit = 10;
                while (!done && GetTickCount64() < deadline) {
                    const auto result = engine.Search(query);
                    if (result.total > 2 || result.hits.size() != result.total) failed = true;
                    for (const auto& hit : result.hits) if (!covered(hit.path)) failed = true;
                    ++reader_rounds;
                    Sleep(1);
                }
            });
            std::thread watchdog([&] {
                while (!done && GetTickCount64() < deadline) Sleep(10);
                if (!done) {
                    failed = true;
                    engine.RequestStop();
                }
            });
            while (reader_rounds < 3 && GetTickCount64() < deadline) Sleep(1);
            for (unsigned i = 0; i < 10 && GetTickCount64() < deadline; ++i) {
                if (i) fill();
                engine.FullRebuild("exclusion_publication_fixture");
                if (!engine.running_) break;
                ++completed;
            }
            done = true;
            feed_reader.join(); coverage_reader.join(); search_reader.join(); watchdog.join();
            check(completed == 10 && GetTickCount64() < deadline,
                  "U06 ten isolated real FullRebuild publications complete within 30-second budget");
            check(reader_rounds > 10 && !failed,
                  "U06 concurrent feed coverage search and exclusion snapshots remain consistent");
            Query final; final.path_prefix = root.wstring(); final.needle = L"*.txt"; final.limit = 10; final.rank = false;
            const auto result = engine.Search(final);
            check(result.total == 2 && result.hits.size() == 2 &&
                      std::all_of(result.hits.begin(), result.hits.end(), [&](const Hit& hit) { return covered(hit.path); }),
                  "U06 final rebuilt snapshot includes both fixture files exclusively");
            {
                std::shared_lock lock(engine.mutex_);
                check(engine.excluded_paths_.empty(), "U06 user-scope rebuild publishes empty configured exclusion snapshot");
            }
            engine.Stop();
        }
        SetActiveIndexDirectory(L"");
        SetMachineIndexScope(previous_scope);
        std::error_code error;
        std::filesystem::remove_all(base, error);
        check(!error, "U06 isolated fixture and cache cleanup");
        std::cout << "[TIME] exclusion_publication_ms=" << GetTickCount64() - started
                  << " reader_rounds=" << reader_rounds.load() << '\n';
        return ok;
    }
