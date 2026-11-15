// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.

// Included inside FluentMenuTestPeer.
static int RunScrollCadence(HWND owner, Compositor& compositor) {
    struct Probe {
        FluentMenu* menu;
        int frames = 0, intermediate = 0;
        bool monotonic = true, timed_out = false;
        float previous = 0;
        std::chrono::steady_clock::time_point first{}, last{};
    };
    int failures = 0;
    for (bool submenu : {false, true}) {
        FluentMenu menu;
        if (!menu.Create(owner, &compositor, 1)) return 1;
        menu.SetMaxVisibleRows(6);
        std::vector<FluentMenuItem> rows;
        for (int i = 0; i < 100; ++i) {
            FluentMenuItem item;
            item.command = 9000 + i; item.text = L"Scroll command " + std::to_wstring(i);
            rows.push_back(std::move(item));
        }
        if (submenu) {
            auto parent = rows.front(); parent.children = rows;
            if (!Begin(menu, {parent})) return 1;
            menu.OpenSubmenu(0);
        } else if (!Begin(menu, rows)) return 1;
        FinishAnimation(menu);
        Probe probe{&menu};
        const auto observe = [](HWND window, UINT message, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR data) -> LRESULT {
            auto& sample = *reinterpret_cast<Probe*>(data);
            if (message == EditMotionFrameMessage()) {
                const auto now = std::chrono::steady_clock::now();
                if (!sample.frames++) sample.first = now;
                sample.last = now;
                const auto& menu = *sample.menu;
                const float offset = menu.open_flyouts_ ? menu.flyouts_[0]->scroll_y : menu.scroll_y_;
                sample.intermediate += offset > 0 && offset < menu.model_.RowHeightPx() * 3;
                sample.monotonic &= offset >= sample.previous;
                sample.previous = offset;
                if (sample.frames >= 12) sample.menu->Dismiss();
            } else if (message == WM_TIMER && wp == 987) {
                sample.timed_out = true; sample.menu->Dismiss();
                return 0;
            }
            return DefSubclassProc(window, message, wp, lp);
        };
        if (!SetWindowSubclass(menu.hwnd_, observe, 987, reinterpret_cast<DWORD_PTR>(&probe)) ||
            !SetTimer(menu.hwnd_, 987, 1500, nullptr)) return 1;
        SendMessageW(submenu ? menu.flyouts_[0]->hwnd : menu.hwnd_, WM_MOUSEWHEEL,
            MAKEWPARAM(0, static_cast<WORD>(-WHEEL_DELTA)), 0);
        menu.RunModalLoop();
        KillTimer(menu.hwnd_, 987); RemoveWindowSubclass(menu.hwnd_, observe, 987);
        const bool passed = !probe.timed_out && probe.frames >= 12 && probe.intermediate >= 8 && probe.monotonic;
        failures += !passed;
        const double average = probe.frames > 1 ? std::chrono::duration<double, std::milli>(
            probe.last - probe.first).count() / (probe.frames - 1) : 0;
        std::printf("[%s] modal menu scrolling uses scheduled intermediate frames\n", passed ? "PASS" : "FAIL");
        std::printf("[MENU_CADENCE] submenu=%d frames=%d intermediate=%d interval_avg_ms=%.3f timeout=%d\n",
            submenu, probe.frames, probe.intermediate, average, probe.timed_out);
    }
    return failures ? 1 : 0;
}

static int RunScrollRendering(HWND owner, Compositor& compositor) {
    int failures = 0;
    const auto check = [&](bool passed, const char* label) {
        std::printf("[%s] %s\n", passed ? "PASS" : "FAIL", label);
        failures += !passed;
    };
    std::vector<FluentMenuItem> rows;
    for (int i = 0; i < 100; ++i) {
        FluentMenuItem item;
        item.command = 9000 + i;
        item.text = L"\u83dc\u5355\u547d\u4ee4 " + std::to_wstring(i + 1);
        item.glyph = L"\xE8A5";
        item.separator_after = i % 7 == 6;
        rows.push_back(std::move(item));
    }
    for (float scale : {1.0f, 1.5f, 2.0f}) for (bool dark : {false, true}) {
        compositor.RecreateTextFormats(scale);
        FluentMenu menu;
        check(menu.Create(owner, &compositor, scale), "create isolated menu scroll rendering fixture");
        menu.SetTheme(dark, HexColor(0x0078D4));
        menu.SetMaxVisibleRows(6);
        check(Begin(menu, rows), "render a tall menu with a six-row viewport");
        FinishAnimation(menu);
        auto parent = rows.front();
        parent.children = rows;
        menu.model_.SetItems({parent});
        menu.model_.Layout(compositor.DwriteFactory(), scale);
        menu.OpenSubmenu(0);
        check(menu.open_flyouts_ == 1, "open a scrolling child menu");
        for (bool submenu : {false, true}) {
            if (!submenu) {
                menu.HideSubWindow();
                menu.model_.SetItems(rows);
                menu.model_.Layout(compositor.DwriteFactory(), scale);
            } else {
                menu.model_.SetItems({parent});
                menu.model_.Layout(compositor.DwriteFactory(), scale);
                menu.OpenSubmenu(0);
            }
            const float body = submenu ? menu.flyouts_[0]->body_height : menu.BodyHeightPx();
            const auto before = *compositor.GetLumaTextStats();
            const auto started = std::chrono::steady_clock::now();
            bool rendered = true;
            for (int frame = 0; frame < 24; ++frame) {
                const float offset = (frame + 1) * 3.25f * scale;
                if (submenu) menu.flyouts_[0]->scroll_y = offset;
                else menu.scroll_y_ = offset;
                rendered &= submenu ? menu.RenderSub() : menu.Render();
            }
            const double average = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - started).count() / 24;
            const auto draws = compositor.GetLumaTextStats()->draw_calls - before.draw_calls;
            const auto limit = static_cast<uint64_t>(std::ceil(body / menu.model_.RowHeightPx()) + 2) * 2;
            check(rendered, "scroll frames render successfully");
            check(draws > 0 && draws <= 24 * limit, "scroll frames draw only visible menu rows");
            if (bench::StreamSnapshots()) {
                const auto& surface = submenu ? menu.flyouts_[0]->surface : menu.surf_;
                const auto path = std::filesystem::absolute(L"bench_data/menu-scroll") /
                    (std::to_wstring(static_cast<int>(scale * 100)) + (dark ? L"-dark" : L"-light") +
                     (submenu ? L"-child.png" : L"-main.png"));
                check(bench::EmitSnapshot(path, surface.w, surface.h, surface.w * 4,
                    static_cast<BYTE*>(surface.bits)), "capture scrolled menu text and separators");
            }
            std::printf("[MENU_SCROLL] dpi=%.0f dark=%d submenu=%d render_avg_ms=%.3f text_draws_per_frame=%.1f\n",
                scale * 100, dark, submenu, average, draws / 24.0);
        }
        menu.Dismiss();
    }
    std::printf("[SUMMARY] menu scroll rendering: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}
