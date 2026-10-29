#pragma once
#include "animation_snapshot.h"

namespace pulse::ui {
struct InteractionFeedbackUiTest {
    static bool Run(AppState& s, const std::filesystem::path& root) {
        int failures = 0;
        const auto check = [&](bool ok, const char* label) {
            std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
            failures += !ok;
        };
        auto& r = s.renderer;
        r.motion_enabled_ = true; r.hover_tracks_.clear();
        r.settings_switch_now_ = 1000;
        check(r.SelectionOpacity(L"fixture", false) == 0, "unselected row initializes without a flash");
        r.settings_switch_now_ = 1100;
        check(r.SelectionOpacity(L"fixture", true) == 0 && r.SurfaceAnimating(), "click schedules the selected-background entrance");
        r.settings_switch_now_ = 1160;
        const float middle = r.SelectionOpacity(L"fixture", true);
        check(middle > 0 && middle < 1 && r.SelectionOpacity(L"fixture", false) == middle,
            "rapid deselection preserves the current background opacity");
        r.settings_switch_now_ = 1280;
        check(r.SelectionOpacity(L"fixture", false) == 0, "selection exit reaches its exact endpoint");
        r.motion_enabled_ = false;
        check(r.SelectionOpacity(L"fixture", true) == 1, "disabled motion selects immediately");

        for (const float scale : {1.0f, 1.5f}) for (const bool dark : {false, true}) {
            s.scale = scale; r.SetScale(scale);
            s.compositor.Resize(static_cast<int>(1200 * scale), static_cast<int>(760 * scale));
            s.compositor.RecreateTextFormats(scale);
            r.motion_enabled_ = true; r.hover_tracks_.clear();
            r.SetDetailsPanelVisible(false); r.details_open_motion_ = {};
            r.TickDetailsPanelAnimation(1000, 1200 * scale, false);
            r.page_surface_.Reset();
            for (auto& surface : r.pane_surfaces_) surface.Reset();
            WindowViewModel vm;
            vm.dark = dark; vm.safe_mode = true; vm.details_motion_sampled = true;
            vm.pane.path = L"C:\\Feedback"; vm.pane.header_text = L"Feedback";
            vm.pane.view_mode = ViewMode::Details;
            for (int i = 0; i < 8; ++i) {
                ListEntryView entry;
                entry.name = L"Image " + std::to_wstring(i + 1) + L".jpg";
                entry.path = vm.pane.path + L"\\" + entry.name;
                entry.attrs = FILE_ATTRIBUTE_NORMAL;
                vm.pane.entries.push_back(entry);
            }
            const auto bounds = D2D1::RectF(0, 0, 1200 * scale, 760 * scale);
            const auto draw = [&](const wchar_t* name) {
                auto* dc = s.compositor.Dc(); dc->BeginDraw(); dc->Clear(D2D1::ColorF(0, 0));
                r.Render(vm, bounds, MakeTheme(dark, HexColor(0x0078D4)));
                const auto result = dc->EndDraw();
                if (FAILED(result)) std::printf("D2D HRESULT: %08lx\n", static_cast<unsigned long>(result));
                check(SUCCEEDED(result), "feedback frame renders without Direct2D errors");
                if (name) {
                    const auto prefix = std::wstring(dark ? L"dark-" : L"light-") + (scale == 1 ? L"100-" : L"150-");
                    check(WriteAnimationSnapshot(s.compositor, root / (prefix + name + L".png")), "capture feedback frame");
                }
            };
            draw(L"selection-empty");
            vm.pane.selected_index = 1; vm.pane.selected_count = 1;
            draw(nullptr);
            check(r.SurfaceAnimating() && vm.pane.IsRowSelected(1), "logical selection is immediate while its background animates");
            Sleep(40); draw(L"selection-row-middle");
            Sleep(140); draw(L"selection-row-end");
            check(!r.SurfaceAnimating(), "row selection stops requesting frames after settling");
            vm.pane.selected_index = 4; draw(nullptr);
            Sleep(40); draw(L"selection-row-retarget");
            Sleep(140); draw(nullptr);
            vm.pane.view_mode_direct = true;
            for (const auto mode : {ViewMode::MediumIcons, ViewMode::LargeIcons, ViewMode::ExtraLargeIcons, ViewMode::Details}) {
                vm.pane.view_mode = mode; draw(nullptr);
                check(!r.pane_surfaces_[0].pending, "wheel view changes never start repeated full-pane fades");
            }
            vm.pane.view_mode = ViewMode::MediumIcons; draw(L"wheel-icons");
            vm.pane.selected_index = 2; draw(nullptr);
            Sleep(40); draw(L"selection-icon-middle");
            Sleep(140); draw(nullptr);
            vm.pane.selected_index = -1; vm.pane.selected_count = 0; draw(nullptr);
            Sleep(40); draw(L"selection-icon-leaving");
            Sleep(140); draw(nullptr);
            check(!r.SurfaceAnimating(), "icon selection exit releases its frame chain");
            vm.pane.view_mode_direct = false; vm.pane.view_mode = ViewMode::Details; draw(nullptr);
            check(r.pane_surfaces_[0].pending, "menu view change retains its content transition");
            Sleep(50); draw(L"menu-middle"); Sleep(220); draw(nullptr);
            check(!r.pane_surfaces_[0].pending, "menu view change settles normally");
            vm.pane.view_mode_direct = true; vm.pane.path += L"\\Child"; draw(nullptr);
            check(r.pane_surfaces_[0].pending, "wheel policy does not suppress a subsequent folder navigation");
            Sleep(230); draw(nullptr);
            r.motion_enabled_ = false; vm.pane.selected_index = 3; vm.pane.selected_count = 1; draw(nullptr);
            check(!r.SurfaceAnimating(), "reduced motion removes selection and view animation work");
        }
        r.motion_enabled_ = true;
        auto& tab = *ActiveTab(s);
        SetViewMode(s, ViewMode::Details);
        PrepareViewState(s);
        auto vm = BuildVm(s, false);
        const auto body = r.ContentRect(static_cast<float>(s.compositor.Width()), static_cast<float>(s.compositor.Height()));
        POINT point{static_cast<LONG>(body.left + 100 * s.scale), static_cast<LONG>(body.top + 100 * s.scale)};
        ClientToScreen(s.hwnd, &point);
        HandleMouseWheel(&s, s.hwnd, WM_MOUSEWHEEL, MAKEWPARAM(MK_CONTROL, WHEEL_DELTA), MAKELPARAM(point.x, point.y));
        check(tab.view_mode_direct && tab.view_mode != ViewMode::Details,
            "production Ctrl-wheel path marks direct manipulation");
        app::FillPaneViewModel(vm.pane, *s.pane, &s.places);
        check(vm.pane.view_mode_direct, "pane projection carries wheel presentation policy");
        SetViewMode(s, ViewMode::Details);
        check(!tab.view_mode_direct, "menu command restores the animated presentation policy");
        return failures == 0;
    }
};
}
