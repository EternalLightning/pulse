// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "../app/app_internal.h"
#include "../ui/ui_renderer_internal.h"
#include <filesystem>
#include <iostream>

LRESULT CALLBACK WndProcImpl(HWND, UINT, WPARAM, LPARAM);

int main() {
    using namespace pulse;
    OleInitialize(nullptr);
    l10n::Initialize(GetModuleHandleW(nullptr), L"zh-CN");
    int failures = 0;
    const auto check = [&](bool ok, const char* label) {
        std::cout << (ok ? "[PASS] " : "[FAIL] ") << label << std::endl;
        failures += !ok;
    };
    auto owned = std::make_unique<AppState>();
    auto& s = *owned;
    s.isolatedTest = true;
    s.appPrefs.persist = s.places.persist = s.searchHistory.persist = s.ctxMenuPrefs.persist = false;
    s.hwnd = CreateWindowExW(0, L"STATIC", L"Duplicate interaction fixture", WS_OVERLAPPEDWINDOW,
        0, 0, 1280, 900, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!s.hwnd || !s.compositor.Init(s.hwnd)) {
        check(false, "initialize isolated duplicate interaction compositor");
        if (s.hwnd) DestroyWindow(s.hwnd);
        s.hwnd = nullptr; owned.reset(); OleUninitialize();
        return 2;
    }
    SetWindowLongPtrW(s.hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&s));
    s.compositor.RecreateTextFormats(s.scale);
    s.renderer.SetCompositor(&s.compositor);
    s.renderer.SetScale(s.scale);
    s.window_tabs.EnsureDefault();
    s.pane = s.window_tabs.Active()->FocusedPane();
    s.settings.SelectPage(4);
    s.settings.BindUi(s.appPrefs, s.ctxMenuPrefs, s.index, s.networkIndex, {});
    s.dup_volume_cache_tick = GetTickCount64();
    auto& tab = *ActiveTab(s);
    tab.current_path = app::MakeSettingsPath(L"duplicates");
    const std::wstring root = fs::NormalizePath(std::filesystem::absolute(
        L"../bench_data/duplicate-interaction").wstring());
    const std::wstring leaf = L"zzz-季度预算-\U0001F4C4.txt";
    s.duplicateScan.completed = true;
    s.duplicateScan.groups = {
        {1, 100, {{root + L"\\ignored-one.txt", L"ignored-one.txt"},
                  {root + L"\\ignored-two.txt", L"ignored-two.txt"}}, 0},
        {2, 100, {{root + L"\\aaa-copy.txt", L"aaa-copy.txt"},
                  {root + L"\\" + leaf, leaf}}, 0}
    };
    ui::fluent::Painter painter(&s.compositor);
    const auto layout = [&] {
        auto vm = BuildVm(s, false);
        const auto bounds = D2D1::RectF(0, 0, static_cast<float>(s.compositor.Width()),
            static_cast<float>(s.compositor.Height()));
        const auto theme = ui::MakeTheme(false, ui::HexColor(0x0078D4));
        painter.BeginFrame(theme);
        s.compositor.Dc()->BeginDraw(); s.renderer.Render(vm, bounds, theme);
        check(SUCCEEDED(s.compositor.Dc()->EndDraw()), "duplicate interaction frame renders");
        return ui::MakeSettingsLayout(vm, bounds, s.scale, s.renderer.TitleBarHeight(),
            28 * s.scale, &painter);
    };
    const auto click = [&](D2D1_RECT_F rect) {
        const LPARAM point = MAKELPARAM(static_cast<int>((rect.left + rect.right) / 2),
            static_cast<int>((rect.top + rect.bottom) / 2));
        HandleLButtonDown(&s, s.hwnd, WM_LBUTTONDOWN, MK_LBUTTON, point);
        HandleLButtonUp(&s, s.hwnd, WM_LBUTTONUP, 0, point);
    };
    s.duplicateScan.scanning = true;
    auto controls = layout();
    click(controls.dup_group_ignore[0]);
    check(s.duplicateScan.groups.size() == 2, "ignore stays disabled while the scan is running");
    s.duplicateScan.scanning = false;
    controls = layout();
    click(controls.dup_group_ignore[0]);
    check(s.duplicateScan.groups.size() == 1 && s.duplicateScan.groups[0].id == 2 &&
        s.duplicateScan.AllFilesToDelete() == std::vector<std::wstring>{root + L"\\" + leaf},
        "ignore click removes its group and excludes its files from bulk deletion");
    controls = layout();
    check(controls.dup_open_group.size() == 2 && controls.dup_open_group[1] == 0 &&
        controls.dup_open_file[1] == 1, "path hit mapping follows the remaining group after ignore");
    const auto path_rect = controls.dup_open[1];
    POINT cursor{};
    GetCursorPos(&cursor);
    const int x = static_cast<int>((path_rect.left + path_rect.right) / 2);
    const int y = static_cast<int>((path_rect.top + path_rect.bottom) / 2);
    POINT client_origin{};
    ClientToScreen(s.hwnd, &client_origin);
    RECT window_rect{};
    GetWindowRect(s.hwnd, &window_rect);
    SetWindowPos(s.hwnd, nullptr, cursor.x - x - (client_origin.x - window_rect.left),
        cursor.y - y - (client_origin.y - window_rect.top), 0, 0,
        SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    const HCURSOR previous_cursor = GetCursor();
    const auto handled = WndProcImpl(s.hwnd, WM_SETCURSOR, reinterpret_cast<WPARAM>(s.hwnd),
        MAKELPARAM(HTCLIENT, WM_MOUSEMOVE));
    check(handled && GetCursor() == LoadCursorW(nullptr, IDC_HAND),
        "hovering a duplicate path sets the real hand cursor");
    SetCursor(previous_cursor);
    click(path_rect);
    check(tab.current_path == root && !IsSettingsTab(&tab), "path click opens its folder in the main pane");
    check(tab.pending_selected_name == leaf && tab.pending_ensure_selection_visible,
        "path click schedules selection and scrolling after asynchronous loading");
    const auto listing = [&] {
        auto rows = std::make_shared<std::vector<fs::DirEntry>>();
        for (int i = 0; i < 100; ++i) {
            fs::DirEntry entry;
            entry.name = L"aaa-" + std::to_wstring(i) + L".txt";
            entry.full_path = root + L"\\" + entry.name;
            entry.attrs = FILE_ATTRIBUTE_NORMAL;
            rows->push_back(std::move(entry));
        }
        fs::DirEntry target;
        target.name = leaf; target.full_path = root + L"\\" + leaf;
        target.attrs = FILE_ATTRIBUTE_NORMAL;
        rows->push_back(std::move(target));
        return rows;
    };
    // The isolated fixture injects a listing instead of starting disk workers.
    tab.pending_generation = 900001;
    app::WorkResult result;
    result.path = root; result.generation = tab.pending_generation; result.snapshot = listing();
    ApplyWorkerResult(s, result);
    check(tab.SelectedCount() == 1 && tab.selected_index >= 0 &&
        tab.EntryAt(static_cast<size_t>(tab.selected_index)).name == leaf,
        "loaded folder selects the requested Unicode filename");
    check(tab.scroll_y > 0, "loaded folder scrolls to reveal the requested file");
    OpenDuplicateLocation(s, 0, 1);
    check(tab.SelectedCount() == 1 && tab.EntryAt(static_cast<size_t>(tab.selected_index)).name == leaf,
        "cached folder also selects the requested file immediately");
    SetWindowLongPtrW(s.hwnd, GWLP_USERDATA, 0);
    painter.SetCompositor(nullptr);
    s.renderer.SetCompositor(nullptr); s.compositor.Shutdown();
    DestroyWindow(s.hwnd); s.hwnd = nullptr;
    owned.reset(); OleUninitialize();
    std::cout << "failures=" << failures << '\n';
    return failures ? 1 : 0;
}
