#include "../app/app_internal.h"
#include "../app/app_exit.h"
#include "../app/app_input.h"
#include "../app/tab_transfer.h"
#include "../common/localization.h"
#include "../common/user_storage.h"
#include "../common/windows_compat.h"
#include <filesystem>
#include <fstream>
#include <functional>
#include <cstdio>
#include <algorithm>

LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);

namespace {
using namespace pulse;
int failures = 0;
void Check(bool passed, const char* label) {
    std::printf("[%s] %s\n", passed ? "PASS" : "FAIL", label);
    failures += !passed;
}
void Pump() {
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        if (message.message == WM_QUIT) continue;
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}
bool Wait(const std::function<bool()>& done) {
    const auto deadline = GetTickCount64() + 10000;
    while (!done() && GetTickCount64() < deadline) { Pump(); Sleep(10); }
    return done();
}
bool Forward(HWND hwnd, const std::wstring& path) {
    COPYDATASTRUCT data{};
    data.dwData = pulse::app::SingleInstanceCoordinator::OpenPathMessageId();
    data.cbData = static_cast<DWORD>((path.size() + 1) * sizeof(wchar_t));
    data.lpData = const_cast<wchar_t*>(path.c_str());
    return SendMessageW(hwnd, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&data)) != 0;
}
POINT FindDropPoint(AppState& state, const std::function<bool(const ui::HitTestResult&)>& accepts) {
    const auto vm = BuildVm(state, false);
    const D2D1_RECT_F bounds = D2D1::RectF(0, 0,
        static_cast<float>(state.compositor.Width()),
        static_cast<float>(state.compositor.Height()));
    for (LONG y = 2; y < static_cast<LONG>(state.compositor.Height()); y += 6) {
        for (LONG x = 2; x < static_cast<LONG>(state.compositor.Width()); x += 6) {
            const auto hit = state.renderer.HitTest(vm, bounds,
                static_cast<float>(x), static_cast<float>(y));
            if (!accepts(hit)) continue;
            POINT point{x, y};
            ClientToScreen(state.hwnd, &point);
            return point;
        }
    }
    return POINT{-2147483647L, -2147483647L};
}
bool IsContentDrop(const ui::HitTestResult& hit) {
    return hit.region == ui::HitTestResult::Row || hit.region == ui::HitTestResult::Pane ||
        hit.region == ui::HitTestResult::PaneEmptyNewFolder;
}
app::LayoutTab* AddLoadedTab(AppState& state, const std::wstring& path) {
    auto& layout = state.window_tabs.NewTab(path);
    BindCurrentLayout(state);
    if (state.pane) StartLoadingPath(state, state.pane->view, path, PathLoadReason::Navigate);
    return &layout;
}
}

int main() {
    using namespace pulse;
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    compat::EnableDpiAwareness();
    if (FAILED(OleInitialize(nullptr))) return 2;
    const auto root = std::filesystem::absolute(L"bench_data/release-1.1.3.0/multiwindow");
    std::filesystem::create_directories(root / L"first");
    std::filesystem::create_directories(root / L"second");
    std::ofstream(root / L"second" / L"reveal.txt") << "folder reveal fixture";
    storage::OverrideDefaultRootForTesting((root / L"profile").wstring());
    l10n::Initialize(GetModuleHandleW(nullptr), L"zh-CN");
    SetEnvironmentVariableW(L"PULSE_TEST_HIDDEN_SHOT", L"1");
    WNDCLASSW wc{};
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpfnWndProc = WndProc;
    wc.lpszClassName = app::SingleInstanceCoordinator::WindowClassName();
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
    AppState state;
    state.isolatedTest = true;
    state.open_path = (root / L"first").wstring();
    HWND primary = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_NOREDIRECTIONBITMAP,
        wc.lpszClassName, L"Pulse fixture", WS_OVERLAPPEDWINDOW & ~WS_CAPTION,
        -20000, -20000, 1080, 760, nullptr, nullptr, wc.hInstance, &state);
    Check(primary && Wait([&] { const auto* tab = ActiveTab(state); return tab && !tab->loading; }),
        "production window initializes with an isolated directory and configuration");
    if (!primary) { OleUninitialize(); return 1; }

    BYTE saved_keyboard[256]{};
    BYTE drag_keyboard[256]{};
    const bool keyboard_saved = GetKeyboardState(saved_keyboard) != FALSE;
    std::copy(std::begin(saved_keyboard), std::end(saved_keyboard), std::begin(drag_keyboard));
    drag_keyboard[VK_LBUTTON] |= 0x80;
    const bool keyboard_set = keyboard_saved && SetKeyboardState(drag_keyboard);
    state.tabDragPending = true;
    state.tabDragIndex = 0;
    state.tabDragStartPt = POINT{100, 20};
    if (keyboard_set) HandleMouseMove(&state, primary, WM_MOUSEMOVE, MK_LBUTTON,
        MAKELPARAM(100, 20 + GetSystemMetrics(SM_CYDRAG) + 2));
    Check(keyboard_set && state.tabDragging && state.tabOrder.size() == 1,
        "a single tab starts a vertical drag without moving the physical mouse");
    if (keyboard_saved) SetKeyboardState(saved_keyboard);
    HandleCaptureChanged(&state, primary, WM_CAPTURECHANGED, 0, 0);
    Check(!state.tabDragging && !state.tabDragPending,
        "losing capture clears the single-tab drag state");

    state.appPrefs.external_open_mode = app::ExternalOpenMode::Tab;
    const size_t before = state.window_tabs.items.size();
    Check(Forward(primary, (root / L"second").wstring()) &&
        Wait([&] { return state.window_tabs.items.size() == before + 1; }) && state.core.windows.Count() == 1,
        "external tab mode opens another tab in the existing window");
    Wait([&] { return ActiveTab(state) && !ActiveTab(state)->loading; });
    const size_t after_first_open = state.window_tabs.items.size();
    Check(Forward(primary, (root / L"second").wstring()) &&
        Wait([&] { return state.window_tabs.items.size() == after_first_open + 1; }),
        "explicit tab mode creates a new tab even when the folder is already open");
    Wait([&] { return ActiveTab(state) && !ActiveTab(state)->loading; });
    state.appPrefs.external_open_mode = app::ExternalOpenMode::ReuseOpenedLocation;
    const size_t with_second = state.window_tabs.items.size();
    Check(Forward(primary, (root / L"first").wstring()) &&
        Wait([&] { return ActiveTab(state)->current_path == fs::NormalizePath((root / L"first").wstring()); }) &&
        state.window_tabs.items.size() == with_second,
        "reuse mode activates the existing tab without adding a duplicate");

    state.appPrefs.external_open_mode = app::ExternalOpenMode::NewWindow;
    Check(Forward(primary, (root / L"second" / L"reveal.txt").wstring()) &&
        Wait([&] { return state.core.windows.Count() == 2; }),
        "external new-window mode creates a second real Pulse window");
    AppState* secondary = state.core.windows.MostRecentExcept(primary);
    if (secondary) {
        ShowWindow(secondary->hwnd, SW_HIDE);
        Wait([&] { return ActiveTab(*secondary) && !ActiveTab(*secondary)->loading; });
        Check(Wait([&] {
            const auto* tab = ActiveTab(*secondary);
            return tab && tab->selected_index >= 0 &&
                tab->EntryAt(static_cast<size_t>(tab->selected_index)).name == L"reveal.txt";
        }), "opening a file location in a new window selects the requested file");
        ui::ComPtr<ID2D1Device> first_device, second_device;
        state.compositor.Dc()->GetDevice(&first_device);
        secondary->compositor.Dc()->GetDevice(&second_device);
        Check(state.compositor.Dc() != secondary->compositor.Dc() &&
            first_device.get() == second_device.get() &&
            &state.renderer.Caches() == &secondary->renderer.Caches() &&
            state.renderer.SharedThumbnailCache() != secondary->renderer.SharedThumbnailCache(),
            "real windows share the graphics device and icons while keeping thumbnail cancellation independent");
        Check(&state.appPrefs == &secondary->appPrefs && &state.places == &secondary->places &&
            &state.ctxMenuPrefs == &secondary->ctxMenuPrefs && &state.tray == &secondary->tray &&
            &state.searchHistory == &secondary->searchHistory &&
            &state.searchHistoryWriter == &secondary->searchHistoryWriter &&
            &state.savedSearches == &secondary->savedSearches,
            "all windows edit the same preferences, places and staging tray");
        secondary->appPrefs.external_open_mode = app::ExternalOpenMode::ReuseOpenedLocation;
        const size_t count = secondary->window_tabs.items.size();
        Check(Forward(secondary->hwnd, (root / L"first").wstring()) &&
            Wait([&] { return state.core.windows.MostRecent() == &state; }) && secondary->window_tabs.items.size() == count &&
            state.window_tabs.items.size() == with_second,
            "an outside open can activate a matching tab in another window");

        std::vector<AppState*> extra_windows;
        state.appPrefs.external_open_mode = app::ExternalOpenMode::NewWindow;
        for (int i = 0; i < 3; ++i) {
            const auto expected = state.core.windows.Count() + 1;
            const bool accepted = Forward(primary, (root / L"second").wstring());
            const bool created = Wait([&] { return state.core.windows.Count() == expected; });
            AppState* extra = created ? state.core.windows.MostRecent() : nullptr;
            Check(accepted && created && extra && Wait([&] {
                const auto* tab = ActiveTab(*extra);
                return tab && !tab->loading && tab->snapshot && tab->EntryCount() != 0;
            }), "each of five simultaneous windows can finish its directory listing");
            if (extra && extra != &state && extra != secondary) {
                ShowWindow(extra->hwnd, SW_HIDE);
                extra_windows.push_back(extra);
            }
        }
        for (AppState* extra : extra_windows) RequestWindowClose(*extra);
        Check(Wait([&] { return state.core.windows.Count() == 2; }),
            "closing extra windows releases their workers while the first pair remains usable");
        app::WindowSessionSnapshot saved_window;
        saved_window.window_rect = {60, 60, 960, 760};
        saved_window.active_path = (root / L"second").wstring();
        saved_window.layout_tabs.push_back(app::CaptureLayoutTab(*state.window_tabs.items.front()));
        saved_window.layout_tabs.push_back(app::CaptureLayoutTab(*secondary->window_tabs.items.front()));
        saved_window.active_layout_tab = 1;
        saved_window.sidebar_hidden = 4;
        ShowWindow(primary, SW_HIDE);
        auto restored_target = std::make_unique<ResolvedOpenTarget>();
        restored_target->folder = saved_window.active_path;
        auto* restored = reinterpret_cast<AppState*>(SendMessageW(primary, WM_NEW_WINDOW,
            reinterpret_cast<WPARAM>(&saved_window), reinterpret_cast<LPARAM>(restored_target.release())));
        RECT restored_rect{};
        if (restored) GetWindowRect(restored->hwnd, &restored_rect);
        Check(restored && restored->window_tabs.items.size() == 2 &&
            restored->window_tabs.active == 1 && restored->sidebarHiddenMask == 4 &&
            restored_rect.right - restored_rect.left == 900 &&
            restored_rect.bottom - restored_rect.top == 700 && !IsWindowVisible(restored->hwnd) &&
            Wait([&] { return ActiveTab(*restored) && !ActiveTab(*restored)->loading; }),
            "the window factory restores tabs, active layout, sidebar and geometry from its session");
        if (restored) RequestWindowClose(*restored);
        Check(Wait([&] { return state.core.windows.Count() == 2; }),
            "a restored window closes independently");
        const size_t source_tabs = state.window_tabs.items.size();
        const size_t destination_tabs = secondary->window_tabs.items.size();
        app::LayoutTab* moved_layout = state.window_tabs.items.back().get();
        Check(TransferLayoutTab(state, source_tabs - 1, *secondary) &&
            state.window_tabs.items.size() == source_tabs - 1 &&
            secondary->window_tabs.items.size() == destination_tabs + 1 &&
            secondary->window_tabs.Active() == moved_layout &&
            Wait([&] { return ActiveTab(*secondary) && !ActiveTab(*secondary)->loading; }),
            "a layout tab moves between real windows and resumes loading in its destination");
        ApplyLayoutPreset(state, app::LayoutPreset::TwoVertical);
        const size_t source_panes = Panes(state).size();
        const size_t destination_panes = Panes(*secondary).size();
        app::Pane* moved_pane = Panes(state).back().get();
        Check(TransferPane(state, source_panes - 1, *secondary) &&
            Panes(state).size() == source_panes - 1 &&
            Panes(*secondary).size() == destination_panes + 1 &&
            secondary->pane == moved_pane &&
            Wait([&] { return !moved_pane->view.loading; }),
            "one split pane moves independently without moving the other source pane");

        SetWindowPos(primary, nullptr, -20000, -20000, 1080, 760,
            SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        SetWindowPos(secondary->hwnd, nullptr, -17000, -20000, 1080, 760,
            SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        AddLoadedTab(*secondary, (root / L"first").wstring());
        app::LayoutTab* split_tab = AddLoadedTab(state, (root / L"first").wstring());
        ApplyLayoutPreset(state, app::LayoutPreset::TwoVertical);
        Check(Wait([&] {
            return std::all_of(split_tab->panes.begin(), split_tab->panes.end(),
                [](const auto& pane) { return pane && !pane->view.loading; });
        }), "prepare a loaded two-pane layout tab for content-area drag routing");
        std::vector<app::Pane*> split_pointers;
        for (const auto& pane : Panes(state)) split_pointers.push_back(pane.get());
        const size_t before_whole_drop = Panes(*secondary).size();
        const size_t source_tabs_before_whole_drop = state.window_tabs.items.size();
        const POINT content_drop = FindDropPoint(*secondary, IsContentDrop);
        const size_t split_index = static_cast<size_t>(std::find_if(state.window_tabs.items.begin(),
            state.window_tabs.items.end(), [&](const auto& tab) { return tab.get() == split_tab; }) -
            state.window_tabs.items.begin());
        Check(content_drop.x != -2147483647L &&
            TryTransferLayoutTabAtScreenPoint(state, split_index, content_drop) &&
            state.window_tabs.items.size() == source_tabs_before_whole_drop - 1 &&
            Panes(*secondary).size() == before_whole_drop + 2 &&
            Panes(*secondary)[before_whole_drop].get() == split_pointers[0] &&
            Panes(*secondary)[before_whole_drop + 1].get() == split_pointers[1] &&
            Wait([&] {
                return std::all_of(Panes(*secondary).begin(), Panes(*secondary).end(),
                    [](const auto& pane) { return pane && !pane->view.loading; });
            }), "dropping a multi-pane tab on content preserves every pane and joins the target split");

        app::LayoutTab* strip_tab = AddLoadedTab(state, (root / L"second").wstring());
        const size_t strip_index = static_cast<size_t>(std::find_if(state.window_tabs.items.begin(),
            state.window_tabs.items.end(), [&](const auto& tab) { return tab.get() == strip_tab; }) -
            state.window_tabs.items.begin());
        const POINT strip_drop = FindDropPoint(*secondary, [](const ui::HitTestResult& hit) {
            return hit.region == ui::HitTestResult::Tab;
        });
        const size_t destination_tabs_before_strip = secondary->window_tabs.items.size();
        Check(strip_drop.x != -2147483647L &&
            TryTransferLayoutTabAtScreenPoint(state, strip_index, strip_drop) &&
            secondary->window_tabs.items.size() == destination_tabs_before_strip + 1 &&
            secondary->window_tabs.Active()->panes.size() == 1 &&
            Wait([&] { return !ActiveTab(*secondary)->loading; }),
            "dropping a tab on the target tab strip preserves it as a separate tab");

        const size_t detached_tab_count = state.core.windows.Count() + 1;
        const size_t detach_tab_index = state.window_tabs.active;
        Check(TryTransferLayoutTabAtScreenPoint(state, detach_tab_index, POINT{30000, 30000}) &&
            Wait([&] { return state.core.windows.Count() == detached_tab_count; }),
            "dropping a tab outside all windows creates a detached top-level window");
        AppState* detached_tab_window = state.core.windows.MostRecent();
        Check(detached_tab_window && detached_tab_window != &state &&
            detached_tab_window->window_tabs.items.size() == 1 &&
            Wait([&] { return ActiveTab(*detached_tab_window) && !ActiveTab(*detached_tab_window)->loading; }),
            "the detached window contains the moved tab and finishes loading");
        if (detached_tab_window) RequestWindowClose(*detached_tab_window);
        Check(Wait([&] { return state.core.windows.Count() == 2; }),
            "a detached tab window closes without affecting the root pair");

        ApplyLayoutPreset(state, app::LayoutPreset::TwoVertical);
        const size_t detached_pane_count = state.core.windows.Count() + 1;
        const size_t detached_pane_index = Panes(state).size() - 1;
        Check(TryTransferPaneAtScreenPoint(state, state.window_tabs.active, detached_pane_index,
                POINT{30000, 30000}) &&
            Wait([&] { return state.core.windows.Count() == detached_pane_count; }),
            "dropping a pane outside all windows creates a detached top-level window");
        AppState* detached_pane_window = state.core.windows.MostRecent();
        Check(detached_pane_window && detached_pane_window != &state &&
            Panes(state).size() == 1 && Panes(*detached_pane_window).size() == 1 &&
            Wait([&] { return !ActiveTab(*detached_pane_window)->loading; }),
            "pane detachment replaces the new window starter and leaves one source pane");
        if (detached_pane_window) RequestWindowClose(*detached_pane_window);
        Check(Wait([&] { return state.core.windows.Count() == 2; }),
            "a detached pane window closes without affecting the root pair");

        auto merge_target = std::make_unique<ResolvedOpenTarget>();
        merge_target->folder = (root / L"first").wstring();
        auto* merge_source = reinterpret_cast<AppState*>(SendMessageW(primary, WM_NEW_WINDOW, 0,
            reinterpret_cast<LPARAM>(merge_target.release())));
        if (merge_source) {
            ShowWindow(merge_source->hwnd, SW_HIDE);
            app::LayoutTab* merge_pinned = AddLoadedTab(*merge_source, (root / L"first").wstring());
            merge_pinned->pinned = true;
            app::LayoutTab* merge_unpinned = AddLoadedTab(*merge_source, (root / L"second").wstring());
            const auto moved_merge_tabs = std::vector<app::LayoutTab*>{
                merge_source->window_tabs.items[0].get(), merge_pinned, merge_unpinned};
            if (!merge_source->window_tabs.items.empty()) {
                auto* target_first = secondary->window_tabs.items.front().get();
                target_first->pinned = true;
                const size_t old_target_count = secondary->window_tabs.items.size();
                const size_t expected_windows = state.core.windows.Count() - 1;
                POINT title_drop{100, 8};
                ClientToScreen(secondary->hwnd, &title_drop);
                Check(TryMergeWindowAtScreenPoint(*merge_source, title_drop) &&
                    secondary->window_tabs.items.size() == old_target_count + moved_merge_tabs.size() &&
                    std::all_of(moved_merge_tabs.begin(), moved_merge_tabs.end(), [&](auto* tab) {
                        return std::any_of(secondary->window_tabs.items.begin(), secondary->window_tabs.items.end(),
                            [&](const auto& candidate) { return candidate.get() == tab; });
                    }) && std::is_partitioned(secondary->window_tabs.items.begin(),
                        secondary->window_tabs.items.end(), [](const auto& tab) { return tab->pinned; }) &&
                    Wait([&] { return state.core.windows.Count() == expected_windows; }),
                    "dropping a window title bar merges every tab and keeps pinned tabs before unpinned tabs");
            } else {
                Check(false, "title-bar merge source has tabs");
                RequestWindowClose(*merge_source);
            }
        } else {
            Check(false, "title-bar merge source window is created");
        }

        OpenSettingsTab(*secondary, 0);
        secondary->shot.active = true;
        for (const auto* language : {L"zh-CN", L"en-US"}) {
            l10n::SetLanguage(language);
            for (bool narrow : {false, true}) {
                SetThemeMode(*secondary, narrow ? 2 : 1);
                Check(state.darkMode == narrow && secondary->darkMode == narrow,
                    "theme changes update every live window");
                const int width = narrow ? 720 : 1180;
                SetWindowPos(secondary->hwnd, nullptr, 0, 0, width, 820,
                    SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
                secondary->compositor.Resize(width, 820);
                const auto vm = BuildVm(*secondary, false);
                const auto offset = secondary->renderer.SettingsDestinationOffset(vm,
                    static_cast<int>(l10n::StringId::SettingsExternalOpen),
                    static_cast<float>(width), 820);
                secondary->settings.SetScroll(offset, secondary->renderer.SettingsMaxScroll(vm,
                    static_cast<float>(width), 820));
                Render(*secondary);
                const auto image = root / (std::wstring(L"settings-") + language +
                    (narrow ? L"-narrow.png" : L"-wide.png"));
                Check(secondary->compositor.SaveSnapshot(image.c_str()),
                    "save a rendered external-open setting in the actual window");
            }
        }
        secondary->shot.active = false;
        RequestWindowClose(state);
        Check(Wait([&] { return !IsWindow(primary) && state.core.windows.Count() == 1; }),
            "closing the first window keeps the second window and process alive");
        secondary->appPrefs.external_open_mode = app::ExternalOpenMode::Tab;
        const size_t surviving_tabs = secondary->window_tabs.items.size();
        Check(Forward(secondary->hwnd, (root / L"first").wstring()) &&
            Wait([&] { return secondary->window_tabs.items.size() == surviving_tabs + 1; }),
            "the surviving window still accepts outside folder opens");
        RequestWindowClose(*secondary);
        Check(Wait([&] { return state.core.windows.Count() == 0; }),
            "closing the final window completes its asynchronous shutdown");
    } else {
        Check(false, "second window is registered");
        RequestWindowClose(state);
        Wait([&] { return state.core.windows.Count() == 0; });
    }
    Pump();
    SetEnvironmentVariableW(L"PULSE_TEST_HIDDEN_SHOT", nullptr);
    std::printf("multiwindow application: %d failures\n", failures);
    OleUninitialize();
    return failures ? 1 : 0;
}
