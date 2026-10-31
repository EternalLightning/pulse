// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution.

#pragma once
#include "ui_compositor.h"
#include "view_layout.h"
#include "animation_image_cache.h"
#include <algorithm>
#include <string>
#include <unordered_map>

namespace pulse::ui {

// A bounded picture of the visible list, with icons separated from text. No
// directory models are copied and no filesystem work is done by this surface.
class ViewModeTransition {
public:
    static constexpr size_t kMaxIcons = 256;
    static constexpr uint64_t kDuration = 220;
    static constexpr uint64_t kWheelDuration = 120;

    void Reset() {
        current_ = {}; previous_ = {}; incoming_ = {};
        identity_.clear(); initialized_ = pending_ = recording_ = false;
        image_cache_.Reset(); reused_ = false;
    }
    bool Pending() const { return pending_; }
    bool Capturing() const { return recording_ && capture_icons_; }
    size_t CachedIcons() const { return current_.icons.size() + previous_.icons.size(); }
    void SuppressCapture() { capture_icons_ = false; }
    bool ReusedFrame() const { return reused_; }
    template<class Visit> void RetainIcons(Visit visit) const {
        for (const auto& [_, picture] : current_.icons)
            visit(picture.source, std::max(picture.bounds.right - picture.bounds.left, picture.bounds.bottom - picture.bounds.top));
    }
    bool Matches(const std::wstring& identity, ViewMode mode, D2D1_RECT_F viewport,
                 D2D1_POINT_2F scroll, uint64_t revision) const {
        return pending_ && identity == identity_ && mode == mode_ && revision == revision_ &&
            scroll.x == scroll_.x && scroll.y == scroll_.y &&
            viewport.left == viewport_.left && viewport.top == viewport_.top &&
            viewport.right == viewport_.right && viewport.bottom == viewport_.bottom;
    }

    D2D1_RECT_F IconBounds(const std::wstring& key) const {
        const auto next = current_.icons.find(key);
        const auto old = previous_.icons.find(key);
        if (next == current_.icons.end()) return old == previous_.icons.end() ? D2D1_RECT_F{} : old->second.bounds;
        if (!pending_ || old == previous_.icons.end()) return next->second.bounds;
        return Mix(old->second.bounds, next->second.bounds, progress_);
    }

    // Moving icons select their live source, rather than the row underneath.
    int HitTest(float x, float y) const {
        if (!pending_) return -1;
        int source = -1;
        float distance = 3.0f;
        const auto consider = [&](const Picture& picture, D2D1_RECT_F bounds, float opacity) {
            if (opacity <= 0.1f || x < bounds.left || x >= bounds.right || y < bounds.top || y >= bounds.bottom) return;
            const float dx = (x - (bounds.left + bounds.right) * 0.5f) / std::max(1.0f, bounds.right - bounds.left);
            const float dy = (y - (bounds.top + bounds.bottom) * 0.5f) / std::max(1.0f, bounds.bottom - bounds.top);
            if (dx * dx + dy * dy < distance) { distance = dx * dx + dy * dy; source = picture.source; }
        };
        for (const auto& [key, picture] : current_.icons) {
            const auto old = previous_.icons.find(key);
            const float opacity = old == previous_.icons.end() ? NewTextOpacity()
                : old->second.opacity + (1.0f - old->second.opacity) * progress_;
            consider(picture, IconBounds(key), opacity);
        }
        for (const auto& [key, picture] : previous_.icons)
            if (!current_.icons.contains(key)) consider(picture, picture.bounds, OldTextOpacity() * picture.opacity);
        return source;
    }

    template<class Draw>
    void Icon(ID2D1DeviceContext* dc, const std::wstring& key, D2D1_RECT_F bounds, int source, Draw draw) {
        if (!Capturing()) { draw(); return; }
        if (!Intersects(bounds, viewport_)) return;
        if (incoming_.icons.size() >= kMaxIcons) {
            capture_icons_ = false;
            draw();
            return;
        }
        Picture picture;
        picture.bounds = picture.image_bounds = bounds;
        picture.source = source;
        if (!Record(dc, picture.commands, draw)) {
            capture_icons_ = false;
            draw();
            return;
        }
        incoming_.icons.emplace(key, std::move(picture));
    }

    template<class Draw>
    void Paint(ID2D1DeviceContext* dc, const std::wstring& identity, ViewMode mode,
               D2D1_RECT_F viewport, D2D1_POINT_2F scroll, uint64_t revision,
               uint64_t now, bool animate, bool direct, Draw draw, uint64_t mode_revision = 0,
               uint64_t frame_signature = 0, bool allow_reuse = true) {
        if (!animate) { Reset(); draw(); return; }
        const bool input_changed = initialized_ && mode_revision != mode_revision_;
        const bool scroll_changed = scroll.x != scroll_.x || scroll.y != scroll_.y;
        const bool changed = initialized_ && (mode != mode_ || (input_changed && scroll_changed));
        const bool compatible = initialized_ && identity == identity_ &&
            viewport.left == viewport_.left && viewport.right == viewport_.right && viewport.bottom == viewport_.bottom &&
            (changed || (viewport.top == viewport_.top && !scroll_changed && (revision == revision_ || input_changed)));
        if (!compatible) {
            Reset();
        } else if (changed && current_.valid && !current_.icons.empty()) {
            // Restart from the last displayed icon rectangles. Retain primitive
            // icon pictures, never a recursive chain of composed whole frames.
            if (pending_) {
                for (auto& [key, picture] : current_.icons) {
                    picture.bounds = IconBounds(key);
                    const auto old = previous_.icons.find(key);
                    if (old == previous_.icons.end()) picture.opacity = NewTextOpacity();
                    else {
                        picture.opacity = old->second.opacity + (1.0f - old->second.opacity) * progress_;
                        if (Extent(old->second) > Extent(picture)) ShareImage(picture, old->second);
                    }
                }
                for (const auto& [key, picture] : previous_.icons) {
                    if (current_.icons.size() >= kMaxIcons) break;
                    const float opacity = OldTextOpacity() * picture.opacity;
                    if (current_.icons.contains(key) || opacity <= 0.01f) continue;
                    Picture frozen;
                    ShareImage(frozen, picture);
                    frozen.bounds = picture.bounds; frozen.source = picture.source; frozen.opacity = opacity;
                    current_.icons.emplace(key, std::move(frozen));
                }
                current_.decoration_opacity *= NewTextOpacity();
            }
            previous_ = std::move(current_);
            started_ = now;
            duration_ = direct ? kWheelDuration : kDuration;
            direct_ = direct;
            pending_ = true;
        } else if (changed) {
            previous_ = {};
            pending_ = false;
        }
        identity_ = identity; mode_ = mode; viewport_ = viewport; scroll_ = scroll; revision_ = revision; mode_revision_ = mode_revision;
        initialized_ = true;
        linear_ = pending_ ? std::clamp(static_cast<float>(now >= started_ ? now - started_ : 0) /
            static_cast<float>(duration_), 0.0f, 1.0f) : 1.0f;
        const float remaining = 1.0f - linear_;
        progress_ = direct_ ? 1.0f - remaining * remaining * remaining : linear_ * linear_ * (3.0f - 2.0f * linear_);
        if (linear_ >= 1.0f) { pending_ = false; previous_ = {}; }
        reused_ = pending_ && frame_signature && allow_reuse && frame_signature == frame_signature_ &&
            current_.valid && current_.decoration.get();
        if (!reused_) {
            incoming_ = {};
            recording_ = capture_icons_ = true;
            const bool recorded = Record(dc, incoming_.decoration, draw);
            recording_ = false;
            if (!recorded) { Reset(); draw(); return; }
            incoming_.valid = capture_icons_;
            current_ = std::move(incoming_);
            frame_signature_ = frame_signature;
            if (pending_ && frame_signature && allow_reuse) {
                Rasterize(dc, previous_);
                Rasterize(dc, current_);
            }
        }
        if (!current_.valid) { pending_ = false; previous_ = {}; }
        if (pending_) {
            Decoration(dc, previous_, previous_.decoration_opacity * OldTextOpacity());
            Decoration(dc, current_, NewTextOpacity());
            for (const auto& [key, picture] : previous_.icons) {
                if (!current_.icons.contains(key)) DrawPicture(dc, picture, picture.bounds, OldTextOpacity());
            }
            for (const auto& [key, picture] : current_.icons) {
                const auto old = previous_.icons.find(key);
                if (old == previous_.icons.end()) DrawPicture(dc, picture, picture.bounds, NewTextOpacity());
                else {
                    // Draw one image per file. The larger source avoids
                    // magnifying a tiny row icon while shrinking a thumbnail.
                    const auto& source = Extent(old->second) > Extent(picture) ? old->second : picture;
                    const float opacity = old->second.opacity + (1.0f - old->second.opacity) * progress_;
                    DrawPicture(dc, source, IconBounds(key), opacity, false);
                }
            }
        } else {
            Image(dc, current_.decoration.get(), 1.0f);
            for (const auto& [_, picture] : current_.icons) DrawPicture(dc, picture, picture.bounds, 1.0f);
        }
        // Keep only one ordinary visible frame after settling.
        if (!pending_) previous_ = {};
    }

private:
    struct Picture {
        ComPtr<ID2D1CommandList> commands;
        ComPtr<ID2D1Bitmap1> bitmap;
        D2D1_RECT_F bitmap_source{};
        D2D1_RECT_F bounds{}, image_bounds{};
        int source = -1;
        float opacity = 1.0f;
    };
    struct Frame {
        ComPtr<ID2D1CommandList> decoration;
        ComPtr<ID2D1Bitmap1> decoration_bitmap;
        D2D1_RECT_F decoration_bounds{};
        std::unordered_map<std::wstring, Picture> icons;
        float decoration_opacity = 1.0f;
        bool valid = false;
    };
    Frame current_, previous_, incoming_;
    AnimationImageCache image_cache_;
    std::wstring identity_;
    ViewMode mode_ = ViewMode::Details;
    D2D1_RECT_F viewport_{};
    D2D1_POINT_2F scroll_{};
    uint64_t revision_ = 0, mode_revision_ = 0, started_ = 0, duration_ = kDuration;
    uint64_t frame_signature_ = 0;
    float linear_ = 1.0f, progress_ = 1.0f;
    bool initialized_ = false, pending_ = false, recording_ = false, capture_icons_ = false, direct_ = false;
    bool reused_ = false;

    void Rasterize(ID2D1DeviceContext* dc, Frame& frame) {
        if (!AnimationPixelCacheEnabled() || !frame.decoration.get() || frame.decoration_bitmap.get()) return;
        std::vector<AnimationImageSlice> slices;
        std::vector<Picture*> pictures;
        slices.reserve(frame.icons.size()); pictures.reserve(frame.icons.size());
        for (auto& [_, picture] : frame.icons) {
            if (picture.bitmap.get()) continue;
            slices.push_back({picture.commands.get(), picture.image_bounds, {}, {}});
            pictures.push_back(&picture);
        }
        if (!image_cache_.Build(dc, frame.decoration.get(), viewport_, slices, frame.decoration_bitmap)) return;
        frame.decoration_bounds = viewport_;
        for (size_t i = 0; i < pictures.size(); ++i) {
            pictures[i]->bitmap = std::move(slices[i].bitmap);
            pictures[i]->bitmap_source = slices[i].source;
        }
    }
    static void Decoration(ID2D1DeviceContext* dc, const Frame& frame, float opacity) {
        if (opacity <= 0) return;
        const auto source = D2D1::RectF(0, 0, frame.decoration_bounds.right - frame.decoration_bounds.left,
            frame.decoration_bounds.bottom - frame.decoration_bounds.top);
        if (frame.decoration_bitmap.get()) dc->DrawBitmap(frame.decoration_bitmap.get(), &frame.decoration_bounds,
            opacity, D2D1_INTERPOLATION_MODE_LINEAR, &source, nullptr);
        else Image(dc, frame.decoration.get(), opacity);
    }

    float NewTextOpacity() const { return std::max(0.0f, linear_ * 2.0f - 1.0f); }
    float OldTextOpacity() const { return std::max(0.0f, 1.0f - linear_ * 2.0f); }
    static float Extent(const Picture& picture) {
        return std::max(picture.image_bounds.right - picture.image_bounds.left,
            picture.image_bounds.bottom - picture.image_bounds.top);
    }
    static void ShareImage(Picture& destination, const Picture& source) {
        auto* image = source.commands.get();
        if (image) image->AddRef();
        destination.commands.reset();
        destination.commands.p = image;
        destination.image_bounds = source.image_bounds;
        auto* bitmap = source.bitmap.get();
        if (bitmap) bitmap->AddRef();
        destination.bitmap.reset(); destination.bitmap.p = bitmap;
        destination.bitmap_source = source.bitmap_source;
    }
    static bool Intersects(D2D1_RECT_F a, D2D1_RECT_F b) {
        return a.left < b.right && a.right > b.left && a.top < b.bottom && a.bottom > b.top;
    }
    static D2D1_RECT_F Mix(D2D1_RECT_F a, D2D1_RECT_F b, float t) {
        return {a.left + (b.left - a.left) * t, a.top + (b.top - a.top) * t,
            a.right + (b.right - a.right) * t, a.bottom + (b.bottom - a.bottom) * t};
    }
    template<class Draw>
    static bool Record(ID2D1DeviceContext* dc, ComPtr<ID2D1CommandList>& commands, Draw draw) {
        if (FAILED(dc->CreateCommandList(&commands))) return false;
        ComPtr<ID2D1Image> target;
        dc->GetTarget(&target);
        dc->SetTarget(commands.get());
        draw();
        dc->SetTarget(target.get());
        return SUCCEEDED(commands->Close());
    }
    static void Image(ID2D1DeviceContext* dc, ID2D1Image* image, float opacity) {
        if (!image || opacity <= 0) return;
        const bool faded = opacity < 1;
        if (faded) dc->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), nullptr,
            D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(), opacity), nullptr);
        dc->DrawImage(image);
        if (faded) dc->PopLayer();
    }
    static void DrawPicture(ID2D1DeviceContext* dc, const Picture& picture, D2D1_RECT_F bounds, float opacity,
                            bool picture_opacity = true) {
        const auto original = picture.image_bounds;
        if (original.right <= original.left || original.bottom <= original.top || opacity <= 0) return;
        if (picture.bitmap.get()) {
            dc->DrawBitmap(picture.bitmap.get(), &bounds, opacity * (picture_opacity ? picture.opacity : 1.0f),
                D2D1_INTERPOLATION_MODE_LINEAR, &picture.bitmap_source, nullptr);
            return;
        }
        D2D1_MATRIX_3X2_F previous;
        dc->GetTransform(&previous);
        dc->SetTransform(D2D1::Matrix3x2F::Translation(-original.left, -original.top) *
            D2D1::Matrix3x2F::Scale((bounds.right - bounds.left) / (original.right - original.left),
                (bounds.bottom - bounds.top) / (original.bottom - original.top)) *
            D2D1::Matrix3x2F::Translation(bounds.left, bounds.top) * previous);
        Image(dc, picture.commands.get(), opacity * (picture_opacity ? picture.opacity : 1.0f));
        dc->SetTransform(previous);
    }
};

} // namespace pulse::ui
