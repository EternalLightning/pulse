// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "app/app_internal.h"
#include "app/search_query.h"
#include <filesystem>
#include <iostream>
#include <thread>

using namespace pulse;
static int failures = 0;
static void Check(bool pass, const char* name) {
    std::cout << (pass ? "[PASS] " : "[FAIL] ") << name << std::endl;
    if (!pass) ++failures;
}
static void Drain(AppState& s) {
    for (int i = 0; i < 300 && (ActiveTab(s)->loading || ActiveTab(s)->pending_generation != 0); ++i) {
        ProcessPendingResults(s);
        if (ActiveTab(s)->snapshot && !ActiveTab(s)->loading) ClampScroll(s);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}
int main() {
    SetDllDirectoryW(L"E:\\Projects\\pulse\\build");
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const auto root = std::filesystem::absolute(std::filesystem::path(L"bench_data/issue-fixes-navigation/fixture") /
        std::to_wstring(GetCurrentProcessId())).wstring();
    std::filesystem::create_directories(root);
    for (int i = 0; i < 240; ++i) {
        wchar_t name[32]{};
        swprintf_s(name, L"folder%03d", i);
        std::filesystem::create_directories(std::filesystem::path(root) / name);
    }
    auto state = std::make_unique<AppState>();
    auto& s = *state;
    s.isolatedTest = true;
    s.places.persist = s.appPrefs.persist = s.ctxMenuPrefs.persist = s.searchHistory.persist = false;
    WNDCLASSW wc{};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"PulseNavigationReturnTest";
    RegisterClassW(&wc);
    s.hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, wc.lpszClassName, L"", WS_POPUP,
        -30000, -30000, 1000, 700, nullptr, nullptr, wc.hInstance, nullptr);
    s.hwndAddressEdit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD, 0, 0, 200, 25,
        s.hwnd, nullptr, wc.hInstance, nullptr);
    Check(s.hwnd && s.hwndAddressEdit && s.compositor.Init(s.hwnd), "isolated hidden owner and compositor");
    s.compositor.RecreateTextFormats(1.0f);
    s.renderer.SetCompositor(&s.compositor);
    s.renderer.SetScale(1.0f);
    s.window_tabs.NewTab(root);
    s.pane = s.window_tabs.Active()->panes.front().get();
    auto& tab = *ActiveTab(s);
    s.worker.Start([&](app::WorkResult result) {
        std::lock_guard lock(s.resultMutex);
        s.results.push(std::move(result));
    });
    const auto origin = tab.current_path;
    for (const bool content : {false, true}) {
        for (const bool live : {false, true}) {
            tab.current_path = origin;
            tab.NavigateTo(app::MakeSearchPath(content ? L"content:needle" : L"needle"));
            s.addressSearchContent = content;
            s.addressSearchCurrent = true;
            s.addressSearchRoot = origin;
            s.addressSearching = true;
            s.addressHistoryDue = GetTickCount64() + 500;
            tab.pending_generation = 900000123;
            SetWindowTextW(s.hwndAddressEdit, L" \t ");
            SubmitAddressSearch(s, live);
            std::cout << "CLEAR content=" << content << " live=" << live << std::endl;
            Check(tab.current_path == origin, "empty search restores original directory");
            Check(s.addressHistoryDue == 0, "empty search cancels delayed history write");
            index::ContentSearchUpdate stale;
            stale.progress.generation = 900000123;
            stale.progress.done = true;
            stale.hits.push_back({L"C:\\fixture\\late.txt", L"late.txt", L"late"});
            const auto snapshot = tab.snapshot;
            ApplyContentSearchUpdate(s, std::move(stale));
            Check(tab.current_path == origin && tab.snapshot == snapshot, "late content results cannot overwrite restored directory");
            Drain(s);
        }
    }
    tab.current_path = origin;
    tab.NavigateTo(app::MakeSearchPath(L"needle"));
    s.addressSearchComposing = true;
    SubmitAddressSearch(s, true);
    Check(IsAddressSearchResults(&tab), "IME composition defers empty-search navigation");
    s.addressSearchComposing = false;
    for (const auto& virtual_origin : {std::wstring(L"pulse:home"), std::wstring{}}) {
        tab.current_path = virtual_origin;
        tab.NavigateTo(app::MakeSearchPath(L"content:needle"));
        s.addressSearching = true;
        s.addressSearchContent = true;
        SubmitAddressSearch(s, true);
        Check(tab.current_path == virtual_origin, "empty search restores virtual and This PC origins");
        Drain(s);
    }
    for (const auto mode : {ui::ViewMode::Details, ui::ViewMode::LargeIcons}) {
        tab.current_path = origin;
        s.addressSearching = false;
        s.appPrefs.folder_views.Set(origin, mode);
        s.appPrefs.folder_views.SetGroup(origin, ui::GroupMode::None, ui::SortDirection::Asc);
        StartLoadingPath(s, tab, origin);
        Drain(s);
        Check(tab.EntryCount() == 240 && !tab.loading && tab.pending_generation == 0, "real directory listing published");
        tab.SelectOnly(75);
        EnsureRowVisible(s, tab, 75);
        tab.scroll_y += 40.0f;
        const float original = tab.scroll_y;
        tab.scroll_x = 0.0f;
        s.scrollTargetY = tab.scroll_y;
        const auto child = tab.EntryAt(75).name;
        NavigateTo(s, origin + L"\\" + child);
        Drain(s);
        GoBack(s);
        Drain(s);
        std::cout << "RETURN mode=" << static_cast<int>(mode) << " before=" << original
            << " after=" << tab.scroll_y << " scroll_x=" << tab.scroll_x
            << " selected=" << tab.selected_index << " target=" << s.scrollTargetY << std::endl;
        Check(tab.selected_index == 75, "return retains child selection");
        Check(tab.scroll_y == original && tab.scroll_x == 0.0f, "back restores exact previous viewport");
        Check(s.scrollTargetY == tab.scroll_y && !s.scrollAnimating, "back synchronizes scroll animation target");
        tab.scroll_y = original + 20.0f;
        GoForward(s);
        Drain(s);
        Check(tab.scroll_y == 0.0f, "forward restores child viewport");
        GoBack(s);
        Drain(s);
        Check(tab.scroll_y == original + 20.0f, "repeated history navigation saves updated viewport");
    }
    app::Tab legacy;
    legacy.current_path = origin + L"\\folder075";
    legacy.back_stack.push({origin});
    legacy.GoBack();
    Check(!legacy.pending_history_viewport && legacy.scroll_y == 0.0f, "older history without viewport metadata remains valid");
    legacy.scroll_y = 120.0f;
    legacy.scroll_x = 17.0f;
    legacy.NavigateTo(origin + L"\\folder075");
    legacy.GoBack();
    Check(legacy.scroll_y == 120.0f && legacy.scroll_x == 17.0f, "model history restores both viewport axes");
    legacy.NavigateTo(origin + L"\\folder076");
    Check(!legacy.pending_history_viewport && !legacy.CanGoForward(), "new navigation clears pending restoration and forward history");
    s.worker.Stop();
    s.watches.Stop();
    s.renderer.SetCompositor(nullptr);
    s.compositor.Shutdown();
    DestroyWindow(s.hwnd);
    s.hwnd = s.hwndAddressEdit = nullptr;
    state.reset();
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    CoUninitialize();
    std::cout << "RESULT failures=" << failures << std::endl;
    return failures ? 1 : 0;
}
