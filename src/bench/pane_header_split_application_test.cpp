// SPDX-License-Identifier: Apache-2.0
#include "../app/app_internal.h"
#include "../common/user_storage.h"
#include <filesystem>
#include <cstdio>

namespace {
int passed = 0, failed = 0;
void Check(bool ok, const char* label) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
    ok ? ++passed : ++failed;
}
bool Nonempty(D2D1_RECT_F rc) { return rc.right > rc.left && rc.bottom > rc.top; }
POINT Center(D2D1_RECT_F rc) {
    return {static_cast<LONG>((rc.left + rc.right) / 2), static_cast<LONG>((rc.top + rc.bottom) / 2)};
}
RECT captured_menu{};
bool captured = false;
void CALLBACK SelectHorizontal(HWND, UINT, UINT_PTR timer, DWORD) {
    HWND menu = nullptr;
    EnumThreadWindows(GetCurrentThreadId(), [](HWND hwnd, LPARAM out) -> BOOL {
        wchar_t name[64]{}; GetClassNameW(hwnd, name, ARRAYSIZE(name));
        if (std::wstring_view(name) == L"PulseFluentMenu" && IsWindowVisible(hwnd)) {
            *reinterpret_cast<HWND*>(out) = hwnd; return FALSE;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&menu));
    KillTimer(nullptr, timer);
    if (menu) {
        captured = GetWindowRect(menu, &captured_menu) != FALSE;
        PostMessageW(menu, WM_KEYDOWN, VK_DOWN, 0);
        PostMessageW(menu, WM_KEYDOWN, VK_DOWN, 0);
        PostMessageW(menu, WM_KEYDOWN, VK_RETURN, 0);
    } else {
        PostThreadMessageW(GetCurrentThreadId(), WM_KEYDOWN, VK_ESCAPE, 0);
    }
}
}
int main() {
    using namespace pulse;
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    OleInitialize(nullptr);
    const auto root = std::filesystem::absolute(L"../bench_data/pane-header-split-2026-11-17");
    std::filesystem::create_directories(root / L"profile");
    storage::OverrideDefaultRootForTesting((root / L"profile").wstring());
    l10n::Initialize(GetModuleHandleW(nullptr), L"zh-CN");
    auto owned = std::make_unique<AppState>();
    auto& s = *owned;
    s.isolatedTest = true;
    s.appPrefs.persist = s.places.persist = s.ctxMenuPrefs.persist = false;
    s.appPrefs.auto_folder_size = false;
    s.showDetailsPanel = false;
    s.accentColor = ui::HexColor(0x0078D4);
    s.window_tabs.EnsureDefault();
    auto* layout = s.window_tabs.Active();
    layout->panes.push_back(std::make_unique<app::Pane>());
    std::vector<app::Pane*> panes;
    for (auto& pane : layout->panes) {
        pane->view.current_path = root.wstring();
        pane->view.loading = false;
        pane->view.SetSnapshot(std::make_shared<const std::vector<fs::DirEntry>>());
        pane->header_animation.opacity = 1;
        panes.push_back(pane.get());
    }
    layout->layout = app::LayoutPreset::TwoVertical;
    layout->root = app::MakePresetTree(layout->layout, panes);
    s.pane = panes[1]; layout->focused_index = 1;
    s.hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"STATIC", L"Pane split fixture",
        WS_POPUP, 40, 40, 1000, 620, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    Check(s.hwnd && s.compositor.Init(s.hwnd), "initialize isolated production pane renderer");
    if (!s.compositor.Dc()) return 2;
    s.renderer.SetCompositor(&s.compositor);
    s.renderer.SetDetailsPanelVisible(false);
    for (const float scale : {1.0f, 1.25f, 1.5f, 1.75f, 2.0f}) {
        s.scale = scale; s.renderer.SetScale(scale);
        s.compositor.RecreateTextFormats(scale);
        for (const int dip_width : {620, 1000, 1500}) {
            const float width = dip_width * scale, height = 620 * scale;
            s.compositor.Resize(static_cast<UINT>(width), static_cast<UINT>(height));
            const auto bounds = D2D1::RectF(0, 0, width, height);
            auto vm = BuildVm(s, false); vm.safe_mode = true;
            Check(vm.pane_slots.size() == 2, "build independent rectangles for both file panes");
            for (size_t i = 0; i < vm.pane_slots.size(); ++i) {
                auto& slot = vm.pane_slots[i];
                const auto split = s.renderer.PaneSplitButtonRect(slot.rect);
                const auto more = s.renderer.PaneViewButtonRect(slot.rect);
                const auto title = s.renderer.PaneHeaderTextRect(slot.rect);
                Check(Nonempty(split) && split.left >= slot.rect.left && split.right < more.left &&
                    title.right <= split.left - 7 * scale && title.right >= title.left,
                    "split sits before More and preserves a separate title region at every width and DPI");
                const auto point = Center(split);
                auto hit = s.renderer.HitTest(vm, bounds, static_cast<float>(point.x), static_cast<float>(point.y));
                Check(hit.region == (slot.focused ? ui::HitTestResult::SplitButton : ui::HitTestResult::PaneHeader) &&
                    hit.pane_index == static_cast<int>(i),
                    "split hit testing preserves pane identity and existing inactive-pane activation behavior");
                slot.focused = true; slot.pane.header_controls_opacity = 1;
                hit = s.renderer.HitTest(vm, bounds, static_cast<float>(point.x), static_cast<float>(point.y));
                Check(hit.region == ui::HitTestResult::SplitButton && hit.pane_index == static_cast<int>(i),
                    "each visible pane split button has its own hit target");
                slot.pane.header_controls_opacity = 0;
                hit = s.renderer.HitTest(vm, bounds, static_cast<float>(point.x), static_cast<float>(point.y));
                Check(hit.region == ui::HitTestResult::PaneHeader, "hidden split controls do not consume a header click");
                slot.focused = i == 1; slot.pane.header_controls_opacity = 1;
            }
            for (const bool dark : {false, true}) {
                vm.dark = dark;
                auto* dc = s.compositor.Dc(); dc->BeginDraw(); dc->Clear(D2D1::ColorF(0, 0));
                s.renderer.Render(vm, bounds, ui::MakeTheme(dark, s.accentColor));
                Check(SUCCEEDED(dc->EndDraw()), "render pane controls using production theme and DPI rules");
                if (scale == 1 && (dip_width == 620 || dip_width == 1500)) {
                    const auto file = std::wstring(dark ? L"dark-" : L"light-") + std::to_wstring(dip_width) + L".png";
                    Check(s.compositor.SaveSnapshot((root / file).c_str()), "save representative narrow and wide pane headers");
                }
            }
        }
        const auto narrow = D2D1::RectF(0, 0, 160 * scale, 400 * scale);
        Check(!Nonempty(s.renderer.PaneDetailsRect(narrow)) && !Nonempty(s.renderer.PaneMediumIconsRect(narrow)) &&
            Nonempty(s.renderer.PaneSplitButtonRect(narrow)) && s.renderer.PaneHeaderTextRect(narrow).right > 0,
            "narrow panes fold direct view shortcuts while retaining split and More");
    }
    s.scale = 1; s.renderer.SetScale(1); s.compositor.RecreateTextFormats(1); s.compositor.Resize(1000, 620);
    const auto vm = BuildVm(s, false);
    const auto split = s.renderer.PaneSplitButtonRect(vm.pane_slots[1].rect);
    const auto point = Center(split);
    POINT anchor{static_cast<LONG>(split.left), static_cast<LONG>(split.bottom)};
    ClientToScreen(s.hwnd, &anchor);
    Check(EnsureMenu(s), "initialize the existing split menu for a production click");
    s.menu->SetHoverFirstOnOpen(true);
    const auto timer = SetTimer(nullptr, 0, 260, SelectHorizontal);
    if (timer) HandleLButtonDown(&s, s.hwnd, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(point.x, point.y));
    if (timer) KillTimer(nullptr, timer);
    Check(captured && std::abs(captured_menu.left - anchor.x) < 5 && std::abs(captured_menu.top - anchor.y) < 12,
        "a real pane-header click anchors the existing split menu to that pane button");
    Check(layout->layout == app::LayoutPreset::TwoHorizontal && s.pane == panes[1],
        "selecting the existing menu applies its layout and preserves the clicked pane");
    s.menu.reset(); s.renderer.SetCompositor(nullptr); s.compositor.Shutdown(); DestroyWindow(s.hwnd); s.hwnd = nullptr;
    owned.reset(); OleUninitialize();
    std::printf("%d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
