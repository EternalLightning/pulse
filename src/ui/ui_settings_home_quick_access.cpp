// SPDX-License-Identifier: Apache-2.0
#include "ui_renderer.h"
#include "ui_renderer_internal.h"

namespace pulse::ui {
void MainRenderer::DrawSettingsHomeQuickAccess(const WindowViewModel& vm, const D2D1_RECT_F& rect, const Theme& theme) {
    using H = HitTestResult;
    const auto layout = MakeSettingsLayout(vm, rect, scale_, title_bar_height_, status_height_, &painter_, EffectiveSidebarWidth(rect.right));
    const auto card = [&](D2D1_RECT_F bounds) {
        painter_.FillRoundedRect(bounds, 8 * scale_, vm.dark && !IsHighContrast() ? theme.fill_input : theme.surface_card);
        painter_.StrokeRoundedRect(bounds, 8 * scale_, theme.stroke_card);
    };
    const auto button = [&](D2D1_RECT_F bounds, const std::wstring& title, int action, int index = -1,
                            bool enabled = true, const wchar_t* glyph = L"") {
        fluent::ButtonSpec spec;
        spec.bounds = bounds; spec.text = title; spec.glyph = glyph;
        spec.state.enabled = enabled;
        spec.state.hovered = enabled && IsHovered(vm, H::SettingsHomeQuickAccess, action, index);
        DrawSettingsButton(vm, spec);
    };
    button(layout.home_access_add, l10n::Pick(L"添加文件夹…", L"Add folder…"), HomeAccessAddFolder);
    button(layout.home_access_reset, l10n::Get(l10n::StringId::RestoreDefaults), HomeAccessReset);
    const std::wstring sections[] = {l10n::Pick(L"库文件夹", L"Library folders"),
        l10n::Pick(L"自定义文件夹", L"Custom folders"), l10n::Pick(L"系统位置", L"System locations")};
    for (int i = 0; i < 3; ++i) painter_.DrawText(sections[i], layout.home_access_sections[i], compositor_->HeaderFormat(), theme.text_secondary);
    for (int bit = 0; bit < model::kHomeQuickAccessItemCount; ++bit) {
        const auto& row = layout.home_access_builtin[bit];
        if (!VisibleInContent(row, layout.content, 0)) continue;
        card(row);
        const bool hovered = IsHovered(vm, H::SettingsHomeQuickAccess, HomeAccessToggle, bit);
        if (hovered) painter_.FillRoundedRect(D2D1::RectF(row.left + 2 * scale_, row.top + 2 * scale_, row.right - 2 * scale_, row.bottom - 2 * scale_),
            6 * scale_, SettingsPressed(vm, row) ? theme.fill_pressed : theme.fill_hover);
        painter_.DrawText(HomeQuickAccessItemLabel(bit), D2D1::RectF(row.left + 16 * scale_, row.top + 16 * scale_,
            row.right - 80 * scale_, row.bottom - 16 * scale_), compositor_->TextFormat(), theme.text);
        const float center = (row.top + row.bottom) * 0.5f;
        fluent::ControlState state;
        state.checked = !(vm.settings_home_access_hidden & (1u << bit)); state.hovered = hovered;
        DrawSettingsSwitch(vm, L"home-access:" + std::to_wstring(bit), D2D1::RectF(row.right - 60 * scale_, center - 16 * scale_,
            row.right - 16 * scale_, center + 16 * scale_), state);
    }
    for (size_t i = 0; i < layout.home_access_folders.size() && i < vm.settings_home_access_folders.size(); ++i) {
        const auto& row = layout.home_access_folders[i];
        if (!VisibleInContent(row.card, layout.content, 0)) continue;
        const auto& folder = vm.settings_home_access_folders[i];
        card(row.card);
        const auto title = FitEndEllipsis(folder.title, row.title.right - row.title.left,
            [&](const std::wstring& value) { return CellTextWidth(value); });
        painter_.DrawText(title, row.title, compositor_->TextFormat(), theme.text);
        painter_.DrawWrappedCaption(folder.path, D2D1::Point2F(row.path.left, row.path.top), row.path.right - row.path.left, theme.text_secondary);
        button(row.up, L"", HomeAccessMoveUp, static_cast<int>(i), i > 0, L"\xE70E");
        button(row.down, L"", HomeAccessMoveDown, static_cast<int>(i), i + 1 < vm.settings_home_access_folders.size(), L"\xE70D");
        button(row.remove, L"", HomeAccessRemove, static_cast<int>(i), true, L"\xE74D");
    }
    if (vm.settings_home_access_folders.empty()) painter_.DrawText(l10n::Pick(L"尚未添加文件夹", L"No custom folders"),
        layout.home_access_empty, compositor_->SmallFormat(), theme.text_secondary);
}
} // namespace pulse::ui
