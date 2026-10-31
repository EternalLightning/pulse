// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "../app/app_internal.h"
#include "../ui/ui_renderer_internal.h"
#include "animation_snapshot.h"
#include <cstdio>
#include <filesystem>

namespace {
int failures = 0;
void Check(bool passed, const char* message) {
    std::printf("[%s] %s\n", passed ? "PASS" : "FAIL", message);
    failures += !passed;
}

pulse::ui::SidebarGroup Access(bool project, const wchar_t* path = L"C:\\fixture\\pulse") {
    pulse::ui::SidebarGroup group;
    group.id = static_cast<int>(pulse::app::SidebarSectionId::QuickAccess);
    group.header = L"Quick access";
    pulse::ui::SidebarItem desktop;
    desktop.path = L"C:\\fixture\\Desktop";
    desktop.label = L"Desktop";
    desktop.icon_glyph = L"\xE7F4";
    group.items.push_back(desktop);
    if (project) {
        auto row = desktop;
        row.path = path;
        row.label = L"Project pulse";
        row.badge = L"Git";
        row.icon_glyph = L"\xE8B7";
        row.transient_project = true;
        group.items.push_back(row);
    }
    return group;
}

void CheckTracks() {
    using pulse::app::SidebarProjectAnimation;
    SidebarProjectAnimation animation;
    auto group = Access(true);
    animation.Apply(group, 1000, true);
    Check(group.items.back().presence == 1 && !animation.Pending(), "initial project is visible without replaying entry");
    group = Access(false); animation.Apply(group, 1100, true);
    Check(group.items.size() == 2 && group.items.back().exiting &&
        group.items.back().presence == 1 && animation.Pending(), "exit retains the row and requests the next frame at zero elapsed time");
    group = Access(false); animation.Apply(group, 1160, true);
    const float middle = group.items.back().presence;
    Check(middle > 0 && middle < 1, "exit advances opacity and height together");
    group = Access(true); animation.Apply(group, 1160, true);
    Check(group.items.size() == 2 && !group.items.back().exiting && group.items.back().presence == middle,
        "rapid return reverses from the current sample without duplication");
    group = Access(true); animation.Apply(group, 1220, true);
    Check(group.items.back().presence > middle, "reversed row grows toward full visibility");
    group = Access(true); animation.Apply(group, 1400, true);
    Check(!animation.Pending() && group.items.back().presence == 1, "reversal settles and releases scheduling");
    group = Access(true, L"C:\\fixture\\other"); animation.Apply(group, 1500, true);
    Check(group.items.size() == 3 && group.items[1].exiting && group.items[2].presence == 0,
        "switching repositories retains the old row while the new one enters");
    group = Access(true, L"C:\\fixture\\other"); animation.Apply(group, 1700, true);
    Check(group.items.size() == 2 && group.items.back().presence == 1 && !animation.Pending(),
        "repository switch removes its ghost at the endpoint");
    group = Access(false); animation.Apply(group, 1800, true);
    group = Access(false); animation.Apply(group, 1900, false);
    Check(group.items.size() == 1 && !animation.Pending(), "disabled motion immediately removes outgoing rows");
    group = Access(true); animation.Apply(group, 2000, true);
    Check(group.items.back().presence == 0 && animation.Pending(), "entering after absence starts at zero and wakes animation");
    group = Access(true); animation.Apply(group, 2010, false);
    Check(group.items.back().presence == 1 && !animation.Pending(), "disabled motion immediately completes entry");
    group = Access(true); group.items.back().transient_project = false;
    animation.Apply(group, 2100, true);
    Check(group.items.size() == 2 && !animation.Pending(), "a permanent pin replaces the contextual row without a ghost");
    group = Access(false); animation.Apply(group, 2200, true);
    Check(group.items.size() == 1 && !animation.Pending(), "permanent pins never become retained project rows");
    group = Access(true); animation.Apply(group, 2300, true);
    group = Access(false); group.hidden = true; animation.Apply(group, 2310, true);
    Check(!animation.Pending() && group.items.size() == 1, "hidden sections discard project tracks");
    group = Access(true); animation.Apply(group, 2400, false);
    group = Access(false); group.collapsed = true; animation.Apply(group, 2410, true);
    Check(!animation.Pending() && group.items.size() == 1, "folded sections leave visibility to the group animation");
}
}

int main(int argc, char** argv) {
    using namespace pulse;
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    CheckTracks();
    OleInitialize(nullptr);
    l10n::Initialize(GetModuleHandleW(nullptr), L"en-US");
    const auto output = argc > 1 ? std::filesystem::path(argv[1]) :
        std::filesystem::path(L"bench_data/sidebar-project-animation");
    std::filesystem::create_directories(output);
    auto owned = std::make_unique<AppState>();
    auto& s = *owned;
    s.isolatedTest = true;
    s.shot.active = true;
    s.appPrefs.persist = s.ctxMenuPrefs.persist = s.places.persist = false;
    SetEnvironmentVariableW(L"PULSE_TEST_HIDDEN_SHOT", L"1");
    s.hwnd = CreateWindowExW(WS_EX_NOACTIVATE, L"STATIC", L"Sidebar project fixture",
        WS_POPUP, 0, 0, 1000, 800, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!s.hwnd || !s.compositor.Init(s.hwnd)) {
        Check(false, "initialize sidebar compositor");
        return 1;
    }
    s.renderer.SetCompositor(&s.compositor);
    s.renderer.SetDetailsPanelVisible(false);
    for (float scale : {1.0f, 1.5f}) for (bool dark : {false, true}) for (bool rail : {false, true}) {
        s.renderer.SetScale(scale);
        s.compositor.Resize(static_cast<UINT>(1000 * scale), static_cast<UINT>(800 * scale));
        s.compositor.RecreateTextFormats(scale);
        const auto rect = D2D1::RectF(0, 0, 1000 * scale, 800 * scale);
        ui::WindowViewModel vm;
        vm.dark = dark;
        vm.pane.path = L"C:\\fixture\\elsewhere";
        auto next = Access(false);
        next.id = static_cast<int>(app::SidebarSectionId::Workspaces);
        next.header = L"Workspaces";
        next.items.front().label = L"Following row";
        app::SidebarProjectAnimation animation;
        auto access = Access(true); animation.Apply(access, 1000, true);
        const float width = rail ? ui::kSidebarRailWidthDip * scale : 224 * scale;
        // Layout is shared with painting and hit testing, including rail mode.
        const auto bounds = D2D1::RectF(0, 100 * scale, width, 740 * scale);
        float last_top = 0;
        for (uint64_t elapsed : {0ull, 60ull, 200ull}) {
            access = Access(false); animation.Apply(access, 1100 + elapsed, true);
            vm.sidebar = {access, next};
            std::vector<ui::SidebarSlot> slots;
            ui::LayoutSidebar(vm, bounds, scale, slots);
            auto following = std::find_if(slots.begin(), slots.end(), [](const auto& slot) {
                return slot.group == 1;
            });
            Check(following != slots.end(), "following section remains laid out during project exit");
            if (following != slots.end()) {
                Check(!last_top || following->rc.top < last_top, "following section moves upward throughout exit");
                last_top = following->rc.top;
            }
            if (rail) continue;
            auto* dc = s.compositor.Dc();
            dc->BeginDraw(); dc->Clear(D2D1::ColorF(0, 0));
            s.renderer.Render(vm, rect, ui::MakeTheme(dark, ui::HexColor(0x0078D4)));
            Check(SUCCEEDED(dc->EndDraw()), "project fade paints without Direct2D errors");
            D2D1_RECT_F row{};
            if (s.renderer.SidebarRowRect(vm, rect.right, rect.bottom,
                static_cast<int>(app::SidebarSectionId::QuickAccess), 1, &row)) {
                const auto hit = s.renderer.HitTest(vm, rect, row.left + 10 * scale, row.top + scale);
                Check(hit.region != ui::HitTestResult::SidebarItem, "outgoing project cannot be clicked");
            }
            const auto filename = std::wstring(dark ? L"dark-" : L"light-") +
                (scale == 1 ? L"100-" : L"150-") + std::to_wstring(elapsed) + L".png";
            Check(WriteAnimationSnapshot(s.compositor, output / filename), "save actual sidebar animation frame");
        }
    }
    s.scale = 1; s.renderer.SetScale(1);
    s.window_tabs.EnsureDefault(); s.pane = s.window_tabs.Active()->FocusedPane();
    auto& tab = *ActiveTab(s);
    tab.current_path = L"C:\\fixture\\pulse"; tab.git_root = tab.current_path; tab.loading = false;
    s.shot_high_contrast = true; Render(s);
    tab.current_path = app::MakeHomePath(); tab.git_root.clear();
    s.shot_high_contrast = false;
    Check(Render(s) && s.sidebarProjectAnimation.Pending() && s.ui_schedule.animation_active,
        "production navigation schedules the contextual project exit from its first frame");
    s.shot_high_contrast = true; Render(s);
    Check(!s.sidebarProjectAnimation.Pending(), "production motion toggle releases project scheduling");
    s.watches.Stop();
    DestroyWindow(s.hwnd); s.hwnd = nullptr;
    owned.reset(); OleUninitialize();
    std::printf("sidebar project animation: %d failures\n", failures);
    return failures ? 1 : 0;
}
