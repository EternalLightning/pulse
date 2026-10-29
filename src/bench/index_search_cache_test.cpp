#include "../index/index_engine.h"
#include "../index/index_caller_visibility.h"
#include "../index/index_name_pool.h"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <tuple>

namespace pulse::index {
struct EngineTestAccess {
    static inline size_t checks = 0, failures = 0;
    static void Check(bool ok, const char* label) {
        ++checks;
        if (!ok) ++failures;
        std::cout << (ok ? "[PASS] " : "[FAIL] ") << label << '\n';
    }
    static Query QueryFor(std::wstring text = L"item") {
        Query q; q.needle = std::move(text); q.rank = false; q.limit = 100;
        return q;
    }
    static void Build(Engine& e) {
        for (const auto* path : {L"C:\\A\\item-alpha.txt", L"C:\\A\\item-beta.log",
             L"C:\\B\\item-gamma.txt", L"C:\\B\\item-dir\\item-child.txt"})
            e.AddForTest(path, L"", false);
        e.AddForTest(L"C:\\A\\item-alpha.txt", L"", false, 10);
        e.AddForTest(L"C:\\A\\item-beta.log", L"", false, 20);
        e.AddForTest(L"C:\\B\\item-gamma.txt", L"", false, 30);
        e.AddForTest(L"C:\\B\\item-dir\\item-child.txt", L"", false, 40);
    }
    static bool Cached(const Engine& e, const Query& q, size_t total) {
        return e.cache_epoch_ == e.filter_epoch_ && e.cache_raw_ == q.needle &&
            e.cache_path_prefix_ == q.path_prefix && e.cache_folders_only_ == q.folders_only &&
            e.cache_set_.universe == e.LiveCount() && e.cache_set_.total == total;
    }
    static bool Same(const SearchResult& a, const SearchResult& b) {
        if (a.total != b.total || a.hits.size() != b.hits.size() || a.incomplete != b.incomplete) return false;
        for (size_t i = 0; i < a.hits.size(); ++i)
            if (a.hits[i].path != b.hits[i].path || a.hits[i].size != b.hits[i].size) return false;
        return true;
    }
    static void RawCacheAndCallers() {
        Engine e; Build(e); auto q = QueryFor();
        size_t calls = 0;
        const auto caller_a = [&](const std::wstring& path) { ++calls; return path.starts_with(L"C:\\A\\"); };
        const auto caller_b = [&](const std::wstring& path) { ++calls; return path.starts_with(L"C:\\B\\"); };
        const auto a = e.Search(q, nullptr, 0, caller_a);
        Check(a.total == 2 && calls == 5 && Cached(e, q, 5), "authorized query caches all five raw candidates, including denied rows");
        calls = 0; const auto b = e.Search(q, nullptr, 0, caller_b);
        Check(b.total == 3 && calls == 5, "second caller reauthorizes every cached candidate and sees its own rows");
        calls = 0; const auto again = e.Search(q, nullptr, 0, caller_a);
        Check(Same(a, again) && calls == 5, "same caller response reauthorizes instead of reusing previous allow decisions");
        calls = 0;
        const auto denied = e.Search(q, nullptr, 0, [&](const auto&) { ++calls; return false; });
        Check(denied.total == 0 && calls == 5 && !denied.incomplete && Cached(e, q, 5), "ordinary denial continues through all candidates and never truncates the raw cache");
        Check(e.Search(q).total == 5, "unfiltered query after a denied response still sees every raw candidate");
        q.limit = 0;
        calls = 0; const auto count = e.Search(q, nullptr, 0, caller_b);
        Check(count.total == 3 && count.hits.empty() && calls == 5 && Cached(e, q, 5), "count-only query reauthorizes and retains the complete raw set");
        q.limit = 1; q.offset = 1; q.rank = true;
        calls = 0; const auto page = e.Search(q, nullptr, 0, caller_a);
        Check(page.total == 2 && page.hits.size() == 1 && calls == 5, "ranked page uses the same raw cache with fresh authorization");
        q.rank = false; q.sort = ResultSort::Size; q.sort_desc = true;
        const auto sorted = e.Search(q, nullptr, 0, caller_a);
        Check(sorted.total == 2 && sorted.hits.size() == 1 && sorted.hits[0].size == 10, "sort and paging operate on the authorized rows without changing the raw key");
        e.Search(q, nullptr, 0, caller_b);
        q = QueryFor(L"item-a");
        const auto narrow = e.Search(q, nullptr, 0, caller_a);
        Check(narrow.total == 1 && Cached(e, q, 1), "query narrowing filters raw candidates rather than the last caller's visible subset");
    }
    static void IndexedNamesWithoutFileAccess() {
        const auto directory = std::filesystem::absolute(std::filesystem::path(L"bench_data") /
            (L"filename_metadata_" + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(GetTickCount64())));
        std::filesystem::create_directories(directory);
        const auto path = (directory / L"item-private.txt").wstring();
        const auto identity = transport::Identity::Current();
        const auto set_acl = [&](const std::wstring& text) {
            PSECURITY_DESCRIPTOR security = nullptr;
            if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(text.c_str(), SDDL_REVISION_1, &security, nullptr)) return false;
            PACL acl = nullptr; BOOL present = FALSE, defaulted = FALSE;
            const auto error = GetSecurityDescriptorDacl(security, &present, &acl, &defaulted) && present
                ? SetNamedSecurityInfoW(const_cast<wchar_t*>(path.c_str()), SE_FILE_OBJECT,
                    DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr, nullptr, acl, nullptr)
                : ERROR_INVALID_SECURITY_DESCR;
            LocalFree(security);
            return error == ERROR_SUCCESS;
        };
        {
            transport::Handle file(CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
            Check(file.get() != INVALID_HANDLE_VALUE, "create isolated file for index-only filename discovery");
        }
        const auto allow = L"D:P(A;;FA;;;" + identity.user + L")";
        const bool denied = identity.valid && set_acl(L"D:P(D;;0x81;;;" + identity.user + L")(A;;FA;;;" + identity.user + L")");
        Check(denied, "deny actual fixture data and attribute access for the current user");
        if (denied) {
            const auto access_denied = [&](DWORD access) {
                transport::Handle file(CreateFileW(path.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    nullptr, OPEN_EXISTING, 0, nullptr));
                return file.get() == INVALID_HANDLE_VALUE && GetLastError() == ERROR_ACCESS_DENIED;
            };
            Check(access_denied(FILE_READ_DATA), "Windows rejects reading protected fixture contents");
            std::cout << "[INFO] fixture attribute-open denied=" << access_denied(FILE_READ_ATTRIBUTES) << '\n';
            HANDLE primary = nullptr, duplicate = nullptr;
            const bool captured = OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY | TOKEN_DUPLICATE, &primary) &&
                DuplicateTokenEx(primary, TOKEN_QUERY | TOKEN_IMPERSONATE, nullptr, SecurityImpersonation, TokenImpersonation, &duplicate);
            transport::Handle primary_owner(primary), caller(duplicate);
            CallerVisibility old_policy(caller.get());
            Check(captured && !old_policy.Visible(path), "previous metadata authorization would hide the protected filename");
            Engine e;
            e.AddForTest(path, L"", false, 30);
            e.AddForTest((directory / L"item-public.txt").wstring(), L"", false, 10);
            e.AddForTest((directory / L"item-missing.txt").wstring(), L"", false, 20);
            auto q = QueryFor(); q.limit = 2;
            SearchDiagnostics diagnostics;
            const auto all = e.Search(q, nullptr, 0, {}, {}, &diagnostics);
            Check(all.total == 3 && all.hits.size() == 2 && !all.incomplete && !all.error,
                "index-only search counts inaccessible and missing indexed names without partial results");
            Check(diagnostics.raw_candidates == 3 && diagnostics.examined == 0 && diagnostics.authorize_us == 0 && diagnostics.path_us == 0,
                "index-only filename search performs no per-candidate authorization or path construction");
            q.limit = 0;
            const auto count = e.Search(q);
            Check(count.total == 3 && count.hits.empty() && !count.incomplete,
                "index-only count is complete and independent of current file permissions");
            q.limit = 1; q.sort = ResultSort::Size; q.sort_desc = true;
            const auto first = e.Search(q);
            Check(first.total == 3 && first.hits.size() == 1 && first.hits[0].path == path && first.hits[0].size == 30,
                "index-only page can return denied filename and cached size in global sort order");
            q.offset = 1;
            const auto second = e.Search(q);
            Check(second.total == 3 && second.hits.size() == 1 && second.hits[0].size == 20,
                "index-only paging preserves full counts across inaccessible and missing files");
            q.offset = 0; q.limit = 3; q.rank = true;
            const auto ranked = e.Search(q);
            Check(ranked.total == 3 && ranked.hits.size() == 3 && !ranked.incomplete,
                "ranked filename discovery includes every indexed candidate without an ACL budget");
            std::atomic<uint32_t> latest{2};
            const auto cancelled = e.Search(q, &latest, 1);
            Check(cancelled.hits.empty() && cancelled.total == 0,
                "index-only filename search still discards superseded requests");
            Check(access_denied(FILE_READ_DATA), "discovering a filename does not grant content access");
        }
        Check(set_acl(allow), "restore isolated filename fixture ACL");
        std::error_code error;
        std::filesystem::remove_all(directory, error);
        Check(!error, "isolated filename metadata fixtures clean up");
    }
    static void ScopeAndEpoch() {
        Engine e; Build(e); auto q = QueryFor();
        const auto allow = [](const auto&) { return true; };
        q.path_prefix = L"C:\\A";
        Check(e.Search(q, nullptr, 0, allow).total == 2 && Cached(e, q, 2), "path A is a distinct raw candidate scope");
        q.path_prefix = L"C:\\B";
        Check(e.Search(q, nullptr, 0, allow).total == 3 && Cached(e, q, 3), "changing to path B cannot reuse A's candidate set");
        q.path_prefix.clear(); q.folders_only = true;
        Check(e.Search(q, nullptr, 0, allow).total == 1 && Cached(e, q, 1), "folders-only scope cannot reuse file candidates");
        q.folders_only = false;
        Check(e.Search(q, nullptr, 0, allow).total == 5 && Cached(e, q, 5), "leaving folder scope restores all matching entries");
        q = QueryFor(L"item size:>15");
        Check(e.Search(q, nullptr, 0, allow).total == 3, "attribute query primes the raw cache");
        const auto count = e.LiveCount(); const auto epoch = e.cache_epoch_;
        e.AddForTest(L"C:\\A\\item-alpha.txt", L"", false, 50);
        Check(e.LiveCount() == count && e.filter_epoch_ != epoch &&
            e.Search(q, nullptr, 0, allow).total == 4 && Cached(e, q, 4), "same-node-count attribute update invalidates candidates by index epoch");
        q = QueryFor(L"item-new");
        Check(e.Search(q, nullptr, 0, allow).total == 0 && Cached(e, q, 0), "empty raw candidates are cached");
        e.AddForTest(L"C:\\B\\item-new.txt", L"", false, 1);
        Check(e.Search(q, nullptr, 0, allow).total == 1 && Cached(e, q, 1), "added entry invalidates an empty cached result");
    }
    static void Stopping() {
        Engine e; Build(e); const auto q = QueryFor();
        std::atomic<uint32_t> latest{8};
        size_t calls = 0, probes = 0;
        const auto cancelled_before = e.Search(q, &latest, 7, [&](const auto&) { ++calls; return true; });
        Check(cancelled_before.total == 0 && calls == 0 && e.cache_epoch_ == 0, "already cancelled query does not collect or authorize candidates");
        latest = 7;
        const auto cancelled = e.Search(q, &latest, 7, [&](const auto&) { ++calls; latest = 8; return true; },
            [&] { ++probes; return false; });
        Check(cancelled.total == 0 && cancelled.hits.empty() && calls == 1 && probes == 1 && Cached(e, q, 5), "cancellation during authorization stops before the next path and discards partial output");
        calls = probes = 0;
        const auto stopped = e.Search(q, nullptr, 0, [&](const auto&) { ++calls; return true; },
            [&] { ++probes; return calls == 2; });
        Check(stopped.total == 2 && stopped.hits.size() == 2 && stopped.incomplete && calls == 2 && probes == 3,
            "explicit budget stop exits traversal before authorizing a third candidate");
        Check(Cached(e, q, 5) && e.Search(q).total == 5, "budget-truncated authorization never truncates the cached raw set");
        calls = probes = 0;
        const auto immediate = e.Search(q, nullptr, 0, [&](const auto&) { ++calls; return true; },
            [&] { ++probes; return true; });
        Check(immediate.total == 0 && immediate.incomplete && calls == 0 && probes == 1,
            "budget already exhausted stops before building the first authorization path");
        CallerVisibility expired(nullptr, 0);
        calls = 0;
        const auto expired_result = e.Search(q, nullptr, 0, [&](const auto& path) { ++calls; return expired.Visible(path); },
            [&] { return expired.StopRequested(); });
        Check(expired.Incomplete() && expired_result.incomplete && calls == 0, "real visibility deadline signals stop separately from a permission denial");
        CallerVisibility denied(nullptr, 60000);
        Check(!denied.Visible(L"C:\\not-authorized") && !denied.StopRequested() && !denied.Incomplete(),
            "ordinary invalid-token denial does not request traversal termination");
    }
    static void DenseAndSparseTraversal() {
        for (int32_t universe : {1024, 70000}) {
            Engine::MatchSet set; set.Begin(universe);
            for (int32_t id : {1, 63, 64, 129}) set.Add(id);
            std::vector<int32_t> visited;
            set.ForEachWhile([&](int32_t id) { visited.push_back(id); return visited.size() < 3; });
            Check(visited == std::vector<int32_t>({1, 63, 64}), universe < 65536 ?
                "sparse traversal truly breaks on stop" : "dense traversal breaks across bitset words on stop");
        }
        Engine e;
        const auto root = e.AddNodeLocked(e.live_, -1, L"C:", Engine::kFlagDir, 0, 0, 0, true);
        for (int32_t i = 0; i < 70000; ++i)
            e.AddNodeLocked(e.live_, root, L"item-" + std::to_wstring(i), 0, 0, 0, 0, true);
        e.InvalidateFilterLocked();
        auto q = QueryFor(); q.limit = 0;
        size_t calls = 0, probes = 0;
        const auto begin = std::chrono::steady_clock::now();
        const auto first = e.Search(q, nullptr, 0, [&](const auto&) { ++calls; return true; },
            [&] { ++probes; return calls == 3; });
        const auto cold_us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - begin).count();
        Check(first.total == 3 && first.incomplete && calls == 3 && probes == 4 && e.cache_set_.dense && Cached(e, q, 70000),
            "dense 70000-candidate response stops after three checks and retains all raw candidates");
        calls = probes = 0;
        const auto warm_begin = std::chrono::steady_clock::now();
        const auto repeat = e.Search(q, nullptr, 0, [&](const auto&) { ++calls; return false; },
            [&] { ++probes; return calls == 3; });
        const auto warm_us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - warm_begin).count();
        Check(repeat.total == 0 && repeat.incomplete && calls == 3 && probes == 4 && Cached(e, q, 70000),
            "cached dense candidates are reauthorized and stop even when every visited row is denied");
        std::cout << "[METRIC] synthetic_dense_cold_us=" << cold_us << " cached_us=" << warm_us << '\n';
        calls = 0;
        const auto all_denied_began = std::chrono::steady_clock::now();
        const auto complete = e.Search(q, nullptr, 0, [&](const auto&) { ++calls; return false; });
        const auto all_denied_us = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - all_denied_began).count();
        Check(complete.total == 0 && !complete.incomplete && calls == 70000,
            "70000 ordinary denials complete rather than masquerading as a stop signal");
        std::cout << "[METRIC] synthetic_dense_all_denied_us=" << all_denied_us << " candidates=" << calls << '\n';
    }
    static void AuthorizationLockScope() {
        Engine e; Build(e); const auto q = QueryFor();
        const auto completed = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        Check(completed != nullptr, "create isolated authorization concurrency signal");
        if (!completed) return;
        std::thread concurrent;
        bool query_completed = false;
        SearchResult nested;
        size_t calls = 0;
        SearchDiagnostics diagnostics;
        const auto result = e.Search(q, nullptr, 0, [&](const auto&) {
            if (++calls == 1) {
                concurrent = std::thread([&] { nested = e.Search(q); SetEvent(completed); });
                query_completed = WaitForSingleObject(completed, 2000) == WAIT_OBJECT_0;
            }
            return true;
        }, {}, &diagnostics);
        if (concurrent.joinable()) concurrent.join();
        Check(query_completed && nested.total == 5, "another query completes while authorization is in progress");
        Check(result.total == 5 && !result.incomplete && diagnostics.raw_candidates == 5 &&
            diagnostics.examined == 5 && diagnostics.authorized == 5 &&
            !diagnostics.cache_hit && diagnostics.authorize_us > 0 && diagnostics.total_us >= diagnostics.authorize_us,
            "response diagnostics separate full candidate authorization from cache and total time");
        std::cout << "[METRIC] concurrent_authorization_us=" << diagnostics.authorize_us
            << " index_wait_us=" << diagnostics.index_wait_us << " total_us=" << diagnostics.total_us << '\n';
        const auto cached = e.Search(q, nullptr, 0, [](const auto& path) { return path.starts_with(L"C:\\A\\"); }, {}, &diagnostics);
        Check(cached.total == 2 && diagnostics.cache_hit && diagnostics.raw_candidates == 5 &&
            diagnostics.examined == 5 && diagnostics.authorized == 2,
            "cached response diagnostics still count fresh authorization for every raw candidate");

        ResetEvent(completed); calls = 0;
        const auto writer_progress = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        Check(writer_progress != nullptr, "create isolated background metadata writer signal");
        if (!writer_progress) { CloseHandle(completed); return; }
        std::atomic<bool> stop_writer{false};
        size_t updates = 0;
        bool writer_attempted = false, writer_queued = false;
        const auto stable = e.Search(q, nullptr, 0, [&](const auto&) {
            if (++calls == 1) {
                concurrent = std::thread([&] {
                    SetEvent(completed);
                    do {
                        e.AddForTest(L"C:\\outside\\unrelated.bin", L"", false, ++updates);
                        SetEvent(writer_progress);
                    } while (!stop_writer.load());
                });
                writer_attempted = WaitForSingleObject(completed, 2000) == WAIT_OBJECT_0;
                writer_queued = WaitForSingleObject(writer_progress, 50) == WAIT_TIMEOUT;
            }
            return true;
        }, {}, &diagnostics);
        stop_writer = true;
        if (concurrent.joinable()) concurrent.join();
        CloseHandle(writer_progress);
        CloseHandle(completed);
        Check(writer_attempted && writer_queued && updates > 0,
            "continuous unrelated metadata writer queues behind the response snapshot and completes after it");
        Check(stable.total == 5 && stable.hits.size() == 5 && !stable.incomplete && calls == 5,
            "unrelated writer pressure cannot clear an otherwise valid authorized response");
        const auto fresh = e.Search(q);
        Check(fresh.total == 5 && !fresh.incomplete && fresh.hits[0].size == 10 && e.Search(QueryFor(L"unrelated")).total == 1,
            "later queries retain matching rows and observe the unrelated writer's new entry");
    }
    static void InternedSnapshotNames() {
        Engine source;
        const auto root = source.AddNodeLocked(source.live_, -1, L"C:", Engine::kFlagDir, 0, 0, 0, true);
        const std::vector<std::wstring> names{L"independent.txt", L"same.txt", L"Same.txt", L"中文", L"e\u0301", L"\u00e9",
            std::wstring{L'n', L'\0', L'x'}, std::wstring{static_cast<wchar_t>(0xd800), L'x'},
            std::wstring{static_cast<wchar_t>(0xdc00), L'x'}, L"扩容-" + std::wstring(1024, L'x')};
        for (const auto* parent : {L"parent-a", L"parent-b"}) {
            const auto directory = source.AddNodeLocked(source.live_, root, parent, Engine::kFlagDir, 0, 0, 0, true);
            for (size_t i = 0; i < names.size(); ++i)
                source.AddNodeLocked(source.live_, directory, names[i], 0, 0, i + 10, 0, true);
        }
        Engine::Store snapshot;
        std::vector<Engine::VolState> volumes;
        Check(source.FlattenLocked(snapshot, volumes), "flatten repeated basenames into a snapshot with stable source views");
        size_t expected_chars = 2 + std::wstring_view(L"parent-a").size() + std::wstring_view(L"parent-b").size();
        for (const auto& name : names) expected_chars += name.size();
        bool shared = true;
        for (const auto& name : names) {
            std::vector<uint32_t> offsets;
            for (const auto& node : snapshot.nodes)
                if (std::wstring_view(snapshot.pool.data() + node.off, node.len) == name) offsets.push_back(node.off);
            shared = shared && offsets.size() == 2 && offsets[0] == offsets[1];
        }
        Check(shared && snapshot.pool.size() == expected_chars && snapshot.pool.size() < source.live_.pool.size(),
            "only byte-identical basenames share offsets across parents; case and Unicode code units stay distinct");
        const auto directory = std::filesystem::absolute(std::filesystem::path(L"bench_data") /
            (L"interned_names_" + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(GetTickCount64())));
        std::error_code error;
        if (!std::filesystem::create_directories(directory, error)) {
            Check(false, "create isolated interned-name roundtrip directory");
            return;
        }
        {
            Engine rebuilt, loaded_build;
            rebuilt.build_ = source.live_;
            const auto rebuild_path = (directory / L"full-rebuild.bin").wstring();
            const bool rebuilt_mapped = rebuilt.WriteBuildIndexFile(rebuild_path, 456) &&
                loaded_build.MapIndexFile(rebuild_path + L".tmp", loaded_build.map_);
            bool exact = rebuilt_mapped && loaded_build.LiveCount() == source.LiveCount();
            for (int32_t id = 0; exact && id < source.LiveCount(); ++id) {
                Node before = source.NodeAt(id), after = loaded_build.NodeAt(id);
                before.off = after.off = 0;
                exact = std::memcmp(&before, &after, sizeof(Node)) == 0 && source.NameOf(id) == loaded_build.NameOf(id) &&
                    source.AttrAt(id).size == loaded_build.AttrAt(id).size && source.AttrAt(id).mtime == loaded_build.AttrAt(id).mtime;
            }
            Check(exact && loaded_build.map_->hdr->pool_chars == expected_chars,
                "FullRebuild write boundary interns names and preserves every node ID, order, metadata and UTF-16 unit");
            const auto before_pool = rebuilt.build_.pool;
            const auto before_nodes = rebuilt.build_.nodes;
            Check(InternSnapshotNamePool(rebuilt.build_.pool, rebuilt.build_.nodes) == ERROR_SUCCESS &&
                rebuilt.build_.pool == before_pool && rebuilt.build_.nodes.size() == before_nodes.size() &&
                std::memcmp(rebuilt.build_.nodes.data(), before_nodes.data(), before_nodes.size() * sizeof(Node)) == 0,
                "repeated full-build name preparation is byte-idempotent");
            Check(CompactOverlayNamePool(rebuilt.build_.pool, rebuilt.build_.nodes, rebuilt.patches_) == ERROR_SUCCESS &&
                rebuilt.build_.pool.size() == expected_chars,
                "shared full-build names remain valid if a failed publication falls back to heap maintenance");
            rebuilt.build_.nodes.back().off = static_cast<uint32_t>(rebuilt.build_.pool.size() + 1);
            const auto invalid_pool = rebuilt.build_.pool;
            const auto invalid_nodes = rebuilt.build_.nodes;
            const auto rejected_path = (directory / L"invalid-rebuild.bin").wstring();
            Check(!rebuilt.WriteBuildIndexFile(rejected_path, 789) && GetLastError() == ERROR_INVALID_DATA &&
                rebuilt.build_.pool == invalid_pool &&
                std::memcmp(rebuilt.build_.nodes.data(), invalid_nodes.data(), invalid_nodes.size() * sizeof(Node)) == 0 &&
                !std::filesystem::exists(rejected_path + L".tmp"),
                "invalid build name references fail before changing offsets or opening a snapshot file");
        }
        {
            Engine old, compressed;
            const auto old_path = (directory / L"old-unshared.bin").wstring();
            const auto new_path = (directory / L"new-shared.bin").wstring();
            const bool mapped = source.WriteIndexFile(old_path, source.live_, volumes, 123) &&
                source.WriteIndexFile(new_path, snapshot, volumes, 123) &&
                old.MapIndexFile(old_path + L".tmp", old.map_) &&
                compressed.MapIndexFile(new_path + L".tmp", compressed.map_);
            Check(mapped, "existing mapper accepts unshared and shared-offset snapshots without a new disk format");
            if (mapped) {
                auto rows = [](const Engine& engine) {
                    std::vector<std::tuple<std::wstring, std::wstring, uint64_t>> result;
                    for (int32_t id = 0; id < engine.LiveCount(); ++id)
                        result.emplace_back(engine.BuildQueryPathLocked(id), std::wstring(engine.NameOf(id)), engine.AttrAt(id).size);
                    std::sort(result.begin(), result.end());
                    return result;
                };
                Check(rows(old) == rows(compressed),
                    "roundtrip preserves every parent, metadata, embedded NUL and unpaired-surrogate name exactly");
                compressed.RebuildChildMapLocked();
                compressed.AddForTest(L"C:\\parent-a\\independent.txt", L"", false, 999);
                const auto a = compressed.ResolvePathLocked(L"C:\\parent-a\\independent.txt");
                const auto b = compressed.ResolvePathLocked(L"C:\\parent-b\\independent.txt");
                Check(a >= 0 && b >= 0 && compressed.AttrAt(a).size == 999 && compressed.AttrAt(b).size == 10 &&
                    compressed.NameOf(a) == L"independent.txt" && compressed.NameOf(b) == L"independent.txt",
                    "updating one shared basename keeps the other parent's node and attributes independent");
                const auto id = compressed.LiveCount();
                const std::wstring replacement = L"renamed.txt";
                auto& patch = compressed.patches_[a];
                patch.has_name = true; patch.off = static_cast<uint32_t>(compressed.live_.pool.size());
                patch.len = static_cast<uint16_t>(replacement.size());
                compressed.live_.pool.insert(compressed.live_.pool.end(), replacement.begin(), replacement.end());
                compressed.InvalidateFilterLocked();
                Engine::Store renamed;
                Check(compressed.FlattenLocked(renamed, volumes) && compressed.LiveCount() == id &&
                    compressed.NameOf(a) == replacement && compressed.NameOf(b) == L"independent.txt",
                    "rename overlay and subsequent flatten never mutate a different node sharing the old pool offset");
            }
        }
        std::filesystem::remove_all(directory, error);
        Check(!error, "interned-name snapshot fixtures clean up");
    }
};
} // namespace pulse::index

int main() {
    using A = pulse::index::EngineTestAccess;
    A::RawCacheAndCallers();
    A::IndexedNamesWithoutFileAccess();
    A::ScopeAndEpoch();
    A::Stopping();
    A::DenseAndSparseTraversal();
    A::AuthorizationLockScope();
    A::InternedSnapshotNames();
    std::cout << "[SUMMARY] " << A::checks - A::failures << " PASS / " << A::failures << " FAIL\n";
    return A::failures ? 1 : 0;
}
