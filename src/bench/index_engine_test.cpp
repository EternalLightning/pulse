// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "../index/index_engine.h"
#include "../index/usn_stream.h"
#include <cstring>
#include "../index/index_shard.h"
#include "../index/index_paths.h"
#include "../index/index_delta.h"
#include "../index/index_mft.h"
#include "../index/usn_catch_up.h"
#include "../index/index_fallback_metadata.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <chrono>

namespace pulse::index {
struct UsnStreamTestAccess {
    static std::unique_ptr<UsnStream> Make(HANDLE signal) {
        return std::unique_ptr<UsnStream>(new UsnStream(signal));
    }
    static bool Push(UsnStream& s, const std::vector<BYTE>& data) { return s.packets_.Push(data.data(), data.size()); }
    static void Fail(UsnStream& s, DWORD error) { s.Failed(error); }
};

struct EngineTestAccess {
#include "index_notify_modified_cases.h"
#include "index_delta_replay_cases.h"
#include "index_engine_outstanding_cases.h"
#include "index_engine_exclusions_cases.h"
#include "index_ingest_boundary_cases.h"
#include "index_engine_remaining_cases.h"

    static bool InvalidMemoryParentFixture() {
        Engine engine;
        if (!Build(engine) || engine.live_.nodes.size() < 3) return false;
        const auto parent = engine.live_.nodes[1].parent;
        bool ok = true;
        for (int32_t invalid : {INT32_MAX, static_cast<int32_t>(engine.live_.nodes.size()), int32_t{1}}) {
            engine.live_.nodes[1].parent = invalid;
            Engine::Store output;
            std::vector<Engine::VolState> volumes;
            const bool rejected = !engine.FlattenLocked(output, volumes);
            const bool safe = rejected && output.nodes.empty() && volumes.empty() && engine.build_.nodes.empty();
            std::cout << (safe ? "[PASS] " : "[FAIL] ") << "invalid in-memory parent rejected before remap access\n";
            ok &= safe;
        }
        engine.live_.nodes[1].parent = parent;
        return ok;
    }

    static bool FolderSizesFixture(bool mixed_security = false) {
        bool ok = true;
        auto check = [&](bool value, const char* name) {
            std::cout << (value ? "[PASS] " : "[FAIL] ") << name << '\n'; ok &= value;
        };
        const auto fixture = std::filesystem::absolute(std::filesystem::path(L"bench_data") /
            (L"folder-index-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64())));
        const auto a = fixture / L"A", b = fixture / L"B";
        std::filesystem::create_directories(a); std::filesystem::create_directories(b);
        auto write = [](const std::filesystem::path& p, size_t n) { std::ofstream f(p, std::ios::binary | std::ios::trunc); f << std::string(n, 'x'); };
        write(a / L"one.bin", 100);
        Engine e;
        e.AddForTest((a / L"one.bin").wstring(), L"one.bin", false, 100);
        e.AddForTest(b.wstring(), L"B", true);
        Engine::VolState volume;
        volume.letter = fixture.wstring()[0]; volume.root_idx = e.ResolvePathLocked(fixture.root_path().wstring());
        volume.journal_id = 1; volume.folder_size_current = true;
        for (const auto& pair : std::vector<std::pair<uint64_t, std::wstring>>{
            {1, fixture.root_path().wstring()}, {2, fixture.wstring()}, {3, a.wstring()}, {4, b.wstring()}, {5, (a / L"one.bin").wstring()}})
            volume.frn_new.push_back({pair.first, e.ResolvePathLocked(pair.second), 0});
        e.vols_.push_back(std::move(volume)); e.running_ = true; e.ready_ = true;
        auto size = [&](const std::filesystem::path& path) { return e.FolderSizes({path.wstring()})[0]; };
        auto visible_size = [&](const std::filesystem::path& path) {
            return e.FolderSizes({path.wstring()}, [](const auto&) { return true; })[0];
        };
        check(size(a).available && size(a).bytes == 100 && size(b).available && size(b).bytes == 0,
            "MFT metadata aggregates nested folders and known empty zero");
        check(visible_size(a).available && visible_size(a).bytes == 100 && visible_size(b).available && visible_size(b).bytes == 0,
            "caller-filtered covered folders distinguish exact size and known empty zero");
        const auto hidden_size = e.FolderSizes({a.wstring()}, [](const auto& path) { return !path.ends_with(L"one.bin"); })[0];
        check(hidden_size.available && hidden_size.bytes == 0, "caller-filtered folder size excludes denied descendant bytes");
        const auto builds = e.folder_sizes_.Builds();
        VolumeInfo active; active.id = e.vols_[0].volume_id;
        e.UpdateVolumeVisibilityLocked({active});
        check(size(a).bytes == 100 && e.folder_sizes_.Builds() == builds,
            "routine volume visibility polling preserves size aggregates");
        auto usn = [&](uint64_t id, uint64_t parent, const wchar_t* name, DWORD reason, bool directory = false) {
            const auto length = static_cast<WORD>(wcslen(name) * sizeof(wchar_t));
            std::vector<BYTE> bytes(sizeof(USN_RECORD_V2) + length);
            auto* record = reinterpret_cast<USN_RECORD_V2*>(bytes.data());
            record->RecordLength = static_cast<DWORD>(bytes.size()); record->MajorVersion = 2;
            record->FileReferenceNumber = id; record->ParentFileReferenceNumber = parent;
            record->Reason = reason | (mixed_security ? USN_REASON_SECURITY_CHANGE : 0);
            record->FileAttributes = directory ? FILE_ATTRIBUTE_DIRECTORY : 0;
            record->FileNameOffset = static_cast<WORD>(offsetof(USN_RECORD_V2, FileName)); record->FileNameLength = length;
            memcpy(bytes.data() + record->FileNameOffset, name, length);
            e.ApplyUsnLocked(e.vols_.front(), record);
        };
        write(a / L"one.bin", 350); usn(5, 3, L"one.bin", USN_REASON_DATA_EXTEND);
        check(size(a).bytes == 350 && size(fixture).bytes == 350, "real USN data extension updates ancestors");
        write(a / L"one.bin", 25); usn(5, 3, L"one.bin", USN_REASON_DATA_TRUNCATION);
        check(size(a).bytes == 25, "USN truncation subtracts old logical size");
        write(a / L"two.bin", 75); usn(6, 3, L"two.bin", USN_REASON_FILE_CREATE);
        check(size(a).bytes == 100, "USN creation adds file size without index rebuild");
        std::filesystem::rename(a / L"two.bin", b / L"two.bin");
        usn(6, 4, L"two.bin", USN_REASON_RENAME_NEW_NAME);
        check(size(a).bytes == 25 && size(b).bytes == 75 && size(fixture).bytes == 100,
            "USN file move updates both ancestor chains exactly once");
        std::filesystem::rename(a, b / L"A"); usn(3, 4, L"A", USN_REASON_RENAME_NEW_NAME, true);
        check(!size(a).available && size(b).bytes == 100 && size(b / L"A").bytes == 25,
            "USN subtree move transfers its cached total");
        std::filesystem::remove(b / L"A" / L"one.bin"); usn(5, 3, L"one.bin", USN_REASON_FILE_DELETE);
        check(size(b / L"A").bytes == 0 && size(b).bytes == 75, "USN deletion subtracts previous file metadata");
        if (mixed_security) {
            check(e.folder_sizes_.Builds() == builds, "mixed security edits preserve incremental size aggregation");
            std::error_code error; std::filesystem::remove_all(fixture, error);
            check(!error, "isolated mixed security fixture cleanup");
            return ok;
        }
        usn(5, 3, L"one.bin", USN_REASON_FILE_DELETE);
        check(size(b).bytes == 75 && e.folder_sizes_.Builds() == builds,
            "duplicate deletion is harmless and all USN edits avoid aggregate rebuilds");
        e.building_ = true;
        check(!size(b).available, "unfinished index never returns a complete size");
        check(!visible_size(b).available, "caller filtering cannot turn an unfinished index into false zero");
        e.building_ = false;
        e.vols_[0].folder_size_current = false; check(!size(b).available, "startup cache waits for journal catch-up"); e.vols_[0].folder_size_current = true;
        e.inactive_volume_roots_.push_back(e.vols_[0].root_idx);
        check(!size(b).available, "offline volume declines cached totals"); e.inactive_volume_roots_.clear();
        e.GapFeed(); check(!size(b).available, "journal gap declines stale aggregation until recovery");
        e.folder_size_gap_ = false; e.InvalidateFilterLocked();
        check(size(b).available && size(b).bytes == 75 && e.folder_sizes_.Builds() == builds + 1,
            "replacement snapshot rebuilds aggregation from current metadata");
        check(!size(L"\\\\server\\share").available && !size(fixture / L"absent").available, "uncovered UNC and missing paths never return false zero");
        check(!visible_size(L"\\\\server\\share").available && !visible_size(fixture / L"absent").available,
            "caller filtering preserves unavailable for uncovered and missing folders");
        e.excluded_paths_.push_back(b.wstring()); check(!size(b).available, "excluded root falls back to filesystem statistics");
        check(!size(L"\\\\?\\" + b.wstring()).available, "extended-length client path cannot bypass index exclusion");
        std::error_code ec; std::filesystem::remove_all(fixture, ec); check(!ec, "isolated folder-index fixture cleanup");

        FolderSizeIndex index;
        using Item = FolderSizeIndex::Item;
        std::vector<Item> nodes{{-1, 0, true, true}, {0, UINT64_MAX, false, true}, {0, 1, false, true}};
        check(!index.Build(3, [&](int32_t i) { return nodes[i]; }), "aggregate overflow rejected without wrapped sizes");
        nodes = {{1, 0, true, true}, {0, 0, true, true}};
        check(!index.Build(2, [&](int32_t i) { return nodes[i]; }), "cyclic metadata terminates without false totals");
        nodes = {{-1, 0, true, true}};
        index = FolderSizeIndex{};
        for (int32_t i = 1; i <= 200000; ++i) nodes.push_back({0, 1, false, true});
        const auto start = std::chrono::steady_clock::now();
        check(index.Build(static_cast<int32_t>(nodes.size()), [&](int32_t i) { return nodes[i]; }) && index.Get(0) == 200000,
            "large metadata baseline sums 200000 files without filesystem enumeration");
        const auto built = std::chrono::steady_clock::now();
        for (int32_t i = 1; i <= 10000; ++i) { const auto before = nodes[i]; nodes[i].bytes = 2; index.Replace(i, before, nodes[i], [&](int32_t id) { return nodes[id]; }); }
        check(index.Get(0) == 210000 && index.Builds() == 1, "10000 incremental edits reuse a single metadata baseline");
        std::cout << "[TIME] baseline_200000_ms=" << std::chrono::duration<double, std::milli>(built - start).count()
            << " updates_10000_ms=" << std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - built).count() << '\n';
        ipc::PayloadWriter writer; PutFolderSizes(writer, {{true, 0}, {true, UINT64_MAX}, {false, 0}});
        ipc::PayloadReader reader(writer.data().data(), writer.data().size()); std::vector<IndexedFolderSize> values;
        check(ReadFolderSizes(reader, 3, values) && values[0].available && values[1].bytes == UINT64_MAX && !values[2].available,
            "versioned IPC preserves zero, 64-bit size and unavailable distinctly");
        ipc::PayloadReader truncated(writer.data().data(), writer.data().size() - 1);
        check(!ReadFolderSizes(truncated, 3, values), "truncated folder-size IPC rejected");
        return ok;
    }
    static bool NamePoolFixture();
    static bool QuietDiagnosticsFixture();
    static bool UsnQueueFixture(bool mixed_security = false) {
        bool ok = true;
        auto check = [&](bool valid, const char* label) {
            std::cout << (valid ? "[PASS] " : "[FAIL] ") << label << '\n'; ok &= valid;
        };
        const auto dir = std::filesystem::absolute(std::filesystem::path(L"bench_data") /
            (L"usn-queue-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64())));
        std::filesystem::create_directories(dir);
        SetMachineIndexScope(false); SetActiveIndexDirectory(dir.wstring());
        {
            Engine engine;
            check(Build(engine), "build isolated USN consumer fixture without opening volumes");
            auto& volume = engine.vols_.front(); volume.journal_id = 1; volume.next_usn = 10;
            engine.change_signal_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            auto stream = UsnStreamTestAccess::Make(engine.change_signal_);
            auto* collector = stream.get();
            engine.journal_streams_[volume.volume_id] = std::move(stream);
            engine.changes_.Open(dir.wstring()); engine.changes_.Lease(L"usn-queue-fixture", true);
            const auto arm = engine.ReadFeed(true, L"", 0, 0);
            auto push = [&](int64_t next, DWORD reason, const wchar_t* name) {
                const auto length = static_cast<WORD>(wcslen(name) * sizeof(wchar_t));
                const size_t record_bytes = (sizeof(USN_RECORD_V2) + length + 7) & ~size_t{7};
                std::vector<BYTE> packet(sizeof(next) + record_bytes);
                std::memcpy(packet.data(), &next, sizeof(next));
                auto* record = reinterpret_cast<USN_RECORD_V2*>(packet.data() + sizeof(next));
                record->RecordLength = static_cast<DWORD>(record_bytes); record->MajorVersion = 2;
                record->FileReferenceNumber = 1000; record->ParentFileReferenceNumber = 20;
                record->Usn = next - 1;
                record->Reason = reason | (mixed_security ? USN_REASON_SECURITY_CHANGE : 0);
                record->FileNameOffset = static_cast<WORD>(offsetof(USN_RECORD_V2, FileName)); record->FileNameLength = length;
                FILETIME now{}; GetSystemTimeAsFileTime(&now);
                record->TimeStamp.QuadPart = (static_cast<uint64_t>(now.dwHighDateTime) << 32) | now.dwLowDateTime;
                std::memcpy(reinterpret_cast<BYTE*>(record) + record->FileNameOffset, name, length);
                return UsnStreamTestAccess::Push(*collector, packet);
            };
            if (mixed_security) {
                const auto revision = engine.Revision();
                bool changed = false, structural = false;
                check(push(50, USN_REASON_SECURITY_CHANGE, L"usn-permission.txt") &&
                      engine.CatchUpVolume(volume, &changed, &structural) && changed && !structural &&
                      volume.next_usn == 50 && engine.Revision() > revision,
                      "pure security change commits cursor and notifies subscriptions");
            }
            check(push(100, USN_REASON_FILE_CREATE, L"usn-queued.txt") &&
                  push(200, USN_REASON_RENAME_OLD_NAME, L"usn-queued.txt") &&
                  push(300, USN_REASON_RENAME_NEW_NAME, L"usn-renamed.txt") &&
                  push(400, USN_REASON_FILE_DELETE, L"usn-renamed.txt"), "enqueue create/rename/delete packets");
            const auto first_peak = collector->Memory();
            bool changed = false, structural = false;
            check(engine.CatchUpVolume(volume, &changed, &structural) && volume.next_usn == 400 && changed && structural,
                  "real CatchUpVolume applies queued records and commits final cursor");
            const auto page = engine.ReadFeed(true, L"", arm.epoch, arm.next);
            check(!page.gap && page.records.size() == 3 && page.records[0].kind == ChangeKind::Created &&
                  page.records[1].kind == ChangeKind::Renamed && page.records[2].kind == ChangeKind::Deleted &&
                  page.records[1].old_path == L"C:\\Users\\TestUser\\usn-queued.txt" &&
                  page.records[1].path == L"C:\\Users\\TestUser\\usn-renamed.txt",
                  "queued records preserve create/rename/delete order and rename payload");
            auto second = UsnStreamTestAccess::Make(engine.change_signal_);
            std::vector<BYTE> header(sizeof(int64_t)); int64_t next = 500;
            std::memcpy(header.data(), &next, sizeof(next));
            check(UsnStreamTestAccess::Push(*second, header), "enqueue independent diagnostic stream");
            const auto second_state = second->Memory();
            engine.journal_streams_[L"diagnostic-only"] = std::move(second);
            engine.CaptureMemoryState(); const auto& memory = engine.filename_timing_.Memory().retained;
            check(memory.usn_streams == 2 && memory.usn_queue_packets == 1 &&
                  memory.usn_queue_capacity_bytes == second_state.capacity_bytes &&
                  memory.usn_queue_charged_bytes == second_state.charged_bytes &&
                  memory.usn_sum_stream_peak_capacity_bytes == first_peak.peak_capacity_bytes + second_state.peak_capacity_bytes &&
                  memory.usn_sum_stream_peak_charged_bytes == first_peak.peak_charged_bytes + second_state.peak_charged_bytes,
                  "engine aggregates current queue bytes separately from independent lifetime peaks");
            check(UsnStreamTestAccess::Push(*collector, header) && engine.CatchUpVolume(volume, &changed, &structural) &&
                  volume.next_usn == 500, "header-only packet advances engine cursor without emitting an event");
            next = 600; std::memcpy(header.data(), &next, sizeof(next)); UsnStreamTestAccess::Push(*collector, header);
            UsnStreamTestAccess::Fail(*collector, ERROR_BUFFER_OVERFLOW);
            check(!engine.CatchUpVolume(volume, &changed, &structural) && volume.next_usn == 500 &&
                  collector->Memory().packets == 1 && engine.ReadFeed(true, L"", arm.epoch, page.next).records.empty(),
                  "stream overflow reaches engine failure path without cursor advance or silent consumption");
            engine.journal_streams_.clear(); engine.CaptureMemoryState();
            check(!memory.usn_streams && !memory.usn_queue_packets && !memory.usn_queue_capacity_bytes &&
                  !memory.usn_sum_stream_peak_charged_bytes, "collector replacement resets diagnostic aggregates");
        }
        SetActiveIndexDirectory(L""); std::error_code ec; std::filesystem::remove_all(dir, ec);
        check(!ec, "isolated USN consumer fixture cleaned up");
        return ok;
    }

    static bool FeedFixture() {
        bool ok = true;
        auto check = [&](bool valid, const char* label) {
            std::cout << (valid ? "[PASS] " : "[FAIL] ") << label << '\n';
            ok &= valid;
        };
        const auto dir = std::filesystem::absolute(std::filesystem::path(L"bench_data") /
            (L"filename-feed-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64())));
        std::filesystem::create_directories(dir);
        SetMachineIndexScope(false);
        SetActiveIndexDirectory(dir.wstring());
        {
            Engine engine;
            check(Build(engine), "build isolated feed fixture");
            auto arm = engine.ReadFeed(true, L"", 0, 0);
            check(arm.ready && arm.done && arm.records.empty(), "arming starts at current sequence");
            ChangeRecord event;
            event.path = L"C:\\feed-fixture\\新.txt"; event.old_path = L"C:\\feed-fixture\\旧.txt";
            event.kind = ChangeKind::Renamed; event.time = ChangeTracker::Now(); event.file_id = 99;
            engine.changes_.Open(dir.wstring());
            engine.changes_.Lease(L"feed-fixture", true);
            engine.changes_.Record(event);
            for (unsigned i = 0; i < 100020; ++i) engine.RecordFeed(event);
            check(engine.ReadFeed(true, L"", arm.epoch, 0).gap, "slow consumer detects count-retention gap");
            const auto page = engine.ReadFeed(true, L"", arm.epoch, 20);
            check(!page.gap && !page.done && page.records.size() == 512 && page.next == 532 &&
                page.records.front().id == 21 && page.records.front().path == event.path &&
                page.records.front().old_path == event.old_path && page.records.front().file_id == event.file_id &&
                page.records.front().kind == event.kind, "512-event page preserves cursor and rename payload");
            check(engine.ReadFeed(true, L"", arm.epoch, 100020).done, "caught-up consumer returns done");
            const auto path = (dir / L"snapshot.bin").wstring();
            const auto snapshot = engine.ReadFeed(false, L"", 0, 0);
            check(Save(engine, path), "save isolated feed-generation snapshot");
            {
                std::unique_lock lock(engine.mutex_);
                check(Load(engine, path), "replace mapped snapshot under engine lock");
            }
            check(!engine.ReadFeed(true, L"", arm.epoch, 100020).gap,
                "ordinary snapshot replacement preserves change-stream continuity");
            check(engine.feed_changes_.Size() == 100000, "snapshot replacement retains bounded change history");
            check(engine.ReadFeed(false, L"", snapshot.epoch, snapshot.next).gap,
                "snapshot replacement invalidates row-ordinal pagination");
            arm = engine.ReadFeed(true, L"", 0, 0);
            check(arm.ready && arm.done && arm.records.empty() && arm.next == 100020,
                "new subscription keeps monotonic sequence after generation change");
            engine.RecordFeed(event);
            const auto next = engine.ReadFeed(true, L"", arm.epoch, arm.next);
            check(!next.gap && next.done && next.records.size() == 1 && next.records[0].id == 100021,
                "new-generation event remains readable without false gaps");
            auto& memory = engine.filename_timing_.Memory();
            engine.changes_.Flush(true, &memory);
            engine.changes_.Flush(false, &memory);
            check(memory.At(IndexMemoryPoint::ChangeFlushBefore).calls == 1 &&
                memory.At(IndexMemoryPoint::ChangeFlushSnapshot).calls == 1 &&
                memory.At(IndexMemoryPoint::ChangeFlushAfter).calls == 1 &&
                memory.retained.flush_snapshot_records == 1 && memory.retained.history_records == 1,
                "memory sampling preserves sixty-second batching and snapshot record counts");
            engine.CaptureMemoryState();
            check(memory.retained.feed_records == ChangeFeedHistory::kMaxRecords && memory.retained.feed_page_capacity_bytes >= 65536 &&
                memory.retained.aggregate_mapped_file_bytes > 0,
                "memory diagnostics distinguish feed capacity from mapped file size");
            engine.filename_timing_.Flush(true);
            std::ifstream timing(dir / L"pulse-index-timing.jsonl");
            const std::string line((std::istreambuf_iterator<char>(timing)), std::istreambuf_iterator<char>());
            timing.close();
            check(line.find("\"stages\":") != std::string::npos && line.find("\"memory\":") != std::string::npos &&
                line.find("\"change_flush_snapshot\":") != std::string::npos &&
                memory.At(IndexMemoryPoint::TimingFlush).failures == 0,
                "existing timing log retains stages and adds sampled memory points");
            engine.GapFeed();
            check(engine.ReadFeed(true, L"", arm.epoch, next.next).gap,
                "genuine coverage loss still invalidates the change stream");
            engine.CaptureMemoryState();
            check(memory.retained.feed_records == ChangeFeedHistory::kMaxRecords,
                "coverage gaps retain bounded diagnostic history");
            const auto rearmed = engine.ReadFeed(true, L"", 0, 0);
            check(rearmed.epoch != arm.epoch && rearmed.next == next.next && rearmed.records.empty(),
                "new subscription starts beyond the uncertain history after a gap");
            check(engine.changes_.Details(L"feed-fixture", L"C:\\feed-fixture", 0, 0, 200).records.size() == 1,
                "feed retirement does not discard persisted change history");
            engine.Stop();
        }
        SetActiveIndexDirectory(L"");
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        check(!ec, "isolated feed fixture cleaned up");
        return ok;
    }

    static bool MaintenanceFixture() {
        bool ok = true;
        auto check = [&](bool valid, const char* label) {
            std::cout << (valid ? "[PASS] " : "[FAIL] ") << label << '\n';
            ok &= valid;
        };
        const auto dir = std::filesystem::absolute(std::filesystem::path(L"bench_data") /
            (L"filename-maintenance-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64())));
        std::filesystem::create_directories(dir);
        SetActiveIndexDirectory(dir.wstring());
        {
            Engine engine;
            engine.running_ = true;
            engine.ready_ = true;
            engine.AddForTest(L"Q:\\tiny\\one.txt", L"one.txt", false);
            const std::wstring name = L"one.txt";
            std::vector<BYTE> bytes(offsetof(FILE_NOTIFY_INFORMATION, FileName) + name.size() * sizeof(wchar_t));
            auto* removed = reinterpret_cast<FILE_NOTIFY_INFORMATION*>(bytes.data());
            removed->Action = FILE_ACTION_REMOVED;
            removed->FileNameLength = static_cast<DWORD>(name.size() * sizeof(wchar_t));
            std::memcpy(removed->FileName, name.data(), removed->FileNameLength);
            check(engine.ApplyNotifyLocked(L"Q:\\tiny", bytes.data(), static_cast<DWORD>(bytes.size())) == 1 &&
                engine.deleted_ == 1 && engine.LiveCount() < 10,
                "one real delete notification crosses the tiny-index deletion ratio");
            engine.last_struct_tick_ = engine.last_merge_tick_ = 1000;
            check(engine.MaintenanceMergeReason(1001, 0) == nullptr &&
                engine.MaintenanceMergeReason(600999, 0) == nullptr,
                "deletion ratio cannot rewrite a recent snapshot on the event path");
            const auto reason = engine.MaintenanceMergeReason(601000, 0);
            check(reason && std::strcmp(reason, "deleted_ratio") == 0,
                "pending deletion ratio becomes eligible at the normal merge interval");
            engine.struct_changes_ = 100000;
            const auto structural = engine.MaintenanceMergeReason(1001, 0);
            check(structural && std::strcmp(structural, "structural_threshold") == 0,
                "hard structural budget still compacts within the normal interval");
            engine.struct_changes_ = 1;
            const auto delta = engine.MaintenanceMergeReason(1001, 64ull * 1024 * 1024);
            check(delta && std::strcmp(delta, "delta_threshold") == 0,
                "hard delta budget still compacts within the normal interval");
            const auto feed = engine.ReadFeed(true, L"", engine.feed_epoch_.load(), 0);
            check(engine.IsTomb(engine.LiveCount() - 1) && feed.records.size() == 1 &&
                feed.records[0].kind == ChangeKind::Deleted && !feed.gap,
                "deferred compaction retains deletion state and continuous path events");
        }
        {
            Engine engine;
            check(Build(engine), "build isolated maintenance fixture");
            engine.struct_changes_ = 1;
            engine.last_struct_tick_ = 1;
            engine.last_merge_tick_ = 1;
            check(engine.MaintenanceMergeReason(600001, 0) != nullptr, "quiet pending changes request a merge");
            engine.MergeBase(true, "fixture");
            check(engine.struct_changes_ == 0 && engine.merge_retry_after_tick_ == 0,
                "successful merge clears pending structural work");
            check(engine.MaintenanceMergeReason(GetTickCount64() + 1200000, 0) == nullptr,
                "a successful merge never repeats solely because the quiet timer expired");
            engine.Stop();
        }
        {
            Engine engine;
            check(Build(engine), "build isolated failed-merge fixture");
            const auto cache = CacheFilePath();
            HANDLE blocker = CreateFileW(cache.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
            check(blocker != INVALID_HANDLE_VALUE, "lock fixture snapshot against replacement");
            engine.built_unix_ = 42;
            engine.struct_changes_ = 100000;
            if (blocker != INVALID_HANDLE_VALUE) {
                engine.MergeBase(true, "fixture_failure");
                const auto retry = engine.merge_retry_after_tick_;
                check(retry > GetTickCount64() && engine.built_unix_ == 42 && engine.struct_changes_ == 100000,
                    "failed merge retains generation and pending changes and schedules retry");
                engine.MergeBase(true, "fixture_failure");
                check(engine.merge_retry_after_tick_ == retry &&
                    engine.MaintenanceMergeReason(retry - 1, 100000000) == nullptr,
                    "forced compaction and delta triggers honor failure backoff");
                CloseHandle(blocker);
                engine.merge_retry_after_tick_ = 0;
                engine.MergeBase(true, "fixture_retry");
                check(engine.struct_changes_ == 0, "failed merge succeeds once replacement becomes possible");
            }
        }
        {
            Engine engine;
            engine.running_ = true;
            VolumeInfo failed; failed.id = L"failed-fixture-volume"; failed.mount_point = L"X:\\";
            VolumeInfo healthy; healthy.id = L"healthy-fixture-volume"; healthy.mount_point = L"Y:\\";
            unsigned attempts = 0;
            const auto epoch = GetTickCount64() + 1000000;
            engine.indexed_ = 123;
            engine.built_unix_ = 456;
            auto failure = [&](const VolumeInfo& volume) {
                ++attempts;
                if (volume.id != failed.id) return true;
                engine.indexed_ = 12;
                engine.built_unix_ = 45;
                return false;
            };
            engine.RecoverFailedVolumes({failed, healthy}, epoch + 1000, failure);
            for (ULONGLONG tick = 1001; tick < 61000; tick += 1000)
                engine.RecoverFailedVolumes({failed}, epoch + tick, failure);
            check(attempts == 2 && !engine.volume_retry_after_.contains(healthy.id),
                "persistent single-volume failure is attempted once per retry window; healthy volume is not retried");
            check(engine.Count() == 123 && engine.built_unix_ == 456,
                "failed single-volume recovery preserves the last published count and generation");
            engine.RecoverFailedVolumes({failed}, epoch + 61000, failure);
            check(attempts == 3, "failed volume becomes eligible after sixty seconds");
            engine.RecoverFailedVolumes({failed}, epoch + 121000, [](const VolumeInfo&) { return true; });
            check(engine.volume_retry_after_.empty(), "successful recovery clears its retry state");
        }
        {
            Engine engine;
            check(Build(engine), "build self-log exclusion fixture");
            const auto before = engine.LiveCount();
            Usn(engine, 9990, 20, L"pulse-index-timing.jsonl", USN_REASON_FILE_CREATE);
            check(engine.LiveCount() == before, "USN excludes the default filename timing log");
            engine.index_directory_ = L"C:\\Users\\TestUser\\index-data";
            for (const auto* name : {L"pulse-index-timing.jsonl", L"pulse-index-timing.jsonl.1", L"index-data", L"index-data\\internal.tmp"}) {
                const size_t chars = wcslen(name);
                std::vector<BYTE> packet(sizeof(FILE_NOTIFY_INFORMATION) + chars * sizeof(wchar_t));
                auto* notification = reinterpret_cast<FILE_NOTIFY_INFORMATION*>(packet.data());
                notification->Action = FILE_ACTION_MODIFIED;
                notification->FileNameLength = static_cast<DWORD>(chars * sizeof(wchar_t));
                memcpy(notification->FileName, name, chars * sizeof(wchar_t));
                check(engine.ApplyNotifyLocked(L"C:\\Users\\TestUser", packet.data(), static_cast<DWORD>(packet.size())) == 0 &&
                    engine.struct_changes_ == 0 && engine.LiveCount() == before,
                    "directory notifications exclude self logs, their directory and descendants without marking changes");
            }
        }
        SetActiveIndexDirectory(L"");
        std::filesystem::remove_all(dir);
        return ok;
    }

    static bool IdleFixture(unsigned seconds) {
        if (seconds < 5 || seconds > 3600) return false;
        const auto dir = std::filesystem::absolute(std::filesystem::path(L"bench_data") /
            (L"filename-idle-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64())));
        const auto root = dir / L"files";
        const auto data = root / L"index-data";
        std::filesystem::create_directories(data);
        { std::ofstream seed(root / L"stable-fixture.txt"); seed << "fixture"; }
        SetMachineIndexScope(false);
        SetActiveIndexDirectory(data.wstring());
        Engine engine;
        engine.StartFixture(nullptr, 0, root.wstring());
        auto wait_until = [](const auto& predicate, unsigned timeout_ms) {
            const auto deadline = GetTickCount64() + timeout_ms;
            while (!predicate() && GetTickCount64() < deadline) Sleep(20);
            return predicate();
        };
        const bool ready = wait_until([&] { return engine.Ready() && !engine.building_.load() && engine.Count() > 0; }, 30000);
        Sleep(2000);
        auto cpu = [] {
            FILETIME created{}, exited{}, kernel{}, user{};
            GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user);
            return ((static_cast<uint64_t>(kernel.dwHighDateTime) << 32) | kernel.dwLowDateTime) +
                ((static_cast<uint64_t>(user.dwHighDateTime) << 32) | user.dwLowDateTime);
        };
        const auto revision = engine.Revision();
        const auto started = GetTickCount64();
        const auto before = cpu();
        std::cout << "[INFO] isolated filename idle observation started: " << seconds << " seconds\n" << std::flush;
        while (GetTickCount64() - started < seconds * 1000ull) Sleep(250);
        const double elapsed = static_cast<double>(GetTickCount64() - started);
        const double percent = static_cast<double>(cpu() - before) / (elapsed * 100.0);
        const bool stable = revision == engine.Revision();
        auto has = [&](const wchar_t* name) { Query query; query.needle = name; return engine.Search(query).total > 0; };
        { std::ofstream item(root / L"created-fixture.txt"); item << "created"; }
        const bool created = wait_until([&] { return has(L"created-fixture.txt"); }, 3000);
        std::filesystem::rename(root / L"created-fixture.txt", root / L"renamed-fixture.txt");
        const bool renamed = wait_until([&] { return has(L"renamed-fixture.txt") && !has(L"created-fixture.txt"); }, 3000);
        std::filesystem::remove(root / L"renamed-fixture.txt");
        const bool deleted = wait_until([&] { return !has(L"renamed-fixture.txt"); }, 3000);
        const auto stop_started = GetTickCount64();
        engine.Stop();
        const bool stopped = GetTickCount64() - stop_started < 2000;
        SetActiveIndexDirectory(L"");
        const bool ok = ready && stable && percent < 1.0 && created && renamed && deleted && stopped;
        std::cout << "[INFO] elapsed_ms=" << elapsed << " single_core_cpu_percent=" << percent
            << " ready=" << ready << " revision_stable=" << stable << " create=" << created
            << " rename=" << renamed << " delete=" << deleted << " prompt_stop=" << stopped << '\n';
        std::wcout << L"[INFO] retained isolated timing log: " << (data / L"pulse-index-timing.jsonl").wstring() << L'\n';
        std::cout << (ok ? "[PASS] " : "[FAIL] ") << "filename idle CPU and post-idle live updates\n";
        return ok;
    }

    static bool CoverageBenchmark() {
        constexpr uint32_t siblings = 1000000;
        std::vector<Node> nodes(siblings + 2);
        std::vector<Attr> attrs(siblings + 2);
        std::vector<int32_t> order(siblings + 2);
        std::vector<wchar_t> pool;
        pool.reserve(static_cast<size_t>(siblings) * 16);
        auto add = [&](uint32_t index, int32_t parent, const std::wstring& name) {
            auto& node = nodes[index]; node.parent = parent; node.flags = Engine::kFlagDir;
            node.off = static_cast<uint32_t>(pool.size()); node.len = static_cast<uint16_t>(name.size());
            pool.insert(pool.end(), name.begin(), name.end()); order[index] = static_cast<int32_t>(index);
        };
        add(0, -1, L"C:"); add(1, 0, L"Large");
        for (uint32_t i = 0; i < siblings; ++i) {
            wchar_t name[32]{}; swprintf_s(name, L"folder%08u", i); add(i + 2, 1, name);
        }
        DiskHeader header{}; header.pool_chars = pool.size(); header.node_count = static_cast<uint32_t>(nodes.size());
        Engine engine; engine.map_ = std::make_unique<Engine::MappedFile>();
        engine.map_->hdr = &header;
        engine.map_->n = static_cast<uint32_t>(nodes.size()); engine.map_->nodes = nodes.data();
        engine.map_->attrs = attrs.data(); engine.map_->pool = pool.data(); engine.map_->child_order = order.data();
        engine.ready_ = true;
        std::vector<std::wstring> paths;
        for (uint32_t i = siblings - 6; i < siblings; ++i) {
            wchar_t name[64]{}; swprintf_s(name, L"\\\\?\\C:\\Large\\folder%08u", i); paths.emplace_back(name);
        }
        bool valid = true;
        const auto start = std::chrono::steady_clock::now();
        for (const auto& path : paths) valid &= engine.ChangeCoverage(path) == ChangeState::Gap;
        const double milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        std::cout << "[INFO] 1000000 mapped siblings / 6 coverage paths: " << milliseconds << " ms\n";
        engine.tombstones_.insert(siblings + 1);
        valid &= engine.ChangeCoverage(paths.back()) == ChangeState::NotCovered;
        valid &= engine.ChangeCoverage(L"C:\\Large\\missing") == ChangeState::NotCovered;
        std::cout << (valid && milliseconds < 1500.0 ? "[PASS] " : "[FAIL] ") << "mapped coverage bounded lookup and tombstone/missing path checks\n";
        return valid && milliseconds < 1500.0;
    }

    static bool Build(Engine& e) {
        e.running_ = true;
        e.excluded_paths_ = {L"C:\\Users\\TestUser\\excluded"};
        Engine::VolState volume;
        volume.letter = L'C';
        volume.kind = VolumeKind::Removable;
        volume.volume_id = L"test-volume";
        std::vector<Engine::FrnNode> nodes;
        auto add = [&](uint64_t id, uint64_t parent, const wchar_t* name, bool dir) {
            Engine::FrnNode node;
            node.frn = id;
            node.parent = parent;
            node.name = name;
            node.is_dir = dir;
            nodes.push_back(std::move(node));
        };
        // Deliberately put a descendant before its ancestors in FRN order.
        add(1, 30, L"settings.toml", false);
        add(10, 5, L"Users", true);
        add(20, 10, L"TestUser", true);
        add(30, 20, L".codex", true);
        add(40, 20, L"excluded", true);
        add(41, 40, L".codex", true);
        add(50, 20, L"excluded-neighbor", true);
        add(51, 50, L".codex", true);
        add(60, 20, L"node_modules", true);
        add(61, 60, L".codex", true);
        add(70, 20, L".config", true);
        add(80, 20, L".codex-backup", true);
        add(90, 20, L"sample.codex", false);
        add(110, 20, L"AppData", true);
        add(111, 110, L"Local", true);
        add(112, 111, L"Temp", true);
        add(113, 112, L"ShowBoxDebug.log", false);
        const bool ok = e.BuildMftTree(std::move(volume), 5, std::move(nodes));
        e.live_ = std::move(e.build_);
        e.vols_ = std::move(e.build_vols_);
        e.RebuildChildMapLocked();
        e.ready_ = true;
        return ok;
    }

    static bool LoadCache(Engine& e) { return e.TryLoadCache(); }
    static bool CommitCache(Engine& e, const std::wstring& path) {
        e.built_unix_ = 123456;
        return e.CommitMappedFile(path);
    }

    static bool Save(Engine& e, const std::wstring& path) {
        Engine::Store store;
        std::vector<Engine::VolState> volumes;
        return e.FlattenLocked(store, volumes) && e.WriteIndexFile(path, store, volumes, 123456) &&
            MoveFileExW((path + L".tmp").c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
    }

    static bool Load(Engine& e, const std::wstring& path) {
        e.excluded_paths_ = {L"C:\\Users\\TestUser\\excluded"};
        std::unique_ptr<Engine::MappedFile> mapped;
        if (!e.MapIndexFile(path, mapped)) return false;
        e.AdoptMappedLocked(std::move(mapped));
        e.running_ = true;
        return true;
    }

    static bool NeedsRebuild(const Engine& e) { return e.NeedsSearchRebuildLocked(); }

    static bool ReplayAttrOnly(Engine& e) {
        const int32_t log = e.FindByFrnLocked(e.vols_.front(), 113);
        const int32_t folder = e.FindByFrnLocked(e.vols_.front(), 112);
        if (log < 0 || folder < 0) return false;
        DeltaLog delta;
        if (!delta.Open(DeltaFilePathForVolume(e.vols_.front().volume_id), e.built_unix_)) return false;
        for (int32_t idx : {log, folder})
            delta.QueuePatch(idx, static_cast<uint8_t>(PatchBits::Attr), 0, 0, 100, 4096, {});
        if (!delta.Flush()) return false;
        delta.Close();
        e.ReplayDeltasLocked();
        e.InvalidateFilterLocked();
        return e.AttrAt(log).size == 4096 && e.AttrAt(folder).mtime == 100 &&
            (e.NodeAt(folder).flags & Engine::kFlagDir) != 0;
    }

    static bool RebuildLegacyFixture(Engine& e) {
        // Recreate this fixture's raw MFT records and exercise the production
        // builder; offline retention alone does not repair legacy hidden flags.
        e.excluded_paths_ = {L"C:\\Users\\TestUser\\excluded"};
        if (e.vols_.size() != 1) return false;
        const auto& old = e.vols_.front();
        std::vector<uint64_t> frns(static_cast<size_t>(e.LiveCount()));
        const auto remember = [&](uint64_t frn, int32_t id) {
            if (id >= 0 && id < e.LiveCount()) frns[static_cast<size_t>(id)] = frn;
        };
        for (uint32_t i = 0; i < old.frn_base_n; ++i) remember(old.frn_base[i].frn, old.frn_base[i].idx);
        for (const auto& item : old.frn_new) remember(item.frn, item.idx);
        for (const auto& item : old.frn_build) remember(item.first, item.second);
        if (old.root_idx < 0 || old.root_idx >= e.LiveCount()) return false;
        const uint64_t root_frn = frns[static_cast<size_t>(old.root_idx)];
        if (!root_frn) return false;
        std::vector<Engine::FrnNode> records;
        for (int32_t i = 0; i < e.LiveCount(); ++i) {
            if (i == old.root_idx || e.IsTomb(i)) continue;
            const auto node = e.NodeAt(i);
            if (node.parent < 0 || node.parent >= e.LiveCount()) return false;
            Engine::FrnNode record;
            record.frn = frns[static_cast<size_t>(i)];
            record.parent = frns[static_cast<size_t>(node.parent)];
            if (!record.frn || !record.parent) return false;
            record.name = e.NameOf(i);
            record.is_dir = (node.flags & Engine::kFlagDir) != 0;
            record.size = e.AttrAt(i).size;
            record.mtime = e.AttrAt(i).mtime;
            records.push_back(std::move(record));
        }
        Engine::VolState volume;
        volume.letter = old.letter;
        volume.kind = old.kind;
        volume.volume_id = old.volume_id;
        e.build_.Clear();
        e.build_vols_.clear();
        if (!e.BuildMftTree(std::move(volume), root_frn, std::move(records))) return false;
        e.map_.reset();
        e.live_ = std::move(e.build_);
        e.vols_ = std::move(e.build_vols_);
        e.RebuildChildMapLocked();
        e.InvalidateFilterLocked();
        return true;
    }

    static bool RootHidden(const Engine& e) {
        return (e.NodeAt(e.vols_.front().root_idx).flags & Engine::kFlagHidden) != 0;
    }

    static bool RecycleFixture() {
        Engine engine; if (!Build(engine)) return false;
        Usn(engine, 200, 5, L"$Recycle.Bin", USN_REASON_FILE_CREATE);
        Usn(engine, 201, 200, L"S-1-5-21-123", USN_REASON_FILE_CREATE);
        engine.changes_.Lease(L"fixture", true);
        Usn(engine, 90, 201, L"$Rsample.codex", USN_REASON_RENAME_NEW_NAME);
        auto result = engine.changes_.Details(L"fixture", L"C:\\Users\\TestUser", 0, 0, 200);
        const bool deleted = result.records.size() == 1 && result.records[0].kind == ChangeKind::Deleted &&
            result.records[0].path == L"C:\\Users\\TestUser\\sample.codex";
        Usn(engine, 90, 20, L"sample.codex", USN_REASON_RENAME_NEW_NAME);
        result = engine.changes_.Details(L"fixture", L"C:\\Users\\TestUser", 0, 0, 200);
        const bool restored = result.records.size() == 1 && result.records[0].kind == ChangeKind::Created;
        std::cout << (deleted && restored ? "[PASS]" : "[FAIL]") << " USN recycle and restore original path\n";
        return deleted && restored;
    }

    static bool ParentCycleFixture() {
        Engine e;
        if (!Build(e)) return false;
        auto id = [&](uint64_t frn) { return e.FindByFrnLocked(e.vols_.front(), frn); };
        const int32_t user = id(20), codex = id(30);
        const Node original = e.NodeAt(user);
        bool ok = true;
        auto check = [&](bool passed, const char* label) {
            std::cout << (passed ? "[PASS] " : "[FAIL] ") << label << '\n';
            ok &= passed;
        };
        e.vols_.front().journal_id = 123;
        Usn(e, 20, 20, L"TestUser", USN_REASON_RENAME_NEW_NAME);
        check(e.NodeAt(user).parent == original.parent && e.vols_.front().journal_id == 0,
              "reject self-parent USN rename and request volume recovery");
        Usn(e, 20, 30, L"TestUser", USN_REASON_RENAME_NEW_NAME);
        check(e.NodeAt(user).parent == original.parent, "reject ancestor moved beneath descendant");
        Usn(e, 30, 50, L".codex", USN_REASON_RENAME_NEW_NAME);
        check(e.NodeAt(codex).parent == id(50), "accept valid directory move");
        Usn(e, 30, 20, L".codex", USN_REASON_RENAME_NEW_NAME);
        const auto root = e.vols_.front().root_idx;
        Usn(e, 5, 5, L"root", USN_REASON_FILE_CREATE);
        check(e.NodeAt(root).parent == -1, "preserve volume root");
        // Reproduce a cycle already present in an old in-memory/delta tree.
        e.live_.nodes[user].parent = codex;
        Usn(e, 200, 20, L"new-folder", USN_REASON_FILE_CREATE);
        check(id(200) < 0, "existing parent cycle cannot hang USN processing");
        check(!e.InSubtreeLocked(user, id(50)), "existing cycle cannot hang subtree lookup");
        Term term;
        term.name = L"never-present";
        term.name_how = NameHow::Substring;
        term.name_in_path = true;
        CompiledQuery query;
        query.groups = {{term}};
        check(!e.MatchNodeLocked(user, query, -1, false, false) &&
              !e.MatchQueryNodeLocked(user, query, -1, false, false),
              "existing cycle cannot hang path-name search");
        e.live_.nodes[user].parent = e.LiveCount() + 100;
        Usn(e, 201, 20, L"invalid-parent", USN_REASON_FILE_CREATE);
        check(id(201) < 0, "reject out-of-range ancestor");
        e.live_.nodes[user].parent = original.parent;
        e.RequestStop();
        Usn(e, 202, 20, L"cancelled", USN_REASON_FILE_CREATE);
        check(id(202) < 0, "stop cancels parent validation");
        Engine source;
        const auto file = std::filesystem::path(L"bench_data") /
            (L"index-parent-cycle-" + std::to_wstring(GetCurrentProcessId()) + L".bin");
        const bool saved = Build(source) && Save(source, file.wstring());
        check(saved, "save isolated mapped fixture");
        if (saved) {
            Engine mapped;
            const bool loaded = Load(mapped, file.wstring());
            check(loaded, "load mapped parent fixture");
            if (loaded) {
                const int32_t target = mapped.FindByFrnLocked(mapped.vols_.front(), 30);
                const int32_t excluded = mapped.FindByFrnLocked(mapped.vols_.front(), 40);
                Usn(mapped, 30, 40, L".codex", USN_REASON_RENAME_NEW_NAME);
                check(mapped.NodeAt(target).parent == excluded &&
                      (mapped.NodeAt(target).flags & Engine::kFlagHidden),
                      "mapped rename preserves excluded visibility");
                Usn(mapped, 40, 30, L"excluded", USN_REASON_RENAME_NEW_NAME);
                check(mapped.NodeAt(excluded).parent != target,
                      "reject cycle through mapped overlay even when parent is hidden");
                Usn(mapped, 30, 20, L".codex", USN_REASON_RENAME_NEW_NAME);
                check(!(mapped.NodeAt(target).flags & Engine::kFlagHidden),
                      "mapped valid move restores visibility");
            }
        }
        std::filesystem::remove(file);
        return ok;
    }

    static void Usn(Engine& e, uint64_t id, uint64_t parent, const wchar_t* name, DWORD reason) {
        const auto length = static_cast<WORD>(wcslen(name) * sizeof(wchar_t));
        std::vector<BYTE> bytes(sizeof(USN_RECORD_V2) + length);
        auto* rec = reinterpret_cast<USN_RECORD_V2*>(bytes.data());
        rec->RecordLength = static_cast<DWORD>(bytes.size());
        rec->FileReferenceNumber = id;
        rec->ParentFileReferenceNumber = parent;
        rec->FileNameOffset = static_cast<WORD>(offsetof(USN_RECORD_V2, FileName));
        rec->FileNameLength = length;
        rec->FileAttributes = FILE_ATTRIBUTE_DIRECTORY;
        rec->Reason = reason;
        FILETIME now{}; GetSystemTimeAsFileTime(&now);
        rec->TimeStamp.QuadPart = (static_cast<uint64_t>(now.dwHighDateTime) << 32) | now.dwLowDateTime;
        memcpy(bytes.data() + rec->FileNameOffset, name, length);
        e.ApplyUsnLocked(e.vols_.front(), rec);
    }
};

} // namespace pulse::index

using namespace pulse::index;
namespace {
int failures = 0;
void Check(bool ok, const char* label) {
    std::cout << (ok ? "[PASS] " : "[FAIL] ") << label << '\n';
    if (!ok) ++failures;
}
SearchResult Search(Engine& e, const wchar_t* needle) {
    Query q;
    q.needle = needle;
    q.limit = 100;
    return e.Search(q);
}
bool Has(Engine& e, const wchar_t* needle, const wchar_t* path) {
    const auto result = Search(e, needle);
    return std::any_of(result.hits.begin(), result.hits.end(), [&](const Hit& hit) {
        return hit.path == path;
    });
}
int QueryNarrowingTests() {
    auto populate = [](Engine& engine) {
        engine.AddForTest(L"C:\\QueryNarrowing\\alpha.txt", L"alpha.txt", false, 5);
        engine.AddForTest(L"C:\\QueryNarrowing\\beta.pdf", L"beta.pdf", false, 20);
        engine.AddForTest(L"C:\\QueryNarrowing\\gamma.t", L"gamma.t", false, 0);
        engine.AddForTest(L"C:\\QueryNarrowing\\1.dat", L"1.dat", false, 1);
        engine.AddForTest(L"C:\\QueryNarrowing\\12.dat", L"12.dat", false, 12);
        engine.AddForTest(L"C:\\QueryNarrowing\\\u4e2d\u6587.txt", L"\u4e2d\u6587.txt", false, 3);
    };
    auto paths = [](const SearchResult& result) {
        std::vector<std::wstring> values;
        for (const auto& hit : result.hits) values.push_back(hit.path);
        std::sort(values.begin(), values.end());
        return values;
    };
    auto compare = [&](const wchar_t* previous, const wchar_t* next,
                       const wchar_t* required_name, const char* label) {
        Engine warm, cold;
        populate(warm);
        populate(cold);
        Search(warm, previous);
        const auto actual = Search(warm, next);
        const auto expected = Search(cold, next);
        const bool present = std::any_of(expected.hits.begin(), expected.hits.end(),
            [&](const Hit& hit) { return hit.name == required_name; });
        Check(present && actual.total == expected.total && paths(actual) == paths(expected), label);
    };
    compare(L"size:<1", L"size:<10", L"alpha.txt", "expanded size bound agrees with cold query");
    compare(L"ext:txt", L"ext:txt;pdf", L"beta.pdf", "extension alternatives agree with cold query");
    compare(L"!ext:t", L"!ext:txt", L"gamma.t", "extended extension exclusion agrees with cold query");
    compare(L"!1", L"!12", L"1.dat", "extended name exclusion agrees with cold query");
    compare(L"al", L"alp", L"alpha.txt", "positive substring typing agrees with cold query");
    compare(L"zh", L"zho", L"\u4e2d\u6587.txt", "pinyin substring typing agrees with cold query");
    compare(L"z", L"zh", L"\u4e2d\u6587.txt", "enabling pinyin agrees with cold query");
    Check(QueryCanNarrow(L"al", L"alp"), "ordinary positive substring retains narrowing");
    Check(QueryCanNarrow(L"zh", L"zho"), "existing pinyin substring retains narrowing");
    Check(!QueryCanNarrow(L"z", L"zh"), "enabling pinyin requires full collection");
    Check(!QueryCanNarrow(L"\"al", L"\"alp"), "quoted exact name extension requires full collection");
    Check(!QueryCanNarrow(L"al", L"al nopinyin:"), "pinyin control change requires full collection");
    return failures ? 1 : 0;
}
void CheckSearch(Engine& e) {
    constexpr auto log_path = L"C:\\Users\\TestUser\\AppData\\Local\\Temp\\ShowBoxDebug.log";
    Check(Has(e, L"ShowBoxDebug.log", log_path), "mixed-case dotted temp filename searchable");
    Check(Has(e, L"showboxdebug.LOG", log_path), "temp filename search ignores case");
    Check(Has(e, L"\"ShowBoxDebug.log\"", log_path), "quoted exact temp filename searchable");
    const std::wstring filename = L"ShowBoxDebug.log";
    bool incremental = true;
    for (size_t length = 1; length <= filename.size(); ++length)
        incremental = Has(e, filename.substr(0, length).c_str(), log_path) && incremental;
    Check(incremental, "incremental filename search preserves temp log result");
    Check(Has(e, L"Debug.log", log_path), "interior filename bigram finds temp log");
    Check(Has(e, L".codex", L"C:\\Users\\TestUser\\.codex"), "dot folder searchable");
    Check(Has(e, L"settings", L"C:\\Users\\TestUser\\.codex\\settings.toml"), "descendant searchable");
    Check(Has(e, L".config", L"C:\\Users\\TestUser\\.config"), "other dot folders searchable");
    Check(!Has(e, L".codex", L"C:\\Users\\TestUser\\excluded\\.codex"), "excluded subtree hidden");
    Check(Has(e, L".codex", L"C:\\Users\\TestUser\\excluded-neighbor\\.codex"), "exclusion respects path boundary");
    Check(!Has(e, L".codex", L"C:\\Users\\TestUser\\node_modules\\.codex"), "dependency subtree hidden");
    Check(Search(e, L"folder: \".codex\"").total == 2, "exact folder query");
    Check(!Has(e, L"file: ext:codex", L"C:\\Users\\TestUser\\.codex"), "extension query excludes dot folder");
    Check(EngineTestAccess::RootHidden(e), "root stays hidden");
    std::atomic<uint32_t> latest{2};
    Query q;
    q.needle = L".codex";
    Check(e.Search(q, &latest, 1).hits.empty(), "stale request cancelled");
}
}

#include "index_name_pool_fixture.h"
#include "index_quiet_diagnostics_fixture.h"

bool MftReadFixture() {
    bool ok = true;
    auto check = [&](bool valid, const char* label) {
        std::cout << (valid ? "[PASS] " : "[FAIL] ") << label << '\n'; ok &= valid;
    };
    constexpr DWORD chunk_bytes = 8u * 1024u * 1024u;
    constexpr uint64_t valid_bytes = chunk_bytes + 512;
    constexpr size_t second_extent = chunk_bytes + 1024;
    std::vector<BYTE> disk(second_extent + 1024);
    auto put = [&](size_t offset, auto value) { std::memcpy(disk.data() + offset, &value, sizeof(value)); };
    put(0, uint32_t{0x454c4946}); put(4, uint16_t{48}); put(6, uint16_t{2});
    put(16, uint16_t{1}); put(20, uint16_t{56}); put(22, uint16_t{1});
    put(24, uint32_t{156}); put(28, uint32_t{512});
    put(48, uint16_t{0xabcd}); put(510, uint16_t{0xabcd});
    put(56, uint32_t{0x30}); put(60, uint32_t{96});
    put(72, uint32_t{68}); put(76, uint16_t{24});
    put(80, uint64_t{5}); disk[144] = 1; disk[145] = 1; put(146, wchar_t{L'x'});
    put(152, uint32_t{0xffffffff});
    // Keep names in separate records so a rejected record 0 cannot pass merely
    // because no other live record happens to be present in the fixture.
    std::memcpy(disk.data() + 512, disk.data(), 512);
    std::memcpy(disk.data() + second_extent, disk.data(), 512);
    put(second_extent + 146, wchar_t{L'y'});
    std::fill(disk.begin(), disk.begin() + 512, BYTE{});
    put(0, uint32_t{0x454c4946}); put(4, uint16_t{48}); put(6, uint16_t{2});
    put(16, uint16_t{1}); put(20, uint16_t{56}); put(22, uint16_t{1});
    put(24, uint32_t{140}); put(28, uint32_t{512});
    put(48, uint16_t{0xabcd}); put(510, uint16_t{0xabcd});
    put(56, uint32_t{0x80}); put(60, uint32_t{80}); disk[64] = 1;
    put(80, uint64_t{16384}); put(88, uint16_t{64});
    put(104, uint64_t{valid_bytes}); put(112, uint64_t{valid_bytes});
    const BYTE runlist[] = {0x12, 0x00, 0x40, 0x00, 0x21, 0x01, 0x02, 0x40, 0x00};
    std::memcpy(disk.data() + 120, runlist, sizeof(runlist));
    put(136, uint32_t{0xffffffff});
    const auto path = std::filesystem::absolute(std::filesystem::path(L"bench_data") /
        (L"mft-read-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()) + L".bin"));
    std::filesystem::create_directories(path.parent_path());
    {
        std::ofstream file(path, std::ios::binary);
        file.write(reinterpret_cast<const char*>(disk.data()), static_cast<std::streamsize>(disk.size()));
        check(file.good(), "write isolated fragmented MFT with a valid mapping and two live records");
    }
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    check(file != INVALID_HANDLE_VALUE, "open isolated MFT stream without a volume or elevation");
    if (file == INVALID_HANDLE_VALUE) return false;
    MftReadIo io;
    uint64_t queried_valid_bytes = valid_bytes;
    io.query = [&](NTFS_VOLUME_DATA_BUFFER& geometry) {
        geometry.BytesPerFileRecordSegment = 512; geometry.BytesPerCluster = 512;
        geometry.BytesPerSector = 512; geometry.MftValidDataLength.QuadPart = queried_valid_bytes;
        return true;
    };
    std::atomic<bool> running{true}; size_t emitted = 0;
    bool identities = true;
    auto emit = [&](MftFile&& record) {
        ++emitted;
        identities &= (record.name == L"x" && (record.frn & 0xffffffffffffull) == 1) ||
            (record.name == L"y" && (record.frn & 0xffffffffffffull) == 16384);
        return true;
    };
    check(EnumerateMft(file, &running, {}, emit, &io) == MftReadStatus::Complete && emitted == 2 && identities,
          "fragmented mapping yields both records with logical FRN identities");
    io.read = [&](uint64_t offset, void* buffer, DWORD bytes) {
        std::memcpy(buffer, disk.data() + offset, bytes); return true;
    };
    std::memcpy(disk.data() + second_extent + 512, disk.data() + second_extent, 512);
    put(second_extent + 512 + 146, wchar_t{L'z'});
    disk[125] = 2;
    emitted = 0;
    check(EnumerateMft(file, &running, {}, emit, &io) == MftReadStatus::Complete && emitted == 2,
          "allocated mapping tail beyond valid data cannot emit stale records");
    disk[125] = 1;
    put(510, uint16_t{0x1234});
    emitted = 0;
    check(EnumerateMft(file, &running, {}, emit, &io) == MftReadStatus::Failed && emitted == 0,
          "corrupt record zero rejects mapping before later valid records can emit");
    put(510, uint16_t{0xabcd});
    disk[124] = 0x19;
    emitted = 0;
    check(EnumerateMft(file, &running, {}, emit, &io) == MftReadStatus::Failed && emitted == 0,
          "malformed mapping after a valid prefix cannot append a guessed extent");
    disk[120] = 0;
    emitted = 0;
    check(EnumerateMft(file, &running, {}, emit, &io) == MftReadStatus::Failed && emitted == 0,
          "empty mapping cannot guess a contiguous physical baseline");
    disk[120] = 0x12;
    disk[124] = 0;
    emitted = 0;
    check(EnumerateMft(file, &running, {}, emit, &io) == MftReadStatus::Failed && emitted == 0,
          "mapping shorter than valid MFT data fails before emitting a partial baseline");
    disk[124] = 0x21;
    put(72, uint64_t{1});
    emitted = 0;
    check(EnumerateMft(file, &running, {}, emit, &io) == MftReadStatus::Failed && emitted == 0,
          "mapping starting after VCN zero cannot establish a baseline");
    put(72, uint64_t{0});
    queried_valid_bytes = valid_bytes - 1;
    emitted = 0;
    check(EnumerateMft(file, &running, {}, emit, &io) == MftReadStatus::Failed && emitted == 0,
          "nonrecord aligned valid data length fails before emitting records");
    queried_valid_bytes = valid_bytes;
    emitted = 0;
    check(EnumerateMft(file, &running, {}, emit, &io) == MftReadStatus::Complete && emitted == 2,
          "restored mapping and update sequence admit both valid records");
    io.read = {};
    LARGE_INTEGER length{}; length.QuadPart = chunk_bytes + 256;
    check(SetFilePointerEx(file, length, nullptr, FILE_BEGIN) && SetEndOfFile(file), "truncate only second MFT chunk");
    emitted = 0;
    check(EnumerateMft(file, &running, {}, emit, &io) == MftReadStatus::Failed && emitted == 1,
          "native short second read rejects nonempty partial MFT baseline");
    io.read = [&](uint64_t offset, void* buffer, DWORD bytes) {
        if (offset >= chunk_bytes) { SetLastError(ERROR_READ_FAULT); return false; }
        std::memcpy(buffer, disk.data() + offset, bytes); return true;
    };
    emitted = 0;
    check(EnumerateMft(file, &running, {}, emit, &io) == MftReadStatus::Failed && emitted == 1,
          "failed second chunk never reports complete after prior records");
    emitted = 0;
    check(EnumerateMft(file, &running, {}, [&](MftFile&&) { ++emitted; return false; }, &io) == MftReadStatus::Capped && emitted == 1,
          "consumer cap is distinct from complete baseline");
    emitted = 0;
    check(EnumerateMft(file, &running, {}, [&](MftFile&&) { ++emitted; running = false; return true; }, &io) == MftReadStatus::Cancelled && emitted == 1,
          "cancellation after first record is distinct from complete baseline");
    CloseHandle(file);
    std::error_code error; std::filesystem::remove(path, error);
    check(!error, "isolated MFT fixture cleaned up");
    return ok;
}

#include "index_cache_resolution_cases.h"

void LegacyRepairTests(const std::filesystem::path& dir, const std::filesystem::path& base) {
    {
        std::fstream file(base, std::ios::binary | std::ios::in | std::ios::out);
        DiskHeader header{};
        file.read(reinterpret_cast<char*>(&header), sizeof(header));
        header.ver = 10;
        // Legacy snapshots may omit prefix tables; V12 varints cannot be
        // reinterpreted as the older integer posting representation.
        header.prefix1_off = 0;
        header.prefix2_off = 0;
        file.seekp(0);
        file.write(reinterpret_cast<const char*>(&header), sizeof(header));
        file.close();
        Engine legacy;
        Check(EngineTestAccess::Load(legacy, base.wstring()) && EngineTestAccess::NeedsRebuild(legacy),
              "V10 snapshot requests repair for persisted attr-only corruption");
    }
    // Reproduce a persisted V9 index whose root flag contaminated all descendants.
    {
        std::fstream file(base, std::ios::binary | std::ios::in | std::ios::out);
        DiskHeader header{};
        file.read(reinterpret_cast<char*>(&header), sizeof(header));
        header.ver = 9;
        file.seekp(0);
        file.write(reinterpret_cast<const char*>(&header), sizeof(header));
        for (uint32_t i = 0; i < header.node_count; ++i) {
            const auto offset = static_cast<std::streamoff>(header.nodes_off + i * sizeof(Node));
            Node node;
            file.seekg(offset);
            file.read(reinterpret_cast<char*>(&node), sizeof(node));
            node.flags |= 2;
            file.seekp(offset);
            file.write(reinterpret_cast<const char*>(&node), sizeof(node));
        }
    }
    {
        Engine old;
        Check(EngineTestAccess::Load(old, base.wstring()), "legacy snapshot remains readable");
        Check(EngineTestAccess::NeedsRebuild(old), "legacy snapshot triggers repair");
        Check(Search(old, L".codex").total == 0, "fixture reproduces old search failure");
        Check(EngineTestAccess::RebuildLegacyFixture(old), "legacy fixture rebuilds through actual MFT tree builder");
        CheckSearch(old);
        Check(EngineTestAccess::Save(old, (dir / L"repaired.bin").wstring()), "rebuilt legacy snapshot persists");
    }
    {
        const auto paths = MakeShardPaths(dir.wstring(), L"migration-test");
        ShardManifest manifest;
        Check(PublishShardBase(paths, base.wstring(), 123456, 0, manifest), "publish legacy generation");
        Check(PublishShardBase(paths, (dir / L"repaired.bin").wstring(), 123456, 0, manifest),
              "publish repaired generation");
        std::wstring active;
        Check(ResolveActiveShard(paths, manifest, active), "resolve repaired generation");
        {
            Engine repaired;
            Check(EngineTestAccess::Load(repaired, active) && !EngineTestAccess::NeedsRebuild(repaired),
                  "published repair remains current after restart");
            CheckSearch(repaired);
        }
        {
            std::ofstream corrupt(active, std::ios::binary | std::ios::trunc);
            corrupt << "corrupt";
        }
        Check(ResolveActiveShard(paths, manifest, active), "corrupt repaired slot falls back safely");
        Engine fallback;
        Check(EngineTestAccess::Load(fallback, active) && EngineTestAccess::NeedsRebuild(fallback),
              "legacy fallback still requests repair");
    }
}

int wmain(int argc, wchar_t** argv) {
    if (argc > 1 && std::wstring_view(argv[1]) == L"--offline-retention-only") {
        std::cout << std::unitbuf;
        const bool parents = EngineTestAccess::InvalidMemoryParentFixture();
        const bool retained = EngineTestAccess::OutstandingStructureFixture(true);
        return parents && retained ? 0 : 1;
    }
    if (argc > 1 && std::wstring_view(argv[1]) == L"--legacy-repair-only") {
        std::cout << std::unitbuf;
        const auto dir = std::filesystem::absolute(std::filesystem::path(L"bench_data") /
            (L"legacy-repair-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64())));
        std::filesystem::create_directories(dir);
        const auto base = dir / L"base.bin";
        {
            Engine source;
            const bool saved = EngineTestAccess::Build(source) && EngineTestAccess::Save(source, base.wstring());
            Check(saved, "save legacy repair source fixture");
            if (saved) LegacyRepairTests(dir, base);
        }
        std::error_code error;
        std::filesystem::remove_all(dir, error);
        Check(!error && !std::filesystem::exists(dir), "legacy repair fixture is removed");
        return failures ? 1 : 0;
    }
    std::cout << std::unitbuf;
    if (argc > 1 && std::wstring_view(argv[1]) == L"--outstanding-structure-only")
        return EngineTestAccess::OutstandingStructureFixture() ? 0 : 1;
    if (argc > 1 && std::wstring_view(argv[1]) == L"--outstanding-query-only")
        return EngineTestAccess::OutstandingQueryFixture() ? 0 : 1;
    if (argc > 1 && std::wstring_view(argv[1]) == L"--exclusions-concurrency-only")
        return EngineTestAccess::ExclusionsConcurrencyFixture() ? 0 : 1;
    if (argc > 1 && std::wstring_view(argv[1]) == L"--delta-replay-only")
        return EngineTestAccess::DeltaReplayFixture() ? 0 : 1;
    if (argc > 1 && std::wstring_view(argv[1]) == L"--notify-modified-only") return EngineTestAccess::NotifyModifiedFixture() ? 0 : 1;
    if (argc > 1 && std::wstring_view(argv[1]) == L"--ingest-boundary-only") return EngineTestAccess::IngestBoundaryFixture() ? 0 : 1;
    if (argc > 1 && std::wstring_view(argv[1]) == L"--query-narrowing-only") return QueryNarrowingTests();
    if (argc > 1 && std::wstring_view(argv[1]) == L"--cache-resolution-only") return CacheResolutionTests();
    if (argc > 1 && std::wstring_view(argv[1]) == L"--shard-io-rebuild-only") {
        const auto root = std::filesystem::absolute(std::filesystem::path(L"bench_data") /
            (L"shard-io-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64())));
        std::filesystem::create_directories(root);
        const auto paths = MakeShardPaths(root.wstring(), L"io-fixture");
        const auto candidate = root / L"candidate.bin";
        std::string bytes(256, 'x');
        const uint32_t version = kIndexSnapshotVersion;
        const uint64_t built = 123;
        std::memcpy(bytes.data(), "PIDX", 4);
        std::memcpy(bytes.data() + 4, &version, sizeof(version));
        std::memcpy(bytes.data() + 16, &built, sizeof(built));
        auto write = [&](const std::wstring& path, const std::string& data) {
            std::ofstream output(std::filesystem::path(path), std::ios::binary | std::ios::trunc);
            output.write(data.data(), data.size());
        };
        ShardManifest original;
        original.source_id = L"io-fixture";
        original.active_built = built;
        original.active_bytes = bytes.size();
        auto reset = [&] {
            write(paths.base_a, bytes); write(paths.base_b, bytes); write(candidate.wstring(), bytes);
            original.active_crc64 = FileCrc64(paths.base_a);
            Check(SaveShardManifest(paths.manifest, original), "reset isolated I/O fixture");
        };
        for (unsigned scenario = 0; scenario < 5; ++scenario) {
            reset();
            const bool manifest_lock = scenario >= 3;
            const bool byte_lock = scenario == 1 || scenario == 2 || scenario == 4;
            const auto& path = manifest_lock ? paths.manifest : paths.base_a;
            const uint64_t manifest_crc = FileCrc64(paths.manifest);
            HANDLE file = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                byte_lock ? FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE : 0,
                nullptr, OPEN_EXISTING, 0, nullptr);
            OVERLAPPED lock{}; lock.Offset = scenario == 2 ? 128 : 0;
            const bool locked = file != INVALID_HANDLE_VALUE && (!byte_lock ||
                LockFileEx(file, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 16, 0, &lock));
            Check(locked, "hold active or manifest share/header/payload lock");
            if (locked) {
                ShardManifest resolved;
                std::wstring active;
                ShardReadStatus status;
                Check(!ResolveActiveShard(paths, resolved, active, nullptr, &status) &&
                    status == ShardReadStatus::IoError, manifest_lock ?
                    "manifest I/O failure does not scan orphan slots" :
                    "active I/O failure does not select older slot");
                Check(!PublishShardBase(paths, candidate.wstring(), built + 1, 0, resolved) &&
                    std::filesystem::exists(candidate), "I/O failure preserves unpublished candidate");
                if (byte_lock) UnlockFileEx(file, 0, 16, 0, &lock);
            }
            if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
            Check(FileCrc64(paths.manifest) == manifest_crc, "transient I/O preserves manifest bytes");
        }
        reset();
        ShardManifest resolved;
        std::wstring active;
        ShardReadStatus status;
        Check(ResolveActiveShard(paths, resolved, active, nullptr, &status) && active == paths.base_a &&
            status == ShardReadStatus::Valid,
            "unlocked original active resolves normally");
        write(paths.base_a, "bad"); write(paths.base_b, "bad");
        Check(!ResolveActiveShard(paths, resolved, active, nullptr, &status) && status == ShardReadStatus::Invalid,
            "invalid pair reports content failure rather than I/O failure");
        Check(PublishShardBase(paths, candidate.wstring(), built + 1, 0, resolved) &&
            ResolveActiveShard(paths, resolved, active) && ValidateShardBase(active) &&
            resolved.source_id == original.source_id && resolved.generation == original.generation + 1,
            "confirmed invalid pair can publish and resolve healthy rebuild");
        reset();
        std::filesystem::remove(paths.base_a); std::filesystem::remove(paths.base_b);
        Check(!ResolveActiveShard(paths, resolved, active, nullptr, &status) && status == ShardReadStatus::Missing,
            "absent pair reports missing");
        Check(PublishShardBase(paths, candidate.wstring(), built + 1, 0, resolved) &&
            ResolveActiveShard(paths, resolved, active),
            "confirmed missing pair can rebuild despite existing manifest");
        reset();
        auto future = original; future.version = 10;
        Check(SaveShardManifest(paths.manifest, future), "prepare future manifest version");
        Check(!ResolveActiveShard(paths, resolved, active, nullptr, &status) && status == ShardReadStatus::Unsupported &&
            !PublishShardBase(paths, candidate.wstring(), built + 1, 0, resolved) && std::filesystem::exists(candidate),
            "future manifest is preserved rather than rebuilt");
        reset();
        auto future_bytes = bytes;
        const uint32_t future_version = kIndexSnapshotVersion + 1;
        std::memcpy(future_bytes.data() + 4, &future_version, sizeof(future_version));
        write(paths.base_a, future_bytes);
        Check(!ResolveActiveShard(paths, resolved, active, nullptr, &status) && status == ShardReadStatus::Unsupported &&
            !PublishShardBase(paths, candidate.wstring(), built + 1, 0, resolved),
            "future snapshot is not treated as corrupt active");
        reset();
        std::filesystem::remove(paths.manifest);
        HANDLE orphan = CreateFileW(paths.base_b.c_str(), GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
        OVERLAPPED orphan_lock{}; orphan_lock.Offset = 128;
        const bool orphan_locked = orphan != INVALID_HANDLE_VALUE &&
            LockFileEx(orphan, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 16, 0, &orphan_lock);
        Check(orphan_locked, "lock orphan payload with another readable slot present");
        if (orphan_locked) {
            Check(!ResolveActiveShard(paths, resolved, active, nullptr, &status) && status == ShardReadStatus::IoError &&
                !PublishShardBase(paths, candidate.wstring(), built + 1, 0, resolved),
                "orphan payload I/O failure blocks selection and publication");
            UnlockFileEx(orphan, 0, 16, 0, &orphan_lock);
        }
        if (orphan != INVALID_HANDLE_VALUE) CloseHandle(orphan);
        Check(LoadShardManifest(paths.manifest, resolved, nullptr, &status) && status == ShardReadStatus::Missing,
            "missing manifest retains legacy success with explicit missing status");
        auto wide = original;
        wide.active_slot = wide.previous_slot = 1;
        wide.active_bytes = 0x123456789abcdef0ull;
        wide.active_crc64 = 0xfedcba9876543210ull;
        Check(SaveShardManifest(paths.manifest, wide) && LoadShardManifest(paths.manifest, resolved, nullptr, &status) &&
            status == ShardReadStatus::Valid && resolved.active_bytes == wide.active_bytes &&
            resolved.active_crc64 == wide.active_crc64 && resolved.active_slot == resolved.previous_slot,
            "manifest roundtrip preserves unsigned 64-bit fields and equal legacy slots");
        Check(!ResolveActiveShard(paths, resolved, active, nullptr, &status) &&
            !PublishShardBase(paths, candidate.wstring(), built + 1, 0, resolved),
            "equal-slot manifest does not adopt or overwrite unspecified other slot");
        write(paths.manifest, "bad");
        Check(!LoadShardManifest(paths.manifest, resolved, nullptr, &status) && status == ShardReadStatus::Invalid,
            "malformed manifest is invalid rather than I/O failure");
        for (const auto& path : {paths.base_a, paths.base_b, paths.manifest, paths.manifest + L".tmp", candidate.wstring()})
            std::filesystem::remove(path);
        std::filesystem::remove(paths.directory);
        std::filesystem::remove(root);
        return failures ? 1 : 0;
    }
    if (argc > 1 && std::wstring_view(argv[1]) == L"--shard-recovery-only") {
        const auto root = std::filesystem::absolute(std::filesystem::path(L"bench_data") /
            (L"shard-recovery-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64())));
        std::filesystem::create_directories(root);
        const auto paths = MakeShardPaths(root.wstring(), L"recovery-fixture");
        std::string bytes(256, 'x');
        const uint32_t version = kIndexSnapshotVersion;
        const uint64_t built = 123;
        std::memcpy(bytes.data(), "PIDX", 4);
        std::memcpy(bytes.data() + 4, &version, sizeof(version));
        std::memcpy(bytes.data() + 16, &built, sizeof(built));
        auto write = [&](const std::wstring& path, const std::string& data) {
            std::ofstream output(std::filesystem::path(path), std::ios::binary | std::ios::trunc);
            output.write(data.data(), data.size());
        };
        write(paths.base_a, bytes);
        ShardManifest original;
        original.source_id = L"recovery-fixture";
        original.active_built = built;
        original.active_bytes = bytes.size();
        original.active_crc64 = FileCrc64(paths.base_a);
        original.generation = 2;
        Check(SaveShardManifest(paths.manifest, original), "create recovery manifest");
        ShardManifest resolved;
        std::wstring active;
        Check(ResolveActiveShard(paths, resolved, active) && active == paths.base_a,
            "valid active snapshot resolves");
        auto corrupt = bytes; corrupt.back() = 'y';
        write(paths.base_a, corrupt);
        Check(!ResolveActiveShard(paths, resolved, active),
            "checksum-rejected active is not accepted by header-only orphan scan");
        write(paths.base_b, bytes);
        HANDLE file = CreateFileW(paths.base_b.c_str(), GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
        OVERLAPPED lock{}; lock.Offset = 128;
        const bool locked = file != INVALID_HANDLE_VALUE &&
            LockFileEx(file, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 16, 0, &lock);
        Check(locked, "lock fallback payload while keeping header readable");
        if (locked) {
            Check(!ResolveActiveShard(paths, resolved, active), "unreadable fallback payload fails recovery");
            ShardManifest unchanged;
            Check(LoadShardManifest(paths.manifest, unchanged) && unchanged.active_slot == 0 &&
                unchanged.active_crc64 == original.active_crc64,
                "failed fallback read preserves original manifest");
            UnlockFileEx(file, 0, 16, 0, &lock);
        }
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
        Check(SaveShardManifest(paths.manifest, original), "restore manifest before publication failure fixture");
        file = CreateFileW(paths.manifest.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr, OPEN_EXISTING, 0, nullptr);
        Check(file != INVALID_HANDLE_VALUE, "hold manifest without delete sharing");
        if (file != INVALID_HANDLE_VALUE) {
            Check(!ResolveActiveShard(paths, resolved, active), "failed manifest replacement fails recovery");
            const auto candidate = root / L"candidate.bin";
            auto replacement = bytes; replacement.back() = 'z';
            write(candidate.wstring(), replacement);
            const uint64_t original_active_crc = FileCrc64(paths.base_a);
            ShardManifest published;
            Check(!PublishShardBase(paths, candidate.wstring(), built + 1, 0, published),
                "publication rejects unresolved existing manifest");
            Check(std::filesystem::exists(candidate) && FileCrc64(paths.base_a) == original_active_crc,
                "failed recovery leaves publication candidate and existing active slot untouched");
            std::filesystem::remove(candidate);
            CloseHandle(file);
        }
        Check(ResolveActiveShard(paths, resolved, active) && active == paths.base_b &&
            resolved.active_crc64 == original.active_crc64,
            "unlocked fallback publishes complete checksum");
        ShardManifest saved;
        Check(LoadShardManifest(paths.manifest, saved) && saved.active_slot == 1 &&
            saved.active_crc64 == resolved.active_crc64, "recovered manifest persists selected fallback");
        const auto candidate = root / L"next.bin";
        write(candidate.wstring(), bytes);
        ShardManifest published;
        Check(PublishShardBase(paths, candidate.wstring(), built + 1, 0, published) &&
            published.active_slot == 0 && published.previous_slot == 1 &&
            FileCrc64(paths.base_b) == original.active_crc64,
            "resolved manifest permits next publication while preserving active predecessor");
        std::filesystem::remove(paths.manifest);
        std::filesystem::remove(paths.base_a);
        Check(ResolveActiveShard(paths, resolved, active) && active == paths.base_b,
            "missing manifest still recovers an orphan snapshot");
        for (const auto& path : {paths.base_a, paths.base_b, paths.manifest, paths.manifest + L".tmp"})
            std::filesystem::remove(path);
        std::filesystem::remove(paths.directory);
        std::filesystem::remove(root);
        return failures ? 1 : 0;
    }
    if (argc > 1 && std::wstring_view(argv[1]) == L"--delta-read-failure-only") {
        const auto root = std::filesystem::absolute(std::filesystem::path(L"bench_data") /
            (L"delta-read-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64())));
        std::filesystem::create_directories(root);
        const auto path = root / L"changes.dlt";
        unsigned records = 0;
        auto consume = [&](auto...) { ++records; };
        Check(DeltaLog::Replay(path.wstring(), 123, consume) && records == 0,
            "absent optional delta remains acceptable");
        {
            DeltaLog log;
            const bool opened = log.Open(path.wstring(), 123);
            Check(opened, "create isolated delta fixture");
            if (opened) { log.QueueTomb(7); Check(log.Flush(), "commit fixture delta record"); }
        }
        HANDLE locked = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
            nullptr, OPEN_EXISTING, 0, nullptr);
        Check(locked != INVALID_HANDLE_VALUE, "hold existing delta with exclusive sharing");
        if (locked != INVALID_HANDLE_VALUE) {
            DWORD error = ERROR_SUCCESS;
            Check(!DeltaLog::Replay(path.wstring(), 123, consume, &error) && records == 0 &&
                error == ERROR_SHARING_VIOLATION,
                "unreadable existing delta is not reported as successful replay");
            CloseHandle(locked);
        }
        Check(DeltaLog::Replay(path.wstring(), 123, consume) && records == 1,
            "unlocked delta replays committed record");
        { std::ofstream output(path, std::ios::binary | std::ios::trunc); output << "bad"; }
        DWORD error = ERROR_SUCCESS;
        Check(!DeltaLog::Replay(path.wstring(), 123, consume, &error) && error == ERROR_INVALID_DATA,
            "truncated delta is distinguished from transient read failure");
        std::filesystem::remove(path);
        std::filesystem::remove(root);
        return failures ? 1 : 0;
    }
    if (argc > 1 && std::wstring_view(argv[1]) == L"--shard-read-failure-only") {
        const auto root = std::filesystem::absolute(std::filesystem::path(L"bench_data") /
            (L"shard-read-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64())));
        std::filesystem::create_directories(root);
        const auto path = root / L"bytes.bin";
        const std::string contents((1u << 20) + 16, 'x');
        { std::ofstream output(path, std::ios::binary); output.write(contents.data(), contents.size()); }
        uint64_t expected = 14695981039346656037ull;
        for (unsigned char c : contents) { expected ^= c; expected *= 1099511628211ull; }
        Check(FileCrc64(path.wstring()) == expected, "complete shard read hashes all bytes");
        HANDLE file = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
        Check(file != INVALID_HANDLE_VALUE, "open isolated shard lock fixture");
        if (file != INVALID_HANDLE_VALUE) {
            for (DWORD offset : {0u, 1u << 20}) {
                OVERLAPPED lock{}; lock.Offset = offset;
                const bool locked = LockFileEx(file, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY,
                    0, 16, 0, &lock) != FALSE;
                Check(locked, "lock shard read range");
                if (locked) {
                    Check(FileCrc64(path.wstring()) == 0, offset ?
                        "failed second read rejects partial hash" : "failed first read rejects empty hash");
                    Check(UnlockFileEx(file, 0, 16, 0, &lock) != FALSE, "unlock shard read range");
                }
            }
            CloseHandle(file);
        }
        Check(FileCrc64(path.wstring()) == expected, "unlocked shard still hashes correctly");
        const auto candidate = root / L"candidate.bin";
        auto snapshot = contents;
        std::memcpy(snapshot.data(), "PIDX", 4);
        const uint32_t version = kIndexSnapshotVersion;
        std::memcpy(snapshot.data() + 4, &version, sizeof(version));
        { std::ofstream output(candidate, std::ios::binary); output.write(snapshot.data(), snapshot.size()); }
        const auto paths = MakeShardPaths(root.wstring(), L"read-failure-fixture");
        HANDLE locked_file = CreateFileW(candidate.c_str(), GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
        OVERLAPPED lock{}; lock.Offset = 1u << 20;
        const bool locked = locked_file != INVALID_HANDLE_VALUE &&
            LockFileEx(locked_file, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 16, 0, &lock);
        Check(locked, "lock candidate payload beyond readable header");
        if (locked) {
            ShardManifest manifest;
            Check(!PublishShardBase(paths, candidate.wstring(), 123456, 0, manifest),
                "read failure prevents publishing checksum-free manifest");
            Check(!std::filesystem::exists(paths.manifest), "failed publication leaves manifest absent");
            UnlockFileEx(locked_file, 0, 16, 0, &lock);
        }
        if (locked_file != INVALID_HANDLE_VALUE) CloseHandle(locked_file);
        { std::ofstream output(candidate, std::ios::binary); output.write(snapshot.data(), snapshot.size()); }
        ShardManifest published;
        Check(PublishShardBase(paths, candidate.wstring(), 123456, 0, published) &&
            published.active_crc64 != 0 && published.active_bytes == snapshot.size(),
            "complete read still publishes checksummed manifest");
        std::filesystem::remove(candidate);
        std::filesystem::remove(paths.base_a);
        std::filesystem::remove(paths.base_b);
        std::filesystem::remove(paths.manifest);
        std::filesystem::remove(paths.directory);
        std::filesystem::remove(path);
        std::filesystem::remove(root);
        return failures ? 1 : 0;
    }
    if (argc > 1 && std::wstring_view(argv[1]) == L"--mapped-validation-only") {
        Check(EngineTestAccess::InvalidMemoryParentFixture(), "reject unsafe in-memory remap inputs");
        const auto root = std::filesystem::absolute(std::filesystem::path(L"bench_data") /
            (L"mapped-validation-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64())));
        std::filesystem::create_directories(root);
        const auto path = root / L"base.bin";
        Engine source;
        Check(EngineTestAccess::Build(source) && EngineTestAccess::Save(source, path.wstring()), "save valid mapped fixture");
        std::ifstream input(path, std::ios::binary);
        std::vector<char> bytes((std::istreambuf_iterator<char>(input)), {});
        input.close();
        bool valid = bytes.size() >= sizeof(DiskHeader);
        Check(valid, "fixture contains complete disk header");
        if (!valid) return 1;
        auto check_map = [&](const std::vector<char>& data, bool expected, const char* label) {
            { std::ofstream output(path, std::ios::binary | std::ios::trunc); output.write(data.data(), data.size()); }
            Engine mapped;
            Check(EngineTestAccess::Load(mapped, path.wstring()) == expected, label);
        };
        check_map(bytes, true, "valid mapped snapshot still loads");
        auto changed = bytes;
        DiskHeader header{}; std::memcpy(&header, bytes.data(), sizeof(header));
        if (header.node_count >= 3) {
            Node first{}, second{};
            std::memcpy(&first, bytes.data() + header.nodes_off + sizeof(Node), sizeof(Node));
            std::memcpy(&second, bytes.data() + header.nodes_off + 2 * sizeof(Node), sizeof(Node));
            for (int32_t parent : {static_cast<int32_t>(header.node_count), INT32_MAX, int32_t{1}}) {
                changed = bytes;
                auto bad_node = first; bad_node.parent = parent;
                std::memcpy(changed.data() + header.nodes_off + sizeof(Node), &bad_node, sizeof(Node));
                check_map(changed, false, parent == 1 ? "reject snapshot self-parent cycle" :
                    "reject snapshot parent outside node range");
            }
            changed = bytes;
            first.parent = 2; second.parent = 1;
            std::memcpy(changed.data() + header.nodes_off + sizeof(Node), &first, sizeof(Node));
            std::memcpy(changed.data() + header.nodes_off + 2 * sizeof(Node), &second, sizeof(Node));
            check_map(changed, false, "reject snapshot multi-node parent cycle");
            changed = bytes;
            Node root_node{};
            std::memcpy(&root_node, bytes.data() + header.nodes_off, sizeof(Node));
            root_node.parent = -7;
            std::memcpy(changed.data() + header.nodes_off, &root_node, sizeof(Node));
            check_map(changed, true, "preserve arbitrary negative root parent semantics");
        }
        for (uint32_t version : {7u, 8u}) {
            changed = bytes;
            auto legacy = header; legacy.ver = version;
            std::memcpy(changed.data(), &legacy, sizeof(legacy));
            if (legacy.vol_count) {
                DiskVol volume{};
                std::memcpy(&volume, changed.data() + legacy.vols_off, sizeof(volume));
                volume.first_idx = INT32_MAX;
                std::memcpy(changed.data() + legacy.vols_off, &volume, sizeof(volume));
            }
            check_map(changed, true, "legacy v7/v8 ignores reserved volume span field");
        }
        changed = bytes;
        auto bad = header; bad.pool_chars = (uint64_t{1} << 63) + header.pool_chars;
        std::memcpy(changed.data(), &bad, sizeof(bad));
        check_map(changed, false, "reject character count whose byte size wraps");
        changed = bytes; bad = header; bad.frn_count = (uint64_t{1} << 60) + header.frn_count;
        std::memcpy(changed.data(), &bad, sizeof(bad));
        check_map(changed, false, "reject file reference count whose byte size wraps");
        const uint64_t prefix = header.prefix1_off & ~(uint64_t{1} << 63);
        const bool have_prefix = prefix && prefix <= bytes.size() &&
            (65537ull * sizeof(uint32_t)) <= bytes.size() - prefix;
        Check(have_prefix, "fixture contains a complete prefix table");
        if (have_prefix) {
            changed = bytes;
            const uint32_t invalid = UINT32_MAX;
            std::memcpy(changed.data() + prefix + sizeof(uint32_t), &invalid, sizeof(invalid));
            check_map(changed, false, "reject prefix bucket extending beyond posting table");
        }
        valid = header.vol_count && header.vols_off <= bytes.size() && sizeof(DiskVol) <= bytes.size() - header.vols_off;
        Check(valid, "fixture contains a volume record");
        if (valid) {
            DiskVol volume{}; std::memcpy(&volume, bytes.data() + header.vols_off, sizeof(volume));
            auto bad_volume = volume;
            bad_volume.root_idx = static_cast<int32_t>(header.node_count);
            changed = bytes; std::memcpy(changed.data() + header.vols_off, &bad_volume, sizeof(bad_volume));
            check_map(changed, false, "reject volume root outside snapshot");
            bad_volume = volume; bad_volume.first_idx = INT32_MAX;
            changed = bytes; std::memcpy(changed.data() + header.vols_off, &bad_volume, sizeof(bad_volume));
            check_map(changed, false, "reject v9 invalid volume span start");
            bad_volume = volume; bad_volume.item_count = UINT64_MAX;
            changed = bytes; std::memcpy(changed.data() + header.vols_off, &bad_volume, sizeof(bad_volume));
            check_map(changed, false, "reject v9 overflowing volume span length");
            bad_volume = volume;
            std::fill(std::begin(bad_volume.volume_id), std::end(bad_volume.volume_id), L'x');
            changed = bytes; std::memcpy(changed.data() + header.vols_off, &bad_volume, sizeof(bad_volume));
            check_map(changed, false, "reject unterminated volume identifier");
            bad_volume = volume; bad_volume.frn_off = UINT64_MAX - sizeof(DiskFrn); bad_volume.frn_count = 2;
            changed = bytes; std::memcpy(changed.data() + header.vols_off, &bad_volume, sizeof(bad_volume));
            check_map(changed, false, "reject overflowing per-volume reference range");
        }
        std::filesystem::remove(path);
        std::filesystem::remove(root);
        return failures ? 1 : 0;
    }
    const bool quiet = argc > 1 && std::wstring_view(argv[1]) == L"--quiet-maintenance-only";
    if (!SetEnvironmentVariableW(L"PULSE_INDEX_DIAGNOSTICS", quiet ? nullptr : L"1")) return 2;
    if (quiet) return EngineTestAccess::QuietDiagnosticsFixture() ? 0 : 1;
    if (argc > 1 && std::wstring_view(argv[1]) == L"--mft-read-only") return MftReadFixture() ? 0 : 1;
    if (argc > 1 && std::wstring_view(argv[1]) == L"--mixed-usn-only")
        return EngineTestAccess::UsnQueueFixture(true) && EngineTestAccess::FolderSizesFixture(true) ? 0 : 1;
    if (argc > 1 && std::wstring_view(argv[1]) == L"--authorization-only") {
        Engine fixture;
        Check(EngineTestAccess::Build(fixture), "build MFT metadata authorization fixture");
        Query query; query.needle = L".codex"; query.limit = 1; query.rank = false;
        const auto denied = fixture.Search(query, nullptr, 0, [](const auto&) { return false; });
        Check(denied.total == 0 && denied.hits.empty(), "authorization applies to total as well as page");
        const auto allow = [](const std::wstring& path) { return path == L"C:\\Users\\TestUser\\.codex"; };
        const auto first = fixture.Search(query, nullptr, 0, allow);
        Check(first.total == 1 && first.hits.size() == 1 && first.hits[0].path == L"C:\\Users\\TestUser\\.codex",
            "MFT search filters before ranking and page selection");
        query.offset = 1;
        const auto next = fixture.Search(query, nullptr, 0, allow);
        Check(next.total == 1 && next.hits.empty(), "second page cannot expose raw unfiltered matches");
        query.offset = 0;
        const auto revoked = fixture.Search(query, nullptr, 0, [](const auto&) { return false; });
        Check(revoked.total == 0 && revoked.hits.empty(), "caller-specific results never enter shared match cache");
        query.limit = 0;
        const auto count = fixture.Search(query, nullptr, 0, allow);
        Check(count.total == 1 && count.hits.empty(), "count-only query is caller-filtered");
        return failures ? 1 : 0;
    }
    if (argc > 1 && std::wstring_view(argv[1]) == L"--name-pool-only") return EngineTestAccess::NamePoolFixture() ? 0 : 1;
    if (argc > 1 && std::wstring_view(argv[1]) == L"--folder-sizes-only") return EngineTestAccess::FolderSizesFixture() ? 0 : 1;
    if (argc > 1 && std::wstring_view(argv[1]) == L"--startup-catch-up-only") return EngineTestAccess::StartupCatchUpFixture() ? 0 : 1;
    if (argc > 1 && std::wstring_view(argv[1]) == L"--fallback-metadata-only") return EngineTestAccess::FallbackMetadataFixture() ? 0 : 1;
    if (argc > 1 && std::wstring_view(argv[1]) == L"--usn-only") return EngineTestAccess::UsnQueueFixture() ? 0 : 1;
    if (argc > 1 && std::wstring_view(argv[1]) == L"--feed-only") return EngineTestAccess::FeedFixture() ? 0 : 1;
    if (argc > 1 && std::wstring_view(argv[1]) == L"--maintenance-only") return EngineTestAccess::MaintenanceFixture() ? 0 : 1;
    if (argc > 2 && std::wstring_view(argv[1]) == L"--idle-seconds") return EngineTestAccess::IdleFixture(static_cast<unsigned>(wcstoul(argv[2], nullptr, 10))) ? 0 : 1;
    if (argc > 1 && std::wstring_view(argv[1]) == L"--parent-cycle-only") return EngineTestAccess::ParentCycleFixture() ? 0 : 1;
    if (argc > 1 && std::wstring_view(argv[1]) == L"--recycle-only") return EngineTestAccess::RecycleFixture() ? 0 : 1;
    if (argc > 1 && std::wstring_view(argv[1]) == L"--coverage-only") return EngineTestAccess::CoverageBenchmark() ? 0 : 1;
    std::cout << std::unitbuf;
    Engine engine;
    Check(EngineTestAccess::Build(engine), "actual MFT tree builder accepts fixture");
    CheckSearch(engine);
    const auto dir = std::filesystem::absolute(std::filesystem::path(L"bench_data") /
        (L"index_engine_test_" + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(GetTickCount64())));
    std::filesystem::create_directories(dir);
    {
        const auto log_path = dir / L"partial-write.dlt";
        DeltaLog delta;
        Check(delta.Open(log_path.wstring(), 42), "open partial-write fixture");
        constexpr int records = 70000;
        for (int i = 0; i < records; ++i) delta.QueueUsn(1, i);
        HANDLE blocker = CreateFileW(log_path.c_str(), GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
        OVERLAPPED lock{};
        lock.Offset = 16 + (1u << 20);
        const bool locked = blocker != INVALID_HANDLE_VALUE &&
            LockFileEx(blocker, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &lock);
        Check(locked, "lock second write chunk");
        if (locked) {
            Check(!delta.Flush() && delta.HasPending(), "partial write retains all pending records");
            UnlockFileEx(blocker, 0, 1, 0, &lock);
        }
        if (blocker != INVALID_HANDLE_VALUE) CloseHandle(blocker);
        Check(delta.Flush() && !delta.HasPending(), "retry writes a complete log");
        delta.Close();
        Check(std::filesystem::file_size(log_path) == 16ull + records * 17ull,
              "partial write retry does not duplicate log bytes");
    }
    {
        const auto selected = dir / L"selected-index";
        const auto next = dir / L"next-index";
        std::filesystem::create_directory(selected);
        std::filesystem::create_directory(next);
        SetMachineIndexScope(true);
        SetActiveIndexDirectory(selected.wstring());
        Check(DataDir() == selected.wstring(), "running host reports actual index directory");
        Check(CacheFilePath() == selected.wstring() + L"\\pulse-index.bin",
              "base cache follows active directory");
        const auto delta_path = DeltaFilePath(L'C');
        {
            DeltaLog delta;
            Check(delta.Open(delta_path, 123), "open delta in selected directory");
            delta.QueueUsn(1, 2);
            Check(delta.Flush(), "update delta in selected directory");
        }
        const auto old_size = std::filesystem::file_size(delta_path);
        SetActiveIndexDirectory(next.wstring());
        {
            DeltaLog delta;
            Check(delta.Open(DeltaFilePath(L'C'), 124), "reopened delta uses replacement directory");
            delta.QueueUsn(1, 3);
            Check(delta.Flush(), "replacement directory receives index updates");
        }
        Check(std::filesystem::file_size(delta_path) == old_size &&
              std::filesystem::exists(next / L"pulse-index-C.dlt"),
              "closed old directory receives no replacement writes");
        Check(DeltaFilePathForVolume(L"test-volume").find(next.wstring() + L"\\") == 0,
              "volume shard delta follows active directory");
        SetActiveIndexDirectory(L"");
        SetMachineIndexScope(false);
    }
    const auto base = dir / L"base.bin";
    Check(EngineTestAccess::Save(engine, base.wstring()), "write snapshot");
    Check(ValidateShardBase(base.wstring()), "shard accepts new snapshot");
    for (bool mapped_fixture : {false, true}) {
        const auto replay_dir = dir / (mapped_fixture ? L"mapped-attr" : L"live-attr");
        std::filesystem::create_directory(replay_dir);
        SetActiveIndexDirectory(replay_dir.wstring());
        Engine replayed;
        Check(mapped_fixture ? EngineTestAccess::Load(replayed, base.wstring()) :
              EngineTestAccess::Build(replayed), "prepare attr-only replay fixture");
        Check(EngineTestAccess::ReplayAttrOnly(replayed), "attr-only replay updates attributes and preserves directory flags");
        Check(Has(replayed, L"ShowBoxDebug.log", L"C:\\Users\\TestUser\\AppData\\Local\\Temp\\ShowBoxDebug.log"),
              "attr-only replay preserves filename search and full path");
        const auto compact = replay_dir / L"compact.bin";
        Check(EngineTestAccess::Save(replayed, compact.wstring()), "compact attr-only replay snapshot");
        Engine reloaded;
        Check(EngineTestAccess::Load(reloaded, compact.wstring()), "reload attr-only replay snapshot");
        Check(Has(reloaded, L"ShowBoxDebug.log", L"C:\\Users\\TestUser\\AppData\\Local\\Temp\\ShowBoxDebug.log"),
              "attr-only replay remains searchable after compact and reload");
        SetActiveIndexDirectory(L"");
    }
    {
        Engine mapped;
        Check(EngineTestAccess::Load(mapped, base.wstring()), "load snapshot");
        Check(!EngineTestAccess::NeedsRebuild(mapped), "new snapshot does not rebuild again");
        CheckSearch(mapped);
        EngineTestAccess::Usn(mapped, 30, 40, L".codex", USN_REASON_RENAME_NEW_NAME);
        Check(Search(mapped, L"settings.toml").total == 0, "moving directory hides existing descendants");
        EngineTestAccess::Usn(mapped, 30, 20, L".codex", USN_REASON_RENAME_NEW_NAME);
        Check(Has(mapped, L"settings.toml", L"C:\\Users\\TestUser\\.codex\\settings.toml"),
              "moving directory restores existing descendants");
        EngineTestAccess::Usn(mapped, 100, 20, L".codex-new", USN_REASON_FILE_CREATE);
        Check(Has(mapped, L".codex-new", L"C:\\Users\\TestUser\\.codex-new"), "USN create searchable");
        EngineTestAccess::Usn(mapped, 100, 40, L".codex-new", USN_REASON_RENAME_NEW_NAME);
        Check(Search(mapped, L".codex-new").total == 0, "USN move into excluded directory");
        EngineTestAccess::Usn(mapped, 100, 20, L".codex-new", USN_REASON_RENAME_NEW_NAME);
        Check(Search(mapped, L".codex-new").total == 1, "USN move out of excluded directory");
        EngineTestAccess::Usn(mapped, 100, 20, L".codex-new", USN_REASON_FILE_DELETE);
        Check(Search(mapped, L".codex-new").total == 0, "USN delete removed");
    }
    LegacyRepairTests(dir, base);
    // This directory was created exclusively by this process under bench_data.
    std::filesystem::remove_all(dir);
    return failures ? 1 : 0;
}
