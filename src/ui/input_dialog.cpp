// SPDX-License-Identifier: Apache-2.0
#include "input_dialog.h"
#include "dialog_keyboard.h"
#include "dialog_style.h"
#include "edit_host.h"
#include "typography.h"
#include "window_helpers.h"
#include "../common/localization.h"
#include "../common/windows_compat.h"

#include <commctrl.h>
#include <uxtheme.h>
#include <windowsx.h>
#include <algorithm>
#include <cmath>

namespace pulse::ui {
namespace {
constexpr int kBrowse = 102, kCheckbox = 103;

class InputWindow {
public:
    ~InputWindow() {
        if (IsWindow(hwnd_)) DestroyWindow(hwnd_);
        if (font_) DeleteObject(font_);
        if (edit_brush_) DeleteObject(edit_brush_);
    }

    InputDialogResult Show(HWND owner, const InputDialogSpec& spec, bool dark, D2D1_COLOR_F accent) {
        owner_ = owner; spec_ = spec; dark_ = dark; accent_ = accent;
        checked_ = spec.checked;
        scale_ = compat::WindowDpi(owner ? owner : GetDesktopWindow()) / 96.0f;
        edits_.resize(spec_.fields.size());
        if (spec_.confirm_text.empty()) spec_.confirm_text = l10n::Get(l10n::StringId::ConfirmDefault);
        WNDCLASSEXW wc{sizeof(wc)};
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpfnWndProc = WndProc;
        wc.lpszClassName = spec.window_class;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        if (!GetClassInfoExW(wc.hInstance, wc.lpszClassName, &wc) && !RegisterClassExW(&wc)) return {};
        hwnd_ = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOOLWINDOW, spec.window_class,
            spec.title.c_str(), WS_POPUP | WS_SYSMENU, CW_USEDEFAULT, CW_USEDEFAULT,
            Pixels(spec.width), Pixels(spec.height), owner, nullptr, wc.hInstance, this);
        if (!hwnd_) return {};
        CenterOwnedWindow(hwnd_, owner, Pixels(spec.width), Pixels(spec.height));
        ModalOwnerScope modal(owner);
        modal_owner_ = modal.DisabledOwner();
        ShowDialogWithFade(hwnd_);
        Layout();
        SetForegroundWindow(hwnd_);
        if (!edits_.empty()) {
            SetFocus(edits_.front());
            if (spec.select_all) SendMessageW(edits_.front(), EM_SETSEL, 0, -1);
        }
        MSG message{};
        while (!done_ && GetModalMessage(message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (IsWindow(hwnd_)) Complete(false);
        return result_;
    }

private:
    int Pixels(float dip) const { return static_cast<int>(std::lround(dip * scale_)); }
    float Width() const { return static_cast<float>(compositor_.Width()); }
    float Height() const { return static_cast<float>(compositor_.Height()); }
    float FooterTop() const { return Height() - dialog_style::kFooterHeight * scale_; }
    D2D1_RECT_F CloseRect() const { return DialogCloseRect(Width(), scale_); }
    D2D1_RECT_F BrowseRect() const {
        if (!spec_.browse) return {};
        const float right = Width() - dialog_style::kMargin * scale_;
        const float width = (std::max)(80 * scale_, painter_.MeasureButtonWidth(spec_.browse_text));
        return D2D1::RectF(right - width, spec_.fields.front().top * scale_, right,
            (spec_.fields.front().top + dialog_style::kControlHeight) * scale_);
    }
    D2D1_RECT_F FieldRect(size_t index) const {
        float right = Width() - dialog_style::kMargin * scale_;
        if (!index && spec_.browse) right = BrowseRect().left - dialog_style::kGap * scale_;
        return D2D1::RectF(dialog_style::kMargin * scale_, spec_.fields[index].top * scale_,
            right, (spec_.fields[index].top + dialog_style::kControlHeight) * scale_);
    }
    D2D1_RECT_F CheckboxRect() const {
        if (spec_.checkbox_text.empty()) return {};
        return D2D1::RectF(20 * scale_, FooterTop() - 44 * scale_, Width() - 20 * scale_, FooterTop() - 12 * scale_);
    }
    D2D1_RECT_F ErrorRect() const {
        const float top = (spec_.fields.back().top + 36) * scale_;
        return D2D1::RectF(20 * scale_, top, Width() - 20 * scale_, FooterTop() - 4 * scale_);
    }
    void Invalidate(D2D1_RECT_F rect) const {
        if (!hwnd_) return;
        RECT pixels{static_cast<LONG>(std::floor(rect.left)), static_cast<LONG>(std::floor(rect.top)),
            static_cast<LONG>(std::ceil(rect.right)), static_cast<LONG>(std::ceil(rect.bottom))};
        InvalidateRect(hwnd_, &pixels, FALSE);
    }
    std::vector<std::wstring> Values() const {
        std::vector<std::wstring> values;
        for (size_t i = 0; i < edits_.size(); ++i) {
            if (!edits_[i]) { values.push_back(spec_.fields[i].text); continue; }
            const int count = GetWindowTextLengthW(edits_[i]);
            std::wstring text(static_cast<size_t>(count) + 1, L'\0');
            GetWindowTextW(edits_[i], text.data(), count + 1);
            text.resize(static_cast<size_t>(count));
            values.push_back(std::move(text));
        }
        return values;
    }
    void SyncValidation() {
        const auto next = spec_.validate ? spec_.validate(Values()) : InputDialogValidation{};
        if (next.enabled != validation_.enabled) Invalidate(primary_);
        if (next.error != validation_.error) Invalidate(ErrorRect());
        validation_ = next;
    }
    void RecreateFont() {
        HFONT next = typography::CreateEditFont(scale_);
        if (!next) return;
        for (HWND edit : edits_) if (edit) SendMessageW(edit, WM_SETFONT, reinterpret_cast<WPARAM>(next), TRUE);
        if (font_) DeleteObject(font_);
        font_ = next;
    }
    void Layout() {
        if (!compositor_.Dc() || done_) return;
        LayoutDialogActions(painter_, scale_, Width(), Height() - 16 * scale_, spec_.confirm_text,
            l10n::Get(l10n::StringId::Cancel), primary_, cancel_);
        for (size_t i = 0; i < edits_.size(); ++i) if (edits_[i]) {
            const auto bounds = FieldRect(i);
            const int height = EditLineHeight(edits_[i], font_, Pixels(dialog_style::kControlHeight));
            SetWindowPos(edits_[i], HWND_TOP, static_cast<int>(std::lround(bounds.left + 8 * scale_)),
                static_cast<int>(std::lround((bounds.top + bounds.bottom - height) * 0.5f)),
                static_cast<int>(std::lround(bounds.right - bounds.left - 16 * scale_)), height,
                SWP_NOACTIVATE | SWP_SHOWWINDOW);
            PresentChildEdit(compositor_, compositor_.TextFormat(), ChildEditColor(ChildEditTextColor(dark_)),
                ChildEditColor(ChildEditBackColor(dark_)), edits_[i]);
        }
    }
    void MoveFocus(bool backwards) {
        std::vector<DialogFocusTarget> targets;
        for (size_t i = 0; i < edits_.size(); ++i) targets.push_back({spec_.fields[i].id, edits_[i]});
        if (spec_.browse) targets.push_back({kBrowse});
        if (!spec_.checkbox_text.empty()) targets.push_back({kCheckbox});
        targets.push_back({IDCANCEL});
        if (validation_.enabled) targets.push_back({IDOK});
        targets.push_back({spec_.close_id});
        MoveDialogFocus(hwnd_, focus_, targets, backwards);
    }
    void Complete(bool accepted) {
        if (done_) return;
        if (accepted) {
            SyncValidation();
            if (!validation_.enabled) return;
            result_ = {true, Values(), checked_};
        }
        done_ = true;
        if (IsWindow(hwnd_)) {
            HideComposedDialog(hwnd_, modal_owner_);
            DestroyWindow(hwnd_);
        }
    }
    void Invoke(int id) {
        if (id == IDOK) Complete(true);
        else if (id == IDCANCEL || id == spec_.close_id) Complete(false);
        else if (id == kCheckbox && !spec_.checkbox_text.empty()) {
            checked_ = !checked_;
            Invalidate(CheckboxRect());
        } else if (id == kBrowse && spec_.browse && !edits_.empty()) {
            auto values = Values();
            if (spec_.browse(hwnd_, values.front())) SetWindowTextW(edits_.front(), values.front().c_str());
            SetFocus(edits_.front());
        }
    }
    int Hit(float x, float y) const {
        if (ContainsRect(CloseRect(), x, y)) return spec_.close_id;
        if (ContainsRect(primary_, x, y)) return IDOK;
        if (ContainsRect(cancel_, x, y)) return IDCANCEL;
        if (!spec_.checkbox_text.empty() && ContainsRect(CheckboxRect(), x, y)) return kCheckbox;
        if (spec_.browse && ContainsRect(BrowseRect(), x, y)) return kBrowse;
        return 0;
    }
    fluent::ControlState State(int id) const {
        fluent::ControlState state{};
        state.hovered = hover_ == id;
        state.pressed = pressed_ == id;
        state.keyboard_focus = GetFocus() == hwnd_ && focus_ == id;
        return state;
    }
    bool Render(bool present = true) {
        if (compositor_.NeedsRecovery()) {
            if (!compositor_.Recover()) return false;
            compositor_.RecreateTextFormats(scale_);
            painter_.SetCompositor(&compositor_);
            painter_.SetScale(scale_);
            Layout();
        }
        if (!compositor_.Dc()) return false;
        const bool high_contrast = IsHighContrast();
        const auto theme = high_contrast ? MakeHighContrastTheme() : MakeTheme(dark_, accent_);
        BeginSurface(compositor_, painter_, theme, dark_, high_contrast, backdrop_, scale_);
        DrawDialogHeader(compositor_, painter_, theme, scale_, spec_.title, spec_.glyph, State(spec_.close_id));
        DrawDialogFooter(compositor_, painter_, theme, FooterTop());
        for (size_t i = 0; i < edits_.size(); ++i) {
            const auto bounds = FieldRect(i);
            painter_.DrawText(spec_.fields[i].label,
                D2D1::RectF(bounds.left, bounds.top - 36 * scale_, Width() - 20 * scale_, bounds.top - 8 * scale_),
                compositor_.TextFormat(), theme.text);
            fluent::ControlState field{};
            field.focused = GetFocus() == edits_[i];
            painter_.DrawTextFieldFrame(bounds, field, true);
        }
        if (!validation_.error.empty()) painter_.DrawText(validation_.error, ErrorRect(), compositor_.SmallFormat(), theme.danger);
        if (spec_.browse) painter_.DrawButton({BrowseRect(), spec_.browse_text, {}, fluent::ButtonKind::Standard, State(kBrowse)});
        if (!spec_.checkbox_text.empty()) {
            auto state = State(kCheckbox); state.checked = checked_;
            painter_.DrawCheckBox(CheckboxRect(), spec_.checkbox_text, state);
        }
        painter_.DrawButton({cancel_, l10n::Get(l10n::StringId::Cancel), {}, fluent::ButtonKind::Standard, State(IDCANCEL)});
        auto primary = State(IDOK); primary.enabled = validation_.enabled;
        painter_.DrawButton({primary_, spec_.confirm_text, {}, fluent::ButtonKind::Primary, primary});
        if (present) EndSurface(compositor_);
        else {
            const HRESULT result = compositor_.Dc()->EndDraw();
            if (FAILED(result)) { compositor_.NotifyDeviceLost(result); return false; }
        }
        return true;
    }
    static LRESULT CALLBACK EditProc(HWND edit, UINT message, WPARAM wparam, LPARAM lparam, UINT_PTR, DWORD_PTR reference) {
        auto& self = *reinterpret_cast<InputWindow*>(reference);
        TrackDialogEditComposition(edit, message);
        if (!DialogEditComposing(edit)) {
            if (message == WM_KEYDOWN) {
                if (wparam == VK_RETURN || wparam == VK_ESCAPE) {
                    self.Invoke(wparam == VK_RETURN ? IDOK : IDCANCEL);
                    return 0;
                }
                if (wparam == VK_TAB) { self.MoveFocus(GetKeyState(VK_SHIFT) < 0); return 0; }
            }
            if (message == WM_CHAR && (wparam == VK_RETURN || wparam == VK_ESCAPE || wparam == VK_TAB)) return 0;
        }
        const auto foreground = ChildEditColor(ChildEditTextColor(self.dark_));
        const auto background = ChildEditColor(ChildEditBackColor(self.dark_));
        LRESULT result = 0;
        if (HandleChildEditMessage(self.compositor_, self.compositor_.TextFormat(), foreground, background,
            ChildEditBackBrush(self.edit_brush_), edit, message, wparam, lparam, result)) return result;
        return DefPresentedChildEditProc(self.compositor_, self.compositor_.TextFormat(), foreground, background,
            edit, message, wparam, lparam);
    }
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
        auto* self = reinterpret_cast<InputWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            self = static_cast<InputWindow*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
            self->hwnd_ = hwnd;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        return self ? self->Handle(message, wparam, lparam) : DefWindowProcW(hwnd, message, wparam, lparam);
    }
    LRESULT Handle(UINT message, WPARAM wparam, LPARAM lparam) {
        switch (message) {
        case WM_CREATE:
            if (spec_.fields.empty() || !compositor_.Init(hwnd_)) return -1;
            compositor_.RecreateTextFormats(scale_);
            painter_.SetCompositor(&compositor_); painter_.SetScale(scale_);
            backdrop_ = ApplyBackdrop(hwnd_, dark_);
            RecreateFont();
            edit_brush_ = CreateSolidBrush(ChildEditBackColor(dark_));
            for (size_t i = 0; i < edits_.size(); ++i) {
                const auto& field = spec_.fields[i];
                HWND edit = CreateChildEdit(hwnd_, field.text.c_str(), field.password ? ES_PASSWORD : 0);
                if (!edit) return -1;
                edits_[i] = edit;
                SetWindowLongPtrW(edit, GWLP_ID, field.id);
                SetWindowTheme(edit, L"", L"");
                SendMessageW(edit, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
                SendMessageW(edit, EM_SETLIMITTEXT, field.limit, 0);
                SendMessageW(edit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(2, 2));
                if (field.password) SendMessageW(edit, EM_SETPASSWORDCHAR, 0x25CF, 0);
                if (!field.cue.empty()) SendMessageW(edit, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(field.cue.c_str()));
                SetWindowSubclass(edit, EditProc, 1, reinterpret_cast<DWORD_PTR>(this));
            }
            Layout(); SyncValidation();
            return 0;
        case WM_NCCALCSIZE: return 0;
        case WM_NCHITTEST:
            return BorderlessHitTest(hwnd_, lparam, dialog_style::kTitleHeight * scale_, CloseRect());
        case WM_SIZE:
            if (compositor_.Dc()) { compositor_.Resize(LOWORD(lparam), HIWORD(lparam)); Layout(); }
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        case WM_DPICHANGED: {
            scale_ = HIWORD(wparam) / 96.0f;
            compositor_.RecreateTextFormats(scale_); painter_.SetScale(scale_); RecreateFont();
            const auto& rect = *reinterpret_cast<const RECT*>(lparam);
            SetWindowPos(hwnd_, nullptr, rect.left, rect.top, Pixels(spec_.width), Pixels(spec_.height), SWP_NOZORDER | SWP_NOACTIVATE);
            Layout(); InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }
        case WM_MOVE:
            compositor_.UpdateTextRenderingParams(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST));
            return 0;
        case WM_SETTINGCHANGE:
        case WM_THEMECHANGED:
            backdrop_ = ApplyBackdrop(hwnd_, dark_);
            if (edit_brush_) DeleteObject(edit_brush_);
            edit_brush_ = CreateSolidBrush(ChildEditBackColor(dark_));
            for (HWND edit : edits_) {
                SynchronizeChildEditBackend(compositor_, edit);
                InvalidateRect(edit, nullptr, TRUE);
            }
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        case WM_CTLCOLOREDIT:
            SetBkColor(reinterpret_cast<HDC>(wparam), ChildEditBackColor(dark_));
            SetTextColor(reinterpret_cast<HDC>(wparam), ChildEditTextColor(dark_));
            return reinterpret_cast<LRESULT>(ChildEditBackBrush(edit_brush_));
        case WM_COMMAND:
            for (size_t i = 0; i < edits_.size(); ++i) if (LOWORD(wparam) == spec_.fields[i].id && lparam) {
                if (HIWORD(wparam) == EN_CHANGE) SyncValidation();
                if (HIWORD(wparam) == EN_SETFOCUS || HIWORD(wparam) == EN_KILLFOCUS) Invalidate(FieldRect(i));
                return 0;
            }
            Invoke(LOWORD(wparam));
            return 0;
        case WM_KEYDOWN:
            if (wparam == VK_TAB) MoveFocus(GetKeyState(VK_SHIFT) < 0);
            else if (wparam == VK_ESCAPE) Complete(false);
            else if (wparam == VK_RETURN || wparam == VK_SPACE) Invoke(focus_ ? focus_ : IDOK);
            return 0;
        case WM_GETDLGCODE: return DLGC_WANTALLKEYS;
        case WM_MOUSEMOVE: {
            const int hit = Hit(static_cast<float>(GET_X_LPARAM(lparam)), static_cast<float>(GET_Y_LPARAM(lparam)));
            if (hover_ != hit) { hover_ = hit; InvalidateRect(hwnd_, nullptr, FALSE); }
            TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, hwnd_, 0}; TrackMouseEvent(&track);
            return 0;
        }
        case WM_MOUSELEAVE:
            hover_ = 0; InvalidateRect(hwnd_, nullptr, FALSE); return 0;
        case WM_LBUTTONDOWN:
            pressed_ = Hit(static_cast<float>(GET_X_LPARAM(lparam)), static_cast<float>(GET_Y_LPARAM(lparam)));
            if (pressed_) { focus_ = pressed_; SetFocus(hwnd_); SetCapture(hwnd_); }
            else for (size_t i = 0; i < edits_.size(); ++i)
                if (ContainsRect(FieldRect(i), static_cast<float>(GET_X_LPARAM(lparam)), static_cast<float>(GET_Y_LPARAM(lparam)))) SetFocus(edits_[i]);
            InvalidateRect(hwnd_, nullptr, FALSE); return 0;
        case WM_LBUTTONUP: {
            const int pressed = pressed_; pressed_ = 0; ReleaseCapture();
            if (pressed && pressed == Hit(static_cast<float>(GET_X_LPARAM(lparam)), static_cast<float>(GET_Y_LPARAM(lparam)))) Invoke(pressed);
            if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }
        case WM_CAPTURECHANGED:
        case WM_CANCELMODE:
            pressed_ = 0; if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE); return 0;
        case WM_CLOSE: Complete(false); return 0;
        case WM_PAINT: {
            PAINTSTRUCT paint{}; BeginPaint(hwnd_, &paint); Render(); EndPaint(hwnd_, &paint); return 0;
        }
#ifdef PULSE_UI_TEST_HOOKS
        case kInputDialogInspectMessage:
            if (lparam) {
                if (wparam && !Render(false)) return FALSE;
                auto& info = *reinterpret_cast<InputDialogInspection*>(lparam);
                info = {};
                info.scale = scale_; info.close = CloseRect(); info.primary = primary_; info.cancel = cancel_;
                info.checkbox = CheckboxRect(); info.browse = BrowseRect(); info.enabled = validation_.enabled;
                info.checked = checked_; info.high_contrast = IsHighContrast(); info.focus = focus_;
                info.checkbox_text = spec_.checkbox_text; info.error = validation_.error; info.compositor = &compositor_;
                info.title_font_size = compositor_.DialogTitleFormat()->GetFontSize();
                info.title_weight = compositor_.DialogTitleFormat()->GetFontWeight();
                for (size_t i = 0; i < edits_.size(); ++i) info.fields.push_back({spec_.fields[i].id, edits_[i],
                    FieldRect(i), spec_.fields[i].password, SynchronizeChildEditBackend(compositor_, edits_[i])});
                return TRUE;
            }
            return FALSE;
        case kInputDialogSnapshotMessage:
            return lparam && Render(false) && compositor_.SaveSnapshot(reinterpret_cast<const wchar_t*>(lparam));
#endif
        case WM_ERASEBKGND: return 1;
        case WM_DESTROY:
            done_ = true;
            for (HWND& edit : edits_) { if (IsWindow(edit)) DestroyWindow(edit); edit = nullptr; }
            compositor_.Shutdown();
            return 0;
        case WM_NCDESTROY: {
            const HWND hwnd = hwnd_; hwnd_ = nullptr;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            return DefWindowProcW(hwnd, message, wparam, lparam);
        }
        }
        return DefWindowProcW(hwnd_, message, wparam, lparam);
    }

    HWND hwnd_ = nullptr, owner_ = nullptr, modal_owner_ = nullptr;
    Compositor compositor_;
    fluent::Painter painter_;
    InputDialogSpec spec_;
    InputDialogResult result_;
    InputDialogValidation validation_;
    std::vector<HWND> edits_;
    HFONT font_ = nullptr;
    HBRUSH edit_brush_ = nullptr;
    D2D1_RECT_F primary_{}, cancel_{};
    D2D1_COLOR_F accent_{};
    float scale_ = 1.0f;
    bool dark_ = false, backdrop_ = false, checked_ = false, done_ = false;
    int hover_ = 0, pressed_ = 0, focus_ = 0;
};
} // namespace

InputDialogResult ShowInputDialog(HWND owner, const InputDialogSpec& spec, bool dark, D2D1_COLOR_F accent) {
    InputWindow window;
    return window.Show(owner, spec, dark, accent);
}
} // namespace pulse::ui
