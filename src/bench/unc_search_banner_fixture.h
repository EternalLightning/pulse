// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once
#include "../app/app_internal.h"
#include "../common/localization.h"
#include "../ui/search_banner_layout.h"
#include "../ui/typography.h"
#include <cstdio>
#include <filesystem>

namespace pulse::bench {
inline int RunUncSearchBannerCases(AppState& s, app::Tab& tab) {
    int failures = 0;
    const auto check = [&](bool ok, const char* label) {
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
        if (!ok) ++failures;
    };
    const auto output = std::filesystem::absolute(L"bench_data/unc-search-banner");
    std::filesystem::create_directories(output);
    const auto previous_language = l10n::preference();
    const auto previous_scale = s.scale;
    const auto previous_dark = s.darkMode;
    tab = app::Tab{};
    tab.current_path = app::MakeSearchPath(L"match path:\"\\\\server\\share\\folder\"");
    tab.network_search_root = L"\\\\server\\share\\folder";
    tab.search_entries = std::make_shared<std::vector<fs::DirEntry>>();
    tab.SetSnapshot(tab.search_entries);
    const auto root = tab.network_search_root;
    for (const wchar_t* language : {L"zh-CN", L"en-US"}) {
        l10n::SetLanguage(language);
        for (const float scale : {1.0f, 1.5f}) {
            s.scale = scale;
            s.renderer.SetScale(scale);
            s.compositor.RecreateTextFormats(scale);
            const float width = 720 * scale, height = 520 * scale;
            s.compositor.Resize(static_cast<int>(width), static_cast<int>(height));
            for (const bool dark : {false, true}) {
                s.darkMode = dark;
                for (const bool scanning : {true, false}) {
                    index::SearchResult result;
                    result.scanning = scanning;
                    tab.pending_generation = scanning ? 70001 : 0;
                    ApplySearchHits(tab, L"match", std::move(result));
                    const auto vm = BuildVm(s, false);
                    const auto pane = FocusedPaneRect(s);
                    const auto body = s.renderer.PaneBodyBounds(vm.pane, pane);
                    const float action_width = ui::SearchBannerActionWidth(body.right - body.left - 16 * scale, scale, true);
                    const D2D1_RECT_F action = D2D1::RectF(body.right - 8 * scale - action_width,
                        body.top + s.renderer.PaneHeaderHeight(), body.right - 8 * scale,
                        body.top + s.renderer.PaneHeaderHeight() + 32 * scale);
                    const auto hit = s.renderer.HitTest(vm, D2D1::RectF(0, 0, width, height),
                        (action.left + action.right) * 0.5f, (action.top + action.bottom) * 0.5f);
                    check(hit.region == ui::HitTestResult::NetworkSearchAdd && hit.path == root,
                        "UNC banner hit uses the visible pane action and carries its exact root");
                    check(vm.pane.banner_title == l10n::Get(scanning ? l10n::StringId::NetworkSearchRunning :
                        l10n::StringId::NetworkSearchComplete) && vm.pane.network_search_root == root,
                        "running and empty-completed UNC search retain the indexing action");
                    ui::ComPtr<IDWriteTextFormat> format;
                    ui::ComPtr<IDWriteTextLayout> layout;
                    const auto& label = l10n::Get(l10n::StringId::NetworkSearchAdd);
                    const bool measured = SUCCEEDED(ui::typography::CreateTextFormat(s.compositor.DwriteFactory(),
                        {ui::typography::FontRole::Text, 14 * scale, DWRITE_FONT_WEIGHT_NORMAL}, &format)) &&
                        SUCCEEDED(s.compositor.DwriteFactory()->CreateTextLayout(label.c_str(),
                            static_cast<UINT32>(label.size()), format.get(), 4096 * scale, 100 * scale, &layout));
                    DWRITE_TEXT_METRICS metrics{};
                    if (measured) layout->GetMetrics(&metrics);
                    check(measured && metrics.widthIncludingTrailingWhitespace <= action_width - 16 * scale + 0.1f &&
                        metrics.height <= 24 * scale + 0.1f && action.left >= body.left &&
                        s.renderer.PaneListRect(vm.pane, pane).top >= action.bottom,
                        "localized index button fits a narrow pane at 100 and 150 percent without clipping or list overlap");
                    ui::ComPtr<IDWriteTextLayout> title_layout;
                    DWRITE_TEXT_METRICS title_metrics{};
                    const bool title_measured = measured && SUCCEEDED(s.compositor.DwriteFactory()->CreateTextLayout(
                        vm.pane.banner_title.c_str(), static_cast<UINT32>(vm.pane.banner_title.size()),
                        format.get(), 4096 * scale, 100 * scale, &title_layout));
                    if (title_measured) title_layout->GetMetrics(&title_metrics);
                    const float text_width = body.right - body.left - 78 * scale - action_width;
                    const bool shows_message = title_measured && title_metrics.width + 132 * scale <= text_width;
                    ui::ComPtr<IDWriteTextFormat> caption_format;
                    ui::ComPtr<IDWriteTextLayout> message_layout;
                    DWRITE_TEXT_METRICS message_metrics{};
                    const float message_width = text_width - title_metrics.width - 12 * scale;
                    const bool message_measured = shows_message &&
                        SUCCEEDED(ui::typography::CreateTextFormat(s.compositor.DwriteFactory(),
                            {ui::typography::FontRole::Text, 12 * scale, DWRITE_FONT_WEIGHT_NORMAL}, &caption_format)) &&
                        SUCCEEDED(s.compositor.DwriteFactory()->CreateTextLayout(vm.pane.banner_message.c_str(),
                            static_cast<UINT32>(vm.pane.banner_message.size()), caption_format.get(),
                            message_width, 32 * scale, &message_layout));
                    if (message_measured) message_layout->GetMetrics(&message_metrics);
                    check(message_measured && message_metrics.height <= 32 * scale + 0.1f &&
                        message_metrics.widthIncludingTrailingWhitespace <= message_width + 0.1f,
                        "network indexing explanation fits without clipping in scanning and completed states");
                    s.compositor.Dc()->BeginDraw();
                    s.renderer.Render(vm, D2D1::RectF(0, 0, width, height), ui::MakeTheme(dark, ui::HexColor(0x0078D4)));
                    check(SUCCEEDED(s.compositor.Dc()->EndDraw()), "UNC banner renders in light and dark themes");
                    const auto image = output / (std::wstring(language) + L"-" +
                        std::to_wstring(static_cast<int>(scale * 100)) + (dark ? L"-dark" : L"-light") +
                        (scanning ? L"-scanning.png" : L"-complete.png"));
                    check(s.compositor.SaveSnapshot(image.c_str()), "UNC banner visual evidence saved");
                }
            }
        }
    }
    l10n::SetLanguage(l10n::LanguageId(previous_language));
    s.scale = previous_scale; s.darkMode = previous_dark;
    s.renderer.SetScale(previous_scale);
    s.compositor.RecreateTextFormats(previous_scale);
    s.compositor.Resize(1000, 700);
    return failures;
}
}
