// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once
#include "ui_compositor.h"
#include <algorithm>
#include <string>
#include <string_view>

namespace pulse::ui {

enum class SurfaceMotion { Fade, Forward, Backward };

// Retain drawing commands rather than copying directory models or GPU pixels.
// Only the previous and current visible surfaces are retained per pane.
class SurfaceTransition {
    friend struct NavigationMotionUiTest;
public:
    bool pending = false;

    bool MatchesNavigation(const std::wstring& key, uint64_t owner, uint64_t revision) const {
        return current_.get() && key == key_ && owner == owner_ && revision == revision_;
    }

    void Reset() {
        current_.reset(); previous_.reset(); key_.clear(); variant_.clear(); pending = false;
        owner_ = revision_ = 0;
        waiting_ = false;
    }

    template<class Draw>
    void Paint(ID2D1DeviceContext* dc, D2D1_RECT_F bounds, const std::wstring& key,
               bool ready, uint64_t now, bool animate, float distance, float duration, Draw draw,
               std::wstring_view variant = {}, bool direct_variant = false,
               SurfaceMotion motion = SurfaceMotion::Forward, uint64_t owner = 0, uint64_t revision = 0) {
        if (!animate) { Reset(); draw(); return; }
        if (bounds.left != bounds_.left || bounds.top != bounds_.top ||
            bounds.right != bounds_.right || bounds.bottom != bounds_.bottom) Reset();
        bounds_ = bounds;
        if (direct_variant && key == key_ && variant != variant_) {
            // Wheel resizing is direct manipulation: never queue repeated
            // full-pane exits or leave an old fade underneath the next step.
            previous_.reset();
            pending = false;
            variant_ = variant;
        }
        const bool owner_changed = owner != owner_;
        const bool navigated = key != key_ || owner_changed || revision != revision_;
        if (navigated || variant != variant_) {
            // A rapid second navigation starts from the last composed frame.
            previous_ = std::move(current_);
            // Navigation commands own direction. A tab switch or a view change
            // must not replay the last navigation stored on the destination tab.
            direction_ = !navigated || owner_changed || motion == SurfaceMotion::Fade ? 0.0f :
                motion == SurfaceMotion::Backward ? -1.0f : 1.0f;
            key_ = key;
            variant_ = variant;
            owner_ = owner;
            revision_ = revision;
            started_ = now;
            waiting_ = !ready;
            pending = previous_.get() != nullptr;
        }
        if (waiting_ && ready) { waiting_ = false; started_ = now; }
        const auto elapsed = now >= started_ ? now - started_ : 0;
        // Brief enumerations keep the old visual; slow folders reveal their
        // loading surface after a bounded grace period.
        if (waiting_ && elapsed >= 120) { waiting_ = false; started_ = now; }
        const float t = waiting_ ? 0.0f : std::clamp(
            static_cast<float>(now >= started_ ? now - started_ : 0) / duration, 0.0f, 1.0f);
        const float p = 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);
        if (t >= 1.0f) { previous_.reset(); pending = false; }

        ComPtr<ID2D1Image> target;
        dc->GetTarget(&target);
        ComPtr<ID2D1CommandList> incoming;
        if (FAILED(dc->CreateCommandList(&incoming))) { Reset(); draw(); return; }
        dc->SetTarget(incoming.get());
        draw();
        dc->SetTarget(target.get());
        if (FAILED(incoming->Close())) { Reset(); draw(); return; }

        ComPtr<ID2D1CommandList> composed;
        if (FAILED(dc->CreateCommandList(&composed))) { Reset(); draw(); return; }
        dc->SetTarget(composed.get());
        if (previous_.get()) {
            // Separate the text-bearing phases so old and new labels never
            // become a double-exposed, unreadable heading during navigation.
            Image(dc, previous_.get(), bounds, std::max(0.0f, 1.0f - 2.0f * p),
                  -direction_ * distance * p);
            Image(dc, incoming.get(), bounds, std::max(0.0f, 2.0f * p - 1.0f),
                  direction_ * distance * (1.0f - p));
        } else dc->DrawImage(incoming.get());
        dc->SetTarget(target.get());
        if (FAILED(composed->Close())) { Reset(); draw(); return; }
        dc->DrawImage(composed.get());
        // Do not retain a chain of prior frames after a transition settles.
        current_ = pending ? std::move(composed) : std::move(incoming);
    }

private:
    ComPtr<ID2D1CommandList> current_, previous_;
    std::wstring key_;
    std::wstring variant_;
    D2D1_RECT_F bounds_{};
    uint64_t started_ = 0;
    uint64_t owner_ = 0, revision_ = 0;
    float direction_ = 1.0f;
    bool waiting_ = false;

    static void Image(ID2D1DeviceContext* dc, ID2D1Image* image, D2D1_RECT_F bounds,
                      float opacity, float offset) {
        if (opacity <= 0.0f) return;
        dc->PushAxisAlignedClip(bounds, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        const auto layer = D2D1::LayerParameters1(bounds, nullptr,
            D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(), opacity);
        dc->PushLayer(layer, nullptr);
        dc->DrawImage(image, D2D1::Point2F(offset, 0));
        dc->PopLayer();
        dc->PopAxisAlignedClip();
    }
};

} // namespace pulse::ui
