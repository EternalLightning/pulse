// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once
#include "item_reflow_animation.h"
#include "item_reflow_geometry.h"
#include "animation_image_cache.h"
#include "ui_compositor.h"

namespace pulse::ui {

// Only visible layout identities are retained. Deleted entries leave the
// painted list immediately and never participate in hit testing.
class ItemReflowSurface {
public:
    void Reset() {
        animation_.Reset();
        geometry_.Reset();
        pictures_.clear();
        atlas_attempt_.clear();
        image_cache_.Reset();
        slots_.clear();
        context_.clear();
        retained_rows_.clear();
        enabled_ = false;
    }

    void Begin(const std::vector<ItemReflowSlot>& slots, const std::wstring& context,
               D2D1_RECT_F viewport, uint64_t now, bool animate) {
        if (!animate || slots.size() > 512) { Reset(); return; }
        const bool reset = !Matches(context, viewport);
        if (reset) { pictures_.clear(); atlas_attempt_.clear(); }
        context_ = context;
        viewport_ = viewport;
        enabled_ = true;
        slots_ = slots;
        animation_.Update(slots, now, true, reset, &viewport);
        if (Pending()) geometry_.Update(slots_, animation_);
        else {
            geometry_.Reset();
            pictures_.clear();
            atlas_attempt_.clear();
        }
        for (auto it = pictures_.begin(); it != pictures_.end();) {
            if (!animation_.Visuals().contains(it->first)) it = pictures_.erase(it);
            else ++it;
        }
    }

    bool Matches(const std::wstring& context, D2D1_RECT_F viewport) const {
        return enabled_ && context == context_ && Equal(viewport, viewport_);
    }
    bool Pending() const {
        if (!animation_.Pending()) return false;
        if (!slots_.empty()) return true;
        for (const auto& [_, visual] : animation_.Visuals())
            if (visual.exiting && visual.stripe > 0 && visual.opacity > 0) return true;
        return false;
    }
    size_t CachedRows() const { return pictures_.size(); }
    size_t CachedPixelRows() const {
        size_t count = 0;
        for (const auto& [_, picture] : pictures_) count += picture.bitmap.get() != nullptr;
        return count;
    }
    uint64_t PixelCacheBuilds() const { return pixel_cache_builds_; }
    const std::vector<ItemReflowSlot>& Slots() const { return slots_; }
    const std::vector<int>& RetainedRows() const { return retained_rows_; }
    void SetRetainedRows(std::vector<int> rows) { retained_rows_ = std::move(rows); }
    ItemReflowVisual BackgroundVisual(const std::wstring& key, D2D1_RECT_F bounds, float stripe) const {
        if (enabled_) return animation_.Visual(key);
        ItemReflowVisual visual;
        visual.bounds = bounds;
        visual.stripe = stripe;
        return visual;
    }

    template<class Draw>
    void PaintRemovedBackgrounds(ID2D1DeviceContext* dc, Draw draw) const {
        for (const auto& [key, visual] : animation_.Visuals()) {
            if (!visual.exiting || visual.stripe <= 0 || visual.opacity <= 0) continue;
            const auto vacancy = geometry_.RemovedVacancy(visual.bounds);
            if (vacancy.bottom <= vacancy.top) continue;
            dc->PushAxisAlignedClip(vacancy, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
            draw(visual.bounds, visual.stripe * visual.opacity);
            dc->PopAxisAlignedClip();
        }
    }

    template<class Draw>
    void PaintBackground(ID2D1DeviceContext* dc, const std::wstring& key, D2D1_RECT_F bounds,
                         float stripe, Draw draw) const {
        const auto visual = BackgroundVisual(key, bounds, stripe);
        if (visual.opacity <= 0 || visual.stripe <= 0) return;
        const bool reveal = enabled_ && visual.opacity < 1 && Equal(visual.bounds, bounds);
        const auto vacancy = reveal ? Vacancy(key, bounds) : bounds;
        if (reveal && (vacancy.right <= vacancy.left || vacancy.bottom <= vacancy.top)) return;
        if (reveal) dc->PushAxisAlignedClip(vacancy, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        draw(visual.bounds, visual.stripe * visual.opacity);
        if (reveal) dc->PopAxisAlignedClip();
    }

    void SetRevision(const void* snapshot, const void* filter, size_t count, uint64_t generation) {
        if (!SameRevision(snapshot, filter, count, generation)) {
            pictures_.clear(); atlas_attempt_.clear();
        }
        snapshot_ = snapshot; filter_ = filter; count_ = count; generation_ = generation;
    }
    void SetPaintRevision(uint64_t revision) {
        if (paint_revision_ != revision) {
            pictures_.clear();
            atlas_attempt_.clear();
            paint_revision_ = revision;
        }
    }
    bool SameRevision(const void* snapshot, const void* filter, size_t count, uint64_t generation) const {
        return snapshot_ == snapshot && filter_ == filter && count_ == count && generation_ == generation;
    }

    bool ShouldPaint(const std::wstring& key, D2D1_RECT_F bounds) const {
        const auto visible = [&](D2D1_RECT_F rc) {
            return rc.right >= viewport_.left && rc.left <= viewport_.right &&
                rc.bottom >= viewport_.top && rc.top <= viewport_.bottom;
        };
        return !enabled_ || visible(bounds) || visible(animation_.Visual(key).bounds);
    }

    // Map a painted live row back to its final geometry for row actions.
    int HitTest(float& x, float& y) const {
        for (const auto& slot : slots_) {
            const auto visual = animation_.Visual(slot.key);
            const auto rc = visual.bounds;
            if (visual.exiting || visual.opacity <= 0 || x < rc.left || x >= rc.right ||
                y < rc.top || y >= rc.bottom) continue;
            x += slot.bounds.left - visual.bounds.left;
            y += slot.bounds.top - visual.bounds.top;
            return slot.source;
        }
        return -1;
    }

    template<class Draw>
    void Paint(ID2D1DeviceContext* dc, const std::wstring& key, D2D1_RECT_F bounds, Draw draw,
               bool cache = true) {
        if (!enabled_) { draw(); return; }
        const auto visual = animation_.Visual(key);
        if (visual.opacity <= 0) return;
        const bool reveal = visual.opacity < 1 && Equal(visual.bounds, bounds);
        const auto vacancy = reveal ? Vacancy(key, bounds) : bounds;
        if (reveal && (vacancy.right <= vacancy.left || vacancy.bottom <= vacancy.top)) return;
        ID2D1CommandList* commands = nullptr;
        const Picture* picture = nullptr;
        if (cache && Pending()) {
            auto found = pictures_.find(key);
            if (found == pictures_.end()) {
                ComPtr<ID2D1Image> target;
                ComPtr<ID2D1CommandList> incoming;
                dc->GetTarget(&target);
                if (SUCCEEDED(dc->CreateCommandList(&incoming))) {
                    dc->SetTarget(incoming.get());
                    draw();
                    dc->SetTarget(target.get());
                    if (SUCCEEDED(incoming->Close())) {
                        Picture captured;
                        captured.commands = std::move(incoming);
                        captured.bounds = bounds;
                        captured.capture_id = ++capture_id_;
                        found = pictures_.emplace(key, std::move(captured)).first;
                    }
                }
            }
            if (found != pictures_.end()) {
                commands = found->second.commands.get();
                picture = &found->second;
                bounds = found->second.bounds;
            }
        } else pictures_.erase(key);
        D2D1_MATRIX_3X2_F previous;
        dc->GetTransform(&previous);
        // Reveal only the space already opened by neighboring rows. The clip
        // is applied to replay, so cached text remains independent of progress.
        if (reveal) dc->PushAxisAlignedClip(vacancy, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        dc->SetTransform(D2D1::Matrix3x2F::Translation(
            visual.bounds.left - bounds.left, visual.bounds.top - bounds.top) * previous);
        const bool pixels = picture && picture->bitmap.get();
        const bool faded = visual.opacity < 1 && !pixels;
        if (faded) dc->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), nullptr,
            D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(), visual.opacity), nullptr);
        if (pixels) dc->DrawBitmap(picture->bitmap.get(), &bounds, visual.opacity,
            D2D1_INTERPOLATION_MODE_LINEAR, &picture->bitmap_source, nullptr);
        else if (commands) dc->DrawImage(commands);
        else draw();
        if (faded) dc->PopLayer();
        dc->SetTransform(previous);
        if (reveal) dc->PopAxisAlignedClip();
    }

    // Called after all rows have had a chance to capture their stable commands.
    void FinishFrame(ID2D1DeviceContext* dc) {
        if (!enabled_ || !Pending()) return;
        const auto eligible = [&](const std::wstring& key, const Picture& picture) {
            const auto visual = animation_.Visual(key);
            return !visual.exiting && visual.opacity > 0 &&
                (Intersects(picture.bounds, viewport_) || Intersects(visual.bounds, viewport_));
        };
        bool missing = false;
        for (const auto& [key, picture] : pictures_) {
            if (!picture.bitmap.get() && eligible(key, picture)) { missing = true; break; }
        }
        if (!missing) return;
        std::vector<AnimationImageSlice> slices;
        std::vector<Picture*> selected;
        std::vector<uint64_t> capture_ids;
        slices.reserve(pictures_.size());
        selected.reserve(pictures_.size());
        capture_ids.reserve(pictures_.size());
        for (auto& [key, picture] : pictures_) {
            if (!eligible(key, picture)) continue;
            slices.push_back({picture.commands.get(), picture.bounds, {}, {}});
            selected.push_back(&picture);
            capture_ids.push_back(picture.capture_id);
        }
        std::sort(capture_ids.begin(), capture_ids.end());
        if (capture_ids == atlas_attempt_) return;
        atlas_attempt_ = std::move(capture_ids);
        ComPtr<ID2D1Bitmap1> unused_background;
        if (!image_cache_.Build(dc, nullptr, {0, 0, 1, 1}, slices, unused_background)) return;
        // Keep one atlas generation. Offscreen commands remain available for
        // fallback or repacking when their sampled bounds enter the viewport.
        for (auto& [_, picture] : pictures_) picture.bitmap.reset();
        for (size_t i = 0; i < selected.size(); ++i) {
            auto& picture = *selected[i];
            picture.bitmap = std::move(slices[i].bitmap);
            picture.bitmap_source = slices[i].source;
            picture.bitmap_source.right = picture.bitmap_source.left + picture.bounds.right - picture.bounds.left;
            picture.bitmap_source.bottom = picture.bitmap_source.top + picture.bounds.bottom - picture.bounds.top;
        }
        ++pixel_cache_builds_;
    }

private:
    struct Picture {
        ComPtr<ID2D1CommandList> commands;
        D2D1_RECT_F bounds{};
        ComPtr<ID2D1Bitmap1> bitmap;
        D2D1_RECT_F bitmap_source{};
        uint64_t capture_id = 0;
    };
    ItemReflowAnimation animation_;
    ItemReflowGeometry geometry_;
    AnimationImageCache image_cache_;
    std::vector<uint64_t> atlas_attempt_;
    uint64_t capture_id_ = 0;
    uint64_t pixel_cache_builds_ = 0;
    std::unordered_map<std::wstring, Picture> pictures_;
    std::vector<ItemReflowSlot> slots_;
    std::vector<int> retained_rows_;
    std::wstring context_;
    D2D1_RECT_F viewport_{};
    bool enabled_ = false;
    const void* snapshot_ = nullptr;
    const void* filter_ = nullptr;
    size_t count_ = 0;
    uint64_t generation_ = 0;
    uint64_t paint_revision_ = 0;

    static bool Equal(D2D1_RECT_F a, D2D1_RECT_F b) {
        return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
    }
    static bool Intersects(D2D1_RECT_F a, D2D1_RECT_F b) {
        return a.right > b.left && a.left < b.right && a.bottom > b.top && a.top < b.bottom;
    }

    D2D1_RECT_F Vacancy(const std::wstring& key, D2D1_RECT_F bounds) const {
        return geometry_.Vacancy(key, bounds);
    }

};

} // namespace pulse::ui
