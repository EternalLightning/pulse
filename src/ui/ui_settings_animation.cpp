// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "ui_renderer.h"

namespace pulse::ui {

float MainRenderer::DisclosureProgress(const std::wstring& key, bool expanded) {
    auto& track = hover_tracks_[key];
    track.frame = settings_switch_frame_;
    track.animation.Tick(expanded, settings_switch_now_, motion_enabled_ && !IsHighContrast(), 180.0f);
    surface_animating_ |= track.animation.Pending();
    return track.animation.value;
}

float MainRenderer::HoverOpacity(const std::wstring& key, bool hovered, float enter_ms, float exit_ms) {
    auto& track = hover_tracks_[key];
    track.frame = settings_switch_frame_;
    if (!track.animation.initialized) track.animation.Tick(false, settings_switch_now_, true, 110);
    // Finish sampling the current leg with its own duration before reversing.
    track.animation.Tick(hovered, settings_switch_now_, motion_enabled_ && !IsHighContrast(),
                         track.animation.target > 0.0f ? enter_ms : exit_ms);
    surface_animating_ |= track.animation.Pending();
    return track.animation.value;
}

float MainRenderer::ControlHoverOpacity(const D2D1_RECT_F& bounds, bool hovered) {
    const auto key = L"control:" + std::to_wstring(painting_pane_) + L":" +
        std::to_wstring(bounds.left) + L":" + std::to_wstring(bounds.top) + L":" +
        std::to_wstring(bounds.right) + L":" + std::to_wstring(bounds.bottom);
    return HoverOpacity(key, hovered);
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
