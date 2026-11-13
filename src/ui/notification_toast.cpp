// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "typography.h"
#include "notification_toast.h"
#include "file_operation_dialog.h"
#include "../common/localization.h"
#include "animation_clock.h"
#include <windowsx.h>
#include <algorithm>
#include <cwctype>
#include <vector>

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
    overflow_ = details_focus_ = pressed_details_ = false;
    details_ = {};
    born_ = last_tick_ = GetTickCount64();
    reveal_ = {};
    reveal_.Tick(false, AnimationTimeMs(), false, 160.0f);
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
    reveal_.Tick(false, AnimationTimeMs(), motion_enabled_, 140.0f);
    pressed_close_ = pressed_card_ = hovered_ = false;
    details_focus_ = pressed_details_ = false;
    KillTimer(owner, timer);
    InvalidateRect(owner, nullptr, FALSE);
}
void NotificationToast::ShowDetails(HWND owner) {
    ConfirmDialogSpec spec;
    spec.title = title_;
    spec.message = message_;
    spec.confirm_text = l10n::Get(l10n::StringId::Close);
    spec.hide_cancel = true;
    spec.preserve_message = true;
    spec.fit_to_work_area = true;
    spec.max_height_dip = 640.0f;
    Dismiss(owner);
    ShowConfirmDialog(owner, spec, dark_, accent_);
}
bool NotificationToast::HandleMessage(HWND owner, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_DESTROY) { KillTimer(owner, timer); return false; }
    if (!visible_) return false;
    if (message == WM_KILLFOCUS && details_focus_) {
        details_focus_ = false;
        InvalidateRect(owner, nullptr, FALSE);
    }
    if (message == WM_KEYDOWN && overflow_) {
        if (wparam == VK_F6) {
            details_focus_ = !details_focus_;
            last_tick_ = GetTickCount64();
            InvalidateRect(owner, nullptr, FALSE);
            return true;
        }
        if (details_focus_ && (wparam == VK_RETURN || wparam == VK_SPACE)) {
            ShowDetails(owner);
            return true;
        }
        if (details_focus_ && wparam == VK_TAB) {
            details_focus_ = false;
            InvalidateRect(owner, nullptr, FALSE);
        }
    }
    if (message == WM_KEYDOWN && wparam == VK_ESCAPE) {
        Dismiss(owner);
        return true;
    }
    if (message == WM_TIMER && wparam == timer) {
        const auto now = GetTickCount64();
        if (!persistent_ && !hovered_ && !details_focus_) {
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
        pressed_details_ = overflow_ && Contains(details_, x, y);
        SetCapture(owner);
        return true;
    } else if (message == WM_LBUTTONUP && (pressed_card_ || Contains(card_, x, y))) {
        const bool close = pressed_close_ && Contains(close_, x, y);
        const bool details = pressed_details_ && Contains(details_, x, y);
        const bool action = pressed_card_ && !pressed_close_ && !pressed_details_ && Contains(card_, x, y) && action_message_;
        pressed_close_ = pressed_card_ = false;
        pressed_details_ = false;
        if (GetCapture() == owner) ReleaseCapture();
        if (close) Dismiss(owner);
        else if (details) ShowDetails(owner);
        else if (action) {
            PostMessageW(owner, action_message_, 0, 0);
            Dismiss(owner);
        }
        return true;
    } else if (message == WM_CAPTURECHANGED) {
        pressed_close_ = pressed_card_ = false;
        pressed_details_ = false;
    } else if ((message == WM_LBUTTONDBLCLK || message == WM_RBUTTONDOWN || message == WM_RBUTTONUP) &&
               Contains(card_, x, y)) {
        return true;
    }
    return false;
}
void NotificationToast::Draw(Compositor& compositor, const Theme& theme, float scale, bool high_contrast) {
    if (!visible_ && !reveal_.Pending()) return;
    // Start on the first painted frame, not while the owner is still handling the command.
    reveal_.Tick(visible_, AnimationTimeMs(), motion_enabled_ && !high_contrast, visible_ ? 160.0f : 140.0f);
    if (!visible_ && !reveal_.Pending()) return;
    painter_.SetCompositor(&compositor);
    painter_.SetScale(scale);
    if (!painter_.BeginFrame(theme, high_contrast)) return;
    dark_ = 0.2126f * theme.bg.r + 0.7152f * theme.bg.g + 0.0722f * theme.bg.b < 0.5f;
    accent_ = theme.accent;
    const float margin = 16.0f * scale;
    const float width = std::min(420.0f * scale, compositor.Width() - 2 * margin);
    if (width < 100.0f * scale) return;
    float body_height = 40.0f * scale;
    const float close_inset = top_center_ ? 12.0f : 2.0f;
    ComPtr<IDWriteTextLayout> layout;
    if (SUCCEEDED(typography::CreateTextLayout(compositor.DwriteFactory(), message_.c_str(),
            static_cast<UINT32>(message_.size()), compositor.SmallFormat(),
            width - (82 + close_inset) * scale, 1000000 * scale, &layout))) {
        layout->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
        DWRITE_TEXT_METRICS metrics{};
        if (SUCCEEDED(layout->GetMetrics(&metrics))) body_height = metrics.height;
    }
    const float height = std::min(std::min(204 * scale, std::max(72 * scale, body_height + 44 * scale)),
        std::max(72 * scale, compositor.Height() - 110 * scale));
    overflow_ = body_height > height - 36 * scale;
    preview_ = message_;
    if (overflow_ && layout.get()) {
        UINT32 count = 0;
        layout->GetLineMetrics(nullptr, 0, &count);
        std::vector<DWRITE_LINE_METRICS> lines(count);
        if (SUCCEEDED(layout->GetLineMetrics(lines.data(), count, &count))) {
            float used = 0;
            size_t length = 0, last_start = 0;
            for (const auto& line : lines) {
                if (used + line.height > height - 80 * scale) break;
                used += line.height;
                last_start = length;
                length += line.length;
            }
            preview_.resize(length);
            while (!preview_.empty() && iswspace(preview_.back())) preview_.pop_back();
            const float available = width - (82 + close_inset) * scale;
            while (preview_.size() > last_start && typography::MeasureLine(&compositor,
                compositor.SmallFormat(), preview_.substr(last_start) + L"\x2026") > available) {
                const wchar_t removed = preview_.back();
                preview_.pop_back();
                if (removed >= 0xDC00 && removed <= 0xDFFF && !preview_.empty() &&
                    preview_.back() >= 0xD800 && preview_.back() <= 0xDBFF) preview_.pop_back();
            }
            preview_ += L"\x2026";
        }
    }
    if (!overflow_) details_focus_ = false;
    const float opacity = reveal_.value;
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
    spec.message = preview_;
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
    details_ = {};
    if (overflow_) {
        const auto band = D2D1::RectF(card_.left + 4 * scale, card_.bottom - 44 * scale,
            card_.right - 4 * scale, card_.bottom - 4 * scale);
        painter_.FillRoundedRect(band, 0, theme.surface_flyout);
        const auto label = l10n::Get(l10n::StringId::OpDetails) + L" (F6)";
        details_ = painter_.FitButtonBounds(D2D1::RectF(card_.left + 52 * scale,
            card_.bottom - 40 * scale, card_.left + 52 * scale, card_.bottom - 8 * scale), label);
        details_.right = std::min(details_.right, card_.right - 8 * scale);
        fluent::ControlState state;
        state.keyboard_focus = details_focus_;
        state.pressed = pressed_details_;
        state.hovered = GetCursorPos(&cursor) && ScreenToClient(compositor.Hwnd(), &cursor) &&
            Contains(details_, static_cast<float>(cursor.x), static_cast<float>(cursor.y));
        painter_.DrawButton({details_, label, {}, fluent::ButtonKind::Transparent, state});
    }
    if (fading) dc->PopLayer();
}
}
