// SPDX-License-Identifier: Apache-2.0
#include "../ui/ui_renderer_internal.h"
#include <filesystem>
#include <cstdio>

int main() {
    using namespace pulse;
    using I = l10n::StringId;
    int passed = 0, failed = 0;
    const auto check = [&](bool ok, const char* label) {
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
        ok ? ++passed : ++failed;
    };
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    OleInitialize(nullptr);
    l10n::Initialize(GetModuleHandleW(nullptr), L"zh-CN");
    const auto root = std::filesystem::absolute(L"../bench_data/action-ellipsis-2026-11-17");
    std::filesystem::create_directories(root);
    HWND window = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"STATIC", L"Action label fixture",
        WS_POPUP, -30000, -30000, 1000, 900, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    ui::Compositor compositor;
    ui::MainRenderer renderer;
    check(window && compositor.Init(window), "initialize production action label renderer");
    if (!compositor.Dc()) return 2;
    renderer.SetCompositor(&compositor); renderer.SetDetailsPanelVisible(false);
    ui::fluent::Painter painter(&compositor);
    for (const bool english : {false, true}) {
        l10n::SetLanguage(english ? L"en-US" : L"zh-CN");
        const float scale = english ? 1.75f : 1.0f;
        const float width = (english ? 620.0f : 1000.0f)*scale, height = 900*scale;
        compositor.Resize(static_cast<UINT>(width), static_cast<UINT>(height));
        compositor.RecreateTextFormats(scale); renderer.SetScale(scale); renderer.InvalidateTypography();
        painter.SetScale(scale);
        const auto theme = ui::MakeTheme(!english, ui::HexColor(0x0078D4)); painter.BeginFrame(theme);
        for (const auto id : {I::ChooseImage, I::ChangeLocation, I::AddFolder, I::DupBrowse,
                I::ClearDiagnostics, I::ExportDiagnostics}) {
            const auto& text = l10n::Get(id);
            check(text.ends_with(L"…") && !text.ends_with(L"……"), "picker and confirmation labels have exactly one intentional ellipsis");
        }
        check(!l10n::Get(I::ExportSettingsTitle).ends_with(L"…") &&
            !l10n::Get(I::ImportSettingsTitle).ends_with(L"…") && !l10n::Get(I::OpenLocation).ends_with(L"…"),
            "dialog titles and immediate navigation keep their original captions");
        for (const int page : {0, 1, 3, 4, 8, 10}) {
            ui::WindowViewModel vm;
            vm.settings_open = true; vm.settings_page = page; vm.dark = !english; vm.safe_mode = true;
            vm.settings_expanded = 3; vm.settings_index_service = true;
            const auto rect = D2D1::RectF(0, 0, width, height);
            auto layout = ui::MakeSettingsLayout(vm, rect, scale, renderer.TitleBarHeight(), 28*scale,
                &painter, renderer.EffectiveSidebarWidth(width));
            std::vector<std::pair<D2D1_RECT_F, std::wstring>> buttons;
            if (page == 0) buttons = {{layout.wallpaper_choose, l10n::Get(I::ChooseImage)}};
            if (page == 1) buttons = {{layout.index_action[2], l10n::Get(I::ChangeLocation)},
                {layout.index_exclude_action, l10n::Get(I::AddFolder)}, {layout.network_action[0], l10n::Get(I::AddFolder)}};
            if (page == 3) buttons = {{layout.diagnostics_action[1], l10n::Get(I::ClearDiagnostics)},
                {layout.diagnostics_action[2], l10n::Get(I::ExportDiagnostics)}};
            if (page == 4) buttons = {{layout.dup_browse, l10n::Get(I::DupBrowse)}};
            if (page == 8) buttons = {{layout.configuration_action[1], l10n::Get(I::ChangeLocation)},
                {layout.transfer_action[0], l10n::Get(I::ExportSettingsTitle)+L"…"},
                {layout.transfer_action[1], l10n::Get(I::ImportSettingsTitle)+L"…"}};
            if (page == 10) buttons = {{layout.shortcut_search, l10n::Pick(L"搜索快捷键…", L"Find shortcuts…")}};
            for (const auto& [bounds, text] : buttons)
                check(bounds.right > bounds.left && painter.MeasureWrappedButtonHeight(text, bounds.right-bounds.left) <=
                    bounds.bottom-bounds.top+0.1f, "updated command label fits its measured button including the suffix");
            const auto target = page == 8 ? buttons[1].first : buttons.front().first;
            vm.settings_scroll = std::max(0.0f, target.top-layout.content.top-48*scale);
            auto* dc = compositor.Dc();
            bool rendered = true;
            for (int frame = 0; frame < 2; ++frame) {
                dc->BeginDraw(); dc->Clear(D2D1::ColorF(0,0));
                renderer.Render(vm, rect, theme);
                rendered &= SUCCEEDED(dc->EndDraw());
                if (frame == 0) Sleep(260);
            }
            check(rendered, "render affected settings page through its transition and final state");
            const auto file = root / ((english ? L"en-narrow-175-page-" : L"zh-dark-page-")+std::to_wstring(page)+L".png");
            check(compositor.SaveSnapshot(file.c_str()), "save targeted settings command label screenshot");
        }
    }
    renderer.SetCompositor(nullptr); compositor.Shutdown(); DestroyWindow(window);
    OleUninitialize();
    std::printf("%d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
