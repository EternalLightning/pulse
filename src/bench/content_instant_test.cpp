#include "../index/content_search_client.h"
#include "../index/content_scope.h"
#include "../index/content_config_storage.h"
#include "../index/index_feed.h"
#include "../index/content_instant_session.h"
#include "../index/content_search_protocol.h"
#include <thread>
#include <filesystem>
#include <fstream>
#include <map>
#include <tlhelp32.h>
#include <cstdio>

namespace {
using namespace pulse::index;
int failures = 0;
DWORD fixture_host = 0;
void Check(bool ok, const char* message) { printf("[%s] %s\n", ok ? "PASS" : "FAIL", message); fflush(stdout); if (!ok) ++failures; }
void Write(const std::filesystem::path& file, const char* body) { std::ofstream(file, std::ios::binary | std::ios::trunc) << body; }
template<class F> bool Await(F ready, DWORD timeout = 5000) {
    const auto until = GetTickCount64() + timeout;
    do { if (ready()) return true; Sleep(10); } while (GetTickCount64() < until);
    return ready();
}
std::vector<DWORD> Children() {
    std::vector<DWORD> ids;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32W entry{sizeof(entry)};
    if (snapshot != INVALID_HANDLE_VALUE) {
        if (Process32FirstW(snapshot, &entry)) do {
            if (entry.th32ParentProcessID == GetCurrentProcessId() && entry.th32ProcessID != fixture_host && _wcsicmp(entry.szExeFile, L"Pulse.Index.exe") == 0) ids.push_back(entry.th32ProcessID);
        } while (Process32NextW(snapshot, &entry));
        CloseHandle(snapshot);
    }
    return ids;
}
uint64_t Cpu() {
    FILETIME c{}, e{}, k{}, u{}; GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u);
    return ((uint64_t{k.dwHighDateTime} << 32) | k.dwLowDateTime) + ((uint64_t{u.dwHighDateTime} << 32) | u.dwLowDateTime);
}
}
int RunInstantDirectoryTests(const std::filesystem::path& base) {
    const auto logs = base / L"directory-logs";
    SetEnvironmentVariableW(L"PULSE_CONTENT_TIMING_DIR", logs.c_str());
    for (bool ancestor : {false, true}) {
        const auto fixture = base / (ancestor ? L"ancestor-import" : L"directory-replace");
        const auto root = fixture / L"files", profile = fixture / L"profile";
        const auto slot = root / L"slot";
        const auto scope = ancestor ? slot / L"mid" / L"leaf" : root;
        std::filesystem::create_directories(scope);
        std::filesystem::create_directories(slot);
        std::filesystem::create_directories(profile);
        if (!ancestor) {
            Write(slot / L"old.txt", "directory_marker old");
            Write(slot / L"changed.txt", "directory_marker before");
        }
        ContentIndex index((profile / L"content.sqlite").wstring(), ContentAgentMode::Instant);
        ContentIndexConfig config; config.roots = {{root.wstring()}};
        config.excluded_directories = {L"excluded"};
        const bool configured = index.Configure(config);
        Check(configured, "directory delta fixture has isolated instant configuration");
        if (!configured) continue;
        ContentSearchRequest request;
        request.generation = ancestor ? 992002 : 992001;
        request.root = scope.wstring(); request.needle = L"directory_marker";
        request.task_scan = request.subscribe = true;
        std::atomic<bool> cancel{false};
        ContentInstantSessionHooks hooks;
        ContentInstantSessionHooks::Enqueue enqueue;
        ContentInstantSessionHooks::Fail fail;
        hooks.arm = [&](auto notify, auto error) { enqueue = std::move(notify); fail = std::move(error); };
        size_t full_scans = 0, local_scans = 0;
        bool bounded = true, paused = false;
        const auto expected = ancestor ? scope / L"new.txt" : slot / L"new.txt";
        hooks.search = [&](const auto& query, const auto& cancelled, auto deliver) {
            if (!query.candidate_paths.empty()) {
                ++local_scans;
                for (const auto& path : query.candidate_paths)
                    bounded &= path == expected.wstring() || (!ancestor && path == (slot / L"changed.txt").wstring());
                return index.SearchTask(query, cancelled, std::move(deliver));
            }
            ++full_scans;
            const bool ok = index.SearchTask(query, cancelled, std::move(deliver));
            // One directory notification represents each whole-tree move; no
            // synthetic child notifications hide a missing reconciliation.
            std::filesystem::rename(slot, fixture / L"departed");
            enqueue(slot.wstring(), false);
            std::filesystem::create_directories(expected.parent_path());
            Write(expected, "directory_marker new");
            if (!ancestor) Write(slot / L"changed.txt", "no match after replacement");
            else {
                std::filesystem::create_directories(slot / L"sibling");
                Write(slot / L"sibling" / L"outside.txt", "directory_marker unrelated");
                std::filesystem::create_directories(scope / L"excluded");
                Write(scope / L"excluded" / L"hidden.txt", "directory_marker excluded");
            }
            enqueue(slot.wstring(), true);
            // Seal after the known event to make this local reconciliation
            // deterministic without a timing-dependent session shutdown.
            fail(ContentSubscriptionFailure::FeedGap, ERROR_NOTIFY_ENUM_DIR);
            return ok;
        };
        std::map<std::wstring, ContentHit> visible;
        RunInstantContentSession(index, request, cancel, [&](const auto& progress, auto hits) {
            for (auto& hit : hits) {
                const auto key = ContentScopeKey(hit.path);
                if (hit.removed) visible.erase(key);
                else visible[key] = std::move(hit);
            }
            paused |= progress.subscription_error == ERROR_NOTIFY_ENUM_DIR;
            return true;
        }, &hooks);
        Check(visible.size() == 1 && visible.contains(ContentScopeKey(expected.wstring())), ancestor ?
            "deep ancestor import reaches the query root and restores its matching file" :
            "coalesced directory replacement removes missing and nonmatching old descendants");
        Check(full_scans == 1 && bounded && local_scans == (ancestor ? 1u : 2u) && paused, ancestor ?
            "ancestor traversal skips unrelated siblings and excluded directories" :
            "directory replacement rescans only current files without restarting the full query");
    }
    printf("directory delta failures=%d\n", failures);
    return failures ? 1 : 0;
}
int RunInstantGapTests(const std::filesystem::path& base) {
    if (GetEnvironmentVariableW(L"PULSE_TEST_INSTANT_DIRECTORIES", nullptr, 0)) return RunInstantDirectoryTests(base);
    const auto root = base / L"gap-files", logs = base / L"gap-logs";
    std::filesystem::create_directories(root);
    std::filesystem::create_directories(base / L"gap-profile");
    SetEnvironmentVariableW(L"PULSE_CONTENT_TIMING_DIR", logs.c_str());
    const auto changed = root / L"changed.txt";
    Write(changed, "new content");
    ContentIndex index((base / L"gap-profile" / L"content.sqlite").wstring(), ContentAgentMode::Instant);
    ContentIndexConfig config; config.roots = {{root.wstring()}};
    const bool configured = index.Configure(config);
    Check(configured, "gap fixture config is isolated and has no body writer");
    if (!configured) return 1;
    struct Case { const char* name; ContentSubscriptionFailure source; bool later; int queued; DWORD scan_error; };
    const Case cases[]{
        {"full queue duplicate", ContentSubscriptionFailure::FeedGap, false, 4096, 0},
        {"4097 unique paths", ContentSubscriptionFailure::QueueOverflow, false, 4097, 0},
        {"initial feed connection", ContentSubscriptionFailure::FeedConnect, false, 2, 0},
        {"initial feed not ready", ContentSubscriptionFailure::FeedNotReady, false, 2, 0},
        {"initial feed gap", ContentSubscriptionFailure::FeedGap, false, 2, 0},
        {"subsequent feed gap", ContentSubscriptionFailure::FeedGap, true, 2, 0},
        {"watch overflow", ContentSubscriptionFailure::WatchOverflow, true, 2, 0},
        {"watch not ready", ContentSubscriptionFailure::WatchNotReady, false, 2, 0},
        {"partial scan and feed gap", ContentSubscriptionFailure::FeedGap, true, 2, ERROR_NOT_SUPPORTED},
    };
    uint64_t generation = 991000;
    for (const auto& item : cases) {
        printf("GAP_CASE %s\n", item.name);
        ContentSearchRequest request;
        request.generation = ++generation; request.root = root.wstring();
        request.needle = L"private-gap-query"; request.task_scan = request.subscribe = request.paged_results = true;
        ContentSearchSession session(request, nullptr, 0);
        std::atomic<bool> cancel{false};
        ContentInstantSessionHooks hooks;
        ContentInstantSessionHooks::Enqueue enqueue;
        ContentInstantSessionHooks::Fail fail;
        auto signal = [&] {
            enqueue((root / L"gone-0.txt").wstring(), false);
            enqueue(changed.wstring(), false);
            for (int i = 2; i < item.queued; ++i) enqueue((root / (L"gone-" + std::to_wstring(i) + L".txt")).wstring(), false);
            if (item.queued == 4096) enqueue((root / L"gone-0.txt").wstring(), true);
            if (item.source != ContentSubscriptionFailure::QueueOverflow) fail(item.source, ERROR_NOTIFY_ENUM_DIR);
        };
        hooks.arm = [&](auto notify, auto error) { enqueue = std::move(notify); fail = std::move(error); if (!item.later) signal(); };
        size_t full_scans = 0, local_scans = 0;
        bool only_known_paths = true;
        hooks.search = [&](const auto& query, const auto&, auto deliver) {
            ContentSearchProgress progress; progress.generation = query.generation; progress.done = true;
            std::vector<ContentHit> hits;
            if (query.candidate_paths.empty()) {
                ++full_scans; progress.scanned_files = progress.total_files = 2; progress.error = item.scan_error;
                hits.push_back({(root / L"gone-0.txt").wstring(), L"gone-0.txt"});
                hits.push_back({(root / L"kept.txt").wstring(), L"kept.txt"});
            } else {
                ++local_scans; only_known_paths &= query.candidate_paths == std::vector<std::wstring>{changed.wstring()};
                hits.push_back({changed.wstring(), L"changed.txt"});
            }
            const bool accepted = deliver(progress, std::move(hits));
            if (query.candidate_paths.empty() && item.later) signal();
            return accepted;
        };
        std::shared_ptr<ContentResultStore> original, current;
        ContentSearchProgress initial, paused;
        bool known_updates_before_pause = false;
        RunInstantContentSession(index, request, cancel, [&](const auto& progress, auto hits) {
            ContentSearchUpdate raw; raw.progress = progress; raw.hits = std::move(hits);
            auto update = session.Accept(std::move(raw));
            if (!update) return true;
            current = update->results;
            if (!progress.delta && progress.done) { initial = progress; original = current; }
            if (progress.subscription_error) {
                paused = progress;
                known_updates_before_pause = current && current->Count() == 2 && local_scans == 1;
            }
            return true;
        }, &hooks);
        Check(full_scans == 1 && local_scans == 1 && only_known_paths,
            "one initial scan; backlog reads only the known changed file");
        Check(paused.subscription_error == ERROR_NOTIFY_ENUM_DIR && paused.subscription_failure == item.source &&
            !paused.live && paused.delta && paused.done && !paused.error,
            "subscription failure keeps its exact source separate from the scan error");
        Check(initial.error == item.scan_error && initial.scanned_files == 2 && initial.total_files == 2 &&
            original == current && known_updates_before_pause,
            "known deletion/addition are applied before pausing without replacing initial results or counts");
        pulse::ipc::PayloadWriter writer; content::PutSubscriptionStatus(writer, paused);
        pulse::ipc::PayloadReader reader(writer.data().data(), writer.data().size());
        ContentSearchProgress decoded;
        Check(content::GetSubscriptionStatus(reader, decoded) && decoded.subscription_error == paused.subscription_error &&
            decoded.subscription_failure == item.source, "subscription status survives the production protocol fields");
    }
    ContentInstantSessionHooks hooks;
    hooks.arm = [](auto, auto) {};
    std::atomic<bool> cancel{false}, started{false}, exited{false};
    hooks.search = [&](const auto& request, const auto&, auto callback) {
        ContentSearchProgress progress; progress.generation = request.generation; progress.done = true;
        const bool accepted = callback(progress, {}); started = true; return accepted;
    };
    ContentSearchRequest request; request.generation = ++generation; request.subscribe = true;
    std::thread active([&] { RunInstantContentSession(index, request, cancel, [](const auto&, auto) { return true; }, &hooks); exited = true; });
    Check(Await([&] { return started.load(); }), "subscribed fixture reaches its idle wait");
    const auto stop_started = GetTickCount64(); cancel = true;
    const bool stopped = Await([&] { return exited.load(); }, 2000);
    active.join();
    Check(stopped && GetTickCount64() - stop_started < 2000, "cancellation interrupts a paused notification wait within two seconds");
    size_t log_rows = 0;
    bool private_text = false, phases = false;
    for (const auto& entry : std::filesystem::directory_iterator(logs)) {
        std::ifstream input(entry.path());
        for (std::string line; std::getline(input, line); ) {
            ++log_rows;
            private_text |= line.find("private-gap-query") != std::string::npos || line.find("changed.txt") != std::string::npos;
            phases |= line.find("\"during_initial_scan\":false") != std::string::npos;
        }
    }
    Check(log_rows == std::size(cases) && !private_text && phases,
        "bounded default subscription diagnostics distinguish phases without query text or paths");
    return failures ? 1 : 0;
}
int RunInstantLifecycleTests(const std::filesystem::path& base) {
    const auto profile = base / L"profile", files = base / L"files";
    std::filesystem::create_directories(profile); std::filesystem::create_directories(files);
    SetEnvironmentVariableW(L"LOCALAPPDATA", profile.c_str());
    SetEnvironmentVariableW(L"PULSE_DOCUMENT_ADMISSION_DIR", profile.c_str());
    const auto logs = base / L"logs";
    SetEnvironmentVariableW(L"PULSE_CONTENT_TIMING_DIR", logs.c_str());
    const auto cache = profile / L"Pulse" / L"ContentIndex" / L"content-v1.sqlite";
    Write(files / L"a.txt", "instant_marker first"); Write(files / L"b.txt", "absent");
    const auto nested = files / L"parent" / L"sub";
    std::filesystem::create_directories(nested);
    for (int i = 0; i < 128; ++i) Write(nested / (L"untouched-" + std::to_wstring(i) + L".txt"), "unchanged");
    struct Host {
        HANDLE process = nullptr;
        ~Host() { if (process) { TerminateProcess(process, 0); WaitForSingleObject(process, 2000); CloseHandle(process); } fixture_host = 0; }
    } host;
    wchar_t shared_mode[8]{};
    const bool shared = GetEnvironmentVariableW(L"PULSE_TEST_INSTANT_SHARED", shared_mode, ARRAYSIZE(shared_mode)) != 0;
    if (shared) {
        const auto token = L"instant-" + std::to_wstring(GetCurrentProcessId());
        SetEnvironmentVariableW(L"PULSE_INDEX_FEED_PIPE", (L"\\\\.\\pipe\\PulseIndex.Test." + token).c_str());
        wchar_t module[32768]{}; GetModuleFileNameW(nullptr, module, ARRAYSIZE(module));
        const auto exe = std::filesystem::path(module).parent_path() / L"Pulse.Index.exe";
        std::wstring command = L"\"" + exe.wstring() + L"\" --test-host " + token + L" \"" + files.wstring() + L"\" \"" + (base / L"filename-cache").wstring() + L"\"";
        STARTUPINFOW startup{sizeof(startup)}; PROCESS_INFORMATION process{};
        if (!CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) {
            Check(false, "isolated filename feed host launches"); return 1;
        }
        host.process = process.hProcess; fixture_host = process.dwProcessId; CloseHandle(process.hThread);
        IndexFeedConnection feed(nullptr);
        Check(Await([&] { FileFeedPage page; return feed.Request(true, L"", 0, 0, page) && page.ready; }, 10000), "isolated filename feed is ready");
    }
    ContentSearchClient client;
    client.Start(nullptr, 0, ContentAgentMode::Instant);
    Check(Await([&] { return client.ConfigurationReady(); }), "instant config loads without starting a writer");
    ContentIndexConfig config; config.roots = {{files.wstring()}};
    config.shared_scope = shared;
    client.Configure(config);
    Check(Await([&] { return client.GetConfig().roots.size() == 1; }), "instant scope can be saved without a body database");
    Check(Children().empty() && !std::filesystem::exists(cache), "startup/configuration have no content process or body database");
    wchar_t seconds[16]{}; GetEnvironmentVariableW(L"PULSE_TEST_INSTANT_IDLE_SECONDS", seconds, ARRAYSIZE(seconds));
    const DWORD idle_ms = wcstoul(seconds, nullptr, 10) * 1000;
    if (idle_ms) {
        const auto start = GetTickCount64(), cpu = Cpu();
        while (GetTickCount64() - start < idle_ms) { (void)client.GetStatus(); Sleep(100); }
        const double cpu_percent = static_cast<double>(Cpu() - cpu) / (GetTickCount64() - start) / 100.0;
        printf("IDLE wall_ms=%llu cpu_one_core_percent=%.4f\n", GetTickCount64() - start, cpu_percent);
        Check(Children().empty() && !std::filesystem::exists(cache), "over-five-minute idle never starts periodic body reconciliation");
        Check(cpu_percent < 1.0, "instant idle CPU is below one percent of one core");
        client.Stop(); return failures ? 1 : 0;
    }
    ContentSearchRequest request; request.generation = 990001; request.session_id = 1;
    request.root = files.wstring(); request.needle = L"instant_marker"; request.task_scan = true; request.subscribe = true;
    std::map<std::wstring, ContentHit> visible;
    size_t completed = 0, delta_count = 0;
    DWORD last_error = 0;
    auto pump = [&] {
        ContentSearchUpdate update;
        while (client.TakeUpdate(update)) {
            if (update.progress.generation != request.generation) continue;
            if (update.progress.delta) ++delta_count;
            else if (update.progress.done) ++completed;
            if (update.progress.error) last_error = update.progress.error;
            for (auto& hit : update.hits) {
                const auto key = ContentScopeKey(hit.path);
                if (hit.removed) visible.erase(key); else visible[key] = std::move(hit);
            }
        }
    };
    client.SearchAsync(request);
    Check(Await([&] { pump(); return completed == 1 && visible.size() == 1; }), "first-run instant search works without any cache");
    Check(last_error == 0 && !std::filesystem::exists(cache), "successful instant search never writes persistent body cache");
    wchar_t crash_ready[32768]{};
    if (GetEnvironmentVariableW(L"PULSE_TEST_INSTANT_CRASH_READY", crash_ready, ARRAYSIZE(crash_ready))) {
        const auto children = Children();
        { std::ofstream ready(std::filesystem::path(crash_ready), std::ios::trunc); for (auto id : children) ready << id << '\n'; }
        // The isolated harness terminates this parent without running its destructors.
        for (;;) {
            const auto stop_file = std::wstring(crash_ready) + L".stop";
            if (GetFileAttributesW(stop_file.c_str()) != INVALID_FILE_ATTRIBUTES) {
                const auto start = GetTickCount64(); client.Suspend();
                const bool exited = Await([&] { return Children().empty(); }, 2000);
                { std::ofstream result(std::filesystem::path(std::wstring(crash_ready) + L".stopped")); result << (GetTickCount64() - start) << ' ' << exited; }
                client.Stop(); return exited ? 0 : 1;
            }
            Sleep(10);
        }
    }
    Write(files / L"b.txt", "instant_marker newly saved");
    Check(Await([&] { pump(); return visible.size() == 2; }), "changed file is searched and added after initial completion");
    Write(files / L"a.txt", "no longer matches");
    Check(Await([&] { pump(); return visible.size() == 1 && visible.contains(ContentScopeKey((files / L"b.txt").wstring())); }), "modified nonmatch is removed from results");
    std::filesystem::rename(files / L"b.txt", files / L"renamed.txt");
    Check(Await([&] { pump(); return visible.size() == 1 && visible.contains(ContentScopeKey((files / L"renamed.txt").wstring())); }), "rename replaces the old result path");
    std::filesystem::remove(files / L"renamed.txt");
    Check(Await([&] { pump(); return visible.empty(); }), "deleted file is removed by a delta");
    Check(completed == 1 && delta_count >= 4, "changes never restart the full search");
    const auto before_delta = delta_count;
    Write(nested / L"z-trigger.txt", "instant_marker");
    Check(Await([&] { pump(); return visible.size() == 1; }) && delta_count - before_delta <= 3,
        "single nested file change does not scan 128 siblings on parent Modified notification");
    std::filesystem::remove(nested / L"z-trigger.txt");
    Await([&] { pump(); return visible.empty(); });

    Write(files / L"a.txt", "instant_marker again");
    Await([&] { pump(); return visible.size() == 1; });
    ContentSearchClient second;
    second.Start(nullptr, 0, ContentAgentMode::Instant);
    Await([&] { return second.ConfigurationReady(); });
    auto other = request; other.generation = 990010;
    second.SearchAsync(other);
    bool second_done = false;
    Check(Await([&] { ContentSearchUpdate update; while (second.TakeUpdate(update)) second_done |= update.progress.done; return second_done && Children().size() == 2; }), "two independent clients own separate content agents");
    const auto stopped = GetTickCount64(); client.Suspend();
    Check(Await([&] { return Children().size() == 1; }, 2000), "suspend terminates only this client's agent within two seconds");
    printf("SUSPEND elapsed_ms=%llu\n", GetTickCount64() - stopped);
    const auto before = visible.size();
    Write(files / L"after-close.txt", "instant_marker");
    Sleep(250); pump();
    Check(visible.size() == before && client.CurrentGeneration() == 0, "suspended query retains results without restarting");
    second.Stop(); Check(Await([&] { return Children().empty(); }, 2000), "last client's exit leaves no owned content process");

    // Explicit new input resumes; queued old generations must not leak into the result.
    request.generation += 100; client.SearchAsync(request);
    request.generation += 1; client.SearchAsync(request);
    request.generation += 1; client.SearchAsync(request);
    completed = 0; visible.clear(); last_error = 0;
    Check(Await([&] { pump(); return completed == 1 && visible.size() == 2; }), "rapid explicit queries after suspend accept only the latest generation");
    const auto exit = GetTickCount64(); client.Stop();
    printf("STOP elapsed_ms=%llu\n", GetTickCount64() - exit);
    Check(GetTickCount64() - exit < 2000 && Children().empty(), "stop joins the active subscription within two seconds");
    Check(!std::filesystem::exists(cache), "entire lifecycle leaves body cache absent");

    ContentSearchClient interrupted;
    interrupted.Start(nullptr, 0, ContentAgentMode::Instant); Await([&] { return interrupted.ConfigurationReady(); });
    auto interrupted_request = request; interrupted_request.generation += 300;
    interrupted.SearchAsync(interrupted_request);
    bool ready = false, disconnected = false;
    auto interrupted_pump = [&] { ContentSearchUpdate update; while (interrupted.TakeUpdate(update)) {
        ready |= update.progress.done;
        disconnected |= update.progress.subscription_error == ERROR_BROKEN_PIPE &&
            update.progress.subscription_failure == ContentSubscriptionFailure::Transport &&
            update.progress.done && update.progress.delta && !update.progress.live && !update.progress.error;
    } };
    Check(Await([&] { interrupted_pump(); return ready; }), "subscription is ready before simulated agent failure");
    for (const auto id : Children()) { HANDLE process = OpenProcess(PROCESS_TERMINATE, FALSE, id); if (process) { TerminateProcess(process, 1); CloseHandle(process); } }
    Check(Await([&] { interrupted_pump(); return disconnected; }), "agent failure stops subscription with an explicit error");
    Sleep(400);
    Check(Children().empty(), "agent failure never restarts a full-scope scan automatically");
    interrupted_request.generation += 1; ready = false; interrupted.SearchAsync(interrupted_request);
    Check(Await([&] { interrupted_pump(); return ready; }), "explicit search can restart after agent failure");
    interrupted.Cancel(interrupted_request.session_id);
    Check(Await([&] { return Children().empty(); }, 2000), "cancelling the final task releases its agent within two seconds");
    interrupted.Stop();

    ContentSearchClient scoped;
    scoped.Start(nullptr, 0, ContentAgentMode::Instant); Await([&] { return scoped.ConfigurationReady(); });
    Write(nested / L"scoped.txt", "instant_marker");
    auto scoped_request = request; scoped_request.root = nested.wstring(); scoped_request.generation += 500;
    scoped.SearchAsync(scoped_request); bool scoped_hit = false, scoped_removed = false, scoped_done = false;
    const bool rename_after_complete = GetEnvironmentVariableW(L"PULSE_TEST_INSTANT_RENAME_AFTER_COMPLETE", nullptr, 0) != 0;
    auto scoped_pump = [&] { ContentSearchUpdate update; while (scoped.TakeUpdate(update)) {
        scoped_done |= update.progress.done && !update.progress.delta;
        for (const auto& hit : update.hits) { scoped_hit |= !hit.removed; scoped_removed |= hit.removed; }
    } };
    Check(Await([&] { scoped_pump(); return scoped_hit && (!rename_after_complete || scoped_done); }),
        rename_after_complete ? "scoped query completes within nested requested root" : "scoped query searches within nested requested root");
    std::error_code rename_error;
    std::filesystem::rename(files / L"parent", files / L"renamed-parent", rename_error);
    if (rename_error) printf("ANCESTOR_RENAME error=%d message=%s\n", rename_error.value(), rename_error.message().c_str());
    Check(!rename_error && Await([&] { scoped_pump(); return scoped_removed; }), "renaming an ancestor removes stale descendants from a scoped query");
    scoped.Stop();

    ContentSearchClient locked;
    locked.Start(nullptr, 0, ContentAgentMode::Instant); Await([&] { return locked.ConfigurationReady(); });
    HANDLE config_mutex = config_storage::NamedMutex(cache.wstring(), L"config");
    WaitForSingleObject(config_mutex, INFINITE);
    locked.Configure(config); Sleep(50);
    const auto lock_stop = GetTickCount64(); locked.Stop();
    Check(GetTickCount64() - lock_stop < 500, "close interrupts contended configuration mutex without waiting for timeout");
    ReleaseMutex(config_mutex); CloseHandle(config_mutex);

    // Build a tiny legacy cache explicitly, then verify read-only compatibility and stale validation.
    { ContentIndex legacy(cache.wstring()); legacy.Configure(config); Check(legacy.WaitUntilIdle(10000), "explicit legacy writer still builds an isolated cache"); }
    const auto before_size = std::filesystem::file_size(cache);
    const auto before_time = std::filesystem::last_write_time(cache);
    Write(files / L"a.txt", "changed to a nonmatch after cache build");
    { ContentIndex instant(cache.wstring(), ContentAgentMode::Instant);
      std::atomic<bool> cancel{false}; size_t hits = 0; bool done = false;
      request.subscribe = false;
      instant.SearchTask(request, cancel, [&](const auto& progress, auto batch) { hits += batch.size(); done |= progress.done; return true; });
      Check(done && hits == 2, "instant scan validates existing cache and rejects stale positive"); }
    Check(std::filesystem::file_size(cache) == before_size && std::filesystem::last_write_time(cache) == before_time,
        "reusing legacy cache does not modify its body database");
    printf("instant lifecycle failures=%d\n", failures);
    return failures ? 1 : 0;
}
