// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

    static bool OutstandingBuild(Engine& e, std::vector<Engine::FrnNode> nodes) {
        e.running_ = true;
        Engine::VolState volume;
        volume.letter = L'C';
        volume.volume_id = L"outstanding-fixture";
        volume.kind = VolumeKind::Fixed;
        if (!e.BuildMftTree(std::move(volume), 5, std::move(nodes))) return false;
        e.live_ = std::move(e.build_);
        e.vols_ = std::move(e.build_vols_);
        e.RebuildChildMapLocked();
        e.ready_ = true;
        return true;
    }

    static Engine::FrnNode OutstandingNode(uint64_t frn, uint64_t parent,
                                         std::wstring name, bool directory) {
        Engine::FrnNode node;
        node.frn = frn; node.parent = parent;
        node.name = std::move(name); node.is_dir = directory;
        return node;
    }

    static bool OutstandingStructureFixture(bool offline_only = false) {
        bool ok = true;
        const auto check = [&](bool value, const char* label) {
            std::cout << (value ? "[PASS] " : "[FAIL] ") << label << '\n'; ok &= value;
        };
        const auto directory = std::filesystem::absolute(std::filesystem::path(L"bench_data") /
            (L"outstanding-structure-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64())));
        std::filesystem::create_directories(directory);
        const auto path = directory / L"snapshot.bin";
        const auto query_paths = [](Engine& e, std::wstring needle, std::wstring scope) {
            Query q; q.needle = std::move(needle); q.path_prefix = std::move(scope);
            q.rank = false; q.limit = 32;
            const auto result = e.Search(q);
            std::vector<std::wstring> paths;
            for (const auto& hit : result.hits) paths.push_back(hit.path);
            std::sort(paths.begin(), paths.end());
            if (paths.size() != result.total) paths.push_back(L"ERROR: total mismatch");
            return paths;
        };
        {
            Engine source;
            const bool saved = OutstandingBuild(source, {
                OutstandingNode(10, 5, L"A", true), OutstandingNode(11, 10, L"child", true),
                OutstandingNode(12, 11, L"deep.txt", false), OutstandingNode(20, 5, L"B", true)}) && Save(source, path.wstring());
            check(saved, "U03 save real DFS mapped fixture");
            Engine mapped;
            const bool loaded = saved && Load(mapped, path.wstring());
            check(loaded, "U03 load real DFS mapped fixture");
            if (loaded) {
                mapped.StopPinyinWorker();
                Usn(mapped, 11, 20, L"child", USN_REASON_RENAME_NEW_NAME);
                const int32_t child = mapped.FindByFrnLocked(mapped.vols_[0], 11);
                const int32_t deep = mapped.FindByFrnLocked(mapped.vols_[0], 12);
                const int32_t a = mapped.FindByFrnLocked(mapped.vols_[0], 10);
                const int32_t b = mapped.FindByFrnLocked(mapped.vols_[0], 20);
                const auto parent_oracle = [&](int32_t id, int32_t root) {
                    for (int32_t hop = 0; id >= 0 && id < mapped.LiveCount() && hop <= mapped.LiveCount(); ++hop) {
                        if (id == root) return true;
                        id = mapped.NodeAt(id).parent;
                    }
                    return false;
                };
                check(child >= 0 && deep >= 0 && parent_oracle(deep, b) && !parent_oracle(deep, a) &&
                          mapped.InSubtreeLocked(deep, b) == parent_oracle(deep, b) &&
                          mapped.InSubtreeLocked(deep, a) == parent_oracle(deep, a),
                      "U03 moved base descendant membership agrees with parent oracle");
                for (const auto* needle : {L"deep", L"d*.txt", L"path:child\\deep"}) {
                    check(query_paths(mapped, needle, L"C:\\A").empty() &&
                              query_paths(mapped, needle, L"C:\\B") == std::vector<std::wstring>{L"C:\\B\\child\\deep.txt"},
                          "U03 moved subtree ordinary wildcard and path searches use current scope");
                }
                check(query_paths(mapped, L"deep", L"C:\\B\\child") == std::vector<std::wstring>{L"C:\\B\\child\\deep.txt"},
                      "U03 descendant scope resolves moved mapped directory");
                Usn(mapped, 11, 20, L"renamed", USN_REASON_RENAME_NEW_NAME);
                check(query_paths(mapped, L"deep", L"C:\\B\\renamed") == std::vector<std::wstring>{L"C:\\B\\renamed\\deep.txt"},
                      "U03 same-parent rename preserves descendant scope");
                mapped.AddForTest(L"C:\\B\\renamed\\live.txt", L"live.txt", false, 1);
                check(query_paths(mapped, L"*.txt", L"C:\\B\\renamed") ==
                          std::vector<std::wstring>{L"C:\\B\\renamed\\deep.txt", L"C:\\B\\renamed\\live.txt"},
                      "U03 moved base and newly live descendants appear exactly once");
                mapped.UpdateVolumeVisibilityLocked({});
                check(query_paths(mapped, L"*.txt", L"C:\\B\\renamed").empty(),
                    "U02 offline volume is hidden without discarding its nodes");
                Engine offline;
                const bool flattened = mapped.FlattenLocked(offline.live_, offline.vols_);
                offline.RebuildChildMapLocked();
                const auto retained = offline.ResolvePathLocked(L"C:\\B\\renamed\\deep.txt");
                check(flattened && retained >= 0 && offline.ResolvePathLocked(L"C:\\B\\renamed\\live.txt") >= 0 &&
                    offline.vols_.size() == 1 && offline.FindByFrnLocked(offline.vols_[0], 12) == retained,
                    "U02/U03 offline preservation retains moved child with later-ID parent and FRN");
            }
        }
        if (offline_only) {
            std::error_code error;
            std::filesystem::remove_all(directory, error);
            check(!error && !std::filesystem::exists(directory), "U02 offline fixture is removed");
            return ok;
        }
        for (bool dense : {false, true}) {
            std::cout << "[CASE] U04 dense=" << dense << '\n';
            Engine source;
            std::vector<Engine::FrnNode> nodes{OutstandingNode(10, 5, L"alpha报告.txt", false)};
            if (dense) for (uint64_t i = 0; i < 65534; ++i)
                nodes.push_back(OutstandingNode(100 + i, 5, L"filler" + std::to_wstring(i), false));
            const bool saved = OutstandingBuild(source, std::move(nodes)) && Save(source, path.wstring());
            Engine mapped;
            const bool loaded = saved && Load(mapped, path.wstring());
            check(loaded, dense ? "U04 load dense 65536-node map" : "U04 load sparse mapped fixture");
            if (!loaded) continue;
            mapped.StopPinyinWorker();
            const int32_t id = mapped.FindByFrnLocked(mapped.vols_[0], 10);
            if (id < 0) { check(false, "U04 target FRN survives mapping"); continue; }
            auto& patch = mapped.patches_[id];
            patch.attr = mapped.AttrAt(id); patch.has_attr = true; patch.attr.size = 23;
            for (bool name_patch : {false, true}) {
                std::cout << "[CASE] U04 name_patch=" << name_patch << '\n';
                if (name_patch) {
                    const std::wstring name = L"alpha报告-renamed.txt";
                    patch.off = static_cast<uint32_t>(mapped.live_.pool.size());
                    patch.len = static_cast<uint16_t>(name.size()); patch.has_name = true;
                    mapped.live_.pool.insert(mapped.live_.pool.end(), name.begin(), name.end());
                    mapped.RebuildChildMapLocked();
                }
                mapped.InvalidateFilterLocked();
                for (const auto* needle : {L"nopinyin: alpha", L"alpha", L"报告", L"baogao"}) {
                    std::wcout << L"[QUERY] " << needle << L'\n';
                    Query q; q.needle = needle; q.rank = false; q.limit = 1;
                    const auto first = mapped.Search(q); q.offset = 1;
                    const auto second = mapped.Search(q);
                    check(first.total == 1 && first.hits.size() == 1 && second.total == 1 && second.hits.empty() &&
                              first.hits[0].size == 23 && first.hits[0].path ==
                                  (name_patch ? L"C:\\alpha报告-renamed.txt" : L"C:\\alpha报告.txt"),
                          "U04 attr/name patch pinyin/nopinyin total and pagination are unique");
                }
            }
        }
        for (unsigned depth : {48u, 64u, 256u, 300u}) for (bool child_first : {false, true}) {
            for (bool excluded : {false, true}) {
                std::cout << "[CASE] U05 depth=" << depth << " child_first=" << child_first << " excluded=" << excluded << '\n';
                Engine source;
                std::vector<Engine::FrnNode> nodes;
                std::wstring expected = L"C:", exclusion;
                const auto frn = [&](unsigned level) { return uint64_t{1000} + (child_first ? depth - level : level); };
                for (unsigned i = 1; i <= depth; ++i) {
                    expected += L"\\n" + std::to_wstring(i);
                    if (i == 47) exclusion = expected;
                    nodes.push_back(OutstandingNode(frn(i), i == 1 ? 5 : frn(i - 1), L"n" + std::to_wstring(i), true));
                }
                if (excluded) source.excluded_paths_ = {exclusion};
                const bool built = OutstandingBuild(source, std::move(nodes));
                const int32_t leaf = built ? source.FindByFrnLocked(source.vols_[0], frn(depth)) : -1;
                check(leaf >= 0 && source.BuildPathLocked(leaf) == expected && source.BuildQueryPathLocked(leaf) == expected,
                      "U05 parent-first/child-first FRN boundary chains retain complete paths");
                check(leaf >= 0 && !!(source.NodeAt(leaf).flags & Engine::kFlagHidden) == excluded &&
                          source.IsExcludedPath(expected) == excluded,
                      "U05 full deep ancestor path controls exclusion and hidden propagation");
                if (!excluded && built && Save(source, path.wstring())) {
                    Engine mapped;
                    const bool loaded = Load(mapped, path.wstring());
                    if (loaded) mapped.StopPinyinWorker();
                    const int32_t mapped_leaf = loaded ? mapped.FindByFrnLocked(mapped.vols_[0], frn(depth)) : -1;
                    check(mapped_leaf >= 0 && mapped.BuildPathLocked(mapped_leaf) == expected &&
                              mapped.BuildQueryPathLocked(mapped_leaf) == expected,
                          "U05 real mapped regular/query path builders retain all 48/64/256/300 components");
                } else if (!excluded) check(false, "U05 persist deep path fixture");
            }
        }
        {
            Engine cycle;
            check(OutstandingBuild(cycle, {OutstandingNode(10, 5, L"safe", true),
                      OutstandingNode(100, 101, L"cycle-a", true), OutstandingNode(101, 100, L"cycle-b", true)}) &&
                      cycle.FindByFrnLocked(cycle.vols_[0], 100) < 0 && cycle.FindByFrnLocked(cycle.vols_[0], 101) < 0,
                  "U05 unresolved FRN cycles are rejected instead of attached to root");
        }
        std::error_code error;
        std::filesystem::remove(path, error); check(!error, "outstanding snapshot cleanup");
        std::filesystem::remove(directory, error); check(!error, "outstanding isolated directory cleanup");
        return ok;
    }

    static bool OutstandingQueryFixture() {
        bool ok = true;
        const auto check = [&](bool value, const char* label) {
            std::cout << (value ? "[PASS] " : "[FAIL] ") << label << '\n'; ok &= value;
        };
        Engine e;
        e.AddForTest(L"C:\\fixture\\foo.txt", L"foo.txt", false, 1);
        e.AddForTest(L"C:\\fixture\\other.txt", L"other.txt", false, 1);
        e.AddForTest(L"C:\\fixture\\foo-dir", L"foo-dir", true);
        e.AddForTest(L"C:\\fixture\\other-dir", L"other-dir", true);
        const auto query = [&](const wchar_t* text, std::wstring scope, std::vector<std::wstring> expected) {
            std::wcout << L"[QUERY] " << text << L'\n';
            Query q; q.needle = text; q.path_prefix = std::move(scope); q.rank = false; q.limit = 100;
            const auto result = e.Search(q);
            std::vector<std::wstring> paths;
            for (const auto& hit : result.hits) paths.push_back(hit.path);
            std::sort(paths.begin(), paths.end()); std::sort(expected.begin(), expected.end());
            if (result.total != expected.size() || paths != expected) {
                std::wcout << L"[ACTUAL total=" << result.total << L"]\n";
                for (const auto& path : paths) std::wcout << path << L'\n';
                std::wcout << L"[EXPECTED]\n";
                for (const auto& path : expected) std::wcout << path << L'\n';
            }
            check(result.total == expected.size() && paths == expected, "U07/U08/R08 real Engine exact hit set");
        };
        // Engine scopes include their indexed directory itself, created by AddForTest.
        query(L"", L"C:\\fixture", {L"C:\\fixture", L"C:\\fixture\\foo.txt", L"C:\\fixture\\other.txt",
            L"C:\\fixture\\foo-dir", L"C:\\fixture\\other-dir"});
        query(L"!file:foo", L"C:\\fixture", {L"C:\\fixture", L"C:\\fixture\\other.txt", L"C:\\fixture\\foo-dir", L"C:\\fixture\\other-dir"});
        query(L"!folder:foo", L"C:\\fixture", {L"C:\\fixture", L"C:\\fixture\\foo.txt", L"C:\\fixture\\other.txt", L"C:\\fixture\\other-dir"});
        query(L"!!file:foo", L"C:\\fixture", {L"C:\\fixture\\foo.txt"});
        query(L"!!folder:foo", L"C:\\fixture", {L"C:\\fixture\\foo-dir"});
        query(L"!file:", L"C:\\fixture", {L"C:\\fixture", L"C:\\fixture\\foo-dir", L"C:\\fixture\\other-dir"});
        query(L"!folder:", L"C:\\fixture", {L"C:\\fixture\\foo.txt", L"C:\\fixture\\other.txt"});
        e.AddForTest(L"C:\\paths\\skip\\a.txt", L"a.txt", false, 1);
        e.AddForTest(L"C:\\paths\\skip2\\a.txt", L"a.txt", false, 1);
        e.AddForTest(L"C:\\paths\\foo\\bar\\a.txt", L"a.txt", false, 1);
        query(L"file: !path:C:\\paths\\skip", L"C:\\paths", {L"C:\\paths\\skip2\\a.txt", L"C:\\paths\\foo\\bar\\a.txt"});
        query(L"path:C:\\paths path:C:\\paths\\skip file:", L"", {L"C:\\paths\\skip\\a.txt"});
        query(L"path:foo/bar file:", L"C:\\paths", {L"C:\\paths\\foo\\bar\\a.txt"});
        for (const auto* raw : {L"size:18446744073709551616", L"!size:18446744073709551616",
                 L"size:18014398509481984kb", L"!size:18014398509481984kb", L"dm:2023-02-29",
                 L"!dm:2023-02-29", L"dm:1900-02-29", L"dm:2024-13", L"!dm:garbage",
                 L"dm:2024-03-01..2024-02-01"}) query(raw, L"C:\\fixture", {});
        return ok;
    }
