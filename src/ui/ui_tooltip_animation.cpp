// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "ui_renderer.h"
#include "ui_renderer_internal.h"
#include "animation_clock.h"

namespace pulse::ui {

void MainRenderer::UpdateTooltipAnimation(const WindowViewModel& vm, uint64_t now, bool animate) {
    const bool suppressed = vm.change_popover.visible || !vm.drag_badge.empty() ||
        vm.tab_drag_index >= 0 || vm.tag_drag_group >= 0 || vm.sidebar_group_drag_id >= 0 ||
        vm.sidebar_pin_drag_index >= 0 || vm.address_search_scope_open || vm.address_search_options_open;
    if (suppressed) {
        tooltip_transition_.Tick(false, now, false, 120.0f);
        tooltip_text_.clear();
        return;
    }
    const bool visible = !vm.tooltip_text.empty();
    const bool changed = visible && (tooltip_text_ != vm.tooltip_text ||
        tooltip_hover_region_ != vm.hover_region || tooltip_control_index_ != vm.hover_control_index ||
        tooltip_sub_index_ != vm.hover_sub_index);
    if (changed) {
        // Replace one retained label atomically; old and new text never overlap.
        tooltip_transition_.Tick(false, now, false, 100.0f);
        tooltip_text_ = vm.tooltip_text;
        tooltip_hover_region_ = vm.hover_region;
        tooltip_control_index_ = vm.hover_control_index;
        tooltip_sub_index_ = vm.hover_sub_index;
    }
    if (visible) {
        tooltip_x_ = vm.tooltip_x;
        tooltip_y_ = vm.tooltip_y;
    }
    // Tick samples the active curve before changing direction, so use that
    // curve's duration even when this frame reverses the visibility target.
    const float duration = tooltip_transition_.target > 0.0f ? 100.0f : 120.0f;
    tooltip_transition_.Tick(visible, now, animate, duration);
    if (!visible && !tooltip_transition_.Pending()) tooltip_text_.clear();
    surface_animating_ |= tooltip_transition_.Pending();
}

void MainRenderer::DrawTooltip(const WindowViewModel& vm, const D2D1_RECT_F& rect, const Theme& theme) {
    UpdateTooltipAnimation(vm, AnimationTimeMs(), motion_enabled_ && !IsHighContrast());
    const float opacity = tooltip_transition_.value;
    if (tooltip_text_.empty() || opacity <= 0.0f) return;
    auto* dc = compositor_->Dc();
    auto* fmt = compositor_->SmallFormat();
    const float tw = MeasureLayoutText(compositor_, compositor_->DwriteFactory(), fmt, tooltip_text_);
    const float bw = std::min(tw + 20.0f * scale_, rect.right - 16.0f * scale_);
    const float bh = 28.0f * scale_;
    if (bw <= 0.0f) return;
    const float bx = std::clamp(tooltip_x_ + 12.0f * scale_, 8.0f * scale_,
        std::max(8.0f * scale_, rect.right - bw - 8.0f * scale_));
    const float tip_y = tooltip_hover_region_ == HitTestResult::StatusBarCancelSearch
        ? rect.bottom - status_height_ - bh - 8.0f * scale_ : tooltip_y_ + 18.0f * scale_;
    const float by = std::clamp(tip_y, 8.0f * scale_,
        std::max(8.0f * scale_, rect.bottom - bh - 8.0f * scale_));
    const auto bounds = D2D1::RectF(bx, by, bx + bw, by + bh);
    const bool layered = opacity < 1.0f;
    if (layered) {
        const auto layer = D2D1::LayerParameters1(rect, nullptr,
            D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(), opacity);
        dc->PushLayer(layer, nullptr);
    }
    MakeBrush(dc, theme.surface_flyout, brFillHover_);
    FillRoundedRect(dc, brFillHover_.get(), bx, by, bw, bh, 4.0f * scale_);
    MakeBrush(dc, theme.stroke_card, brStrokeCard_);
    dc->DrawRoundedRectangle(D2D1::RoundedRect(bounds, 4.0f * scale_, 4.0f * scale_),
        brStrokeCard_.get(), 1.0f);
    MakeBrush(dc, theme.text, brText_);
    const auto saved_tip_bg = text_background_;
    text_background_ = BlendOver(theme.surface_flyout, theme.bg);
    DrawTextRect(dc, fmt, brText_.get(), tooltip_text_, bx + 10.0f * scale_, by,
        std::max(0.0f, bw - 20.0f * scale_), bh);
    text_background_ = saved_tip_bg;
    if (layered) dc->PopLayer();
}

} // namespace pulse::ui
