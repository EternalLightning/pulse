// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
#pragma once

#include "../app/app_input.h"
#include "../app/app_navigation.h"
#include "../app/settings_input.h"
#include "../common/user_storage.h"
#include "../ui/ui_renderer_internal.h"
#include "snapshot_output.h"
#include <array>

int RunSettingsInputCases() {
    using namespace pulse;
    using H = ui::HitTestResult;
    if (FAILED(OleInitialize(nullptr))) return 2;
    const auto output = std::filesystem::absolute(L"bench_data/ui-consistency-fix/settings-input");
    std::filesystem::create_directories(output);
    storage::OverrideDefaultRootForTesting((output / L"isolated-data").wstring());
    l10n::Initialize(GetModuleHandleW(nullptr), L"zh-CN");
    int checks = 0, failures = 0;
    auto check = [&](bool ok, const char* label) {
        ++checks;
        if (!ok) { ++failures; std::cout << "[FAIL] " << label << std::endl; }
    };
    struct KeyboardState {
        std::array<BYTE, 256> saved{};
        bool valid = GetKeyboardState(saved.data()) != FALSE;
        bool Modifiers(bool shift = false) const {
            if (!valid) return false;
            auto current = saved;
            for (int key : {VK_SHIFT, VK_LSHIFT, VK_RSHIFT, VK_CONTROL, VK_LCONTROL,
                            VK_RCONTROL, VK_MENU, VK_LMENU, VK_RMENU}) current[key] = 0;
            if (shift) current[VK_SHIFT] = current[VK_LSHIFT] = 0x80;
            return SetKeyboardState(current.data()) != FALSE;
        }
        ~KeyboardState() { if (valid) SetKeyboardState(saved.data()); }
    } keyboard;
    check(keyboard.Modifiers(), "isolate thread keyboard modifiers without sending input");
    auto owned = std::make_unique<AppState>();
    auto& state = *owned;
    state.isolatedTest = true;
    state.shot.active = true;
    state.appPrefs.persist = state.places.persist = state.ctxMenuPrefs.persist = state.searchHistory.persist = false;
    state.hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"STATIC", L"Settings input fixture",
        WS_POPUP, -20000, -20000, 1100, 900, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!state.hwnd || !state.compositor.Init(state.hwnd)) {
        if (state.hwnd) DestroyWindow(state.hwnd);
        owned.reset(); OleUninitialize(); return 2;
    }
    state.renderer.SetCompositor(&state.compositor);
    state.scale = 1.0f;
    state.renderer.SetScale(state.scale);
    state.compositor.RecreateTextFormats(state.scale);
    state.renderer.InvalidateTypography();
    check(state.compositor.TextFormat() != nullptr && state.compositor.SmallFormat() != nullptr,
        "fixture initializes production text formats before any layout or rendering");
    if (!state.compositor.TextFormat() || !state.compositor.SmallFormat()) {
        state.renderer.SetCompositor(nullptr); state.compositor.Shutdown();
        DestroyWindow(state.hwnd); state.hwnd = nullptr;
        owned.reset(); OleUninitialize(); return 2;
    }
    state.renderer.SetDetailsPanelVisible(false);
    state.window_tabs.EnsureDefault();
    BindCurrentLayout(state);
    state.pane->NewTab(app::MakeSettingsPath(L"files"));
    ActiveTab(state)->loading = false;
    ActiveTab(state)->SetSnapshot(std::make_shared<std::vector<fs::DirEntry>>());
    app::SettingsController::UiCallbacks callbacks;
    callbacks.apply_effects = [&](app::SettingsEffect effect) { ApplySettingsEffects(state, effect); };
    state.settings.BindUi(state.appPrefs, state.ctxMenuPrefs, state.index, state.networkIndex, std::move(callbacks));
    auto rect = [&] {
        return D2D1::RectF(0, 0, static_cast<float>(state.compositor.Width()),
            static_cast<float>(state.compositor.Height()));
    };
    auto targets = [&] { return state.renderer.SettingsControls(BuildVm(state, false), rect()); };
    auto same = [](const H& a, const H& b) {
        return a.region == b.region && a.index == b.index && a.sub_index == b.sub_index && a.path == b.path;
    };
    auto find = [](const std::vector<H>& controls, H::Region region, int index, int sub_index = -1) {
        for (const auto& target : controls)
            if (target.region == region && target.index == index && target.sub_index == sub_index) return target;
        return H{};
    };
    auto page = [&](int value) {
        CancelSettingsPointer(state);
        OpenSettingsTab(state, value);
        state.settings.SetScroll(0, 0);
        state.hitTestVmValid = false;
    };
    auto visible = [&](H::Region region, int index, int sub_index = -1) {
        const auto target = find(targets(), region, index, sub_index);
        check(target.region == region, "requested production input target exists");
        const auto vm = BuildVm(state, false);
        const auto content = state.renderer.SettingsContentBounds(vm, rect());
        const float offset = state.settings.scroll() + target.control_bounds.top - content.top - 16 * state.scale;
        state.settings.SetScroll((std::max)(0.0f, offset),
            state.renderer.SettingsMaxScroll(vm, rect().right, rect().bottom));
        return find(targets(), region, index, sub_index);
    };
    auto center = [](const H& target) {
        const auto& r = target.control_bounds;
        return POINT{static_cast<LONG>((r.left + r.right) * 0.5f),
            static_cast<LONG>((r.top + r.bottom) * 0.5f)};
    };
    auto down = [&](POINT point) {
        HandleLButtonDown(&state, state.hwnd, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(point.x, point.y));
    };
    auto up = [&](POINT point) {
        HandleLButtonUp(&state, state.hwnd, WM_LBUTTONUP, 0, MAKELPARAM(point.x, point.y));
    };

    page(6);
    auto hidden = visible(H::SettingsToggle, 5);
    auto point = center(hidden);
    auto hit = state.renderer.HitTest(BuildVm(state, false), rect(), static_cast<float>(point.x), static_cast<float>(point.y));
    check(hit.region == H::SettingsToggle && hit.index == 5, "safe toggle coordinates use the real production HitTest");
    const bool initial_hidden = state.appPrefs.show_hidden_files;
    down(point);
    check(state.appPrefs.show_hidden_files == initial_hidden && state.settings_input.pressed.index == 5,
        "button down arms the preference without activating it");
    auto pressed_vm = BuildVm(state, false);
    check(pressed_vm.settings_pressed_region == H::SettingsToggle && pressed_vm.settings_pressed_index == 5,
        "BuildVm propagates the pending press");
    state.compositor.Dc()->BeginDraw();
    state.renderer.Render(pressed_vm, rect(), ui::MakeTheme(false, ui::HexColor(0x0078D4)));
    check(SUCCEEDED(state.compositor.Dc()->EndDraw()) &&
        bench::SaveSnapshot(state.compositor, output / L"pressed-toggle.png"),
        "capture production pressed state before release");
    up(point);
    check(state.appPrefs.show_hidden_files != initial_hidden && state.settings_input.pressed.region == H::None,
        "release on the same control activates exactly once");
    up(point);
    check(state.appPrefs.show_hidden_files != initial_hidden, "an extra release cannot activate the previous press");
    down(point); up(point);
    check(state.appPrefs.show_hidden_files == initial_hidden, "second complete click restores the isolated preference");

    for (int cancellation = 0; cancellation < 5; ++cancellation) {
        page(6); hidden = visible(H::SettingsToggle, 5); point = center(hidden);
        down(point);
        check(state.settings_input.pressed.region == H::SettingsToggle, "cancellation case begins with an armed press");
        if (cancellation == 0) {
            up({0, 0});
        } else if (cancellation == 1) {
            HandleCaptureChanged(&state, state.hwnd, WM_CAPTURECHANGED, 0, 0);
        } else if (cancellation == 2) {
            POINT screen = point; ClientToScreen(state.hwnd, &screen);
            HandleMouseWheel(&state, state.hwnd, WM_MOUSEWHEEL, MAKEWPARAM(0, WHEEL_DELTA),
                MAKELPARAM(screen.x, screen.y));
        } else if (cancellation == 3) {
            check(HandleSettingsKey(state, VK_ESCAPE, 0), "Escape consumes an armed settings press");
        } else {
            OpenSettingsTab(state, 7);
        }
        up(point);
        check(state.settings_input.pressed.region == H::None && state.appPrefs.show_hidden_files == initial_hidden,
            "outside release, capture loss, wheel, Escape and page switch cancel activation");
    }

    page(6);
    state.settings_input = {};
    check(HandleSettingsKey(state, VK_TAB, 0), "Tab enters settings keyboard navigation");
    auto controls = targets();
    check(controls.size() >= 2, "keyboard fixture exposes at least two enabled settings targets");
    if (controls.size() < 2) {
        CancelSettingsPointer(state); state.settings.ResetUi();
        state.renderer.SetCompositor(nullptr); state.compositor.Shutdown();
        DestroyWindow(state.hwnd); state.hwnd = nullptr;
        owned.reset(); OleUninitialize(); return 2;
    }
    check(same(state.settings_input.focus, controls.front()), "first Tab selects the first navigation target");
    check(keyboard.Modifiers(true), "set thread Shift state for reverse Tab");
    state.scrollAnimating = true;
    state.scrollVelocityY = 600.0f;
    HandleSettingsKey(state, VK_TAB, 0);
    check(!state.scrollAnimating && state.scrollVelocityY == 0.0f,
        "keyboard navigation stops wheel inertia before revealing the focused control");
    check(same(state.settings_input.focus, controls.back()), "Shift Tab wraps to the last enabled target");
    check(keyboard.Modifiers(), "restore unmodified keyboard navigation");
    HandleSettingsKey(state, VK_TAB, 0);
    HandleSettingsKey(state, VK_DOWN, 0);
    check(same(state.settings_input.focus, controls[1]), "Down moves to the next target in reading order");
    HandleSettingsKey(state, VK_LEFT, 0);
    check(same(state.settings_input.focus, controls.front()), "Left reverses target navigation");
    hidden = visible(H::SettingsToggle, 5);
    state.settings_input.focus = hidden;
    state.settings_input.keyboard = true;
    HandleSettingsKey(state, VK_RETURN, 0);
    check(state.appPrefs.show_hidden_files != initial_hidden, "Enter activates the keyboard focused toggle");
    HandleSettingsKey(state, VK_RETURN, static_cast<LPARAM>(1) << 30);
    check(state.appPrefs.show_hidden_files != initial_hidden, "held Enter does not repeatedly toggle a preference");
    HandleSettingsKey(state, VK_SPACE, 0);
    HandleSettingsKey(state, VK_SPACE, static_cast<LPARAM>(1) << 30);
    check(state.appPrefs.show_hidden_files == initial_hidden, "Space activates once and ignores autorepeat");
    SetFocus(state.hwnd);
    if (GetFocus() == state.hwnd) {
        const auto focused_vm = BuildVm(state, false);
        check(focused_vm.settings_focus_region == H::SettingsToggle && focused_vm.settings_focus_index == 5,
            "BuildVm propagates keyboard focus when the fixture owns Win32 focus");
    } else std::cout << "[INFO] hidden fixture cannot acquire Win32 focus; focus propagation check unavailable\n";

    for (float scale : {1.0f, 1.5f}) {
        state.scale = scale; state.renderer.SetScale(scale);
        state.compositor.RecreateTextFormats(scale); state.renderer.InvalidateTypography();
        state.compositor.Resize(static_cast<UINT>(620 * scale), static_cast<UINT>(520 * scale));
        page(6);
        HandleSettingsKey(state, VK_TAB, 0);
        controls = targets();
        const auto last = find(controls, H::SettingsToggle, 4);
        const auto at = std::find_if(controls.begin(), controls.end(), [&](const H& target) { return same(target, last); });
        check(at != controls.end() && at != controls.begin() && last.control_bounds.top > rect().bottom,
            "narrow fixture contains a real focus target below the viewport");
        if (at != controls.end() && at != controls.begin()) {
            state.settings_input.focus = *(at - 1);
            state.settings_input.keyboard = true;
            HandleSettingsKey(state, VK_DOWN, 0);
            const auto content = state.renderer.SettingsContentBounds(BuildVm(state, false), rect());
            const auto& focus = state.settings_input.focus;
            check(same(focus, last) && state.settings.scroll() > 0 &&
                focus.control_bounds.top >= content.top - 0.5f && focus.control_bounds.bottom <= content.bottom + 0.5f,
                "keyboard focus scrolls a lower control fully into view at 100 and 150 percent");
            auto vm = BuildVm(state, false);
            vm.dark = true;
            vm.settings_focus_region = focus.region; vm.settings_focus_index = focus.index;
            vm.settings_focus_sub_index = focus.sub_index; vm.settings_focus_key = focus.path;
            state.compositor.Dc()->BeginDraw();
            state.renderer.Render(vm, rect(), ui::MakeTheme(true, ui::HexColor(0x0078D4)));
            check(SUCCEEDED(state.compositor.Dc()->EndDraw()), "focused narrow settings render successfully");
            check(bench::SaveSnapshot(state.compositor,
                output / (L"focus-narrow-" + std::to_wstring(static_cast<int>(scale * 100)) + L".png")),
                "capture the actual focused control for visual review");
        }
    }

    state.scale = 1; state.renderer.SetScale(1);
    state.compositor.RecreateTextFormats(1); state.renderer.InvalidateTypography();
    state.compositor.Resize(1100, 900);
    page(4);
    state.duplicateScan.groups.clear(); ++state.duplicateScan.result_epoch;
    state.duplicateScan.scanning = true;
    state.settings_input = {};
    for (int i = 0; i < 10; ++i) HandleSettingsKey(state, VK_TAB, 0);
    check(state.settings_input.focus.region == H::SettingsDupCancel,
        "Tab skips every disabled duplicate option and scan button while scanning");
    const auto vm_scanning = BuildVm(state, false);
    ui::fluent::Painter measuring(&state.compositor);
    const auto disabled_layout = ui::MakeSettingsLayout(vm_scanning, rect(), 1, state.renderer.TitleBarHeight(),
        28, &measuring, state.renderer.EffectiveSidebarWidth(rect().right));
    H disabled;
    disabled.control_bounds = disabled_layout.dup_scan;
    point = center(disabled);
    hit = state.renderer.HitTest(vm_scanning, rect(), static_cast<float>(point.x), static_cast<float>(point.y));
    check(hit.region == H::SettingsDupScan, "disabled pointer check reaches the actual scan hit target");
    down(point);
    check(state.settings_input.pressed.region == H::None, "disabled scan control cannot arm a press");
    up(point);
    state.duplicateScan.scanning = false;

    app::DuplicateGroup duplicate;
    duplicate.id = 1; duplicate.size = 100;
    duplicate.files = {{(output / L"first.txt").wstring(), L"first.txt", 100, 0},
                       {(output / L"second.txt").wstring(), L"second.txt", 100, 0}};
    state.duplicateScan.groups = {duplicate}; ++state.duplicateScan.result_epoch;
    auto keep = visible(H::SettingsDupKeep, 0, 1);
    point = center(keep);
    down(point);
    check(!state.settings_input.pressed.path.empty(), "dynamic press stores the duplicate file identity");
    state.duplicateScan.groups[0].files[1].path = (output / L"replacement.txt").wstring();
    ++state.duplicateScan.result_epoch;
    up(point);
    check(state.duplicateScan.groups[0].keep_index == 0 && state.settings_input.pressed.region == H::None,
        "a replacement entity at the same dynamic index cannot receive the previous release");

    page(6);
    auto base = BuildVm(state, false);
    base.dup_groups = {{L"Fixture duplicates", {
        {L"first.txt", duplicate.files[0].path, L"", true},
        {L"second.txt", duplicate.files[1].path, L"", false}}}};
    base.settings_index_service = true;
    base.settings_index_volumes = {{L"fixture-volume", L"Fixture drive", L"", L"",
        ui::fluent::BadgeKind::Neutral, true, true, false, 0}};
    base.settings_index_excluded_paths = {(output / L"excluded").wstring()};
    base.settings_network_roots = {{L"\\\\fixture-server\\share", L"", L"", true, false}};
    auto sysctx = std::make_shared<ui::SysCtxView>();
    sysctx->state = ui::SysCtxView::State::Ready;
    sysctx->scenes.push_back({L"Fixture scene"});
    ui::SysCtxRowView row;
    row.title = L"Fixture command"; row.registry = L"HKCU\\Fixture\\command"; row.pulse = 1;
    sysctx->rows = {row}; sysctx->selected = 0; sysctx->side_tab = 1;
    base.settings_sysctx = sysctx;
    for (float scale : {1.0f, 1.5f}) for (float width : {620.0f, 1600.0f}) {
        state.renderer.SetScale(scale); measuring.SetScale(scale);
        state.compositor.RecreateTextFormats(scale); state.renderer.InvalidateTypography();
        const auto bounds = D2D1::RectF(0, 0, width * scale, 900 * scale);
        for (int value = 0; value < 10; ++value) {
            auto vm = base; vm.settings_page = value; vm.settings_scroll = 0;
            const auto enumerated = state.renderer.SettingsControls(vm, bounds);
            const auto content = state.renderer.SettingsContentBounds(vm, bounds);
            check(enumerated.size() > 9, "each settings page exposes navigation and real body controls");
            size_t nav_count = 0;
            for (const auto& target : enumerated) {
                const auto& r = target.control_bounds;
                check(std::isfinite(r.left) && std::isfinite(r.top) && r.right > r.left && r.bottom > r.top,
                    "all enumerated focus bounds are finite and nonempty");
                if (target.region == H::SettingsNav) { ++nav_count; continue; }
                check(r.left >= content.left - 0.5f && r.right <= content.right + 0.5f,
                    "body focus bounds stay inside the narrow and wide content columns");
            }
            check(nav_count == 9, "navigation enumerates the nine visible destinations without the hidden system-menu alias");
        }
    }
    state.renderer.SetScale(1); state.compositor.RecreateTextFormats(1); state.renderer.InvalidateTypography();
    state.compositor.Resize(1100, 900);
    auto index_vm = base; index_vm.settings_page = 1; index_vm.settings_index_migrating = true;
    index_vm.settings_index_volumes[0].pending = true;
    const auto index_controls = state.renderer.SettingsControls(index_vm, rect());
    check(find(index_controls, H::SettingsIndexAction, 0).region == H::None &&
        find(index_controls, H::SettingsIndexAction, 1).region == H::SettingsIndexAction &&
        find(index_controls, H::SettingsIndexVolume, 0).region == H::None,
        "migration and pending volume states filter disabled index controls");
    auto duplicate_vm = base; duplicate_vm.settings_page = 4; duplicate_vm.settings_scroll = 0;
    duplicate_vm.dup_scanning = false;
    duplicate_vm.dup_groups.resize(4, base.dup_groups.front());
    const auto duplicate_controls = state.renderer.SettingsControls(duplicate_vm, rect());
    const auto lower_keep = find(duplicate_controls, H::SettingsDupKeep, 3, 1);
    check(lower_keep.region == H::SettingsDupKeep && lower_keep.control_bounds.top > rect().bottom,
        "duplicate groups retain real file targets beyond layout's visible-row culling");
    auto system_vm = base; system_vm.settings_page = 8;
    system_vm.settings_storage_pending = system_vm.settings_system_pending = system_vm.settings_transfer_busy = true;
    const auto system_controls = state.renderer.SettingsControls(system_vm, rect());
    check(find(system_controls, H::SettingsToggle, 21).region == H::None &&
        find(system_controls, H::SettingsConfigurationAction, 1).region == H::None &&
        find(system_controls, H::SettingsDiagnosticsAction, 3).region == H::None,
        "busy integration, storage and transfer targets are absent from keyboard traversal");
    auto sys_vm = base; sys_vm.settings_page = 5;
    sysctx->state = ui::SysCtxView::State::Loading;
    const auto loading_controls = state.renderer.SettingsControls(sys_vm, rect());
    check(find(loading_controls, H::SettingsSysCtx, ui::kSysCtxRowBase).region == H::None &&
        find(loading_controls, H::SettingsSysCtx, ui::kSysCtxRefresh).region == H::None &&
        find(loading_controls, H::SettingsSysCtx, ui::kSysCtxLocalLayer).region == H::SettingsSysCtx,
        "loading system menu retains navigation while excluding mutating controls");

    {
        ui::HighContrastFrameScope contrast(true);
        auto high = ui::MakeHighContrastTheme();
        high.bg = high.surface_title = high.surface_sheet = high.surface_card = high.surface_flyout =
            high.surface_pane_inactive = high.fill_input = high.fill_input_hover = high.fill_input_focus =
            high.fill_input_disabled = high.header_bg = high.tab_bg = high.tab_active_bg = high.address_bg =
            high.status_bg = ui::HexColor(0xFFFFFF);
        high.fill_selected = high.fill_hover = high.fill_pressed = high.text = high.text_secondary =
            high.accent = high.accent_ink = ui::HexColor(0x000000);
        high.text_selected = high.accent_text = ui::HexColor(0xFFFF00);
        for (int width : {620, 1100}) for (int appearance = 0; appearance < 4; ++appearance) {
            state.compositor.Resize(static_cast<UINT>(width), 900);
            auto vm = base;
            vm.settings_page = 6; vm.settings_scroll = 0; vm.dark = false;
            vm.settings_focus_region = vm.settings_pressed_region = H::None;
            vm.settings_open = appearance == 0;
            vm.pane.is_home = appearance == 1;
            vm.pane.path = appearance == 2 ? L"C:\\FixtureSelected" : L"C:\\FixtureOther";
            ui::SidebarItem entry;
            entry.label = L"Fixture path"; entry.path = L"C:\\FixtureSelected";
            entry.icon_glyph = L"\xE8B7";
            ui::SidebarGroup group; group.items.push_back(std::move(entry));
            vm.sidebar = {std::move(group)};
            vm.hover_region = appearance == 0 ? H::SettingsNav : appearance == 1 ? H::SettingsButton
                : appearance == 2 ? H::HomeButton : H::SidebarItem;
            vm.hover_control_index = 0;
            if (appearance == 0) {
                vm.settings_focus_region = H::SettingsNav;
                vm.settings_focus_index = 6;
                vm.settings_focus_sub_index = -1;
                vm.settings_focus_key.clear();
            }
            state.compositor.Dc()->BeginDraw();
            state.renderer.Render(vm, rect(), high);
            check(SUCCEEDED(state.compositor.Dc()->EndDraw()), "production callers render with simulated high contrast colors");
            check(bench::SaveSnapshot(state.compositor, output / (L"hc-callers-" + std::to_wstring(width) +
                L"-" + std::to_wstring(appearance) + L".png")),
                "capture selected and hovered settings navigation, Home, Settings and path rows");
        }
        auto contrast_system = std::make_shared<ui::SysCtxView>(*sysctx);
        contrast_system->state = ui::SysCtxView::State::Ready;
        contrast_system->scene_title = L"Fixture scene";
        contrast_system->scene_keys = L"HKCU\\Fixture";
        contrast_system->filter = ui::kSysCtxFilterAll;
        contrast_system->filter_counts.fill(2);
        contrast_system->selected = 0;
        contrast_system->rows[0].selected = true;
        auto hovered_row = contrast_system->rows[0];
        hovered_row.title = L"Hovered fixture command";
        hovered_row.registry = L"HKCU\\Fixture\\hovered";
        hovered_row.selected = false;
        contrast_system->rows.push_back(std::move(hovered_row));
        contrast_system->side_tab = 0;
        ui::SysCtxPreviewItem preview;
        preview.title = L"Fixture preview command";
        preview.selected = preview.draggable = preview.movable = true;
        contrast_system->preview = {preview};
        for (int width : {620, 1100, 1600}) for (int value : {2, 5}) {
            state.compositor.Resize(static_cast<UINT>(width), 900);
            auto vm = base;
            vm.settings_open = true; vm.settings_page = value;
            vm.settings_scroll = 0; vm.dark = false;
            vm.settings_focus_region = vm.settings_pressed_region = H::None;
            vm.settings_context_tab = 0;
            vm.settings_sysctx = contrast_system;
            vm.hover_region = value == 2 ? H::SettingsToggle : H::SettingsSysCtx;
            vm.hover_control_index = value == 2 ? 60 : ui::kSysCtxSelectBase + 1;
            state.compositor.Dc()->BeginDraw();
            state.renderer.Render(vm, rect(), high);
            check(SUCCEEDED(state.compositor.Dc()->EndDraw()),
                "context and system menu production callers render simulated high contrast selection and hover");
            check(bench::SaveSnapshot(state.compositor, output / (L"hc-menu-" + std::to_wstring(value) +
                L"-" + std::to_wstring(width) + L".png")),
                "capture context and system menu callers with isolated in-memory view data");
        }
    }

    CancelSettingsPointer(state);
    state.settings.ResetUi();
    state.renderer.SetCompositor(nullptr); state.compositor.Shutdown();
    DestroyWindow(state.hwnd); state.hwnd = nullptr;
    owned.reset(); OleUninitialize();
    std::cout << "[SUMMARY] " << checks - failures << " PASS / " << failures << " FAIL\n";
    return failures ? 1 : 0;
}
