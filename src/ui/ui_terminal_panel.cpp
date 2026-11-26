// SPDX-License-Identifier: Apache-2.0
#include "ui_renderer.h"
#include "terminal_surface.h"
#include "ui_renderer_internal.h"
#include "../common/localization.h"

namespace pulse::ui {

D2D1_RECT_F MainRenderer::TerminalBodyRect(float w, float h) const {
    const auto panel = DetailsPanelRect(w, h);
    if (panel.right <= panel.left) return {};
    return D2D1::RectF(panel.left + 12 * scale_, panel.top + 48 * scale_,
                      panel.right - 12 * scale_, panel.bottom - 10 * scale_);
}

D2D1_RECT_F MainRenderer::TerminalCloseRect(float w, float h) const {
    const auto panel = DetailsPanelRect(w, h);
    if (panel.right <= panel.left) return {};
    return D2D1::RectF(panel.right - 39 * scale_, panel.top + 6 * scale_,
                      panel.right - 7 * scale_, panel.top + 38 * scale_);
}

void MainRenderer::DrawTerminalPanel(const WindowViewModel& vm, const D2D1_RECT_F& rect,
                                     const Theme& theme) {
    auto* dc = compositor_->Dc();
    const auto panel = DetailsPanelRect(rect.right, rect.bottom);
    const auto body = TerminalBodyRect(rect.right, rect.bottom);
    const float s = scale_;
    const float radius = theme.radius_control * s;
    const auto fill = UsesNativeMica(vm) ? NativeMicaPaneFill(vm) : vm.dark && !IsHighContrast()
        ? WithAlpha(HexColor(0xFFFFFF), vm.background_image.empty() ? 0.045f : 0.09f)
        : WithAlpha(theme.surface_card, card_alpha_);
    MakeBrush(dc, fill, brFillHover_);
    FillRoundedRect(dc, brFillHover_.get(), panel.left, panel.top,
        panel.right - panel.left, panel.bottom - panel.top, radius);
    MakeBrush(dc, theme.stroke_card, brStrokeCard_);
    dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(panel.left + s * 0.5f, panel.top + s * 0.5f,
        panel.right - s * 0.5f, panel.bottom - s * 0.5f), radius, radius), brStrokeCard_.get(), s);

    const auto close_bounds = TerminalCloseRect(rect.right, rect.bottom);
    const auto title = l10n::Get(l10n::StringId::TerminalPanel);
    MakeBrush(dc, theme.text, brText_);
    DrawTextRect(dc, compositor_->HeaderFormat(), brText_.get(), title, body.left, panel.top,
        close_bounds.left - body.left - 8 * s, 44 * s);
    fluent::ButtonSpec close;
    close.bounds = close_bounds;
    close.glyph = L"\xE711";
    close.icon_only = true;
    close.kind = fluent::ButtonKind::Transparent;
    close.bordered = false;
    close.state.hovered = IsHovered(vm, HitTestResult::TerminalClose);
    close.state.pressed = close.state.hovered && vm.terminal_close_pressed;
    close.state.hover_progress = HoverOpacity(L"terminal:close", close.state.hovered);
    painter_.DrawButton(close);

    fluent::SplitterSpec splitter;
    splitter.bounds = D2D1::RectF(panel.left - 4 * s, panel.top, panel.left + 4 * s, panel.bottom);
    splitter.vertical = true;
    splitter.state.hovered = IsHovered(vm, HitTestResult::DetailsResize);
    splitter.state.pressed = vm.details_resize_pressed;
    splitter.state.hover_progress = HoverOpacity(L"terminal:splitter", splitter.state.hovered);
    painter_.DrawSplitter(splitter);
    vm.terminal->Draw(*compositor_, theme, body, s, vm.terminal_focused,
        !details_open_motion_.Pending() && !terminal_mode_motion_.Pending(),
        MotionEnabled() && !IsHighContrast());
}

} // namespace pulse::ui
