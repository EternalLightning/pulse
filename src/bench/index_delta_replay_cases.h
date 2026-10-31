// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

    static bool DeltaReplayFixture() {
        bool ok = true;
        auto check = [&](bool value, const char* label) {
            std::cout << (value ? "[PASS] " : "[FAIL] ") << label << '\n';
            ok &= value;
        };
        const auto root = std::filesystem::absolute(std::filesystem::path(L"bench_data") /
            (L"delta-replay-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64())));
        std::filesystem::create_directories(root);
        SetMachineIndexScope(false);
        SetActiveIndexDirectory(root.wstring());
        const auto base_path = root / L"base.bin";
        {
            Engine source;
            source.running_ = true;
            for (wchar_t letter : {L'C', L'D'}) {
                Engine::VolState v;
                v.letter = letter;
                v.volume_id = std::wstring(L"delta-fixture-") + letter;
                v.kind = VolumeKind::Fixed;
                Engine::FrnNode seed;
                seed.frn = 6;
                seed.parent = 5;
                seed.name = L"seed.txt";
                if (!source.BuildMftTree(std::move(v), 5, {seed})) {
                    check(false, "build isolated volume root");
                    return false;
                }
                check(true, "build isolated volume root");
            }
            source.live_ = std::move(source.build_);
            source.vols_ = std::move(source.build_vols_);
            check(Save(source, base_path.wstring()), "save two-volume base snapshot");
        }
        auto usn = [](Engine& e, size_t volume, uint64_t frn, uint64_t parent,
                      const wchar_t* name, DWORD reason, bool directory = false) {
            const auto length = wcslen(name) * sizeof(wchar_t);
            std::vector<BYTE> buffer(sizeof(USN_RECORD_V2) + length);
            auto* record = reinterpret_cast<USN_RECORD_V2*>(buffer.data());
            record->RecordLength = static_cast<DWORD>(buffer.size());
            record->MajorVersion = 2;
            record->FileReferenceNumber = frn;
            record->ParentFileReferenceNumber = parent;
            record->Reason = reason;
            record->FileAttributes = directory ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
            record->FileNameOffset = offsetof(USN_RECORD_V2, FileName);
            record->FileNameLength = static_cast<WORD>(length);
            std::memcpy(record->FileName, name, length);
            e.ApplyUsnLocked(e.vols_[volume], record);
        };
        int32_t c_file = -1, d_folder = -1, d_child = -1;
        {
            Engine writer;
            if (!Load(writer, base_path.wstring())) {
                check(false, "load production-mapped base for journal writer");
                return false;
            }
            check(true, "load production-mapped base for journal writer");
            writer.StopPinyinWorker();
            writer.OpenDeltasLocked();
            usn(writer, 1, 100, 5, L"pulse_delta_folder", USN_REASON_FILE_CREATE, true);
            usn(writer, 0, 200, 5, L"pulse_delta_c.txt", USN_REASON_FILE_CREATE);
            usn(writer, 1, 101, 100, L"pulse_delta_child.txt", USN_REASON_FILE_CREATE);
            usn(writer, 0, 201, 5, L"pulse_delta_keep.txt", USN_REASON_FILE_CREATE);
            c_file = writer.FindByFrnLocked(writer.vols_[0], 200);
            d_folder = writer.FindByFrnLocked(writer.vols_[1], 100);
            d_child = writer.FindByFrnLocked(writer.vols_[1], 101);
            check(d_folder == writer.BaseCount() && c_file == d_folder + 1 && d_child == c_file + 1,
                "production journal assigns interleaved global node IDs");
            usn(writer, 0, 200, 5, L"pulse_delta_renamed.txt", USN_REASON_RENAME_NEW_NAME);
            usn(writer, 1, 101, 100, L"pulse_delta_child.txt", USN_REASON_FILE_DELETE);
            writer.DeltaFor(L'C')->QueuePatch(c_file, static_cast<uint8_t>(PatchBits::Attr), 0, 0, 123, 456, {});
            writer.DeltaFor(L'C')->QueueUsn(10, 1000);
            writer.DeltaFor(L'D')->QueueUsn(20, 2000);
            writer.FlushDeltas();
            writer.CloseDeltas();
        }
        {
            Engine reader;
            check(Load(reader, base_path.wstring()), "load production-mapped base for replay");
            reader.StopPinyinWorker();
            reader.ReplayDeltasLocked();
            check(reader.FindByFrnLocked(reader.vols_[0], 200) == c_file &&
                reader.FindByFrnLocked(reader.vols_[1], 100) == d_folder &&
                reader.FindByFrnLocked(reader.vols_[1], 101) == d_child,
                "replay preserves cross-volume node identities");
            check(reader.BuildPathLocked(c_file) == L"C:\\pulse_delta_renamed.txt" && !reader.IsTomb(c_file),
                "rename remains attached to original C-volume file");
            check(reader.BuildPathLocked(d_folder) == L"D:\\pulse_delta_folder" && !reader.IsTomb(d_folder),
                "other volume retains its newly created directory");
            check(reader.NodeAt(d_child).parent == d_folder && reader.IsTomb(d_child),
                "child parent and tombstone retain original D-volume identity");
            check(reader.AttrAt(c_file).size == 456 && reader.AttrAt(c_file).mtime == 123,
                "attribute patch reaches original file after replay");
            check(reader.vols_[0].next_usn == 1000 && reader.vols_[1].next_usn == 2000,
                "successful replay retains each volume checkpoint");
        }
        int32_t later_id = -1;
        {
            Engine continuation;
            if (!Load(continuation, base_path.wstring())) return false;
            continuation.StopPinyinWorker();
            continuation.ReplayDeltasLocked();
            continuation.OpenDeltasLocked();
            usn(continuation, 1, 102, 100, L"pulse_delta_later.txt", USN_REASON_FILE_CREATE);
            later_id = continuation.FindByFrnLocked(continuation.vols_[1], 102);
            continuation.DeltaFor(L'D')->QueuePatch(later_id, static_cast<uint8_t>(PatchBits::Attr), 0, 0, 789, 987, {});
            continuation.DeltaFor(L'D')->QueueUsn(20, 3000);
            continuation.FlushDeltas();
            continuation.CloseDeltas();
        }
        {
            Engine restart;
            if (!Load(restart, base_path.wstring())) return false;
            restart.StopPinyinWorker();
            restart.ReplayDeltasLocked();
            check(!restart.delta_replay_failed_ && restart.FindByFrnLocked(restart.vols_[1], 102) == later_id &&
                restart.NodeAt(later_id).parent == d_folder && !restart.IsTomb(later_id) &&
                restart.BuildPathLocked(later_id) == L"D:\\pulse_delta_folder\\pulse_delta_later.txt",
                "second restart preserves appended same-volume child and cross-volume identities");
            check(restart.AttrAt(later_id).size == 987 && restart.vols_[1].next_usn == 3000 &&
                restart.vols_[0].next_usn == 1000 && restart.IsTomb(d_child),
                "second restart preserves attributes tombstones and independent checkpoints");
        }
        const auto legacy_path = DeltaFilePathForVolume(L"delta-fixture-C");
        {
            const auto read_bytes = [](const std::filesystem::path& path) {
                std::ifstream input(path, std::ios::binary);
                return std::vector<char>(std::istreambuf_iterator<char>(input), {});
            };
            const auto original_base = read_bytes(base_path);
            const auto original_log = read_bytes(std::filesystem::path(legacy_path));
            auto corrupted_base = original_base;
            DiskHeader header{};
            std::memcpy(&header, corrupted_base.data(), sizeof(header));
            Node root_node{};
            std::memcpy(&root_node, corrupted_base.data() + header.nodes_off, sizeof(root_node));
            root_node.unused = header.node_count;
            std::memcpy(corrupted_base.data() + header.nodes_off, &root_node, sizeof(root_node));
            { std::ofstream output(base_path, std::ios::binary | std::ios::trunc);
              output.write(corrupted_base.data(), static_cast<std::streamsize>(corrupted_base.size())); }
            {
                DeltaLog malicious;
                check(malicious.Open(legacy_path, 123456), "open isolated cross-volume corruption fixture");
                malicious.QueueTomb(3);
                check(malicious.Flush(), "append wrong-volume tombstone to isolated log");
            }
            {
                Engine rejected;
                const bool loaded = Load(rejected, base_path.wstring());
                check(loaded, "map parent-valid fixture with forged DFS subtree shortcut");
                if (loaded) {
                    rejected.StopPinyinWorker();
                    rejected.ReplayDeltasLocked();
                    check(rejected.delta_replay_failed_ && rejected.live_.nodes.empty() &&
                        !rejected.IsTomb(3) && rejected.vols_[0].next_usn == 0 && rejected.vols_[1].next_usn == 0,
                        "wrong-volume patch cannot use forged DFS range to bypass identity validation");
                }
            }
            { std::ofstream output(base_path, std::ios::binary | std::ios::trunc);
              output.write(original_base.data(), static_cast<std::streamsize>(original_base.size())); }
            { std::ofstream output(std::filesystem::path(legacy_path), std::ios::binary | std::ios::trunc);
              output.write(original_log.data(), static_cast<std::streamsize>(original_log.size())); }
        }
        {
            std::fstream legacy(std::filesystem::path(legacy_path), std::ios::binary | std::ios::in | std::ios::out);
            const uint32_t version = 1;
            legacy.seekp(4); legacy.write(reinterpret_cast<const char*>(&version), sizeof(version));
        }
        const auto legacy_crc = FileCrc64(legacy_path);
        {
            Engine recovery;
            if (!Load(recovery, base_path.wstring())) return false;
            recovery.StopPinyinWorker();
            recovery.ReplayDeltasLocked();
            recovery.OpenDeltasLocked();
            check(recovery.delta_replay_failed_ && recovery.NeedsSearchRebuildLocked() &&
                recovery.live_.nodes.empty() && recovery.delta_logs_.empty() &&
                recovery.vols_[0].next_usn == 0 && recovery.vols_[1].next_usn == 0,
                "legacy logs require rebuild without guessing identities or consuming checkpoints");
            check(legacy_crc && FileCrc64(legacy_path) == legacy_crc,
                "legacy log remains byte-identical before recovery publication");
            const auto letter_path = DeltaFilePath(L'C');
            check(MoveFileExW(legacy_path.c_str(), letter_path.c_str(), MOVEFILE_REPLACE_EXISTING) != FALSE,
                "prepare actual legacy letter-keyed log with canonical file absent");
            recovery.ReplayDeltasLocked();
            recovery.OpenDeltasLocked();
            check(recovery.delta_replay_failed_ && recovery.delta_logs_.empty() &&
                FileCrc64(letter_path) == legacy_crc && recovery.vols_[0].next_usn == 0,
                "letter-keyed legacy log is retained and requires recovery before new writes");
            const auto data = root / L"recovery-data";
            std::filesystem::create_directory(data);
            { std::ofstream fresh(data / L"fresh.txt"); fresh << "fresh"; }
            recovery.fixture_root_ = data.wstring();
            recovery.FullRebuild("legacy_delta_fixture");
            recovery.StopWalkWatches();
            recovery.StopPinyinWorker();
            check(!recovery.delta_replay_failed_ && recovery.ResolvePathLocked((data / L"fresh.txt").wstring()) >= 0,
                "legacy delta recovery publishes filesystem-derived isolated snapshot");
        }
        {
            Engine recovered;
            check(recovered.TryLoadCache() && !recovered.delta_replay_failed_ &&
                recovered.ResolvePathLocked((root / L"recovery-data" / L"fresh.txt").wstring()) >= 0,
                "restart after legacy recovery loads published snapshot and current log format");
            recovered.StopPinyinWorker();
        }
        SetActiveIndexDirectory(L"");
        std::error_code error;
        std::filesystem::remove_all(root, error);
        check(!error, "isolated delta replay fixture cleanup");
        return ok;
    }
