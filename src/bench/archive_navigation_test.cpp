#include "../app/app_internal.h"
#include "../app/archive_navigation.h"
#include "../common/localization.h"
#include "../ops/archive.h"
#include <atomic>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <set>
#include <thread>

namespace {
namespace stdfs = std::filesystem;
struct Probe {
    pulse::AppState* state = nullptr;
    bool hold = false;
    std::vector<LPARAM> held;
    unsigned delivered = 0, pings = 0;
};
constexpr UINT kPing = WM_APP + 190;
LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
    auto* probe = reinterpret_cast<Probe*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        probe = static_cast<Probe*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(probe));
    }
    if (probe && message == pulse::WM_ARCHIVE_RESULT) {
        ++probe->delivered;
        if (probe->hold) probe->held.push_back(lp);
        else pulse::HandleArchiveResult(*probe->state, lp);
        return 0;
    }
    if (probe && (message == kPing || message == WM_TIMER)) { ++probe->pings; return 0; }
    return DefWindowProcW(hwnd, message, wp, lp);
}
void Pump() {
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}
bool Wait(const std::function<bool()>& done, DWORD timeout = 10000) {
    const auto deadline = GetTickCount64() + timeout;
    while (!done() && GetTickCount64() < deadline) { Pump(); Sleep(2); }
    Pump();
    return done();
}
void Word(std::ostream& stream, uint32_t value, unsigned bytes) {
    for (unsigned i = 0; i < bytes; ++i) stream.put(static_cast<char>(value >> (i * 8)));
}
void MakeTruncatedZip(const stdfs::path& path) {
    std::ofstream stream(path, std::ios::binary);
    const auto local = [&](const std::string& name, bool truncated) {
        Word(stream, 0x04034b50, 4); Word(stream, 20, 2);
        Word(stream, truncated ? 8 : 0, 2); Word(stream, truncated ? 8 : 0, 2);
        Word(stream, 0, 4); Word(stream, 0, 4); Word(stream, 0, 4); Word(stream, 0, 4);
        Word(stream, static_cast<uint32_t>(name.size()), 2); Word(stream, 0, 2); stream << name;
    };
    local("download/", false);
    local("download/readable.txt", false);
    local("download/incomplete.m4s", true);
}
struct DialogWatch {
    HWND owner = nullptr;
    std::atomic_bool seen{false};
    std::set<std::wstring> auxiliary_classes;
};
BOOL CALLBACK CancelOwnDialog(HWND window, LPARAM data) {
    auto& watch = *reinterpret_cast<DialogWatch*>(data);
    if (GetWindow(window, GW_OWNER) != watch.owner) return TRUE;
    wchar_t window_class[256]{};
    GetClassNameW(window, window_class, 256);
    const bool known_dialog = std::wstring_view(window_class) == L"#32770" ||
        std::wstring_view(window_class) == L"PulseArchiveDialog";
    const bool visible = IsWindowVisible(window) != FALSE;
    const bool owner_enabled = IsWindowEnabled(watch.owner) != FALSE;
    if (!known_dialog && !visible && owner_enabled) {
        if (watch.auxiliary_classes.insert(window_class).second)
            std::wcout << L"[INFO] hidden owned helper class=" << window_class << L" visible=" << visible <<
                L" owner_enabled=" << owner_enabled << L" style=0x" << std::hex <<
                static_cast<uint32_t>(GetWindowLongPtrW(window, GWL_STYLE)) << std::dec << L'\n';
        return TRUE;
    }
    watch.seen.store(true);
    // Only windows belonging to this fixture's UI thread are enumerated.
    PostMessageW(window, WM_COMMAND, IDNO, 0);
    PostMessageW(window, WM_CLOSE, 0, 0);
    return TRUE;
}
using Children = std::map<std::wstring, std::pair<bool, uint64_t>>;
Children Expected(const pulse::ops::ArchiveResult& listing, const std::wstring& prefix) {
    Children children;
    for (const auto& item : listing.entries) {
        auto path = item.path;
        std::replace(path.begin(), path.end(), L'\\', L'/');
        if (!path.starts_with(prefix)) continue;
        const auto tail = path.substr(prefix.size());
        if (tail.empty()) continue;
        const auto slash = tail.find(L'/');
        const bool directory = item.is_dir || slash != std::wstring::npos;
        children[tail.substr(0, slash)] = {directory, directory ? 0 : item.size};
    }
    return children;
}
Children Actual(const pulse::app::Tab& tab) {
    Children children;
    if (tab.snapshot) for (const auto& entry : *tab.snapshot)
        children[entry.name] = {entry.is_dir, entry.size};
    return children;
}
bool SameIdentity(const WIN32_FILE_ATTRIBUTE_DATA& a, const WIN32_FILE_ATTRIBUTE_DATA& b) {
    return a.nFileSizeHigh == b.nFileSizeHigh && a.nFileSizeLow == b.nFileSizeLow &&
        CompareFileTime(&a.ftLastWriteTime, &b.ftLastWriteTime) == 0;
}
}

int wmain(int argc, wchar_t** argv) {
    using namespace pulse;
    std::cout << std::unitbuf;
    int failures = 0;
    const auto check = [&](bool ok, const char* label) {
        std::cout << (ok ? "[PASS] " : "[FAIL] ") << label << '\n';
        failures += !ok;
        return ok;
    };
    if (argc > 2) { std::cerr << "usage: pulse_archive_navigation_test [read-only archive]\n"; return 2; }
    const HRESULT ole = OleInitialize(nullptr);
    if (!check(SUCCEEDED(ole), "initialize fixture UI apartment")) return 1;
    l10n::Initialize(GetModuleHandleW(nullptr), L"zh-CN");
    std::error_code error;
    const auto fixture = stdfs::absolute(stdfs::path(L"bench_data") /
        (L"archive-navigation-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64())));
    stdfs::create_directories(fixture, error);
    const bool external = argc == 2;
    const stdfs::path archive = external ? stdfs::absolute(argv[1]) : fixture / L"truncated.zip";
    if (!external) MakeTruncatedZip(archive);
    WIN32_FILE_ATTRIBUTE_DATA before{};
    if (!check(!error && GetFileAttributesExW(archive.c_str(), GetFileExInfoStandard, &before),
        "open isolated fixture or external archive without write access")) {
        stdfs::remove_all(fixture, error); OleUninitialize(); return 1;
    }

    auto owned = std::make_unique<AppState>();
    auto& state = *owned;
    state.isolatedTest = true;
    state.appPrefs.persist = state.places.persist = state.searchHistory.persist = state.ctxMenuPrefs.persist = false;
    Probe probe; probe.state = &state;
    WNDCLASSW wc{};
    wc.hInstance = GetModuleHandleW(nullptr); wc.lpfnWndProc = WindowProc;
    wc.lpszClassName = L"PulseArchiveNavigationFixture";
    RegisterClassW(&wc);
    state.hwnd = CreateWindowExW(0, wc.lpszClassName, L"Archive navigation fixture", WS_OVERLAPPEDWINDOW,
        -30000, -30000, 1100, 720, nullptr, nullptr, wc.hInstance, &probe);
    DialogWatch watch; watch.owner = state.hwnd;
    std::atomic_bool finished{false};
    const DWORD ui_thread = GetCurrentThreadId();
    std::thread watchdog([&] {
        const auto started = GetTickCount64();
        while (!finished.load()) {
            EnumThreadWindows(ui_thread, CancelOwnDialog, reinterpret_cast<LPARAM>(&watch));
            if (GetTickCount64() - started > 45000) {
                std::cerr << "[FAIL] fixture watchdog: UI did not finish within 45 seconds\n";
                TerminateProcess(GetCurrentProcess(), 2);
            }
            Sleep(10);
        }
    });
    if (check(state.hwnd && state.compositor.Init(state.hwnd), "initialize isolated application window and renderer")) {
        state.compositor.RecreateTextFormats(state.scale);
        state.renderer.SetCompositor(&state.compositor);
        state.window_tabs.NewTab(fixture.wstring());
        state.pane = state.window_tabs.Active()->FocusedPane();
        auto& tab = *ActiveTab(state);
        tab.view_mode = ui::ViewMode::Details;
        ShowWindow(state.hwnd, SW_SHOWNOACTIVATE);
        SetTimer(state.hwnd, 1, 10, nullptr);
        state.worker.Start([](app::WorkResult) {});

        auto reference = std::make_shared<ops::ArchiveResult>();
        auto reference_done = std::make_shared<std::atomic_bool>(false);
        const bool enqueued = state.worker.EnqueueIo([reference, reference_done, archive] {
            *reference = ops::ListArchive(archive.wstring()); reference_done->store(true);
        }, [] {});
        if (check(enqueued && Wait([&] { return reference_done->load(); }), "real WorkerPool completes bundled 7-Zip listing")) {
            check(reference->outcome == ops::ArchiveOutcome::PartialListing && reference->exit_code == 2u &&
                !reference->entries.empty() && !reference->error.empty() && reference->error.size() < 1024,
                "incomplete ZIP supplies safe records, real exit code and bounded warning");
            std::cout << "[INFO] accepted_records=" << reference->entries.size() << " exit_code="
                << reference->exit_code.value_or(0) << "\n";
            const auto started = GetTickCount64();
            const auto pings = probe.pings;
            tab.current_path = ArchiveViewPath(archive.wstring());
            LoadArchiveView(state, tab);
            PostMessageW(state.hwnd, kPing, 0, 0);
            check(tab.loading && Wait([&] { return !tab.loading; }), "WM_ARCHIVE_RESULT finishes actual archive navigation");
            check(probe.delivered > 0 && probe.pings > pings && !watch.seen.load(),
                "fixture window remains responsive and no modal password retry opens");
            check(tab.archive_listing && tab.snapshot && Actual(tab) == Expected(*reference, L""),
                "root snapshot publishes exactly accepted safe records and their parent folders");
            const auto cache = tab.archive_listing;
            const auto warning = tab.banner_message;
            check(tab.net_readonly && !warning.empty() && warning == reference->error &&
                tab.banner_type == app::BannerType::Other && !tab.banner_title.empty(),
                "partial listing displays explicit nonmodal warning alongside read-only contents");
            std::cout << "[INFO] navigation_ms=" << GetTickCount64() - started << " ui_pings=" << probe.pings - pings << '\n';

            const auto render = [&](const wchar_t* label) {
                const auto vm = BuildVm(state, false);
                check(vm.pane.banner_message == warning && vm.pane.banner_kind == 2 && !vm.pane.loading,
                    "production pane model exposes warning and read-only state");
                const auto rect = D2D1::RectF(0, 0, static_cast<float>(state.compositor.Width()),
                    static_cast<float>(state.compositor.Height()));
                state.compositor.Dc()->BeginDraw();
                state.renderer.Render(vm, rect, ui::MakeTheme(state.darkMode, ui::HexColor(0x0078D4)));
                check(SUCCEEDED(state.compositor.Dc()->EndDraw()), "production renderer draws archive content and warning");
                const auto capture = stdfs::absolute(stdfs::path(L"build") /
                    (std::wstring(L"archive_navigation_") + (external ? L"external_" : L"default_") + label + L".png"));
                check(state.compositor.SaveSnapshot(capture.c_str()), "save archive warning UI evidence");
            };
            render(L"root");
            std::set<std::wstring> prefixes;
            for (const auto& item : reference->entries) {
                auto path = item.path;
                std::replace(path.begin(), path.end(), L'\\', L'/');
                for (size_t slash = path.find(L'/'); slash != std::wstring::npos; slash = path.find(L'/', slash + 1))
                    prefixes.insert(path.substr(0, slash + 1));
            }
            bool cached_ok = !prefixes.empty(), safe = true;
            size_t published_files = 0;
            for (const auto& prefix : prefixes) {
                tab.current_path = ArchiveViewPath(archive.wstring(), prefix);
                LoadArchiveView(state, tab);
                cached_ok = Wait([&] { return !tab.loading; }) && cached_ok;
                cached_ok = cached_ok && tab.archive_listing == cache && tab.banner_message == warning &&
                    tab.banner_type == app::BannerType::Other && Actual(tab) == Expected(*reference, prefix);
                if (tab.snapshot) for (const auto& entry : *tab.snapshot) {
                    safe = safe && ops::IsSafeArchiveMember(prefix + entry.name);
                    published_files += !entry.is_dir;
                }
            }
            if (prefixes.empty() && tab.snapshot) for (const auto& entry : *tab.snapshot) published_files += !entry.is_dir;
            check(cached_ok, "internal directory navigation reuses cache and preserves partial-listing warning");
            check(safe, "every published internal member remains within the safe archive namespace");
            std::cout << "[INFO] internal_directories=" << prefixes.size() << " published_internal_files=" << published_files << '\n';
            render(L"internal");

            probe.hold = true;
            tab.current_path = ArchiveViewPath(archive.wstring());
            LoadArchiveView(state, tab);
            check(Wait([&] { return !probe.held.empty(); }), "capture actual worker result before UI delivery");
            const auto stale_generation = tab.pending_generation;
            tab.pending_generation = stale_generation + 1000;
            auto sentinel = std::make_shared<std::vector<pulse::fs::DirEntry>>();
            pulse::fs::DirEntry sentinel_entry; sentinel_entry.name = L"other-view.txt"; sentinel->push_back(sentinel_entry);
            tab.SetSnapshot(sentinel); tab.banner_message = L"other view warning"; tab.loading = true;
            if (!probe.held.empty()) {
                HandleArchiveResult(state, probe.held.front()); probe.held.erase(probe.held.begin());
            }
            check(tab.snapshot == sentinel && tab.pending_generation == stale_generation + 1000 && tab.loading &&
                tab.banner_message == L"other view warning", "old generation cannot overwrite newer request at the same archive path");
            LoadArchiveView(state, tab);
            check(Wait([&] { return !probe.held.empty(); }), "capture archive result before switching to another view");
            tab.current_path = fixture.wstring(); tab.SetSnapshot(sentinel);
            tab.banner_message = L"other view warning"; tab.loading = false;
            if (!probe.held.empty()) {
                HandleArchiveResult(state, probe.held.front()); probe.held.erase(probe.held.begin());
            }
            check(tab.current_path == fixture.wstring() && tab.snapshot == sentinel && !tab.loading &&
                tab.banner_message == L"other view warning", "late archive result cannot pollute a different view");
            probe.hold = false;
        }
        if (tab.archive_cancel) tab.archive_cancel->store(true);
        state.worker.Stop();
        Pump();
        for (const auto payload : probe.held) DiscardArchiveResult(payload);
        probe.held.clear();
        check(!watch.seen.load(), "only nonmodal archive warning was displayed throughout navigation");
        state.renderer.SetCompositor(nullptr);
        state.compositor.Shutdown();
    }
    finished.store(true); watchdog.join();
    if (state.hwnd) { KillTimer(state.hwnd, 1); DestroyWindow(state.hwnd); state.hwnd = nullptr; }
    owned.reset();
    WIN32_FILE_ATTRIBUTE_DATA after{};
    check(GetFileAttributesExW(archive.c_str(), GetFileExInfoStandard, &after) && SameIdentity(before, after),
        "archive size and modification time stay unchanged after read-only navigation");
    // The computed fixture root is always an immediate child of this repository's bench_data.
    const auto fixture_parent = stdfs::absolute(L"bench_data").lexically_normal();
    check(fixture.lexically_normal().parent_path() == fixture_parent, "cleanup target stays inside owned bench_data fixture");
    if (fixture.lexically_normal().parent_path() == fixture_parent) stdfs::remove_all(fixture, error);
    check(!error && !stdfs::exists(fixture), "close fixture window and remove only isolated test files");
    OleUninitialize();
    std::cout << "[INFO] failures=" << failures << '\n';
    return failures ? 1 : 0;
}
