// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "../app/app_internal.h"
#include "../app/settings_input.h"
#include "../app/shortcut_settings_ui.h"
#include "../app/command_catalog.h"
#include "../ui/ui_renderer_internal.h"
#include <filesystem>
#include <functional>

namespace pulse::bench::shortcut_settings_detail {
inline HWND dialog_owner = nullptr;
inline std::wstring dialog_text;
inline bool dialog_driven = false, rejection_observed = false;
inline unsigned timer_ticks = 0;

inline BOOL CALLBACK FindInput(HWND window, LPARAM parameter) {
    wchar_t name[80]{};
    GetClassNameW(window, name, ARRAYSIZE(name));
    if (GetWindow(window, GW_OWNER) == dialog_owner && std::wstring_view(name) == L"PulseInputDialog") {
        *reinterpret_cast<HWND*>(parameter) = window;
        return FALSE;
    }
    return TRUE;
}

inline void CALLBACK DriveInput(HWND, UINT, UINT_PTR timer, DWORD) {
    HWND dialog = nullptr;
    EnumThreadWindows(GetCurrentThreadId(), FindInput, reinterpret_cast<LPARAM>(&dialog));
    if (!dialog || !GetDlgItem(dialog, 100)) {
        if (++timer_ticks > 100 && dialog) SendMessageW(dialog, WM_CLOSE, 0, 0);
        return;
    }
    KillTimer(nullptr, timer);
    dialog_driven = true;
    SetWindowTextW(GetDlgItem(dialog, 100), dialog_text.c_str());
    SendMessageW(dialog, WM_COMMAND, IDOK, 0);
    rejection_observed = IsWindow(dialog) != FALSE;
    if (IsWindow(dialog)) SendMessageW(dialog, WM_COMMAND, IDCANCEL, 0);
}

inline bool DialogAction(AppState& state, int action, int command, std::wstring text, bool reject = false) {
    dialog_owner = state.hwnd; dialog_text = std::move(text);
    dialog_driven = rejection_observed = false; timer_ticks = 0;
    const auto timer = SetTimer(nullptr, 0, 50, DriveInput);
    if (!timer) return false;
    ui::HitTestResult hit;
    hit.region = ui::HitTestResult::SettingsShortcut; hit.index = action; hit.sub_index = command;
    ActivateSettingsControl(state, hit);
    KillTimer(nullptr, timer);
    return dialog_driven && (reject ? rejection_observed : !rejection_observed);
}

inline bool Nonoverlapping(D2D1_RECT_F a, D2D1_RECT_F b) {
    return a.right <= b.left + 0.5f || b.right <= a.left + 0.5f ||
        a.bottom <= b.top + 0.5f || b.bottom <= a.top + 0.5f;
}
} // namespace pulse::bench::shortcut_settings_detail

namespace pulse::bench {
inline void RunShortcutSettingsChecks(const std::filesystem::path& root,
                                      const std::function<void(bool,const char*)>& check) {
    using namespace shortcut_settings_detail;
    using H = ui::HitTestResult;
    std::filesystem::create_directories(root / L"shortcuts");
    check(app::SettingsController::PageFromName(L"shortcuts") == 10 &&
          std::wstring_view(app::SettingsController::PageName(10)) == L"shortcuts",
          "shortcut settings page has a stable direct route");
    auto owned = std::make_unique<AppState>();
    auto& s = *owned;
    s.isolatedTest = true;
    s.appPrefs.persist = s.places.persist = s.ctxMenuPrefs.persist = s.searchHistory.persist = false;
    s.hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"STATIC", L"Shortcut settings fixture",
        WS_POPUP, -20000, -20000, 1200, 900, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!s.hwnd || !s.compositor.Init(s.hwnd)) {
        check(false, "initialize isolated shortcut settings renderer");
        if (s.hwnd) DestroyWindow(s.hwnd);
        return;
    }
    s.renderer.SetCompositor(&s.compositor);
    s.renderer.SetDetailsPanelVisible(false);
    s.window_tabs.EnsureDefault(); s.pane = s.window_tabs.Active()->FocusedPane();
    s.pane->NewTab(app::MakeSettingsPath(L"shortcuts"));
    ActiveTab(s)->loading = false;
    ActiveTab(s)->SetSnapshot(std::make_shared<const std::vector<fs::DirEntry>>());
    s.settings.BindUi(s.appPrefs, s.ctxMenuPrefs, s.index, s.networkIndex, {});
    s.settings.SelectPage(10);
    const auto toolbar = s.appPrefs.toolbar_order;
    check(s.settings.page() == 10 && BuildVm(s, false).settings_page == 10,
        "production BuildVm resolves the shortcuts settings page");
    const auto rows = app::BuildShortcutSettingsRows({}, {});
    check(rows.size() == app::GetCommandCatalog().size() && !rows.empty(),
        "shortcut settings exposes ordinary command rows from the catalog");
    app::CommandShortcut f8{VK_F8};
    s.appPrefs.command_shortcuts = {{app::CmdCopy, f8}};
    app::ConfigureCommandShortcuts(s.appPrefs.command_shortcuts);
    HandleShortcutSettingsAction(s, ui::ShortcutRemove, app::CmdCopy);
    check(app::EffectiveCommandShortcut(app::CmdCopy).key == 0,
        "remove binding action disables its default shortcut");
    app::AppPrefs restored;
    check(restored.FromJson(s.appPrefs.ToJson()) && restored.command_shortcuts.size() == 1 &&
        restored.command_shortcuts[0].shortcut.key == 0,
        "removed shortcut round trips through preferences without reappearing");
    HandleShortcutSettingsAction(s, ui::ShortcutReset, app::CmdCopy);
    check(app::EffectiveCommandShortcut(app::CmdCopy) == app::FindCommandDescriptor(app::CmdCopy)->shortcut,
        "reset one shortcut restores the catalog default");
    s.appPrefs.global_search_enabled = true;
    s.appPrefs.global_search_modifiers = MOD_CONTROL | MOD_SHIFT;
    s.appPrefs.global_search_key = 'Y';
    check(DialogAction(s, ui::ShortcutEdit, app::CmdCopy, L"Ctrl+T", true) &&
        app::EffectiveCommandShortcut(app::CmdCopy).key == 'C',
        "shortcut input rejects reserved navigation keys and cancel preserves the binding");
    check(DialogAction(s, ui::ShortcutEdit, app::CmdCopy, L"Ctrl+V", true) &&
        app::EffectiveCommandShortcut(app::CmdCopy).key == 'C',
        "shortcut input rejects another command's effective binding");
    check(DialogAction(s, ui::ShortcutEdit, app::CmdCopy, L"Ctrl+Shift+Y", true) &&
        app::EffectiveCommandShortcut(app::CmdCopy).key == 'C',
        "shortcut input protects the configured global search combination");
    check(DialogAction(s, ui::ShortcutEdit, app::CmdCopy, L"F8") &&
        app::LookupCommandShortcut(VK_F8, false, false, false) == app::CmdCopy,
        "ordinary settings edit accepts a valid shortcut and applies dispatch immediately");
    const auto copy_rows = app::BuildShortcutSettingsRows(s.appPrefs.command_shortcuts, L"f8");
    check(copy_rows.size() == 1 && copy_rows[0].command == app::CmdCopy && copy_rows[0].customized,
        "shortcut search matches effective bindings without case sensitivity");
    check(DialogAction(s, ui::ShortcutFilter, -1, L"f8") && s.settings.shortcut_filter() == L"f8" &&
        BuildVm(s, false).settings_shortcuts.size() == 1,
        "search input filters the actual settings page model");
    HandleShortcutSettingsAction(s, ui::ShortcutClearFilter, -1);
    check(s.settings.shortcut_filter().empty(), "clear search restores the full command list");
    s.appPrefs.global_search_modifiers = MOD_CONTROL; s.appPrefs.global_search_key = 'C';
    HandleShortcutSettingsAction(s, ui::ShortcutResetAll, -1);
    check(app::EffectiveCommandShortcut(app::CmdCopy).key == VK_F8 &&
        s.appPrefs.global_search_key == 'C' && s.appPrefs.global_search_modifiers == MOD_CONTROL,
        "reset all refuses a conflicting global search key without changing either binding");
    s.appPrefs.global_search_modifiers = MOD_CONTROL | MOD_SHIFT; s.appPrefs.global_search_key = 'Y';
    HandleShortcutSettingsAction(s, ui::ShortcutResetAll, -1);
    check(s.appPrefs.command_shortcuts.empty() && s.appPrefs.toolbar_order == toolbar &&
        app::LookupCommandShortcut('C', true, false, false) == app::CmdCopy,
        "reset all shortcuts restores defaults without changing toolbar configuration");
    H global; global.region = H::SettingsGlobalSearchHotkey;
    ActivateSettingsControl(s, global);
    check(s.settings.global_search_hotkey_capturing(), "global search capture remains reachable from shortcuts settings");
    s.settings.CancelGlobalSearchHotkeyCapture();
    s.appPrefs.command_shortcuts = {{app::CmdCopy, f8}, {app::CmdRename, {}}};
    app::ConfigureCommandShortcuts(s.appPrefs.command_shortcuts);
    ui::fluent::Painter measuring(&s.compositor);
    for (const auto language : {L"zh-CN", L"en-US"}) for (const bool dark : {false, true})
    for (const float scale : {1.0f, 1.5f, 2.0f}) for (const int width : {620, 1480}) {
        l10n::SetLanguage(language);
        s.appPrefs.language = language; s.darkMode = dark; s.scale = scale;
        s.renderer.SetScale(scale); s.compositor.RecreateTextFormats(scale); s.renderer.InvalidateTypography();
        s.compositor.Resize(static_cast<UINT>(width * scale), static_cast<UINT>(900 * scale));
        measuring.SetScale(scale);
        const auto theme = ui::MakeTheme(dark, ui::HexColor(0x0078D4));
        measuring.BeginFrame(theme);
        const auto rect = D2D1::RectF(0, 0, width * scale, 900 * scale);
        s.settings.SetScroll(0, 100000);
        auto vm = BuildVm(s, false);
        const auto layout = ui::MakeSettingsLayout(vm, rect, scale, s.renderer.TitleBarHeight(), 28 * scale,
            &measuring, s.renderer.EffectiveSidebarWidth(rect.right));
        auto index_vm = vm; index_vm.settings_page = 1;
        const auto index_controls = s.renderer.SettingsControls(index_vm, rect);
        const auto shortcut_controls = s.renderer.SettingsControls(vm, rect);
        check(std::none_of(index_controls.begin(), index_controls.end(), [](const auto& target) {
            return target.region == H::SettingsGlobalSearchHotkey;
        }) && std::count_if(shortcut_controls.begin(), shortcut_controls.end(), [](const auto& target) {
            return target.region == H::SettingsGlobalSearchHotkey;
        }) == 1 && std::any_of(index_controls.begin(), index_controls.end(), [](const auto& target) {
            return target.region == H::SettingsToggle && target.index == 15;
        }), "global search recorder has one settings entry while search enable remains in indexing settings");
        check(!layout.shortcut_rows.empty() && layout.nav_row[10].bottom > layout.nav_row[10].top,
            "shortcut page has a real settings navigation item and row layout");
        bool fits = true;
        for (const auto& row : layout.shortcut_rows) {
            fits &= row.card.left >= layout.content.left && row.card.right <= layout.content.right &&
                row.title.left >= row.card.left && row.reset.right <= row.card.right &&
                Nonoverlapping(row.title, row.binding) && Nonoverlapping(row.binding, row.remove) &&
                Nonoverlapping(row.remove, row.reset) && row.binding.right > row.binding.left;
        }
        check(fits, "shortcut titles and action controls do not overlap at narrow widths or high DPI");
        const auto& first = layout.shortcut_rows.front();
        const auto hit = s.renderer.HitTest(vm, rect, (first.binding.left + first.binding.right) * 0.5f,
            (first.binding.top + first.binding.bottom) * 0.5f);
        check(hit.region == H::SettingsShortcut && hit.index == ui::ShortcutEdit &&
            hit.sub_index == vm.settings_shortcuts.front().command,
            "shortcut row drawing and pointer hit tests identify the same command");
        auto* dc = s.compositor.Dc();
        dc->BeginDraw(); dc->Clear(D2D1::ColorF(0, 0));
        s.renderer.Render(vm, rect, theme);
        check(SUCCEEDED(dc->EndDraw()), "render production shortcuts settings page");
        Sleep(190);
        dc->BeginDraw(); dc->Clear(D2D1::ColorF(0, 0));
        s.renderer.Render(vm, rect, theme);
        check(SUCCEEDED(dc->EndDraw()), "render settled shortcuts settings navigation selection");
        const auto filename = std::wstring(language) + (dark ? L"-dark-" : L"-light-") +
            std::to_wstring(static_cast<int>(scale * 100)) + L"-" + std::to_wstring(width) + L".png";
        check(s.compositor.SaveSnapshot((root / L"shortcuts" / filename).c_str()), "save shortcuts settings page screenshot");
        if (width == 620 && scale == 1.5f) {
            vm.settings_global_search_capturing = true;
            vm.settings_global_search_error = l10n::Get(l10n::StringId::GlobalSearchConflict);
            const auto error_layout = ui::MakeSettingsLayout(vm, rect, scale, s.renderer.TitleBarHeight(), 28 * scale,
                &measuring, s.renderer.EffectiveSidebarWidth(rect.right));
            const auto& error = error_layout.shortcut_global_error;
            check(error.bottom > error.top && error.top >= error_layout.global_search_row.bottom &&
                error.bottom <= error_layout.shortcut_rows.front().card.top &&
                error_layout.global_search_hotkey_button.bottom <= error_layout.global_search_row.bottom,
                "global recorder state and wrapped conflict text fit the narrow settings page");
            dc->BeginDraw(); dc->Clear(D2D1::ColorF(0, 0));
            s.renderer.Render(vm, rect, theme);
            check(SUCCEEDED(dc->EndDraw()) && s.compositor.SaveSnapshot((root / L"shortcuts" / (L"recording-error-" + filename)).c_str()),
                "save narrow recorder and conflict state screenshot");
        }
    }
    app::ConfigureCommandShortcuts({});
    s.settings.ResetUi(); s.watches.Stop();
    s.renderer.SetCompositor(nullptr); s.compositor.Shutdown();
    DestroyWindow(s.hwnd); s.hwnd = nullptr;
}
} // namespace pulse::bench
