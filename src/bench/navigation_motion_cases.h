#pragma once
#include "animation_snapshot.h"

namespace pulse::ui {
struct NavigationMotionUiTest {
    static bool Run(AppState& s, const std::filesystem::path& root) {
        int failures = 0;
        const auto check = [&](bool ok, const char* label) {
            std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
            if (!ok) ++failures;
        };
        auto& r = s.renderer;
        for (const float scale : {1.0f, 1.5f}) for (bool dark : {false, true}) {
            r.SetScale(scale);
            s.compositor.Resize(static_cast<int>(1000 * scale), static_cast<int>(720 * scale));
            s.compositor.RecreateTextFormats(scale);
            r.page_surface_.Reset(); r.settings_content_surface_.Reset();
            for (auto& surface : r.pane_surfaces_) surface.Reset();
            r.hover_tracks_.clear(); r.motion_enabled_ = true;
            WindowViewModel vm;
            vm.dark = dark; vm.safe_mode = true;
            vm.pane.path = L"C:\\Motion";
            vm.pane.view_mode = ViewMode::Details;
            for (int i = 0; i < 8; ++i) {
                ListEntryView e;
                e.name = L"Folder " + std::to_wstring(i + 1);
                e.path = vm.pane.path + L"\\" + e.name;
                e.is_dir = true;
                vm.pane.entries.push_back(e);
            }
            const auto draw = [&](const wchar_t* name) {
                auto* dc = s.compositor.Dc();
                dc->BeginDraw(); dc->Clear(D2D1::ColorF(0, 0));
                r.Render(vm, D2D1::RectF(0, 0, 1000 * scale, 720 * scale),
                    MakeTheme(dark, HexColor(0x0078D4)));
                const auto hr = dc->EndDraw();
                if (FAILED(hr)) std::printf("D2D HRESULT: %08lx\n", static_cast<unsigned long>(hr));
                check(SUCCEEDED(hr), "navigation surfaces render without Direct2D errors");
                if (name) {
                    const auto prefix = std::wstring(dark ? L"dark-" : L"light-") +
                        (scale == 1 ? L"100-" : L"150-");
                    check(WriteAnimationSnapshot(s.compositor, root / (prefix + name)), "save motion frame");
                }
            };
            draw(L"folder.png");
            check(!r.SurfaceAnimating(), "initial page settles immediately");
            vm.pane.hover_index = 1; draw(nullptr);
            check(r.SurfaceAnimating(), "entering row schedules hover fade");
            Sleep(60); draw(L"hover.png");
            vm.pane.hover_index = 3; draw(nullptr);
            check(r.SurfaceAnimating(), "moving rows preserves outgoing fade");
            vm.pane.path += L"\\Child"; vm.pane.loading = true; draw(nullptr);
            check(r.SurfaceAnimating(), "navigation retains previous visual during short load");
            vm.pane.loading = false; vm.pane.entries[0].name = L"New folder contents"; draw(nullptr);
            Sleep(65); draw(L"folder-middle.png");
            vm.pane.path = L"C:\\Motion"; draw(nullptr);
            check(r.SurfaceAnimating(), "rapid return starts from current visual");
            Sleep(300); draw(nullptr);
            check(!r.SurfaceAnimating(), "folder and hover animations release frame chain");
            vm.pane.hover_index = -1;
            vm.pane.path = L"C:\\Motion\\Bilibili.zip";
            vm.pane.header_text = L"Bilibili.zip";
            vm.pane.banner_title = L"Bilibili.zip";
            vm.pane.banner_message = L"Archive is incomplete; showing readable contents.";
            vm.pane.banner_kind = 2;
            draw(nullptr);
            check(r.pane_surfaces_[0].pending, "entering banner page preserves transition despite list-origin change");
            Sleep(65); draw(L"banner-enter-middle.png");
            Sleep(260); draw(nullptr);
            check(!r.SurfaceAnimating(), "banner page transition settles");
            vm.pane.path = L"C:\\Motion";
            vm.pane.banner_title.clear(); vm.pane.banner_message.clear();
            draw(nullptr);
            check(r.pane_surfaces_[0].pending, "leaving banner page animates the whole content panel");
            Sleep(65); draw(L"banner-leave-middle.png");
            Sleep(260); draw(nullptr);
            vm.pane.path = L"pulse:home"; vm.pane.is_home = true;
            vm.pane.network_discovery_disabled = true;
            HomeCardView home_card;
            home_card.label = L"Documents"; home_card.path = L"C:\\Motion";
            home_card.glyph = L"\xE8A5";
            vm.pane.home_cards = {home_card};
            draw(nullptr);
            check(r.pane_surfaces_[0].pending, "entering home participates in pane transition");
            Sleep(65); draw(L"home-enter-middle.png");
            Sleep(260); draw(nullptr);
            check(!r.SurfaceAnimating(), "home transition settles without recurring frames");
            vm.pane.path = L"C:\\Motion"; vm.pane.is_home = false;
            draw(nullptr);
            check(r.pane_surfaces_[0].pending, "leaving home retains previous home visual");
            Sleep(65); draw(L"home-leave-middle.png");
            Sleep(260); draw(nullptr);
            vm.settings_open = true; draw(nullptr);
            check(r.SurfaceAnimating(), "opening settings starts transition");
            Sleep(80); draw(L"settings-middle.png");
            Sleep(300); draw(L"settings.png");
            vm.settings_page = 6; draw(nullptr);
            check(r.SurfaceAnimating(), "settings selection and content animate together");
            Sleep(70); draw(L"category-middle.png");
            vm.settings_page = 0; draw(nullptr);
            Sleep(40); draw(L"category-reverse.png");
            check(r.SurfaceAnimating(), "rapid category reversal keeps shared selection moving");
            r.motion_enabled_ = false; draw(nullptr);
            check(!r.SurfaceAnimating(), "disabled motion settles page selection and hover");
            r.motion_enabled_ = true; draw(nullptr);
            vm.settings_open = false; draw(nullptr);
            check(r.SurfaceAnimating(), "closing settings transitions back to files");
            Sleep(300); draw(nullptr);
            check(!r.SurfaceAnimating(), "page animation releases frame chain");
        }
        return failures == 0;
    }
};
}
