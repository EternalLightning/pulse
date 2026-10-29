#pragma once
#include "../ui/home_layout.h"
#include "../ui/ui_renderer_internal.h"
#include "animation_snapshot.h"
#include <string_view>

inline bool RunHomeSettingsAnimationChecks(pulse::AppState& s, const std::filesystem::path& fixture_root) {
    using namespace pulse;
    const auto& root = fixture_root;
    std::wprintf(L"Animation snapshots: %ls\n", root.c_str());
    int failures = 0;
    const auto check = [&](bool ok, const char* label) {
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
        if (!ok) ++failures;
    };
    ui::BinaryTransition transition;
    transition.Tick(false, 1000, true, 160);
    check(transition.value == 0 && !transition.Pending(), "initial switch state appears without an entrance animation");
    transition.Tick(true, 2000, true, 160);
    check(transition.value == 0 && transition.Pending(), "opening schedules the next frame at zero elapsed time");
    transition.Tick(true, 2080, true, 160);
    const float middle = transition.value;
    transition.Tick(false, 2080, true, 160);
    check(middle > 0 && middle < 1 && transition.value == middle, "reversing a switch preserves the current visible position");
    transition.Tick(false, 2240, true, 160);
    check(transition.value == 0 && !transition.Pending(), "closing reaches its endpoint and releases the frame chain");
    transition.Tick(true, 2300, false, 160);
    check(transition.value == 1 && !transition.Pending(), "disabled motion snaps to the actual switch state");

    ui::PaneViewModel home;
    home.is_home = true;
    home.path = app::MakeHomePath();
    home.network_discovery_disabled = true;
    for (int group = 0; group < 3; ++group) {
        for (int item = 0; item < (group == 0 ? 5 : 3); ++item) {
            ui::HomeCardView card;
            card.label = group == 0 ? L"Library" : L"Fixture drive";
            card.path = L"X:\\";
            card.glyph = group == 0 ? L"\xE8B7" : L"\xE7F1";
            card.group = group;
            if (group) { card.detail = L"120 GB available, 240 GB total"; card.used_ratio = 0.5f; }
            home.home_cards.push_back(std::move(card));
        }
    }
    for (float scale : {1.0f, 1.5f, 2.0f}) for (float width : {300.0f, 760.0f, 1200.0f}) {
        const auto bounds = D2D1::RectF(0, 0, width * scale, 1600 * scale);
        for (int group = 0; group < 3; ++group) {
            home.home_collapsed_mask = 0;
            home.home_expansion = {-1, -1, -1};
            const auto full = ui::MakeHomeLayout(home, bounds, scale);
            home.home_expansion[group] = 0.5f;
            const auto partial = ui::MakeHomeLayout(home, bounds, scale);
            home.home_expansion[group] = 0;
            const auto folded = ui::MakeHomeLayout(home, bounds, scale);
            check(folded.height < partial.height && partial.height < full.height,
                "animated home extent lies between folded and expanded layouts at each width and DPI");
            if (group < 2) check(partial.headings[group + 1].top < full.headings[group + 1].top &&
                partial.headings[group + 1].top > folded.headings[group + 1].top &&
                partial.content_clips[group].bottom < partial.headings[group + 1].top,
                "following group moves continuously without overlapping revealed content");
            home.home_expansion[group] = 0.5f;
            home.home_collapsed_mask = 1u << group;
            bool safe = true;
            for (const auto& card : partial.cards) {
                if (home.home_cards[card.index].group != group) continue;
                safe &= ui::HitHomeCard(home, bounds, scale, (card.rect.left + card.rect.right) / 2,
                    (card.rect.top + card.rect.bottom) / 2) != card.index;
            }
            check(safe, "retracting cards cannot receive navigation clicks");
            if (group == 2) check(!ui::HitHomeNetworkSettings(home, bounds, scale,
                partial.network_notice.left + scale, partial.network_notice.top + scale),
                "retracting network notice cannot open settings");
        }
    }

    auto& tab = *ActiveTab(s);
    tab.current_path = app::MakeHomePath();
    Render(s);
    auto vm = BuildVm(s, false);
    const auto bounds = vm.pane_slots.front().rect;
    const auto heading = ui::MakeHomeLayout(vm.pane, bounds, s.scale).headings[0];
    HandleLButtonDown(&s, s.hwnd, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(static_cast<int>(heading.left + 8 * s.scale),
        static_cast<int>(heading.top + 8 * s.scale)));
    check((tab.home_collapsed_mask & 1u) && tab.home_group_transitions[0].Pending(),
        "production home heading click starts collapse from its current layout");
    check(Render(s), "production frame scheduler keeps a home transition running");
    s.shot_high_contrast = true;
    check(!Render(s) && tab.home_group_transitions[0].value == 0,
        "high contrast settles home groups immediately and stops animation frames");
    s.shot_high_contrast = false;

    // Render synthetic data through the production renderer without storage or network workers.
    for (bool dark : {false, true}) for (float scale : {1.0f, 1.5f}) {
        s.scale = scale;
        s.renderer.SetScale(scale);
        s.compositor.RecreateTextFormats(scale);
        const float width = dark ? 760.0f : 1100.0f;
        s.compositor.Resize(static_cast<UINT>(width * scale), static_cast<UINT>(900 * scale));
        auto frame = BuildVm(s, false);
        frame.dark = dark;
        frame.background_image.clear();
        const auto rect = D2D1::RectF(0, 0, width * scale, 900 * scale);
        const auto theme = ui::MakeTheme(dark, ui::HexColor(0x0078D4));
        const auto prefix = std::wstring(dark ? L"dark-" : L"light-") + std::to_wstring(static_cast<int>(scale * 100));
        const auto draw = [&](const std::wstring& name, bool capture = true) {
            s.compositor.Dc()->BeginDraw();
            s.renderer.Render(frame, rect, theme);
            check(SUCCEEDED(s.compositor.Dc()->EndDraw()), "production animation frame draws without D2D errors");
            if (capture) check(WriteAnimationSnapshot(s.compositor, root / (prefix + L"-" + name + L".png")),
                "animation verification snapshot captured");
        };
        frame.settings_open = false;
        home.home_collapsed_mask = 0;
        home.home_expansion = {1, 1, 1};
        frame.pane = home;
        frame.pane_slots = {{home, s.renderer.ContentRect(rect.right, rect.bottom), true, false}};
        draw(L"home-open");
        frame.pane.home_expansion = {0.5f, 0.5f, 0.5f};
        frame.pane_slots.front().pane = frame.pane;
        draw(L"home-middle");
        frame.pane.home_expansion = {0, 0, 0};
        frame.pane_slots.front().pane = frame.pane;
        draw(L"home-closed");
        frame.settings_open = true;
        frame.settings_page = 0;
        frame.settings_scroll = 0;
        ui::fluent::Painter measuring(&s.compositor);
        measuring.SetScale(scale);
        measuring.BeginFrame(theme);
        const auto settings_layout = ui::MakeSettingsLayout(frame, rect, scale,
            s.renderer.TitleBarHeight(), 28 * scale, &measuring);
        frame.settings_scroll = std::max(0.0f, settings_layout.startup_row[1].top - settings_layout.content.top - 140 * scale);
        frame.settings_keep_running = false;
        draw(L"switch-off");
        check(!s.renderer.SettingsSwitchAnimating(), "opening settings snaps each switch to its actual saved value");
        frame.settings_keep_running = true;
        draw(L"switch-start", false);
        check(s.renderer.SettingsSwitchAnimating(), "settings changes request animation frames through the renderer");
        Sleep(45);
        draw(L"switch-middle");
        check(s.renderer.SettingsSwitchAnimating(), "switch transition remains active at an intermediate frame");
        Sleep(170);
        draw(L"switch-on");
        check(!s.renderer.SettingsSwitchAnimating(), "completed settings transition releases animation scheduling");
        frame.settings_keep_running = false;
        draw(L"switch-close-start", false);
        Sleep(45);
        draw(L"switch-close-middle");
        frame.settings_keep_running = true;
        draw(L"switch-reverse");
        check(s.renderer.SettingsSwitchAnimating(), "rapid switch reversal keeps requesting frames");
        frame.settings_open = false;
        draw(L"settings-leave");
        check(!s.renderer.SettingsSwitchAnimating(), "leaving settings clears switch animation work");
    }
    return failures == 0;
}
