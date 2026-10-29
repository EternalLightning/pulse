#include "../index/index_engine.h"
#include "../index/index_postings.h"
#include "../index/index_paths.h"
#include "../index/index_shard.h"
#include "../index/index_name_pool.h"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <psapi.h>

namespace pulse::index {
struct EngineTestAccess {
    static inline size_t checks = 0, failures = 0;
    static bool Check(bool value, const char* label) {
        ++checks;
        if (!value) ++failures;
        std::cout << (value ? "[PASS] " : "[FAIL] ") << label << '\n';
        return value;
    }
    static uint64_t Hash(const void* data, size_t length) {
        uint64_t hash = 14695981039346656037ull;
        const auto* bytes = static_cast<const uint8_t*>(data);
        for (size_t i = 0; i < length; ++i) { hash ^= bytes[i]; hash *= 1099511628211ull; }
        return hash;
    }
    static bool SamePostings(const Engine::MappedFile& a, const Engine::MappedFile& b, bool pair) {
        const auto* a_start = pair ? a.prefix2_start : a.prefix1_start;
        const auto* b_start = pair ? b.prefix2_start : b.prefix1_start;
        const auto* a_raw = pair ? a.prefix2_ids : a.prefix1_ids;
        const auto* b_raw = pair ? b.prefix2_ids : b.prefix1_ids;
        const auto* a_bytes = pair ? a.prefix2_bytes : a.prefix1_bytes;
        const auto* b_bytes = pair ? b.prefix2_bytes : b.prefix1_bytes;
        if (!a_start || !b_start) return a_start == b_start;
        for (uint32_t bucket = 0; bucket < 65536; ++bucket) {
            uint32_t ap = a_start[bucket], bp = b_start[bucket], ai = 0, bi = 0;
            const uint32_t ae = a_start[bucket + 1], be = b_start[bucket + 1];
            while (ap < ae && bp < be) {
                if (a_bytes) { if (!postings::Next({a_bytes, ae}, ap, ai)) return false; }
                else ai = static_cast<uint32_t>(a_raw[ap++]);
                if (b_bytes) { if (!postings::Next({b_bytes, be}, bp, bi)) return false; }
                else bi = static_cast<uint32_t>(b_raw[bp++]);
                if (ai != bi) return false;
            }
            if (ap != ae || bp != be) return false;
        }
        return true;
    }
    static bool SameSnapshot(const Engine::MappedFile& a, const Engine::MappedFile& b, bool interned = false) {
        bool ok = Check(a.n == b.n && a.nvol == b.nvol && a.nfrn == b.nfrn &&
            a.hdr->built_unix == b.hdr->built_unix && (interned || a.hdr->pool_chars == b.hdr->pool_chars),
            "built timestamp and node/volume/FRN counts preserved; plain conversion also preserves pool count");
        if (!ok) return false;
        auto same = [&](const void* x, const void* y, size_t bytes, const char* label) {
            std::cout << "[HASH] " << label << " bytes=" << bytes << " before=" << Hash(x, bytes)
                << " after=" << Hash(y, bytes) << '\n';
            ok &= Check(std::memcmp(x, y, bytes) == 0, label);
        };
        if (interned) {
            bool nodes = true;
            for (uint32_t i = 0; i < a.n; ++i) {
                Node an = a.nodes[i], bn = b.nodes[i];
                const std::wstring_view before(a.pool + an.off, an.len), after(b.pool + bn.off, bn.len);
                an.off = bn.off = 0;
                nodes = nodes && before == after && std::memcmp(&an, &bn, sizeof(Node)) == 0;
            }
            ok &= Check(nodes, "interning preserves every node ID, parent, flags, subtree span and exact UTF-16 name; only off may change");
            ok &= Check(b.hdr->pool_chars <= a.hdr->pool_chars, "interned name pool does not grow");
        } else same(a.nodes, b.nodes, sizeof(Node) * a.n, "node bytes identical");
        same(a.attrs, b.attrs, sizeof(Attr) * a.n, "attribute bytes identical");
        if (!interned) same(a.pool, b.pool, sizeof(wchar_t) * static_cast<size_t>(a.hdr->pool_chars), "name pool bytes identical");
        same(a.child_order, b.child_order, sizeof(int32_t) * a.n, "child order bytes identical");
        if (a.nfrn) same(a.frns, b.frns, sizeof(DiskFrn) * a.nfrn, "FRN identity bytes identical");
        bool volumes = true;
        for (uint32_t i = 0; i < a.nvol; ++i) {
            DiskVol av = a.vols[i], bv = b.vols[i];
            av.frn_off = bv.frn_off = 0;
            volumes &= std::memcmp(&av, &bv, sizeof(av)) == 0;
        }
        ok &= Check(volumes, "all volume IDs, roots, spans, counts, journal IDs and USN cursors identical");
        ok &= Check(a.prefix1_all_chars == b.prefix1_all_chars && SamePostings(a, b, false),
            "all single-character posting IDs exactly equal bucket by bucket");
        ok &= Check(SamePostings(a, b, true), "all bigram posting IDs exactly equal bucket by bucket");
        return ok;
    }
    static bool SameResult(const SearchResult& a, const SearchResult& b) {
        if (a.total != b.total || a.hits.size() != b.hits.size() || a.error != b.error || a.incomplete != b.incomplete)
            return false;
        for (size_t i = 0; i < a.hits.size(); ++i) {
            const auto& x = a.hits[i]; const auto& y = b.hits[i];
            if (x.path != y.path || x.name != y.name || x.is_dir != y.is_dir ||
                x.size != y.size || x.mtime != y.mtime) return false;
        }
        return true;
    }
    static bool CompareQueries(Engine& a, Engine& b, bool thorough) {
        size_t cases = 0;
        bool ok = true;
        std::vector<std::wstring> terms{L"", L"a", L"o", L"0", L"alpha", L"record", L"report", L"*.txt",
            L"ext:log", L"size:>200000", L"!ext:txt", L"alpha | report", L"path:Folder01", L"absent-compaction", L"中", L"compact-"};
        // Sample actual names so a real input snapshot also has nonempty name queries.
        for (uint32_t i = 1; i < a.map_->n && i < 100; i += 19) {
            const auto name = a.NameOf(static_cast<int32_t>(i));
            if (name.size() >= 2) terms.emplace_back(name.substr(0, 2));
        }
        std::vector<double> old_times, new_times;
        for (const auto& text : terms) {
            const auto sorts = thorough ? std::vector<ResultSort>{ResultSort::Index, ResultSort::Name, ResultSort::Size, ResultSort::Mtime}
                                        : std::vector<ResultSort>{ResultSort::Index};
            for (auto sort : sorts) {
                Query q; q.needle = text; q.rank = false; q.sort = sort; q.limit = thorough ? 257 : 32;
                for (size_t offset : {size_t{0}, size_t{17}}) {
                    q.offset = offset;
                    a.InvalidateFilterLocked(); b.InvalidateFilterLocked();
                    auto start = std::chrono::steady_clock::now(); const auto ar = a.Search(q);
                    old_times.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
                    start = std::chrono::steady_clock::now(); const auto br = b.Search(q);
                    new_times.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
                    ok &= SameResult(ar, br); ++cases;
                    if (thorough) {
                        q.sort_desc = true; ok &= SameResult(a.Search(q), b.Search(q)); ++cases; q.sort_desc = false;
                    }
                }
            }
        }
        if (thorough) {
            for (const auto* scope : {L"C:\\Folder01", L"D:\\Folder02", L"Z:\\absent"}) {
                Query q; q.needle = L"a"; q.path_prefix = scope; q.limit = 123;
                ok &= SameResult(a.Search(q), b.Search(q)); ++cases;
            }
            Query q; q.needle = L"a"; q.limit = 123;
            const auto authorize = [](const std::wstring& path) { return path.starts_with(L"D:\\"); };
            ok &= SameResult(a.Search(q, nullptr, 0, authorize), b.Search(q, nullptr, 0, authorize)); ++cases;
            std::atomic<uint32_t> latest{2};
            ok &= SameResult(a.Search(q, &latest, 1), b.Search(q, &latest, 1)); ++cases;
        }
        auto metrics = [&](std::vector<double>& values, const char* mode) {
            std::sort(values.begin(), values.end());
            std::cout << "[METRIC] query_mode=" << mode << " samples=" << values.size()
                << " median_ms=" << values[values.size() / 2] << " p95_ms=" << values[values.size() * 95 / 100] << '\n';
        };
        metrics(old_times, "old"); metrics(new_times, "compressed");
        std::cout << "[METRIC] compared_query_cases=" << cases << '\n';
        return Check(ok, "old/compressed query counts, result metadata, order, pages, scopes, authorization and cancellation agree");
    }
    static void Memory(const char* stage) {
        PROCESS_MEMORY_COUNTERS_EX counters{};
        counters.cb = sizeof(counters);
        if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters))) {
            std::cout << "[MEMORY] stage=" << stage << " private_bytes=" << counters.PrivateUsage
                << " working_set_bytes=" << counters.WorkingSetSize << " peak_working_set_bytes=" << counters.PeakWorkingSetSize << '\n';
        } else std::cout << "[MEMORY] stage=" << stage << " error=" << GetLastError() << '\n';
    }
    static bool Convert(const std::filesystem::path& input, const std::filesystem::path& output, bool thorough, bool interned = false) {
        if (!Check(input.is_absolute() && output.is_absolute() && !std::filesystem::exists(output) &&
            !std::filesystem::exists(output.wstring() + L".tmp"), "absolute input/output and previously absent output/tmp required")) return false;
        Engine a, b;
        if (interned) Memory("before_map");
        auto start = std::chrono::steady_clock::now();
        if (!Check(a.MapIndexFile(input.wstring(), a.map_), "production mapper accepts input old/new snapshot")) return false;
        const double old_map = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        if (interned) Memory("input_mapped");
        Engine::Store store;
        const auto& m = *a.map_;
        store.nodes.assign(m.nodes, m.nodes + m.n);
        store.attrs.assign(m.attrs, m.attrs + m.n);
        if (interned) {
            // The source mapping owns key storage. Output pool growth cannot
            // invalidate these views, and no second copy of the old pool is made.
            SnapshotNameInterner names(store.pool);
            const auto began = std::chrono::steady_clock::now();
            for (auto& node : store.nodes) {
                const std::wstring_view name(m.pool + node.off, node.len);
                node.off = names.Add(name);
            }
            std::cout << "[METRIC] intern_unique_names=" << names.Count() << " before_pool_chars=" << m.hdr->pool_chars
                << " after_pool_chars=" << store.pool.size() << " intern_ms="
                << std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - began).count() << '\n';
            Memory("intern_dictionary_live");
        } else store.pool.assign(m.pool, m.pool + m.hdr->pool_chars);
        if (interned) Memory("intern_dictionary_released");
        std::vector<Engine::VolState> volumes;
        for (uint32_t i = 0; i < m.nvol; ++i) {
            const auto& dv = m.vols[i];
            Engine::VolState v;
            v.letter = static_cast<wchar_t>(dv.letter); v.kind = static_cast<VolumeKind>(dv.kind);
            v.item_count = dv.item_count; v.volume_id = dv.volume_id; v.journal_id = dv.journal_id;
            v.next_usn = dv.next_usn; v.root_idx = dv.root_idx; v.first_idx = dv.first_idx;
            const auto* frns = reinterpret_cast<const DiskFrn*>(m.view + dv.frn_off);
            for (uint32_t j = 0; j < dv.frn_count; ++j) v.frn_build.emplace_back(frns[j].frn, frns[j].idx);
            volumes.push_back(std::move(v));
        }
        start = std::chrono::steady_clock::now();
        if (!Check(a.WriteIndexFile(output.wstring(), store, volumes, m.hdr->built_unix), "production writer creates only explicit new output.tmp")) return false;
        if (interned) Memory("output_written");
        const double write_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        start = std::chrono::steady_clock::now();
        if (!Check(b.MapIndexFile(output.wstring() + L".tmp", b.map_), "production mapper validates complete compressed snapshot")) return false;
        const double new_map = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        std::cout << "[METRIC] before_bytes=" << m.size << " after_bytes=" << b.map_->size
            << " reduction_percent=" << 100.0 * static_cast<double>(m.size - b.map_->size) / m.size
            << " old_map_ms=" << old_map << " new_map_ms=" << new_map << " write_ms=" << write_ms << '\n';
        if (!SameSnapshot(m, *b.map_, interned)) return false;
        a.RebuildChildMapLocked(); b.RebuildChildMapLocked();
        if (!CompareQueries(a, b, thorough)) return false;
        if (interned) Memory("query_validation_complete");
        if (!Check(MoveFileExW((output.wstring() + L".tmp").c_str(), output.c_str(), MOVEFILE_WRITE_THROUGH) != FALSE,
            "verified explicit output published without replacing existing file")) return false;
        return true;
    }
    static bool CodecCases() {
        postings::Encoded encoded;
        const std::vector<uint32_t> starts{0, 0, 5, 7};
        const std::vector<int32_t> ids{0, 1, 127, 128, 5000063, 127, 5000063};
        bool ok = Check(postings::Encode(starts, ids, encoded) &&
            postings::Validate(encoded.starts, encoded.bytes, 5000064), "codec round trip covers empty buckets, zero ID and multi-byte max node gaps");
        auto bad = encoded.bytes; bad[encoded.starts[2] - 1] |= 128;
        ok &= Check(!postings::Validate(encoded.starts, bad, 5000064), "reject continuation crossing bucket boundary");
        ok &= Check(!postings::Validate(encoded.starts, encoded.bytes, 5000063), "reject out-of-range final node ID");
        const std::vector<uint32_t> one{0, 2};
        ok &= Check(!postings::Validate(one, std::vector<uint8_t>{128, 0}, 10), "reject noncanonical overlong zero varint");
        ok &= Check(!postings::Validate(one, std::vector<uint8_t>{0, 0}, 10), "reject duplicate IDs in one bucket");
        ok &= Check(!postings::Validate(std::vector<uint32_t>{0, 5}, std::vector<uint8_t>{255, 255, 255, 255, 31}, UINT32_MAX), "reject 32-bit varint overflow");
        auto offsets = encoded.starts; offsets[1] = UINT32_MAX;
        ok &= Check(!postings::Validate(offsets, encoded.bytes, 5000064), "reject reversed or unbounded bucket offsets");
        return ok;
    }
    static bool CorruptionCases(const std::filesystem::path& file, const std::filesystem::path& directory) {
        std::ifstream in(file, std::ios::binary);
        std::vector<char> good((std::istreambuf_iterator<char>(in)), {});
        DiskHeader hdr{}; std::memcpy(&hdr, good.data(), sizeof(hdr));
        const size_t prefix = static_cast<size_t>(hdr.prefix1_off & ~(uint64_t{1} << 63));
        const auto* starts = reinterpret_cast<const uint32_t*>(good.data() + prefix);
        uint32_t first_nonempty = 0;
        while (first_nonempty < 65536 && starts[first_nonempty] == starts[first_nonempty + 1]) ++first_nonempty;
        if (!Check(first_nonempty < 65536, "corruption fixture contains postings")) return false;
        auto reject = [&](std::vector<char> changed, const char* label) {
            const auto target = directory / L"corrupt.bin";
            { std::ofstream out(target, std::ios::binary | std::ios::trunc); out.write(changed.data(), static_cast<std::streamsize>(changed.size())); }
            Engine engine; std::unique_ptr<Engine::MappedFile> mapped;
            return Check(!engine.MapIndexFile(target.wstring(), mapped), label);
        };
        bool ok = true;
        auto changed = good;
        changed[prefix + 65537ull * 4 + starts[first_nonempty + 1] - 1] |= static_cast<char>(128);
        ok &= reject(std::move(changed), "mapper rejects incomplete varint at bucket end");
        changed = good; uint32_t invalid = UINT32_MAX;
        std::memcpy(changed.data() + prefix + first_nonempty * 4, &invalid, 4);
        ok &= reject(std::move(changed), "mapper rejects invalid bucket boundary");
        changed = good; std::memcpy(changed.data() + prefix + 65536ull * 4, &invalid, 4);
        ok &= reject(std::move(changed), "mapper rejects encoded bytes extending beyond section");
        changed = good; changed.resize(static_cast<size_t>(hdr.prefix2_off) + 20);
        ok &= reject(std::move(changed), "mapper rejects truncated compressed table/file");
        changed = good; DiskHeader missing = hdr; missing.prefix1_off = 0;
        std::memcpy(changed.data(), &missing, sizeof(missing));
        ok &= reject(std::move(changed), "mapper rejects missing required v12 prefix table");
        return ok;
    }
    static bool Seed(const std::filesystem::path& root, const std::filesystem::path& source) {
        std::filesystem::create_directories(root);
        const auto paths = MakeShardPaths((root / L"v9").wstring(), L"aggregate");
        std::filesystem::copy_file(source, paths.base_a);
        Engine engine; std::unique_ptr<Engine::MappedFile> mapped;
        if (!engine.MapIndexFile(paths.base_a, mapped)) return false;
        ShardManifest manifest;
        manifest.generation = 1; manifest.source_id = L"aggregate";
        manifest.active_built = mapped->hdr->built_unix;
        manifest.active_bytes = mapped->size; manifest.active_crc64 = FileCrc64(paths.base_a);
        return SaveShardManifest(paths.manifest, manifest);
    }
    static bool Restart(Engine& engine, const std::filesystem::path& root) {
        SetMachineIndexScope(true); SetActiveIndexDirectory(root.wstring());
        engine.running_ = true;
        const bool loaded = engine.TryLoadCache();
        engine.StopPinyinWorker();
        return loaded;
    }
    static void Usn(Engine& engine, uint64_t frn, uint64_t parent, const wchar_t* name, DWORD reason) {
        const auto length = static_cast<WORD>(wcslen(name) * sizeof(wchar_t));
        std::vector<uint8_t> bytes(sizeof(USN_RECORD_V2) + length);
        auto* record = reinterpret_cast<USN_RECORD_V2*>(bytes.data());
        record->RecordLength = static_cast<DWORD>(bytes.size()); record->MajorVersion = 2;
        record->FileReferenceNumber = frn; record->ParentFileReferenceNumber = parent;
        record->FileNameOffset = static_cast<WORD>(offsetof(USN_RECORD_V2, FileName));
        record->FileNameLength = length; record->Reason = reason; record->FileAttributes = FILE_ATTRIBUTE_NORMAL;
        std::memcpy(bytes.data() + record->FileNameOffset, name, length);
        engine.ApplyUsnLocked(engine.vols_[0], record);
    }
    static std::map<std::wstring, std::pair<uint64_t, uint64_t>> WalInventory(const std::filesystem::path& root) {
        std::map<std::wstring, std::pair<uint64_t, uint64_t>> result;
        for (const auto& item : std::filesystem::recursive_directory_iterator(root / L"Volumes")) {
            if (item.is_regular_file() && item.path().extension() == L".log")
                result.emplace(std::filesystem::relative(item.path(), root).wstring(),
                    std::make_pair(item.file_size(), FileCrc64(item.path().wstring())));
        }
        return result;
    }
    static bool RecoveryCases(const std::filesystem::path& old_file, const std::filesystem::path& compressed,
                              const std::filesystem::path& root) {
        const auto old_root = root / L"wal-old", new_root = root / L"wal-compressed";
        bool ok = Check(Seed(old_root, old_file) && Seed(new_root, compressed), "seed isolated real v11 and v12 manifests for WAL comparison");
        if (!ok) return false;
        int32_t deleted_id = -1;
        {
            Engine a, b;
            if (!Check(Restart(a, old_root) && Restart(b, new_root), "real loader adopts both v11 and v12 snapshots")) return false;
            ok &= Check(!a.NeedsSearchRebuildLocked() && !b.NeedsSearchRebuildLocked(), "valid v11 and v12 avoid background semantic rebuild");
            ok &= Check(a.map_->hdr->ver == 11 && b.map_->hdr->ver == 12 && !a.vols_.empty(), "paired test actually reads old raw IDs and new varints");
            uint64_t root_frn = 0, first_frn = 0, second_frn = 0;
            for (uint32_t i = 0; i < a.vols_[0].frn_base_n; ++i) {
                const auto& frn = a.vols_[0].frn_base[i];
                if (frn.idx == a.vols_[0].root_idx) root_frn = frn.frn;
                if (frn.idx >= 0 && !(a.NodeAt(frn.idx).flags & Engine::kFlagDir)) {
                    if (!first_frn) first_frn = frn.frn;
                    else if (!second_frn) { second_frn = frn.frn; deleted_id = frn.idx; }
                }
            }
            if (!Check(root_frn && first_frn && second_frn, "fixture has root and two independent file FRN identities")) return false;
            for (Engine* engine : {&a, &b}) {
                SetActiveIndexDirectory((engine == &a ? old_root : new_root).wstring());
                Usn(*engine, first_frn, root_frn, L"compact-unicode-中-\U0001f642.txt", USN_REASON_RENAME_NEW_NAME);
                Usn(*engine, second_frn, root_frn, L"deleted.txt", USN_REASON_FILE_DELETE);
                Usn(*engine, UINT64_MAX - 1, root_frn, L"compact-added.txt", USN_REASON_FILE_CREATE);
                for (auto& volume : engine->vols_) engine->DeltaFor(volume.letter)->QueueUsn(volume.journal_id, 2222);
                engine->FlushDeltas(); engine->CloseDeltas();
                Query q; q.needle = L"compact-";
                ok &= Check(engine->Search(q).total == 2 && engine->IsTomb(deleted_id), "USN rename/create/delete produce independently expected overlay results");
            }
            ok &= CompareQueries(a, b, true);
        }
        const auto old_wal = WalInventory(old_root), new_wal = WalInventory(new_root);
        ok &= Check(!old_wal.empty() && old_wal == new_wal, "real per-volume WAL bytes and hashes identical across v11/v12 bases");
        {
            Engine a, b;
            if (!Check(Restart(a, old_root) && Restart(b, new_root), "both encodings restart through production WAL replay")) return false;
            ok &= Check(!a.delta_replay_failed_ && !b.delta_replay_failed_ &&
                a.vols_[0].next_usn == 2222 && b.vols_[0].next_usn == 2222,
                "WAL replay accepts identical generation IDs and restores independent checkpoint");
            Query q; q.needle = L"compact-";
            ok &= Check(a.Search(q).total == 2 && b.Search(q).total == 2 && a.IsTomb(deleted_id) && b.IsTomb(deleted_id),
                "restart independently retains renamed/added paths and deleted identity");
            q.needle = L"中";
            const auto ar = a.Search(q), br = b.Search(q);
            ok &= Check(ar.total == 1 && br.total == 1 && SameResult(ar, br), "Unicode BMP and surrogate-pair rename survive WAL replay with matching queries");
            ok &= CompareQueries(a, b, true);
        }
        const auto mixed_root = root / L"mixed-AB";
        if (!Check(Seed(mixed_root, old_file), "seed previous v11 generation for mixed-version A/B fallback")) return false;
        const auto paths = MakeShardPaths((mixed_root / L"v9").wstring(), L"aggregate");
        std::filesystem::copy_file(compressed, paths.base_b);
        ShardManifest manifest;
        if (!LoadShardManifest(paths.manifest, manifest)) return false;
        manifest.generation = 2; manifest.active_slot = 1; manifest.previous_slot = 0;
        manifest.active_bytes = std::filesystem::file_size(paths.base_b); manifest.active_crc64 = FileCrc64(paths.base_b);
        if (!SaveShardManifest(paths.manifest, manifest)) return false;
        std::wstring active; ShardManifest resolved;
        ok &= Check(ResolveActiveShard(paths, resolved, active) && active == paths.base_b,
            "mixed A/B manifest selects valid v12 active base");
        { std::fstream corrupt(paths.base_b, std::ios::in | std::ios::out | std::ios::binary); corrupt.put('X'); }
        Engine fallback;
        ok &= Check(ResolveActiveShard(paths, resolved, active) && active == paths.base_a && fallback.MapIndexFile(active, fallback.map_) &&
            fallback.map_->hdr->ver == 11 && !fallback.NeedsSearchRebuildLocked(),
            "corrupt active v12 safely falls back to valid v11 previous generation without rescan");
        SetActiveIndexDirectory(L""); SetMachineIndexScope(false);
        return ok;
    }
};
}

int wmain(int argc, wchar_t** argv) {
    using A = pulse::index::EngineTestAccess;
    bool ok = false;
    if (argc == 4 && std::wstring_view(argv[1]) == L"--convert") {
        ok = A::Convert(argv[2], argv[3], false);
    } else if (argc == 4 && std::wstring_view(argv[1]) == L"--intern") {
        ok = A::Convert(argv[2], argv[3], true, true);
    } else if (argc == 4 && std::wstring_view(argv[1]) == L"--test") {
        const std::filesystem::path root = argv[3];
        if (!root.is_absolute() || std::filesystem::exists(root)) return 2;
        std::filesystem::create_directories(root);
        const auto compressed = root / L"compressed.bin";
        ok = A::CodecCases() && A::Convert(argv[2], compressed, true) && A::CorruptionCases(compressed, root) &&
            A::RecoveryCases(argv[2], compressed, root);
    } else {
        std::cout << "Usage: --convert <absolute input snapshot> <absolute absent output> | --intern <absolute input snapshot> <absolute absent output> | --test <absolute old snapshot> <absolute absent fixture dir>\n";
        return 2;
    }
    std::cout << "[SUMMARY] checks=" << A::checks << " failures=" << A::failures << '\n';
    return ok ? 0 : 1;
}
