// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "../app/app_internal.h"
#include "../app/app_ui_schedule.h"
#include "../app/archive_navigation.h"
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>

namespace {
LRESULT CALLBACK ArchivePresentationProc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
    auto* state = reinterpret_cast<pulse::AppState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == pulse::WM_ARCHIVE_RESULT && state) {
        pulse::HandleArchiveResult(*state, lp);
        return 0;
    }
    return DefWindowProcW(hwnd, message, wp, lp);
}

bool Wait(const std::function<bool()>& done) {
    const auto deadline = GetTickCount64() + 10000;
    do {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (done()) return true;
        Sleep(2);
    } while (GetTickCount64() < deadline);
    return done();
}

bool MakeZip(const std::filesystem::path& archive, const std::filesystem::path& source) {
    wchar_t module[32768]{};
    GetModuleFileNameW(nullptr, module, ARRAYSIZE(module));
    const auto executable = std::filesystem::path(module).parent_path() / L"7zip/7z.exe";
    std::wstring command = L"\"" + executable.wstring() + L"\" a -tzip -mx=0 -y -bd \"" +
        archive.wstring() + L"\" .\\*";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE,
        CREATE_NO_WINDOW, nullptr, source.c_str(), &startup, &process)) return false;
    const bool exited = WaitForSingleObject(process.hProcess, 10000) == WAIT_OBJECT_0;
    DWORD code = 2;
    if (exited) GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return exited && code == 0;
}
}

int RunArchivePresentationUiTest() {
    using namespace pulse;
    int checks = 0, failures = 0;
    auto check = [&](bool ok, const char* label) {
        ++checks;
        failures += !ok;
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
        return ok;
    };
    const auto output = std::filesystem::absolute(L"bench_data/archive-presentation-ui");
    const auto fixture = output / (L"fixture-" + std::to_wstring(GetCurrentProcessId()));
    const auto source = fixture / L"source";
    const std::wstring long_file = L"A_" + std::wstring(64, L'长') + L"_完整报告.txt";
    const std::wstring long_folder = L"B_" + std::wstring(64, L'目') + L"_目录";
    std::filesystem::create_directories(source / long_folder);
    std::ofstream(source / long_file) << "archive presentation fixture";
    std::ofstream(source / L"z.txt") << "short file";
    std::ofstream(source / long_folder / L"child.txt") << "nested fixture";
    const auto archive = fixture / L"presentation.zip";
    if (!check(MakeZip(archive, source), "create isolated ZIP with long file, long folder and trailing short file")) return 1;
    auto owned = std::make_unique<AppState>();
    auto& s = *owned;
    s.isolatedTest = true;
    s.appPrefs.persist = s.places.persist = s.ctxMenuPrefs.persist = s.searchHistory.persist = false;
    s.accentColor = ui::HexColor(0x0078D4);
    WNDCLASSW cls{};
    cls.hInstance = GetModuleHandleW(nullptr);
    cls.lpszClassName = L"PulseArchivePresentationFixture";
    cls.lpfnWndProc = ArchivePresentationProc;
    RegisterClassW(&cls);
    s.hwnd = CreateWindowExW(WS_EX_NOACTIVATE, cls.lpszClassName, L"Archive presentation fixture",
        WS_OVERLAPPEDWINDOW, -30000, -30000, 1100, 720, nullptr, nullptr, cls.hInstance, nullptr);
    if (!check(s.hwnd && s.compositor.Init(s.hwnd), "initialize isolated production renderer")) return 1;
    SetWindowLongPtrW(s.hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&s));
    s.renderer.SetCompositor(&s.compositor);
    s.renderer.SetDetailsPanelVisible(false);
    s.compositor.RecreateTextFormats(1);
    ShowWindow(s.hwnd, SW_SHOWNOACTIVATE);
    SetEnvironmentVariableW(L"PULSE_TEST_HIDDEN_SHOT", L"1");
    s.window_tabs.NewTab(fixture.wstring());
    s.pane = s.window_tabs.Active()->FocusedPane();
    auto& tab = *ActiveTab(s);
    s.worker.Start([](app::WorkResult) {});
    const auto archive_view = ArchiveViewPath(archive.wstring());
    for (const auto inherited : {ui::ViewMode::MediumIcons, ui::ViewMode::ExtraLargeIcons, ui::ViewMode::List}) {
        NavigateTo(s, fixture.wstring());
        SetViewMode(s, inherited);
        tab.scroll_x = 73;
        NavigateTo(s, archive_view);
        check(tab.view_mode == ui::ViewMode::Details && tab.scroll_x == 0,
            "opening ZIP from another folder starts in default Details instead of inheriting the parent mode or scroll");
        if (!check(Wait([&] { return !tab.loading; }) && tab.snapshot && tab.EntryCount() == 3,
            "production archive navigation lists all three ZIP entries")) break;
    }
    auto index_of = [&](const std::wstring& name) {
        for (size_t i = 0; i < tab.EntryCount(); ++i)
            if (tab.EntryAt(i).name == name) return static_cast<int>(i);
        return -1;
    };
    const int file_index = index_of(long_file), folder_index = index_of(long_folder), short_index = index_of(L"z.txt");
    if (check(file_index >= 0 && folder_index >= 0 && short_index > file_index,
        "ZIP snapshot retains full names and renders the short file after the long file")) {
        SetViewMode(s, ui::ViewMode::LargeIcons);
        RefreshActiveTab(s);
        check(Wait([&] { return !tab.loading; }) && tab.view_mode == ui::ViewMode::LargeIcons,
            "refreshing an archive preserves its manually chosen icon view");
        for (const float scale : {1.0f, 1.5f}) for (const bool dark : {false, true}) {
            s.scale = scale;
            s.darkMode = dark;
            s.renderer.SetScale(scale);
            s.compositor.RecreateTextFormats(scale);
            s.compositor.Resize(static_cast<UINT>(1100 * scale), static_cast<UINT>(720 * scale));
            SetWindowPos(s.hwnd, nullptr, 0, 0, static_cast<int>(1100 * scale), static_cast<int>(720 * scale),
                SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            for (const auto mode : {ui::ViewMode::MediumIcons, ui::ViewMode::LargeIcons, ui::ViewMode::ExtraLargeIcons}) {
                SetViewMode(s, mode);
                auto draw = [&] {
                    s.hitTestVmValid = false;
                    s.compositor.Dc()->BeginDraw();
                    s.renderer.Render(BuildVm(s, false), D2D1::RectF(0, 0, 1100 * scale, 720 * scale),
                        ui::MakeTheme(dark, s.accentColor));
                    check(SUCCEEDED(s.compositor.Dc()->EndDraw()), "archive icon frame renders successfully");
                };
                draw();
                check(s.renderer.NameWasTruncated(long_file) && s.renderer.NameWasTruncated(long_folder) &&
                    !s.renderer.NameWasTruncated(L"z.txt"),
                    "pathless archive entries retain independent long-name and short-name truncation records");
                for (const int index : {file_index, folder_index, short_index}) {
                    auto vm = BuildVm(s, false);
                    const auto cell = s.renderer.ItemRectInPane(vm.pane, vm.pane_slots.front().rect, index);
                    ApplyHoverTarget(s, {});
                    HandleMouseMove(&s, s.hwnd, WM_MOUSEMOVE, 0,
                        MAKELPARAM(static_cast<int>((cell.left + cell.right) * 0.5f),
                            static_cast<int>((cell.top + cell.bottom) * 0.5f)));
                    check(s.hoverRegion == static_cast<int>(ui::HitTestResult::Row) && s.hoverControlIndex == index,
                        "production pointer handling reaches the requested archive icon");
                    s.hoverSince = GetTickCount64() - 151;
                    s.ui_schedule.tooltip_checked_since = 0;
                    RunUiDeadlines(s, GetTickCount64());
                    check(index == short_index ? s.tooltipText.empty() : s.tooltipText == tab.EntryAt(index).name,
                        "hover deadline shows complete truncated file or folder name and suppresses a fully visible short name");
                    if (index == file_index) {
                        draw();
                        const auto name = std::to_wstring(static_cast<int>(mode)) + L"-" +
                            std::to_wstring(static_cast<int>(scale * 100)) + (dark ? L"-dark.png" : L"-light.png");
                        check(s.compositor.SaveSnapshot((output / name).c_str()), "save actual archive filename popup evidence");
                    }
                }
            }
        }
        SetViewMode(s, ui::ViewMode::LargeIcons);
        tab.SelectOnly(index_of(long_folder));
        check(OpenArchiveSelection(s) && tab.view_mode == ui::ViewMode::Details &&
            Wait([&] { return !tab.loading; }) && tab.EntryCount() == 1 && tab.EntryAt(0).name == L"child.txt",
            "entering a nested archive folder starts in default Details and loads its child");
        NavigateTo(s, fixture.wstring());
        check(tab.view_mode == ui::ViewMode::List,
            "returning to the filesystem parent restores its own saved view");
        tab.view_mode = ui::ViewMode::ExtraLargeIcons;
        StartLoadingPath(s, tab, archive_view, PathLoadReason::RestoreSession);
        check(tab.view_mode == ui::ViewMode::ExtraLargeIcons && Wait([&] { return !tab.loading; }),
            "explicit session restore keeps the archive view selected in that session");
        NavigateTo(s, source.wstring());
        auto filesystem_rows = std::make_shared<std::vector<fs::DirEntry>>();
        fs::DirectoryEnumerator enumerator(source.wstring());
        fs::DirEntry row;
        while (enumerator.Next(row) == fs::DirectoryRead::Entry) filesystem_rows->push_back(row);
        tab.SetSnapshot(filesystem_rows);
        tab.loading = false;
        for (const auto mode : {ui::ViewMode::MediumIcons, ui::ViewMode::ExtraLargeIcons}) {
            SetViewMode(s, mode);
            ApplyHoverTarget(s, {});
            s.compositor.Dc()->BeginDraw();
            s.renderer.Render(BuildVm(s, false), D2D1::RectF(0, 0, 1100 * s.scale, 720 * s.scale),
                ui::MakeTheme(s.darkMode, s.accentColor));
            check(SUCCEEDED(s.compositor.Dc()->EndDraw()), "ordinary filesystem icon frame renders successfully");
            for (size_t index = 0; index < tab.EntryCount(); ++index) {
                const auto& entry = tab.EntryAt(index);
                const bool long_name = entry.name != L"z.txt";
                check(s.renderer.NameWasTruncated((source / entry.name).wstring()) == long_name,
                    "ordinary filesystem paths keep independent long-name and short-name truncation records");
                ui::HitTestResult hit;
                hit.region = ui::HitTestResult::Row;
                hit.index = static_cast<int>(index);
                ApplyHoverTarget(s, hit);
                s.hoverPaneIndex = 0;
                check(long_name ? TooltipForHover(s) == entry.name : TooltipForHover(s).empty(),
                    "ordinary filesystem icons show only the full names that were actually truncated");
            }
        }
    }
    if (tab.archive_cancel) tab.archive_cancel->store(true);
    s.worker.Stop();
    Wait([] { return true; });
    s.watches.Stop();
    SetWindowLongPtrW(s.hwnd, GWLP_USERDATA, 0);
    DestroyWindow(s.hwnd);
    s.hwnd = nullptr;
    s.renderer.SetCompositor(nullptr);
    SetEnvironmentVariableW(L"PULSE_TEST_HIDDEN_SHOT", nullptr);
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
