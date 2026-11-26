// SPDX-License-Identifier: Apache-2.0
#include "toolbar_customization.h"
#include "dialog_keyboard.h"
#include "dialog_style.h"
#include "window_helpers.h"
#include "animation_clock.h"
#include "binary_transition.h"
#include "edit_frame_scheduler.h"
#include "smooth_scroll.h"
#include "toolbar_icons.h"
#include "../common/windows_compat.h"
#include <windowsx.h>
#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace pulse::ui {
ToolbarCustomizationModel::ToolbarCustomizationModel(const ToolbarCustomizationSpec& spec) {
    for (const auto& action : spec.actions)
        if (action.command > 0 && std::find(allowed_.begin(), allowed_.end(), action.command) == allowed_.end())
            allowed_.push_back(action.command);
    for (const int command : spec.default_order)
        if (std::find(allowed_.begin(), allowed_.end(), command) != allowed_.end() &&
            std::find(defaults_.begin(), defaults_.end(), command) == defaults_.end() && defaults_.size() < 64)
            defaults_.push_back(command);
    for (const int command : spec.order) Add(command);
}
bool ToolbarCustomizationModel::Contains(int command) const {
    return std::find(order_.begin(), order_.end(), command) != order_.end();
}
bool ToolbarCustomizationModel::Add(int command) {
    if (order_.size() >= 64 || Contains(command) ||
        std::find(allowed_.begin(), allowed_.end(), command) == allowed_.end()) return false;
    order_.push_back(command);
    return true;
}
bool ToolbarCustomizationModel::Remove(int command) {
    return std::erase(order_, command) != 0;
}
bool ToolbarCustomizationModel::Move(int command, int before_command) {
    if (!Contains(command) || command == before_command || (before_command && !Contains(before_command))) return false;
    const auto previous = order_;
    std::erase(order_, command);
    order_.insert(std::find(order_.begin(), order_.end(), before_command), command);
    return order_ != previous;
}
void ToolbarCustomizationModel::Reset() { order_ = defaults_; }

namespace {
constexpr int kAvailable = 100, kCurrent = 101, kAdd = 102, kRemove = 103,
    kUp = 104, kDown = 105, kRestore = 106, kClose = 107;
constexpr UINT_PTR kDragTimer = 1;
constexpr UINT_PTR kFrameTimer = 2;
struct AvailableRow { const ToolbarCustomizationAction* action = nullptr; std::wstring category; };
struct ControlMotion { BinaryTransition hover, focus; };
struct RowMotion { BinaryTransition hover, selection; };

class ToolbarWindow {
public:
    explicit ToolbarWindow(const ToolbarCustomizationSpec& spec) : spec_(spec), model_(spec) {
        std::vector<std::wstring> categories;
        for (const auto& action : spec_.actions)
            if (std::find(categories.begin(), categories.end(), action.category) == categories.end())
                categories.push_back(action.category);
        for (const auto& category : categories) {
            if (!category.empty()) rows_.push_back({nullptr, category});
            for (const auto& action : spec_.actions) if (action.category == category) rows_.push_back({&action, {}});
        }
        if (!spec_.actions.empty()) available_selection_ = spec_.actions.front().command;
        if (!model_.Order().empty()) current_selection_ = model_.Order().front();
    }
    ~ToolbarWindow() { if (IsWindow(hwnd_)) DestroyWindow(hwnd_); }
    ToolbarCustomizationResult Show(HWND owner, bool dark, D2D1_COLOR_F accent) {
        dark_ = dark; accent_ = accent;
        scale_ = compat::WindowDpi(owner ? owner : GetDesktopWindow()) / 96.0f;
        WNDCLASSEXW wc{sizeof(wc)};
        wc.hInstance = GetModuleHandleW(nullptr); wc.lpfnWndProc = WndProc;
        wc.lpszClassName = kToolbarCustomizationWindowClass;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        if (!GetClassInfoExW(wc.hInstance, wc.lpszClassName, &wc) && !RegisterClassExW(&wc)) return {};
        const auto work = WorkArea(owner);
        const int width = std::min(Pixels(860), static_cast<int>(work.right-work.left)-16);
        const int height = std::min(Pixels(610), static_cast<int>(work.bottom-work.top)-16);
        hwnd_ = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOOLWINDOW, kToolbarCustomizationWindowClass,
            spec_.title.c_str(), WS_POPUP | WS_SYSMENU | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT,
            width, height, owner, nullptr, wc.hInstance, this);
        if (!hwnd_) return {};
        CenterOwnedWindow(hwnd_, owner, width, height);
        ModalOwnerScope modal(owner);
        modal_owner_ = modal.DisabledOwner();
        ShowDialogWithFade(hwnd_);
        SetForegroundWindow(hwnd_); SetFocus(hwnd_);
        MSG message{};
        while (!done_ && GetModalMessage(message)) { TranslateMessage(&message); DispatchMessageW(&message); }
        if (IsWindow(hwnd_)) Complete(false);
        return result_;
    }
private:
    static RECT WorkArea(HWND owner) {
        MONITORINFO info{sizeof(info)};
        GetMonitorInfoW(MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST), &info);
        return info.rcWork;
    }
    int Pixels(float dip) const { return static_cast<int>(std::lround(dip*scale_)); }
    float RowHeight() const { return 36*scale_; }
    float Width() const { return static_cast<float>(compositor_.Width()); }
    float Height() const { return static_cast<float>(compositor_.Height()); }
    float FooterTop() const { return Height()-dialog_style::kFooterHeight*scale_; }
    const ToolbarCustomizationAction* Action(int command) const {
        const auto found = std::find_if(spec_.actions.begin(), spec_.actions.end(),
            [=](const auto& action) { return action.command == command; });
        return found == spec_.actions.end() ? nullptr : &*found;
    }
    D2D1_RECT_F RowRect(bool current, size_t index) const {
        const auto bounds = current ? current_ : available_;
        const float top = bounds.top+4*scale_+static_cast<float>(index)*RowHeight()-(current ? current_scroll_ : available_scroll_);
        return {bounds.left+4*scale_, top, bounds.right-12*scale_, top+RowHeight()};
    }
    float ScrollMax(bool current) const {
        const auto bounds = current ? current_ : available_;
        return std::max(0.0f, static_cast<float>(current ? model_.Order().size() : rows_.size())*RowHeight()+8*scale_-(bounds.bottom-bounds.top));
    }
    void ClampScroll() {
        current_scroll_ = std::clamp(current_scroll_, 0.0f, ScrollMax(true));
        available_scroll_ = std::clamp(available_scroll_, 0.0f, ScrollMax(false));
    }
    void StopScroll() {
        available_motion_.Reset(); current_motion_.Reset();
    }
    void CancelFrame() {
        frame_scheduler_.Cancel();
        if (hwnd_) KillTimer(hwnd_, kFrameTimer);
    }
    void StopMotion() {
        StopScroll(); CancelFrame();
        control_motion_.clear(); available_row_motion_.clear(); current_row_motion_.clear();
        interaction_pending_ = snap_motion_ = false;
    }
    void ScheduleFrame() {
        if (done_ || !IsWindowVisible(hwnd_) ||
            (!interaction_pending_ && !available_motion_.Pending() && !current_motion_.Pending())) {
            CancelFrame();
            return;
        }
        if (frame_scheduler_.Request(hwnd_)) KillTimer(hwnd_, kFrameTimer);
        else if (!SetTimer(hwnd_, kFrameTimer, USER_TIMER_MINIMUM, nullptr)) {
            snap_motion_ = true;
            TickFrame();
        }
    }
    void RefreshMotion() {
        BOOL enabled = TRUE;
        SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &enabled, 0);
        motion_enabled_ = enabled && !IsHighContrast();
        TickFrame();
    }
    void UpdateHover(float x, float y) {
        hover_ = Hit(x, y);
        hover_command_ = hover_ == kAvailable || hover_ == kCurrent ? RowCommand(hover_ == kCurrent, x, y) : 0;
    }
    void TickFrame() {
        const auto now = AnimationTimeMs();
        const bool animate = motion_enabled_ && !snap_motion_;
        const bool available_changed = available_motion_.Tick(available_scroll_, ScrollMax(false), now, animate);
        const bool current_changed = current_motion_.Tick(current_scroll_, ScrollMax(true), now, animate);
        if (available_changed || current_changed) {
            if (hover_) UpdateHover(pointer_x_, pointer_y_);
            if (dragging_) UpdateDrag(pointer_x_, pointer_y_, false);
        }
        if (available_changed || current_changed || interaction_pending_ || snap_motion_ ||
            available_motion_.Pending() || current_motion_.Pending())
            InvalidateRect(hwnd_, nullptr, FALSE);
    }
    void ScrollList(bool current, float delta, bool reveal = false) {
        auto& offset = current ? current_scroll_ : available_scroll_;
        auto& motion = current ? current_motion_ : available_motion_;
        const auto viewport = current ? current_ : available_;
        if (reveal) motion.Reset();
        motion.ScrollBy(offset, delta, ScrollMax(current),
            std::max(viewport.bottom-viewport.top, reveal ? std::abs(delta) : 0.0f), AnimationTimeMs(), motion_enabled_);
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
    void EnsureSelectionVisible(bool current) {
        TickFrame();
        (current ? current_motion_ : available_motion_).Reset();
        size_t index = 0;
        if (current) {
            const auto found = std::find(model_.Order().begin(), model_.Order().end(), current_selection_);
            if (found == model_.Order().end()) return;
            index = static_cast<size_t>(found-model_.Order().begin());
        } else {
            const auto found = std::find_if(rows_.begin(), rows_.end(), [&](const auto& row) {
                return row.action && row.action->command == available_selection_;
            });
            if (found == rows_.end()) return;
            index = static_cast<size_t>(found-rows_.begin());
        }
        const auto row = RowRect(current, index), viewport = current ? current_ : available_;
        if (row.top < viewport.top+4*scale_) ScrollList(current, row.top-(viewport.top+4*scale_), true);
        else if (row.bottom > viewport.bottom-4*scale_) ScrollList(current, row.bottom-(viewport.bottom-4*scale_), true);
    }
    void Layout() {
        StopScroll();
        if (!compositor_.Dc()) return;
        LayoutDialogActions(painter_, scale_, Width(), Height()-16*scale_, spec_.save_text, spec_.cancel_text, primary_, cancel_);
        const float margin = 20*scale_, gap = 12*scale_;
        const float center = std::max({88*scale_, painter_.MeasureButtonWidth(spec_.add_text), painter_.MeasureButtonWidth(spec_.remove_text)});
        const float column = std::max(24*scale_, (Width()-2*margin-2*gap-center)*0.5f);
        const float top = 96*scale_, bottom = std::max(top, FooterTop()-56*scale_);
        available_ = {margin, top, margin+column, bottom};
        current_ = {available_.right+2*gap+center, top, Width()-margin, bottom};
        const float action_top = top+std::max(0.0f, (bottom-top)*0.5f-36*scale_);
        add_ = {available_.right+gap, action_top, current_.left-gap, action_top+32*scale_};
        remove_ = {add_.left, add_.bottom+8*scale_, add_.right, add_.bottom+40*scale_};
        const float tools_top = FooterTop()-44*scale_;
        const float restore_width = std::min(column, std::max(100*scale_, painter_.MeasureButtonWidth(spec_.restore_text)));
        restore_ = {margin, tools_top, margin+restore_width, tools_top+32*scale_};
        const float move_width = std::min((column-8*scale_)*0.5f, std::max({88*scale_,
            painter_.MeasureButtonWidth(spec_.up_text), painter_.MeasureButtonWidth(spec_.down_text)}));
        up_ = {current_.left, tools_top, current_.left+move_width, tools_top+32*scale_};
        down_ = {up_.right+8*scale_, tools_top, up_.right+8*scale_+move_width, tools_top+32*scale_};
        ClampScroll();
        if (hover_) UpdateHover(pointer_x_, pointer_y_);
    }
    bool Enabled(int id) const {
        if (id == kAdd) return Action(available_selection_) && !model_.Contains(available_selection_) && model_.Order().size() < 64;
        if (id == kRemove) return model_.Contains(current_selection_);
        const auto found = std::find(model_.Order().begin(), model_.Order().end(), current_selection_);
        if (id == kUp) return found != model_.Order().end() && found != model_.Order().begin();
        if (id == kDown) return found != model_.Order().end() && found+1 != model_.Order().end();
        return true;
    }
    float HoverOpacity(BinaryTransition& track, bool hovered) {
        track.TickHover(hovered, frame_now_, motion_enabled_ && !snap_motion_);
        interaction_pending_ |= track.Pending();
        return track.value;
    }
    float FocusOpacity(int id, bool enabled = true) {
        return HoverOpacity(control_motion_[id].focus,
            enabled && keyboard_cues_ && GetFocus() == hwnd_ && focus_ == id);
    }
    fluent::ControlState State(int id) {
        fluent::ControlState state;
        state.enabled = Enabled(id);
        state.hovered = state.enabled && hover_ == id;
        state.pressed = state.hovered && pressed_ == id;
        state.hover_progress = HoverOpacity(control_motion_[id].hover, state.hovered);
        if (id != kClose) state.focus_progress = FocusOpacity(id, state.enabled);
        return state;
    }
    void MoveSelected(int direction) {
        const auto& order = model_.Order();
        const auto found = std::find(order.begin(), order.end(), current_selection_);
        if (found == order.end()) return;
        const auto index = static_cast<size_t>(found-order.begin());
        if (direction < 0 && index) model_.Move(current_selection_, order[index-1]);
        else if (direction > 0 && index+1 < order.size())
            model_.Move(current_selection_, index+2 < order.size() ? order[index+2] : 0);
        EnsureSelectionVisible(true);
    }
    void Invoke(int id) {
        if (!Enabled(id)) return;
        StopScroll();
        if (id == IDOK) { Complete(true); return; }
        if (id == IDCANCEL || id == kClose) { Complete(false); return; }
        if (id == kAdd && model_.Add(available_selection_)) {
            current_selection_ = available_selection_; EnsureSelectionVisible(true);
        } else if (id == kRemove) {
            const auto found = std::find(model_.Order().begin(), model_.Order().end(), current_selection_);
            const auto index = static_cast<size_t>(found-model_.Order().begin());
            model_.Remove(current_selection_);
            current_selection_ = model_.Order().empty() ? 0 : model_.Order()[std::min(index, model_.Order().size()-1)];
            ClampScroll();
        } else if (id == kUp || id == kDown) MoveSelected(id == kUp ? -1 : 1);
        else if (id == kRestore) {
            model_.Reset(); current_scroll_ = 0;
            current_selection_ = model_.Order().empty() ? 0 : model_.Order().front();
        }
        std::erase_if(current_row_motion_, [&](const auto& entry) { return !model_.Contains(entry.first); });
        if (hover_) UpdateHover(pointer_x_, pointer_y_);
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
    void Complete(bool accepted) {
        if (done_) return;
        if (accepted) result_ = {true, model_.Order()};
        done_ = true; StopMotion(); CancelDrag();
        if (IsWindow(hwnd_)) { HideComposedDialog(hwnd_, modal_owner_); DestroyWindow(hwnd_); }
    }
    int Hit(float x, float y) const {
        const std::pair<int, D2D1_RECT_F> bounds[] = {{kClose, DialogCloseRect(Width(), scale_)},
            {IDOK, primary_}, {IDCANCEL, cancel_}, {kAdd, add_}, {kRemove, remove_},
            {kUp, up_}, {kDown, down_}, {kRestore, restore_}, {kAvailable, available_}, {kCurrent, current_}};
        for (const auto& item : bounds) if (ContainsRect(item.second, x, y)) return item.first;
        return 0;
    }
    int RowCommand(bool current, float x, float y) const {
        if (!ContainsRect(current ? current_ : available_, x, y)) return 0;
        const size_t count = current ? model_.Order().size() : rows_.size();
        for (size_t i = 0; i < count; ++i) if (ContainsRect(RowRect(current, i), x, y))
            return current ? model_.Order()[i] : rows_[i].action ? rows_[i].action->command : 0;
        return 0;
    }
    void MoveListSelection(bool current, int direction, bool edge = false) {
        std::vector<int> commands;
        if (current) commands = model_.Order();
        else for (const auto& row : rows_) if (row.action) commands.push_back(row.action->command);
        if (commands.empty()) return;
        auto& selection = current ? current_selection_ : available_selection_;
        const auto found = std::find(commands.begin(), commands.end(), selection);
        const int index = found == commands.end() ? 0 : static_cast<int>(found-commands.begin());
        selection = commands[static_cast<size_t>(edge ? (direction < 0 ? 0 : static_cast<int>(commands.size())-1)
            : std::clamp(index+direction, 0, static_cast<int>(commands.size())-1))];
        EnsureSelectionVisible(current); InvalidateRect(hwnd_, nullptr, FALSE);
    }
    void MoveFocus(bool backwards) {
        std::vector<DialogFocusTarget> targets{{kAvailable}};
        if (Enabled(kAdd)) targets.push_back({kAdd});
        if (Enabled(kRemove)) targets.push_back({kRemove});
        targets.push_back({kCurrent}); targets.push_back({kRestore});
        if (Enabled(kUp)) targets.push_back({kUp});
        if (Enabled(kDown)) targets.push_back({kDown});
        targets.push_back({IDCANCEL}); targets.push_back({IDOK});
        MoveDialogFocus(hwnd_, focus_, targets, backwards);
    }
    void CancelDrag() {
        if (dragging_) StopScroll();
        KillTimer(hwnd_, kDragTimer); drag_command_ = 0; dragging_ = false; drop_before_ = 0;
        pressed_ = 0; pressed_command_ = 0; scrollbar_drag_ = 0;
        if (GetCapture() == hwnd_) ReleaseCapture();
    }
    void UpdateDrag(float x, float y, bool scroll) {
        if (!drag_command_) return;
        if (!dragging_ && (std::abs(x-drag_start_.x) >= GetSystemMetrics(SM_CXDRAG) ||
            std::abs(y-drag_start_.y) >= GetSystemMetrics(SM_CYDRAG))) dragging_ = true;
        if (!dragging_) return;
        if (scroll && x >= current_.left && x < current_.right) {
            if (y < current_.top+24*scale_) ScrollList(true, -18*scale_);
            else if (y > current_.bottom-24*scale_) ScrollList(true, 18*scale_);
        }
        const float position = (y-current_.top-4*scale_+current_scroll_)/RowHeight();
        drop_index_ = std::clamp(static_cast<int>(std::floor(position+0.5f)), 0, static_cast<int>(model_.Order().size()));
        drop_before_ = static_cast<size_t>(drop_index_) < model_.Order().size() ? model_.Order()[static_cast<size_t>(drop_index_)] : 0;
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
    fluent::ScrollbarSpec Scrollbar(bool current) const {
        const int id = current ? kCurrent : kAvailable;
        const auto viewport = current ? current_ : available_;
        const size_t count = current ? model_.Order().size() : rows_.size();
        const auto motion = control_motion_.find(id);
        return {viewport, current ? current_scroll_ : available_scroll_, viewport.bottom - viewport.top,
            static_cast<float>(count)*RowHeight()+8*scale_,
            motion == control_motion_.end() ? 0.0f : motion->second.hover.value};
    }
    D2D1_RECT_F ScrollbarThumb(bool current) const {
        return fluent::ScrollbarThumbRect(Scrollbar(current), scale_, IsHighContrast());
    }
    void DragScrollbar(float y) {
        const bool current = scrollbar_drag_ == kCurrent;
        const auto spec = Scrollbar(current);
        const auto thumb = ScrollbarThumb(current);
        const float travel = spec.viewport_extent - (thumb.bottom - thumb.top);
        if (travel <= 0.0f) return;
        (current ? current_scroll_ : available_scroll_) =
            std::clamp((y - scrollbar_grab_ - spec.viewport.top) / travel, 0.0f, 1.0f) * ScrollMax(current);
    }
    void DrawList(bool current, const Theme& theme) {
        const auto viewport = current ? current_ : available_;
        const int id = current ? kCurrent : kAvailable;
        painter_.FillRoundedRect(viewport, theme.radius_control*scale_, theme.fill_input);
        painter_.StrokeRoundedRect(viewport, theme.radius_control*scale_, theme.stroke_card, scale_);
        auto* dc = compositor_.Dc();
        const bool high_contrast = IsHighContrast();
        dc->PushAxisAlignedClip(viewport, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        const size_t count = current ? model_.Order().size() : rows_.size();
        auto& row_motion = current ? current_row_motion_ : available_row_motion_;
        for (size_t i = 0; i < count; ++i) {
            const auto bounds = RowRect(current, i);
            const bool visible = bounds.bottom > viewport.top && bounds.top < viewport.bottom;
            const auto* action = current ? Action(model_.Order()[i]) : rows_[i].action;
            if (!action) {
                if (!current && visible) painter_.DrawText(rows_[i].category,
                    {bounds.left+10*scale_, bounds.top, bounds.right, bounds.bottom}, compositor_.HeaderFormat(), theme.text_secondary);
                continue;
            }
            fluent::MenuItemSpec row;
            row.bounds = bounds; row.text = action->text;
            row.state.selected = action->command == (current ? current_selection_ : available_selection_);
            row.state.hovered = visible && action->command == hover_command_ && hover_ == id &&
                !dragging_ && !scrollbar_drag_;
            row.state.pressed = row.state.hovered && pressed_ == id && pressed_command_ == action->command;
            auto& motion = row_motion[action->command];
            row.state.hover_progress = HoverOpacity(motion.hover, row.state.hovered);
            motion.selection.Tick(row.state.selected, frame_now_, motion_enabled_ && !snap_motion_, 120.0f);
            row.selection_progress = motion.selection.value;
            interaction_pending_ |= motion.selection.Pending();
            if (!visible) continue;
            if (!current && model_.Contains(action->command)) row.shortcut = L"✓";
            painter_.DrawMenuItem(row);
            const auto color = high_contrast && (row.state.selected || row.state.hovered)
                ? theme.text_selected : theme.text;
            const auto icon_bounds = D2D1::RectF(bounds.left+10*scale_,bounds.top,bounds.left+30*scale_,bounds.bottom);
            if (toolbar_icons::DrawArtwork(painter_, action->command, icon_bounds, scale_, dark_, high_contrast, color)) continue;
            if (!icon_brush_.get()) dc->CreateSolidColorBrush(theme.text, &icon_brush_);
            if (!icon_stroke_.get()) command_icons::CreateStrokeStyle(dc, &icon_stroke_);
            if (icon_brush_.get()) {
                icon_brush_->SetColor(color);
                toolbar_icons::Draw(painter_, dc, icon_brush_.get(), icon_stroke_.get(),
                    toolbar_icons::Resolve(action->glyph,action->pictogram,action->command), icon_bounds, scale_);
            }
        }
        if (current && dragging_) {
            const float y = current_.top+4*scale_+static_cast<float>(drop_index_)*RowHeight()-current_scroll_;
            painter_.FillRoundedRect({current_.left+6*scale_, y-scale_, current_.right-12*scale_, y+scale_}, scale_, theme.accent_ink);
        }
        HoverOpacity(control_motion_[id].hover, hover_ == id || scrollbar_drag_ == id);
        painter_.DrawScrollbar(Scrollbar(current));
        dc->PopAxisAlignedClip();
        painter_.DrawFocusRing(viewport, theme.radius_control*scale_, FocusOpacity(id));
    }
    bool Render(bool present = true) {
        if (compositor_.NeedsRecovery()) {
            if (!compositor_.Recover()) return false;
            icon_brush_.reset(); icon_stroke_.reset();
            compositor_.RecreateTextFormats(scale_); painter_.SetCompositor(&compositor_); painter_.SetScale(scale_); Layout();
        }
        if (!compositor_.Dc()) return false;
        const bool high_contrast = IsHighContrast();
        const auto theme = high_contrast ? MakeHighContrastTheme() : MakeTheme(dark_, accent_);
        frame_now_ = AnimationTimeMs();
        interaction_pending_ = false;
        BeginSurface(compositor_, painter_, theme, dark_, high_contrast, backdrop_, scale_);
        DrawDialogHeader(compositor_, painter_, theme, scale_, spec_.title, L"\xE713", State(kClose));
        DrawDialogFooter(compositor_, painter_, theme, FooterTop());
        painter_.DrawText(spec_.available_label, {available_.left, 60*scale_, available_.right, 88*scale_}, compositor_.HeaderFormat(), theme.text);
        painter_.DrawText(spec_.current_label, {current_.left, 60*scale_, current_.right, 88*scale_}, compositor_.HeaderFormat(), theme.text);
        DrawList(false, theme); DrawList(true, theme);
        painter_.DrawButton({add_, spec_.add_text, {}, fluent::ButtonKind::Standard, State(kAdd)});
        painter_.DrawButton({remove_, spec_.remove_text, {}, fluent::ButtonKind::Standard, State(kRemove)});
        painter_.DrawButton({up_, spec_.up_text, {}, fluent::ButtonKind::Standard, State(kUp)});
        painter_.DrawButton({down_, spec_.down_text, {}, fluent::ButtonKind::Standard, State(kDown)});
        painter_.DrawButton({restore_, spec_.restore_text, {}, fluent::ButtonKind::Transparent, State(kRestore)});
        painter_.DrawButton({cancel_, spec_.cancel_text, {}, fluent::ButtonKind::Standard, State(IDCANCEL)});
        painter_.DrawButton({primary_, spec_.save_text, {}, fluent::ButtonKind::Primary, State(IDOK)});
        snap_motion_ = false;
        if (present) {
            EndSurface(compositor_);
            ScheduleFrame();
        }
        else {
            const HRESULT result = compositor_.Dc()->EndDraw();
            if (FAILED(result)) { compositor_.NotifyDeviceLost(result); return false; }
        }
        return true;
    }
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
        auto* self = reinterpret_cast<ToolbarWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            self = static_cast<ToolbarWindow*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
            self->hwnd_ = hwnd; SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        return self ? self->Handle(message, wparam, lparam) : DefWindowProcW(hwnd, message, wparam, lparam);
    }
    LRESULT Handle(UINT message, WPARAM wparam, LPARAM lparam) {
        if (message == EditMotionFrameMessage()) {
            if (frame_scheduler_.Consume(hwnd_, wparam)) TickFrame();
            return 0;
        }
        switch (message) {
        case WM_CREATE:
            if (!compositor_.Init(hwnd_)) return -1;
            compositor_.RecreateTextFormats(scale_); painter_.SetCompositor(&compositor_); painter_.SetScale(scale_);
            backdrop_ = ApplyBackdrop(hwnd_, dark_); Layout(); RefreshMotion(); return 0;
        case WM_NCCALCSIZE: return 0;
        case WM_NCHITTEST:
            return BorderlessHitTest(hwnd_, lparam, dialog_style::kTitleHeight*scale_, DialogCloseRect(Width(), scale_));
        case WM_SIZE:
            if (compositor_.Dc()) { compositor_.Resize(LOWORD(lparam), HIWORD(lparam)); Layout(); }
            if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE); return 0;
        case WM_DPICHANGED: {
            CancelDrag(); scale_ = HIWORD(wparam)/96.0f;
            compositor_.RecreateTextFormats(scale_); painter_.SetScale(scale_);
            const auto& suggested = *reinterpret_cast<const RECT*>(lparam);
            const auto work = WorkArea(hwnd_);
            const int width = std::min(Pixels(860), static_cast<int>(work.right-work.left)-16);
            const int height = std::min(Pixels(610), static_cast<int>(work.bottom-work.top)-16);
            const int x = std::clamp(static_cast<int>(suggested.left), static_cast<int>(work.left), static_cast<int>(work.right)-width);
            const int y = std::clamp(static_cast<int>(suggested.top), static_cast<int>(work.top), static_cast<int>(work.bottom)-height);
            SetWindowPos(hwnd_, nullptr, x, y, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
            Layout(); InvalidateRect(hwnd_, nullptr, FALSE); return 0;
        }
        case WM_MOVE:
            compositor_.UpdateTextRenderingParams(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST)); return 0;
        case WM_SETTINGCHANGE:
        case WM_THEMECHANGED:
            RefreshMotion(); backdrop_ = ApplyBackdrop(hwnd_, dark_); InvalidateRect(hwnd_, nullptr, FALSE); return 0;
        case WM_SHOWWINDOW:
            if (!wparam) {
                StopMotion(); CancelDrag();
                hover_ = 0; hover_command_ = 0;
            }
            break;
        case WM_SETFOCUS:
        case WM_KILLFOCUS:
            InvalidateRect(hwnd_, nullptr, FALSE); return 0;
        case WM_GETDLGCODE: return DLGC_WANTALLKEYS;
        case WM_KEYDOWN: {
            keyboard_cues_ = true;
            const bool current = focus_ == kCurrent;
            if (wparam == VK_ESCAPE) { if (drag_command_ || scrollbar_drag_) CancelDrag(); else Complete(false); }
            else if (wparam == VK_TAB) MoveFocus(GetKeyState(VK_SHIFT) < 0);
            else if ((focus_ == kAvailable || current) && (wparam == VK_UP || wparam == VK_DOWN)) {
                if (current && (GetKeyState(VK_MENU) < 0 || GetKeyState(VK_CONTROL) < 0)) MoveSelected(wparam == VK_UP ? -1 : 1);
                else MoveListSelection(current, wparam == VK_UP ? -1 : 1);
            } else if ((focus_ == kAvailable || current) && (wparam == VK_HOME || wparam == VK_END)) MoveListSelection(current, wparam == VK_HOME ? -1 : 1, true);
            else if ((focus_ == kAvailable || current) && (wparam == VK_NEXT || wparam == VK_PRIOR)) {
                const int page = std::max(1, static_cast<int>((current_.bottom-current_.top)/RowHeight())-1);
                MoveListSelection(current, wparam == VK_NEXT ? page : -page);
            } else if (current && wparam == VK_DELETE) Invoke(kRemove);
            else if (wparam == VK_RETURN || wparam == VK_SPACE) Invoke(focus_ == kAvailable ? kAdd : current ? kRemove : focus_);
            if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE); return 0;
        }
        case WM_SYSKEYDOWN:
            if (focus_ == kCurrent && (wparam == VK_UP || wparam == VK_DOWN)) {
                keyboard_cues_ = true;
                MoveSelected(wparam == VK_UP ? -1 : 1); InvalidateRect(hwnd_, nullptr, FALSE); return 0;
            }
            break;
        case WM_COMMAND: Invoke(LOWORD(wparam)); return 0;
        case WM_MOUSEWHEEL: {
            if (scrollbar_drag_) return 0;
            POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)}; ScreenToClient(hwnd_, &point);
            const bool current = ContainsRect(current_, static_cast<float>(point.x), static_cast<float>(point.y));
            if (!current && !ContainsRect(available_, static_cast<float>(point.x), static_cast<float>(point.y))) return 0;
            pointer_x_ = static_cast<float>(point.x); pointer_y_ = static_cast<float>(point.y);
            UpdateHover(pointer_x_, pointer_y_);
            ScrollList(current, -static_cast<float>(GET_WHEEL_DELTA_WPARAM(wparam))/WHEEL_DELTA*RowHeight()*3);
            return 0;
        }
        case WM_MOUSEMOVE: {
            pointer_x_ = static_cast<float>(GET_X_LPARAM(lparam));
            pointer_y_ = static_cast<float>(GET_Y_LPARAM(lparam));
            if (scrollbar_drag_) DragScrollbar(pointer_y_);
            UpdateHover(pointer_x_, pointer_y_);
            if (drag_command_) UpdateDrag(pointer_x_, pointer_y_, false);
            TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, hwnd_, 0}; TrackMouseEvent(&track);
            InvalidateRect(hwnd_, nullptr, FALSE); return 0;
        }
        case WM_MOUSELEAVE:
            hover_ = 0; hover_command_ = 0; InvalidateRect(hwnd_, nullptr, FALSE); return 0;
        case WM_LBUTTONDOWN: {
            StopScroll();
            const float x = static_cast<float>(GET_X_LPARAM(lparam)), y = static_cast<float>(GET_Y_LPARAM(lparam));
            pointer_x_ = x; pointer_y_ = y;
            UpdateHover(x, y);
            keyboard_cues_ = false;
            pressed_ = hover_;
            if (!pressed_ || !Enabled(pressed_)) {
                pressed_ = 0; InvalidateRect(hwnd_, nullptr, FALSE); return 0;
            }
            if (pressed_ != kClose) focus_ = pressed_;
            SetFocus(hwnd_); SetCapture(hwnd_);
            if (pressed_ == kAvailable || pressed_ == kCurrent) {
                const auto thumb = ScrollbarThumb(pressed_ == kCurrent);
                if (ContainsRect(thumb, x, y)) {
                    scrollbar_drag_ = pressed_;
                    scrollbar_grab_ = y - thumb.top;
                    InvalidateRect(hwnd_, nullptr, FALSE); return 0;
                }
                pressed_command_ = hover_command_;
            }
            if (pressed_ == kAvailable && pressed_command_) available_selection_ = pressed_command_;
            if (pressed_ == kCurrent) {
                const int command = pressed_command_;
                if (command) {
                    current_selection_ = command; drag_command_ = command; drag_start_ = {static_cast<LONG>(x), static_cast<LONG>(y)};
                    SetTimer(hwnd_, kDragTimer, 60, nullptr);
                }
            }
            InvalidateRect(hwnd_, nullptr, FALSE); return 0;
        }
        case WM_LBUTTONUP: {
            const float x = static_cast<float>(GET_X_LPARAM(lparam)), y = static_cast<float>(GET_Y_LPARAM(lparam));
            const int pressed = pressed_; const bool was_dragging = dragging_;
            if (was_dragging && ContainsRect(current_, x, y)) model_.Move(drag_command_, drop_before_);
            if (scrollbar_drag_) DragScrollbar(y);
            CancelDrag();
            pointer_x_ = x; pointer_y_ = y;
            UpdateHover(x, y);
            if (!was_dragging && pressed && pressed != kAvailable && pressed != kCurrent && pressed == hover_) Invoke(pressed);
            if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE); return 0;
        }
        case WM_TIMER:
            if (wparam == kFrameTimer) {
                KillTimer(hwnd_, kFrameTimer); TickFrame(); return 0;
            }
            if (wparam == kDragTimer && dragging_) {
                POINT point{}; GetCursorPos(&point); ScreenToClient(hwnd_, &point);
                UpdateDrag(static_cast<float>(point.x), static_cast<float>(point.y), true);
            }
            return 0;
        case WM_CAPTURECHANGED:
        case WM_CANCELMODE:
            if (drag_command_ || pressed_) { CancelDrag(); InvalidateRect(hwnd_, nullptr, FALSE); } return 0;
        case WM_CLOSE: Complete(false); return 0;
        case WM_PAINT: {
            PAINTSTRUCT paint{}; BeginPaint(hwnd_, &paint); Render(); EndPaint(hwnd_, &paint); return 0;
        }
        case kToolbarCustomizationInspectMessage:
            if (lparam) {
                auto& info = *reinterpret_cast<ToolbarCustomizationInspection*>(lparam);
                info = {}; info.scale = scale_; info.available = available_; info.current = current_;
                info.add = add_; info.remove = remove_; info.up = up_; info.down = down_; info.restore = restore_;
                info.primary = primary_; info.cancel = cancel_; info.close = DialogCloseRect(Width(), scale_);
                info.focus = focus_; info.available_selection = available_selection_; info.current_selection = current_selection_;
                info.dragging_command = dragging_ ? drag_command_ : 0; info.add_enabled = Enabled(kAdd); info.remove_enabled = Enabled(kRemove);
                info.captured = GetCapture() == hwnd_;
                info.available_scroll = available_scroll_; info.current_scroll = current_scroll_;
                info.available_scrolling = available_motion_.Pending(); info.current_scrolling = current_motion_.Pending();
                info.motion_enabled = motion_enabled_;
                info.available_thumb = ScrollbarThumb(false); info.current_thumb = ScrollbarThumb(true);
                if (auto motion = control_motion_.find(kAvailable); motion != control_motion_.end())
                    info.available_focus_opacity = motion->second.focus.value;
                if (auto motion = control_motion_.find(IDOK); motion != control_motion_.end())
                    info.primary_hover_opacity = motion->second.hover.value;
                if (auto motion = control_motion_.find(kClose); motion != control_motion_.end())
                    info.close_hover_opacity = motion->second.hover.value;
                info.interaction_pending = interaction_pending_;
                info.order = model_.Order();
                const auto inspect_list = [&](bool current, auto& output) {
                    const auto viewport = current ? current_ : available_;
                    const auto& row_motion = current ? current_row_motion_ : available_row_motion_;
                    const size_t count = current ? model_.Order().size() : rows_.size();
                    for (size_t i = 0; i < count; ++i) {
                        const auto bounds = RowRect(current, i);
                        if (bounds.bottom <= viewport.top || bounds.top >= viewport.bottom) continue;
                        const int command = current ? model_.Order()[i] : rows_[i].action ? rows_[i].action->command : 0;
                        auto& row = output.emplace_back(command, bounds, command == 0);
                        if (auto motion = row_motion.find(command); motion != row_motion.end()) {
                            row.hover_opacity = motion->second.hover.value;
                            row.selection_opacity = motion->second.selection.value;
                        }
                    }
                };
                inspect_list(false, info.available_rows);
                inspect_list(true, info.current_rows);
                return TRUE;
            }
            return FALSE;
        case kToolbarCustomizationSnapshotMessage:
            return lparam && Render(false) && compositor_.SaveSnapshot(reinterpret_cast<const wchar_t*>(lparam));
        case WM_ERASEBKGND: return 1;
        case WM_DESTROY:
            done_ = true; StopMotion(); KillTimer(hwnd_, kDragTimer); compositor_.Shutdown();
            // A sent close can run inside GetMessage after the last animation callback is cancelled.
            PostThreadMessageW(GetCurrentThreadId(), WM_NULL, 0, 0);
            return 0;
        case WM_NCDESTROY: {
            const HWND hwnd = hwnd_; hwnd_ = nullptr; SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            return DefWindowProcW(hwnd, message, wparam, lparam);
        }
        }
        return DefWindowProcW(hwnd_, message, wparam, lparam);
    }

    const ToolbarCustomizationSpec& spec_;
    ToolbarCustomizationModel model_;
    ToolbarCustomizationResult result_;
    std::vector<AvailableRow> rows_;
    HWND hwnd_ = nullptr, modal_owner_ = nullptr;
    Compositor compositor_;
    fluent::Painter painter_;
    ComPtr<ID2D1SolidColorBrush> icon_brush_;
    ComPtr<ID2D1StrokeStyle> icon_stroke_;
    SmoothScroll available_motion_, current_motion_;
    EditFrameScheduler frame_scheduler_;
    std::unordered_map<int, ControlMotion> control_motion_;
    std::unordered_map<int, RowMotion> available_row_motion_, current_row_motion_;
    uint64_t frame_now_ = 0;
    D2D1_COLOR_F accent_{};
    D2D1_RECT_F available_{}, current_{}, add_{}, remove_{}, up_{}, down_{}, restore_{}, primary_{}, cancel_{};
    float scale_ = 1.0f, available_scroll_ = 0, current_scroll_ = 0;
    float pointer_x_ = 0.0f, pointer_y_ = 0.0f, scrollbar_grab_ = 0.0f;
    bool dark_ = false, backdrop_ = false, done_ = false, dragging_ = false, motion_enabled_ = true;
    bool keyboard_cues_ = true, interaction_pending_ = false, snap_motion_ = false;
    int available_selection_ = 0, current_selection_ = 0, focus_ = kAvailable, hover_ = 0, hover_command_ = 0,
        pressed_ = 0, pressed_command_ = 0, scrollbar_drag_ = 0, drag_command_ = 0, drop_before_ = 0, drop_index_ = 0;
    POINT drag_start_{};
};
} // namespace

ToolbarCustomizationResult ShowToolbarCustomization(HWND owner, const ToolbarCustomizationSpec& spec,
    bool dark, D2D1_COLOR_F accent) {
    ToolbarWindow window(spec);
    return window.Show(owner, dark, accent);
}
} // namespace pulse::ui
