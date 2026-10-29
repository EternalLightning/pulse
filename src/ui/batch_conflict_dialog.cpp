#include "batch_conflict_dialog.h"
#include "conflict_details.h"
#include "animation_clock.h"
#include "../common/text_format.h"
#include <windowsx.h>
#include <algorithm>
#include <set>

namespace pulse::ui {
namespace {
constexpr wchar_t kClass[] = L"PulseBatchFileConflictDialog";
constexpr UINT_PTR kValidityTimer = 1, kScrollTimer = 2;
constexpr fluent::MotionSpec kScrollMotion{160.0f, fluent::EasingCurve::OutCubic};
constexpr ops::ConflictChoice kChoices[]{ops::ConflictChoice::Replace, ops::ConflictChoice::Skip, ops::ConflictChoice::KeepBoth};
std::wstring Leaf(std::wstring value) {
    while (value.size() > 1 && (value.back() == L'\\' || value.back() == L'/')) value.pop_back();
    const size_t slash = value.find_last_of(L"\\/");
    return slash == std::wstring::npos ? value : value.substr(slash + 1);
}
}

BatchConflictDialog::~BatchConflictDialog() { Destroy(); }

bool BatchConflictDialog::Valid() const {
    if (!conflict_.token || conflict_.link_confirmation || conflict_.rows.size() < 2 ||
        (conflict_.still_valid && !conflict_.still_valid())) return false;
    std::set<uint64_t> ids;
    for (const auto& row : conflict_.rows) if (!row.row_id || !ids.insert(row.row_id).second) return false;
    return true;
}

bool BatchConflictDialog::Initialize(HWND owner, const ops::ConflictItemInfo& conflict,
    const BatchConflictLabels& labels, bool dark, D2D1_COLOR_F accent, bool interactive) {
    owner_ = owner; conflict_ = conflict; labels_ = labels; dark_ = dark; accent_ = accent;
    done_ = false; result_.reset(); selections_.clear();
    if (!Valid()) return false;
    for (const auto& row : conflict_.rows) selections_.push_back({row.row_id, ops::ConflictChoice::KeepBoth});
    scale_ = GetDpiForWindow(owner) / 96.0f;
    if (scale_ <= 0) scale_ = 1;
    WNDCLASSEXW wc{sizeof(wc)};
    wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = kClass;
    wc.lpfnWndProc = WndProc; wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    if (!GetClassInfoExW(wc.hInstance, kClass, &wc) && !RegisterClassExW(&wc)) return false;
    MONITORINFO monitor{sizeof(monitor)};
    if (!GetMonitorInfoW(MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST), &monitor)) return false;
    const int width = (std::min)(static_cast<int>(760 * scale_), static_cast<int>(monitor.rcWork.right - monitor.rcWork.left));
    const int height = (std::min)(static_cast<int>(720 * scale_), static_cast<int>(monitor.rcWork.bottom - monitor.rcWork.top));
    hwnd_ = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP | (interactive ? 0 : WS_EX_NOACTIVATE), kClass,
        labels_.title.c_str(), WS_POPUP | WS_THICKFRAME | WS_SYSMENU,
        interactive ? monitor.rcWork.left : -20000, interactive ? monitor.rcWork.top : -20000,
        width, height, owner, nullptr, wc.hInstance, this);
    if (!hwnd_) return false;
    backdrop_ = ApplyBackdrop(hwnd_, dark_);
    if (!compositor_.Init(hwnd_)) { Destroy(); return false; }
    compositor_.RecreateTextFormats(scale_);
    painter_.SetCompositor(&compositor_); painter_.SetScale(scale_);
    if (interactive) CenterOwnedWindow(hwnd_, owner_, width, height);
    RefreshMotion();
    Layout();
    return true;
}

std::optional<std::vector<ops::ConflictResolution>> BatchConflictDialog::Show(HWND owner,
    const ops::ConflictItemInfo& conflict, const BatchConflictLabels& labels, bool dark, D2D1_COLOR_F accent) {
    if (!Initialize(owner, conflict, labels, dark, accent, true)) return std::nullopt;
    if (!Valid()) { Destroy(); return std::nullopt; }
    OwnerDimScope dim(owner_);
    owner_disabled_ = owner_ && IsWindowEnabled(owner_);
    if (owner_disabled_) EnableWindow(owner_, FALSE);
    SetTimer(hwnd_, kValidityTimer, 100, nullptr);
    ShowDialogWithFade(hwnd_);
    SetForegroundWindow(hwnd_); SetFocus(hwnd_);
    MSG message{};
    while (!done_ && GetModalMessage(message)) { TranslateMessage(&message); DispatchMessageW(&message); }
    if (!done_) result_.reset();
    const auto result = result_;
    Destroy();
    return result;
}

void BatchConflictDialog::Destroy() {
    StopScroll();
    if (hwnd_ && IsWindow(hwnd_)) {
        KillTimer(hwnd_, kValidityTimer);
        HideComposedDialog(hwnd_, owner_disabled_ ? owner_ : nullptr);
        DestroyWindow(hwnd_);
    }
    hwnd_ = nullptr;
    if (owner_disabled_ && IsWindow(owner_)) EnableWindow(owner_, TRUE);
    owner_disabled_ = false;
    compositor_.Shutdown();
}

void BatchConflictDialog::Layout() {
    controls_.clear(); rows_.clear();
    const float s = scale_, width = static_cast<float>(compositor_.Width()), height = static_cast<float>(compositor_.Height());
    if (width <= 0 || height <= 0) return;
    const std::wstring names[]{labels_.replace, labels_.skip, labels_.keep_both};
    const float pad = 20 * s, gap = 8 * s, end = width - pad;
    const float row_inner_width = (std::max)(1.0f, width - 2 * pad - 24 * s);
    float choice_widths[3]{};
    for (size_t i = 0; i < 3; ++i)
        choice_widths[i] = (std::min)(painter_.MeasureButtonWidth(names[i]) + 12 * s, row_inner_width);
    auto add_choices = [&](float right, float top, float available, Action action, size_t row) {
        size_t first = 0;
        while (first < 3) {
            size_t last = first + 1;
            float line_width = choice_widths[first];
            while (last < 3 && line_width + gap + choice_widths[last] <= available) {
                line_width += gap + choice_widths[last++];
            }
            float x = right - line_width;
            for (size_t i = first; i < last; ++i) {
                controls_.push_back({D2D1::RectF(x, top, x + choice_widths[i], top + 32 * s), action, row, kChoices[i], names[i]});
                x += choice_widths[i] + gap;
            }
            first = last;
            if (first < 3) top += 40 * s;
        }
        return top + 32 * s;
    };
    const float total_choices = choice_widths[0] + choice_widths[1] + choice_widths[2] + 2 * gap;
    const float label_width = painter_.MeasureButtonWidth(labels_.apply_all);
    const bool inline_apply = label_width + 24 * s + total_choices <= width - 2 * pad;
    const float actions_bottom = add_choices(end, (inline_apply ? 100 : 120) * s, width - 2 * pad, Action::Apply, 0);
    header_height_ = actions_bottom + 20 * s;
    footer_height_ = 64 * s;
    viewport_ = D2D1::RectF(pad, header_height_, width - 10 * s, (std::max)(header_height_ + 1, height - footer_height_));
    const bool columns = width >= 620 * s;
    const size_t first_row_control = controls_.size();
    const float row_actions_top = (42 + (columns ? 104 : 208) + 12) * s;
    row_height_ = add_choices(end - 12 * s, row_actions_top, row_inner_width, Action::Select, 0) + 12 * s;
    controls_.resize(first_row_control);
    content_height_ = static_cast<float>(conflict_.rows.size()) * (row_height_ + 10 * s);
    scroll_ = std::clamp(scroll_, 0.0f, (std::max)(0.0f, content_height_ - (viewport_.bottom - viewport_.top)));
    for (size_t row = 0; row < conflict_.rows.size(); ++row) {
        const float top = viewport_.top + static_cast<float>(row) * (row_height_ + 10 * s) - scroll_;
        rows_.push_back(D2D1::RectF(pad, top, width - pad, top + row_height_));
        add_choices(end - 12 * s, top + row_actions_top, row_inner_width, Action::Select, row);
    }
    const float button_y = height - footer_height_ + 16 * s;
    const float max_footer_width = (std::max)(1.0f, (width - 2 * pad - gap) / 2);
    const float cancel_width = (std::min)(max_footer_width, painter_.MeasureButtonWidth(labels_.cancel) + 12 * s);
    const float confirm_width = (std::min)(max_footer_width, painter_.MeasureButtonWidth(labels_.confirm) + 12 * s);
    controls_.push_back({D2D1::RectF(end - cancel_width, button_y, end, button_y + 32 * s), Action::Cancel, 0, {}, labels_.cancel});
    controls_.push_back({D2D1::RectF(end - cancel_width - gap - confirm_width, button_y,
        end - cancel_width - gap, button_y + 32 * s), Action::Confirm, 0, {}, labels_.confirm});
    if (focus_ < 0 || focus_ >= static_cast<int>(controls_.size()) + 1) focus_ = static_cast<int>(controls_.size()) - 2;
    controls_.push_back({D2D1::RectF(width - 46 * s, 0, width, 44 * s), Action::Close, 0, {}, labels_.cancel});
}

D2D1_RECT_F BatchConflictDialog::DetailsRect(const D2D1_RECT_F& row) const {
    const float top = row.top + 42 * scale_;
    const bool columns = compositor_.Width() >= 620 * scale_;
    return D2D1::RectF(row.left + 12 * scale_, top, row.right - 12 * scale_, top + (columns ? 104 : 208) * scale_);
}

std::wstring BatchConflictDialog::Metadata(const ops::ConflictRowInfo& row, bool source) const {
    const bool directory = source ? row.source_is_directory : row.destination_is_directory;
    const bool reparse = source ? row.source_is_reparse : row.destination_is_reparse;
    std::wstring value = directory ? labels_.folder : format::ByteSize(source ? row.source_size : row.destination_size);
    if (reparse) value += L" · " + labels_.reparse;
    value += L" · " + format::LocalFileTime(source ? row.source_modified : row.destination_modified, labels_.unknown.c_str());
    return value;
}

void BatchConflictDialog::Text(const std::wstring& text, const D2D1_RECT_F& bounds, D2D1_COLOR_F color, bool heading) {
    DrawConflictText(compositor_, text, bounds, color, heading, !heading && bounds.bottom - bounds.top > 28 * scale_);
}

bool BatchConflictDialog::Render(bool present) {
    if (!compositor_.Dc()) return false;
    const bool high_contrast = IsHighContrast();
    const auto theme = high_contrast ? MakeHighContrastTheme() : MakeTheme(dark_, accent_);
    const float s = scale_, width = static_cast<float>(compositor_.Width());
    BeginSurface(compositor_, painter_, theme, dark_, high_contrast, backdrop_, s);
    painter_.DrawGlyph(L"\xE8C8", D2D1::RectF(14 * s, 11 * s, 36 * s, 33 * s), theme.accent);
    painter_.DrawText(labels_.title, D2D1::RectF(42 * s, 0, width - 50 * s, 44 * s), compositor_.SmallFormat(), theme.text);
    painter_.FillRoundedRect(D2D1::RectF(0, 43 * s, width, 44 * s), 0, theme.stroke_divider);
    Text(labels_.instruction, D2D1::RectF(20 * s, 60 * s, width - 20 * s, 92 * s), theme.text_secondary);
    const bool inline_apply = controls_[0].bounds.top == 100 * s;
    const float label_top = (inline_apply ? 106 : 96) * s;
    const float label_right = inline_apply ? controls_[0].bounds.left - 16 * s : width - 20 * s;
    Text(labels_.apply_all, D2D1::RectF(20 * s, label_top, label_right, label_top + 20 * s), theme.text_secondary);
    auto* dc = compositor_.Dc();
    dc->PushAxisAlignedClip(viewport_, D2D1_ANTIALIAS_MODE_ALIASED);
    const bool columns = width >= 620 * s;
    for (size_t i = 0; i < rows_.size(); ++i) {
        const auto& bounds = rows_[i];
        if (bounds.bottom < viewport_.top || bounds.top > viewport_.bottom) continue;
        painter_.FillRoundedRect(bounds, theme.radius_control * s, theme.surface_card);
        painter_.StrokeRoundedRect(bounds, theme.radius_control * s, theme.stroke_card);
        const auto& row = conflict_.rows[i];
        Text(Leaf(row.destination), D2D1::RectF(bounds.left + 12 * s, bounds.top + 12 * s,
            bounds.right - 12 * s, bounds.top + 36 * s), theme.text, true);
        const auto source_meta = Metadata(row, true), target_meta = Metadata(row, false);
        DrawConflictDetails(painter_, compositor_, DetailsRect(bounds), s, theme, dark_, columns,
            {labels_.source, source_meta, row.source}, {labels_.destination, target_meta, row.destination});
    }
    for (size_t i = 0; i < controls_.size(); ++i) {
        const auto& control = controls_[i];
        if (control.action != Action::Select || control.bounds.bottom <= viewport_.top || control.bounds.top >= viewport_.bottom) continue;
        fluent::ButtonSpec button;
        button.bounds = control.bounds; button.text = control.text; button.kind = fluent::ButtonKind::Toggle;
        button.state.checked = selections_[control.row].choice == control.choice;
        button.state.hovered = static_cast<int>(i) == hover_; button.state.pressed = static_cast<int>(i) == pressed_;
        button.state.keyboard_focus = static_cast<int>(i) == focus_;
        painter_.DrawButton(button);
    }
    if (content_height_ > viewport_.bottom - viewport_.top) {
        fluent::ScrollbarSpec scrollbar;
        scrollbar.viewport = viewport_; scrollbar.offset = scroll_;
        scrollbar.viewport_extent = viewport_.bottom - viewport_.top; scrollbar.content_extent = content_height_;
        painter_.DrawScrollbar(scrollbar);
    }
    dc->PopAxisAlignedClip();
    const float footer_top = static_cast<float>(compositor_.Height()) - footer_height_;
    painter_.FillRoundedRect(D2D1::RectF(0, footer_top, width, footer_top + s), 0, theme.stroke_divider);
    for (size_t i = 0; i < controls_.size(); ++i) {
        const auto& control = controls_[i];
        if (control.action == Action::Select) continue;
        if (control.action == Action::Close) {
            fluent::ControlState state;
            state.hovered = static_cast<int>(i) == hover_; state.pressed = static_cast<int>(i) == pressed_;
            state.keyboard_focus = static_cast<int>(i) == focus_;
            painter_.DrawTitleBarButton(control.bounds, fluent::TitleBarButtonRole::Close, {}, state);
            continue;
        }
        fluent::ButtonSpec button;
        button.bounds = control.bounds; button.text = control.text;
        button.kind = control.action == Action::Confirm ? fluent::ButtonKind::Primary :
            control.action == Action::Apply ? fluent::ButtonKind::Toggle : fluent::ButtonKind::Standard;
        button.state.checked = control.action == Action::Apply && std::all_of(selections_.begin(), selections_.end(),
            [&](const auto& selected) { return selected.choice == control.choice; });
        button.state.hovered = static_cast<int>(i) == hover_; button.state.pressed = static_cast<int>(i) == pressed_;
        button.state.keyboard_focus = static_cast<int>(i) == focus_;
        painter_.DrawButton(button);
    }
    const HRESULT hr = dc->EndDraw();
    if (FAILED(hr)) { compositor_.NotifyDeviceLost(hr); return false; }
    if (present) compositor_.Present();
    return true;
}

void BatchConflictDialog::SetScrollOffset(float value) {
    const float previous = scroll_;
    scroll_ = std::clamp(value, 0.0f, (std::max)(0.0f, content_height_ - (viewport_.bottom - viewport_.top)));
    if (scroll_ == previous) return;
    const float delta = previous - scroll_;
    for (auto& row : rows_) { row.top += delta; row.bottom += delta; }
    for (auto& control : controls_) if (control.action == Action::Select) {
        control.bounds.top += delta; control.bounds.bottom += delta;
    }
    hover_ = pressed_ = -1;
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void BatchConflictDialog::StopScroll() {
    scroll_animating_ = false;
    scroll_from_ = scroll_target_ = scroll_;
    if (hwnd_) KillTimer(hwnd_, kScrollTimer);
}

void BatchConflictDialog::RefreshMotion() {
    BOOL enabled = TRUE;
    SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &enabled, 0);
    animations_enabled_ = enabled && !IsHighContrast();
    if (!animations_enabled_ && scroll_animating_) {
        SetScrollOffset(scroll_target_);
        StopScroll();
    }
}

bool BatchConflictDialog::TickScroll(uint64_t now) {
    if (!scroll_animating_) return false;
    const float previous = scroll_;
    const float elapsed = static_cast<float>(now >= scroll_started_ ? now - scroll_started_ : 0);
    const float progress = fluent::EvaluateMotion(kScrollMotion, elapsed);
    SetScrollOffset(progress >= 1 ? scroll_target_ : scroll_from_ + (scroll_target_ - scroll_from_) * progress);
    if (progress >= 1) StopScroll();
    return scroll_ != previous;
}

void BatchConflictDialog::Scroll(float delta, bool smooth) {
    const uint64_t now = AnimationTimeMs();
    if (smooth) TickScroll(now);
    const bool continuing = smooth && scroll_animating_ && (scroll_target_ - scroll_) * delta > 0;
    const float target = std::clamp((continuing ? scroll_target_ : scroll_) + delta, 0.0f,
        (std::max)(0.0f, content_height_ - (viewport_.bottom - viewport_.top)));
    if (!smooth || !animations_enabled_) {
        StopScroll();
        SetScrollOffset(target);
        scroll_from_ = scroll_target_ = scroll_;
        return;
    }
    const bool timer_running = scroll_animating_;
    scroll_from_ = scroll_; scroll_target_ = target; scroll_started_ = now;
    scroll_animating_ = scroll_target_ != scroll_;
    if (!scroll_animating_) { StopScroll(); return; }
    if (!timer_running && !SetTimer(hwnd_, kScrollTimer, USER_TIMER_MINIMUM, nullptr)) {
        SetScrollOffset(target);
        StopScroll();
    }
}
void BatchConflictDialog::ScrollAt(float y) {
    const float height = (std::max)(1.0f, viewport_.bottom - viewport_.top);
    Scroll((y - viewport_.top) / height * content_height_ - height * 0.5f - scroll_);
}
void BatchConflictDialog::Focus(int direction) {
    StopScroll();
    if (controls_.empty()) return;
    focus_ = (focus_ + direction + static_cast<int>(controls_.size())) % static_cast<int>(controls_.size());
    const auto control = controls_[focus_];
    if (control.action == Action::Select) {
        if (control.bounds.top < viewport_.top) Scroll(control.bounds.top - viewport_.top - 6 * scale_);
        else if (control.bounds.bottom > viewport_.bottom) Scroll(control.bounds.bottom - viewport_.bottom + 6 * scale_);
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}
int BatchConflictDialog::Hit(float x, float y) const {
    for (size_t i = 0; i < controls_.size(); ++i) {
        const auto& control = controls_[i];
        if (control.action == Action::Select && (y < viewport_.top || y >= viewport_.bottom)) continue;
        if (ContainsRect(control.bounds, x, y)) return static_cast<int>(i);
    }
    return -1;
}
void BatchConflictDialog::Invoke(int index) {
    if (index < 0 || index >= static_cast<int>(controls_.size()) || done_) return;
    if (!Valid()) { Complete(false); return; }
    const auto control = controls_[index];
    if (control.action == Action::Confirm) { Complete(true); return; }
    if (control.action == Action::Cancel || control.action == Action::Close) { Complete(false); return; }
    if (control.action == Action::Apply) for (auto& row : selections_) row.choice = control.choice;
    else if (control.row < selections_.size()) selections_[control.row].choice = control.choice;
    InvalidateRect(hwnd_, nullptr, FALSE);
}
void BatchConflictDialog::Complete(bool accepted) {
    result_ = accepted && Valid() ? std::optional{selections_} : std::nullopt;
    done_ = true;
    StopScroll();
    if (hwnd_) KillTimer(hwnd_, kValidityTimer);
}

LRESULT CALLBACK BatchConflictDialog::WndProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* self = reinterpret_cast<BatchConflictDialog*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        self = static_cast<BatchConflictDialog*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
        self->hwnd_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (!self) return DefWindowProcW(hwnd, message, wparam, lparam);
    switch (message) {
    case WM_NCCALCSIZE: return 0;
    case WM_NCHITTEST: return BorderlessHitTest(hwnd, lparam, 44 * self->scale_,
        D2D1::RectF(static_cast<float>(self->compositor_.Width()) - 46 * self->scale_, 0,
            static_cast<float>(self->compositor_.Width()), 44 * self->scale_));
    case WM_ERASEBKGND: return 1;
    case WM_GETMINMAXINFO: {
        MONITORINFO monitor{sizeof(monitor)};
        if (GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &monitor)) {
            auto* limits = reinterpret_cast<MINMAXINFO*>(lparam);
            limits->ptMinTrackSize.x = (std::min)(static_cast<LONG>(340 * self->scale_), monitor.rcWork.right - monitor.rcWork.left);
            limits->ptMinTrackSize.y = (std::min)(static_cast<LONG>(360 * self->scale_), monitor.rcWork.bottom - monitor.rcWork.top);
        }
        return 0;
    }
    case WM_SIZE:
        if (self->compositor_.Dc()) { self->StopScroll(); self->compositor_.Resize(LOWORD(lparam), HIWORD(lparam)); self->Layout(); }
        return 0;
    case WM_DPICHANGED: {
        self->StopScroll();
        self->scale_ = HIWORD(wparam) / 96.0f;
        self->compositor_.RecreateTextFormats(self->scale_); self->painter_.SetScale(self->scale_);
        const auto* bounds = reinterpret_cast<RECT*>(lparam);
        SetWindowPos(hwnd, nullptr, bounds->left, bounds->top, bounds->right - bounds->left, bounds->bottom - bounds->top,
            SWP_NOACTIVATE | SWP_NOZORDER); self->Layout(); return 0;
    }
    case WM_SETTINGCHANGE: case WM_THEMECHANGED:
        self->RefreshMotion();
        self->backdrop_ = ApplyBackdrop(hwnd, self->dark_); InvalidateRect(hwnd, nullptr, FALSE); return 0;
    case WM_TIMER:
        if (wparam == kValidityTimer && !self->Valid()) self->Complete(false);
        else if (wparam == kScrollTimer && !self->done_ && self->TickScroll(AnimationTimeMs())) {
            self->Render();
            ValidateRect(hwnd, nullptr);
        }
        return 0;
    case WM_CLOSE: self->Complete(false); return 0;
    case WM_GETDLGCODE: return DLGC_WANTALLKEYS;
    case WM_KEYDOWN:
        if (wparam == VK_ESCAPE) self->Complete(false);
        else if (wparam == VK_TAB) self->Focus(GetKeyState(VK_SHIFT) < 0 ? -1 : 1);
        else if (wparam == VK_RETURN || wparam == VK_SPACE) self->Invoke(self->focus_);
        else if (wparam == VK_DOWN) self->Scroll(60 * self->scale_, true);
        else if (wparam == VK_UP) self->Scroll(-60 * self->scale_, true);
        else if (wparam == VK_NEXT) self->Scroll((self->viewport_.bottom - self->viewport_.top) * 0.8f, true);
        else if (wparam == VK_PRIOR) self->Scroll(-(self->viewport_.bottom - self->viewport_.top) * 0.8f, true);
        else if (wparam == VK_HOME) self->Scroll(-self->content_height_);
        else if (wparam == VK_END) self->Scroll(self->content_height_);
        return 0;
    case WM_MOUSEWHEEL: self->Scroll(-GET_WHEEL_DELTA_WPARAM(wparam) / static_cast<float>(WHEEL_DELTA) * 72 * self->scale_, true); return 0;
    case WM_MOUSEMOVE:
        if (self->dragging_) { self->ScrollAt(static_cast<float>(GET_Y_LPARAM(lparam))); return 0; }
        { TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, hwnd, 0}; TrackMouseEvent(&track); }
        self->hover_ = self->Hit(static_cast<float>(GET_X_LPARAM(lparam)), static_cast<float>(GET_Y_LPARAM(lparam)));
        InvalidateRect(hwnd, nullptr, FALSE); return 0;
    case WM_MOUSELEAVE: self->hover_ = -1; InvalidateRect(hwnd, nullptr, FALSE); return 0;
    case WM_LBUTTONDOWN:
        self->StopScroll();
        if (GET_X_LPARAM(lparam) >= self->viewport_.right - 10 * self->scale_ &&
            GET_Y_LPARAM(lparam) >= self->viewport_.top && GET_Y_LPARAM(lparam) < self->viewport_.bottom &&
            self->content_height_ > self->viewport_.bottom - self->viewport_.top) {
            self->dragging_ = true; SetCapture(hwnd); self->ScrollAt(static_cast<float>(GET_Y_LPARAM(lparam))); return 0;
        }
        self->pressed_ = self->Hit(static_cast<float>(GET_X_LPARAM(lparam)), static_cast<float>(GET_Y_LPARAM(lparam)));
        if (self->pressed_ >= 0) { self->focus_ = self->pressed_; SetCapture(hwnd); InvalidateRect(hwnd, nullptr, FALSE); } return 0;
    case WM_LBUTTONUP: {
        const int pressed = self->pressed_;
        self->pressed_ = -1; self->dragging_ = false;
        if (GetCapture() == hwnd) ReleaseCapture();
        if (pressed >= 0 && pressed == self->Hit(static_cast<float>(GET_X_LPARAM(lparam)), static_cast<float>(GET_Y_LPARAM(lparam)))) self->Invoke(pressed);
        InvalidateRect(hwnd, nullptr, FALSE); return 0;
    }
    case WM_CAPTURECHANGED: self->pressed_ = -1; self->dragging_ = false; return 0;
    case WM_PAINT: { PAINTSTRUCT paint{}; BeginPaint(hwnd, &paint); self->Render(); EndPaint(hwnd, &paint); return 0; }
    case WM_DESTROY: self->StopScroll(); self->done_ = true; return 0;
    case WM_NCDESTROY: SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0); return DefWindowProcW(hwnd, message, wparam, lparam);
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

std::optional<std::vector<ops::ConflictResolution>> ShowBatchFileConflictDialog(HWND owner,
    const ops::ConflictItemInfo& conflict, const BatchConflictLabels& labels, bool dark, D2D1_COLOR_F accent) {
    BatchConflictDialog dialog;
    return dialog.Show(owner, conflict, labels, dark, accent);
}
} // namespace pulse::ui
