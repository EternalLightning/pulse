// Included inside EngineTestAccess to exercise the production ingest paths.
static bool IngestBoundaryFixture() {
    bool ok = true;
    const auto check = [&](bool value, const char* label) {
        std::cout << (value ? "[PASS] " : "[FAIL] ") << label << '\n'; ok &= value;
    };
    const auto record = [] {
        std::vector<BYTE> bytes(72, 0);
        auto* value = reinterpret_cast<USN_RECORD_V2*>(bytes.data());
        value->RecordLength = static_cast<DWORD>(bytes.size()); value->MajorVersion = 2;
        value->FileReferenceNumber = 9999; value->ParentFileReferenceNumber = 20;
        value->Reason = USN_REASON_FILE_CREATE;
        value->FileNameOffset = 64; value->FileNameLength = 4;
        std::memcpy(bytes.data() + value->FileNameOffset, L"ok", value->FileNameLength);
        return bytes;
    };
    auto valid = record();
    UsnRecordView view;
    check(ReadUsnRecord(valid.data(), valid.size(), view) && view.name == L"ok" &&
        view.record.FileReferenceNumber == 9999, "USN parser honors a valid nondefault filename offset");
    for (size_t length : {size_t{0}, size_t{7}, size_t{59}, size_t{71}})
        check(!ReadUsnRecord(valid.data(), length, view), "USN parser rejects truncated fixed headers and records");
    const std::function<void(USN_RECORD_V2&)> corruptions[] = {
        [](auto& value) { value.RecordLength = 0; },
        [](auto& value) { value.RecordLength = 56; },
        [](auto& value) { value.RecordLength = 71; },
        [](auto& value) { value.RecordLength = MAXDWORD; },
        [](auto& value) { value.MajorVersion = 3; },
        [](auto& value) { value.MinorVersion = 1; },
        [](auto& value) { value.FileNameOffset = 58; },
        [](auto& value) { value.FileNameOffset = 61; },
        [](auto& value) { value.FileNameOffset = 74; },
        [](auto& value) { value.FileNameLength = 3; },
        [](auto& value) { value.FileNameLength = 10; },
        [](auto& value) { value.FileNameLength = MAXWORD; }
    };
    for (const auto& corrupt : corruptions) {
        auto bytes = record(); corrupt(*reinterpret_cast<USN_RECORD_V2*>(bytes.data()));
        check(!ReadUsnRecord(bytes.data(), bytes.size(), view), "USN parser rejects malformed bounds, alignment and versions");
    }
    auto records = valid;
    records.push_back(0);
    check(!ValidateUsnRecords(records.data(), records.size()), "USN parser rejects an incomplete trailing header");
    records = valid; records.insert(records.end(), valid.begin(), valid.end());
    check(ValidateUsnRecords(records.data(), records.size()), "USN parser accepts a complete multi-record batch");

    {
        Engine engine;
        check(Build(engine), "build isolated journal corruption fixture");
        auto& volume = engine.vols_.front(); volume.journal_id = 1; volume.next_usn = 10;
        engine.change_signal_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        auto stream = UsnStreamTestAccess::Make(engine.change_signal_);
        auto* collector = stream.get();
        engine.journal_streams_[volume.volume_id] = std::move(stream);
        const auto count = engine.LiveCount();
        const auto epoch = engine.feed_epoch_.load();
        std::vector<BYTE> packet(sizeof(int64_t));
        const int64_t next = 100;
        std::memcpy(packet.data(), &next, sizeof(next));
        packet.insert(packet.end(), valid.begin(), valid.end());
        auto broken = record();
        reinterpret_cast<USN_RECORD_V2*>(broken.data())->FileNameLength = MAXWORD;
        packet.insert(packet.end(), broken.begin(), broken.end());
        check(UsnStreamTestAccess::Push(*collector, packet), "queue a complete record followed by corrupt metadata");
        bool changed = false, structural = false;
        check(!engine.CatchUpVolume(volume, &changed, &structural) && volume.next_usn == 10 &&
            volume.journal_id == 0 && engine.feed_epoch_ != epoch && engine.LiveCount() == count &&
            !changed && !structural, "corrupt journal batch gaps without partial ingestion or cursor advancement");
    }

    const auto fixture = std::filesystem::absolute(std::filesystem::path(L"bench_data") /
        (L"ingest-boundary-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64())));
    for (bool mapped : {false, true}) {
        const auto root = fixture / (mapped ? L"mapped" : L"live");
        std::filesystem::create_directories(root / L"tree");
        std::filesystem::create_directories(root / L"blocked");
        std::filesystem::create_directories(root / L"NODE_MODULES");
        const auto write = [](const auto& path) { std::ofstream file(path); file << "fixture"; return !!file; };
        check(write(root / L"tree" / L"leaf.txt") && write(root / L"keep.txt"), "prepare real watcher visibility fixture");
        Engine engine;
        engine.AddForTest((root / L"tree" / L"leaf.txt").wstring(), L"leaf.txt", false, 7);
        engine.AddForTest((root / L"keep.txt").wstring(), L"keep.txt", false, 7);
        if (mapped) {
            check(Save(engine, (root / L"snapshot.bin").wstring()) && Load(engine, (root / L"snapshot.bin").wstring()),
                "map watcher visibility fixture to exercise base-node patches");
            engine.StopPinyinWorker();
        }
        engine.running_ = true; engine.ready_ = true;
        engine.excluded_paths_ = {(root / L"blocked").wstring()};
        Query query; query.needle = L"*.txt"; query.path_prefix = root.wstring(); query.rank = false; query.limit = 20;
        const auto search = [&] { return engine.Search(query); };
        const auto notify = [&](DWORD action, const std::wstring& relative) {
            std::vector<BYTE> bytes(sizeof(FILE_NOTIFY_INFORMATION) + relative.size() * sizeof(wchar_t), 0);
            auto* info = reinterpret_cast<FILE_NOTIFY_INFORMATION*>(bytes.data());
            info->Action = action; info->FileNameLength = static_cast<DWORD>(relative.size() * sizeof(wchar_t));
            std::memcpy(info->FileName, relative.data(), info->FileNameLength);
            return engine.ApplyNotifyLocked(root.wstring(), bytes.data(), static_cast<DWORD>(bytes.size()));
        };
        const auto rename = [&](const std::wstring& old_name, const std::wstring& new_name) {
            std::filesystem::rename(root / old_name, root / new_name);
            return notify(FILE_ACTION_RENAMED_OLD_NAME, old_name) == 1 &&
                notify(FILE_ACTION_RENAMED_NEW_NAME, new_name) == 1;
        };
        auto feed = engine.ReadFeed(true, L"", 0, 0);
        check(search().total == 2, "ordinary initial files remain searchable");
        check(write(root / L"blocked" / L"private.txt") && notify(FILE_ACTION_ADDED, L"blocked\\private.txt") == 1 &&
            write(root / L"NODE_MODULES" / L"package.txt") && notify(FILE_ACTION_ADDED, L"NODE_MODULES\\package.txt") == 1 &&
            search().total == 2, "watcher additions inherit exclusion and skipped ancestor visibility");
        auto page = engine.ReadFeed(true, L"", feed.epoch, feed.next);
        check(!page.gap && page.records.empty(), "excluded watcher additions never enter the change feed");
        feed = page;
        check(rename(L"tree", L"blocked\\tree") && search().total == 1,
            "moving a visible directory into an exclusion hides all descendants");
        page = engine.ReadFeed(true, L"", feed.epoch, feed.next);
        check(!page.gap && page.records.size() == 1 && page.records[0].kind == ChangeKind::Deleted &&
            page.records[0].path == (root / L"tree").wstring() && page.records[0].old_path.empty(),
            "crossing into an exclusion emits only deletion of the prior visible path");
        feed = page;
        const bool restored = rename(L"blocked\\tree", L"shown");
        const auto restored_result = search();
        check(restored && restored_result.total == 2 &&
            std::any_of(restored_result.hits.begin(), restored_result.hits.end(), [&](const Hit& hit) {
                return hit.path == (root / L"shown" / L"leaf.txt").wstring();
            }),
            "moving a hidden directory out restores descendant search visibility");
        page = engine.ReadFeed(true, L"", feed.epoch, feed.next);
        check(!page.gap && page.records.size() == 1 && page.records[0].kind == ChangeKind::Created &&
            page.records[0].path == (root / L"shown").wstring() && page.records[0].old_path.empty(),
            "crossing out of an exclusion emits creation without an excluded old path");
        check(rename(L"shown", L"servicing") && search().total == 1 &&
            rename(L"servicing", L"restored") && search().total == 2,
            "directory rename recomputes skip-name policy for the whole subtree");
        check(rename(L"keep.txt", L"blocked\\moved.txt") && search().total == 1 &&
            rename(L"blocked\\moved.txt", L"kept.txt") && search().total == 2,
            "file rename recomputes live and mapped visibility in both directions");
        std::filesystem::create_directories(root / L"NODE_MODULES" / L"newdir");
        notify(FILE_ACTION_RENAMED_NEW_NAME, L"NODE_MODULES\\newdir");
        const auto directory = engine.ResolvePathLocked((root / L"NODE_MODULES" / L"newdir").wstring());
        constexpr auto directory_flags = Engine::kFlagDir | Engine::kFlagHidden;
        check(directory >= 0 && (engine.NodeAt(directory).flags & directory_flags) == directory_flags,
            "unpaired rename resolves the actual directory kind while preserving hidden ancestry");
    }
    std::error_code error;
    std::filesystem::remove_all(fixture, error);
    check(!error, "isolated ingest boundary fixtures cleaned up");
    return ok;
}
