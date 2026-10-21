#include "../app/app_internal.h"
#include "../app/archive_navigation.h"
#include "../app/archive_operation_ui.h"
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
#include <vector>

namespace {
namespace stdfs = std::filesystem;
struct Probe {
    pulse::AppState* state = nullptr;
    bool hold = false;
    std::vector<LPARAM> held;
    unsigned delivered = 0, pings = 0;
};
constexpr UINT kPing = WM_APP + 190;
constexpr wchar_t kProbeProperty[] = L"PulseArchiveNavigationProbe";
LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
    if (message == WM_NCCREATE) {
        auto* state = static_cast<pulse::AppState*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }
    auto* probe = static_cast<Probe*>(GetPropW(hwnd, kProbeProperty));
    if (probe && message == pulse::WM_ARCHIVE_RESULT) {
        ++probe->delivered;
        if (probe->hold) probe->held.push_back(lp);
        else pulse::HandleArchiveResult(*probe->state, lp);
        return 0;
    }
    if (probe && (message == kPing || message == WM_TIMER)) { ++probe->pings; return 0; }
    if (message == WM_NCDESTROY) RemovePropW(hwnd, kProbeProperty);
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
bool MakeEncrypted7z(const stdfs::path& archive, const stdfs::path& source) {
    stdfs::path executable;
#ifdef PULSE_ARCHIVE_RUNTIME_DIR
    executable = stdfs::path(PULSE_ARCHIVE_RUNTIME_DIR) / L"7z.exe";
#else
    wchar_t module[32768]{};
    GetModuleFileNameW(nullptr, module, ARRAYSIZE(module));
    executable = stdfs::path(module).parent_path() / L"7zip" / L"7z.exe";
#endif
    std::wstring command = L"\"" + executable.wstring() + L"\" a -y -bd -psecret \"" +
        archive.wstring() + L"\" \".\\*\"";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE,
        CREATE_NO_WINDOW, nullptr, source.c_str(), &startup, &process)) return false;
    const bool exited = WaitForSingleObject(process.hProcess, 30000) == WAIT_OBJECT_0;
    DWORD code = 2;
    if (exited) GetExitCodeProcess(process.hProcess, &code);
    else {
        TerminateProcess(process.hProcess, 2);
        WaitForSingleObject(process.hProcess, INFINITE);
    }
    CloseHandle(process.hProcess);
    CloseHandle(process.hThread);
    return exited && code == 0;
}
struct DialogWatch {
    HWND owner = nullptr;
    std::atomic_bool seen{false};
    std::atomic_bool password_error_seen{false};
    std::atomic_uint password_prompts{0};
    std::vector<std::wstring> password_inputs;
    std::set<HWND> handled_password_dialogs;
    std::set<std::wstring> auxiliary_classes;
    std::set<std::wstring> operation_titles;
};
BOOL CALLBACK CancelOwnDialog(HWND window, LPARAM data) {
    auto& watch = *reinterpret_cast<DialogWatch*>(data);
    if (GetWindow(window, GW_OWNER) != watch.owner) return TRUE;
    wchar_t window_class[256]{};
    GetClassNameW(window, window_class, 256);
    wchar_t window_title[512]{};
    GetWindowTextW(window, window_title, ARRAYSIZE(window_title));
    const std::wstring_view class_name(window_class);
    if (class_name == L"IME" || class_name == L"MSCTFIME UI" ||
        class_name == L"PulseOwnerDimOverlay" || class_name == L"tooltips_class32") return TRUE;
    std::erase_if(watch.handled_password_dialogs, [](HWND handle) { return !IsWindow(handle); });
    if (std::wstring_view(window_class) == L"PulseArchiveDialog" &&
        std::wstring_view(window_title) == pulse::l10n::Get(pulse::l10n::StringId::ArchivePassword) &&
        watch.handled_password_dialogs.insert(window).second) {
        const auto index = watch.password_prompts.fetch_add(1, std::memory_order_relaxed);
        if (index < watch.password_inputs.size()) {
            const auto text = watch.password_inputs[index];
            SendMessageW(GetDlgItem(window, 101), WM_SETTEXT, 0, reinterpret_cast<LPARAM>(text.c_str()));
            PostMessageW(window, WM_COMMAND, MAKEWPARAM(IDOK, BN_CLICKED),
                reinterpret_cast<LPARAM>(GetDlgItem(window, IDOK)));
        } else {
            PostMessageW(window, WM_COMMAND, MAKEWPARAM(IDCANCEL, BN_CLICKED),
                reinterpret_cast<LPARAM>(GetDlgItem(window, IDCANCEL)));
        }
        return TRUE;
    }
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
    if (std::wstring_view(window_class) == L"PulseFileOperationWindow") {
        if (watch.operation_titles.insert(window_title).second) {
            const int length = WideCharToMultiByte(CP_UTF8, 0, window_title, -1, nullptr, 0, nullptr, nullptr);
            std::string title(static_cast<size_t>((std::max)(1, length)), '\0');
            WideCharToMultiByte(CP_UTF8, 0, window_title, -1, title.data(), length, nullptr, nullptr);
            std::cout << "[INFO] archive operation title=" << title.c_str() << '\n';
        }
        if (std::wstring_view(window_title) == pulse::l10n::Get(pulse::l10n::StringId::ArchivePasswordOrDataError)) {
            watch.password_error_seen.store(true);
            PostMessageW(window, WM_KEYDOWN, VK_ESCAPE, 0);
        } else if (std::wstring_view(window_title) == pulse::l10n::Get(pulse::l10n::StringId::ArchiveExtractFailed)) {
            watch.seen.store(true);
            PostMessageW(window, WM_KEYDOWN, VK_ESCAPE, 0);
        }
        return TRUE;
    }
    watch.seen.store(true);
    std::wcout << L"[INFO] unexpected owned modal class=" << window_class << L" title=" << window_title << L'\n';
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

int LifecycleTests(const std::wstring& external = {}) {
    using namespace pulse;
    int failures = 0;
    const auto check = [&](bool ok, const char* label) {
        std::cout << (ok ? "[PASS] " : "[FAIL] ") << label << '\n';
        failures += !ok;
    };
    const auto fixture = stdfs::absolute(stdfs::path(L"bench_data") /
        (L"archive-lifecycle-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64())));
    std::error_code error;
    stdfs::create_directories(fixture / L"source", error);
    if (external.empty()) {
        std::ofstream source(fixture / L"source" / L"compressed.bin", std::ios::binary);
        const std::string block(1024 * 1024, 'x');
        for (int i = 0; i < 16; ++i) source.write(block.data(), block.size());
    }
    const auto archive = external.empty() ? fixture / L"compressed.7z" : stdfs::path(external);
    WIN32_FILE_ATTRIBUTE_DATA before{};
    const bool packed = !error && (external.empty() ? MakeEncrypted7z(archive, fixture / L"source")
        : GetFileAttributesExW(archive.c_str(), GetFileExInfoStandard, &before) != FALSE);
    check(packed && (!external.empty() || stdfs::file_size(archive, error) < 16 * 1024),
        external.empty() ? "create a real 7z with more than 1000:1 compression for metadata-only navigation"
            : "open the original archive for a read-only navigation probe");
    auto owned = std::make_unique<AppState>();
    auto& state = *owned;
    state.isolatedTest = true;
    state.appPrefs.persist = state.places.persist = state.searchHistory.persist = state.ctxMenuPrefs.persist = false;
    Probe probe; probe.state = &state;
    WNDCLASSW wc{};
    wc.hInstance = GetModuleHandleW(nullptr); wc.lpfnWndProc = WindowProc;
    wc.lpszClassName = L"PulseArchiveLifecycleFixture";
    RegisterClassW(&wc);
    state.hwnd = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0,
        HWND_MESSAGE, nullptr, wc.hInstance, &state);
    if (state.hwnd) SetPropW(state.hwnd, kProbeProperty, &probe);
    state.window_tabs.NewTab(fixture.wstring());
    state.pane = state.window_tabs.Active()->FocusedPane();
    auto& tab = *ActiveTab(state);
    const bool started = state.hwnd && state.worker.Start([](app::WorkResult) {});
    check(started, "initialize isolated archive navigation worker and result window");
    if (started && packed) {
        const auto started_at = GetTickCount64();
        StartLoadingPath(state, tab, ArchiveViewPath(archive.wstring()));
        const bool listed = Wait([&] { return !tab.loading; }, 30000);
        check(listed && tab.archive_listing && tab.EntryCount() > 0 && (!external.empty() ||
            (tab.EntryCount() == 1 && tab.EntryAt(0).name == L"compressed.bin" &&
                tab.EntryAt(0).size == 16 * 1024 * 1024)),
            "production navigation lists highly compressed contents without extracting members");
        std::cout << "[INFO] listing_ms=" << GetTickCount64() - started_at
            << " root_entries=" << tab.EntryCount() << '\n';
        if (!tab.archive_listing) std::wcerr << L"[DIAG] listing error=" << tab.banner_message << L'\n';
        check(!stdfs::exists(fixture / L"compressed.bin"), "listing creates no extracted member beside the archive");
        const auto cancelled = tab.archive_cancel;
        tab.archive_password = L"cached-password";
        StartLoadingPath(state, tab, fixture.wstring());
        check(cancelled && cancelled->load() && !tab.archive_cancel && !tab.archive_listing &&
            tab.archive_file.empty() && tab.archive_prefix.empty() && tab.archive_password.empty() &&
            tab.archive_encrypted.empty(), "leaving an archive cancels its read and clears all archive-only state");
        tab.HoldEntryRename(L"before.txt", L"after.txt");
        check(!tab.held_renames.empty(), "returning to a filesystem folder restores ordinary rename tracking");
        const auto delivered = probe.delivered;
        StartLoadingPath(state, tab, ArchiveViewPath(archive.wstring()));
        const auto in_flight = tab.archive_cancel;
        Sleep(20);
        const auto cancelled_at = GetTickCount64();
        StartLoadingPath(state, tab, fixture.wstring());
        auto available = std::make_shared<std::atomic_bool>(false);
        const bool enqueued = state.worker.EnqueueIo([available] { available->store(true); });
        check(in_flight && in_flight->load() && enqueued &&
            Wait([&] { return available->load() && probe.delivered > delivered; }, 3000),
            "leaving during listing cancels the read while subsequent worker tasks remain available");
        std::cout << "[INFO] cancellation_followup_ms=" << GetTickCount64() - cancelled_at << '\n';
        const auto closing = std::make_shared<std::atomic_bool>(false);
        state.window_tabs.NewTab(ArchiveViewPath(archive.wstring()));
        state.window_tabs.Active()->FocusedPane()->view.archive_cancel = closing;
        state.window_tabs.CloseTab(state.window_tabs.active);
        check(closing->load(), "closing an archive tab cancels its outstanding listing");
        const auto replacing = std::make_shared<std::atomic_bool>(false);
        app::Pane pane;
        pane.view.archive_cancel = replacing;
        pane.NewTab(fixture.wstring());
        check(replacing->load() && !pane.view.archive_cancel,
            "replacing a pane view cancels the previous archive listing");
        const auto removing = std::make_shared<std::atomic_bool>(false);
        {
            app::Pane removed;
            removed.view.archive_cancel = removing;
        }
        check(removing->load(), "removing an archive pane cancels its outstanding listing");
    }
    state.worker.Stop();
    Pump();
    if (state.hwnd) { DestroyWindow(state.hwnd); state.hwnd = nullptr; }
    owned.reset();
    if (!external.empty() && packed) {
        WIN32_FILE_ATTRIBUTE_DATA after{};
        check(GetFileAttributesExW(archive.c_str(), GetFileExInfoStandard, &after) && SameIdentity(before, after),
            "original archive size and modification time remain unchanged");
    }
    const auto parent = stdfs::absolute(L"bench_data").lexically_normal();
    if (fixture.lexically_normal().parent_path() == parent) stdfs::remove_all(fixture, error);
    check(!error && !stdfs::exists(fixture), "remove only isolated archive lifecycle fixtures");
    return failures ? 1 : 0;
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
    const bool lifecycle_only = argc >= 2 && std::wstring_view(argv[1]) == L"--lifecycle-only";
    if (argc > (lifecycle_only ? 3 : 2)) {
        std::cerr << "usage: pulse_archive_navigation_test [read-only archive] | --lifecycle-only [read-only archive]\n";
        return 2;
    }
    const HRESULT ole = OleInitialize(nullptr);
    if (!check(SUCCEEDED(ole), "initialize fixture UI apartment")) return 1;
    l10n::Initialize(GetModuleHandleW(nullptr), L"zh-CN");
    if (lifecycle_only) {
        const int result = LifecycleTests(argc == 3 ? argv[2] : L"");
        OleUninitialize();
        return result;
    }
    std::error_code error;
    const auto fixture = stdfs::absolute(stdfs::path(L"bench_data") /
        (L"archive-navigation-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64())));
    stdfs::create_directories(fixture, error);
    struct RestoreTemporaryEnvironment {
        std::wstring tmp, temp;
        RestoreTemporaryEnvironment() {
            wchar_t value[32768]{};
            if (GetEnvironmentVariableW(L"TMP", value, ARRAYSIZE(value))) tmp = value;
            if (GetEnvironmentVariableW(L"TEMP", value, ARRAYSIZE(value))) temp = value;
        }
        ~RestoreTemporaryEnvironment() {
            SetEnvironmentVariableW(L"TMP", tmp.empty() ? nullptr : tmp.c_str());
            SetEnvironmentVariableW(L"TEMP", temp.empty() ? nullptr : temp.c_str());
        }
    } restore_temporary_environment;
    const auto temporary = fixture / L"temp";
    stdfs::create_directories(temporary, error);
    SetEnvironmentVariableW(L"TMP", temporary.c_str());
    SetEnvironmentVariableW(L"TEMP", temporary.c_str());
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
        -30000, -30000, 1100, 720, nullptr, nullptr, wc.hInstance, &state);
    if (state.hwnd) SetPropW(state.hwnd, kProbeProperty, &probe);
    DialogWatch watch; watch.owner = state.hwnd;
    watch.password_inputs = {L"wrong-password", L"secret"};
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
            if (!reference->entries.empty()) {
                const auto member = reference->entries.front().path;
                tab.archive_encrypted.insert(member);
                tab.archive_password = L"wrong-password";
                ops::ArchiveOperationResult wrong_password;
                wrong_password.outcome = ops::ArchiveOutcome::Failed;
                wrong_password.exit_code = 2;
                wrong_password.password_state = ops::ArchivePasswordState::Required;
                UpdateArchiveOpenPassword(tab, tab.current_path, tab.applied_generation,
                    archive.wstring(), tab.archive_password, wrong_password);
                check(tab.archive_password.empty() && tab.archive_encrypted.contains(member),
                    "failed encrypted member open clears the cached password while retaining encryption evidence");
                check(tab.archive_encrypted.contains(member) && tab.archive_password.empty(),
                    "the next double-click again enters the password prompt path");
                const auto failed_view = tab.current_path;
                tab.current_path = fixture.wstring();
                tab.archive_password = L"newer-tab-password";
                UpdateArchiveOpenPassword(tab, failed_view, tab.applied_generation,
                    archive.wstring(), L"wrong-password", wrong_password);
                check(tab.archive_password == L"newer-tab-password",
                    "late extraction failure cannot clear a password after the tab switches views");
                tab.current_path = failed_view;
                tab.archive_password.clear();
                ops::ArchiveOperationResult success;
                success.outcome = ops::ArchiveOutcome::Success;
                UpdateArchiveOpenPassword(tab, tab.current_path, tab.applied_generation,
                    archive.wstring(), L"verified-password", success);
                check(tab.archive_password == L"verified-password",
                    "archive open caches the supplied password only after successful extraction");
                tab.archive_password.clear();
                tab.archive_encrypted.erase(member);
            }
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

            const auto password_source = fixture / L"password-source";
            const auto password_archive = fixture / L"encrypted.7z";
            stdfs::create_directories(password_source, error);
            std::ofstream(password_source / L"secret.noapp", std::ios::binary) << "encrypted member payload";
            const bool packed = !error && MakeEncrypted7z(password_archive, password_source);
            check(packed, "create an actual password-protected 7z archive for the double-click flow");
            if (packed) {
                tab.archive_password.clear();
                tab.current_path = ArchiveViewPath(password_archive.wstring());
                LoadArchiveView(state, tab);
                const bool loaded = Wait([&] { return !tab.loading; });
                const auto member_name = L"secret.noapp";
                check(loaded && tab.archive_listing && tab.archive_encrypted.contains(member_name),
                    "real 7z listing reports the selected member as encrypted");
                if (loaded && tab.archive_listing && tab.snapshot && tab.archive_encrypted.contains(member_name)) {
                    auto found = std::find_if(tab.snapshot->begin(), tab.snapshot->end(), [&](const auto& entry) {
                        return entry.name == member_name;
                    });
                    check(found != tab.snapshot->end(), "encrypted member appears in the navigable snapshot");
                    if (found != tab.snapshot->end()) {
                        auto extraction = std::make_shared<ops::ArchiveOperationResult>();
                        auto extraction_done = std::make_shared<std::atomic_bool>(false);
                        const auto probe_destination = (fixture / L"password-probe").wstring();
                        const bool probe_queued = state.worker.EnqueueIo([extraction, extraction_done,
                            password_archive, probe_destination, member_name] {
                            *extraction = ops::ExtractArchive(password_archive.wstring(), probe_destination,
                                {member_name}, L"wrong-password");
                            extraction_done->store(true);
                        });
                        check(probe_queued && Wait([&] { return extraction_done->load(); }) &&
                            app::IsArchivePasswordFailure(*extraction),
                            "real encrypted member extraction reports wrong password with 7z exit 2");
                        watch.password_error_seen.store(false);
                        tab.SelectOnly(static_cast<int>(found - tab.snapshot->begin()));
                        const bool first_open = OpenArchiveSelection(state);
                        check(first_open && watch.password_prompts.load() == 1 && tab.archive_password.empty(),
                            "wrong password fails the first double-click without caching it");
                        check(watch.password_error_seen.load(),
                            "wrong encrypted password presents and closes the localized failure window");

                        tab.SelectOnly(static_cast<int>(found - tab.snapshot->begin()));
                        const bool second_open = OpenArchiveSelection(state);
                        check(second_open && watch.password_prompts.load() == 2,
                            "a second double-click opens the password dialog again");
                        check(tab.archive_password == L"secret",
                            "correct password is cached only after the second extraction succeeds");
                    }
                }
            }
        }
        if (tab.archive_cancel) tab.archive_cancel->store(true);
        state.worker.Stop();
        Pump();
        for (const auto payload : probe.held) DiscardArchiveResult(payload);
        probe.held.clear();
        check(!watch.seen.load(), "navigation and password retries displayed no unexpected modal dialog");
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
