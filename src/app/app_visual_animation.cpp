#include "app_visual_animation.h"
#include "app_internal.h"
#include "pane_animations.h"
#include <algorithm>
#include <cmath>
#include <unordered_set>

namespace pulse {
namespace {

// --- Tag slide animation ------------------------------------------------------
// SortableJS-style reorder motion: a quick easeOutCubic with no overshoot.
// On 32px rows an overshoot spring (the old Pivot indicator curve) reads as
// wobble, especially when fast drags restart the track mid-flight.
float TagEaseOutCubic(float x) {
    const float u = 1.0f - x;
    return 1.0f - u * u * u;
}

float TagEaseInOutQuad(float t) { // used by the window-tab slide animation
    const float u = -2.0f * t + 2.0f;
    return t < 0.5f ? 2.0f * t * t : 1.0f - u * u * 0.5f;
}

void TickTagTransitions(AppState& s) {
    const auto now = std::chrono::steady_clock::now();
    for (auto it = s.tagTracks.begin(); it != s.tagTracks.end();) {
        const float t = std::chrono::duration<float, std::milli>(now - it->second.t0).count()
            / static_cast<float>(it->second.durationMs);
        if (t >= 1.0f) {
            s.tagOffsets.erase(it->first);
            it = s.tagTracks.erase(it);
        } else {
            s.tagOffsets[it->first] = it->second.start * (1.0f - TagEaseOutCubic(t));
            ++it;
        }
    }
    // Insertion indicator chase (same 180ms easeOutCubic as the slides).
    if (s.tagGapFrom != s.tagGapTo) {
        const float t = std::chrono::duration<float, std::milli>(now - s.tagGapT0).count()
            / 180.0f;
        if (t >= 1.0f) {
            s.tagGapLineY = s.tagGapTo;
            s.tagGapFrom = s.tagGapTo;
        } else {
            s.tagGapLineY = s.tagGapFrom +
                (s.tagGapTo - s.tagGapFrom) * TagEaseOutCubic(t);
        }
    }
}

void TickTabTransitions(AppState& s) {
    std::unordered_set<const app::LayoutTab*> live;
    if (s.pane) {
        for (const auto& t : s.window_tabs.items) live.insert(t.get());
    }
    const auto now = std::chrono::steady_clock::now();
    for (auto it = s.tabTracks.begin(); it != s.tabTracks.end();) {
        if (!live.count(it->first)) {
            s.tabOffsets.erase(it->first);
            it = s.tabTracks.erase(it);
            continue;
        }
        const float t = std::chrono::duration<float, std::milli>(now - it->second.t0).count()
            / static_cast<float>(it->second.durationMs);
        if (t >= 1.0f) {
            s.tabOffsets.erase(it->first);
            it = s.tabTracks.erase(it);
        } else {
            s.tabOffsets[it->first] = it->second.start * (1.0f - TagEaseInOutQuad(t));
            ++it;
        }
    }
    for (auto it = s.tabOffsets.begin(); it != s.tabOffsets.end();) {
        if (!live.count(it->first)) it = s.tabOffsets.erase(it);
        else ++it;
    }
    // Chip slide channel: same easing, px units, keyed by group id.
    std::unordered_set<int> liveGroups;
    if (s.pane) {
        for (const auto& g : s.window_tabs.tab_groups) liveGroups.insert(g.id);
    }
    for (auto it = s.chipTracks.begin(); it != s.chipTracks.end();) {
        if (!liveGroups.count(it->first)) {
            s.chipOffsets.erase(it->first);
            it = s.chipTracks.erase(it);
            continue;
        }
        const float t = std::chrono::duration<float, std::milli>(now - it->second.t0).count()
            / static_cast<float>(it->second.durationMs);
        if (t >= 1.0f) {
            s.chipOffsets.erase(it->first);
            it = s.chipTracks.erase(it);
        } else {
            s.chipOffsets[it->first] = it->second.start * (1.0f - TagEaseInOutQuad(t));
            ++it;
        }
    }
    for (auto it = s.chipOffsets.begin(); it != s.chipOffsets.end();) {
        if (!liveGroups.count(it->first)) it = s.chipOffsets.erase(it);
        else ++it;
    }
}

} // namespace

bool TickVisualAnimations(AppState& s) {
    const double now = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    const double previous_wake = s.sidebarScrollbarFade.NextWake();
    s.sidebarScrollbarFade.Tick(now, s.sidebarScroll,
        s.hoverRegion == static_cast<int>(ui::HitTestResult::Scrollbar) && s.hoverPaneIndex < 0,
        s.scrollbarDragging && s.scrollbarSidebar);
    const double wake = s.sidebarScrollbarFade.NextWake();
    if (!wake) s.sidebarFadeDeadline = 0;
    else if (wake != previous_wake) {
        // Convert once per new hold, so clock sampling cannot move an armed timer.
        s.sidebarFadeDeadline = GetTickCount64() +
            static_cast<uint64_t>(std::ceil(std::max(0.0, wake - now)));
    }
    TickSidebarFolds(s, static_cast<ULONGLONG>(now));
    if (s.detailsPreviewFoldStart) {
        const float t = std::clamp(static_cast<float>(now - s.detailsPreviewFoldStart) / 150.0f, 0.0f, 1.0f);
        const float eased = t * t * (3.0f - 2.0f * t);
        const float target = s.detailsPreviewOnly ? 1.0f : 0.0f;
        s.detailsPreviewExpansion = s.detailsPreviewExpansionFrom +
            (target - s.detailsPreviewExpansionFrom) * eased;
        if (t >= 1.0f) s.detailsPreviewFoldStart = 0;
    }
    const float target = s.scrollbarHovered
        ? std::max(0.0f, s.renderer.Margin() * 1.5f - 6.0f * s.scale) : 0.0f;
    // A changed target starts at this paint; idle time must not jump the animation.
    const double elapsed = s.visualAnimationSampleMs && target == s.scrollbarHoverTarget
        ? std::clamp(now - s.visualAnimationSampleMs, 0.0, 200.0) : 0.0;
    s.visualAnimationSampleMs = now;
    s.scrollbarHoverTarget = target;
    s.scrollbarHoverWidth = app::AdvanceScrollbarHover(s.scrollbarHoverWidth, target, elapsed);
    if (!s.tagTracks.empty() || s.tagGapFrom != s.tagGapTo) TickTagTransitions(s);
    if (!s.tabTracks.empty() || !s.chipTracks.empty()) TickTabTransitions(s);
    return s.sidebarScrollbarFade.Pending() || !s.sidebarFoldTracks.empty() ||
        s.detailsPreviewFoldStart || s.scrollbarHoverWidth != target ||
        !s.tagTracks.empty() || s.tagGapFrom != s.tagGapTo ||
        !s.tabTracks.empty() || !s.chipTracks.empty();
}

} // namespace pulse
