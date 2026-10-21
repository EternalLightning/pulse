#include "../app/app_internal.h"
#include "../app/app_input.h"
#include "../app/session.h"
#include "../ui/ui_renderer_internal.h"
#include <algorithm>
#include <cstdio>
#include <filesystem>

namespace {
int failures = 0;
int menu_row = 0;
float menu_scale = 1;
bool menu_clicked = false;

void Check(bool passed, const char* label) {
    std::printf("[%s] %s\n", passed ? "PASS" : "FAIL", label);
    failures += !passed;
}

BOOL CALLBACK ClickMenu(HWND window, LPARAM) {
    wchar_t name[64]{};
    GetClassNameW(window, name, ARRAYSIZE(name));
    if (wcscmp(name, L"PulseFluentMenu") != 0 || !IsWindowVisible(window)) return TRUE;
    const auto point = MAKELPARAM(50, static_cast<int>(
        pulse::ui::FluentMenu::kShadowMargin + (20 + 32 * menu_row) * menu_scale));
    PostMessageW(window, WM_MOUSEMOVE, 0, point);
    PostMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, point);
    PostMessageW(window, WM_LBUTTONUP, 0, point);
    menu_clicked = true;
    return FALSE;
}

void CALLBACK SelectMenu(HWND, UINT, UINT_PTR timer, DWORD) {
    EnumThreadWindows(GetCurrentThreadId(), ClickMenu, 0);
    if (menu_clicked) KillTimer(nullptr, timer);
}

void RightClick(pulse::AppState& state, float x, float y, int row) {
    menu_row = row;
    menu_scale = state.scale;
    menu_clicked = false;
    const auto timer = SetTimer(nullptr, 0, 200, SelectMenu);
    pulse::HandleRButtonUp(&state, state.hwnd, WM_RBUTTONUP, 0,
        MAKELPARAM(static_cast<int>(x), static_cast<int>(y)));
    KillTimer(nullptr, timer);
    Check(menu_clicked, "production sidebar context menu receives a pointer click");
}
}

int RunSidebarStarredUiTest() {
    using namespace pulse;
    const auto output = std::filesystem::absolute(L"bench_data/sidebar-starred-ui");
    std::filesystem::create_directories(output);
    auto owned = std::make_unique<AppState>();
    auto& s = *owned;
    s.isolatedTest = true;
    s.shot.active = true;
    s.appPrefs.persist = s.ctxMenuPrefs.persist = s.places.persist = false;
    s.accentColor = ui::HexColor(0x0078D4);
    SetEnvironmentVariableW(L"PULSE_TEST_HIDDEN_SHOT", L"1");
    s.hwnd = CreateWindowExW(WS_EX_NOACTIVATE, L"STATIC", L"Sidebar starred fixture",
        WS_POPUP, 0, 0, 960, 800, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!s.hwnd || !s.compositor.Init(s.hwnd)) {
        Check(false, "initialize production sidebar compositor");
        return 1;
    }
    SetWindowLongPtrW(s.hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&s));
    s.renderer.SetCompositor(&s.compositor);
    s.renderer.SetDetailsPanelVisible(false);
    s.window_tabs.EnsureDefault();
    s.pane = s.window_tabs.Active()->FocusedPane();
    ActiveTab(s)->current_path = app::MakeHomePath();
    ActiveTab(s)->loading = false;
    app::SidebarEntry root;
    root.path = app::MakeStarredPath();
    root.label = l10n::Get(l10n::StringId::StarredItems);
    root.glyph = L"\xE735";
    root.color = ui::HexColor(0xFBBF24);
    root.expandable = true;
    s.sidebar.starred.push_back(root);
    root.path = L"C:\\fixture\\desktop";
    root.label = L"桌面";
    root.glyph = L"\xE7F4";
    root.expandable = false;
    s.sidebar.quick_access.push_back(root);
    s.places.starred_items.push_back({L"C:\\fixture\\project", app::PlaceItemKind::Folder});
    constexpr int starred_id = static_cast<int>(app::SidebarSectionId::Starred);
    for (const float scale : {1.0f, 1.5f}) {
        s.menu.reset();
        s.scale = scale;
        s.renderer.SetScale(scale);
        s.compositor.RecreateTextFormats(scale);
        SetWindowPos(s.hwnd, nullptr, 0, 0, static_cast<int>(960 * scale),
            static_cast<int>(800 * scale), SWP_NOMOVE | SWP_NOACTIVATE | SWP_NOZORDER);
        s.compositor.Resize(static_cast<UINT>(960 * scale), static_cast<UINT>(800 * scale));
        const auto sb = s.renderer.SidebarRect(960 * scale, 800 * scale);
        const auto slots = [&](const ui::WindowViewModel& vm, bool rail = false) {
            std::vector<ui::SidebarSlot> result;
            auto bounds = sb;
            if (rail) bounds.right = ui::kSidebarRailWidthDip * scale;
            ui::LayoutSidebar(vm, bounds, scale, result);
            return result;
        };
        for (const bool dark : {false, true}) {
            s.darkMode = dark;
            s.sidebarHiddenMask = 0;
            auto vm = BuildVm(s, false);
            const auto visible = slots(vm);
            const auto star = std::find_if(visible.begin(), visible.end(), [&](const auto& slot) {
                return slot.group >= 0 && slot.item == 0 && vm.sidebar[slot.group].id == starred_id;
            });
            Check(star != visible.end(), "starred root is visible beside the quick-access header");
            if (star == visible.end()) continue;
            const auto star_bounds = star->rc;
            Render(s);
            const auto suffix = std::to_wstring(static_cast<int>(scale * 100)) +
                (dark ? L"-dark.png" : L"-light.png");
            Check(s.compositor.SaveSnapshot((output / (L"visible-" + suffix)).c_str()),
                "capture actual sidebar typography at each theme and DPI");
            // Alternate text and disclosure targets: both must open the section menu.
            const float x = dark ? star_bounds.right - 14 * scale : star_bounds.left + 50 * scale;
            RightClick(s, x, (star_bounds.top + star_bounds.bottom) * 0.5f, 0);
            Check(s.sidebarHiddenMask == (1u << starred_id),
                "right-clicking the starred root hides the entire section only");
            app::SessionSnapshot saved, loaded;
            saved.sidebar_hidden = static_cast<int>(s.sidebarHiddenMask);
            Check(app::ParseSessionJson(app::SessionToJson(saved), loaded) &&
                loaded.sidebar_hidden == saved.sidebar_hidden,
                "starred visibility persists through the existing session format");
            vm = BuildVm(s, false);
            for (const bool rail : {false, true}) {
                const auto hidden = slots(vm, rail);
                const auto access = std::find_if(hidden.begin(), hidden.end(), [&](const auto& slot) {
                    return slot.group >= 0 && vm.sidebar[slot.group].id ==
                        static_cast<int>(app::SidebarSectionId::QuickAccess);
                });
                Check(std::none_of(hidden.begin(), hidden.end(), [&](const auto& slot) {
                    return slot.group >= 0 && vm.sidebar[slot.group].id == starred_id;
                }), "hidden root and starred children have no slots in wide and rail layouts");
                Check(access != hidden.end() && std::abs(access->rc.top -
                    (rail ? sb.top : sb.top + ui::MakeSidebarMetrics(scale).pad)) < 0.5f,
                    "quick access closes the gap left by the hidden starred section");
            }
            Render(s);
            Check(s.compositor.SaveSnapshot((output / (L"hidden-" + suffix)).c_str()),
                "capture sidebar with no residual starred-section gap");
            // Space below all sections opens the existing pane-wide show/hide menu.
            std::vector<ui::SidebarGroupBand> bands;
            s.renderer.SidebarGroupBands(vm, 960 * scale, 800 * scale, bands);
            const float blank_y = bands.empty() ? sb.top + 10 * scale : bands.back().bottom + 12 * scale;
            RightClick(s, sb.left + 40 * scale, blank_y, 1);
            Check(s.sidebarHiddenMask == 0, "pane-wide menu restores the starred section");
        }
    }
    s.menu.reset();
    s.watches.Stop();
    DestroyWindow(s.hwnd);
    s.hwnd = nullptr;
    std::printf("sidebar starred: %d failures\n", failures);
    return failures ? 1 : 0;
}
