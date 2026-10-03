#include "../app/app_internal.h"
#include "../common/localization.h"
#include "../ui/ui_renderer_internal.h"
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string_view>
#include <algorithm>
#include <tuple>

namespace pulse { void UpdateSidebarPinDrag(AppState& s, int my); }

int QuickAccessDragTest() {
    using namespace pulse;
    auto owned = std::make_unique<AppState>();
    auto& s = *owned;
    s.isolatedTest = true;
    s.appPrefs.persist = s.places.persist = s.ctxMenuPrefs.persist = s.searchHistory.persist = false;
    s.hwnd = CreateWindowExW(WS_EX_NOACTIVATE, L"STATIC", L"quick access fixture", WS_OVERLAPPEDWINDOW,
        -30000, -30000, 1100, 780, nullptr, nullptr, nullptr, nullptr);
    if (!s.hwnd || !s.compositor.Init(s.hwnd)) return 2;
    s.renderer.SetCompositor(&s.compositor);
    s.window_tabs.EnsureDefault(); s.pane = s.window_tabs.Active()->FocusedPane();
    ActiveTab(s)->current_path = L"C:\\quick-access-fixture";
    ActiveTab(s)->git_root = L"C:\\quick-access-fixture\\project";
    s.sidebarHiddenMask = ((1u << app::kSidebarSectionCount) - 1u) &
        ~(1u << static_cast<int>(app::SidebarSectionId::QuickAccess));
    for (const auto& [name, path, builtin] : std::vector<std::tuple<const wchar_t*, const wchar_t*, app::BuiltinQuickAccess>>{
             {L"最近使用", L"pulse:recent", app::BuiltinQuickAccess::Recent},
             {L"桌面", L"C:\\quick-access-fixture\\desktop", app::BuiltinQuickAccess::Desktop},
             {L"下载", L"C:\\quick-access-fixture\\downloads", app::BuiltinQuickAccess::Downloads},
             {L"回收站", L"pulse:recycle", app::BuiltinQuickAccess::RecycleBin}}) {
        app::SidebarEntry entry;
        entry.label = name; entry.path = path; entry.glyph = L"\xE8B7";
        entry.builtin = static_cast<int>(builtin);
        s.sidebar.quick_access.push_back(std::move(entry));
    }
    const std::wstring pin = L"C:\\quick-access-fixture\\pinned";
    s.places.SetQuickAccessPinned({pin}, true);
    int failures = 0;
    const auto check = [&](bool ok, const char* text) {
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", text);
        if (!ok) ++failures;
    };
    const int section = static_cast<int>(app::SidebarSectionId::QuickAccess);
    const auto drag = [&](const std::wstring& path, bool to_front) {
        const auto vm = BuildVm(s, false);
        const int group = app::SidebarSectionIndex(vm, section);
        if (group < 0) { check(false, "quick access group exists"); return; }
        const auto& items = vm.sidebar[static_cast<size_t>(group)].items;
        const auto found = std::find_if(items.begin(), items.end(), [&](const auto& item) {
            return _wcsicmp(fs::NormalizePath(item.path).c_str(), fs::NormalizePath(path).c_str()) == 0;
        });
        D2D1_RECT_F source{}, target{};
        const float width = static_cast<float>(s.compositor.Width());
        const float height = static_cast<float>(s.compositor.Height());
        if (found == items.end() || !s.renderer.SidebarRowRect(vm, width, height, section,
                static_cast<int>(found - items.begin()), &source) ||
            !s.renderer.SidebarRowRect(vm, width, height, section,
                to_front ? 0 : static_cast<int>(items.size() - 1), &target)) {
            check(false, "real layout exposes drag source and target"); return;
        }
        const POINT point{static_cast<LONG>(source.left + 100 * s.scale),
            static_cast<LONG>((source.top + source.bottom) / 2)};
        HandleLButtonDown(&s, s.hwnd, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(point.x, point.y));
        check(s.pinDragPending && !s.pinDragActive && s.pinDragPath == found->path,
            "real row press arms drag for built-ins, project and pinned folders");
        // Enter the drag state directly instead of injecting desktop mouse input.
        s.pinDragActive = true;
        const int y = static_cast<int>(to_front ? target.top - 1 : target.bottom + 1);
        UpdateSidebarPinDrag(s, y);
        check(s.pinGapVisible && s.pinDragToIndex == (to_front ? 0 : static_cast<int>(items.size())),
            "insertion line measures the entire quick-access list");
        HandleLButtonUp(&s, s.hwnd, WM_LBUTTONUP, 0, MAKELPARAM(point.x, y));
        const auto after = BuildVm(s, false);
        const auto& moved = after.sidebar[static_cast<size_t>(app::SidebarSectionIndex(after, section))].items;
        check(_wcsicmp(fs::NormalizePath((to_front ? moved.front() : moved.back()).path).c_str(),
                fs::NormalizePath(path).c_str()) == 0 && !s.pinDragPending && !s.pinDragActive,
            "release commits full-list ordering and clears drag state");
    };
    for (const float scale : {1.0f, 1.25f, 1.5f}) {
        s.scale = scale; s.renderer.SetScale(scale); s.compositor.RecreateTextFormats(scale);
        s.compositor.Resize(static_cast<UINT>(1100 * scale), static_cast<UINT>(780 * scale));
        s.places.quick_access_order.clear();
        for (const auto& entry : s.sidebar.quick_access) drag(entry.path, false);
        drag(ActiveTab(s)->git_root, false);
        drag(pin, true);
    }
    wchar_t executable[MAX_PATH]{};
    GetModuleFileNameW(nullptr, executable, MAX_PATH);
    const auto output = std::filesystem::path(executable).parent_path().parent_path() / L"bench_data" / L"quick-access-order";
    std::error_code error;
    std::filesystem::create_directories(output, error);
    for (const bool dark : {false, true}) {
        s.darkMode = dark;
        const auto vm = BuildVm(s, false);
        const auto theme = ui::MakeTheme(dark, ui::HexColor(0x0078D4));
        auto* dc = s.compositor.Dc();
        dc->BeginDraw(); dc->Clear(theme.bg);
        s.renderer.Render(vm, D2D1::RectF(0, 0, static_cast<float>(s.compositor.Width()),
            static_cast<float>(s.compositor.Height())), theme);
        const bool rendered = SUCCEEDED(dc->EndDraw());
        const auto snapshot = output / (dark ? L"dark-150.png" : L"light-150.png");
        check(!error && rendered && s.compositor.SaveSnapshot(snapshot.c_str()),
            "render and save reordered quick-access rows in both themes");
    }
    const HWND hwnd = s.hwnd;
    s.hwnd = nullptr; owned.reset(); DestroyWindow(hwnd);
    return failures ? 1 : 0;
}

int wmain(int argc, wchar_t** argv) {
    using namespace pulse;
    OleInitialize(nullptr);
    l10n::Initialize(GetModuleHandleW(nullptr), L"zh-CN");
    if (argc == 2 && std::wstring_view(argv[1]) == L"--quick-access") {
        const int result = QuickAccessDragTest();
        OleUninitialize();
        return result;
    }
    int failures = 0;
    auto check = [&](bool ok, const char* text) {
        printf("[%s] %s\n", ok ? "PASS" : "FAIL", text);
        if (!ok) ++failures;
    };
    ui::fluent::Painter painter;
    for (float scale : {1.0f, 1.25f, 1.5f, 2.0f}) {
        painter.SetScale(scale);
        for (float height : {20.0f, 26.0f, 32.0f}) {
            for (bool dot : {false, true}) {
                const auto row = D2D1::RectF(10 * scale, 20 * scale, 210 * scale, (20 + height) * scale);
                const auto icon = painter.SidebarItemIconRect(row, dot);
                check(std::abs(icon.right - icon.left - 16 * scale) < 0.001f &&
                    std::abs(icon.bottom - icon.top - 16 * scale) < 0.001f &&
                    std::abs(icon.top + icon.bottom - row.top - row.bottom) < 0.001f &&
                    icon.top >= row.top && icon.bottom <= row.bottom,
                    "sidebar icon keeps centered 16 DIP square across row heights and DPI");
            }
        }
    }
    auto owned = std::make_unique<AppState>();
    auto& s = *owned;
    s.isolatedTest = true;
    s.appPrefs.persist = false;
    s.places.persist = false;
    s.hwnd = CreateWindowExW(0, L"STATIC", L"", WS_POPUP, 0, 0, 1000, 700,
        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    check(s.hwnd && s.compositor.Init(s.hwnd), "initialize isolated real layout/hit fixture");
    if (!s.compositor.Dc()) return 1;
    s.renderer.SetCompositor(&s.compositor);
    s.renderer.SetScale(1.0f);
    s.compositor.RecreateTextFormats(1.0f);
    s.window_tabs.NewTab(L"C:\\PulseSidebarTrayFixture");
    s.pane = s.window_tabs.Active()->panes.front().get();
    // Nonexistent synthetic paths are allowed in the tray; no file writes/deletes.
    s.tray.Collect({L"C:\\PulseSidebarTrayFixture\\a", L"C:\\PulseSidebarTrayFixture\\b",
        L"C:\\PulseSidebarTrayFixture\\c", L"C:\\PulseSidebarTrayFixture\\d"}, false);
    double now = TrayAnimationNow();
    auto settle = [&]() {
        for (int i = 0; i < 90; ++i) TickTrayDeck(s, now += 16.0);
    };
    settle();
    auto find_hit = [&](ui::HitTestResult::Region region) {
        const auto vm = BuildVm(s, false);
        const auto panel = s.renderer.StagingTrayRect(vm, 1000, 700);
        for (float y = panel.top; y < panel.bottom; y += 1.0f)
            for (float x = panel.left; x < panel.right; x += 1.0f)
                if (s.renderer.HitTest(vm, D2D1::RectF(0, 0, 1000, 700), x, y).region == region)
                    return POINT{static_cast<LONG>(x), static_cast<LONG>(y)};
        return POINT{-1, -1};
    };
    auto press = [&](POINT pt, bool twice) {
        const LPARAM pos = MAKELPARAM(pt.x, pt.y);
        if (twice) HandleLButtonDblClk(&s, s.hwnd, WM_LBUTTONDBLCLK, MK_LBUTTON, pos);
        else HandleLButtonDown(&s, s.hwnd, WM_LBUTTONDOWN, MK_LBUTTON, pos);
        HandleLButtonUp(&s, s.hwnd, WM_LBUTTONUP, 0, pos);
    };
    const auto next = find_hit(ui::HitTestResult::TrayNext);
    const auto prev = find_hit(ui::HitTestResult::TrayPrev);
    check(next.x >= 0 && prev.x >= 0, "real layout exposes both pager buttons");
    press(next, false);
    press(next, true);
    check(TrayStackTop(s) == 2, "rapid next DOWN/DBLCLK advances twice without waiting");
    press(prev, false);
    press(prev, true);
    check(TrayStackTop(s) == 0, "rapid previous DOWN/DBLCLK advances twice without waiting");
    settle();
    const auto top = TrayDeckEntries(s.tray, 0, 1).front().item->path;
    ThrowTrayTop(s, 1.0f, 25.0f, 5.0f);
    const double start = s.trayCards.at(top).motion_started;
    TickTrayDeck(s, start + 140);
    check(s.trayCards.at(top).motion == AppState::TrayMotion::ThrowBack,
        "throw starts returning within 140ms");
    TickTrayDeck(s, start + 360);
    check(s.trayCards.at(top).motion == AppState::TrayMotion::None,
        "throw motion finishes within 360ms");
    s.trayDeckOffset = 0;
    settle();
    const auto close = find_hit(ui::HitTestResult::TrayItemRemove);
    check(close.x >= 0, "real layout exposes the top card X");
    const auto before = s.trayCards.at(top);
    press(close, false);
    TickTrayDeck(s, now += 16);
    check(TrayItemTotalCount(s.tray) == 3 && s.trayCards.at(top).ghost &&
        s.trayCards.at(top).exit == AppState::TrayExit::Fade && s.trayPuffs.empty(),
        "X removes only one item and creates a fade without smoke");
    TickTrayDeck(s, now += 70);
    const auto& faded = s.trayCards.at(top);
    check(faded.opacity < before.opacity && faded.opacity > 0 && faded.fly == before.fly &&
        faded.dx == before.dx && faded.dy == before.dy && faded.angle == before.angle &&
        faded.shrink == before.shrink && faded.motion == AppState::TrayMotion::None,
        "single dismissal changes opacity only, without flight rotation or shrink");
    TickTrayDeck(s, now += 80);
    check(!s.trayCards.contains(top), "dismissed ghost is gone within 150ms");
    settle();
    const auto remaining = TrayDeckEntries(s.tray, static_cast<size_t>(TrayStackTop(s)), 1).front();
    const auto path = remaining.item->path;
    ThrowTrayTop(s, -1.0f, -40, 8);
    const auto moving = s.trayCards.at(path);
    MarkTrayExit(s, {path}, false);
    s.tray.RemoveItem(static_cast<size_t>(remaining.batch), static_cast<size_t>(remaining.sub));
    TickTrayDeck(s, now += 16);
    TickTrayDeck(s, now += 70);
    const auto& stopped = s.trayCards.at(path);
    check(stopped.motion == AppState::TrayMotion::None && stopped.exit == AppState::TrayExit::Fade &&
        stopped.dx == moving.dx && stopped.dy == moving.dy && stopped.fly == moving.fly &&
        stopped.angle == moving.angle, "dismissal freezes an in-progress fling at its current pose");
    // Native renderer snapshots use isolated in-memory sidebar data, never preferences.
    settle();
    wchar_t executable[MAX_PATH]{};
    const DWORD executable_length = GetModuleFileNameW(nullptr, executable, ARRAYSIZE(executable));
    check(executable_length > 0 && executable_length < ARRAYSIZE(executable),
        "resolve snapshot output relative to the fixture executable");
    const auto snapshot_dir = std::filesystem::path(executable).parent_path().parent_path() /
        L"bench_data" / L"review-changes" / L"feedback-ui";
    std::error_code snapshot_error;
    std::filesystem::create_directories(snapshot_dir, snapshot_error);
    check(!snapshot_error, "create isolated feedback snapshot directory");
    auto snapshot_vm = BuildVm(s, false);
    snapshot_vm.sidebar.clear();
    ui::SidebarGroup group;
    group.header = L"快速访问";
    for (const auto& [label, folder] : {
        std::pair{L"Pulse", L"E:\\Projects\\pulse"},
        std::pair{L"测试程序", L"E:\\Projects\\TestPrograms"},
        std::pair{L"源代码", L"E:\\Projects\\pulse\\src"},
        std::pair{L"界面", L"E:\\Projects\\pulse\\src\\ui"}}) {
        ui::SidebarItem item;
        item.label = label;
        item.path = folder;
        item.use_path_icon = true;
        item.icon_glyph = L"\uE8B7";
        item.fallback_text = L"D";
        item.icon_color = ui::HexColor(0xD6A02D);
        group.items.push_back(std::move(item));
    }
    snapshot_vm.sidebar.push_back(std::move(group));
    snapshot_vm.sidebar_scroll = 0;
    for (bool dark : {false, true}) {
        snapshot_vm.dark = dark;
        const auto theme = ui::MakeTheme(dark, ui::HexColor(0x0078D4));
        bool rendered = true;
        // Path icons resolve asynchronously; warm up bounded frames before capture.
        for (int frame = 0; frame < 20; ++frame) {
            auto* dc = s.compositor.Dc();
            dc->BeginDraw();
            dc->Clear(theme.bg);
            s.renderer.Render(snapshot_vm, D2D1::RectF(0, 0, 1000, 700), theme);
            rendered = SUCCEEDED(dc->EndDraw()) && rendered;
            if (frame < 19) Sleep(50);
        }
        const auto snapshot = snapshot_dir / (dark ? L"sidebar-tray-dark-100.png" :
            L"sidebar-tray-light-100.png");
        check(rendered && !snapshot_error && s.compositor.SaveSnapshot(snapshot.c_str()),
            dark ? "save native 100% dark sidebar/tray snapshot" :
                   "save native 100% light sidebar/tray snapshot");
        wprintf(L"[SNAPSHOT] %ls\n", snapshot.c_str());
    }
    const HWND hwnd = s.hwnd;
    s.hwnd = nullptr;
    owned.reset();
    DestroyWindow(hwnd);
    OleUninitialize();
    return failures ? 1 : 0;
}
