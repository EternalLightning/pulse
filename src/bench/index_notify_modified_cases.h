// Included inside EngineTestAccess to exercise the real notification parser.
static bool NotifyModifiedFixture() {
    bool ok = true;
    auto check = [&](bool value, const char* name) {
        std::cout << (value ? "[PASS] " : "[FAIL] ") << name << '\n';
        ok &= value;
    };
    const auto fixture = std::filesystem::absolute(std::filesystem::path(L"bench_data") /
        (L"notify-modified-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
         std::to_wstring(GetTickCount64())));
    std::filesystem::create_directory(fixture);
    auto write = [&](const std::filesystem::path& path, size_t length, WORD year,
                     WIN32_FILE_ATTRIBUTE_DATA& attributes) {
        {
            std::ofstream file(path, std::ios::binary | std::ios::trunc);
            file << std::string(length, 'x');
            file.close();
            if (!file) return false;
        }
        HANDLE file = CreateFileW(path.c_str(), FILE_WRITE_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (file == INVALID_HANDLE_VALUE) return false;
        SYSTEMTIME time{};
        time.wYear = year; time.wMonth = 6; time.wDay = 15; time.wHour = 12;
        FILETIME timestamp{};
        const bool written = SystemTimeToFileTime(&time, &timestamp) &&
            SetFileTime(file, nullptr, nullptr, &timestamp);
        CloseHandle(file);
        return written && GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attributes);
    };
    const wchar_t* queries[] = {L"file: size:>20", L"file: size:<20", L"file: dm:2030", L"file: dm:2020"};
    for (bool mapped : {false, true}) {
        for (size_t scenario = 0; scenario < std::size(queries); ++scenario) {
            const auto directory = fixture / (std::wstring(mapped ? L"mapped-" : L"live-") +
                std::to_wstring(scenario));
            std::filesystem::create_directory(directory);
            const auto path = directory / L"one.bin";
            WIN32_FILE_ATTRIBUTE_DATA initial{};
            const bool prepared = write(path, 10, 2020, initial);
            check(prepared, "prepare isolated notification file with deterministic size and date");
            if (!prepared) continue;
            Engine engine;
            const uint64_t mtime = (static_cast<uint64_t>(initial.ftLastWriteTime.dwHighDateTime) << 32) |
                initial.ftLastWriteTime.dwLowDateTime;
            engine.AddForTest(path.wstring(), L"one.bin", false, 10, mtime);
            if (mapped) {
                const auto cache = directory / L"snapshot.bin";
                const bool loaded = Save(engine, cache.wstring()) && Load(engine, cache.wstring());
                check(loaded, "map isolated snapshot to exercise base attribute patch");
                if (!loaded) continue;
                // Finish unrelated asynchronous cache invalidation before priming.
                engine.StopPinyinWorker();
            }
            Engine::VolState volume;
            volume.letter = path.wstring()[0];
            volume.root_idx = engine.ResolvePathLocked(path.root_path().wstring());
            volume.journal_id = 1;
            volume.folder_size_current = true;
            engine.vols_.push_back(std::move(volume));
            engine.ready_ = true;
            Query query;
            query.needle = queries[scenario]; query.path_prefix = directory.wstring();
            query.rank = false; query.limit = 10;
            const bool enters = scenario % 2 == 0;
            const auto before = engine.Search(query);
            check(before.total == (enters ? 0u : 1u), "prime size or date membership cache");
            const auto initial_size = engine.FolderSizes({directory.wstring()});
            check(initial_size.size() == 1 && initial_size[0].available && initial_size[0].bytes == 10,
                "prime folder aggregate before notification");
            WIN32_FILE_ATTRIBUTE_DATA changed{};
            const bool changed_file = write(path, 30, 2030, changed);
            constexpr wchar_t filename[] = L"one.bin";
            constexpr DWORD name_bytes = (std::size(filename) - 1) * sizeof(wchar_t);
            std::vector<BYTE> packet(sizeof(FILE_NOTIFY_INFORMATION) + name_bytes, 0);
            auto* notification = reinterpret_cast<FILE_NOTIFY_INFORMATION*>(packet.data());
            notification->Action = FILE_ACTION_MODIFIED;
            notification->FileNameLength = name_bytes;
            memcpy(notification->FileName, filename, name_bytes);
            const auto applied = changed_file ? engine.ApplyNotifyLocked(directory.wstring(), packet.data(),
                static_cast<DWORD>(packet.size())) : 0;
            const int32_t id = engine.ResolvePathLocked(path.wstring());
            check(applied == 1 && id >= 0 && engine.AttrAt(id).size == 30,
                "real modified parser refreshes file attributes");
            const auto after = engine.Search(query);
            check(after.total == (enters ? 1u : 0u) && after.hits.size() == after.total &&
                (!enters || (after.hits[0].path == path.wstring() && after.hits[0].size == 30)),
                "modified file enters or leaves cached size and date results");
            const auto updated_size = engine.FolderSizes({directory.wstring()});
            check(updated_size.size() == 1 && updated_size[0].available && updated_size[0].bytes == 30,
                "modified notification rebuilds folder aggregate from refreshed metadata");
        }
    }
    // Only this function's known files and empty directories are removed.
    std::error_code error;
    bool cleaned = true;
    auto remove = [&](const std::filesystem::path& path) {
        std::filesystem::remove(path, error);
        cleaned &= !error;
    };
    for (bool mapped : {false, true}) {
        for (size_t scenario = 0; scenario < std::size(queries); ++scenario) {
            const auto directory = fixture / (std::wstring(mapped ? L"mapped-" : L"live-") +
                std::to_wstring(scenario));
            remove(directory / L"one.bin");
            if (mapped) remove(directory / L"snapshot.bin");
            remove(directory);
        }
    }
    remove(fixture);
    check(cleaned, "isolated modified notification fixture cleanup");
    return ok;
}
