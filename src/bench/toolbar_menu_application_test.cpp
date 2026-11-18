// SPDX-License-Identifier: Apache-2.0
#include "../app/app_internal.h"
#include "../app/toolbar_menus.h"
#include "../app/command_customization_ui.h"
#include "../common/user_storage.h"
#include "../ui/toolbar_layout.h"
#include "toolbar_customization_checks.h"
#include "shortcut_settings_checks.h"
#include <filesystem>
#include <cstdio>
#include <tuple>

namespace {
int passed = 0, failed = 0;
void Check(bool value, const char* label) {
    std::printf("[%s] %s\n", value ? "PASS" : "FAIL", label);
    value ? ++passed : ++failed;
}
bool Has(const std::vector<pulse::ui::FluentMenuItem>& items, int command) {
    return std::any_of(items.begin(), items.end(), [=](const auto& item) { return item.command == command; });
}
std::wstring customization_snapshot;
bool customization_captured = false;
void CALLBACK CaptureCustomization(HWND, UINT, UINT_PTR timer, DWORD) {
    HWND window = nullptr;
    EnumThreadWindows(GetCurrentThreadId(), [](HWND candidate, LPARAM reference) -> BOOL {
        wchar_t name[80]{}; GetClassNameW(candidate, name, ARRAYSIZE(name));
        if (std::wstring_view(name) == pulse::ui::kToolbarCustomizationWindowClass) {
            *reinterpret_cast<HWND*>(reference) = candidate; return FALSE;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&window));
    if (!window) return;
    KillTimer(nullptr, timer);
    customization_captured = SendMessageW(window, pulse::ui::kToolbarCustomizationSnapshotMessage, 0,
        reinterpret_cast<LPARAM>(customization_snapshot.c_str())) != 0;
    SendMessageW(window, WM_COMMAND, IDCANCEL, 0);
}
}
int main(int argc, char** argv) {
    using namespace pulse;
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    OleInitialize(nullptr);
    const auto root = std::filesystem::absolute(L"../bench_data/toolbar-menus-2026-11-17");
    std::filesystem::create_directories(root / L"profile");
    storage::OverrideDefaultRootForTesting((root / L"profile").wstring());
    l10n::Initialize(GetModuleHandleW(nullptr), L"zh-CN");
    app::CommandContext context;
    context.filesystem_folder = true;
    for (const auto language : {L"zh-CN", L"en-US"}) {
        l10n::SetLanguage(language);
        const bool english = std::wstring_view(language) == L"en-US";
        const auto wide = ui::MakeToolbarLayout(1500, 1, 44, 4, 80, 224);
        const auto compact = ui::MakeToolbarLayout(420, 1, 44, 4, 80, 64);
        const auto menu = app::BuildToolbarOverflowMenu(ui::kDefaultToolbarOrder, wide, context, 0);
        Check(!wide.has_overflow && menu.size() == 1 && menu.front().command == app::CmdCustomizeCommands,
            "wide toolbar offers customization without an all-command menu");
        Check(menu.front().text == (english ? L"Customize toolbar…" : L"自定义工具栏…"),
            "customization entry is localized and names the toolbar");
        Check(!Has(menu, app::CmdDetailsPanel) && !Has(menu, app::CmdAdvancedSearch) &&
            !Has(menu, app::CmdViewBase) && !Has(menu, app::CmdInstallFullIndex),
            "unpinned view and application settings stay out of toolbar overflow");
        const auto overflow = app::BuildToolbarOverflowMenu(ui::kDefaultToolbarOrder, compact, context, 2);
        Check(compact.has_overflow && overflow.size() > 1 &&
            overflow.size() == ui::kDefaultToolbarOrder.size() - compact.placed_commands.size() + 1,
            "compact toolbar exposes exactly folded buttons plus customization");
        std::vector<int> expected;
        for (const int id : ui::kDefaultToolbarOrder)
            if (std::none_of(compact.placed_commands.begin(), compact.placed_commands.end(),
                [=](const auto& placed) { return placed.command == id; })) expected.push_back(id);
        bool same_order = true;
        for (size_t i = 0; i < expected.size(); ++i) same_order &= overflow[i].command == expected[i];
        Check(same_order, "overflow preserves configured toolbar order");
        const auto split = std::find_if(overflow.begin(), overflow.end(),
            [](const auto& item) { return item.command == app::CmdSplitMenu; });
        Check(split != overflow.end() && split->children.size() == 5 &&
            std::any_of(split->children.begin(), split->children.end(), [](const auto& item) { return item.radio; }),
            "split layouts form one checked submenu rather than five root entries");
        const std::array<int, 5> synthetic{app::CmdCopy, app::CmdCopy, app::CmdNewMenu,
            app::CmdCustomizeCommands, 5999};
        const auto empty_layout = ui::ToolbarLayout{};
        const auto clean = app::BuildToolbarOverflowMenu(synthetic, empty_layout, context, 0);
        Check(clean.size() == 3 && clean[0].command == app::CmdCopy &&
            clean[1].command == app::CmdNewMenu && !clean[1].children.empty() && !clean[0].enabled,
            "overflow deduplicates commands, retains disabled state and groups New");
        Check(app::BuildToolbarContextMenu(false).size() == 1 &&
            app::BuildToolbarContextMenu(true).size() == 2,
            "toolbar background and button context menus contain one or two actions");
        const auto empty_toolbar = ui::MakeToolbarLayout(760, 1, 44, 4, 80, 64, 0, false, false, std::span<const int>{});
        Check(!empty_toolbar.has_overflow && empty_toolbar.placed_commands.empty() &&
            app::BuildToolbarOverflowMenu({}, empty_toolbar, context, 0).size() == 1,
            "an empty toolbar retains a customization entry so buttons can be restored");
        const auto view = app::BuildViewMenu(ui::ViewMode::Details, true);
        Check(view.size() == 9 && Has(view, app::CmdDetailsPanel) &&
            !Has(view, app::CmdCopy) && !Has(view, app::CmdSplitMenu),
            "view menu contains file display modes and details only");
        Check(app::FindCommandDescriptor(app::CmdAdvancedSearch) && app::FindCommandDescriptor(app::CmdApplications),
            "advanced and namespace actions remain available in the command catalog");
    }
    l10n::SetLanguage(L"zh-CN");
    if (argc == 2 && std::string_view(argv[1]) == "--menus-only") {
        OleUninitialize();
        std::printf("%d passed, %d failed\n", passed, failed);
        return failed ? 1 : 0;
    }
    {
        auto owned = std::make_unique<AppState>();
        auto& s = *owned;
        s.isolatedTest = true;
        s.accentColor = ui::HexColor(0x0078D4);
        s.appPrefs.persist = s.places.persist = s.ctxMenuPrefs.persist = false;
        s.appPrefs.auto_folder_size = false;
        s.window_tabs.EnsureDefault(); s.pane = s.window_tabs.Active()->FocusedPane();
        s.pane->header_animation.opacity = 1.0f;
        auto* tab = s.pane->ActiveTab();
        tab->current_path = root.wstring(); tab->loading = false;
        tab->SetSnapshot(std::make_shared<const std::vector<fs::DirEntry>>());
        s.hwnd = CreateWindowExW(WS_EX_NOACTIVATE, L"STATIC", L"Pulse toolbar menu fixture", WS_POPUP,
            -30000, -30000, 1500, 760, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        Check(s.hwnd && s.compositor.Init(s.hwnd), "initialize isolated production toolbar renderer");
        if (s.compositor.Dc()) {
            s.renderer.SetCompositor(&s.compositor);
            ui::FluentMenu menu;
            Check(menu.Create(s.hwnd, &s.compositor, 1), "initialize normal Fluent menu snapshots");
            for (const bool dark : {false, true}) {
                menu.SetTheme(dark, ui::HexColor(0x0078D4));
                const auto name = dark ? L"dark" : L"light";
                Check(menu.SaveDebugSnapshot((root / (std::wstring(L"toolbar-context-") + name + L".png")).c_str(),
                    app::BuildToolbarContextMenu(true)), "save two-action toolbar context menu");
                const auto layout = ui::MakeToolbarLayout(420, 1, 44, 4, 80, 64);
                Check(menu.SaveDebugSnapshot((root / (std::wstring(L"toolbar-overflow-") + name + L".png")).c_str(),
                    app::BuildToolbarOverflowMenu(ui::kDefaultToolbarOrder, layout, context, 0)),
                    "save compact toolbar overflow menu");
            }
            for (const auto& sample : {std::tuple{false, 1.0f, 1500}, std::tuple{true, 1.5f, 760},
                                      std::tuple{true, 2.0f, 540}, std::tuple{true, 1.0f, 1500}}) {
                const auto [dark, scale, dip_width] = sample;
                s.appPrefs.toolbar_order.assign(ui::kDefaultToolbarOrder.begin(), ui::kDefaultToolbarOrder.end());
                if (dark && dip_width == 1500) s.appPrefs.toolbar_order = {app::CmdSplitMenu,
                    app::CmdLayoutSingle, app::CmdLayoutTwoVertical, app::CmdLayoutTwoHorizontal,
                    app::CmdLayoutThree, app::CmdLayoutFourGrid, app::CmdCopy};
                s.scale = scale; s.darkMode = dark;
                const int width = static_cast<int>(dip_width * scale), height = static_cast<int>(660 * scale);
                SetWindowPos(s.hwnd, nullptr, -30000, -30000, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
                s.compositor.Resize(width, height); s.compositor.RecreateTextFormats(scale); s.renderer.SetScale(scale);
                auto vm = BuildVm(s, false); vm.safe_mode = true;
                const float left = s.renderer.EffectiveSidebarWidth(static_cast<float>(width));
                const auto layout = ui::MakeToolbarLayout(static_cast<float>(width), scale, s.renderer.TitleBarHeight(),
                    s.renderer.Margin(), s.renderer.NewButtonWidthPx(width-left < 600*scale), left,
                    vm.pane.filter_expand, false, false, vm.toolbar_order);
                if (dark && dip_width == 1500) {
                    const auto anchor = s.renderer.SplitCommandRect(static_cast<float>(width));
                    Check(anchor.left == layout.commands[6].left && anchor.right == layout.commands[6].right &&
                        layout.placed_commands.front().command == app::CmdSplitMenu,
                        "split dropdown anchor follows a reordered toolbar button");
                }
                const auto hit = s.renderer.HitTest(vm, D2D1::RectF(0, 0, static_cast<float>(width), static_cast<float>(height)),
                    (layout.overflow.left + layout.overflow.right) / 2, (layout.overflow.top + layout.overflow.bottom) / 2);
                Check(hit.region == ui::HitTestResult::ToolbarMore && hit.label == l10n::Get(layout.has_overflow
                    ? l10n::StringId::More : l10n::StringId::CustomizeCommands),
                    "toolbar action and tooltip follow the actual overflow layout");
                const auto pane_bounds = vm.pane_slots.empty() ? s.renderer.ContentRect(static_cast<float>(width),
                    static_cast<float>(height)) : vm.pane_slots.front().rect;
                const auto view_button = s.renderer.PaneViewButtonRect(pane_bounds, vm.pane.filter_expand);
                const auto view_hit = s.renderer.HitTest(vm, D2D1::RectF(0, 0, static_cast<float>(width), static_cast<float>(height)),
                    (view_button.left + view_button.right) / 2, (view_button.top + view_button.bottom) / 2);
                Check(view_hit.region == ui::HitTestResult::PaneViewButton,
                    "settled pane view picker remains visible and clickable beside direct view buttons");
                auto* dc = s.compositor.Dc(); dc->BeginDraw(); dc->Clear(D2D1::ColorF(0, 0));
                s.renderer.Render(vm, D2D1::RectF(0, 0, static_cast<float>(width), static_cast<float>(height)),
                    ui::MakeTheme(dark, ui::HexColor(0x0078D4)));
                Check(SUCCEEDED(dc->EndDraw()), "render separate toolbar customize and pane view icons");
                const auto name = std::wstring(dark ? L"toolbar-dark-" : L"toolbar-light-") +
                    std::to_wstring(static_cast<int>(scale * 100)) + L"-" + std::to_wstring(dip_width) + L".png";
                Check(s.compositor.SaveSnapshot((root / name).c_str()), "save production toolbar and view header frame");
            }
            s.appPrefs.toolbar_order.assign(ui::kDefaultToolbarOrder.begin(), ui::kDefaultToolbarOrder.end());
            const auto original_order = s.appPrefs.toolbar_order;
            const auto original_shortcuts = s.appPrefs.command_shortcuts;
            customization_snapshot = (root / L"toolbar-customize-production.png").wstring();
            const auto timer = SetTimer(nullptr, 0, 80, CaptureCustomization);
            if (timer) ShowCommandCustomization(s);
            if (timer) KillTimer(nullptr, timer);
            Check(customization_captured, "capture the real application customization entry with production command icons");
            Check(s.appPrefs.toolbar_order == original_order && std::equal(s.appPrefs.command_shortcuts.begin(),
                s.appPrefs.command_shortcuts.end(), original_shortcuts.begin(), original_shortcuts.end(),
                [](const auto& left, const auto& right) { return left.command == right.command && left.shortcut == right.shortcut; }),
                "cancelling the application customization entry preserves toolbar and shortcut preferences");
            s.renderer.SetCompositor(nullptr); s.compositor.Shutdown();
        }
        if (s.hwnd) DestroyWindow(s.hwnd);
        s.hwnd = nullptr;
    }
    if (!(argc == 2 && std::string_view(argv[1]) == "--toolbar-only")) {
        bench::RunToolbarCustomizationChecks(root, Check);
        bench::RunShortcutSettingsChecks(root, Check);
    }
    OleUninitialize();
    std::printf("%d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
