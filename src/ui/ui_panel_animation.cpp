#include "ui_renderer.h"

namespace pulse::ui {

void MainRenderer::TickDetailsPanelAnimation(uint64_t now, float window_width, bool animate) {
    const bool wide = window_width / scale_ >= kDetailsVisibleWindowDip;
    const bool open = details_visible_ && wide;
    // Resize/DPI changes follow layout immediately; toggling preserves the
    // sampled position so a quick reopen reverses without jumping.
    const bool same_width = details_motion_window_width_ == 0 || details_motion_window_width_ == window_width;
    if (!open && details_open_motion_.target > 0) preview_handler_.Hide();
    details_open_motion_.Tick(open, now, animate && wide && same_width, 220);
    details_motion_window_width_ = window_width;
    if (!details_open_motion_.Pending() && !open) details_exit_commands_.reset();
}

void MainRenderer::DrawAnimatedDetailsPanel(const WindowViewModel& vm, const D2D1_RECT_F& rect,
                                           const Theme& theme) {
    auto* dc = compositor_->Dc();
    const auto panel = DetailsPanelRect(rect.right, rect.bottom);
    const auto clip = D2D1::RectF(panel.left, panel.top, rect.right - margin_, panel.bottom);
    if (clip.right <= clip.left) return;
    if (vm.details_visible) {
        ComPtr<ID2D1Image> target;
        dc->GetTarget(&target);
        ComPtr<ID2D1CommandList> commands;
        if (SUCCEEDED(dc->CreateCommandList(&commands))) {
            dc->SetTarget(commands.get());
            DrawDetailsPanel(vm, rect, theme);
            dc->SetTarget(target.get());
            if (SUCCEEDED(commands->Close())) {
                details_exit_commands_ = std::move(commands);
                details_exit_left_ = panel.left;
            }
        }
    }
    dc->PushAxisAlignedClip(clip, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    const float opacity = details_open_motion_.initialized ? details_open_motion_.value : 1;
    const auto layer = D2D1::LayerParameters1(clip, nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
        D2D1::IdentityMatrix(), opacity);
    dc->PushLayer(layer, nullptr);
    if (details_exit_commands_.get())
        dc->DrawImage(details_exit_commands_.get(), D2D1::Point2F(panel.left - details_exit_left_, 0));
    else if (vm.details_visible) DrawDetailsPanel(vm, rect, theme);
    dc->PopLayer();
    dc->PopAxisAlignedClip();
}

} // namespace pulse::ui
