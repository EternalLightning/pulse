// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once

#include <d2d1.h>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace pulse::ui {

struct ItemReflowSlot {
    std::wstring key;
    D2D1_RECT_F bounds{};
    int source = -1;
    bool wrap_rows = false;
    float stripe = 0.0f;
};

struct ItemReflowVisual {
    D2D1_RECT_F bounds{};
    float opacity = 1.0f;
    int source = -1;
    bool exiting = false;
    float stripe = 0.0f;
};

// Tracks only the supplied visible items. Coordinates and source identities are
// owned by the caller; an outgoing item keeps its last supplied source value.
class ItemReflowAnimation {
public:
    void Reset() {
        states_.clear();
        visuals_.clear();
        initialized_ = false;
        pending_ = false;
    }

    void Update(const std::vector<ItemReflowSlot>& slots, uint64_t now,
                bool animate, bool reset = false, const D2D1_RECT_F* viewport = nullptr) {
        if (!initialized_ || reset || !animate || slots.size() > kMaxSlots) {
            Snap(slots);
            return;
        }

        for (auto it = states_.begin(); it != states_.end();) {
            State& state = it->second;
            Sample(state, now);
            state.present = false;
            if (state.visual.exiting && !state.active) {
                visuals_.erase(it->first);
                it = states_.erase(it);
            } else {
                ++it;
            }
        }

        // Mark existing live keys before admitting additions, so capacity never
        // evicts a live item simply because it occurs later in the input.
        for (const auto& slot : slots) {
            const auto found = states_.find(slot.key);
            if (found != states_.end()) found->second.present = true;
        }
        for (auto it = states_.begin(); it != states_.end();) {
            State& state = it->second;
            if (!state.present && !state.visual.exiting) {
                Freeze(state);
                StartOpacity(state, 0.0f, now, kExitDuration);
                state.visual.exiting = true;
            }
            if (state.visual.exiting && !state.active) {
                visuals_.erase(it->first);
                it = states_.erase(it);
            } else {
                ++it;
            }
        }

        for (const auto& slot : slots) {
            auto found = states_.find(slot.key);
            if (found == states_.end()) {
                if (states_.size() >= kMaxSlots) {
                    // Live slots take priority over retained exit visuals.
                    for (auto it = states_.begin(); it != states_.end(); ++it) {
                        if (!it->second.present) {
                            visuals_.erase(it->first);
                            states_.erase(it);
                            break;
                        }
                    }
                }
                State state;
                state.visual = {slot.bounds, 0.0f, slot.source, false, ClampStripe(slot.stripe)};
                state.present = true;
                state.from_bounds = state.target_bounds = slot.bounds;
                state.from_stripe = state.target_stripe = state.visual.stripe;
                bool occupied = !viewport;
                if (viewport) {
                    for (const auto& [_, sibling] : states_) {
                        if (sibling.present && sibling.visual.opacity > 0 &&
                            Intersects(sibling.visual.bounds, slot.bounds)) { occupied = true; break; }
                    }
                }
                StartOpacity(state, 1.0f, now, kMoveDuration, occupied ? kEntryDelay : 0);
                states_.emplace(slot.key, state);
            } else {
                State& state = found->second;
                if (state.visual.exiting || !Equal(state.target_bounds, slot.bounds)) {
                    if (state.visual.exiting) {
                        StartOpacity(state, 1.0f, now, kMoveDuration);
                    }
                    if (state.wrap_offset != 0.0f && state.motion_active) {
                        const uint64_t elapsed = now >= state.started ? now - state.started : 0;
                        if (elapsed < state.duration / 2) {
                            // Finish leaving the old row before using the latest destination.
                            state.target_bounds = slot.bounds;
                        } else {
                            // Reentry follows the sampled position to the new destination
                            // without another disappearance or a new wrap deadline.
                            StartMotion(state, slot.bounds, now, state.duration - elapsed);
                        }
                    } else {
                        const bool reentering = state.wrap_fade_active;
                        const uint64_t remaining = reentering
                            ? state.wrap_started + state.wrap_duration - now : kMoveDuration;
                        StartMotion(state, slot.bounds, now, remaining);
                        const float cell_width = slot.bounds.right - slot.bounds.left;
                        const float horizontal_delta = slot.bounds.left - state.from_bounds.left;
                        if (!reentering && slot.wrap_rows && cell_width > 0.0f &&
                            std::fabs(horizontal_delta) > 1.5f * cell_width &&
                            slot.bounds.top != state.from_bounds.top) {
                            state.wrap_offset = horizontal_delta > 0.0f ? -cell_width : cell_width;
                            state.wrap_started = now;
                            state.wrap_duration = state.duration;
                            state.wrap_fade_active = true;
                        }
                    }
                }
                const float stripe = ClampStripe(slot.stripe);
                if (state.target_stripe != stripe) {
                    state.from_stripe = state.visual.stripe;
                    state.target_stripe = stripe;
                    state.stripe_started = now;
                    state.stripe_active = state.from_stripe != stripe;
                    state.active = state.motion_active || state.opacity_active ||
                                   state.wrap_fade_active || state.stripe_active;
                }
                state.visual.source = slot.source;
                state.visual.exiting = false;
                state.present = true;
            }
        }

        pending_ = false;
        for (const auto& [key, state] : states_) {
            visuals_[key] = state.visual;
            pending_ = pending_ || state.active;
        }
    }

    bool Pending() const { return pending_; }

    ItemReflowVisual Visual(const std::wstring& key) const {
        const auto found = visuals_.find(key);
        return found == visuals_.end() ? ItemReflowVisual{} : found->second;
    }

    const std::unordered_map<std::wstring, ItemReflowVisual>& Visuals() const {
        return visuals_;
    }

private:
    static constexpr size_t kMaxSlots = 512;
    static constexpr uint64_t kMoveDuration = 240;
    static constexpr uint64_t kEntryDelay = 120;
    static constexpr uint64_t kExitDuration = 80;

    struct State {
        ItemReflowVisual visual;
        D2D1_RECT_F from_bounds{};
        D2D1_RECT_F target_bounds{};
        D2D1_RECT_F from_velocity{};
        D2D1_RECT_F velocity{};
        float from_opacity = 1.0f;
        float target_opacity = 1.0f;
        float opacity = 1.0f;
        float from_stripe = 0.0f;
        float target_stripe = 0.0f;
        uint64_t started = 0;
        uint64_t duration = 0;
        uint64_t opacity_started = 0;
        uint64_t opacity_duration = 0;
        uint64_t opacity_delay = 0;
        uint64_t wrap_started = 0;
        uint64_t wrap_duration = 0;
        uint64_t stripe_started = 0;
        float wrap_offset = 0.0f;
        bool motion_active = false;
        bool opacity_active = false;
        bool wrap_fade_active = false;
        bool stripe_active = false;
        bool active = false;
        bool present = false;
    };

    static bool Equal(D2D1_RECT_F a, D2D1_RECT_F b) {
        return a.left == b.left && a.top == b.top &&
               a.right == b.right && a.bottom == b.bottom;
    }

    static bool Intersects(D2D1_RECT_F a, D2D1_RECT_F b) {
        return a.right > b.left && a.left < b.right &&
               a.bottom > b.top && a.top < b.bottom;
    }

    static float Mix(float from, float to, float amount) {
        return from + (to - from) * amount;
    }

    static float ClampStripe(float stripe) {
        return (std::max)(0.0f, (std::min)(stripe, 1.0f));
    }

    static float Ease(uint64_t elapsed, uint64_t duration) {
        const float amount = static_cast<float>(elapsed) / static_cast<float>(duration);
        return amount * amount * (3.0f - 2.0f * amount);
    }

    static float BoundedVelocity(float velocity, float delta, uint64_t duration) {
        // A reversal brakes at the current position. Same-direction retargets
        // carry velocity unless the monotonic Hermite slope bound requires less.
        if (velocity * delta <= 0.0f) return 0.0f;
        const float limit = 3.0f * std::fabs(delta) / static_cast<float>(duration);
        return std::copysign((std::min)(std::fabs(velocity), limit), delta);
    }

    static void StartMotion(State& state, D2D1_RECT_F bounds,
                            uint64_t now, uint64_t duration) {
        state.from_bounds = state.visual.bounds;
        state.target_bounds = bounds;
        state.from_velocity = {
            BoundedVelocity(state.velocity.left, bounds.left - state.from_bounds.left, duration),
            BoundedVelocity(state.velocity.top, bounds.top - state.from_bounds.top, duration),
            BoundedVelocity(state.velocity.right, bounds.right - state.from_bounds.right, duration),
            BoundedVelocity(state.velocity.bottom, bounds.bottom - state.from_bounds.bottom, duration)};
        state.velocity = state.from_velocity;
        state.started = now;
        state.duration = duration;
        state.wrap_offset = 0.0f;
        state.motion_active = !Equal(state.from_bounds, bounds);
        state.active = state.motion_active || state.opacity_active || state.wrap_fade_active ||
                       state.stripe_active;
    }

    static void StartOpacity(State& state, float opacity, uint64_t now,
                             uint64_t duration, uint64_t delay = 0) {
        state.opacity = state.from_opacity = state.visual.opacity;
        state.target_opacity = opacity;
        state.opacity_started = now;
        state.opacity_duration = duration;
        state.opacity_delay = delay;
        state.opacity_active = state.from_opacity != opacity;
        state.active = state.motion_active || state.opacity_active || state.wrap_fade_active ||
                       state.stripe_active;
    }

    static void Freeze(State& state) {
        state.from_bounds = state.target_bounds = state.visual.bounds;
        state.velocity = state.from_velocity = {};
        state.motion_active = false;
        state.wrap_offset = 0.0f;
        state.wrap_fade_active = false;
        state.from_stripe = state.target_stripe = state.visual.stripe;
        state.stripe_active = false;
    }

    static float Position(float from, float to, float velocity, float amount,
                          float duration, float& sampled_velocity) {
        const float squared = amount * amount;
        const float progress = squared * (3.0f - 2.0f * amount);
        const float tangent = amount * (1.0f - amount) * (1.0f - amount);
        sampled_velocity = (to - from) * 6.0f * amount * (1.0f - amount) / duration +
                           velocity * (1.0f - 4.0f * amount + 3.0f * squared);
        return Mix(from, to, progress) + velocity * duration * tangent;
    }

    static void SampleMotion(State& state, uint64_t now) {
        if (!state.motion_active) return;
        const uint64_t elapsed = now >= state.started ? now - state.started : 0;
        if (elapsed >= state.duration) {
            state.visual.bounds = state.target_bounds;
            state.velocity = {};
            state.motion_active = false;
            state.wrap_offset = 0.0f;
            return;
        }
        if (state.wrap_offset != 0.0f) {
            // Cross the row boundary only while invisible, so a wrapped item
            // never travels diagonally through its siblings' icons and labels.
            const uint64_t half_duration = state.duration / 2;
            if (elapsed < half_duration) {
                const float amount = Ease(elapsed, half_duration);
                state.visual.bounds = state.from_bounds;
                state.visual.bounds.left += state.wrap_offset * amount;
                state.visual.bounds.right += state.wrap_offset * amount;
            } else {
                const float amount = Ease(elapsed - half_duration,
                                          state.duration - half_duration);
                state.visual.bounds = state.target_bounds;
                state.visual.bounds.left -= state.wrap_offset * (1.0f - amount);
                state.visual.bounds.right -= state.wrap_offset * (1.0f - amount);
            }
            const uint64_t leg_duration = elapsed < half_duration
                ? half_duration : state.duration - half_duration;
            const uint64_t leg_elapsed = elapsed < half_duration ? elapsed : elapsed - half_duration;
            const float leg = static_cast<float>(leg_elapsed) / static_cast<float>(leg_duration);
            const float velocity = state.wrap_offset * 6.0f * leg * (1.0f - leg) /
                                   static_cast<float>(leg_duration);
            state.velocity = {velocity, 0.0f, velocity, 0.0f};
            return;
        }
        const float amount = static_cast<float>(elapsed) / static_cast<float>(state.duration);
        const float duration = static_cast<float>(state.duration);
        state.visual.bounds = {
            Position(state.from_bounds.left, state.target_bounds.left, state.from_velocity.left,
                     amount, duration, state.velocity.left),
            Position(state.from_bounds.top, state.target_bounds.top, state.from_velocity.top,
                     amount, duration, state.velocity.top),
            Position(state.from_bounds.right, state.target_bounds.right, state.from_velocity.right,
                     amount, duration, state.velocity.right),
            Position(state.from_bounds.bottom, state.target_bounds.bottom, state.from_velocity.bottom,
                     amount, duration, state.velocity.bottom)};
    }

    static void Sample(State& state, uint64_t now) {
        if (!state.active) return;
        SampleMotion(state, now);
        if (state.opacity_active) {
            const uint64_t elapsed = now >= state.opacity_started ? now - state.opacity_started : 0;
            if (elapsed >= state.opacity_duration) {
                state.opacity = state.target_opacity;
                state.opacity_active = false;
            } else {
                const float amount = elapsed > state.opacity_delay
                    ? Ease(elapsed - state.opacity_delay, state.opacity_duration - state.opacity_delay)
                    : 0.0f;
                state.opacity = Mix(state.from_opacity, state.target_opacity, amount);
            }
        }
        float wrap_opacity = 1.0f;
        if (state.wrap_fade_active) {
            const uint64_t elapsed = now >= state.wrap_started ? now - state.wrap_started : 0;
            const uint64_t half_duration = state.wrap_duration / 2;
            if (elapsed >= state.wrap_duration) {
                state.wrap_fade_active = false;
            } else if (elapsed < half_duration) {
                wrap_opacity = 1.0f - Ease(elapsed, half_duration);
            } else {
                wrap_opacity = Ease(elapsed - half_duration, state.wrap_duration - half_duration);
            }
        }
        state.visual.opacity = state.opacity * wrap_opacity;
        if (state.stripe_active) {
            const uint64_t elapsed = now >= state.stripe_started ? now - state.stripe_started : 0;
            if (elapsed >= kMoveDuration) {
                state.visual.stripe = state.target_stripe;
                state.stripe_active = false;
            } else {
                state.visual.stripe = Mix(state.from_stripe, state.target_stripe,
                                          Ease(elapsed, kMoveDuration));
            }
        }
        state.active = state.motion_active || state.opacity_active || state.wrap_fade_active ||
                       state.stripe_active;
    }

    void Snap(const std::vector<ItemReflowSlot>& slots) {
        states_.clear();
        visuals_.clear();
        for (const auto& slot : slots) {
            if (states_.size() >= kMaxSlots) break;
            State state;
            state.visual = {slot.bounds, 1.0f, slot.source, false, ClampStripe(slot.stripe)};
            state.from_bounds = slot.bounds;
            state.target_bounds = slot.bounds;
            state.from_stripe = state.target_stripe = state.visual.stripe;
            states_[slot.key] = state;
            visuals_[slot.key] = state.visual;
        }
        initialized_ = true;
        pending_ = false;
    }

    std::unordered_map<std::wstring, State> states_;
    std::unordered_map<std::wstring, ItemReflowVisual> visuals_;
    bool initialized_ = false;
    bool pending_ = false;
};

} // namespace pulse::ui
