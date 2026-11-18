// SPDX-License-Identifier: Apache-2.0
#include "../app/app_internal.h"
#include "../app/app_input.h"
#include "../common/user_storage.h"
#include "../ui/home_layout.h"
#include "home_quick_access_model_checks.h"
#include "home_quick_access_settings_checks.h"
#include <filesystem>
#include <cstdio>
#include <cstring>

namespace {
int passed = 0, failed = 0;
void Check(bool value, const char* label) {
    std::printf("[%s] %s\n", value ? "PASS" : "FAIL", label);
    value ? ++passed : ++failed;
}
}
int main(int argc, char** argv) {
    using namespace pulse;
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    OleInitialize(nullptr);
    const auto root = std::filesystem::absolute(L"../bench_data/quick-access-2026-11-17");
    std::filesystem::create_directories(root / L"profile");
    storage::OverrideDefaultRootForTesting((root / L"profile").wstring());
    l10n::Initialize(GetModuleHandleW(nullptr), L"zh-CN");
    if (argc < 2 || std::strcmp(argv[1], "--home-only") != 0) {
        bench::RunHomeQuickAccessModelChecks(root, Check);
        bench::RunHomeQuickAccessSettingsChecks(root, Check);
    }
    {
        auto owned = std::make_unique<AppState>(); auto& s = *owned;
        s.isolatedTest = true; s.appPrefs.persist = false;
        s.places.persist = s.ctxMenuPrefs.persist = s.searchHistory.persist = false;
        s.appPrefs.auto_folder_size = false;
        s.accentColor = ui::HexColor(0x0078D4);
        s.window_tabs.EnsureDefault(); s.pane = s.window_tabs.Active()->FocusedPane();
        auto* tab = ActiveTab(s);
        tab->current_path = app::MakeHomePath(); tab->loading = false;
        tab->virtual_title = l10n::Get(l10n::StringId::Home);
        s.pane->header_animation.opacity = 1;
        s.hwnd = CreateWindowExW(WS_EX_NOACTIVATE, L"STATIC", L"Home quick access entry fixture", WS_POPUP,
            -30000, -30000, 1480, 900, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        Check(s.hwnd && s.compositor.Init(s.hwnd), "initialize isolated production quick access home renderer");
        if (s.compositor.Dc()) {
            s.renderer.SetCompositor(&s.compositor);
            const auto folder = root / L"项目资料";
            std::filesystem::create_directories(folder);
            s.appPrefs.home_quick_access = {};
            s.appPrefs.home_quick_access.hidden = (1u << 1) | (1u << 7);
            s.appPrefs.home_quick_access.AddFolder(folder.wstring());
            for (const bool english : {false, true}) {
                s.appPrefs.language = english ? L"en-US" : L"zh-CN";
                l10n::SetLanguage(s.appPrefs.language);
                s.darkMode = !english; s.scale = english ? 1.0f : 1.75f;
                const int dip_width = english ? 760 : 1480;
                const int width = static_cast<int>(dip_width*s.scale), height = static_cast<int>(900*s.scale);
                SetWindowPos(s.hwnd, nullptr, -30000, -30000, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
                s.compositor.Resize(width, height); s.compositor.RecreateTextFormats(s.scale); s.renderer.SetScale(s.scale);
                s.home_catalog.Refresh(s.appPrefs.home_quick_access);
                ui::WindowViewModel vm;
                const auto deadline = GetTickCount64()+12000;
                do {
                    vm = BuildVm(s, false);
                    const auto recycle = std::find_if(vm.pane.home_cards.begin(), vm.pane.home_cards.end(),
                        [](const auto& card) { return card.path == app::MakeRecyclePath(); });
                    if (recycle != vm.pane.home_cards.end() && recycle->label == l10n::Get(l10n::StringId::RecycleBin)) break;
                    Sleep(10);
                } while (GetTickCount64() < deadline);
                Check(l10n::Get(l10n::StringId::HomeLibraries) == (english ? L"Quick access" : L"快捷访问"),
                    "home section heading follows the renamed quick access label");
                Check(std::any_of(vm.pane.home_cards.begin(), vm.pane.home_cards.end(), [&](const auto& card) {
                    return card.path == folder.wstring(); }) &&
                    std::none_of(vm.pane.home_cards.begin(), vm.pane.home_cards.end(), [](const auto& card) {
                        return card.path == fs::kApplicationsPath;
                    }), "production home model applies custom folders and special-location visibility");
                const auto pane_bounds = vm.pane_slots.empty() ? s.renderer.ContentRect(static_cast<float>(width),
                    static_cast<float>(height)) : vm.pane_slots.front().rect;
                const auto layout = ui::MakeHomeLayout(vm.pane, pane_bounds, s.scale);
                const auto hit = s.renderer.HitTest(vm, D2D1::RectF(0,0,static_cast<float>(width),static_cast<float>(height)),
                    (layout.customize.left+layout.customize.right)/2, (layout.customize.top+layout.customize.bottom)/2);
                Check(hit.region == ui::HitTestResult::HomeCustomize && layout.customize.left > layout.headings[0].left,
                    "home quick access customization has a separate visible hit target");
                vm.safe_mode = true;
                vm.hover_region = ui::HitTestResult::HomeCustomize;
                vm.hover_pane_index = 0;
                for (int frame = 0; frame < 3; ++frame) {
                    auto* dc = s.compositor.Dc(); dc->BeginDraw(); dc->Clear(D2D1::ColorF(0,0));
                    s.renderer.Render(vm, D2D1::RectF(0,0,static_cast<float>(width),static_cast<float>(height)),
                        ui::MakeTheme(s.darkMode, s.accentColor));
                    Check(SUCCEEDED(dc->EndDraw()), "render configured home quick access with native folder icons");
                    if (frame < 2) Sleep(150);
                }
                Check(s.compositor.SaveSnapshot((root / (english ? L"home-en-light-760.png" : L"home-zh-dark-175.png")).c_str()),
                    "save real home customization entry and filtered cards screenshot");
                if (english) {
                    const int x = static_cast<int>((layout.customize.left+layout.customize.right)/2);
                    const int y = static_cast<int>((layout.customize.top+layout.customize.bottom)/2);
                    HandleLButtonDown(&s, s.hwnd, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(x,y));
                    Check(s.settings.page() == 11 && ActiveTab(s) &&
                        ActiveTab(s)->current_path == app::MakeSettingsPath(L"quick-access"),
                        "clicking home Customize opens the actual quick access settings tab");
                    SwitchTab(s, 0);
                }
            }
            s.appPrefs.home_quick_access = {}; s.appPrefs.home_quick_access.hidden = model::kHomeQuickAccessHiddenMask;
            s.home_catalog.Refresh(s.appPrefs.home_quick_access);
            auto empty = BuildVm(s, false); empty.safe_mode = true;
            const auto width = static_cast<float>(s.compositor.Width()), height = static_cast<float>(s.compositor.Height());
            const auto pane_bounds = empty.pane_slots.empty() ? s.renderer.ContentRect(width,height) : empty.pane_slots.front().rect;
            const auto layout = ui::MakeHomeLayout(empty.pane, pane_bounds, s.scale);
            Check(std::none_of(empty.pane.home_cards.begin(), empty.pane.home_cards.end(),
                [](const auto& card) { return card.group == 0; }), "hiding all quick access cards takes effect immediately");
            empty.pane.home_loading = false;
            for (auto& slot : empty.pane_slots) slot.pane.home_loading = false;
            auto* dc = s.compositor.Dc(); dc->BeginDraw(); dc->Clear(D2D1::ColorF(0,0));
            s.renderer.Render(empty, D2D1::RectF(0,0,width,height), ui::MakeTheme(s.darkMode,s.accentColor));
            Check(SUCCEEDED(dc->EndDraw()) && s.compositor.SaveSnapshot((root / L"home-empty-quick-access.png").c_str()),
                "empty quick access remains a readable state with a customization entry");
            empty.pane.home_collapsed_mask |= 1; empty.pane.home_expansion[0] = -1;
            for (auto& slot : empty.pane_slots) { slot.pane.home_collapsed_mask |= 1; slot.pane.home_expansion[0] = -1; }
            const auto hit = s.renderer.HitTest(empty, D2D1::RectF(0,0,width,height),
                (layout.customize.left+layout.customize.right)/2, (layout.customize.top+layout.customize.bottom)/2);
            Check(hit.region == ui::HitTestResult::HomeCustomize,
                "customization stays reachable when quick access is empty and collapsed");
            s.renderer.SetCompositor(nullptr); s.compositor.Shutdown();
        }
        if (s.hwnd) DestroyWindow(s.hwnd); s.hwnd = nullptr;
    }
    OleUninitialize();
    std::printf("%d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
