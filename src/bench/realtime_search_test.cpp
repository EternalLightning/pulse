// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "../app/app_internal.h"
#include "../app/search_query.h"
#include "content_search_fixture.h"
#include "../index/search_trace.h"
#include "../index/index_feed.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <algorithm>

int wmain(int argc, wchar_t** argv) {
    using namespace pulse;
    const std::wstring filter = argc == 2 ? argv[1] : L"all";
    if (argc > 2 || (filter != L"all" && filter != L"content" && filter != L"content-instant" && filter != L"filename" && filter != L"dedup")) {
        std::cerr << "Usage: pulse_realtime_search_test.exe [all|content|content-instant|filename|dedup]" << std::endl;
        return 2;
    }
    bench::ContentSearchFixture fixture;
    if (!fixture.Initialize(L"../bench_data", L"realtime-search")) {
        std::cerr << "[FAIL] initialize private content database and storage sentinels" << std::endl;
        std::cerr << "[INFO] fixture stage=" << fixture.Stage() << " error=" << GetLastError() << std::endl;
        std::wcerr << L"[INFO] attempted=" << fixture.AttemptedPath() << L" fs_error=" << fixture.FilesystemError() << std::endl;
        return 1;
    }
    const auto& base = fixture.Root();
    const auto root = base / L"files";
    const auto profile = base / L"profile";
    const auto filename_cache = base / L"name-index";
    std::filesystem::create_directories(root);
    std::filesystem::create_directories(filename_cache);
    SetEnvironmentVariableW(L"PULSE_SEARCH_TRACE", (base / L"trace.csv").c_str());
    const auto token = std::to_wstring(GetCurrentProcessId());
    const auto pipe_name = L"\\\\.\\pipe\\PulseIndex.Test." + token;
    if (!SetEnvironmentVariableW(L"PULSE_INDEX_FEED_PIPE", pipe_name.c_str())) {
        std::cerr << "[FAIL] set isolated feed pipe error=" << GetLastError() << std::endl;
        return 1;
    }
    const auto exe = std::filesystem::absolute(L"Pulse.Index.exe");
    std::wstring command = L"\"" + exe.wstring() + L"\" --test-host " + token + L" \"" + root.wstring() + L"\" \"" + filename_cache.wstring() + L"\"";
    STARTUPINFOW startup{sizeof(startup)}; PROCESS_INFORMATION host{};
    std::wcout << L"[INFO] stage=launch exe=" << exe << L" root=" << root
        << L" cache=" << filename_cache << L" profile=" << profile << L" pipe=" << pipe_name
        << L" filter=" << filter << L" shared_scope=true" << std::endl;
    if (!CreateProcessW(exe.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&startup,&host)) {
        std::cerr << "[FAIL] stage=launch error=" << GetLastError() << std::endl;
        return 1;
    }
    CloseHandle(host.hThread);
    auto owned = std::make_unique<AppState>(); auto& s = *owned;
    s.isolatedTest = true; s.appPrefs.persist = false;
    if (!s.contentSearch.SetDatabasePathForTesting(fixture.DatabasePath())) {
        std::cerr << "[FAIL] bind realtime client to private content database" << std::endl;
        TerminateProcess(host.hProcess, ERROR_CANCELLED); WaitForSingleObject(host.hProcess, 5000); CloseHandle(host.hProcess);
        return 1;
    }
    s.hwnd = CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, nullptr, nullptr);
    s.window_tabs.EnsureDefault(); s.pane = s.window_tabs.Active()->FocusedPane();
    auto& tab = *ActiveTab(s);
    const std::wstring query = L"content:pulserealtime";
    tab.current_path = app::MakeSearchPath(query);
    s.index.Start(nullptr,0,0,pipe_name);
    index::ContentIndexConfig config; config.roots = {{root.wstring()}};
    config.shared_scope = true;
    if (filter != L"filename" && filter != L"dedup") {
        s.contentSearch.Start(nullptr, 0, filter == L"content-instant" ?
            index::ContentAgentMode::Instant : index::ContentAgentMode::LegacyWriter);
        s.contentSearch.Configure(config);
    }
    auto diagnose = [&](const char* stage) {
        DWORD exit_code = 0;
        const bool process_ok = GetExitCodeProcess(host.hProcess, &exit_code) != FALSE;
        const DWORD process_error = process_ok ? ERROR_SUCCESS : GetLastError();
        const bool pipe_ready = WaitNamedPipeW(pipe_name.c_str(), 1) != FALSE;
        const DWORD pipe_error = pipe_ready ? ERROR_SUCCESS : GetLastError();
        std::cout << "[INFO] stage=" << stage << " pid=" << host.dwProcessId
            << " exit_code=" << exit_code << " process_error=" << process_error
            << " pipe_ready=" << pipe_ready << " pipe_error=" << pipe_error
            << " connected=" << s.index.Connected() << " revision=" << s.index.Revision() << std::endl;
        std::wcout << L"[INFO] filename status=" << s.index.Status() << std::endl;
    };
    size_t results_taken = 0;
    DWORD content_error = ERROR_SUCCESS;
    auto wait = [&](auto ready, DWORD timeout) {
        const auto until = GetTickCount64() + timeout;
        while (GetTickCount64() < until) {
            index::ContentSearchUpdate update;
            while (s.contentSearch.TakeUpdate(update)) {
                if (update.progress.error) content_error = update.progress.error;
                ApplyContentSearchUpdate(s, std::move(update));
            }
            std::vector<uint32_t> ids;
            for (const auto& [id,pending] : s.pendingIndexSearches) ids.push_back(id);
            for (auto id : ids) { index::SearchResult result; if(s.index.TakeResult(id,result)) { ++results_taken; AcceptIndexProviderResult(s,id,std::move(result),false); } }
            TickAddressSearch(s, GetTickCount64());
            if (ready()) return true;
            if (WaitForSingleObject(host.hProcess, 0) == WAIT_OBJECT_0) {
                diagnose("host-exited");
                return false;
            }
            Sleep(5);
        }
        return false;
    };
    int failures = 0;
    auto check = [&](bool ok, const char* label) {
        std::cout << (ok ? "[PASS] " : "[FAIL] ") << label << std::endl;
        if (!ok) ++failures;
    };
    const bool connected = wait([&] { return s.index.Connected() && s.index.Revision() > 0; }, 15000);
    check(connected, "isolated filename host connected and ready");
    diagnose(connected ? "host-ready" : "host-connect-failed");
    std::ofstream report(base / L"latency.csv"); report << "operation,iteration,milliseconds,success\n";
    wchar_t baseline[8]{};
    const bool record_only = GetEnvironmentVariableW(L"PULSE_REALTIME_BASELINE", baseline, 8) != 0;
    const int repeats = record_only ? 10 : 100;
    auto summarize = [&](std::vector<ULONGLONG> values, const char* label, ULONGLONG target,
                         bool enforce = true, const char* unit = "ms") {
        if (values.empty()) { check(false, label); return; }
        std::sort(values.begin(), values.end());
        auto p = [&](size_t percent) { return values[(values.size() * percent + 99) / 100 - 1]; };
        std::cout << "[INFO] " << label << " n=" << values.size() << " P50=" << p(50) << " P95=" << p(95) << " P99=" << p(99) << ' ' << unit << std::endl;
        if (!record_only && enforce) check(values.size() == 100 && p(95) <= target, label);
    };
    if (connected && filter != L"filename" && filter != L"dedup") {
        check(wait([&] { auto status = s.contentSearch.GetStatus(); return s.contentSearch.ConfigurationReady() && !status.indexing && !status.pending_files && s.contentSearch.GetConfig().shared_scope && !s.contentSearch.GetConfig().roots.empty(); }, 30000), "isolated shared content agent becomes ready");
        { const auto status = s.contentSearch.GetStatus(); std::cout << "[INFO] status error=" << status.error << " indexed=" << status.indexed_files << " pending=" << status.pending_files << " ready=" << s.contentSearch.ConfigurationReady() << std::endl; }
        if (filter == L"content-instant") {
            index::IndexFeedConnection feed(nullptr);
            check(wait([&] {
                index::FileFeedPage page;
                return feed.Request(false, root.wstring(), 0, 0, page) && page.ready && !page.gap;
            }, 15000), "filename feed snapshot is ready for the instant initial scan");
        }
        RequestSearchPage(s, tab, query, true);
        const bool initial_ok = wait([&] { return !tab.search_content_active; }, 10000) && !content_error;
        check(initial_ok, "initial query completes without error through IPC and application result handler");
        if (!initial_ok) std::cout << "[INFO] initial content query error=" << content_error << std::endl;
        std::vector<ULONGLONG> creates, removes, delete_calls;
        LARGE_INTEGER frequency{};
        QueryPerformanceFrequency(&frequency);
        const auto file = root / L"live.txt";
        for (int i = 0; initial_ok && i < repeats; ++i) {
            auto start = GetTickCount64();
            { std::ofstream out(file); out << "pulserealtime " << i; }
            bool ok = wait([&] { return !tab.search_content_active && tab.EntryCount() == 1; }, 5000);
            auto elapsed = GetTickCount64() - start; creates.push_back(elapsed);
            report << "text_create," << i << ',' << elapsed << ',' << ok << '\n';
            if (!ok) { check(false, "new text file appears without manual refresh"); break; }
            start = GetTickCount64();
            index::TraceSearch("test_delete_begin", i, file.wstring());
            LARGE_INTEGER delete_begin{}, delete_end{};
            QueryPerformanceCounter(&delete_begin);
            const bool deleted = DeleteFileW(file.c_str()) != FALSE;
            QueryPerformanceCounter(&delete_end);
            delete_calls.push_back(static_cast<ULONGLONG>((delete_end.QuadPart - delete_begin.QuadPart) * 1000000 / frequency.QuadPart));
            index::TraceSearch("test_delete_return", i, file.wstring());
            ok = deleted && wait([&] { return !tab.search_content_active && tab.EntryCount() == 0; }, 5000);
            index::TraceSearch(ok ? "test_delete_visible" : "test_delete_timeout", i, file.wstring());
            elapsed = GetTickCount64() - start; removes.push_back(elapsed);
            report << "delete," << i << ',' << elapsed << ',' << ok << '\n';
            if (!ok) { check(false, "deleted file disappears without manual refresh"); break; }
            if (i % 10 == 0) std::cout << "[INFO] completed " << i + 1 << '/' << repeats << " real create/delete pairs" << std::endl;
        }
        summarize(creates, "text save -> UI <= 1000 ms", 1000);
        summarize(removes, "delete -> UI <= 300 ms", 300);
        summarize(delete_calls, "DeleteFileW call duration", 0, false, "us");
        s.contentSearch.Cancel(tab.search_session_id); tab.search_live_generation = 0;
    }
    if (connected && (filter == L"all" || filter == L"dedup")) {
        const auto dir = root / L"dedup";
        std::filesystem::create_directories(dir);
        const std::wstring needle = L"pulsededup";
        tab.current_path = app::MakeSearchPath(needle);
        tab.ClearSelection();
        RequestSearchPage(s, tab, needle, true);
        check(wait([&] { return !tab.loading && !tab.pending_generation; }, 10000), "dedup: subscription returns initial page");
        const auto a = dir / L"pulsededup-a.txt", b = dir / L"pulsededup-b.txt";
        const auto c = dir / L"pulsededup-c.txt", d = dir / L"pulsededup-d.txt";
        auto write = [](const std::filesystem::path& path) { std::ofstream out(path); out << 1; return out.good(); };
        auto selected = [&](const std::filesystem::path& path) {
            for (size_t i = 0; i < tab.EntryCount(); ++i)
                if (tab.EntryAt(i).full_path == path.wstring()) return tab.IsSelected(static_cast<int>(i));
            return false;
        };
        write(a); write(b);
        check(wait([&] { return tab.EntryCount() == 2; }, 5000), "dedup: two matching files appear");
        tab.SelectIndices({0, 1});
        const size_t before = results_taken;
        const auto noise = dir / L"unrelated-noise.bin";
        for (int i = 0; i < 10; ++i) {
            write(noise); wait([] { return false; }, 60);
            DeleteFileW(noise.c_str()); wait([] { return false; }, 60);
        }
        wait([] { return false; }, 800);
        std::cout << "[INFO] dedup: pages received during unrelated churn=" << results_taken - before << std::endl;
        check(results_taken == before, "dedup: unrelated file changes push no unchanged page");
        write(c);
        check(wait([&] { return tab.EntryCount() == 3; }, 5000), "dedup: a changed page still arrives");
        check(tab.SelectedCount() == 2 && selected(a) && selected(b) && !selected(c),
              "dedup: live refresh keeps a multi-selection by full path");
        tab.ClearSelection();
        write(d);
        check(wait([&] { return tab.EntryCount() == 4; }, 5000) && tab.SelectedCount() == 0,
              "dedup: live refresh selects nothing when nothing was selected");
        for (const auto& path : {a, b, c, d}) DeleteFileW(path.c_str());
        wait([&] { return tab.EntryCount() == 0; }, 5000);
    }
    if (connected && filter != L"content" && filter != L"content-instant" && filter != L"dedup") {
        tab.current_path = app::MakeSearchPath(L"pulserealtime");
        RequestSearchPage(s,tab,L"pulserealtime",true);
        check(wait([&]{return !tab.loading && !tab.pending_generation;},10000),"filename subscription returns initial page");
        std::vector<ULONGLONG> name_create, name_delete, name_rename, name_move;
        const auto source = root / L"pulserealtime.txt", renamed = root / L"pulserealtime-renamed.txt";
        const auto subdirectory = root / L"subdirectory";
        std::filesystem::create_directories(subdirectory);
        const auto moved = subdirectory / renamed.filename();
        auto timed = [&](const char* operation, int iteration, auto mutate, auto visible, std::vector<ULONGLONG>& values) {
            const auto start = GetTickCount64(); const bool ok = mutate() && wait(visible,5000);
            const auto elapsed = GetTickCount64()-start; values.push_back(elapsed);
            report << operation << ',' << iteration << ',' << elapsed << ',' << ok << '\n';
            if (!ok) { check(false,operation); diagnose(operation); }
            return ok;
        };
        for (int i=0;i<repeats;++i) {
            if (!timed("filename_create",i,[&]{std::ofstream out(source); out << i; return out.good();},[&]{return tab.EntryCount()==1;},name_create)) break;
            if (!timed("filename_rename",i,[&]{return MoveFileW(source.c_str(),renamed.c_str())!=FALSE;},[&]{return tab.EntryCount()==1 && tab.EntryAt(0).full_path==renamed.wstring();},name_rename)) break;
            if (!timed("filename_move",i,[&]{return MoveFileW(renamed.c_str(),moved.c_str())!=FALSE;},[&]{return tab.EntryCount()==1 && tab.EntryAt(0).full_path==moved.wstring();},name_move)) break;
            if (!timed("filename_delete",i,[&]{return DeleteFileW(moved.c_str())!=FALSE;},[&]{return tab.EntryCount()==0;},name_delete)) break;
            if(i%10==0) std::cout << "[INFO] filename operation pairs " << i+1 << '/' << repeats << std::endl;
        }
        summarize(name_create,"filename create -> UI <= 300 ms",300);
        summarize(name_rename,"filename rename -> UI <= 300 ms",300);
        summarize(name_move,"filename move -> UI <= 300 ms",300);
        summarize(name_delete,"filename delete -> UI <= 300 ms",300);
    }
    s.contentSearch.Stop(); DestroyWindow(s.hwnd); s.hwnd = nullptr;
    s.index.Stop();
    HANDLE control = CreateFileW(pipe_name.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,0,nullptr);
    if(control!=INVALID_HANDLE_VALUE) { const auto header=index::MakeIndexHdr(index::REQ_IDX_TEST_SHUTDOWN,0,0); DWORD written=0; WriteFile(control,&header,sizeof(header),&written,nullptr); CloseHandle(control); }
    if(WaitForSingleObject(host.hProcess,5000)!=WAIT_OBJECT_0) { check(false,"test filename host shuts down cleanly"); diagnose("shutdown-timeout"); TerminateProcess(host.hProcess,ERROR_CANCELLED); WaitForSingleObject(host.hProcess,5000); }
    CloseHandle(host.hProcess);
    if (filter != L"filename" && filter != L"dedup")
        check(fixture.ConfigurationMatches(config), "realtime agent writes only the selected database configuration");
    check(fixture.DefaultConfigurationUnchanged(), "private default configuration sentinel stays unchanged");
    check(fixture.LocatorUnchanged(), "private file locator sentinel stays unchanged");
    std::wcout << L"[INFO] report: " << (base / L"latency.csv").wstring() << std::endl;
    return failures ? 1 : 0;
}
