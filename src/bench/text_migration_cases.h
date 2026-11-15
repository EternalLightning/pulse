// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <chrono>

namespace pulse::ui {
struct FilenameRenderTest {
    static int RunMigration() {
        const auto output = std::filesystem::absolute(L"bench_data/text-migration-20261115/pane-settings");
        if (!bench::StreamSnapshots()) std::filesystem::create_directories(output);
        HWND window = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"STATIC", L"Text migration",
            WS_POPUP, -30000, -30000, 900, 620, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        Compositor compositor;
        Check(window && compositor.Init(window), "migration fixture initializes the production compositor");
        if (!compositor.Dc()) {
            if (window) DestroyWindow(window);
            return 1;
        }
        Check(compositor.LumaTextAvailable(), "migration fixture loads the real LumaText backend");
        MainRenderer renderer;
        renderer.SetCompositor(&compositor);
        renderer.motion_enabled_ = false;
        const std::wstring names[] = {L"照明.txt", L"中文 report 警醒 龘靐 资料.txt",
            L"very-long-report-复杂中文-照明-资料-0123456789-abcdefghijklmnopqrstuvwxyz-final.txt"};
        for (float scale : {1.0f, 1.25f, 1.5f, 1.75f}) for (bool dark : {false, true})
        for (int mode : {0, 1, 2}) {
            typography::SetTextRenderMode(static_cast<typography::TextRenderMode>(mode));
            compositor.Resize(static_cast<UINT>(900 * scale), static_cast<UINT>(620 * scale));
            compositor.RecreateTextFormats(scale);
            renderer.SetScale(scale);
            const auto theme = MakeTheme(dark, HexColor(0x0078D4));
            auto* dc = compositor.Dc();
            auto rect = [scale](float x, float y, float w, float h) {
                return D2D1::RectF(x * scale, y * scale, (x + w) * scale, (y + h) * scale);
            };
            auto draws = [&] {
                const auto* stats = compositor.GetLumaTextStats();
                return stats ? stats->draw_calls : 0;
            };
            auto check_backend = [&](auto draw, const char* label) {
                const auto before = draws();
                draw();
                Check(mode == 0 ? draws() > before : draws() == before, label);
            };
            dc->BeginDraw();
            dc->Clear(theme.bg);
            renderer.painter_.BeginFrame(theme);
            ComPtr<IDWriteTextFormat> display;
            Check(SUCCEEDED(typography::CreateTextFormat(compositor.DwriteFactory(),
                {typography::FontRole::Display, 22 * scale, DWRITE_FONT_WEIGHT_NORMAL}, &display)),
                "settings sample retains the 22 DIP normal-weight Display role");
            if (display.get()) {
                display->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
                display->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                check_backend([&] {
                    renderer.painter_.DrawText(L"Settings 设置 照明 警醒", rect(20, 20, 850, 40), display.get(), theme.text);
                }, "22 DIP Display sample uses LumaText only in Auto");
            }
            for (int i = 0; i < 3; ++i) {
                bool truncated = false;
                const auto matches = NameMatchRanges(names[i], NameHighlightTerms(L"", L"report 照明"));
                Check(!matches.empty(), "filename sample contains actual English or Chinese highlight spans");
                check_backend([&] {
                    truncated = renderer.DrawCenteredIconName(names[i], rect(20 + i * 280.0f, 100, 180, 64),
                        theme.text, theme, matches);
                }, "centered wrapped filenames and highlighted ranges follow the selected backend");
                if (i != 1) Check(truncated == (i == 2), "filename truncation retains its original width and height budget");
            }
            PaneViewModel changes;
            changes.is_changes = true;
            changes.change_empty_text = L"No matching changes 当前没有匹配的变更。\n复杂中文：照明、警醒、龘靐。";
            check_backend([&] {
                renderer.DrawPaneEmptyState({}, changes, rect(20, 210, 340, 180), 0, theme);
            }, "centered multiline changes empty state follows the selected backend");
            PaneViewModel pane;
            pane.view_mode = ViewMode::LargeIcons;
            pane.group_mode = GroupMode::None;
            for (const auto& name : names) {
                ListEntryView entry;
                entry.name = name;
                pane.entries.push_back(std::move(entry));
            }
            const auto viewport = rect(410, 210, 450, 280);
            ViewLayout layout(pane.view_mode, viewport, pane.EntryCount(), 0, 0, scale);
            for (int i = 0; i < 3; ++i) {
                const auto name = layout.NameRect(i);
                const auto rename = renderer.RenameFieldRect(pane, viewport, i);
                Check(layout.HitTest((name.left + name.right) * 0.5f, (name.top + name.bottom) * 0.5f) == i &&
                    rename.top == name.top && rename.left >= name.left && rename.right <= name.right,
                    "icon filename hit testing and rename field retain the shared layout geometry");
            }
            Check(SUCCEEDED(dc->EndDraw()), "migrated pane and 22 DIP sample frame completes without D2D error");
            const auto suffix = std::to_wstring(static_cast<int>(scale * 100)) +
                (dark ? L"-dark-" : L"-light-") + std::to_wstring(mode) + L".png";
            Check(bench::SaveSnapshot(compositor, output / (L"samples-" + suffix)), "migration samples save at native pixel size");

            WindowViewModel settings;
            settings.settings_open = true;
            settings.dark = dark;
            settings.safe_mode = true;
            settings.window_effect = WindowEffect::None;
            settings.settings_page = 0;
            const auto aa = dc->GetTextAntialiasMode();
            ComPtr<IDWriteRenderingParams> before, after;
            dc->GetTextRenderingParams(&before);
            dc->BeginDraw();
            dc->Clear(theme.bg);
            renderer.painter_.BeginFrame(theme);
            renderer.DrawSettings(settings, rect(0, 0, 900, 620), theme);
            Check(SUCCEEDED(dc->EndDraw()), "production settings page renders with its migrated 22 DIP title");
            dc->GetTextRenderingParams(&after);
            Check(before.get() == after.get() && aa == dc->GetTextAntialiasMode(),
                "settings title restores the explicit-mode OUTLINE rendering state");
            Check(bench::SaveSnapshot(compositor, output / (L"settings-" + suffix)), "production settings title snapshot saves at native pixel size");

            if (scale == 1.0f && !dark && mode != 1) {
                FILETIME created{}, exited{}, kernel_before{}, user_before{}, kernel_after{}, user_after{};
                GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel_before, &user_before);
                const auto started = std::chrono::steady_clock::now();
                const auto before_draws = draws();
                const auto* stats = compositor.GetLumaTextStats();
                const auto before_hits = stats ? stats->surface_cache_hits : 0;
                bool frames_ok = true;
                for (int frame = 0; frame < 60; ++frame) {
                    dc->BeginDraw();
                    dc->Clear(theme.bg);
                    for (int i = 0; i < 24; ++i) {
                        const float top = 20.0f + (i / 4) * 85.0f - (frame % 5);
                        renderer.DrawCenteredIconName(names[i % 3], rect(20 + (i % 4) * 210.0f, top, 180, 64),
                            theme.text, theme, {});
                    }
                    frames_ok &= SUCCEEDED(dc->EndDraw());
                }
                const double wall_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
                GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel_after, &user_after);
                auto ticks = [](FILETIME time) { return (static_cast<unsigned long long>(time.dwHighDateTime) << 32) | time.dwLowDateTime; };
                const double cpu_ms = static_cast<double>(ticks(kernel_after) + ticks(user_after) - ticks(kernel_before) - ticks(user_before)) / 10000.0;
                stats = compositor.GetLumaTextStats();
                std::cout << "[PERF] multiline filenames mode=" << mode << " frames=60 names=1440 wall_ms=" << wall_ms
                    << " cpu_ms=" << cpu_ms << " luma_draws=" << draws() - before_draws
                    << " surface_hits=" << (stats ? stats->surface_cache_hits : 0) - before_hits << '\n';
                Check(frames_ok, "repeated scrolling filename frames complete without D2D error");
            }
        }
        renderer.SetCompositor(nullptr);
        compositor.Shutdown();
        DestroyWindow(window);
        std::cout << "[SUMMARY] " << checks - failures << " PASS / " << failures << " FAIL\n";
        return failures ? 1 : 0;
    }
};
} // namespace pulse::ui
