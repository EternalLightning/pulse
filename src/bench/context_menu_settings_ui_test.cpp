// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "../app/app_internal.h"
#include "../common/user_storage.h"
#include "../common/windows_compat.h"
#include "../ui/ui_renderer_internal.h"
#include "snapshot_output.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <memory>
#include <string_view>

namespace {
using namespace pulse;
using B = model::BuiltinMenuItem;
using S = model::BuiltinMenuSurface;
using H = ui::HitTestResult;
int checks = 0, failures = 0;
bool screenshots_only = false;

void Check(bool pass, const char* label) {
    ++checks;
    failures += !pass;
    std::printf("[%s] %s\n", pass ? "PASS" : "FAIL", label);
}
bool HasArea(const D2D1_RECT_F& r) { return r.right > r.left && r.bottom > r.top; }
D2D1_POINT_2F Center(const D2D1_RECT_F& r) {
    return D2D1::Point2F((r.left + r.right) * 0.5f, (r.top + r.bottom) * 0.5f);
}
bool Within(const D2D1_RECT_F& r, const D2D1_RECT_F& bounds) {
    return HasArea(r) && r.left >= bounds.left - 1 && r.right <= bounds.right + 1 &&
        r.top >= bounds.top - 1 && r.bottom <= bounds.bottom + 1;
}
bool ShellUnchanged(const app::ContextMenuPrefs& a, const app::ContextMenuPrefs& b) {
    if (a.software != b.software || a.share != b.share || a.wallpaper != b.wallpaper ||
        a.rotate != b.rotate || a.shortcut != b.shortcut || a.open_with != b.open_with ||
        a.open_with_com != b.open_with_com || a.system_extra != b.system_extra || a.print != b.print ||
        a.explorer_cap != b.explorer_cap || a.open_with_mru != b.open_with_mru ||
        a.item_enabled != b.item_enabled || a.disabled_com != b.disabled_com || a.seen.size() != b.seen.size()) return false;
    for (size_t i = 0; i < a.seen.size(); ++i) {
        const auto& x = a.seen[i]; const auto& y = b.seen[i];
        if (x.key != y.key || x.text != y.text || x.flyout != y.flyout ||
            x.from_com != y.from_com || x.category != y.category) return false;
    }
    return true;
}

struct Fixture {
    std::unique_ptr<AppState> owned = std::make_unique<AppState>();
    std::unique_ptr<ui::fluent::Painter> painter;
    std::filesystem::path output;
    D2D1_RECT_F window{};
    int effects = 0;
    AppState& State() { return *owned; }
    bool Init(const std::filesystem::path& directory) {
        output = directory;
        auto& s = State();
        s.isolatedTest = true;
        s.accentColor = ui::HexColor(0x0078D4);
        s.appPrefs.persist = s.places.persist = s.ctxMenuPrefs.persist = s.searchHistory.persist = false;
        s.hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"STATIC", L"isolated context menu settings",
            WS_POPUP, -30000, -30000, 1100, 900, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!s.hwnd || !s.compositor.Init(s.hwnd)) return false;
        s.renderer.SetCompositor(&s.compositor);
        s.renderer.SetDetailsPanelVisible(false);
        s.core.windows.Add(s.hwnd, &s);
        s.core.global_ui_owner = s.hwnd;
        s.window_tabs.NewTab(app::MakeSettingsPath(L"context"));
        s.pane = s.window_tabs.Active()->FocusedPane();
        ActiveTab(s)->loading = false;
        s.settings.SelectPage(2);
        app::SettingsController::UiCallbacks callbacks;
        callbacks.apply_effects = [&](app::SettingsEffect) { ++effects; };
        s.settings.BindUi(s.appPrefs, s.ctxMenuPrefs, s.index, s.networkIndex, std::move(callbacks));
        s.ctxMenuPrefs.RecordSeen(L"fixture:software", L"Isolated software command", false, ipc::CtxMenuCategory::Software);
        s.ctxMenuPrefs.SetItemEnabled(L"fixture:software", false);
        s.ctxMenuPrefs.SetComDisabled(ipc::HandlerCatalogKey(L"{01234567-89AB-CDEF-0123-456789ABCDEF}"), true);
        s.ctxMenuPrefs.share = true;
        s.ctxMenuPrefs.system_extra = true;
        s.ctxMenuPrefs.explorer_cap = 43;
        s.ctxMenuPrefs.open_with_mru = 4;
        painter = std::make_unique<ui::fluent::Painter>(&s.compositor);
        Resize(1100, 1.0f, false);
        return true;
    }
    ~Fixture() {
        auto& s = State();
        s.settings.EndMenuDrag();
        s.settings.ResetUi();
        if (s.hwnd) {
            s.core.windows.Remove(s.hwnd);
            s.renderer.SetCompositor(nullptr);
            s.compositor.Shutdown();
            DestroyWindow(s.hwnd);
            s.hwnd = nullptr;
        }
    }
    void Resize(int width, float scale, bool dark) {
        auto& s = State();
        s.scale = scale; s.darkMode = dark;
        s.renderer.SetScale(scale);
        s.compositor.RecreateTextFormats(scale);
        painter->SetScale(scale);
        painter->BeginFrame(ui::MakeTheme(dark, s.accentColor));
        window = D2D1::RectF(0, 0, width * scale, 900 * scale);
        SetWindowPos(s.hwnd, nullptr, -30000, -30000, static_cast<int>(window.right),
            static_cast<int>(window.bottom), SWP_NOZORDER | SWP_NOACTIVATE);
        s.compositor.Resize(static_cast<UINT>(window.right), static_cast<UINT>(window.bottom));
        s.settings.SetScroll(0, 0);
    }
    ui::WindowViewModel Vm() { return BuildVm(State(), false); }
    ui::SettingsLayout Layout(const ui::WindowViewModel& vm) {
        auto& s = State();
        return ui::MakeSettingsLayout(vm, window, s.scale, s.renderer.TitleBarHeight(), 28 * s.scale, painter.get());
    }
    ui::SettingsLayout Reach(ui::WindowViewModel& vm,
        const std::function<D2D1_RECT_F(const ui::SettingsLayout&)>& choose) {
        vm.settings_scroll = 0;
        auto layout = Layout(vm);
        const auto r = choose(layout);
        const float maximum = State().renderer.SettingsMaxScroll(vm, window.right, window.bottom);
        vm.settings_scroll = std::clamp(Center(r).y - Center(layout.content).y, 0.0f, maximum);
        return Layout(vm);
    }
    H Hit(const ui::WindowViewModel& vm, const D2D1_RECT_F& r) {
        const auto p = Center(r);
        return State().renderer.HitTest(vm, window, p.x, p.y);
    }
    void HitCheck(const ui::WindowViewModel& vm, const D2D1_RECT_F& r, H::Region region, int index, const char* text) {
        const auto hit = Hit(vm, r);
        Check(HasArea(r) && hit.region == region && hit.index == index, text);
    }
    void Render(const ui::WindowViewModel& vm, const std::wstring& file = {}) {
        auto& s = State();
        s.compositor.Dc()->BeginDraw();
        s.renderer.Render(vm, window, ui::MakeTheme(s.darkMode, s.accentColor));
        Check(SUCCEEDED(s.compositor.Dc()->EndDraw()), "production context settings renderer completes");
        if (!file.empty()) Check(bench::SaveSnapshot(s.compositor, output / file), "save production context settings screenshot");
    }
};

void BehaviorChecks(Fixture& f) {
    auto& s = f.State();
    const auto shell_before = s.ctxMenuPrefs;
    auto vm = f.Vm();
    Check(vm.settings_open && vm.settings_page == 2 && vm.settings_builtin_first == s.ctxMenuPrefs.seen.size() &&
        vm.settings_items.size() == vm.settings_builtin_first + model::kBuiltinMenuItemCount,
        "real BuildVm includes the isolated catalog followed by every built-in command");
    for (int preset = 0; preset < 3; ++preset) {
        s.settings.ToggleUi(60 + preset);
        vm = f.Vm();
        Check(vm.settings_builtin_hidden == model::BuiltinMenuPresetHidden(static_cast<model::BuiltinMenuPreset>(preset)) &&
            model::BuiltinMenuPresetFor(vm.settings_builtin_hidden) == static_cast<model::BuiltinMenuPreset>(preset),
            "preset controller action updates the real view model and active preset");
    }
    s.settings.ToggleUi(67);
    const int copy_hit = 100 + static_cast<int>(s.ctxMenuPrefs.seen.size()) + static_cast<int>(B::CopyPath);
    s.settings.ToggleUi(copy_hit);
    for (int tab = 0; tab < 2; ++tab) {
        s.settings.ToggleUi(64 + tab);
        vm = f.Vm();
        Check(vm.settings_context_tab == tab && !vm.settings_items[vm.settings_builtin_first + static_cast<size_t>(B::CopyPath)].on &&
            model::BuiltinMenuShared(B::CopyPath), "one shared command switch is reflected on both real surfaces");
    }
    s.settings.ToggleUi(copy_hit);
    Check(s.ctxMenuPrefs.BuiltinVisible(B::CopyPath), "shared command switch can be re-enabled");
    for (int tab = 0; tab < 3; ++tab) {
        s.settings.ToggleUi(64 + tab);
        auto order = model::BuiltinMenuDefaultOrder(static_cast<S>(tab));
        std::reverse(order.begin(), order.end());
        s.settings.SetMenuOrder(order);
        Check(s.ctxMenuPrefs.BuiltinOrder(static_cast<S>(tab)) == order, "controller persists each surface's independent order");
    }
    s.settings.ToggleUi(65);
    s.settings.ToggleUi(68);
    Check(!s.ctxMenuPrefs.BuiltinOrderCustom(S::Background) && s.ctxMenuPrefs.BuiltinOrderCustom(S::Item) &&
        s.ctxMenuPrefs.BuiltinOrderCustom(S::RowButtons), "restore order affects only the selected surface");
    s.settings.ToggleUi(60);
    s.settings.ToggleUi(67);
    Check(s.ctxMenuPrefs.builtin_hidden == 0 && !s.ctxMenuPrefs.BuiltinOrderCustom(S::Item) &&
        !s.ctxMenuPrefs.BuiltinOrderCustom(S::Background) && !s.ctxMenuPrefs.BuiltinOrderCustom(S::RowButtons),
        "card restore defaults restores all visibility and all three orders");

    for (int tab = 0; tab < 3; ++tab) {
        s.settings.ToggleUi(64 + tab);
        vm = f.Vm();
        auto layout = f.Reach(vm, [](const auto& l) { return l.pulse_preview; });
        s.settings.SetScroll(vm.settings_scroll, s.renderer.SettingsMaxScroll(vm, f.window.right, f.window.bottom));
        vm = f.Vm();
        layout = f.Layout(vm);
        const auto preview = ui::LayoutPulsePreview(vm, ui::PulsePreviewBox(layout.pulse_preview, s.scale), s.scale);
        const auto first = std::find_if(preview.rows.begin(), preview.rows.end(), [](const auto& row) {
            return row.row.fixed == model::BuiltinFixedRow::None;
        });
        if (first == preview.rows.end() || first + 1 == preview.rows.end()) {
            Check(false, "each surface offers two movable real preview rows");
            continue;
        }
        const auto source = Center(first->rect), next = Center((first + 1)->rect);
        const bool horizontal = tab == 2;
        const float start = horizontal ? source.x : source.y;
        const float grab = horizontal ? source.x - first->rect.left : source.y - first->rect.top;
        const int item = static_cast<int>(first->row.item);
        const auto original = s.ctxMenuPrefs.BuiltinOrder(static_cast<S>(tab));
        s.settings.BeginMenuDrag(item, start, grab);
        Check(!s.settings.MenuDragMove(start + 2), "small pointer motion does not start a menu drag");
        const float destination = (horizontal ? next.x : next.y) + 8 * s.scale;
        Check(s.settings.MenuDragMove(destination), "pointer movement starts a real controller drag");
        auto dragging = f.Vm();
        Check(dragging.settings_menu_drag == item, "real BuildVm publishes live drag state");
        const auto drop = s.renderer.SettingsMenuDropOrder(dragging, f.window,
            horizontal ? destination : source.x, horizontal ? source.y : destination);
        Check(!drop.empty() && drop != original, "production renderer computes a changed drop order");
        Check(s.renderer.SettingsMenuDropOrder(dragging, f.window, -100, -100).empty(),
            "a drop outside the production preview is rejected");
        s.settings.EndMenuDrag();
        Check(s.ctxMenuPrefs.BuiltinOrder(static_cast<S>(tab)) == original && f.Vm().settings_menu_drag == -1,
            "cancelling a live drag clears its view model without changing saved order");
        s.settings.SetMenuOrder(drop);
        Check(s.ctxMenuPrefs.BuiltinOrder(static_cast<S>(tab)) == drop, "accepted renderer drop order reaches production preferences");
        s.settings.BeginMenuDrag(item, start, grab);
        s.settings.MenuDragMove(start + 12);
        s.settings.ToggleUi(64 + (tab + 1) % 3);
        Check(s.settings.menu_drag_item() == -1 && !s.settings.menu_drag_live(), "switching surface cancels a captured menu drag");
        s.settings.ToggleUi(64 + tab);
        s.settings.ToggleUi(68);
    }
    Check(ShellUnchanged(shell_before, s.ctxMenuPrefs), "Pulse presets, toggles, reset and sorting preserve all local Shell settings");
    Check(f.effects > 0 && !s.settings.SystemCtxLoaded(), "Pulse actions notify the bound UI without starting the registry manager");
    s.settings.ToggleUi(67);

    const size_t tabs_before = s.window_tabs.items.size();
    auto* settings_tab = ActiveTab(s);
    H system_layer;
    system_layer.region = H::SettingsCtxPivot;
    system_layer.index = 5;
    Check(HandleSettingsControl(s, system_layer) && s.window_tabs.items.size() == tabs_before &&
              ActiveTab(s) == settings_tab && settings_tab->current_path == app::MakeSettingsPath(L"system-menu"),
          "system layer action reuses the existing settings tab and compatibility route");
    OpenSettingsTab(s, 2);
    Check(s.window_tabs.items.size() == tabs_before && ActiveTab(s) == settings_tab &&
              settings_tab->current_path == app::MakeSettingsPath(L"context") && !s.settings.SystemCtxLoaded(),
          "returning to the Pulse layer reuses the settings tab without scanning the registry");
}

void SidebarChecks(Fixture& f, const ui::WindowViewModel& vm) {
    const auto layout = f.Layout(vm);
    Check(ui::kSettingsNavCount == 5 && ui::SettingsSidebarPage(vm.settings_page) == 2,
        "both context menu layers share one active sidebar entry");
    Check(l10n::Get(l10n::StringId::SettingsContextMenu) == l10n::Pick(L"右键菜单", L"Context menu"),
        "the shared context menu heading and sidebar label use the unified localized name");
    for (int page = 0; page < ui::kSettingsNavCount; ++page)
        f.HitCheck(vm, layout.nav_row[page], H::SettingsNav, page,
            "each remaining settings sidebar entry retains its production navigation route");
}

void LayerCardChecks(Fixture& f, const D2D1_RECT_F& local, const D2D1_RECT_F& system,
                     float controls_top, bool system_current) {
    auto& s = f.State();
    const float scale = s.scale;
    const bool narrow = f.window.right / scale == 760;
    Check(narrow ? system.top >= local.bottom + 9 * scale && std::abs(local.left - system.left) <= 1 &&
        std::abs(local.right - system.right) <= 1 : std::abs(local.top - system.top) <= 1 &&
        system.left >= local.right + 9 * scale,
        "layer cards stack at 760 DIP and remain separate side by side at 1100 DIP");
    Check(controls_top >= std::max(local.bottom, system.bottom) + 11 * scale,
        "the following settings controls begin below both complete layer cards");
    ui::ComPtr<IDWriteTextFormat> title_format;
    Check(SUCCEEDED(ui::typography::CreateTextFormat(s.compositor.DwriteFactory(),
        {ui::typography::FontRole::Text, 13.0f * scale, DWRITE_FONT_WEIGHT_SEMI_BOLD}, &title_format)),
        "create the production layer-card title typography for text-fit checks");
    if (!title_format.get()) return;
    const std::array<D2D1_RECT_F, 2> cards{local, system};
    const wchar_t* titles[] = {ui::sysctx_text::LocalTitle(), ui::sysctx_text::SystemTitle()};
    const wchar_t* captions[] = {ui::sysctx_text::LocalCaption(), ui::sysctx_text::SystemCaption()};
    for (size_t i = 0; i < cards.size(); ++i) {
        const auto content = ui::LayoutCtxLayerCard(cards[i], captions[i], f.painter.get(), scale);
        const wchar_t* tag = (i == 1) == system_current ? ui::sysctx_text::Current() : ui::sysctx_text::LocalTag();
        Check(Within(content.icon, cards[i]) && Within(content.title, cards[i]) &&
            Within(content.caption, cards[i]) && Within(content.tag, cards[i]),
            "layer-card icon, full title, description and state label stay inside the card");
        Check(content.icon.right + 11 * scale <= content.title.left &&
            content.title.right + 7 * scale <= content.tag.left && content.caption.top >= content.title.bottom + scale &&
            content.caption.left >= content.icon.right + 11 * scale,
            "layer-card title, wrapped description, icon and state label do not overlap");
        Check(ui::typography::MeasureLine(&s.compositor, title_format.get(), titles[i]) <=
            content.title.right - content.title.left + 1,
            "each complete localized layer-card title fits without ellipsis");
        Check(ui::typography::MeasureLine(&s.compositor, s.compositor.SmallFormat(), tag) <=
            content.tag.right - content.tag.left + 1,
            "the current or switch label fits its reserved title-row space");
        Check(f.painter->MeasureWrappedCaptionHeight(captions[i], content.caption.right - content.caption.left) <=
            content.caption.bottom - content.caption.top + 1 && content.caption.bottom + 11 * scale <= cards[i].bottom,
            "the complete localized description fits with bottom padding after wrapping");
    }
}

void GeometryChecks(Fixture& f, ui::WindowViewModel vm) {
    auto& s = f.State();
    const float scale = s.scale;
    auto layout = f.Layout(vm);
    SidebarChecks(f, vm);
    LayerCardChecks(f, layout.ctx_layer[0], layout.ctx_layer[1], layout.pulse_card.top, false);
    Check(HasArea(layout.pulse_preview) && layout.pulse_preview.left >= layout.content.left &&
        layout.pulse_preview.right <= layout.content.right - 16 * scale,
        "live preview is retained within the settings width at both DPI scales");
    if (f.window.right / scale == 760) {
        float switches_bottom = 0;
        for (const auto& row : layout.context_rows)
            if (HasArea(row)) switches_bottom = std::max(switches_bottom, row.bottom);
        for (const auto& row : layout.pulse_fixed)
            if (HasArea(row)) switches_bottom = std::max(switches_bottom, row.bottom);
        Check(switches_bottom > 0 && layout.pulse_preview.top >= switches_bottom,
            "narrow settings stacks the live preview beneath every command switch and fixed row");
    }
    for (int i = 0; i < 4; ++i) {
        auto shown = vm;
        const auto visible = f.Reach(shown, [i](const auto& l) { return l.pulse_preset[i]; });
        const auto tile = visible.pulse_preset[i];
        Check(Within(tile, visible.content), "every preset is scrollable inside the visible content");
        if (i < 3) f.HitCheck(shown, tile, H::SettingsToggle, 60 + i, "production preset hit matches its controller action");
        else Check(f.Hit(shown, tile).region != H::SettingsToggle, "custom preset remains a display-only state");
        static constexpr l10n::StringId descriptions[] = {l10n::StringId::ContextPresetSlimDesc,
            l10n::StringId::ContextPresetStandardDesc, l10n::StringId::ContextPresetFullDesc, l10n::StringId::ContextPresetCustomDesc};
        Check(f.painter->MeasureWrappedCaptionHeight(l10n::Get(descriptions[i]), tile.right - tile.left - 20 * scale) <=
            tile.bottom - tile.top - 29 * scale + 1, "localized preset caption fits its wrapped tile");
    }
    for (int tab = 0; tab < 3; ++tab) {
        auto shown = vm;
        const auto visible = f.Reach(shown, [tab](const auto& l) { return l.pulse_tab[tab]; });
        f.HitCheck(shown, visible.pulse_tab[tab], H::SettingsToggle, 64 + tab, "all surface tabs remain independently reachable");
    }
    const auto rows = model::BuiltinMenuOrderedRows(ui::PulseMenuSurface(vm), ui::PulseMenuOrder(vm));
    for (const auto& row : rows) {
        auto shown = vm;
        const auto choose = [&](const ui::SettingsLayout& l) {
            return row.fixed != model::BuiltinFixedRow::None ? l.pulse_fixed[row.fixed == model::BuiltinFixedRow::Open ? 0 : 1]
                : l.context_rows[vm.settings_builtin_first + static_cast<size_t>(row.item)];
        };
        const auto visible = f.Reach(shown, choose);
        const auto r = choose(visible);
        Check(Within(r, visible.content), "every fixed or switch row is vertically reachable without clipping");
        if (row.fixed == model::BuiltinFixedRow::None)
            f.HitCheck(shown, r, H::SettingsToggle, 100 + static_cast<int>(vm.settings_builtin_first) + static_cast<int>(row.item),
                "command switch hit follows the real catalog offset");
        else Check(f.Hit(shown, r).region != H::SettingsToggle, "fixed command rows expose no visibility toggle");
    }
    const auto preview = ui::LayoutPulsePreview(vm, ui::PulsePreviewBox(layout.pulse_preview, scale), scale);
    for (size_t i = 0; i < preview.rows.size(); ++i) {
        auto shown = vm;
        const auto choose = [&](const ui::SettingsLayout& l) {
            return ui::LayoutPulsePreview(shown, ui::PulsePreviewBox(l.pulse_preview, scale), scale).rows[i].rect;
        };
        const auto visible = f.Reach(shown, choose);
        const auto r = choose(visible);
        Check(Within(r, visible.content) && Within(r, ui::PulsePreviewBox(visible.pulse_preview, scale)),
            "each preview row fits horizontally and can be scrolled completely into view");
        f.HitCheck(shown, r, H::SettingsToggle, ui::PulseMenuRowHit(preview.rows[i].row),
            "production preview hit preserves fixed locks and draggable command identities");
    }
    auto shown = vm;
    const auto visible = f.Reach(shown, [](const auto& l) { return l.ctx_layer[1]; });
    Check(Within(visible.ctx_layer[1], visible.content), "system layer card is reachable within the shared context menu entry");
    f.HitCheck(shown, visible.ctx_layer[1], H::SettingsCtxPivot, 5,
        "system layer card switches to the preserved system manager route");
}

std::shared_ptr<ui::SysCtxView> SystemView(int state) {
    auto view = std::make_shared<ui::SysCtxView>();
    view->state = state == 0 ? ui::SysCtxView::State::Loading : ui::SysCtxView::State::Ready;
    view->scene_title = l10n::Pick(L"文件夹", L"Folders");
    ui::SysCtxSceneView scene;
    scene.label = view->scene_title;
    scene.glyph = L"\xE838";
    view->scenes.push_back(scene);
    if (state == 2) {
        view->message = l10n::Pick(L"隔离测试：上次更改未能保存，请重试。", L"Isolated fixture: the last changes could not be saved. Retry.");
        view->message_error = true;
    } else if (state == 3) {
        ui::SysCtxRowView row;
        row.title = l10n::Pick(L"示例软件命令", L"Example app command");
        row.subtitle = l10n::Pick(L"隔离展示数据", L"Isolated display data");
        row.kind_label = l10n::Pick(L"静态动词", L"Static verb");
        row.registry = L"HKEY_CURRENT_USER\\Software\\Classes\\Directory\\shell\\Fixture";
        row.method = l10n::Pick(L"仅供显示，不访问注册表。", L"Display data only. No registry access.");
        row.pulse = 1;
        row.selected = true;
        view->rows.push_back(row);
        view->total = 1;
        view->selected = 0;
    }
    return view;
}

void SystemChecks(Fixture& f, const ui::WindowViewModel& base, const std::wstring& prefix, bool save_all) {
    static constexpr const wchar_t* names[] = {L"loading", L"empty", L"error", L"ready"};
    for (int state = 0; state < 4; ++state) {
        auto vm = base;
        vm.settings_page = 5;
        vm.settings_scroll = 0;
        vm.settings_sysctx = SystemView(state);
        auto layout = f.Layout(vm);
        if (!screenshots_only) {
            if (state == 0) {
                SidebarChecks(f, vm);
                LayerCardChecks(f, layout.sysctx.local_card, layout.sysctx.system_card, layout.sysctx.list_card.top, true);
            }
            Check(HasArea(layout.sysctx.empty) == (state != 3), "system loading, empty and error views reserve an explicit empty-state area");
            if (state == 2) Check(HasArea(layout.sysctx.notice), "system error is visible in the production notice layout");
            auto shown = vm;
            auto visible = f.Reach(shown, [](const auto& l) { return l.sysctx.local_card; });
            Check(Within(visible.sysctx.local_card, visible.content), "Pulse layer card remains reachable from the system manager");
            f.HitCheck(shown, visible.sysctx.local_card, H::SettingsSysCtx, ui::kSysCtxLocalLayer,
                "system page layer control returns to the Pulse menu");
            shown = vm;
            visible = f.Reach(shown, [](const auto& l) { return l.sysctx.refresh; });
            const auto refresh = f.Hit(shown, visible.sysctx.refresh);
            Check(state == 0 ? refresh.region != H::SettingsSysCtx || refresh.index != ui::kSysCtxRefresh
                : refresh.region == H::SettingsSysCtx && refresh.index == ui::kSysCtxRefresh,
                "loading blocks refresh while ready empty and error states keep retry available");
            if (state == 3) {
                shown = vm;
                visible = f.Reach(shown, [](const auto& l) { return l.sysctx.switches.front(); });
                f.HitCheck(shown, visible.sysctx.switches.front(), H::SettingsSysCtx, ui::kSysCtxRowBase,
                    "isolated system command switch retains its production hit route");
                shown = vm;
                visible = f.Reach(shown, [](const auto& l) { return l.sysctx.detail.pulse_toggle; });
                f.HitCheck(shown, visible.sysctx.detail.pulse_toggle, H::SettingsSysCtx, ui::kSysCtxDetailPulse,
                    "isolated system detail has an independent Pulse visibility switch");
            }
        }
        const bool capture = save_all || state == 2 || state == 3;
        f.Render(vm, capture ? prefix + L"-system-" + names[state] + L".png" : std::wstring{});
    }
}

void RowActionChecks(Fixture& f) {
    auto& s = f.State();
    auto* tab = ActiveTab(s);
    tab->current_path = f.output.wstring();
    tab->view_mode = ui::ViewMode::Details;
    tab->details_column_dividers = {48.0f, 48.0f, 48.0f};
    auto entries = std::make_shared<std::vector<fs::DirEntry>>(1);
    (*entries)[0].name = L"folder";
    (*entries)[0].full_path = (f.output / L"folder").wstring();
    (*entries)[0].is_dir = true;
    (*entries)[0].attrs = FILE_ATTRIBUTE_DIRECTORY;
    tab->SetSnapshot(entries);
    tab->loading = false;
    s.hoverRow = 0;
    s.hoverPaneIndex = 0;
    const std::array<model::BuiltinMenuOrder, 2> orders{{{}, {B::RowMore, B::RowStar, B::RowNewTab}}};
    for (const auto& order : orders) {
        s.ctxMenuPrefs.SetBuiltinOrder(S::RowButtons, order);
        for (const bool hide_star : {false, true}) {
            s.ctxMenuPrefs.SetBuiltinVisible(B::RowStar, !hide_star);
            auto vm = f.Vm();
            Check(!vm.settings_open && !vm.pane_slots.empty(), "file-row fixture uses the real production file pane view model");
            if (vm.pane_slots.empty()) continue;
            const auto& pane = vm.pane_slots.front();
            const auto name = s.renderer.NameCellRect(pane.rect, 0, 0, 0, ui::ViewMode::Details, 0, 1,
                pane.pane.details_column_dividers);
            const float y = Center(name).y;
            std::array<float, 3> positions{-1, -1, -1};
            for (float x = pane.rect.left; x < pane.rect.right; x += 2 * s.scale) {
                const auto hit = s.renderer.HitTest(vm, f.window, x, y);
                if (hit.index != 0) continue;
                const int which = hit.region == H::RowNewTab ? 0 : hit.region == H::RowStar ? 1 : hit.region == H::RowMore ? 2 : -1;
                if (which >= 0 && positions[static_cast<size_t>(which)] < 0) positions[static_cast<size_t>(which)] = x;
            }
            Check(positions[0] >= 0 && positions[2] >= 0 && (hide_star ? positions[1] < 0 : positions[1] >= 0),
                "real row hit testing preserves enabled actions and removes the hidden star");
            Check(order.empty() ? positions[0] < positions[2] && (hide_star || (positions[0] < positions[1] && positions[1] < positions[2]))
                : positions[2] < positions[0] && (hide_star || (positions[2] < positions[1] && positions[1] < positions[0])),
                "production row action hit positions follow the persisted left-to-right order");
        }
    }
    s.ctxMenuPrefs.SetBuiltinVisible(B::RowStar, true);
    s.ctxMenuPrefs.ResetBuiltinOrder();
    tab->current_path = app::MakeSettingsPath(L"context");
}
}

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    for (int i = 1; i < argc; ++i) {
        if (std::string_view(argv[i]) == "--screenshots-only") screenshots_only = true;
        else { std::printf("Unknown argument: %s\n", argv[i]); return 2; }
    }
    if (FAILED(OleInitialize(nullptr))) return 2;
    compat::EnableDpiAwareness();
    SetEnvironmentVariableW(L"PULSE_TEST_HIDDEN_SHOT", L"1");
    const auto output = std::filesystem::absolute(L"docs/validation/context-menu-port-20261025/screenshots");
    std::filesystem::create_directories(output);
    storage::OverrideDefaultRootForTesting((output.parent_path() / L"isolated-profile").wstring());
    l10n::Initialize(GetModuleHandleW(nullptr), L"zh-CN");
    int result = 2;
    {
        Fixture fixture;
        const bool initialized = fixture.Init(output);
        Check(initialized, "initialize isolated hidden production renderer and controller");
        if (initialized) {
            if (!screenshots_only) BehaviorChecks(fixture);
            for (const auto* language : {L"zh-CN", L"en-US"}) {
                l10n::SetLanguage(language);
                for (float scale : {1.0f, 1.5f}) for (bool dark : {false, true}) for (int width : {760, 1100}) {
                    fixture.Resize(width, scale, dark);
                    const auto prefix = std::wstring(language) + L"-" + std::to_wstring(width) + L"-" +
                        std::to_wstring(static_cast<int>(scale * 100)) + (dark ? L"-dark" : L"-light");
                    for (int tab = 0; tab < 3; ++tab) {
                        fixture.State().settings.ToggleUi(67);
                        fixture.State().settings.ToggleUi(64 + tab);
                        auto vm = fixture.Vm();
                        if (!screenshots_only) GeometryChecks(fixture, vm);
                        if (tab == 0) fixture.Render(vm, prefix + L"-pulse-controls.png");
                        fixture.Reach(vm, [](const auto& l) { return l.pulse_preview; });
                        fixture.Render(vm, prefix + L"-pulse-preview-" + std::to_wstring(tab) + L".png");
                    }
                    SystemChecks(fixture, fixture.Vm(), prefix, width == 1100 && scale == 1.0f);
                    if (!screenshots_only) RowActionChecks(fixture);
                }
            }
            Check(!fixture.State().settings.SystemCtxLoaded(), "all system display fixtures avoid real registry scanning and mutation");
            result = failures ? 1 : 0;
        }
    }
    OleUninitialize();
    std::printf("RESULT checks=%d failures=%d\n", checks, failures);
    return result;
}
