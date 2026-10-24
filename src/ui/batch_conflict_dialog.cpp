#include "typography.h"
#include "batch_conflict_dialog.h"
#include "../common/text_format.h"
#include <windowsx.h>
#include <algorithm>
#include <set>

namespace pulse::ui {
namespace {
constexpr wchar_t kClass[] = L"PulseBatchFileConflictDialog";
constexpr UINT_PTR kValidityTimer = 1;
constexpr ops::ConflictChoice kChoices[]{ops::ConflictChoice::Replace, ops::ConflictChoice::Skip, ops::ConflictChoice::KeepBoth};
std::wstring OneLine(std::wstring value) {
    for (auto& ch : value) if (ch == L'\r' || ch == L'\n' || ch == L'\t') ch = L' ';
    return value;
}
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
    float x = pad, y = 112 * s;
    for (size_t i = 0; i < 3; ++i) {
        if (x + choice_widths[i] > end && x > pad) { x = pad; y += 38 * s; }
        controls_.push_back({D2D1::RectF(x, y, x + choice_widths[i], y + 32 * s), Action::Apply, 0, kChoices[i], names[i]});
        x += choice_widths[i] + gap;
    }
    header_height_ = y + 48 * s;
    footer_height_ = 64 * s;
    viewport_ = D2D1::RectF(pad, header_height_, width - 10 * s, (std::max)(header_height_ + 1, height - footer_height_));
    int action_rows = 1;
    x = 0;
    for (const float choice_width : choice_widths) {
        if (x + choice_width > row_inner_width && x > 0) { ++action_rows; x = 0; }
        x += choice_width + gap;
    }
    const bool columns = width >= 620 * s;
    row_height_ = ((columns ? 108 : 216) + 16 + action_rows * 38) * s;
    content_height_ = static_cast<float>(conflict_.rows.size()) * (row_height_ + 10 * s);
    scroll_ = std::clamp(scroll_, 0.0f, (std::max)(0.0f, content_height_ - (viewport_.bottom - viewport_.top)));
    for (size_t row = 0; row < conflict_.rows.size(); ++row) {
        const float top = viewport_.top + static_cast<float>(row) * (row_height_ + 10 * s) - scroll_;
        rows_.push_back(D2D1::RectF(pad, top, width - pad, top + row_height_));
        x = pad + 12 * s; y = top + (columns ? 108 : 216) * s + 6 * s;
        for (size_t i = 0; i < 3; ++i) {
            if (x + choice_widths[i] > width - pad - 12 * s && x > pad + 12 * s) { x = pad + 12 * s; y += 38 * s; }
            controls_.push_back({D2D1::RectF(x, y, x + choice_widths[i], y + 32 * s), Action::Select, row, kChoices[i], names[i]});
            x += choice_widths[i] + gap;
        }
    }
    const float button_y = height - footer_height_ + 16 * s;
    const float max_footer_width = (std::max)(1.0f, (width - 2 * pad - gap) / 2);
    const float cancel_width = (std::min)(max_footer_width, painter_.MeasureButtonWidth(labels_.cancel) + 12 * s);
    const float confirm_width = (std::min)(max_footer_width, painter_.MeasureButtonWidth(labels_.confirm) + 12 * s);
    controls_.push_back({D2D1::RectF(end - cancel_width, button_y, end, button_y + 32 * s), Action::Cancel, 0, {}, labels_.cancel});
    controls_.push_back({D2D1::RectF(end - cancel_width - gap - confirm_width, button_y,
        end - cancel_width - gap, button_y + 32 * s), Action::Confirm, 0, {}, labels_.confirm});
    if (focus_ < 0) focus_ = static_cast<int>(controls_.size()) - 2;
    if (focus_ >= static_cast<int>(controls_.size())) focus_ = static_cast<int>(controls_.size()) - 2;
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
    if (text.empty() || bounds.right <= bounds.left || bounds.bottom <= bounds.top) return;
    const auto value = OneLine(text);
    auto* format = heading ? compositor_.TextFormat() : compositor_.SmallFormat();
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(typography::CreateTextLayout(compositor_.DwriteFactory(), value.c_str(), static_cast<UINT32>(value.size()), format,
        bounds.right - bounds.left, bounds.bottom - bounds.top, &layout))) return;
    layout->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    if (!heading && bounds.bottom - bounds.top > 28 * scale_) layout->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    ComPtr<IDWriteInlineObject> ellipsis;
    compositor_.DwriteFactory()->CreateEllipsisTrimmingSign(format, &ellipsis);
    DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
    layout->SetTrimming(&trim, ellipsis.get());
    if (layout->GetWordWrapping() == DWRITE_WORD_WRAPPING_NO_WRAP && compositor_.DrawLumaText(
            value, layout.get(), bounds, color, MakeTheme(dark_, accent_).bg)) return;
    ComPtr<ID2D1SolidColorBrush> brush;
    if (SUCCEEDED(compositor_.Dc()->CreateSolidColorBrush(color, &brush)))
        compositor_.Dc()->DrawTextLayout(D2D1::Point2F(bounds.left, bounds.top), layout.get(), brush.get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

bool BatchConflictDialog::Render(bool present) {
    if (!compositor_.Dc()) return false;
    const bool high_contrast = IsHighContrast();
    const auto theme = high_contrast ? MakeHighContrastTheme() : MakeTheme(dark_, accent_);
    const float s = scale_, width = static_cast<float>(compositor_.Width());
    BeginSurface(compositor_, painter_, theme, dark_, high_contrast, backdrop_, s);
    Text(labels_.title, D2D1::RectF(20 * s, 12 * s, width - 20 * s, 38 * s), theme.text, true);
    Text(labels_.instruction, D2D1::RectF(20 * s, 46 * s, width - 20 * s, 82 * s), theme.text_secondary);
    Text(labels_.apply_all, D2D1::RectF(20 * s, 88 * s, width - 20 * s, 108 * s), theme.text_secondary);
    auto* dc = compositor_.Dc();
    dc->PushAxisAlignedClip(viewport_, D2D1_ANTIALIAS_MODE_ALIASED);
    const bool columns = width >= 620 * s;
    for (size_t i = 0; i < rows_.size(); ++i) {
        const auto& bounds = rows_[i];
        if (bounds.bottom < viewport_.top || bounds.top > viewport_.bottom) continue;
        painter_.FillRoundedRect(bounds, theme.radius_control * s, theme.surface_card);
        painter_.StrokeRoundedRect(bounds, theme.radius_control * s, theme.stroke_card);
        const auto& row = conflict_.rows[i];
        for (const bool source : {true, false}) {
            const float left = bounds.left + 12 * s + (!source && columns ? (bounds.right - bounds.left) * 0.5f : 0);
            const float right = source && columns ? (bounds.left + bounds.right) * 0.5f - 8 * s : bounds.right - 12 * s;
            const float top = bounds.top + 8 * s + (!source && !columns ? 108 * s : 0);
            Text(source ? labels_.source : labels_.destination, D2D1::RectF(left, top, right, top + 20 * s), theme.text_secondary);
            const auto& path = source ? row.source : row.destination;
            Text(Leaf(path), D2D1::RectF(left, top + 22 * s, right, top + 44 * s), theme.text, true);
            Text(path, D2D1::RectF(left, top + 46 * s, right, top + 66 * s), theme.text_secondary);
            Text(Metadata(row, source), D2D1::RectF(left, top + 68 * s, right, top + 100 * s), theme.text_secondary);
        }
    }
    for (size_t i = 0; i < controls_.size(); ++i) {
        const auto& control = controls_[i];
        if (control.action != Action::Select) continue;
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
    for (size_t i = 0; i < controls_.size(); ++i) {
        const auto& control = controls_[i];
        if (control.action == Action::Select) continue;
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

void BatchConflictDialog::Scroll(float delta) {
    const float previous = scroll_;
    scroll_ = std::clamp(scroll_ + delta, 0.0f, (std::max)(0.0f, content_height_ - (viewport_.bottom - viewport_.top)));
    if (scroll_ != previous) { hover_ = pressed_ = -1; Layout(); InvalidateRect(hwnd_, nullptr, FALSE); }
}
void BatchConflictDialog::ScrollAt(float y) {
    const float height = (std::max)(1.0f, viewport_.bottom - viewport_.top);
    Scroll((y - viewport_.top) / height * content_height_ - height * 0.5f - scroll_);
}
void BatchConflictDialog::Focus(int direction) {
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
    if (control.action == Action::Cancel) { Complete(false); return; }
    if (control.action == Action::Apply) for (auto& row : selections_) row.choice = control.choice;
    else if (control.row < selections_.size()) selections_[control.row].choice = control.choice;
    InvalidateRect(hwnd_, nullptr, FALSE);
}
void BatchConflictDialog::Complete(bool accepted) {
    result_ = accepted && Valid() ? std::optional{selections_} : std::nullopt;
    done_ = true;
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
    case WM_NCHITTEST: return BorderlessHitTest(hwnd, lparam, 40 * self->scale_, D2D1::RectF());
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
        if (self->compositor_.Dc()) { self->compositor_.Resize(LOWORD(lparam), HIWORD(lparam)); self->Layout(); }
        return 0;
    case WM_DPICHANGED: {
        self->scale_ = HIWORD(wparam) / 96.0f;
        self->compositor_.RecreateTextFormats(self->scale_); self->painter_.SetScale(self->scale_);
        const auto* bounds = reinterpret_cast<RECT*>(lparam);
        SetWindowPos(hwnd, nullptr, bounds->left, bounds->top, bounds->right - bounds->left, bounds->bottom - bounds->top,
            SWP_NOACTIVATE | SWP_NOZORDER); self->Layout(); return 0;
    }
    case WM_SETTINGCHANGE: case WM_THEMECHANGED:
        self->backdrop_ = ApplyBackdrop(hwnd, self->dark_); InvalidateRect(hwnd, nullptr, FALSE); return 0;
    case WM_TIMER: if (!self->Valid()) self->Complete(false); return 0;
    case WM_CLOSE: self->Complete(false); return 0;
    case WM_GETDLGCODE: return DLGC_WANTALLKEYS;
    case WM_KEYDOWN:
        if (wparam == VK_ESCAPE) self->Complete(false);
        else if (wparam == VK_TAB) self->Focus(GetKeyState(VK_SHIFT) < 0 ? -1 : 1);
        else if (wparam == VK_RETURN || wparam == VK_SPACE) self->Invoke(self->focus_);
        else if (wparam == VK_DOWN) self->Scroll(60 * self->scale_);
        else if (wparam == VK_UP) self->Scroll(-60 * self->scale_);
        else if (wparam == VK_NEXT) self->Scroll((self->viewport_.bottom - self->viewport_.top) * 0.8f);
        else if (wparam == VK_PRIOR) self->Scroll(-(self->viewport_.bottom - self->viewport_.top) * 0.8f);
        else if (wparam == VK_HOME) self->Scroll(-self->content_height_);
        else if (wparam == VK_END) self->Scroll(self->content_height_);
        return 0;
    case WM_MOUSEWHEEL: self->Scroll(-GET_WHEEL_DELTA_WPARAM(wparam) / static_cast<float>(WHEEL_DELTA) * 72 * self->scale_); return 0;
    case WM_MOUSEMOVE:
        if (self->dragging_) { self->ScrollAt(static_cast<float>(GET_Y_LPARAM(lparam))); return 0; }
        { TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, hwnd, 0}; TrackMouseEvent(&track); }
        self->hover_ = self->Hit(static_cast<float>(GET_X_LPARAM(lparam)), static_cast<float>(GET_Y_LPARAM(lparam)));
        InvalidateRect(hwnd, nullptr, FALSE); return 0;
    case WM_MOUSELEAVE: self->hover_ = -1; InvalidateRect(hwnd, nullptr, FALSE); return 0;
    case WM_LBUTTONDOWN:
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
    case WM_DESTROY: self->done_ = true; return 0;
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
