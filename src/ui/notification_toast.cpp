// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "typography.h"
#include "notification_toast.h"
#include "animation_clock.h"
#include <windowsx.h>
#include <algorithm>

namespace pulse::ui {
namespace {
bool Contains(const D2D1_RECT_F& rect, float x, float y) {
    return x >= rect.left && x < rect.right && y >= rect.top && y < rect.bottom;
}
}
void NotificationToast::Show(HWND owner, std::wstring title, std::wstring message, bool persistent, UINT action_message) {
    title_ = std::move(title);
    message_ = std::move(message);
    persistent_ = persistent;
    top_center_ = false;
    kind_ = persistent ? fluent::InfoBarKind::Warning : fluent::InfoBarKind::Informational;
    action_message_ = action_message;
    visible_ = true;
    hovered_ = pressed_close_ = pressed_card_ = false;
    born_ = last_tick_ = GetTickCount64();
    reveal_.Reset();
    reveal_.Tick(true, AnimationTimeMs(), motion_enabled_);
    remaining_ = 6000;
    // The owner schedules reveal frames; this timer only measures lifetime.
    if (persistent_) KillTimer(owner, timer);
    else SetTimer(owner, timer, 200, nullptr);
    InvalidateRect(owner, nullptr, FALSE);
}
void NotificationToast::ShowError(HWND owner, std::wstring title, std::wstring message) {
    Show(owner, std::move(title), std::move(message), false);
    top_center_ = true;
    kind_ = fluent::InfoBarKind::Error;
}
void NotificationToast::Dismiss(HWND owner) {
    visible_ = false;
    pressed_close_ = pressed_card_ = hovered_ = false;
    KillTimer(owner, timer);
    InvalidateRect(owner, nullptr, FALSE);
}
bool NotificationToast::HandleMessage(HWND owner, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_DESTROY) { KillTimer(owner, timer); return false; }
    if (!visible_) return false;
    if (message == WM_KEYDOWN && wparam == VK_ESCAPE) {
        Dismiss(owner);
        return true;
    }
    if (message == WM_TIMER && wparam == timer) {
        const auto now = GetTickCount64();
        if (!persistent_ && !hovered_) {
            const auto elapsed = now - last_tick_;
            remaining_ = elapsed >= remaining_ ? 0 : remaining_ - elapsed;
            if (!remaining_) { Dismiss(owner); return true; }
        }
        last_tick_ = now;
        return true;
    }
    const float x = static_cast<float>(GET_X_LPARAM(lparam));
    const float y = static_cast<float>(GET_Y_LPARAM(lparam));
    if (message == WM_MOUSEMOVE) {
        const bool hovered = Contains(card_, x, y);
        if (hovered != hovered_) {
            hovered_ = hovered;
            last_tick_ = GetTickCount64();
            InvalidateRect(owner, nullptr, FALSE);
        }
        if (hovered_) {
            TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, owner, 0};
            TrackMouseEvent(&tracking);
            InvalidateRect(owner, nullptr, FALSE);
        }
        if (pressed_card_ || hovered_) return true;
    } else if (message == WM_MOUSELEAVE) {
        hovered_ = false;
        last_tick_ = GetTickCount64();
    } else if (message == WM_LBUTTONDOWN && Contains(card_, x, y)) {
        pressed_card_ = true;
        pressed_close_ = Contains(close_, x, y);
        SetCapture(owner);
        return true;
    } else if (message == WM_LBUTTONUP && (pressed_card_ || Contains(card_, x, y))) {
        const bool close = pressed_close_ && Contains(close_, x, y);
        const bool action = pressed_card_ && !pressed_close_ && Contains(card_, x, y) && action_message_;
        pressed_close_ = pressed_card_ = false;
        if (GetCapture() == owner) ReleaseCapture();
        if (close) Dismiss(owner);
        else if (action) {
            PostMessageW(owner, action_message_, 0, 0);
            Dismiss(owner);
        }
        return true;
    } else if (message == WM_CAPTURECHANGED) {
        pressed_close_ = pressed_card_ = false;
    } else if ((message == WM_LBUTTONDBLCLK || message == WM_RBUTTONDOWN || message == WM_RBUTTONUP) &&
               Contains(card_, x, y)) {
        return true;
    }
    return false;
}
void NotificationToast::Draw(Compositor& compositor, const Theme& theme, float scale, bool high_contrast) {
    if (!visible_) return;
    painter_.SetCompositor(&compositor);
    painter_.SetScale(scale);
    if (!painter_.BeginFrame(theme, high_contrast)) return;
    const float margin = 16.0f * scale;
    const float width = std::min(420.0f * scale, compositor.Width() - 2 * margin);
    if (width < 100.0f * scale) return;
    float body_height = 40.0f * scale;
    const float close_inset = top_center_ ? 12.0f : 2.0f;
    ComPtr<IDWriteTextLayout> layout;
    if (SUCCEEDED(typography::CreateTextLayout(compositor.DwriteFactory(), message_.c_str(),
            static_cast<UINT32>(message_.size()), compositor.SmallFormat(),
            width - (82 + close_inset) * scale, 400 * scale, &layout))) {
        layout->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
        DWRITE_TEXT_METRICS metrics{};
        if (SUCCEEDED(layout->GetMetrics(&metrics))) body_height = metrics.height;
    }
    const float height = std::min(std::max(72 * scale, body_height + 44 * scale),
        std::max(72 * scale, compositor.Height() - 110 * scale));
    reveal_.Sample(AnimationTimeMs(), motion_enabled_ && !high_contrast);
    const float opacity = reveal_.opacity;
    const float slide = 6 * scale * (1 - opacity);
    const float right = top_center_ ? (compositor.Width() + width) * 0.5f : compositor.Width() - margin;
    const float top = top_center_ ? 64 * scale - slide : compositor.Height() - 48 * scale + slide - height;
    card_ = D2D1::RectF(right - width, top, right, top + height);
    const float center = (card_.top + card_.bottom) * 0.5f;
    close_ = D2D1::RectF(right - (28 + close_inset) * scale, center - 14 * scale,
        right - close_inset * scale, center + 14 * scale);
    fluent::InfoBarSpec spec;
    spec.bounds = card_;
    spec.title = title_;
    spec.message = message_;
    spec.kind = kind_;
    spec.show_icon_background = !top_center_;
    spec.close_inset = close_inset;
    spec.show_close = true;
    POINT cursor{};
    if (GetCursorPos(&cursor) && ScreenToClient(compositor.Hwnd(), &cursor))
        spec.state.hovered = Contains(close_, static_cast<float>(cursor.x), static_cast<float>(cursor.y));
    const bool fading = opacity < 1.0f;
    auto* dc = compositor.Dc();
    if (fading) dc->PushLayer(D2D1::LayerParameters(card_, nullptr,
        D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::Matrix3x2F::Identity(), opacity), nullptr);
    painter_.DrawInfoBar(spec);
    if (fading) dc->PopLayer();
}
}
