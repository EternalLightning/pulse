// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "../app/app_internal.h"
#include "../app/home_quick_access_ui.h"
#include "../app/settings_input.h"
#include "../common/user_storage.h"
#include "../common/utf8_file.h"
#include "../ui/ui_renderer_internal.h"
#include <filesystem>
#include <functional>
#include <tuple>

namespace pulse::bench::home_access_detail {
inline bool Separate(D2D1_RECT_F a, D2D1_RECT_F b) {
    return a.right <= b.left + 0.5f || b.right <= a.left + 0.5f || a.bottom <= b.top + 0.5f || b.bottom <= a.top + 0.5f;
}
} // namespace pulse::bench::home_access_detail

namespace pulse::bench {
inline void RunHomeQuickAccessSettingsChecks(const std::filesystem::path& root,
                                             const std::function<void(bool,const char*)>& check) {
    using namespace home_access_detail;
    using H = ui::HitTestResult;
    namespace fsys = std::filesystem;
    const auto output = fsys::absolute(root / L"quick-access-settings");
    const auto profile = output / L"profile";
    fsys::create_directories(profile);
    if (fsys::is_directory(profile / L"app.json")) fsys::remove(profile / L"app.json");
    struct StorageScope {
        std::wstring previous = storage::DefaultRoot();
        ~StorageScope() { storage::OverrideDefaultRootForTesting(previous); }
    } storage_scope;
    storage::OverrideDefaultRootForTesting(profile.wstring());
    auto owned = std::make_unique<AppState>(); auto& s = *owned;
    s.isolatedTest = true;
    s.appPrefs.persist = true;
    s.places.persist = s.ctxMenuPrefs.persist = s.searchHistory.persist = false;
    s.hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"STATIC", L"Quick access settings fixture",
        WS_POPUP, -20000, -20000, 1200, 900, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!s.hwnd || !s.compositor.Init(s.hwnd)) {
        check(false, "initialize quick access settings renderer");
        if (s.hwnd) DestroyWindow(s.hwnd);
        return;
    }
    s.renderer.SetCompositor(&s.compositor); s.renderer.SetDetailsPanelVisible(false);
    s.window_tabs.EnsureDefault(); s.pane = s.window_tabs.Active()->FocusedPane();
    s.pane->NewTab(app::MakeSettingsPath(L"quick-access"));
    ActiveTab(s)->loading = false;
    ActiveTab(s)->SetSnapshot(std::make_shared<const std::vector<fs::DirEntry>>());
    s.settings.SelectPage(11);
    check(app::SettingsController::PageFromName(L"quick-access") == 11 &&
        std::wstring_view(app::SettingsController::PageName(11)) == L"quick-access" &&
        BuildVm(s, false).settings_page == 11, "quick access has a direct settings route and production page model");
    s.core.windows.Add(s.hwnd, &s);
    auto second = std::make_unique<AppState>(s.core);
    second->isolatedTest = true;
    second->hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"STATIC", L"Quick access secondary fixture",
        WS_POPUP, -20000, -20000, 500, 400, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    s.core.windows.Add(second->hwnd, second.get());
    s.hitTestVmValid = second->hitTestVmValid = true;
    check(HandleHomeQuickAccessSettingsAction(s, ui::HomeAccessToggle, 0) &&
        (s.appPrefs.home_quick_access.hidden & 1u) && !s.hitTestVmValid && !second->hitTestVmValid,
        "changing a default folder saves immediately and invalidates every shared window");
    std::wstring saved;
    app::AppPrefs restored;
    check(ReadUtf8File((profile / L"app.json").wstring(), saved) && restored.FromJson(saved) &&
        restored.home_quick_access == s.appPrefs.home_quick_access,
        "quick access setting action writes the isolated preferences file");
    for (int bit = 4; bit < 8; ++bit)
        check(HandleHomeQuickAccessSettingsAction(s, ui::HomeAccessToggle, bit) &&
            (s.appPrefs.home_quick_access.hidden & (1u << bit)), "recycle and each special system location can be hidden independently");
    for (const int bit : {8, 9})
        check(HandleHomeQuickAccessSettingsAction(s, ui::HomeAccessToggle, bit) &&
            (s.appPrefs.home_quick_access.hidden & (1u << bit)), "Documents and Music each have an independent saved visibility switch");
    const auto folder_a = (output / L"工作资料").wstring();
    const auto folder_b = (output / L"课程与项目文件夹").wstring();
    fsys::create_directories(folder_a); fsys::create_directories(folder_b);
    check(HandleHomeQuickAccessSettingsAction(s, ui::HomeAccessAddFolder, -1, folder_a),
        "custom folder action adds an absolute folder through the remaining action");
    check(HandleHomeQuickAccessSettingsAction(s, ui::HomeAccessAddFolder, -1, folder_b) &&
        s.appPrefs.home_quick_access.folders.size() == 2, "custom folder add action accepts a second absolute path");
    check(!HandleHomeQuickAccessSettingsAction(s, ui::HomeAccessAddFolder, -1, folder_a) &&
        !HandleHomeQuickAccessSettingsAction(s, ui::HomeAccessAddFolder, -1, L"relative\\folder"),
        "custom folder action rejects duplicate and relative paths without saving them");
    check(HandleHomeQuickAccessSettingsAction(s, ui::HomeAccessMoveUp, 1, folder_b) &&
        s.appPrefs.home_quick_access.folders.front() == folder_b,
        "move up changes custom folder display order");
    check(HandleHomeQuickAccessSettingsAction(s, ui::HomeAccessMoveDown, 0, folder_b) &&
        s.appPrefs.home_quick_access.folders.back() == folder_b,
        "move down changes custom folder display order");
    check(HandleHomeQuickAccessSettingsAction(s, ui::HomeAccessRemove, 0, folder_b) &&
        s.appPrefs.home_quick_access.folders == std::vector<std::wstring>{folder_a},
        "remove action follows its stable folder path even if the pressed row index is stale");
    const auto before_failure = s.appPrefs.home_quick_access;
    fsys::remove(profile / L"app.json");
    fsys::create_directory(profile / L"app.json");
    check(!HandleHomeQuickAccessSettingsAction(s, ui::HomeAccessToggle, 1) &&
        s.appPrefs.home_quick_access == before_failure,
        "failed preference save rolls back the quick access model");
    fsys::remove(profile / L"app.json");
    check(HandleHomeQuickAccessSettingsAction(s, ui::HomeAccessReset) &&
        s.appPrefs.home_quick_access == model::HomeQuickAccessPrefs{},
        "restore defaults clears hidden locations and custom folders");
    auto empty_vm = BuildVm(s, false);
    check(empty_vm.settings_home_access_folders.empty() && empty_vm.settings_home_access_hidden == 0,
        "empty custom folder settings remain a valid immediate view model");
    HandleHomeQuickAccessSettingsAction(s, ui::HomeAccessAddFolder, -1, folder_a);
    HandleHomeQuickAccessSettingsAction(s, ui::HomeAccessAddFolder, -1, folder_b);
    HandleHomeQuickAccessSettingsAction(s, ui::HomeAccessAddFolder, -1, LR"(\\server\share\跨部门协作\长期项目资料与评审归档\设计文件夹)");
    ui::fluent::Painter measuring(&s.compositor);
    const std::tuple<const wchar_t*, bool, float, int> cases[] = {
        {L"zh-CN", false, 1.0f, 620}, {L"en-US", false, 1.5f, 1480},
        {L"en-US", true, 2.0f, 620}, {L"zh-CN", true, 1.5f, 1480}};
    for (const auto& [language, dark, scale, width] : cases) {
        l10n::SetLanguage(language); s.appPrefs.language = language; s.darkMode = dark; s.scale = scale;
        s.renderer.SetScale(scale); s.compositor.RecreateTextFormats(scale); s.renderer.InvalidateTypography();
        s.compositor.Resize(static_cast<UINT>(width * scale), static_cast<UINT>(900 * scale));
        measuring.SetScale(scale);
        const auto theme = ui::MakeTheme(dark, ui::HexColor(0x0078D4)); measuring.BeginFrame(theme);
        const auto rect = D2D1::RectF(0, 0, width * scale, 900 * scale);
        s.settings.SetScroll(0, 100000);
        auto vm = BuildVm(s, false);
        auto layout = ui::MakeSettingsLayout(vm, rect, scale, s.renderer.TitleBarHeight(), 28 * scale,
            &measuring, s.renderer.EffectiveSidebarWidth(rect.right));
        check(std::all_of(model::kHomeQuickAccessFolderBits.begin(), model::kHomeQuickAccessFolderBits.end(), [&](int bit) {
            const auto& row = layout.home_access_builtin[bit];
            return row.right > row.left && row.top >= layout.home_access_sections[0].bottom &&
                row.bottom < layout.home_access_sections[1].top;
        }), "library folders section lays out all six common folders before custom folders");
        check(layout.home_access_add.right-layout.home_access_add.left >=
            measuring.MeasureButtonWidth(l10n::Pick(L"添加文件夹…", L"Add folder…")) &&
            Separate(layout.home_access_add, layout.home_access_reset),
            "two remaining header actions fit their full labels including the intentional ellipsis");
        const auto hit = s.renderer.HitTest(vm, rect, (layout.home_access_builtin[0].left + layout.home_access_builtin[0].right) * 0.5f,
            (layout.home_access_builtin[0].top + layout.home_access_builtin[0].bottom) * 0.5f);
        check(hit.region == H::SettingsHomeQuickAccess && hit.index == ui::HomeAccessToggle && hit.sub_index == 0,
            "default folder switch paint and hit target use the same layout");
        bool fits = layout.nav_row[11].bottom > layout.nav_row[11].top;
        for (const auto& row : layout.home_access_folders) fits &= row.card.right <= layout.content.right &&
            row.title.left >= row.card.left && row.remove.right <= row.card.right &&
            row.path.bottom <= row.card.bottom && Separate(row.path, row.up) && Separate(row.up, row.down) && Separate(row.down, row.remove);
        check(fits, "custom folder paths and actions fit narrow, wide and high DPI settings layouts");
        const auto base = std::wstring(language) + (dark ? L"-dark-" : L"-light-") +
            std::to_wstring(static_cast<int>(scale * 100)) + L"-" + std::to_wstring(width);
        const auto render = [&](const ui::WindowViewModel& frame, const std::wstring& suffix) {
            auto* dc = s.compositor.Dc(); dc->BeginDraw(); dc->Clear(D2D1::ColorF(0, 0));
            s.renderer.Render(frame, rect, theme);
            check(SUCCEEDED(dc->EndDraw()) && s.compositor.SaveSnapshot((output / (base + suffix + L".png")).c_str()),
                "render and save real quick access settings page");
        };
        render(vm, L"-top");
        vm.settings_scroll = std::max(0.0f, layout.home_access_sections[1].top - layout.content.top - 16 * scale);
        layout = ui::MakeSettingsLayout(vm, rect, scale, s.renderer.TitleBarHeight(), 28 * scale,
            &measuring, s.renderer.EffectiveSidebarWidth(rect.right));
        const auto& remove = layout.home_access_folders[0].remove;
        const auto remove_hit = s.renderer.HitTest(vm, rect, (remove.left + remove.right) * 0.5f, (remove.top + remove.bottom) * 0.5f);
        check(remove_hit.region == H::SettingsHomeQuickAccess && remove_hit.index == ui::HomeAccessRemove && remove_hit.path == folder_a,
            "custom folder hit target carries the stable filesystem path");
        render(vm, L"-custom-and-system");
        auto system_vm = vm;
        system_vm.settings_scroll += std::max(0.0f, layout.home_access_sections[2].top - layout.content.top - 16 * scale);
        render(system_vm, L"-system");
        empty_vm = vm; empty_vm.settings_scroll = 0; empty_vm.settings_home_access_folders.clear();
        const auto empty_controls = s.renderer.SettingsControls(empty_vm, rect);
        check(std::any_of(empty_controls.begin(), empty_controls.end(), [](const auto& target) {
            return target.region == H::SettingsHomeQuickAccess && target.index == ui::HomeAccessAddFolder;
        }), "empty custom folder list keeps its add folder action");
        render(empty_vm, L"-empty");
    }
    s.core.windows.Remove(second->hwnd); DestroyWindow(second->hwnd); second->hwnd = nullptr; second.reset();
    s.core.windows.Remove(s.hwnd);
    s.renderer.SetCompositor(nullptr); s.compositor.Shutdown(); DestroyWindow(s.hwnd); s.hwnd = nullptr;
}
} // namespace pulse::bench
