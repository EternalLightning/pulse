#include "../app/app_internal.h"
#include "../ui/ui_renderer_internal.h"
#include <filesystem>
#include <iostream>

int main() {
    using namespace pulse;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    l10n::Initialize(GetModuleHandleW(nullptr), L"zh-CN");
    int failures = 0;
    auto check = [&](bool ok, const char* label) {
        std::cout << (ok ? "[PASS] " : "[FAIL] ") << label << std::endl;
        if (!ok) ++failures;
    };
    auto owned = std::make_unique<AppState>();
    auto& s = *owned;
    s.isolatedTest = true;
    s.appPrefs.persist = false;
    s.ctxMenuPrefs.persist = false;
    WNDCLASSW wc{};
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpfnWndProc = DefWindowProcW;
    wc.lpszClassName = L"PulseGlobalSearchSettingsFixture";
    RegisterClassW(&wc);
    s.hwnd = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW,
        0, 0, 1100, 900, nullptr, nullptr, wc.hInstance, nullptr);
    if (!s.hwnd || !s.compositor.Init(s.hwnd)) return 2;
    s.renderer.SetCompositor(&s.compositor);
    s.window_tabs.EnsureDefault();
    s.pane = s.window_tabs.Active()->FocusedPane();
    ActiveTab(s)->current_path = app::MakeSettingsPath(app::SettingsController::PageName(1));
    s.settings.SelectPage(1);
    s.settings.BindUi(s.appPrefs, s.ctxMenuPrefs, s.index, s.networkIndex, {});
    const auto output = std::filesystem::absolute(L"../bench_data/global-search-settings-ui");
    std::filesystem::create_directories(output);
    ui::fluent::Painter painter(&s.compositor);
    for (const auto* language : {L"zh-CN", L"en-US"}) {
        l10n::SetLanguage(language);
        for (float scale : {1.0f, 1.5f}) for (int width : {720, 1100}) for (bool dark : {false, true}) {
            s.scale = scale;
            s.darkMode = dark;
            s.compositor.RecreateTextFormats(scale);
            s.renderer.SetScale(scale);
            painter.SetScale(scale);
            const auto rect = D2D1::RectF(0, 0, width*scale, 900*scale);
            s.compositor.Resize(static_cast<UINT>(rect.right), static_cast<UINT>(rect.bottom));
            for (int state = 0; state < 3; ++state) {
                s.appPrefs.global_search_enabled = state != 0;
                s.settings.CancelGlobalSearchHotkeyCapture();
                s.settings.SetGlobalSearchError(L"");
                if (state == 1) s.settings.BeginGlobalSearchHotkeyCapture();
                if (state == 2) s.settings.SetGlobalSearchError(l10n::Get(l10n::StringId::GlobalSearchConflict));
                auto vm = BuildVm(s, false);
                check(vm.settings_open && vm.settings_page == 1 &&
                    vm.settings_global_search_enabled == (state != 0) &&
                    vm.settings_global_search_capturing == (state == 1), "settings view model exposes global search state");
                const auto layout = ui::MakeSettingsLayout(vm, rect, scale,
                    s.renderer.TitleBarHeight(), 28*scale, &painter);
                const auto& toggle = layout.global_search_row;
                const auto& row = layout.global_search_hotkey_row;
                const auto& button = layout.global_search_hotkey_button;
                auto hit = [&](const auto& r) {
                    return s.renderer.HitTest(vm, rect, (r.left+r.right)/2, (r.top+r.bottom)/2);
                };
                const auto toggle_hit = hit(toggle);
                check(toggle_hit.region == ui::HitTestResult::SettingsToggle && toggle_hit.index == 15,
                    "global search toggle has dedicated hit target");
                check(hit(button).region == ui::HitTestResult::SettingsGlobalSearchHotkey,
                    "shortcut recorder has dedicated hit target");
                const bool stacked = row.right-row.left < 560*scale;
                check(toggle.bottom <= row.top && button.top >= row.top+(stacked ? 56 : 12)*scale && button.bottom <= row.bottom &&
                    button.left >= row.left && button.right <= row.right && row.bottom <= layout.search_pinyin_row.top,
                    "global search settings controls fit without overlap");
                auto fits = [&](const std::wstring& text, float text_width, float text_height) {
                    ui::ComPtr<IDWriteTextLayout> text_layout;
                    if (FAILED(s.compositor.DwriteFactory()->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()),
                        s.compositor.SmallFormat(), text_width, text_height, &text_layout))) return false;
                    if (text_height > 22*scale) text_layout->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
                    DWRITE_TEXT_METRICS metrics{};
                    return SUCCEEDED(text_layout->GetMetrics(&metrics)) && metrics.width <= text_width+1 && metrics.height <= text_height+1;
                };
                check(fits(l10n::Get(l10n::StringId::GlobalSearchDesc), toggle.right-toggle.left-126*scale, 43*scale),
                    "global search lifecycle description fits at width and DPI");
                const auto& types = layout.content_types;
                check(!l10n::Get(l10n::StringId::SettingsContentTypes).empty() &&
                    !l10n::Get(l10n::StringId::SettingsContentTypesDesc).empty(), "supported types localized strings load");
                check(fits(l10n::Get(l10n::StringId::SettingsContentTypesDesc), types.right-types.left-70*scale,
                    types.bottom-types.top-45*scale), "supported document types fit at width and DPI");
                check(layout.content_header.bottom <= types.top && types.bottom <= layout.content_options.top,
                    "document types do not overlap surrounding rows");
                check(fits(state == 2 ? vm.settings_global_search_error : l10n::Get(l10n::StringId::GlobalSearchHotkeyDesc),
                    row.right-row.left-70*scale, 21*scale), "shortcut hint or conflict text fits at width and DPI");
                const auto theme = ui::MakeTheme(dark, ui::HexColor(0x0078D4));
                s.compositor.Dc()->BeginDraw();
                s.renderer.Render(vm, rect, theme);
                check(SUCCEEDED(s.compositor.Dc()->EndDraw()), "global search settings render completes");
                const auto file = output/(std::wstring(language)+L"-"+std::to_wstring(width)+L"-"+
                    std::to_wstring(static_cast<int>(scale*100))+(dark ? L"-dark-" : L"-light-")+std::to_wstring(state)+L".png");
                check(s.compositor.SaveSnapshot(file.c_str()), "global search settings screenshot saved");
                if (state == 0) {
                    vm.settings_scroll += layout.content_header.top-layout.content.top;
                    s.compositor.Dc()->BeginDraw();
                    s.renderer.Render(vm, rect, theme);
                    check(SUCCEEDED(s.compositor.Dc()->EndDraw()), "content types settings render completes");
                    const auto content_file = output/(L"content-"+file.filename().wstring());
                    check(s.compositor.SaveSnapshot(content_file.c_str()), "content types screenshot saved");
                }
            }
        }
    }
    s.settings.ResetUi();
    s.renderer.SetCompositor(nullptr);
    s.compositor.Shutdown();
    DestroyWindow(s.hwnd);
    s.hwnd = nullptr;
    CoUninitialize();
    return failures ? 1 : 0;
}
