// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once

namespace pulse::index {
bool EngineTestAccess::QuietDiagnosticsFixture() {
    bool ok = true;
    auto check = [&](bool passed, const char* label) {
        std::cout << (passed ? "[PASS] " : "[FAIL] ") << label << '\n';
        ok &= passed;
    };
    check(!IndexDiagnosticsEnabled(), "temporary index diagnostics are disabled by default");
    const auto dir = std::filesystem::absolute(std::filesystem::path(L"bench_data") /
        (L"quiet-diagnostics-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64())));
    std::filesystem::create_directories(dir);
    SetMachineIndexScope(false);
    SetActiveIndexDirectory(dir.wstring());
    const auto log = dir / L"pulse-index-timing.jsonl";
    {
        FilenameTiming timing;
        const auto token = FilenameTiming::Begin();
        timing.End(FilenameStage::Journal, token, 1, ERROR_WRITE_FAULT, "fixture");
        timing.Memory().Capture(IndexMemoryPoint::TimingFlush);
        timing.Flush(true);
        check(!timing.Due() && token.wall == 0 && token.cpu == 0 &&
            timing.Memory().At(IndexMemoryPoint::TimingFlush).calls == 0,
            "disabled diagnostics skip timers, process samples and flush scheduling");
        check(!std::filesystem::exists(log), "forced flush does not create a timing log when disabled");
        { std::ofstream sentinel(log); sentinel << "keep-existing"; }
        timing.Flush(true);
        std::ifstream sentinel(log);
        std::string content((std::istreambuf_iterator<char>(sentinel)), std::istreambuf_iterator<char>());
        check(content == "keep-existing", "disabled diagnostics neither append to nor truncate an existing log");
        sentinel.close();
        std::filesystem::remove(log);
    }
    for (bool mapped : {false, true}) {
        Engine e;
        check(Build(e), "build isolated quiet maintenance fixture");
        if (mapped) e.MergeBase(true, "fixture");
        check(!mapped || e.map_ != nullptr, "prepare heap or mapped base with diagnostics disabled");
        std::vector<std::wstring> before;
        for (int32_t i = 0; i < e.LiveCount(); ++i) before.push_back(e.BuildPathLocked(i));
        const auto epoch = e.feed_epoch_.load(), revision = e.Revision();
        auto* base = e.map_.get();
        const auto original_chars = e.live_.pool.size();
        constexpr size_t garbage = 1ull << 20;
        e.live_.pool.insert(e.live_.pool.end(), garbage, L'x');
        e.pool_waste_ += garbage;
        {
            std::unique_lock lock(e.mutex_);
            check(e.CompactNamePoolLocked(GetTickCount64()), "name pool repair still runs with diagnostics disabled");
        }
        std::vector<std::wstring> after;
        for (int32_t i = 0; i < e.LiveCount(); ++i) after.push_back(e.BuildPathLocked(i));
        check(e.live_.pool.size() == original_chars && e.pool_waste_ == 0 && before == after &&
            e.feed_epoch_ == epoch && e.Revision() == revision && e.map_.get() == base,
            "quiet compaction reclaims garbage without changing paths or generations");
        e.CaptureMemoryState();
        check(e.filename_timing_.Maintenance().pool.attempts == 0 &&
            e.filename_timing_.Memory().retained.containers_tick == 0,
            "quiet maintenance does not collect pool or retained-container diagnostics");
        e.struct_changes_ = 1;
        e.MergeBase(true, "fixture");
        check(e.map_ && e.struct_changes_ == 0 &&
            e.filename_timing_.Maintenance().merge.attempts == 0,
            "necessary full merge still commits without diagnostic collection");
        Query query; query.needle = L"settings.toml";
        check(e.Search(query).total == 1, "filename query remains correct after quiet maintenance");
        check(!std::filesystem::exists(log), "maintenance does not recreate the removed timing log");
    }
    {
        ChangeTracker history;
        IndexMemoryProbe memory;
        history.Open(dir.wstring()); history.Lease(L"quiet-fixture", true);
        ChangeRecord record;
        record.kind = ChangeKind::Created;
        record.path = L"C:\\quiet-fixture\\saved.txt";
        history.Record(record);
        history.Flush(true, &memory);
        const auto page = history.Details(L"quiet-fixture", L"C:\\quiet-fixture", 0, 0, 200);
        check(page.records.size() == 1 && NormalizeChangePath(page.records.front().path) == NormalizeChangePath(record.path),
            "change history remains readable when performance diagnostics are disabled");
        check(memory.retained.history_tick == 0 && memory.At(IndexMemoryPoint::ChangeFlushBefore).calls == 0,
            "history flush skips temporary process and capacity probes");
    }
    {
        ChangeTracker reloaded;
        reloaded.Open(dir.wstring()); reloaded.Lease(L"quiet-fixture", true);
        const auto saved = reloaded.Details(L"quiet-fixture", L"C:\\quiet-fixture", 0, 0, 200);
        check(saved.records.size() == 1 && saved.records.front().kind == ChangeKind::Created,
            "quiet history flush persisted the record across reopening");
    }
    SetActiveIndexDirectory(L"");
    std::error_code error;
    std::filesystem::remove_all(dir, error);
    check(!error, "quiet diagnostic fixture removed only its isolated test data");
    return ok;
}
}
