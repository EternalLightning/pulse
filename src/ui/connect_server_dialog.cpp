// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

// connect_server_dialog.cpp - see connect_server_dialog.h. Same window
// pattern as batch_rename_dialog: composed popup, one hosted EDIT, own loop.
#include "edit_host.h"
#include "../common/windows_compat.h"
#include "connect_server_dialog.h"
#include "FluentTokens.h"
#include "fluent_components.h"
#include "typography.h"
#include "ui_compositor.h"
#include "window_helpers.h"
#include "dialog_keyboard.h"
#include "../common/localization.h"

#include <commctrl.h>
#include <windowsx.h>
#include <algorithm>
#include <cmath>
#include <string>

namespace pulse::ui {
namespace {

constexpr wchar_t kClass[] = L"PulseConnectServerWindow";
constexpr float kDlgW = 520.0f;
constexpr float kConnectH = 348.0f;
constexpr float kRenameH = 212.0f;
constexpr int kMaxSuggestions = 4;

// Hit ids.
constexpr int kHitConfirm = 1, kHitCancel = 2, kHitClose = 3, kHitPin = 4, kHitField = 10;
constexpr int kHitSuggestion = 20;

D2D1_RECT_F Rect(float scale, float x, float y, float width, float height) {
    return pulse::ui::DipRect(scale, x, y, width, height);
}

class ConnectServerWindow {
public:
    ConnectServerDialogResult Show(HWND owner, const ConnectServerDialogSpec& spec, bool dark, D2D1_COLOR_F accent) {
        owner_ = owner;
        spec_ = spec;
        dark_ = dark;
        accent_ = accent;
        pin_ = spec.pin;
        height_ = spec_.rename ? kRenameH : kConnectH;
        scale_ = static_cast<float>(pulse::compat::WindowDpi(owner ? owner : GetDesktopWindow())) / 96.0f;
        result_ = {};

        WNDCLASSEXW wc{ sizeof(wc) };
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpfnWndProc = WndProc;
        wc.lpszClassName = kClass;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;
        if (!GetClassInfoExW(wc.hInstance, kClass, &wc)) RegisterClassExW(&wc);

        const int width = static_cast<int>(kDlgW * scale_);
        const int height = static_cast<int>(height_ * scale_);
        hwnd_ = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP, kClass, spec_.title.c_str(),
            WS_POPUP | WS_THICKFRAME | WS_SYSMENU,
            CW_USEDEFAULT, CW_USEDEFAULT, width, height, owner, nullptr, wc.hInstance, this);
        if (!hwnd_) return result_;
        pulse::ui::CenterOwnedWindow(hwnd_, owner_, width, height);
        if (owner_) EnableWindow(owner_, FALSE);
        ShowWindow(hwnd_, SW_SHOW);
        LayoutEdit();
        SetForegroundWindow(hwnd_);
        if (edit_) {
            SetFocus(edit_);
            const int n = GetWindowTextLengthW(edit_);
            if (spec_.rename) SendMessageW(edit_, EM_SETSEL, 0, n);
            else SendMessageW(edit_, EM_SETSEL, n, n);
        }

        MSG message{};
        BOOL received = 1;
        while (!done_ && (received = GetMessageW(&message, nullptr, 0, 0)) > 0) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (received == 0) PostQuitMessage(static_cast<int>(message.wParam));
        done_ = true;
        if (IsWindow(hwnd_)) HideComposedDialog(hwnd_, owner_);
        DestroyEdit();
        if (IsWindow(hwnd_)) DestroyWindow(hwnd_);
        hwnd_ = nullptr;
        if (owner_) EnableWindow(owner_, TRUE);
        return result_;
    }

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
        auto* self = reinterpret_cast<ConnectServerWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
            self = static_cast<ConnectServerWindow*>(create->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            self->hwnd_ = hwnd;
        }
        return self ? self->Handle(message, wparam, lparam) : DefWindowProcW(hwnd, message, wparam, lparam);
    }

    D2D1_RECT_F CloseRect() const { return Rect(scale_, kDlgW - 46, 0, 46, 36); }
    D2D1_RECT_F LabelRect() const { return Rect(scale_, 20, 50, 480, 20); }
    D2D1_RECT_F Field() const { return Rect(scale_, 20, 72, 480, 34); }
    D2D1_RECT_F PreviewRect() const { return Rect(scale_, 20, 110, 480, 22); }
    D2D1_RECT_F NoteRect() const { return Rect(scale_, 20, 112, 480, 44); }
    D2D1_RECT_F SuggestRow(int i) const { return Rect(scale_, 20, 138 + 28.0f * static_cast<float>(i), 480, 28); }
    D2D1_RECT_F PinRect() const { return Rect(scale_, 20, 264, 320, 24); }
    D2D1_RECT_F CancelRect() const { return Rect(scale_, kDlgW - 196, height_ - 48, 80, 32); }
    D2D1_RECT_F ConfirmRect() const { return Rect(scale_, kDlgW - 108, height_ - 48, 88, 32); }

    bool CanConfirm() const { return spec_.rename || preview_.ok; }
    void MoveFocus(bool backwards) {
        std::vector<DialogFocusTarget> targets{{kHitField, edit_}};
        if (!spec_.rename) targets.push_back({kHitPin});
        targets.push_back({kHitCancel});
        if (CanConfirm()) targets.push_back({kHitConfirm});
        targets.push_back({kHitClose});
        MoveDialogFocus(hwnd_, focus_, targets, backwards);
    }

    D2D1_RECT_F FocusRect() const {
        if (focus_ == kHitPin) return PinRect();
        if (focus_ == kHitConfirm) return ConfirmRect();
        if (focus_ == kHitCancel) return CancelRect();
        if (focus_ == kHitClose) return CloseRect();
        return {};
    }
    D2D1_COLOR_F EditForeground() const { return ChildEditColor(ChildEditTextColor(dark_)); }
    D2D1_COLOR_F EditBackground() const { return ChildEditColor(ChildEditBackColor(dark_)); }

    void CreateEdit() {
        edit_ = CreateChildEdit(hwnd_, spec_.initial_text.c_str(), 0);
        if (!edit_) return;
        SetWindowTheme(edit_, L"", L"");
        if (font_) SendMessageW(edit_, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
        SetWindowSubclass(edit_, EditProc, 1, reinterpret_cast<DWORD_PTR>(this));
    }

    void LayoutEdit() {
        if (done_ || !edit_ || !hwnd_) return;
        const D2D1_RECT_F cell = Field();
        POINT pt{ static_cast<int>(std::lround(cell.left + 10.0f * scale_)), static_cast<int>(std::lround(cell.top)) };
        const int w = std::max(40, static_cast<int>(std::lround(cell.right - cell.left - 20.0f * scale_)));
        const int cell_h = std::max(18, static_cast<int>(std::lround(cell.bottom - cell.top)));
        const int line_h = EditLineHeight(edit_, font_, cell_h);
        pt.y += std::max(0, (cell_h - line_h) / 2);
        RECT previous{};
        GetWindowRect(edit_, &previous);
        MapWindowPoints(nullptr, hwnd_, reinterpret_cast<POINT*>(&previous), 2);
        const bool surface_changed = !IsWindowVisible(edit_) || previous.left != pt.x || previous.top != pt.y ||
            previous.right - previous.left != w || previous.bottom - previous.top != line_h;
        SetWindowPos(edit_, HWND_TOP, pt.x, pt.y, w, line_h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        if (surface_changed)
            PresentChildEdit(compositor_, compositor_.TextFormat(), EditForeground(), EditBackground(), edit_);
    }

    void DestroyEdit() {
        if (edit_) { DestroyWindow(edit_); edit_ = nullptr; }
    }

    std::wstring EditText() const {
        if (!edit_) return {};
        const int n = GetWindowTextLengthW(edit_);
        std::wstring text(static_cast<size_t>(n) + 1, L'\0');
        if (n > 0) GetWindowTextW(edit_, text.data(), n + 1);
        text.resize(static_cast<size_t>(n));
        return text;
    }

    void Sync() {
        const std::wstring text = EditText();
        preview_ = spec_.preview && !spec_.rename ? spec_.preview(text) : ConnectServerPreview{};
        suggestions_.clear();
        if (spec_.suggestions && !spec_.rename) suggestions_ = spec_.suggestions(text);
        if (suggestions_.size() > static_cast<size_t>(kMaxSuggestions)) suggestions_.resize(kMaxSuggestions);
        selected_ = -1;
    }

    void SetText(const std::wstring& text) {
        if (!edit_) return;
        SetWindowTextW(edit_, text.c_str());  // EN_CHANGE syncs
        const int n = static_cast<int>(text.size());
        SendMessageW(edit_, EM_SETSEL, n, n);
        SetFocus(edit_);
    }

    void MoveSelection(int delta) {
        const int count = static_cast<int>(suggestions_.size());
        if (count == 0) return;
        selected_ = selected_ < 0 ? (delta > 0 ? 0 : count - 1) : (selected_ + delta + count) % count;
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void Complete(bool accepted) {
        if (accepted) {
            if (selected_ >= 0 && selected_ < static_cast<int>(suggestions_.size())) {
                const std::wstring path = suggestions_[static_cast<size_t>(selected_)].path;
                SetText(path);
                Sync();
            }
            if (!CanConfirm()) return;
            result_.accepted = true;
            result_.text = EditText();
            result_.pin = !spec_.rename && pin_;
        }
        done_ = true;
        if (hwnd_) { HideComposedDialog(hwnd_, owner_); DestroyWindow(hwnd_); }
    }

    static LRESULT CALLBACK EditProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam, UINT_PTR, DWORD_PTR ref) {
        auto* self = reinterpret_cast<ConnectServerWindow*>(ref);
        if (!self) return DefSubclassProc(hwnd, msg, wparam, lparam);
        switch (msg) {
        case WM_KEYDOWN:
            if (wparam == VK_ESCAPE) {
                if (self->selected_ >= 0) { self->selected_ = -1; InvalidateRect(self->hwnd_, nullptr, FALSE); }
                else self->Complete(false);
                return 0;
            }
            if (wparam == VK_RETURN) { self->Complete(true); return 0; }
            if (wparam == VK_DOWN) { self->MoveSelection(1); return 0; }
            if (wparam == VK_UP) { self->MoveSelection(-1); return 0; }
            if (wparam == VK_TAB) { self->MoveFocus(GetKeyState(VK_SHIFT) < 0); return 0; }
            break;
        case WM_CHAR:
            if (wparam == VK_RETURN || wparam == VK_ESCAPE || wparam == VK_TAB) return 0;
            break;
        }
        const bool focus_ring = msg == WM_SETFOCUS || msg == WM_KILLFOCUS || msg == WM_LBUTTONDOWN || msg == WM_LBUTTONDBLCLK;
        LRESULT result = 0;
        if (!HandleChildEditMessage(self->compositor_, self->compositor_.TextFormat(), self->EditForeground(),
                                    self->EditBackground(), ChildEditBackBrush(self->edit_brush_), hwnd, msg, wparam,
                                    lparam, result)) {
            result = DefPresentedChildEditProc(self->compositor_, self->compositor_.TextFormat(), self->EditForeground(),
                                               self->EditBackground(), hwnd, msg, wparam, lparam);
        }
        if (focus_ring) InvalidateRect(self->hwnd_, nullptr, FALSE);
        return result;
    }

    int Hit(float x, float y) const {
        using pulse::ui::ContainsRect;
        if (ContainsRect(CloseRect(), x, y)) return kHitClose;
        if (ContainsRect(ConfirmRect(), x, y)) return kHitConfirm;
        if (ContainsRect(CancelRect(), x, y)) return kHitCancel;
        if (ContainsRect(Field(), x, y)) return kHitField;
        if (spec_.rename) return 0;
        for (int i = 0; i < static_cast<int>(suggestions_.size()); ++i)
            if (ContainsRect(SuggestRow(i), x, y)) return kHitSuggestion + i;
        if (ContainsRect(PinRect(), x, y)) return kHitPin;


        return 0;
    }

    void Render(bool present = true) {
        if (compositor_.NeedsRecovery()) {
            if (!compositor_.Recover()) return;
            compositor_.RecreateTextFormats(scale_);
            painter_.SetCompositor(&compositor_);
            painter_.SetScale(scale_);
        }
        if (!compositor_.Dc()) return;
        const bool high_contrast = IsHighContrast();
        const Theme theme = high_contrast ? MakeHighContrastTheme() : MakeTheme(dark_, accent_);
        pulse::ui::BeginSurface(compositor_, painter_, theme, dark_, high_contrast, backdrop_enabled_, scale_);

        painter_.DrawText(spec_.title, Rect(scale_, 16, 0, 400, 36), compositor_.SmallFormat(), theme.text);
        fluent::ControlState close_state{};
        close_state.hovered = hover_ == kHitClose;
        close_state.pressed = pressed_ == kHitClose;
        painter_.DrawTitleBarButton(CloseRect(), fluent::TitleBarButtonRole::Close, {}, close_state);
        painter_.FillRoundedRect(Rect(scale_, 0, 36, kDlgW, 1), 0, theme.stroke_divider);

        painter_.DrawText(spec_.label, LabelRect(), compositor_.SmallFormat(), theme.text_secondary);
        fluent::ControlState field{};
        field.focused = GetFocus() == edit_;
        field.hovered = hover_ == kHitField;
        painter_.DrawTextFieldFrame(Field(), field, true);

        if (spec_.rename) {
            painter_.DrawText(spec_.note, NoteRect(), compositor_.SmallFormat(), theme.text_secondary);
        } else {
            if (!preview_.text.empty())
                painter_.DrawText(preview_.text, PreviewRect(), compositor_.SmallFormat(),
                                  preview_.ok ? theme.text_secondary : theme.danger);
            if (suggestions_.empty()) {
                painter_.DrawText(l10n::Get(l10n::StringId::ConnectServerHint), Rect(scale_, 28, 142, 464, 44),
                                  compositor_.SmallFormat(), theme.text_secondary);
            }
            for (int i = 0; i < static_cast<int>(suggestions_.size()); ++i) {
                const auto row = SuggestRow(i);
                const auto& item = suggestions_[static_cast<size_t>(i)];
                if (selected_ == i || hover_ == kHitSuggestion + i)
                    painter_.FillRoundedRect(row, 4.0f * scale_, selected_ == i ? theme.fill_input_hover : theme.fill_hover);
                painter_.DrawText(item.path, D2D1::RectF(row.left + 8 * scale_, row.top, row.right - 150 * scale_, row.bottom),
                                  compositor_.SmallFormat(), theme.text);
                painter_.DrawText(item.hint, D2D1::RectF(row.right - 150 * scale_, row.top, row.right - 8 * scale_, row.bottom),
                                  compositor_.SmallFormat(), theme.text_secondary, fluent::HorizontalAlignment::Right);
            }

            fluent::ControlState pin{};
            pin.checked = pin_;
            pin.hovered = hover_ == kHitPin;
            pin.pressed = pressed_ == kHitPin;
            painter_.DrawCheckBox(PinRect(), l10n::Get(l10n::StringId::ConnectServerPinAfter), pin);


        }

        fluent::ControlState cancel{};
        cancel.hovered = hover_ == kHitCancel;
        cancel.pressed = pressed_ == kHitCancel;
        painter_.DrawButton({ CancelRect(), l10n::Get(l10n::StringId::Cancel), {}, fluent::ButtonKind::Transparent, cancel });
        const bool can = CanConfirm();
        const auto confirm = ConfirmRect();
        const D2D1_COLOR_F fill = !can ? theme.fill_input
            : (hover_ == kHitConfirm || pressed_ == kHitConfirm ? theme.accent_hover : theme.accent);
        painter_.FillRoundedRect(confirm, theme.radius_control * scale_, fill);
        painter_.DrawText(spec_.confirm_text, confirm, compositor_.SmallFormat(),
                          can ? theme.accent_text : theme.text_secondary, fluent::HorizontalAlignment::Center);

        if (GetFocus() == hwnd_ && focus_) painter_.DrawFocusRing(FocusRect(), 4.0f * scale_);
        if (present) pulse::ui::EndSurface(compositor_);
        else compositor_.Dc()->EndDraw();
    }

    void Click(int hit, bool keyboard = false) {
        if (hit == kHitConfirm) Complete(true);
        else if (hit == kHitCancel || hit == kHitClose) Complete(false);
        else if (hit == kHitPin) pin_ = !pin_;
        else if (hit >= kHitSuggestion && hit < kHitSuggestion + kMaxSuggestions &&
                   hit - kHitSuggestion < static_cast<int>(suggestions_.size())) {
            SetText(suggestions_[static_cast<size_t>(hit - kHitSuggestion)].path);
        }
        if (edit_ && !done_ && hit != kHitConfirm && !keyboard) SetFocus(edit_);
        if (hwnd_ && !done_) InvalidateRect(hwnd_, nullptr, FALSE);
    }

    LRESULT Handle(UINT message, WPARAM wparam, LPARAM lparam) {
        switch (message) {
        case WM_CREATE:
            scale_ = static_cast<float>(pulse::compat::WindowDpi(hwnd_)) / 96.0f;
            backdrop_enabled_ = pulse::ui::ApplyBackdrop(hwnd_, dark_);
            if (!compositor_.Init(hwnd_)) return -1;
            compositor_.RecreateTextFormats(scale_);
            painter_.SetCompositor(&compositor_);
            painter_.SetScale(scale_);
            font_ = typography::CreateEditFont(scale_);
            edit_brush_ = CreateSolidBrush(dark_ ? RGB(30, 30, 30) : RGB(255, 255, 255));
            CreateEdit();
            Sync();
            return 0;
        case WM_NCCALCSIZE:
            return 0;
        case WM_GETMINMAXINFO: {
            auto* info = reinterpret_cast<MINMAXINFO*>(lparam);
            info->ptMinTrackSize = {static_cast<LONG>(std::ceil(kDlgW * scale_)),
                static_cast<LONG>(std::ceil(height_ * scale_))};
            return 0;
        }
        case WM_NCHITTEST:
            return pulse::ui::BorderlessHitTest(hwnd_, lparam, 36 * scale_, CloseRect());
        case WM_SIZE:
            if (compositor_.Dc()) compositor_.Resize(LOWORD(lparam), HIWORD(lparam));
            LayoutEdit();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        case WM_DPICHANGED: {
            scale_ = static_cast<float>(HIWORD(wparam)) / 96.0f;
            HFONT next = typography::CreateEditFont(scale_);
            if (next) {
                if (edit_) SendMessageW(edit_, WM_SETFONT, reinterpret_cast<WPARAM>(next), TRUE);
                if (font_) DeleteObject(font_);
                font_ = next;
            }
            compositor_.RecreateTextFormats(scale_);
            painter_.SetScale(scale_);
            const auto* rect = reinterpret_cast<RECT*>(lparam);
            SetWindowPos(hwnd_, nullptr, rect->left, rect->top,
                (std::max)(rect->right - rect->left, static_cast<LONG>(std::ceil(kDlgW * scale_))),
                (std::max)(rect->bottom - rect->top, static_cast<LONG>(std::ceil(height_ * scale_))),
                SWP_NOZORDER | SWP_NOACTIVATE);
            LayoutEdit();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }
        case WM_MOVE:
            compositor_.UpdateTextRenderingParams(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST));
            LayoutEdit();
            return 0;
        case WM_CTLCOLOREDIT: {
            const HDC hdc = reinterpret_cast<HDC>(wparam);
            SetTextColor(hdc, ChildEditTextColor(dark_));
            SetBkColor(hdc, ChildEditBackColor(dark_));
            return reinterpret_cast<LRESULT>(ChildEditBackBrush(edit_brush_));
        }
        case WM_COMMAND:
            if (HIWORD(wparam) == EN_CHANGE) {
                Sync();
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            return 0;
        case WM_MOUSEMOVE: {
            const int next = Hit(static_cast<float>(GET_X_LPARAM(lparam)), static_cast<float>(GET_Y_LPARAM(lparam)));
            if (next != hover_) { hover_ = next; InvalidateRect(hwnd_, nullptr, FALSE); }
            TRACKMOUSEEVENT track{ sizeof(track), TME_LEAVE, hwnd_, 0 };
            TrackMouseEvent(&track);
            return 0;
        }
        case WM_MOUSELEAVE:
            hover_ = 0;
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        case WM_LBUTTONDOWN: {
            const int hit = Hit(static_cast<float>(GET_X_LPARAM(lparam)), static_cast<float>(GET_Y_LPARAM(lparam)));
            if (hit == kHitField) {
                if (edit_) SetFocus(edit_);
                pressed_ = 0;
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            }
            pressed_ = hit;
            SetCapture(hwnd_);
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }
        case WM_LBUTTONUP: {
            const int hit = Hit(static_cast<float>(GET_X_LPARAM(lparam)), static_cast<float>(GET_Y_LPARAM(lparam)));
            const int pressed = pressed_;
            pressed_ = 0;
            ReleaseCapture();
            if (hit == pressed && hit != 0) Click(hit);
            if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }
        case WM_KEYDOWN:
            if (wparam == VK_ESCAPE) { Complete(false); return 0; }
            if (wparam == VK_TAB) { MoveFocus(GetKeyState(VK_SHIFT) < 0); return 0; }
            if (wparam == VK_RETURN || wparam == VK_SPACE) { Click(focus_, true); return 0; }
            break;
        case WM_GETDLGCODE:
            return DLGC_WANTALLKEYS;
        case WM_CLOSE:
            Complete(false);
            return 0;
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            BeginPaint(hwnd_, &paint);
            Render();
            EndPaint(hwnd_, &paint);
            return 0;
        }
#ifdef PULSE_UI_TEST_HOOKS
        case WM_APP + 0x2B5:
            Render(false);
            return lparam && compositor_.SaveSnapshot(reinterpret_cast<const wchar_t*>(lparam));
#endif
        case WM_ERASEBKGND:
            return 1;
        case WM_DESTROY:
            DestroyEdit();
            if (font_) { DeleteObject(font_); font_ = nullptr; }
            if (edit_brush_) { DeleteObject(edit_brush_); edit_brush_ = nullptr; }
            compositor_.Shutdown();
            hwnd_ = nullptr;
            return 0;
        }
        return DefWindowProcW(hwnd_, message, wparam, lparam);
    }

    HWND hwnd_ = nullptr;
    HWND owner_ = nullptr;
    HWND edit_ = nullptr;
    HFONT font_ = nullptr;
    HBRUSH edit_brush_ = nullptr;
    Compositor compositor_;
    fluent::Painter painter_;
    ConnectServerDialogSpec spec_;
    ConnectServerPreview preview_;
    std::vector<ConnectServerSuggestion> suggestions_;
    ConnectServerDialogResult result_;
    bool dark_ = false;
    bool backdrop_enabled_ = false;
    bool done_ = false;
    bool pin_ = true;
    float scale_ = 1.0f;
    float height_ = kConnectH;
    D2D1_COLOR_F accent_ = HexColor(0x0078D4);
    int hover_ = 0;
    int pressed_ = 0;
    int selected_ = -1;
    int focus_ = 0;
};

} // namespace

ConnectServerDialogResult ShowConnectServerDialog(HWND owner, const ConnectServerDialogSpec& spec,
                                                  bool dark, D2D1_COLOR_F accent) {
    ConnectServerWindow window;
    return window.Show(owner, spec, dark, accent);
}

} // namespace pulse::ui
