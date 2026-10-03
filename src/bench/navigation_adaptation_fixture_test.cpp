#include "../app/app_internal.h"
#include "../ui/ui_renderer_internal.h"
#include "../ui/toolbar_layout.h"
#include "../common/localization.h"
#include <cstdio>
#include <filesystem>

int main() {
    using namespace pulse;
    OleInitialize(nullptr);
    l10n::Initialize(GetModuleHandleW(nullptr), L"zh-CN");
    int failures = 0;
    const auto check = [&](bool ok, const char* name) {
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
        if (!ok) ++failures;
    };
    auto owned = std::make_unique<AppState>();
    auto& s = *owned;
    s.isolatedTest = true;
    s.appPrefs.persist = s.places.persist = s.ctxMenuPrefs.persist = s.searchHistory.persist = false;
    const HWND foreground = GetForegroundWindow();
    s.hwnd = CreateWindowExW(WS_EX_NOACTIVATE, L"STATIC", L"navigation fixture", WS_OVERLAPPEDWINDOW,
        -30000, -30000, 1100, 720, nullptr, nullptr, nullptr, nullptr);
    if (!s.hwnd || !s.compositor.Init(s.hwnd)) return 2;
    s.renderer.SetCompositor(&s.compositor);
    s.window_tabs.EnsureDefault(); s.pane = s.window_tabs.Active()->FocusedPane();
    auto& tab = *ActiveTab(s);
    const auto output = std::filesystem::absolute(L"../bench_data/navigation-adaptation");
    std::filesystem::create_directories(output);
    for (const auto scale : {1.0f, 1.25f, 1.5f}) {
        s.scale = scale;
        s.renderer.SetScale(scale);
        s.compositor.RecreateTextFormats(scale);
        s.compositor.Resize(static_cast<UINT>(1100 * scale), static_cast<UINT>(720 * scale));
        const auto measure = [&](const std::wstring& value) {
            return ui::typography::MeasureAdvance(s.compositor.DwriteFactory(), s.compositor.AddressFormat(), value);
        };
        for (const auto letter : {L'C', L'E', L'F'}) {
            const std::wstring drive = std::wstring(1, letter) + L":";
            check(ui::FitEndEllipsis(drive, measure(drive), measure) == drive,
                "short drive label fits even when an ellipsis is wider than the label");
            tab.current_path = L"\\\\?\\" + drive + L"\\Applications";
            tab.loading = false;
            auto rows = std::make_shared<std::vector<fs::DirEntry>>();
            tab.SetSnapshot(rows);
            const auto vm = BuildVm(s, false);
            std::vector<ui::MainRenderer::BreadcrumbPlaced> placed;
            s.renderer.BreadcrumbLayout(vm.pane, 1100 * scale, placed);
            check(placed.size() == 3 && placed[1].text == drive && placed[1].path == drive + L"\\",
                "drive breadcrumb is visible and remains a clickable drive-root target");
            for (const bool dark : {false, true}) {
                s.darkMode = dark;
                s.compositor.Dc()->BeginDraw();
                s.renderer.Render(BuildVm(s, false), D2D1::RectF(0, 0, 1100 * scale, 720 * scale),
                    ui::MakeTheme(dark, ui::HexColor(0x0078D4)));
                check(SUCCEEDED(s.compositor.Dc()->EndDraw()), "drive breadcrumb frame renders successfully");
                const auto image = output / (std::wstring(1, letter) + L"-" + std::to_wstring(static_cast<int>(scale * 100)) +
                    (dark ? L"-dark.png" : L"-light.png"));
                check(s.compositor.SaveSnapshot(image.c_str()), "drive breadcrumb visual evidence saved");
            }
        }
    }
    s.scale = 1; s.renderer.SetScale(1); s.compositor.RecreateTextFormats(1); s.compositor.Resize(1100, 720);
    tab.current_path = fs::NormalizePath(L"E:\\pulse-navigation-fixture\\one\\two\\three");
    const auto geometry = ui::MakeToolbarLayout(1100, 1, s.renderer.TitleBarHeight(), s.renderer.Margin(),
        84, s.renderer.EffectiveSidebarWidth(1100));
    const auto& button = geometry.navigation[2];
    const LPARAM point = MAKELPARAM(static_cast<int>((button.left + button.right) / 2),
        static_cast<int>((button.top + button.bottom) / 2));
    auto press = [&](bool second) {
        if (second) HandleLButtonDblClk(&s, s.hwnd, WM_LBUTTONDBLCLK, MK_LBUTTON, point);
        else HandleLButtonDown(&s, s.hwnd, WM_LBUTTONDOWN, MK_LBUTTON, point);
        HandleLButtonUp(&s, s.hwnd, WM_LBUTTONUP, 0, point);
    };
    const auto started = GetTickCount64();
    press(false);
    check(tab.current_path == fs::NormalizePath(L"E:\\pulse-navigation-fixture\\one\\two"),
        "first Up press navigates immediately");
    press(true);
    check(tab.current_path == fs::NormalizePath(L"E:\\pulse-navigation-fixture\\one"),
        "rapid second Up press is not swallowed as a double click");
    press(false); press(true);
    check(tab.current_path == fs::NormalizePath(L"E:\\"),
        "four rapid Up presses each count as one ancestor step without a timer wait");
    check(GetTickCount64() - started < GetDoubleClickTime(),
        "navigation handles rapid presses without inserting the system double-click interval");
    s.watches.Stop();
    s.renderer.SetCompositor(nullptr); s.compositor.Shutdown();
    DestroyWindow(s.hwnd); s.hwnd = nullptr;
    owned.reset();
    check(GetForegroundWindow() == foreground, "navigation checks preserve user foreground activation");
    OleUninitialize();
    return failures ? 1 : 0;
}
