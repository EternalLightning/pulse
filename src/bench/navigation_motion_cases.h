// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once
#include "animation_snapshot.h"
#include "../app/archive_navigation.h"
#include <optional>

namespace pulse::ui {
struct NavigationMotionUiTest {
    static std::optional<uint32_t> BackgroundPixel(AppState& s, D2D1_RECT_F bounds, float scale) {
        using Microsoft::WRL::ComPtr;
        auto* dc = s.compositor.Dc();
        ComPtr<ID2D1Image> image;
        ComPtr<ID2D1Bitmap1> target, sample;
        dc->GetTarget(&image);
        if (!image || FAILED(image.As(&target))) return {};
        const auto x = static_cast<UINT>((bounds.left + bounds.right) * 0.5f);
        const auto y = static_cast<UINT>(bounds.bottom - 24 * scale);
        const D2D1_RECT_U region{x, y, x + 1, y + 1};
        const auto properties = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW, target->GetPixelFormat());
        if (FAILED(dc->CreateBitmap(D2D1::SizeU(1, 1), nullptr, 0, properties, &sample)) ||
            FAILED(sample->CopyFromBitmap(nullptr, target.Get(), &region))) return {};
        D2D1_MAPPED_RECT mapped{};
        if (FAILED(sample->Map(D2D1_MAP_OPTIONS_READ, &mapped))) return {};
        uint32_t color = 0;
        std::memcpy(&color, mapped.bits, sizeof(color));
        sample->Unmap();
        return color;
    }

    static bool RunEmptyViewport(AppState& s) {
        int failures = 0;
        const auto check = [&](bool passed, const char* label) {
            std::printf("[%s] %s\n", passed ? "PASS" : "FAIL", label);
            failures += !passed;
        };
        auto& renderer = s.renderer;
        renderer.SetScale(1.0f);
        WindowViewModel frame;
        frame.safe_mode = true;
        // A short window can put the content bottom above its toolbar offset.
        frame.pane_slots.emplace_back().rect = D2D1::RectF(230, 180, 700, 120);
        const auto sample_bounds = D2D1::RectF(230, 0, 700, 174);
        auto* dc = s.compositor.Dc();
        dc->BeginDraw();
        dc->Clear(D2D1::ColorF(0.2f, 0.3f, 0.4f, 1.0f));
        check(SUCCEEDED(dc->EndDraw()), "prepare empty viewport boundary sample");
        const auto before = BackgroundPixel(s, sample_bounds, 1.0f);
        check(before.has_value(), "read the viewport boundary baseline pixel");
        dc->BeginDraw();
        renderer.DrawPane(frame, D2D1::RectF(0, 0, 760, 540), MakeTheme(false, HexColor(0x0078D4)));
        check(SUCCEEDED(dc->EndDraw()), "empty pane viewport draws without Direct2D errors");
        const auto after = BackgroundPixel(s, sample_bounds, 1.0f);
        check(before && after && *before == *after, "non-positive pane viewport cannot paint over the toolbar");
        return failures == 0;
    }

    template<class Check>
    static void CheckCommands(AppState& s, const std::filesystem::path& root, Check check) {
        // The fixture keeps workers stopped: exercise navigation admission and
        // error/loading paths without accessing user folders or spawning 7-Zip.
        s.searchHistory.persist = false;
        auto& tab = *ActiveTab(s);
        const auto folder = fs::NormalizePath(std::filesystem::absolute(root / L"Intent").wstring());
        const auto child = folder + L"\\Child";
        const auto archive_file = folder + L"\\Sample.zip";
        const auto archive = ArchiveViewPath(archive_file);
        const auto expect = [&](const std::wstring& path, SurfaceMotion motion, const char* label) {
            PaneViewModel pane;
            app::FillPaneViewModel(pane, *s.pane, &s.places);
            const bool matches = tab.current_path == path && pane.navigation_motion == motion &&
                pane.navigation_owner == tab.network_identity &&
                pane.navigation_revision == tab.navigation_revision;
            if (!matches) std::printf("navigation mismatch: path=%ls expected=%ls motion=%d expected=%d\n",
                tab.current_path.c_str(), path.c_str(), static_cast<int>(pane.navigation_motion), static_cast<int>(motion));
            check(matches, label);
        };
        const auto select = [&](std::wstring name, bool directory, std::wstring full_path = {}) {
            auto entries = std::make_shared<std::vector<fs::DirEntry>>(1);
            entries->front().name = std::move(name);
            entries->front().full_path = std::move(full_path);
            entries->front().is_dir = directory;
            entries->front().attrs = directory ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
            tab.SetSnapshot(entries);
            tab.loading = false;
            tab.SelectOnly(0);
        };
        NavigateTo(s, folder);
        expect(folder, SurfaceMotion::Fade, "direct location jump fades");
        select(L"Child", true);
        OpenSelected(s);
        expect(child, SurfaceMotion::Forward, "opening a folder moves left");
        GoUp(s);
        expect(folder, SurfaceMotion::Backward, "up moves right");
        GoBack(s);
        expect(child, SurfaceMotion::Backward, "back into a deeper folder still moves right");
        GoForward(s);
        expect(folder, SurfaceMotion::Forward, "forward to an ancestor still moves left");
        NavigateTo(s, child);
        expect(child, SurfaceMotion::Fade, "address jump to a descendant stays neutral");
        NavigateTo(s, folder);
        expect(folder, SurfaceMotion::Fade, "address jump to an ancestor stays neutral");
        tab.current_path = app::MakeSearchPath(L"needle");
        select(L"Child", true, child);
        OpenSelected(s);
        expect(child, SurfaceMotion::Fade, "opening a search result is a neutral location jump");
        NavigateTo(s, folder);
        select(L"Sample.zip", false);
        OpenSelected(s);
        expect(archive, SurfaceMotion::Forward, "opening an archive moves left");
        select(L"Child", true);
        OpenSelected(s);
        expect(ArchiveViewPath(archive_file, L"Child/"), SurfaceMotion::Forward,
            "opening an archive member folder moves left");
        GoUp(s);
        expect(archive, SurfaceMotion::Backward, "leaving an archive member folder moves right");
        GoUp(s);
        expect(folder, SurfaceMotion::Backward, "leaving an archive root moves right");
        GoBack(s);
        expect(archive, SurfaceMotion::Backward, "history back into an archive moves right");
        GoForward(s);
        expect(folder, SurfaceMotion::Forward, "history forward out of an archive moves left");
        const auto revision = tab.navigation_revision;
        NavigateTo(s, folder, SurfaceMotion::Backward);
        GoForward(s);
        check(tab.navigation_revision == revision && tab.navigation_motion == SurfaceMotion::Forward,
            "same destination and unavailable history never enqueue a transition");
        s.watches.Stop();
    }

    template<class Check>
    static void CheckSurface(AppState& s, Check check) {
        SurfaceTransition surface;
        auto* dc = s.compositor.Dc();
        uint64_t owner = 1, revision = 0;
        const auto paint = [&](std::wstring key, SurfaceMotion motion, bool ready, uint64_t now,
                               std::wstring_view variant = L"0", bool animate = true) {
            dc->BeginDraw();
            surface.Paint(dc, D2D1::RectF(0, 0, 500, 400), key, ready, now, animate, 16, 200,
                [&] { dc->Clear(D2D1::ColorF(0, 0)); }, variant, false, motion, owner, revision);
            check(SUCCEEDED(dc->EndDraw()), "explicit navigation surface renders without D2D errors");
        };
        paint(L"C:\\A\\B", SurfaceMotion::Fade, true, 1000);
        ++revision;
        paint(L"C:\\A", SurfaceMotion::Forward, true, 1100);
        check(surface.pending && surface.direction_ == 1, "renderer honors forward even to a path prefix");
        ++revision;
        paint(L"pulse:archive:C:\\A\\Sample.zip|", SurfaceMotion::Backward, true, 1150);
        check(surface.pending && surface.direction_ == -1, "rapid back into an archive reverses immediately");
        ++revision;
        paint(L"C:\\A", SurfaceMotion::Backward, true, 1170);
        check(surface.direction_ == -1, "archive-to-filesystem boundary retains explicit exit direction");
        ++revision;
        paint(L"C:\\A\\B", SurfaceMotion::Fade, true, 1200);
        check(surface.pending && surface.direction_ == 0, "jump to a descendant has no translation");
        ++owner;
        paint(L"C:\\Other", SurfaceMotion::Forward, true, 1250);
        check(surface.direction_ == 0, "switching tabs cannot replay a saved forward direction");
        paint(L"C:\\Other", SurfaceMotion::Forward, true, 1500);
        ++revision;
        paint(L"C:\\Other", SurfaceMotion::Backward, true, 1510);
        check(surface.pending && surface.direction_ == -1, "coalesced return to the same path keeps the latest command");
        paint(L"C:\\Other", SurfaceMotion::Backward, true, 1800);
        paint(L"C:\\Other", SurfaceMotion::Backward, false, 1810);
        paint(L"C:\\Other", SurfaceMotion::Backward, true, 1820);
        check(!surface.pending, "refresh completion cannot replay the last navigation");
        ++revision;
        paint(L"C:\\Slow", SurfaceMotion::Forward, false, 1900);
        paint(L"C:\\Slow", SurfaceMotion::Forward, false, 2020);
        const auto loading_start = surface.started_;
        paint(L"C:\\Slow", SurfaceMotion::Forward, true, 2070);
        check(surface.started_ == loading_start && surface.direction_ == 1,
            "arrival during a slow loading transition does not restart or reverse it");
        paint(L"C:\\Slow", SurfaceMotion::Forward, false, 2250);
        paint(L"C:\\Slow", SurfaceMotion::Forward, true, 2300);
        check(!surface.pending && surface.started_ == loading_start,
            "late results after a settled loading surface update in place");
        ++revision;
        paint(L"C:\\Quick", SurfaceMotion::Backward, false, 2400);
        check(surface.waiting_ && surface.direction_ == -1, "brief loading retains the outgoing surface and intent");
        paint(L"C:\\Quick", SurfaceMotion::Backward, true, 2450);
        check(!surface.waiting_ && surface.started_ == 2450 && surface.direction_ == -1,
            "brief loading starts its single transition when ready");
        paint(L"C:\\Quick", SurfaceMotion::Backward, true, 2700);
        paint(L"C:\\Quick", SurfaceMotion::Backward, true, 2710, L"1");
        check(surface.direction_ == 0, "view mode changes cannot reuse a navigation direction");
        ++revision;
        paint(L"", SurfaceMotion::Backward, true, 2720);
        check(surface.direction_ == -1, "returning to This PC keeps the exit direction with an empty address");
        paint(L"C:\\Disabled", SurfaceMotion::Forward, false, 2730, L"1", false);
        check(!surface.pending && !surface.previous_.get() && !surface.current_.get(),
            "disabled motion releases navigation surfaces immediately");
    }

    static bool Run(AppState& s, const std::filesystem::path& root) {
        int failures = 0;
        const auto check = [&](bool ok, const char* label) {
            std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
            if (!ok) ++failures;
        };
        check(RunEmptyViewport(s), "empty viewport material boundary regression");
        CheckCommands(s, root, check);
        CheckSurface(s, check);
        auto& r = s.renderer;
        for (const float scale : {1.0f, 1.5f}) for (bool dark : {false, true}) {
            r.SetScale(scale);
            s.compositor.Resize(static_cast<int>(1000 * scale), static_cast<int>(720 * scale));
            s.compositor.RecreateTextFormats(scale);
            r.page_surface_.Reset(); r.settings_content_surface_.Reset();
            r.address_path_surface_.Reset();
            for (auto& surface : r.pane_surfaces_) surface.Reset();
            for (auto& surface : r.pane_title_surfaces_) surface.Reset();
            r.hover_tracks_.clear(); r.motion_enabled_ = true;
            WindowViewModel vm;
            vm.dark = dark; vm.safe_mode = true;
            vm.pane.path = L"C:\\Motion";
            vm.pane.header_text = L"Motion";
            vm.pane.navigation_owner = 1;
            vm.pane.is_file_system = true;
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
            const auto pane_bounds = r.ContentRect(1000 * scale, 720 * scale);
            const auto prefix = std::wstring(dark ? L"dark-" : L"light-") + (scale == 1 ? L"100" : L"150");
            std::printf("REGION:%ls:columns:%d,%d,%d,%d\n", prefix.c_str(),
                static_cast<int>(pane_bounds.left + 4 * scale),
                static_cast<int>(pane_bounds.top + r.pane_header_height_ + 2 * scale),
                static_cast<int>(pane_bounds.right - 4 * scale),
                static_cast<int>(pane_bounds.top + r.pane_header_height_ + r.column_header_height_ - 2 * scale));
            const auto address = r.AddressBarRect(1000 * scale);
            std::printf("REGION:%ls:path-border:%d,%d,%d,%d\n", prefix.c_str(),
                static_cast<int>(address.left + 16 * scale), static_cast<int>(address.top),
                static_cast<int>(address.right - 16 * scale), static_cast<int>(address.top + 2 * scale));
            const auto background = BackgroundPixel(s, pane_bounds, scale);
            check(background.has_value(), "read the stationary pane material including alpha");
            const auto check_background = [&] {
                const auto actual = BackgroundPixel(s, pane_bounds, scale);
                check(background && actual && *background == *actual,
                    "pane background color and opacity remain fixed during navigation");
            };
            vm.pane.hover_index = 1; draw(nullptr);
            check(r.SurfaceAnimating(), "entering row schedules hover fade");
            Sleep(60); draw(L"hover.png");
            vm.pane.hover_index = 3; draw(nullptr);
            check(r.SurfaceAnimating(), "moving rows preserves outgoing fade");
            vm.pane.path += L"\\Child"; vm.pane.navigation_motion = SurfaceMotion::Forward;
            vm.pane.header_text = L"Child";
            ++vm.pane.navigation_revision; vm.pane.loading = true; draw(nullptr);
            check(r.pane_surfaces_[0].direction_ == 1, "folder opens from the right");
            check(r.SurfaceAnimating(), "navigation retains previous visual during short load");
            check(r.pane_title_surfaces_[0].pending && r.pane_title_surfaces_[0].direction_ == 1,
                "folder title transitions separately from the fixed header controls");
            check(r.address_path_surface_.pending && r.address_path_surface_.direction_ == 1,
                "breadcrumb text starts its own forward transition");
            vm.pane.loading = false; vm.pane.entries[0].name = L"New folder contents"; draw(nullptr);
            Sleep(65); draw(L"folder-middle.png");
            check_background();
            vm.pane.path = L"C:\\Motion"; vm.pane.navigation_motion = SurfaceMotion::Backward;
            vm.pane.header_text = L"Motion";
            ++vm.pane.navigation_revision; draw(nullptr);
            check(r.pane_surfaces_[0].direction_ == -1, "returning to the parent moves right");
            check(r.pane_title_surfaces_[0].direction_ == -1 && r.address_path_surface_.direction_ == -1,
                "rapid return reverses both folder title and breadcrumb text");
            check(r.SurfaceAnimating(), "rapid return starts from current visual");
            Sleep(300); draw(nullptr);
            check(!r.SurfaceAnimating(), "folder and hover animations release frame chain");
            check(!r.address_path_surface_.pending && !r.pane_title_surfaces_[0].pending,
                "title and breadcrumb transitions settle without extra frames");
            vm.pane.hover_index = -1;
            vm.pane.path = ArchiveViewPath(L"C:\\Motion\\Bilibili.zip");
            vm.pane.navigation_motion = SurfaceMotion::Forward;
            ++vm.pane.navigation_revision;
            vm.pane.header_text = L"Bilibili.zip";
            vm.pane.banner_title = L"Bilibili.zip";
            vm.pane.banner_message = L"Archive is incomplete; showing readable contents.";
            vm.pane.banner_kind = 2;
            draw(nullptr);
            check(r.pane_surfaces_[0].direction_ == 1, "archive opens with the folder entry direction");
            check(r.pane_surfaces_[0].pending, "entering banner page preserves transition despite list-origin change");
            Sleep(65); draw(L"banner-enter-middle.png");
            check_background();
            Sleep(260); draw(nullptr);
            check(!r.SurfaceAnimating(), "banner page transition settles");
            vm.pane.path = L"C:\\Motion";
            vm.pane.navigation_motion = SurfaceMotion::Backward;
            ++vm.pane.navigation_revision;
            vm.pane.header_text = L"Motion";
            vm.pane.banner_title.clear(); vm.pane.banner_message.clear();
            draw(nullptr);
            check(r.pane_surfaces_[0].direction_ == -1, "archive exits opposite to entry");
            check(r.pane_surfaces_[0].pending, "leaving banner page animates its items");
            Sleep(65); draw(L"banner-leave-middle.png");
            check_background();
            Sleep(260); draw(nullptr);
            vm.pane.path = L"pulse:home"; vm.pane.is_home = true;
            vm.pane.navigation_motion = SurfaceMotion::Fade;
            ++vm.pane.navigation_revision;
            vm.pane.network_discovery_disabled = true;
            HomeCardView home_card;
            home_card.label = L"Documents"; home_card.path = L"C:\\Motion";
            home_card.glyph = L"\xE8A5";
            vm.pane.home_cards = {home_card};
            draw(nullptr);
            check(r.pane_surfaces_[0].pending, "entering home participates in pane transition");
            check(r.address_path_surface_.pending && r.address_path_surface_.direction_ == 0,
                "direct jumps crossfade breadcrumb text without translation");
            Sleep(65); draw(L"home-enter-middle.png");
            Sleep(260); draw(nullptr);
            check(!r.SurfaceAnimating(), "home transition settles without recurring frames");
            vm.pane.path = L"C:\\Motion"; vm.pane.is_home = false;
            draw(nullptr);
            check(r.pane_surfaces_[0].pending, "leaving home retains previous home visual");
            Sleep(65); draw(L"home-leave-middle.png");
            Sleep(260); draw(nullptr);
            vm.pane.view_mode = ViewMode::MediumIcons;
            ++vm.pane.view_generation; ++vm.pane.view_mode_revision; draw(nullptr);
            check(r.view_mode_transitions_[0].Pending() && !r.pane_surfaces_[0].pending,
                "view mode change animates items without another navigation transition");
            check(!r.address_path_surface_.pending, "view mode change leaves the breadcrumb stationary");
            Sleep(110); draw(L"view-mode-middle.png");
            check_background();
            vm.pane.view_mode = ViewMode::Details;
            ++vm.pane.view_generation; ++vm.pane.view_mode_revision; draw(nullptr);
            Sleep(260); draw(nullptr);
            check(!r.view_mode_transitions_[0].Pending(), "returning to details releases the view mode transition");
            vm.address_editing = true; vm.address_path_animation = 0.45f; draw(nullptr);
            check(!r.address_path_surface_.pending && !r.address_path_surface_.current_.get(),
                "opening the path editor releases the breadcrumb navigation cache");
            vm.address_editing = false; vm.address_path_animation = 0.2f; draw(nullptr);
            check(!r.address_path_surface_.pending,
                "closing the path editor uses its existing animation without a second navigation fade");
            vm.address_path_animation = 0; draw(nullptr);
            check(!r.address_path_surface_.pending, "returning from the path editor primes a stationary breadcrumb");
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
