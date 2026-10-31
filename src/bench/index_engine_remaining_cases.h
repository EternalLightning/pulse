// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

    static std::vector<BYTE> CatchUpPacket(int64_t cursor, bool malformed = false) {
        constexpr size_t record_bytes = 72;
        std::vector<BYTE> bytes(sizeof(cursor) + 2 * record_bytes);
        const int64_t next = cursor + 2;
        std::memcpy(bytes.data(), &next, sizeof(next));
        for (size_t i = 0; i < 2; ++i) {
            auto* record = reinterpret_cast<USN_RECORD_V2*>(bytes.data() + sizeof(cursor) + i * record_bytes);
            record->RecordLength = record_bytes;
            record->MajorVersion = malformed && i ? 3 : 2;
            record->FileReferenceNumber = 10 + cursor - 100 + i;
            record->ParentFileReferenceNumber = 5;
            record->Usn = cursor + i;
            record->Reason = USN_REASON_FILE_DELETE;
            record->FileNameOffset = offsetof(USN_RECORD_V2, FileName);
            record->FileNameLength = sizeof(wchar_t);
            record->FileName[0] = L'x';
        }
        return bytes;
    }

    static bool StartupCatchUpFixture() {
        bool ok = true;
        auto check = [&](bool value, const char* label) {
            std::cout << (value ? "[PASS] " : "[FAIL] ") << label << '\n'; ok &= value;
        };
        const auto root = std::filesystem::absolute(std::filesystem::path(L"bench_data") /
            L"audit-fixes-20261030-remaining" /
            (L"usn-catch-up-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64())));
        std::filesystem::create_directories(root);
        SetMachineIndexScope(false);
        SetActiveIndexDirectory(root.wstring());
        const auto base_path = (root / L"base.bin").wstring();
        {
            Engine source;
            source.running_ = true;
            Engine::VolState volume;
            volume.letter = L'Q'; volume.kind = VolumeKind::Fixed;
            volume.volume_id = L"isolated-catch-up-fixture";
            volume.journal_id = 9; volume.next_usn = 100;
            std::vector<Engine::FrnNode> nodes;
            for (uint64_t id = 10; id < 16; ++id) {
                Engine::FrnNode node;
                node.frn = id; node.parent = 5; node.name = L"file" + std::to_wstring(id) + L".txt";
                node.size = id;
                nodes.push_back(std::move(node));
            }
            check(source.BuildMftTree(std::move(volume), 5, std::move(nodes)), "build six-file isolated USN base");
            source.live_ = std::move(source.build_);
            source.vols_ = std::move(source.build_vols_);
            check(Save(source, base_path), "persist isolated base for real delta restart replay");
        }
        auto read_packet = [](int64_t from, BYTE* data, DWORD capacity, DWORD& received) -> DWORD {
            const auto packet = CatchUpPacket(from);
            if (packet.size() > capacity) return ERROR_INSUFFICIENT_BUFFER;
            std::memcpy(data, packet.data(), packet.size());
            received = static_cast<DWORD>(packet.size());
            return ERROR_SUCCESS;
        };
        {
            Engine writer;
            if (!Load(writer, base_path)) return false;
            writer.StopPinyinWorker();
            writer.OpenDeltasLocked();
            auto& volume = writer.vols_[0];
            int64_t cursor = volume.next_usn;
            uint64_t applied = 0;
            auto commit = [&](const BYTE* data, size_t bytes, int64_t next) {
                return writer.ApplyUsnBatch(volume, data, bytes, next, nullptr, nullptr, applied);
            };
            const auto failed = CatchUpUsnBatch(106, cursor,
                [&](int64_t from, BYTE* data, DWORD capacity, DWORD& received) -> DWORD {
                    return from == 102 ? ERROR_READ_FAULT : read_packet(from, data, capacity, received);
                }, commit, [] { return true; });
            check(failed == ERROR_READ_FAULT && cursor == 102 && volume.next_usn == 102 && applied == 2,
                "read failure retains exactly the completed packet checkpoint");
            check(writer.IsTomb(writer.FindByFrnLocked(volume, 10)) &&
                writer.IsTomb(writer.FindByFrnLocked(volume, 11)) &&
                !writer.IsTomb(writer.FindByFrnLocked(volume, 12)), "failure applies completed deletes without skipping unread records");
            writer.FlushDeltas(); writer.CloseDeltas();
        }
        {
            Engine resumed;
            if (!Load(resumed, base_path)) return false;
            resumed.StopPinyinWorker(); resumed.ReplayDeltasLocked(); resumed.OpenDeltasLocked();
            auto& volume = resumed.vols_[0];
            check(!resumed.delta_replay_failed_ && volume.next_usn == 102 &&
                resumed.IsTomb(resumed.FindByFrnLocked(volume, 10)) &&
                !resumed.IsTomb(resumed.FindByFrnLocked(volume, 12)), "restart replays patches and their matching checkpoint");
            int64_t cursor = volume.next_usn;
            uint64_t applied = 0;
            auto commit = [&](const BYTE* data, size_t bytes, int64_t next) {
                return resumed.ApplyUsnBatch(volume, data, bytes, next, nullptr, nullptr, applied);
            };
            const auto complete = CatchUpUsnBatch(106, cursor, read_packet, commit, [] { return true; });
            check(complete == ERROR_SUCCESS && cursor == 106 && applied == 4 && resumed.deleted_ == 6,
                "resume from durable checkpoint applies every remaining delete exactly once");
            resumed.FlushDeltas(); resumed.CloseDeltas();
        }
        {
            Engine restarted;
            if (!Load(restarted, base_path)) return false;
            restarted.StopPinyinWorker(); restarted.ReplayDeltasLocked();
            check(!restarted.delta_replay_failed_ && restarted.vols_[0].next_usn == 106 && restarted.deleted_ == 6,
                "second restart preserves complete deletes and final checkpoint");
        }
        int64_t cursor = 100;
        size_t reads = 0, max_buffer = 0, commits = 0;
        auto counted = [&](int64_t from, BYTE* data, DWORD capacity, DWORD& received) {
            ++reads; max_buffer = (std::max)(max_buffer, static_cast<size_t>(capacity));
            return read_packet(from, data, capacity, received);
        };
        auto commit = [&](const BYTE*, size_t, int64_t) -> DWORD { ++commits; return ERROR_SUCCESS; };
        check(CatchUpUsnBatch(1000000, cursor, counted, commit, [] { return true; }) == ERROR_SUCCESS &&
            reads == kUsnCatchUpPackets && cursor == 108 && max_buffer == 256 * 1024,
            "continuously appended source yields after four packets with one 256 KiB buffer");
        reads = commits = 0; cursor = 100;
        check(CatchUpUsnBatch(102, cursor, counted, commit, [] { return true; }) == ERROR_SUCCESS &&
            reads == 1 && cursor == 102, "captured finite target stops without chasing appended tail");
        reads = commits = 0; cursor = 100;
        const auto malformed = CatchUpUsnBatch(102, cursor,
            [&](int64_t from, BYTE* data, DWORD, DWORD& received) -> DWORD {
                const auto packet = CatchUpPacket(from, true);
                std::memcpy(data, packet.data(), packet.size()); received = static_cast<DWORD>(packet.size());
                return ERROR_SUCCESS;
            }, commit, [] { return true; });
        check(malformed == ERROR_INVALID_DATA && cursor == 100 && commits == 0,
            "invalid later record rejects entire packet before applying or checkpointing");
        check(CatchUpUsnBatch(102, cursor, counted, commit, [] { return false; }) == ERROR_OPERATION_ABORTED &&
            cursor == 100 && reads == 0 && commits == 0, "cancellation leaves the unread checkpoint intact");
        check(CatchUpUsnBatch(102, cursor, counted,
            [](const BYTE*, size_t, int64_t) -> DWORD { return ERROR_WRITE_FAULT; }, [] { return true; }) == ERROR_WRITE_FAULT &&
            cursor == 100, "failed packet application never advances replay cursor");
        {
            Engine streaming;
            streaming.running_ = true;
            Engine::VolState volume;
            volume.letter = root.wstring()[0]; volume.volume_id = L"startup-stream-target";
            volume.journal_id = 9; volume.next_usn = 100;
            streaming.vols_.push_back(std::move(volume));
            auto signal = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            auto stream = UsnStreamTestAccess::Make(signal);
            streaming.journal_catch_up_targets_[L"startup-stream-target"] = {9, 104};
            UsnStreamTestAccess::Push(*stream, CatchUpPacket(100));
            streaming.journal_streams_[L"startup-stream-target"] = std::move(stream);
            check(streaming.CatchUpVolume(streaming.vols_[0], nullptr) &&
                streaming.vols_[0].next_usn == 102 && !streaming.vols_[0].folder_size_current,
                "streamed first packet keeps folder-size coverage pending before startup target");
            UsnStreamTestAccess::Push(*streaming.journal_streams_[L"startup-stream-target"], CatchUpPacket(102));
            check(streaming.CatchUpVolume(streaming.vols_[0], nullptr) &&
                streaming.vols_[0].next_usn == 104 && streaming.vols_[0].folder_size_current &&
                streaming.journal_catch_up_targets_.empty(), "stream restores current coverage only after applying captured startup target");
            streaming.journal_streams_.clear();
            CloseHandle(signal);
        }
        SetActiveIndexDirectory(L"");
        std::error_code cleanup;
        std::filesystem::remove_all(root, cleanup);
        check(!cleanup && !std::filesystem::exists(root), "remove and enumerate isolated USN replay fixtures");
        return ok;
    }

    static bool FallbackMetadataFixture() {
        bool ok = true;
        auto check = [&](bool value, const char* label) {
            std::cout << (value ? "[PASS] " : "[FAIL] ") << label << '\n'; ok &= value;
        };
        struct Io {
            static HANDLE WINAPI Open(HANDLE, LPFILE_ID_DESCRIPTOR id, DWORD access, DWORD share,
                                       LPSECURITY_ATTRIBUTES, DWORD flags) {
                if (access != FILE_READ_ATTRIBUTES ||
                    share != (FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE) ||
                    !(flags & FILE_FLAG_BACKUP_SEMANTICS) || id->Type != FileIdType) {
                    SetLastError(ERROR_INVALID_PARAMETER); return INVALID_HANDLE_VALUE;
                }
                if (id->FileId.QuadPart == 13) { SetLastError(ERROR_ACCESS_DENIED); return INVALID_HANDLE_VALUE; }
                return reinterpret_cast<HANDLE>(static_cast<uintptr_t>(id->FileId.QuadPart));
            }
            static BOOL WINAPI Query(HANDLE file, LPBY_HANDLE_FILE_INFORMATION info) {
                const auto id = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(file));
                if (id == 14) { SetLastError(ERROR_READ_FAULT); return FALSE; }
                *info = {};
                info->nFileIndexLow = static_cast<DWORD>(id == 15 ? 16 : id);
                info->dwFileAttributes = id == 6 ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
                info->nFileSizeLow = id == 10 ? 8192 : 0;
                constexpr uint64_t time = 134062560000000000ull;
                info->ftLastWriteTime = {static_cast<DWORD>(time), static_cast<DWORD>(time >> 32)};
                return TRUE;
            }
            static BOOL WINAPI Close(HANDLE) { SetLastError(ERROR_SUCCESS); return TRUE; }
        };
        const MftMetadataIo io{Io::Open, Io::Query, Io::Close};
        Engine engine;
        engine.running_ = true;
        Engine::VolState volume;
        volume.letter = L'Q'; volume.journal_id = 9; volume.folder_size_current = true;
        std::vector<Engine::FrnNode> nodes;
        for (uint64_t id : {6ull, 10ull, 11ull}) {
            Engine::FrnNode node;
            node.frn = id; node.parent = id == 6 ? 5 : 6; node.is_dir = id == 6;
            node.name = id == 6 ? L"folder" : id == 10 ? L"large.txt" : L"empty.txt";
            check(ReadMftFallbackMetadata(nullptr, id, node.is_dir, node.size, node.mtime, io),
                "fallback identity lookup supplies size and stable modification time");
            nodes.push_back(std::move(node));
        }
        check(engine.BuildMftTree(std::move(volume), 5, std::move(nodes)), "fallback attributes enter the actual MFT tree builder");
        engine.live_ = std::move(engine.build_); engine.vols_ = std::move(engine.build_vols_);
        engine.RebuildChildMapLocked(); engine.ready_ = true;
        Query query; query.path_prefix = L"Q:\\folder"; query.needle = L"size:>0";
        auto result = engine.Search(query);
        check(result.total == 1 && result.hits[0].name == L"large.txt" && result.hits[0].size == 8192 &&
            result.hits[0].mtime == 134062560000000000ull, "fallback positive-size filter and displayed attributes use resolved values");
        query.needle = L"file: size:0"; result = engine.Search(query);
        check(result.total == 1 && result.hits[0].name == L"empty.txt", "true zero-byte file remains distinct from failed metadata reads");
        query.needle = L"file: dm:2025..2027"; result = engine.Search(query);
        check(result.total == 2, "fallback stable files satisfy modification-date filters without later journal changes");
        const auto sizes = engine.FolderSizes({L"Q:\\folder"});
        check(sizes.size() == 1 && sizes[0].available && sizes[0].bytes == 8192,
            "fallback metadata contributes complete folder byte totals");
        for (uint64_t id : {13ull, 14ull, 15ull}) {
            uint64_t size = 99, mtime = 88;
            check(!ReadMftFallbackMetadata(nullptr, id, false, size, mtime, io) &&
                GetLastError() == static_cast<DWORD>(id == 13 ? ERROR_ACCESS_DENIED : id == 14 ? ERROR_READ_FAULT : ERROR_FILE_INVALID) &&
                size == 99 && mtime == 88, "denied failed and mismatched lookups preserve unknown outputs and exact error");
        }
        const auto fixture = std::filesystem::absolute(std::filesystem::path(L"bench_data") /
            L"audit-fixes-20261030-remaining" /
            (L"fallback-metadata-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()) + L".bin"));
        { std::ofstream file(fixture, std::ios::binary); file << std::string(8192, 'x'); }
        HANDLE file = CreateFileW(fixture.c_str(), FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
        BY_HANDLE_FILE_INFORMATION info{};
        const bool queried = file != INVALID_HANDLE_VALUE && GetFileInformationByHandle(file, &info);
        const uint64_t frn = (static_cast<uint64_t>(info.nFileIndexHigh) << 32) | info.nFileIndexLow;
        uint64_t size = 0, mtime = 0;
        check(queried && ReadMftFallbackMetadata(file, frn, false, size, mtime) && size == 8192 &&
            mtime == ((static_cast<uint64_t>(info.ftLastWriteTime.dwHighDateTime) << 32) |
                info.ftLastWriteTime.dwLowDateTime), "real isolated file ID lookup verifies Win32 identity and complete attributes");
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
        std::error_code cleanup;
        std::filesystem::remove(fixture, cleanup);
        check(!cleanup && !std::filesystem::exists(fixture), "remove and enumerate isolated fallback metadata file");
        return ok;
    }
