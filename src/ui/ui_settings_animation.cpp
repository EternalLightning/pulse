#include "ui_renderer.h"

namespace pulse::ui {

float MainRenderer::HoverOpacity(const std::wstring& key, bool hovered) {
    auto& track = hover_tracks_[key];
    track.frame = settings_switch_frame_;
    if (!track.animation.initialized) track.animation.Tick(false, settings_switch_now_, true, 110);
    track.animation.Tick(hovered, settings_switch_now_, motion_enabled_ && !IsHighContrast(),
                         hovered ? 110.0f : 150.0f);
    surface_animating_ |= track.animation.Pending();
    return track.animation.value;
}

void MainRenderer::DrawSettingsSwitch(const std::wstring& key, const D2D1_RECT_F& bounds,
                                      fluent::ControlState state) {
    auto& track = settings_switch_tracks_[key];
    track.frame = settings_switch_frame_;
    track.transition.Tick(state.checked, settings_switch_now_, motion_enabled_ && !IsHighContrast(), 160.0f);
    state.check_progress = track.transition.value;
    settings_switch_animating_ |= track.transition.Pending();
    painter_.DrawSwitch(bounds, L"", state);
}

} // namespace pulse::ui
