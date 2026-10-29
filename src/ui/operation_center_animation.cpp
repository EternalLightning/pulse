#include "operation_center.h"
#include "operation_center_layout.h"
#include "animation_clock.h"
#include <algorithm>
#include <cmath>

namespace pulse::ui {
using operation_center_layout::kMargin;
using operation_center_layout::kHeader;
namespace {
constexpr fluent::MotionSpec kScrollMotion{160.0f,fluent::EasingCurve::OutCubic};
float Smooth(float value) {
    value = std::clamp(value, 0.0f, 1.0f);
    return value * value * (3 - 2 * value);
}
}

void OperationCenter::StopScroll() {
    scroll_animating_=false;
    scroll_from_=scroll_target_=scroll_;
}

void OperationCenter::SetScrollOffset(float value) {
    value=std::clamp(value,0.0f,(std::max)(0.0f,content_height_-body_height_));
    if(value==scroll_) return;
    scroll_=value;
    hover_=pressed_=-1;
    Layout();
}

bool OperationCenter::TickScroll(uint64_t now) {
    if(!scroll_animating_) return false;
    const float previous=scroll_;
    const float elapsed=static_cast<float>(now>=scroll_started_ ? now-scroll_started_ : 0);
    const float progress=fluent::EvaluateMotion(kScrollMotion,elapsed);
    SetScrollOffset(progress>=1 ? scroll_target_ : scroll_from_+(scroll_target_-scroll_from_)*progress);
    if(progress>=1) StopScroll();
    return scroll_!=previous;
}

void OperationCenter::Scroll(float delta,bool smooth) {
    if(smooth && (!open_ || closing_ || dragging_scrollbar_)) return;
    const uint64_t now=AnimationTimeMs();
    const bool advanced=smooth && TickScroll(now);
    const bool continuing=smooth && scroll_animating_ && (scroll_target_-scroll_)*delta>0;
    const float target=std::clamp((continuing ? scroll_target_ : scroll_)+delta,0.0f,
        (std::max)(0.0f,content_height_-body_height_));
    if(!smooth || !AnimationsEnabled()) {
        const float previous=scroll_;
        StopScroll();SetScrollOffset(target);StopScroll();
        if(advanced || previous!=scroll_) Redraw();
        return;
    }
    const bool timer_running=scroll_animating_;
    scroll_from_=scroll_;scroll_target_=target;scroll_started_=now;
    scroll_animating_=scroll_target_!=scroll_;
    if(!scroll_animating_) StopScroll();
    else if(!timer_running && !SetTimer(hwnd_,1,kAnimationIntervalMs,nullptr)) {
        SetScrollOffset(target);StopScroll();Redraw();
        return;
    }
    if(advanced) Redraw();
}

void OperationCenter::BeginVisibility(bool closing) {
    closing_ = closing;
    animation_from_ = visibility_;
    animation_start_ = AnimationTimeMs();
    SetTimer(hwnd_, 1, kAnimationIntervalMs, nullptr);
}

void OperationCenter::FinishCardTransition() {
    card_positions_.clear();
    section_positions_.clear();
    departing_cards_.clear();
    cards_animating_ = card_scroll_pending_ = false;
    card_progress_ = 1;
}

void OperationCenter::PrepareCardTransition(const std::vector<ops::OpStatus>& next) {
    if (!open_ || !AnimationsEnabled()) { FinishCardTransition(); return; }
    const auto retained = [&](uint64_t id) {
        return std::any_of(next.begin(), next.end(), [id](const auto& task) { return task.task_id == id; });
    };
    std::erase_if(departing_cards_, [&](const auto& card) { return retained(card.task_id); });
    if (std::none_of(tasks_.begin(), tasks_.end(), [&](const auto& task) { return !retained(task.task_id); })) return;

    StopScroll();
    const float previous_fade = 1 - Smooth(card_progress_ / 0.7f);
    for (auto& card : departing_cards_) {
        card.opacity *= previous_fade;
        card.bounds.left += 12 * scale_ * card_progress_;
        card.bounds.right += 12 * scale_ * card_progress_;
    }
    std::erase_if(departing_cards_, [](const auto& card) { return card.opacity < 0.01f; });
    card_positions_.clear();
    section_positions_.clear();
    const float viewport_top = (kMargin + kHeader) * scale_;
    for (const auto& card : cards_) {
        if (!card.section.empty()) section_positions_.push_back({card.section, card.section_top});
        const auto id = tasks_[card.task].task_id;
        if (retained(id)) { card_positions_.push_back({id, card.bounds.top}); continue; }
        if (!bits_ || !compositor_ || dib_width_ != width_ || dib_height_ != height_) continue;
        const int left = (std::max)(0, static_cast<int>(std::floor(card.bounds.left)));
        const int right = (std::min)(width_, static_cast<int>(std::ceil(card.bounds.right)));
        const int top = (std::max)(static_cast<int>(viewport_top),
            static_cast<int>(std::floor(card.bounds.top + viewport_top - scroll_)));
        const int bottom = (std::min)(static_cast<int>(height_ - kMargin * scale_),
            static_cast<int>(std::ceil(card.bounds.bottom + viewport_top - scroll_)));
        if (right <= left || bottom <= top) continue;
        DepartingCard departing;
        departing.task_id = id;
        departing.bounds = D2D1::RectF(static_cast<float>(left), top - viewport_top + scroll_,
            static_cast<float>(right), bottom - viewport_top + scroll_);
        const auto properties = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
        const auto* pixels = static_cast<const BYTE*>(bits_) + (static_cast<size_t>(top) * width_ + left) * 4;
        if (SUCCEEDED(compositor_->Dc()->CreateBitmap(D2D1::SizeU(right - left, bottom - top),
            pixels, static_cast<UINT32>(width_ * 4), properties, &departing.bitmap)))
            departing_cards_.push_back(std::move(departing));
    }
    cards_animating_ = card_scroll_pending_ = true;
    card_scroll_from_ = scroll_;
    card_progress_ = 0;
    card_animation_start_ = AnimationTimeMs();
}

void OperationCenter::AnimateCardLayout() {
    if (!cards_animating_) return;
    if (card_scroll_pending_) {
        const float delta = scroll_ - card_scroll_from_;
        for (auto& position : card_positions_) position.top += delta;
        for (auto& position : section_positions_) position.top += delta;
        for (auto& card : departing_cards_) { card.bounds.top += delta; card.bounds.bottom += delta; }
        card_scroll_pending_ = false;
    }
    const float remaining = 1 - Smooth((card_progress_ - 0.15f) / 0.85f);
    for (auto& card : cards_) {
        if (!card.section.empty()) {
            const auto section = std::find_if(section_positions_.begin(), section_positions_.end(),
                [&](const auto& position) { return position.label == card.section; });
            if (section != section_positions_.end()) card.section_top += (section->top - card.section_top) * remaining;
        }
        const auto id = tasks_[card.task].task_id;
        const auto found = std::find_if(card_positions_.begin(), card_positions_.end(),
            [id](const auto& position) { return position.task_id == id; });
        if (found == card_positions_.end()) continue;
        const float delta = (found->top - card.bounds.top) * remaining;
        card.bounds.top += delta; card.bounds.bottom += delta;
        for (auto* y : {&card.source_y, &card.destination_y, &card.item_y, &card.error_y,
                       &card.progress_y, &card.statistics_y, &card.footer_y})
            if (*y) *y += delta;
        for (auto& button : buttons_) if (button.action != Action::Clear && button.task_id == id) {
            button.bounds.top += delta; button.bounds.bottom += delta;
        }
    }
}

void OperationCenter::DrawDepartingCards() {
    const float opacity = 1 - Smooth(card_progress_ / 0.7f);
    const float offset = (kMargin + kHeader) * scale_ - scroll_;
    for (const auto& card : departing_cards_) {
        auto bounds = card.bounds;
        bounds.left += 12 * scale_ * card_progress_; bounds.right += 12 * scale_ * card_progress_;
        bounds.top += offset; bounds.bottom += offset;
        compositor_->Dc()->DrawBitmap(card.bitmap.get(), bounds, card.opacity * opacity, D2D1_INTERPOLATION_MODE_LINEAR);
    }
}

} // namespace pulse::ui
