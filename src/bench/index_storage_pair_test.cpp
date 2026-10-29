#include "../index/index_engine.h"
#include "../index/index_delta.h"
#include "../index/index_paths.h"
#include "../index/index_shard.h"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>

namespace pulse::index {
struct EngineTestAccess {
    static constexpr uint32_t kFilesPerVolume = 30000;
    static constexpr uint32_t kFoldersPerVolume = 24;

    static bool Build(Engine& engine) {
        engine.running_ = true;
        for (wchar_t letter : {L'C', L'D'}) {
            Engine::VolState volume;
            volume.letter = letter;
            volume.kind = VolumeKind::Fixed;
            volume.volume_id = std::wstring(L"storage-pair-") + letter;
            volume.journal_id = 17;
            std::vector<Engine::FrnNode> nodes;
            nodes.reserve(kFilesPerVolume + kFoldersPerVolume);
            for (uint32_t i = 0; i < kFoldersPerVolume; ++i) {
                Engine::FrnNode folder;
                folder.frn = 100 + i; folder.parent = 5; folder.is_dir = true;
                wchar_t name[32]{}; swprintf_s(name, L"Folder%02u", i);
                folder.name = name; nodes.push_back(std::move(folder));
            }
            for (uint32_t i = 0; i < kFilesPerVolume; ++i) {
                Engine::FrnNode file;
                file.frn = 1000 + i; file.parent = 100 + i % kFoldersPerVolume;
                wchar_t name[96]{};
                swprintf_s(name, L"%s-%06u-%s.%s", i % 7 == 0 ? L"alpha" : L"record", i,
                    i % 13 == 0 ? L"report" : L"sample", i % 3 == 0 ? L"txt" : L"log");
                file.name = name; file.size = 100 + i * 13; file.mtime = 1700000000 + i;
                nodes.push_back(std::move(file));
            }
            if (!engine.BuildMftTree(std::move(volume), 5, std::move(nodes))) return false;
        }
        engine.live_ = std::move(engine.build_);
        engine.vols_ = std::move(engine.build_vols_);
        engine.RebuildChildMapLocked(); engine.ready_ = true;
        return true;
    }

    static bool Publish(Engine& engine, const std::filesystem::path& root, uint64_t built) {
        SetActiveIndexDirectory(root.wstring());
        Engine::Store snapshot; std::vector<Engine::VolState> volumes;
        if (!engine.FlattenLocked(snapshot, volumes)) return false;
        engine.built_unix_ = built;
        const auto candidate = (root / L"candidate.bin").wstring();
        if (!engine.WriteIndexFile(candidate, snapshot, volumes, built) ||
            !engine.CommitMappedFile(candidate)) return false;
        engine.WriteVolumeShards(snapshot, volumes, built);
        engine.RefreshQueryShardsLocked();
        engine.StopPinyinWorker();
        return true;
    }

    static bool Restart(Engine& engine, const std::filesystem::path& root, bool legacy) {
        SetActiveIndexDirectory(root.wstring());
        if (legacy) engine.use_volume_query_shards_ = true;
        engine.running_ = true;
        if (!engine.TryLoadCache()) return false;
        engine.StopPinyinWorker();
        return true;
    }

    static void Usn(Engine& engine, size_t volume, uint64_t frn, uint64_t parent,
                    const wchar_t* name, DWORD reason, bool directory = false) {
        const auto length = static_cast<WORD>(wcslen(name) * sizeof(wchar_t));
        std::vector<BYTE> bytes(sizeof(USN_RECORD_V2) + length);
        auto* record = reinterpret_cast<USN_RECORD_V2*>(bytes.data());
        record->RecordLength = static_cast<DWORD>(bytes.size()); record->MajorVersion = 2;
        record->FileReferenceNumber = frn; record->ParentFileReferenceNumber = parent;
        record->FileNameOffset = static_cast<WORD>(offsetof(USN_RECORD_V2, FileName));
        record->FileNameLength = length; record->Reason = reason;
        record->FileAttributes = directory ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
        std::memcpy(bytes.data() + record->FileNameOffset, name, length);
        engine.ApplyUsnLocked(engine.vols_[volume], record);
    }

    struct Files {
        uint64_t bytes = 0;
        std::map<std::wstring, std::pair<uint64_t, uint64_t>> values;
        bool operator==(const Files& other) const { return bytes == other.bytes && values == other.values; }
    };
    static Files Inventory(const std::filesystem::path& directory) {
        Files result;
        if (!std::filesystem::exists(directory)) return result;
        for (const auto& entry : std::filesystem::recursive_directory_iterator(directory)) {
            if (!entry.is_regular_file()) continue;
            const auto bytes = entry.file_size();
            result.bytes += bytes;
            result.values.emplace(std::filesystem::relative(entry.path(), directory).wstring(),
                std::make_pair(bytes, FileCrc64(entry.path().wstring())));
        }
        return result;
    }

    static bool Same(const SearchResult& a, const SearchResult& b) {
        if (a.total != b.total || a.hits.size() != b.hits.size() ||
            a.error != b.error || a.incomplete != b.incomplete) return false;
        for (size_t i = 0; i < a.hits.size(); ++i) {
            const auto& x = a.hits[i]; const auto& y = b.hits[i];
            if (x.path != y.path || x.name != y.name || x.is_dir != y.is_dir ||
                x.size != y.size || x.mtime != y.mtime) return false;
        }
        return true;
    }

    static bool Compare(Engine& legacy, Engine& optimized, const char* phase) {
        bool ok = true;
        size_t cases = 0;
        auto compare = [&](const Query& query, const std::function<bool(const std::wstring&)>& authorize = {}) {
            const auto expected = legacy.Search(query, nullptr, 0, authorize);
            const auto actual = optimized.Search(query, nullptr, 0, authorize);
            const bool equal = Same(expected, actual);
            if (!equal) {
                std::wcout << L"[FAIL] paired query needle=" << query.needle << L" scope=" << query.path_prefix
                    << L" offset=" << query.offset << L" expected=" << expected.total << L" actual=" << actual.total << L'\n';
            }
            ok &= equal; ++cases;
        };
        Query full; full.rank = false; full.limit = kSearchPageCap;
        const auto all = legacy.Search(full);
        compare(full);
        // Entire ordered result lists also establish set equality and detect duplicate/missing rows.
        for (ResultSort sort : {ResultSort::Index, ResultSort::Name, ResultSort::Size, ResultSort::Mtime}) {
            for (bool descending : {false, true}) {
                full.sort = sort; full.sort_desc = descending; compare(full);
                Query page = full; page.limit = 257;
                for (size_t offset : {size_t{0}, size_t{257}, size_t{2048}, all.total / 2,
                                     all.total > 5 ? all.total - 5 : size_t{0}, all.total + 1}) {
                    page.offset = offset; compare(page);
                    const auto result = optimized.Search(page);
                    const auto ordered = optimized.Search(full);
                    const size_t start = (std::min)(offset, ordered.hits.size());
                    const size_t count = (std::min)(page.limit, ordered.hits.size() - start);
                    bool slice = result.total == ordered.total && result.hits.size() == count;
                    for (size_t i = 0; slice && i < count; ++i)
                        slice = result.hits[i].path == ordered.hits[start + i].path;
                    ok &= slice;
                }
            }
        }
        for (const auto* text : {L"alpha", L"report", L"*.txt", L"ext:log", L"size:>200000",
                L"alpha | report", L"!ext:txt", L"path:Folder01", L"absent-storage-pair"}) {
            Query q; q.needle = text; q.rank = false; q.limit = kSearchPageCap; compare(q);
            q.path_prefix = L"C:\\Folder01"; compare(q);
            q.path_prefix = L"D:\\Folder02"; compare(q);
            q.path_prefix = L"Z:\\missing"; compare(q);
        }
        Query folders; folders.folders_only = true; folders.rank = false;
        folders.limit = kSearchPageCap; compare(folders);
        Query ranked; ranked.needle = L"alpha"; ranked.limit = 113; compare(ranked);
        ranked.offset = 129; compare(ranked);
        Query auth; auth.rank = false; auth.limit = kSearchPageCap;
        const auto allow = [](const std::wstring& path) {
            return path.starts_with(L"D:\\") && path.find(L"Folder02") == std::wstring::npos;
        };
        compare(auth, allow);
        const auto visible = optimized.Search(auth, nullptr, 0, allow);
        ok &= !visible.hits.empty() && visible.total == visible.hits.size() &&
            std::all_of(visible.hits.begin(), visible.hits.end(), [&](const Hit& hit) { return allow(hit.path); });
        std::atomic<uint32_t> latest{2};
        const auto cancelled_a = legacy.Search(auth, &latest, 1);
        const auto cancelled_b = optimized.Search(auth, &latest, 1);
        ok &= Same(cancelled_a, cancelled_b) && cancelled_b.total == 0 && cancelled_b.hits.empty();
        // Authorizer-driven cancellation occurs after collection has begun, deterministically.
        for (Engine* engine : {&legacy, &optimized}) {
            latest = 1;
            size_t seen = 0;
            Query q; q.rank = true; q.limit = 48;
            const auto cancelled = engine->Search(q, &latest, 1, [&](const auto&) {
                if (++seen == 4) latest = 2;
                return true;
            });
            ok &= seen == 4 && cancelled.hits.empty();
        }
        std::cout << (ok ? "[PASS] " : "[FAIL] ") << phase << " paired full sets/order/pages/filter/scope/auth/cancel cases="
            << cases << '\n';
        return ok;
    }

    static bool Benchmark(Engine& engine, const char* mode, const char* phase) {
        bool ok = true;
        std::vector<double> times;
        for (size_t i = 0; i < 33; ++i) {
            engine.InvalidateFilterLocked();
            Query q; q.needle = i % 3 == 0 ? L"alpha" : i % 3 == 1 ? L"*.txt" : L"record";
            q.rank = false; q.sort = ResultSort::Name; q.offset = 100; q.limit = 128;
            const auto start = std::chrono::steady_clock::now();
            const auto result = engine.Search(q);
            const double elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            if (i >= 3) times.push_back(elapsed);
            if (result.hits.empty()) { std::cout << "[FAIL] benchmark unexpectedly empty\n"; ok = false; }
        }
        std::sort(times.begin(), times.end());
        std::cout << "[METRIC] mode=" << mode << " phase=" << phase
            << " query_samples=" << times.size() << " query_median_ms=" << (times[14] + times[15]) / 2
            << " query_p95_ms=" << times[28] << " cache=invalidated_each_sample warmup=3\n";
        return ok;
    }

    static bool Run() {
        bool ok = true;
        const auto check = [&](bool value, const char* label) {
            std::cout << (value ? "[PASS] " : "[FAIL] ") << label << '\n'; ok &= value;
        };
        SetEnvironmentVariableW(L"PULSE_INDEX_DIAGNOSTICS", L"1");
        const auto root = std::filesystem::absolute(std::filesystem::path(L"bench_data") /
            (L"index-storage-pair-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64())));
        const auto legacy_root = root / L"legacy", optimized_root = root / L"optimized";
        std::filesystem::create_directories(legacy_root); std::filesystem::create_directories(optimized_root);
        std::wcout << L"[INFO] retained isolated fixture: " << root.wstring() << L'\n';
        std::cout << "[INFO] synthetic MFT metadata files=60000 volumes=2 directories=48; no real volume scanning\n";
        SetMachineIndexScope(true);
        for (bool legacy : {true, false}) {
            const auto& directory = legacy ? legacy_root : optimized_root;
            for (uint64_t generation = 0; generation < 2; ++generation) {
                SetActiveIndexDirectory(directory.wstring());
                Engine engine;
                check(!engine.use_volume_query_shards_, "new engine defaults to aggregate query storage");
                if (legacy) engine.use_volume_query_shards_ = true;
                const auto start = std::chrono::steady_clock::now();
                const bool published = Build(engine) && Publish(engine, directory, 700000 + generation);
                const double elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
                check(published, "real BuildMftTree/flatten/write/CommitMappedFile publication");
                if (!published) return false;
                engine.CaptureMemoryState();
                const auto& memory = engine.filename_timing_.Memory().retained;
                check(memory.aggregate_mapped_file_bytes > 0 &&
                    (legacy ? memory.shard_mapped_file_bytes > 0 && engine.query_shards_ready_ && engine.query_shards_.size() == 2
                            : memory.shard_mapped_file_bytes == 0 && !engine.query_shards_ready_ && engine.query_shards_.empty()),
                    "diagnostics establish aggregate mapping plus legacy-only query mappings");
                const auto snapshots = Inventory(directory / L"v9");
                const auto query_copies = Inventory(directory / L"v9" / L"volumes");
                std::cout << "[METRIC] mode=" << (legacy ? "legacy" : "optimized") << " generations=" << generation + 1
                    << " snapshot_logical_bytes=" << snapshots.bytes << " snapshot_files=" << snapshots.values.size()
                    << " volume_copy_bytes=" << query_copies.bytes << " build_publish_ms=" << elapsed
                    << " aggregate_mapped_file_bytes=" << memory.aggregate_mapped_file_bytes
                    << " shard_mapped_file_bytes=" << memory.shard_mapped_file_bytes << '\n';
                if (!legacy) check(!std::filesystem::exists(directory / L"v9" / L"volumes"),
                    "fresh optimized publication never creates v9/volumes");
            }
        }
        {
            Engine legacy, optimized;
            const bool loaded = Restart(legacy, legacy_root, true) && Restart(optimized, optimized_root, false);
            check(loaded, "both modes restart through real machine aggregate manifest and WAL path");
            if (!loaded) return false;
            check(legacy.query_shards_ready_ && legacy.query_shards_.size() == 2 &&
                !optimized.query_shards_ready_ && optimized.query_shards_.empty(),
                "paired baseline actually selects legacy per-volume query path and optimized aggregate path");
            Query count; count.rank = false; count.limit = 0;
            check(legacy.Search(count).total == 60048 && optimized.Search(count).total == 60048,
                "independent expected full matching count excludes two hidden volume roots");
            for (const auto& expected : std::vector<std::pair<std::wstring, size_t>>{
                    {L"alpha", 8572}, {L"report", 4616}, {L"*.txt", 20000}}) {
                count.needle = expected.first;
                check(legacy.Search(count).total == expected.second && optimized.Search(count).total == expected.second,
                    "independent generated-name arithmetic verifies nonempty filtered counts");
            }
            ok &= Compare(legacy, optimized, "baseline");
            ok &= Benchmark(legacy, "legacy", "baseline"); ok &= Benchmark(optimized, "optimized", "baseline");
            for (bool old_mode : {true, false}) {
                Engine& engine = old_mode ? legacy : optimized;
                SetActiveIndexDirectory((old_mode ? legacy_root : optimized_root).wstring());
                Usn(engine, 0, 100, 101, L"MovedFolder", USN_REASON_RENAME_NEW_NAME, true);
                Usn(engine, 0, 1002, 102, L"alpha-renamed.txt", USN_REASON_RENAME_NEW_NAME);
                Usn(engine, 1, 1001, 101, L"record-000001-sample.log", USN_REASON_FILE_DELETE);
                Usn(engine, 1, 900000, 102, L"alpha-added.txt", USN_REASON_FILE_CREATE);
                for (auto& volume : engine.vols_) engine.DeltaFor(volume.letter)->QueueUsn(17, 2222);
                engine.FlushDeltas(); engine.CloseDeltas();
                Query moved; moved.needle = L"alpha-000000"; moved.path_prefix = L"C:\\Folder01\\MovedFolder";
                const auto result = engine.Search(moved);
                check(result.total == 1 && result.hits[0].path == L"C:\\Folder01\\MovedFolder\\alpha-000000-report.txt",
                    "directory overlay move resolves actual descendant path and scoped query");
                const auto renamed = engine.FindByFrnLocked(engine.vols_[0], 1002);
                const auto deleted = engine.FindByFrnLocked(engine.vols_[1], 1001);
                check(renamed >= 0 && engine.BuildPathLocked(renamed) == L"C:\\Folder02\\alpha-renamed.txt" &&
                    deleted >= 0 && engine.IsTomb(deleted) && engine.FindByFrnLocked(engine.vols_[1], 900000) >= 0,
                    "real USN overlays apply rename delete and create on the intended volumes");
            }
            ok &= Compare(legacy, optimized, "overlay");
            ok &= Benchmark(legacy, "legacy", "overlay"); ok &= Benchmark(optimized, "optimized", "overlay");
        }
        {
            Engine legacy, optimized;
            check(Restart(legacy, legacy_root, true) && Restart(optimized, optimized_root, false),
                "restart after real multi-volume WAL flush");
            check(!legacy.delta_replay_failed_ && !optimized.delta_replay_failed_ &&
                legacy.vols_.size() == 2 && optimized.vols_.size() == 2 &&
                legacy.vols_[0].next_usn == 2222 && optimized.vols_[1].next_usn == 2222,
                "real replay retains independent per-volume journal cursors");
            ok &= Compare(legacy, optimized, "WAL-restart");
            for (Engine* engine : {&legacy, &optimized}) {
                Query q; q.needle = L"alpha-added";
                const auto added = engine->Search(q);
                q.needle = L"alpha-renamed"; const auto renamed = engine->Search(q);
                check(added.total == 1 && added.hits[0].path == L"D:\\Folder02\\alpha-added.txt" &&
                    renamed.total == 1 && renamed.hits[0].path == L"C:\\Folder02\\alpha-renamed.txt" &&
                    engine->IsTomb(engine->FindByFrnLocked(engine->vols_[1], 1001)),
                    "WAL restart independently verifies added renamed and deleted identities");
            }
        }
        const auto existing_root = root / L"optimized-existing";
        std::filesystem::copy(legacy_root, existing_root, std::filesystem::copy_options::recursive);
        // Seed a deliberately stale query-copy manifest alongside still-valid legacy copies.
        const auto stale = MakeShardPaths((existing_root / L"v9" / L"volumes").wstring(), L"stale-unused-volume");
        ShardManifest stale_manifest; stale_manifest.source_id = L"stale-unused-volume"; stale_manifest.active_built = 1;
        check(SaveShardManifest(stale.manifest, stale_manifest), "seed stale unused query-copy manifest");
        const auto copies_before = Inventory(existing_root / L"v9" / L"volumes");
        {
            Engine optimized;
            check(Restart(optimized, existing_root, false), "default mode restarts with inherited legacy query copies present");
            optimized.CaptureMemoryState();
            check(!optimized.query_shards_ready_ && optimized.query_shards_.empty() &&
                optimized.filename_timing_.Memory().retained.shard_mapped_file_bytes == 0,
                "existing valid and stale copies are not adopted by default");
            optimized.CloseDeltas();
            check(Publish(optimized, existing_root, 700002), "aggregate publication succeeds over existing legacy query copies");
        }
        check(copies_before == Inventory(existing_root / L"v9" / L"volumes"),
            "all inherited volume bases/manifests including stale copy remain byte-identical after startup and publication");
        for (bool legacy : {true, false}) {
            const auto& directory = legacy ? legacy_root : optimized_root;
            const auto paths = MakeShardPaths((directory / L"v9").wstring(), L"aggregate");
            ShardManifest manifest; std::wstring active;
            const bool resolved = ResolveActiveShard(paths, manifest, active);
            check(resolved && manifest.generation >= 2, "two real aggregate A/B generations retained for fallback");
            if (!resolved) continue;
            { std::fstream corrupt(active, std::ios::in | std::ios::out | std::ios::binary); corrupt.put('X'); }
            Engine recovered;
            const bool loaded = Restart(recovered, directory, legacy);
            Query q; q.rank = false; q.limit = 0;
            check(loaded && recovered.built_unix_ == 700000 && recovered.Search(q).total == 60048,
                "corrupt active aggregate falls back to previous generation without applying incompatible newer WAL");
            q.needle = L"alpha-added";
            check(recovered.Search(q).total == 0 && recovered.delta_replay_failed_,
                "previous aggregate explicitly rejects newer-generation WAL instead of adopting added nodes");
        }
        std::cout << "[INFO] logical-byte totals count snapshot bases/manifests; excludes real WAL and filesystem allocation overhead\n";
        std::cout << "[INFO] timings cover synthetic MFT-tree construction/publication and warmed mapped queries; no full-disk scan or cold OS cache claim\n";
        return ok;
    }
};
}

int wmain() {
    return pulse::index::EngineTestAccess::Run() ? 0 : 1;
}
