// SPDX-License-Identifier: Apache-2.0
#include "file_picker_dialog.h"

#include "animation_clock.h"
#include "dialog_keyboard.h"
#include "dialog_style.h"
#include "edit_host.h"
#include "empty_state_layout.h"
#include "file_conflict_prompt.h"
#include "file_picker_commands.h"
#include "file_picker_listing.h"
#include "file_picker_tree.h"
#include "file_view_menu.h"
#include "fluent_menu.h"
#include "typography.h"
#include "ui_renderer.h"
#include "window_helpers.h"
#include "../common/display_path.h"
#include "../common/file_name.h"
#include "../common/file_operation_error.h"
#include "../common/localization.h"
#include "../common/windows_compat.h"
#include "../fs/fs_enum.h"
#include "../ops/clipboard.h"
#include "../ops/picker_file_operations.h"
#include "../common/resource_ids.h"

#include <commctrl.h>
#include <shlobj.h>
#include <uxtheme.h>
#include <windowsx.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cwctype>
#include <memory>
#include <set>
#include <unordered_set>
#include <utility>

namespace pulse::ui {
namespace {

using l10n::Get;
using l10n::StringId;
constexpr wchar_t kPickerClass[] = L"PulseFilePicker";
constexpr wchar_t kPickerSizeKey[] = L"Software\\Pulse";
constexpr wchar_t kOpenPickerSizeValue[] = L"OpenFilePickerSizeDip";
constexpr UINT_PTR kWatchTimer = 1;
constexpr UINT_PTR kFrameTimer = 2;
constexpr float kMinimumWidth = 500.0f, kMinimumHeight = 372.0f;

SIZE PreferredSize(PickerMode mode) {
    if (mode != PickerMode::Open) return {1000, 680};
    SIZE size{};
    DWORD bytes = sizeof(size);
    if (RegGetValueW(HKEY_CURRENT_USER, kPickerSizeKey, kOpenPickerSizeValue, RRF_RT_REG_BINARY,
            nullptr, &size, &bytes) == ERROR_SUCCESS && bytes == sizeof(size) &&
        size.cx > 0 && size.cx <= MAXWORD && size.cy > 0 && size.cy <= MAXWORD) return size;
    return {1120, 760};
}

enum Control {
    None, Close, Back, Up, Address,
    ToolbarFirst, ToolbarLast = ToolbarFirst + static_cast<int>(kPickerToolbar.size()) - 1,
    Sort, Group, QueryToggle, View, Query, Locations,
    Files, Name, Filter, WindowsDialog, CustomButton, Cancel, Primary, Count
};

constexpr StringId ViewToolLabel(Control control) {
    switch (control) {
    case Sort: return StringId::ToolbarSort;
    case Group: return StringId::ToolbarGroup;
    case QueryToggle: return StringId::ToolbarFilter;
    case View: return StringId::View;
    default: return {};
    }
}

constexpr const wchar_t* ViewToolGlyph(Control control) {
    return control == Sort ? L"\xE8CB" : control == Group ? L"\xF168" : L"\xE71C";
}

PickerCommand ToolbarCommand(int id) {
    if (id < ToolbarFirst || id > ToolbarLast) return PickerCommand::None;
    return kPickerToolbar[static_cast<size_t>(id - ToolbarFirst)];
}

std::wstring EditValue(HWND edit) {
    std::wstring text(static_cast<size_t>(GetWindowTextLengthW(edit)) + 1, L'\0');
    text.resize(static_cast<size_t>(GetWindowTextW(edit, text.data(), static_cast<int>(text.size()))));
    return text;
}

std::wstring_view Trim(std::wstring_view text) {
    while (!text.empty() && std::iswspace(text.front())) text.remove_prefix(1);
    while (!text.empty() && std::iswspace(text.back())) text.remove_suffix(1);
    return text;
}

std::wstring ResolvePath(std::wstring_view folder, std::wstring_view input) {
    input = Trim(input);
    if (input.empty()) return {};
    std::wstring target(input);
    std::replace(target.begin(), target.end(), L'/', L'\\');
    if (target.starts_with(L"\\\\.\\")) return {};
    const bool has_drive = target.size() >= 2 && target[1] == L':';
    if (has_drive) {
        if (target.size() == 2) target += L'\\';
        if (target[2] != L'\\') return {};
    } else if (!target.starts_with(L"\\\\")) {
        if (target.starts_with(L'\\')) {
            if (folder.size() < 3 || folder[1] != L':') return {};
            target.insert(0, folder.substr(0, 2));
        } else {
            if (folder.empty()) return {};
            if (folder.back() != L'\\') target.insert(0, 1, L'\\');
            target.insert(0, folder);
        }
    }
    return path::FriendlyPathText(fs::NormalizePath(std::move(target)));
}

std::wstring UpDirectory(const std::wstring& directory) {
    if (directory.starts_with(L"\\\\")) {
        const size_t host_end = directory.find(L'\\', 2);
        if (host_end == std::wstring::npos || host_end + 1 == directory.size()) return {};
        const size_t share_end = directory.find(L'\\', host_end + 1);
        if (share_end == std::wstring::npos || share_end + 1 == directory.size()) return directory.substr(0, host_end);
    }
    auto parent = path::FriendlyPathText(fs::ParentPath(directory));
    return parent == directory ? std::wstring{} : parent;
}

bool ParseNames(std::wstring_view text, bool multiple, std::vector<std::wstring>& names) {
    text = Trim(text);
    if (text.empty()) return false;
    if (text.find(L'"') == std::wstring_view::npos) {
        names.emplace_back(text);
        return true;
    }
    while (!text.empty()) {
        if (text.front() != L'"' || (!multiple && !names.empty())) return false;
        const size_t end = text.find(L'"', 1);
        if (end < 2 || end == std::wstring_view::npos) return false;
        names.emplace_back(text.substr(1, end - 1));
        text.remove_prefix(end + 1);
        if (!text.empty() && !std::iswspace(text.front())) return false;
        text = Trim(text);
    }
    return true;
}

class PickerWindow {
public:
    PickerWindow(const FilePickerSpec& spec, bool dark, D2D1_COLOR_F accent)
        : spec_(spec), accent_(accent), dark_(dark), has_name_(spec.mode != PickerMode::Folder),
          multiple_(spec.mode == PickerMode::Open && spec.allow_multiselect) {
        const auto action = spec.mode == PickerMode::Folder ? StringId::PickerSelectFolder
            : spec.mode == PickerMode::Save ? StringId::PickerSave : StringId::Open;
        primary_label_ = spec.ok_button_label.empty() ? Get(action) : spec.ok_button_label;
        title_ = spec.title.empty() ? Get(action) : spec.title;
        cancel_label_ = Get(StringId::Cancel);
        windows_label_ = Get(StringId::PickerWindowsDialog);
        name_label_ = spec.file_name_label.empty() ? Get(StringId::PickerFileName) : spec.file_name_label;
        file_view_.show_item_actions = false;
        file_view_.curated_order = true;
        file_view_.selected_indices = &selection_;
        if (has_name_ && !spec.filters.empty())
            applied_filter_ = filter_index_ = std::min(spec.filter_index, spec.filters.size() - 1);
    }

    ~PickerWindow() {
        if (file_operation_ && !file_operation_->Cancel(*operations_)) {
            while (!file_operation_->ready.load(std::memory_order_acquire)) {
                MsgWaitForMultipleObjectsEx(0, nullptr, 50, QS_SENDMESSAGE, MWMO_INPUTAVAILABLE);
                MSG message{};
                PeekMessageW(&message, nullptr, 0, 0, PM_NOREMOVE | PM_QS_SENDMESSAGE);
            }
        }
        if (operations_) operations_->Manager().Stop();
        if (window_) DestroyWindow(window_);
        if (font_) DeleteObject(font_);
        if (edit_brush_) DeleteObject(edit_brush_);
    }

    bool Show(HWND source, FilePickerResult& result) {
        source_ = source;
        const HMONITOR target_monitor = MonitorFromWindow(source, MONITOR_DEFAULTTONEAREST);
        UINT dpi = 0, vertical_dpi = 0;
        if (FAILED(GetDpiForMonitor(target_monitor, MDT_EFFECTIVE_DPI, &dpi, &vertical_dpi)) || !dpi)
            dpi = compat::WindowDpi(source ? source : GetDesktopWindow());
        scale_ = dpi / 96.0f;
        WNDCLASSEXW existing{sizeof(existing)};
        const auto instance = GetModuleHandleW(nullptr);
        if (!GetClassInfoExW(instance, kPickerClass, &existing)) {
            WNDCLASSEXW wc{sizeof(wc)};
            wc.hInstance = instance;
            wc.lpfnWndProc = WindowProc;
            wc.lpszClassName = kPickerClass;
            wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            wc.style = CS_DBLCLKS;
            if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
        }
        MONITORINFO monitor{sizeof(monitor)};
        const bool has_work_area = GetMonitorInfoW(target_monitor, &monitor) != FALSE;
        const int work_width = static_cast<int>(monitor.rcWork.right - monitor.rcWork.left);
        const int work_height = static_cast<int>(monitor.rcWork.bottom - monitor.rcWork.top);
        const auto preferred = PreferredSize(spec_.mode);
        RECT source_bounds{};
        if (spec_.cover_source) GetWindowRect(source, &source_bounds);
        // Cover the native surface at startup without changing its visibility or input queue.
        const int preferred_width = std::max(Pixels(std::max(kMinimumWidth, static_cast<float>(preferred.cx))),
            static_cast<int>(source_bounds.right - source_bounds.left));
        const int preferred_height = std::max(Pixels(std::max(kMinimumHeight, static_cast<float>(preferred.cy))),
            static_cast<int>(source_bounds.bottom - source_bounds.top));
        const int width = work_width > 0 ? std::min(preferred_width, work_width) : preferred_width;
        const int height = work_height > 0 ? std::min(preferred_height, work_height) : preferred_height;
        const DWORD window_style = WS_EX_NOREDIRECTIONBITMAP |
            (spec_.external_handoff && !spec_.cover_source ? WS_EX_APPWINDOW : WS_EX_TOOLWINDOW);
        if (!CreateWindowExW(window_style, kPickerClass, title_.c_str(),
            WS_POPUP | WS_SYSMENU | WS_THICKFRAME, has_work_area ? monitor.rcWork.left : CW_USEDEFAULT,
            has_work_area ? monitor.rcWork.top : CW_USEDEFAULT,
            width, height, spec_.modal_owner ? source : nullptr, nullptr, instance, this)) return false;
        CenterOwnedWindow(window_, source, width, height);
        if (spec_.should_close && spec_.should_close()) return false;
        if (!SetTimer(window_, kWatchTimer, 80, nullptr)) return false;
        ModalOwnerScope modal(spec_.modal_owner ? source : nullptr);
        modal_owner_ = modal.DisabledOwner();
        tree_.Initialize(window_, spec_.visibility, static_cast<bool>(spec_.current_pulse_directory), spec_.load_places);
        StartLoad(spec_.initial_path);
        if (spec_.external_handoff) UpdateExternalZOrder();
        ShowDialogWithFade(window_, spec_.external_handoff ? SW_SHOWNOACTIVATE : SW_SHOW);
        Layout();
        if (!spec_.cover_source || GetForegroundWindow() == source_) SetForegroundWindow(window_);
        if (spec_.external_handoff) UpdateExternalZOrder();
        if (!spec_.external_handoff || Render()) ReportReady();
        else first_frame_deadline_ = GetTickCount64() + 2500;
        MSG message{};
        while (!done_ && GetModalMessage(message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (window_ && !done_) FinishClose(pending_close_.value_or(false));
        if (accepted_) result = std::move(result_);
        return accepted_;
    }

private:
    float Px(float dip) const { return ScaleDip(scale_, dip); }
    int Pixels(float dip) const { return static_cast<int>(std::lround(Px(dip))); }

    SIZE SizeInDips() const {
        RECT bounds{};
        if (!GetWindowRect(window_, &bounds)) return {};
        return {std::lround((bounds.right - bounds.left) / static_cast<double>(scale_)),
            std::lround((bounds.bottom - bounds.top) / static_cast<double>(scale_))};
    }

    const D2D1_RECT_F& Bounds(Control id) const { return controls_[id]; }
    void Invalidate() const { if (window_) InvalidateRect(window_, nullptr, FALSE); }
    void UpdateHover(float x, float y) {
        pointer_ = D2D1::Point2F(x, y);
        const auto hovered = Hit(x, y);
        if (hovered == hover_) return;
        hover_ = hovered;
        hover_started_ = GetTickCount64();
        tooltip_painted_ = false;
    }
    size_t Size() const { return file_view_.EntryCount(); }
    const fs::DirEntry& SourceEntry(int index) const {
        return (*file_view_.snapshot)[static_cast<size_t>(index)];
    }
    const fs::DirEntry& EntryAt(int row) const {
        return SourceEntry(file_view_.SourceIndex(row));
    }
    int FindRow(const auto& matches) const {
        for (int row = 0; row < static_cast<int>(Size()); ++row)
            if (matches(EntryAt(row))) return row;
        return -1;
    }
    std::vector<int> SelectedRows() const {
        std::vector<int> rows;
        rows.reserve(selection_.size());
        for (const int source : selection_) rows.push_back(file_view_.ViewIndex(source));
        std::sort(rows.begin(), rows.end());
        return rows;
    }
    float Viewport() const { return std::max(0.0f, rows_.bottom - rows_.top); }
    bool HorizontalList() const { return file_view_.view_mode == ViewMode::List; }
    float ScrollExtent() const { return HorizontalList() ? rows_.right - rows_.left : Viewport(); }
    ViewLayout FileLayout(float offset) const {
        return file_renderer_.MakeFileLayout(file_view_, rows_, HorizontalList() ? offset : 0.0f,
            HorizontalList() ? 0.0f : offset);
    }
    float Maximum() const {
        const auto layout = FileLayout(0);
        return HorizontalList() ? layout.MaxScrollX() : layout.MaxScrollY();
    }
    bool CanPick() const { return !request_ && loaded_ && !waiting_ && !file_operation_ && !submit_locked_; }
    bool HasFilter() const { return has_name_ && !spec_.filters.empty(); }
    FileGrouping ActiveGrouping() const {
        return directory_.empty() ? FileGrouping{} :
            ResolveFileGrouping(file_view_.view_mode, file_view_.group_mode, file_view_.sort_column,
                file_view_.sort_direction, file_view_.group_direction);
    }

    void RestoreAddressText() {
        const auto display = directory_.empty() ? Get(StringId::ThisPc) : directory_;
        SetWindowTextW(address_, display.c_str());
        if (GetFocus() == address_) SendMessageW(address_, EM_SETSEL, 0, -1);
    }

    struct FolderChange {
        std::wstring directory;
        std::wstring reveal;
        bool back;
        bool reselect;
    };

    FilePickerSnapshot Snapshot() const {
        FilePickerSnapshot snapshot{directory_, EditValue(filename_), filter_index_, {}};
        for (const int row : SelectedRows()) snapshot.selections.push_back(EntryPath(row));
        return snapshot;
    }

    void StartLoad(std::wstring directory, bool back = false, std::wstring reveal = {}, bool reselect = false,
                   bool approved = false, bool project_only = false) {
        if (waiting_ || file_operation_) return;
        if (ready_ && !approved && directory != directory_ && spec_.begin_folder_change) {
            if (folder_change_ || !spec_.begin_folder_change(Snapshot(), directory)) return;
            folder_change_ = FolderChange{std::move(directory), std::move(reveal), back, reselect};
            return;
        }
        view_wheel_accum_ = 0;
        reveal_name_ = std::move(reveal);
        reselect_reveal_ = reselect;
        if (request_) request_->cancelled.store(true, std::memory_order_relaxed);
        request_.reset();
        auto request = std::make_shared<ListingRequest>();
        request->window = window_;
        request->directory = std::move(directory);
        request->mode = spec_.mode;
        request->visibility = spec_.visibility;
        request->back = back;
        request->query = query_;
        request->catalog = tree_.Catalog();
        request->folder_sort = spec_.folder_sort;
        if (spec_.folder_view && (!loaded_ || (!project_only && request->directory != directory_))) {
            const auto destination = path::FriendlyPathText(fs::NormalizePath(request->directory));
            if (!loaded_ || destination != directory_) request->folder_view = spec_.folder_view(destination);
        }
        const auto& view = request->folder_view;
        request->sort_column = view ? view->sort_column : file_view_.sort_column;
        request->sort_direction = view ? view->sort_direction : file_view_.sort_direction;
        if (!request->directory.empty()) request->grouping = view ?
            ResolveFileGrouping(view->mode, view->group_mode, view->sort_column,
                view->sort_direction, view->group_direction) : ActiveGrouping();
        request->project_only = project_only;
        if (project_only) {
            request->base = file_view_.snapshot;
            request->type_filtered = type_filtered_;
        }
        if (HasFilter()) request->filter = spec_.filters[filter_index_].pattern;
        notice_.clear();
        pressed_row_ = last_clicked_row_ = -1;
        scroll_.Reset();
        ScheduleFrame();
        if (SubmitPickerListing(request)) {
            request_ = std::move(request);
        } else {
            filter_index_ = applied_filter_;
            RestoreAddressText();
            notice_ = Get(StringId::DirectoryWorkerStartFailed);
        }
        Invalidate();
    }

    void RefreshProjection() {
        if (file_view_.snapshot && !folder_change_ && (!request_ || request_->project_only))
            StartLoad(directory_, false, {}, false, true, true);
    }

    void ApplyListing() {
        if (!request_ || !request_->ready.load(std::memory_order_acquire)) return;
        auto request = std::move(request_);
        if (!request->succeeded) {
            filter_index_ = applied_filter_;
            RestoreAddressText();
            notice_ = Get(StringId::DirectoryUnavailable);
            Invalidate();
            return;
        }
        const bool same_directory = loaded_ && directory_ == request->directory;
        std::set<std::wstring> selections;
        std::optional<std::wstring> cursor_name, first_name;
        float first_delta = 0.0f;
        if (same_directory && request->project_only && Size() && rows_.bottom > rows_.top) {
            if (cursor_ >= 0) cursor_name = EntryAt(cursor_).name;
            const auto layout = FileLayout(offset_);
            const int first = layout.VisibleRange().first;
            if (first >= 0 && static_cast<size_t>(first) < Size()) {
                first_name = EntryAt(first).name;
                const auto item = layout.ItemRect(first);
                first_delta = HorizontalList() ? item.left - rows_.left : item.top - rows_.top;
            }
        }
        if (same_directory) {
            if (reselect_reveal_) selections.insert(reveal_name_);
            else for (const int source : selection_) selections.insert(SourceEntry(source).name);
        }
        if (request->back) history_.pop_back();
        else if (loaded_ && directory_ != request->directory) history_.push_back(directory_);
        directory_ = std::move(request->directory);
        type_filtered_ = request->type_filtered;
        query_error_ = std::move(request->query_error);
        file_view_.snapshot = std::move(request->base);
        file_view_.filter_map = std::make_shared<const PaneViewModel::FilterMap>(std::move(request->projection.indices));
        file_view_.source_to_view = std::make_shared<const PaneViewModel::FilterMap>(
            std::move(request->projection.source_to_view));
        file_view_.date_groups = std::make_shared<const std::vector<DateGroup>>(std::move(request->projection.groups));
        file_view_.path = directory_;
        file_view_.is_file_system = !directory_.empty();
        file_view_.view_generation = request->token;
        filtered_ = request->filtered;
        applied_filter_ = filter_index_;
        loaded_ = true;
        tree_.Navigate(directory_, Animate());
        ScheduleFrame();
        selection_.clear();
        cursor_ = anchor_ = -1;
        pressed_row_ = last_clicked_row_ = -1;
        offset_ = 0.0f;
        scroll_.Reset();
        if (auto& view = request->folder_view; view) {
            file_view_.sort_column = view->sort_column;
            file_view_.sort_direction = view->sort_direction;
            file_view_.group_mode = view->group_mode;
            file_view_.group_direction = view->group_direction;
            file_view_.details_column_dividers = view->column_dividers;
            file_view_.details_columns = std::move(view->columns);
            if (file_view_.view_mode != view->mode) {
                file_view_.view_mode = view->mode;
                file_view_.view_mode_direct = false;
                ++file_view_.view_mode_revision;
                Layout();
            }
        }
        prefix_.clear();
        RestoreAddressText();
        if (!selections.empty()) {
            for (int row = 0; row < static_cast<int>(Size()) && !selections.empty(); ++row) {
                if (!selections.erase(EntryAt(row).name)) continue;
                selection_.insert(file_view_.SourceIndex(row));
                if (cursor_ < 0) cursor_ = anchor_ = row;
            }
        }
        if (filename_from_selection_) {
            if (selection_.empty() || SourceEntry(*selection_.begin()).is_dir) SetFilename({}, false);
            else UpdateFilenameFromSelection();
        }
        if (!reveal_name_.empty()) {
            const int found = FindRow([&](const fs::DirEntry& entry) {
                return entry.name == reveal_name_;
            });
            reveal_name_.clear();
            if (found >= 0) {
                cursor_ = found;
                if (spec_.mode == PickerMode::Open && selection_.empty() && EditValue(filename_).empty())
                    Select(cursor_, false, false);
            }
        }
        if (cursor_name || first_name) {
            const auto layout = FileLayout(0);
            for (int row = 0; row < static_cast<int>(Size()); ++row) {
                const auto& name = EntryAt(row).name;
                if (cursor_name && *cursor_name == name) cursor_ = anchor_ = row;
                if (first_name && *first_name == name) {
                    const auto item = layout.ItemRect(row);
                    offset_ = std::clamp((HorizontalList() ? item.left - rows_.left : item.top - rows_.top) -
                        first_delta, 0.0f, Maximum());
                }
            }
        } else if (cursor_ >= 0) RevealCursor();
        if (request->query != query_ || (!query_.empty() && request->catalog != tree_.Catalog()) ||
            request->sort_column != file_view_.sort_column ||
            request->sort_direction != file_view_.sort_direction ||
            request->grouping != ActiveGrouping()) RefreshProjection();
        Invalidate();
    }

    void CloseWindow(bool accepted = false) {
        if (done_ || pending_close_) return;
        if (spec_.defer_hide && spec_.defer_hide(window_, accepted)) {
            pending_close_ = accepted;
            SetWaiting(true);
            Invalidate();
            return;
        }
        FinishClose(accepted);
    }

    void FinishClose(bool accepted) {
        accepted_ = accepted;
        done_ = true;
        menu_.Dismiss();
        if (request_) request_->cancelled.store(true, std::memory_order_relaxed);
        frame_.Cancel(window_);
        KillTimer(window_, kWatchTimer);
        KillTimer(window_, kFrameTimer);
        HideComposedDialog(window_, modal_owner_);
        DestroyWindow(window_);
    }

    void UpdateExternalZOrder() {
        const HWND foreground = GetForegroundWindow();
        const HWND foreground_root = foreground ? GetAncestor(foreground, GA_ROOTOWNER) : nullptr;
        const bool picker_active = foreground == window_ || foreground_root == window_;
        const HWND source_root = source_ ? GetAncestor(source_, GA_ROOTOWNER) : nullptr;
        const bool source_active = spec_.cover_source
            ? source_ && IsWindowEnabled(source_) && (foreground == source_ || foreground == source_root)
            : source_root && foreground_root == source_root;
        const bool topmost = (GetWindowLongPtrW(window_, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
        const bool raise = source_active || (picker_active && source_ &&
            (GetWindowLongPtrW(source_, GWL_EXSTYLE) & WS_EX_TOPMOST));
        // Preserve the native owner's Z order even when Windows declines foreground transfer.
        if (topmost != raise || (source_active && foreground != last_handoff_foreground_)) {
            constexpr UINT flags = SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER;
            if (!SetWindowPos(window_, raise ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, flags)) return;
        }
        last_handoff_foreground_ = foreground;
    }

    void ReportReady() {
        ready_ = true;
        if (!spec_.external_handoff || GetForegroundWindow() == window_) FocusInitialControl();
        if (spec_.on_ready) spec_.on_ready(window_);
    }

    void FocusInitialControl() {
        if (initial_focus_) return;
        initial_focus_ = true;
        SetFocus(filename_ ? filename_ : window_);
        if (filename_) SendMessageW(filename_, EM_SETSEL, 0, -1);
    }

    void Watch() {
        if (pending_close_) {
            if (!spec_.defer_hide(window_, *pending_close_)) FinishClose(*pending_close_);
            return;
        }
        if (spec_.external_handoff) UpdateExternalZOrder();
        if (first_frame_deadline_) {
            if (Render()) {
                first_frame_deadline_ = 0;
                ReportReady();
            } else if (GetTickCount64() >= first_frame_deadline_) {
                CloseWindow();
                return;
            }
        }
        if (file_conflict_open_) return;
        if (file_operation_ && spec_.should_close && spec_.should_close()) {
            if (CancelFileOperation()) CloseWindow();
            return;
        }
        ApplyFileOperation();
        if (done_) return;
        if (file_operation_ && !cancel_after_operation_) {
            if (auto conflict = operations_->Manager().PendingConflict(); conflict && conflict->token != conflict_token_) {
                conflict_token_ = conflict->token;
                const auto still_valid = std::move(conflict->still_valid);
                conflict->still_valid = [this, still_valid] {
                    return !done_ && !cancel_after_operation_ &&
                        (!spec_.should_close || !spec_.should_close()) && (!still_valid || still_valid());
                };
                file_conflict_open_ = true;
                ResolveFileConflict(window_, operations_->Manager(), *conflict, dark_, accent_);
                file_conflict_open_ = false;
                return;
            }
        }
        ApplyListing();
        ApplyTreeListing();
        if (folder_change_ && spec_.poll_folder_change) {
            if (const auto allowed = spec_.poll_folder_change()) {
                auto change = std::exchange(folder_change_, std::nullopt);
                if (*allowed) StartLoad(std::move(change->directory), change->back, std::move(change->reveal),
                                        change->reselect, true);
                else RestoreAddressText();
            }
        }
        if (ready_ && loaded_ && spec_.on_snapshot) spec_.on_snapshot(Snapshot());
        if (operations_ && !file_operation_) {
            const auto errors = operations_->Manager().DrainShellLaunchErrors();
            if (!errors.empty()) {
                notice_ = errors.back();
                Invalidate();
            }
        }
        if (waiting_ && spec_.poll_submit) {
            if (auto reply = spec_.poll_submit()) {
                if (reply->accepted) { CloseWindow(!cancelling_); return; }
                SetWaiting(false);
                cancelling_ = false;
                submit_locked_ = !reply->retry_allowed;
                notice_ = reply->error.empty() && !reply->silent ? Get(StringId::PickerSubmitFailed) : std::move(reply->error);
                Invalidate();
            }
        }
        if (!file_operation_ && spec_.should_close && spec_.should_close()) CloseWindow();
    }

    bool CancelFileOperation() {
        if (!file_operation_) return true;
        if (!file_operation_->ready.load(std::memory_order_acquire)) {
            if (cancel_after_operation_) return false;
            if (!file_operation_->Cancel(*operations_)) {
                cancel_after_operation_ = true;
                Invalidate();
                return false;
            }
        }
        file_operation_.reset();
        return true;
    }

    void CancelPicker() {
        if (pending_close_) return;
        if (!CancelFileOperation()) return;
        if (cancelling_) return;
        if (spec_.modal_owner) { CloseWindow(); return; }
        if (!spec_.begin_cancel || !spec_.begin_cancel() || !spec_.poll_submit) {
            if (!waiting_) CloseWindow();
            return;
        }
        cancelling_ = true;
        SetWaiting(true);
        Invalidate();
    }

    void SetWaiting(bool waiting) {
        waiting_ = waiting;
        for (HWND edit : {address_, filename_, query_edit_}) if (edit) SendMessageW(edit, EM_SETREADONLY, waiting, 0);
    }

    std::wstring EntryPath(int row) const {
        const auto& entry = EntryAt(row);
        if (!entry.full_path.empty()) return path::FriendlyPathText(entry.full_path);
        if (directory_.empty()) return entry.name;
        return PathInDirectory(entry.name);
    }

    std::wstring PathInDirectory(const std::wstring& name) const {
        return directory_ + (directory_.ends_with(L'\\') ? L"" : L"\\") + name;
    }

    void UpdateClipboard() {
        clipboard_files_ = IsClipboardFormatAvailable(CF_HDROP) || ops::ClipboardHasShellFiles();
        Invalidate();
    }

    PickerCommandState CommandState() const {
        return {!done_ && loaded_ && !request_ && !waiting_ && !file_operation_,
            !directory_.empty(), selection_.size(), clipboard_files_};
    }

    void BeginFileOperation(ops::PickerFileRequest request, std::wstring reveal = {}) {
        if (!operations_) operations_ = ops::CreatePickerFileOperations();
        auto operation = std::make_shared<ops::PickerFileOperation>();
        const auto* file_request = std::get_if<ops::OpRequest>(&request);
        const bool renamed = file_request && file_request->type == ops::OpType::Rename;
        if (ops::SubmitPickerFileOperation(operations_, operation, std::move(request), window_, spec_.verify_copies)) {
            file_operation_ = std::move(operation);
            operation_reveal_ = std::move(reveal);
            operation_rename_ = renamed;
            notice_.clear();
            for (HWND edit : {address_, filename_, query_edit_}) if (edit) SendMessageW(edit, EM_SETREADONLY, TRUE, 0);
        } else notice_ = Get(StringId::OpFailed);
        Invalidate();
    }

    void ApplyFileOperation() {
        if (!file_operation_ || !file_operation_->ready.load(std::memory_order_acquire)) return;
        const auto operation = std::move(file_operation_);
        const auto result = operation->result;
        for (HWND edit : {address_, filename_, query_edit_}) if (edit) SendMessageW(edit, EM_SETREADONLY, FALSE, 0);
        if (std::exchange(cancel_after_operation_, false)) { CancelPicker(); return; }
        const bool renamed = SUCCEEDED(result) && operation_rename_ && !selection_.empty();
        if (renamed && filename_from_selection_ && !SourceEntry(*selection_.begin()).is_dir)
            SetFilename(operation_reveal_, true);
        if (operation->refresh)
            StartLoad(directory_, false, SUCCEEDED(result) ? std::move(operation_reveal_) : std::wstring{}, renamed);
        operation_reveal_.clear();
        if (FAILED(result) && result != HRESULT_FROM_WIN32(ERROR_CANCELLED))
            notice_ = Get(StringId::OpFailed) + std::wstring(L" · ") + format::FileOperationErrorText(result);
        UpdateClipboard();
    }

    void RunCommand(PickerCommand command) {
        using enum PickerCommand;
        if (!PickerCommandEnabled(command, CommandState())) return;
        if (command == Refresh) { StartLoad(directory_); return; }
        if (command == Open) { ActivateRow(file_view_.ViewIndex(*selection_.begin())); return; }
        if (command == New) {
            if (!menu_.Create(window_, &compositor_, scale_)) return;
            menu_.SetTheme(dark_, accent_);
            const auto bounds = Bounds(ToolbarFirst);
            POINT start{static_cast<LONG>(bounds.left), static_cast<LONG>(bounds.top)};
            POINT end{static_cast<LONG>(bounds.right), static_cast<LONG>(bounds.bottom)};
            ClientToScreen(window_, &start);
            ClientToScreen(window_, &end);
            const int choice = menu_.TrackDropdown({start.x, start.y, end.x, end.y}, PickerNewMenu());
            if (choice) RunCommand(static_cast<PickerCommand>(choice));
            return;
        }
        if (command == Paste) {
            BeginFileOperation(ops::PickerClipboardPaste{directory_, GetClipboardSequenceNumber()});
            return;
        }
        std::vector<std::wstring> paths;
        if (command != NewFile && command != NewFolder && command != NewText)
            for (const int row : SelectedRows()) paths.push_back(EntryPath(row));
        if (command == Cut || command == Copy || command == CopyPath) {
            BeginFileOperation(ops::PickerClipboardWrite{std::move(paths), command == Cut, command == CopyPath});
            return;
        }
        if (command == Properties) {
            BeginFileOperation(ops::PickerProperties{std::move(paths)});
            return;
        }
        ops::OpRequest request;
        if (command == Recycle) {
            request.type = ops::OpType::RecycleDelete;
            request.sources = std::move(paths);
            BeginFileOperation(std::move(request));
            return;
        }
        const bool renaming = command == Rename;
        const std::wstring original = renaming ? path::FileNameOf(paths.front()) : L"";
        const auto directory = directory_;
        auto name = PromptPickerName(window_, command, original, dark_, accent_);
        if (name.empty() || name == original || !CommandState().ready || directory_ != directory) return;
        if (command == NewText && name.find(L'.') == std::wstring::npos) name += L".txt";
        request.type = renaming ? ops::OpType::Rename : command == NewFolder ?
            ops::OpType::CreateFolder : ops::OpType::CreateTextFile;
        if (renaming) {
            request.sources = std::move(paths);
            request.new_name = name;
        } else {
            request.sources.push_back(PathInDirectory(name));
        }
        BeginFileOperation(std::move(request), std::move(name));
    }

    void ShowFileMenu(POINT point) {
        if (!CommandState().ready) return;
        bool item_menu = false;
        if (point.x == -1 && point.y == -1) {
            if (focus_ != Files && ToolbarCommand(focus_) == PickerCommand::None) return;
            item_menu = !selection_.empty();
            const auto item = item_menu && cursor_ >= 0 ? FileLayout(offset_).ItemRect(cursor_) : rows_;
            point = {static_cast<LONG>(item.left + Px(12)), static_cast<LONG>(
                std::clamp(item.top + Px(12), rows_.top, rows_.bottom))};
            ClientToScreen(window_, &point);
        } else {
            POINT client = point;
            ScreenToClient(window_, &client);
            const auto x = static_cast<float>(client.x), y = static_cast<float>(client.y);
            if (!ContainsRect(Bounds(Files), x, y) || OnScrollbar(x, y)) return;
            const int row = HitRow(x, y);
            item_menu = row >= 0;
            if (item_menu && !selection_.contains(file_view_.SourceIndex(row))) {
                const bool has_draft = spec_.mode == PickerMode::Save &&
                    !filename_from_selection_ && !EditValue(filename_).empty();
                Select(row, false, false, !has_draft);
            }
            focus_ = Files;
            SetFocus(window_);
        }
        auto state = CommandState();
        if (!item_menu) state.selected = 0;
        if (!menu_.Create(window_, &compositor_, scale_)) return;
        menu_.SetTheme(dark_, accent_);
        const int choice = menu_.TrackPopup(point, PickerContextMenu(state));
        if (choice) RunCommand(static_cast<PickerCommand>(choice));
    }

    void Submit() {
        if (!CanPick() || folder_change_) return;
        FilePickerResult candidate;
        candidate.filter_index = filter_index_;
        if (!context_selection_ && selection_.size() == 1 && SourceEntry(*selection_.begin()).is_dir) {
            auto folder = EntryPath(file_view_.ViewIndex(*selection_.begin()));
            if (has_name_) { StartLoad(std::move(folder)); return; }
            candidate.paths.push_back(std::move(folder));
        } else if (!has_name_) {
            if (!directory_.empty()) candidate.paths.push_back(directory_);
        } else if (filename_from_selection_) {
            for (const int row : SelectedRows()) {
                if (!EntryAt(row).is_dir) candidate.paths.push_back(EntryPath(row));
            }
        } else {
            std::vector<std::wstring> names;
            if (ParseNames(EditValue(filename_), CanSelectMultiple(), names)) {
                if (names.size() == 1 && names.front().find_first_of(L"\\/:") == std::wstring::npos) {
                    const int folder = FindRow([&](const fs::DirEntry& entry) {
                        return entry.is_dir && _wcsicmp(entry.name.c_str(), names.front().c_str()) == 0;
                    });
                    if (folder >= 0) {
                        SetFilename({}, false);
                        StartLoad(EntryPath(folder));
                        return;
                    }
                }
                for (const auto& name : names) {
                    auto path = ResolvePath(directory_, name);
                    if (path.empty()) { candidate.paths.clear(); break; }
                    candidate.paths.push_back(std::move(path));
                }
            }
        }
        if (candidate.paths.empty()) {
            notice_ = Get(has_name_ ? StringId::PickerNoSelection : StringId::DirectoryUnavailable);
            Invalidate();
            return;
        }
        if (spec_.on_snapshot) spec_.on_snapshot(Snapshot());
        if (spec_.begin_submit && !spec_.begin_submit(candidate)) {
            notice_ = Get(StringId::PickerSubmitFailed);
            Invalidate();
            return;
        }
        result_ = std::move(candidate);
        if (!spec_.begin_submit || !spec_.poll_submit) { CloseWindow(true); return; }
        cancelling_ = false;
        SetWaiting(true);
        notice_.clear();
        Invalidate();
    }

    void SetFilename(const std::wstring& value, bool from_selection) {
        if (!filename_) return;
        updating_filename_ = true;
        SetWindowTextW(filename_, value.c_str());
        updating_filename_ = false;
        filename_from_selection_ = from_selection;
    }

    void Select(int row, bool shift, bool toggle, bool update_filename = true) {
        if (request_ || waiting_ || file_operation_ || row < 0 || static_cast<size_t>(row) >= Size()) return;
        cursor_ = row;
        context_selection_ = !update_filename;
        const int source = file_view_.SourceIndex(row);
        if (SourceEntry(source).is_dir || !CanSelectMultiple() || (!shift && !toggle)) {
            selection_ = {source};
            anchor_ = row;
        } else if (shift) {
            if (anchor_ < 0 || EntryAt(anchor_).is_dir) anchor_ = row;
            if (!toggle) selection_.clear();
            if (!selection_.empty() && SourceEntry(*selection_.begin()).is_dir) selection_.clear();
            for (int i = std::min(anchor_, row); i <= std::max(anchor_, row); ++i) {
                if (!EntryAt(i).is_dir) selection_.insert(file_view_.SourceIndex(i));
            }
        } else {
            if (!selection_.empty() && SourceEntry(*selection_.begin()).is_dir)
                selection_.clear();
            if (!selection_.erase(source)) selection_.insert(source);
            anchor_ = row;
        }
        if (update_filename) UpdateFilenameFromSelection();
        Invalidate();
    }

    void UpdateFilenameFromSelection() {
        if (!has_name_) return;
        if (selection_.empty() || SourceEntry(*selection_.begin()).is_dir) {
            if (spec_.mode == PickerMode::Open && filename_from_selection_) SetFilename({}, false);
            return;
        }
        std::wstring value;
        for (const int row : SelectedRows()) {
            const auto& name = EntryAt(row).name;
            if (selection_.size() == 1) { value = name; break; }
            if (value.size() + name.size() + 3 > 32767) break;
            if (!value.empty()) value += L' ';
            value += L'"' + name + L'"';
        }
        SetFilename(value, true);
    }

    void ActivateRow(int row) {
        if (request_ || waiting_ || file_operation_ || row < 0 || static_cast<size_t>(row) >= Size()) return;
        if (EntryAt(row).is_dir) StartLoad(EntryPath(row));
        else { Select(row, false, false); Submit(); }
    }

    void AddressEnter() {
        const auto text = EditValue(address_);
        auto value = Trim(text);
        if (value.size() >= 2 && value.front() == L'"' && value.back() == L'"') {
            value.remove_prefix(1);
            value.remove_suffix(1);
        }
        const auto computer = Get(StringId::ThisPc);
        if (Trim(value) == computer) { StartLoad({}); return; }
        if (auto path = ResolvePath(directory_, value); !path.empty()) {
            StartLoad(std::move(path));
        } else {
            RestoreAddressText();
            notice_ = Get(StringId::DirectoryUnavailable);
            Invalidate();
        }
    }

    void ChooseFilter() {
        if (!Enabled(Filter) || !HasFilter() || !menu_.Create(window_, &compositor_, scale_)) return;
        menu_.SetTheme(dark_, accent_);
        std::vector<FluentMenuItem> items;
        for (size_t i = 0; i < spec_.filters.size(); ++i) {
            FluentMenuItem item;
            item.command = static_cast<int>(i + 1);
            item.text = spec_.filters[i].label;
            item.radio = i == filter_index_;
            item.radio_group = true;
            items.push_back(std::move(item));
        }
        const auto box = Bounds(Filter);
        POINT start{static_cast<LONG>(box.left), static_cast<LONG>(box.top)};
        POINT end{static_cast<LONG>(box.right), static_cast<LONG>(box.bottom)};
        ClientToScreen(window_, &start);
        ClientToScreen(window_, &end);
        const int choice = menu_.TrackDropdown({start.x, start.y, end.x, end.y}, std::move(items));
        if (done_ || choice <= 0 || filter_index_ == static_cast<size_t>(choice - 1)) return;
        filter_index_ = static_cast<size_t>(choice - 1);
        StartLoad(directory_);
    }

    void NavigateTree(std::optional<FilePickerTree::Target> target) {
        if (!target || waiting_) return;
        if (!target->pulse_folder) {
            StartLoad(std::move(target->directory));
            return;
        }
        auto path = spec_.current_pulse_directory();
        if (!path.empty()) StartLoad(std::move(path));
        else {
            notice_ = Get(StringId::DirectoryUnavailable);
            Invalidate();
        }
    }

    void ApplyTreeListing(UINT_PTR token = 0) {
        const bool had_catalog = static_cast<bool>(tree_.Catalog());
        if (tree_.ApplyListing(token, Animate())) {
            if (!had_catalog && tree_.Catalog() && !query_.empty()) RefreshProjection();
            ScheduleFrame();
            Invalidate();
        }
    }

    void ChooseView() {
        if (!Enabled(View) || !menu_.Create(window_, &compositor_, scale_)) return;
        menu_.SetTheme(dark_, accent_);
        auto items = BuildFileViewChoices(file_view_.view_mode, 1);
        const int count = static_cast<int>(items.size());
        const auto& bounds = Bounds(View);
        POINT start{static_cast<LONG>(bounds.left), static_cast<LONG>(bounds.top)};
        POINT end{static_cast<LONG>(bounds.right), static_cast<LONG>(bounds.bottom)};
        ClientToScreen(window_, &start);
        ClientToScreen(window_, &end);
        const int chosen = menu_.TrackDropdown({start.x, start.y, end.x, end.y}, std::move(items));
        if (!done_ && chosen > 0 && chosen <= count) {
            view_wheel_accum_ = 0;
            SetViewMode(ViewModeFromIndex(chosen - 1), false);
        }
    }

    void ChooseOrder(Control control) {
        if (!Enabled(control) || !menu_.Create(window_, &compositor_, scale_)) return;
        menu_.SetTheme(dark_, accent_);
        constexpr int base = 1;
        auto items = control == Sort
            ? BuildFileSortChoices(file_view_.sort_column, file_view_.sort_direction, base)
            : BuildFileGroupingChoices(file_view_.group_mode, file_view_.group_direction, base, base + 5, base + 6);
        const auto& box = Bounds(control);
        POINT start{static_cast<LONG>(box.left), static_cast<LONG>(box.top)};
        POINT end{static_cast<LONG>(box.right), static_cast<LONG>(box.bottom)};
        ClientToScreen(window_, &start);
        ClientToScreen(window_, &end);
        const int item = menu_.TrackDropdown({start.x, start.y, end.x, end.y}, std::move(items)) - base;
        if (done_ || item < 0 || item > 6) return;
        if (control == Sort) {
            if (item < 4) file_view_.sort_column = kFileSortColumns[item];
            else file_view_.sort_direction = item == 5 ? SortDirection::Asc : SortDirection::Desc;
        } else {
            const auto previous = ActiveGrouping();
            if (item < 5) file_view_.group_mode = kFileGroupModes[item];
            else file_view_.group_direction = item == 5 ? SortDirection::Asc : SortDirection::Desc;
            if (previous == ActiveGrouping()) return;
        }
        RefreshProjection();
    }

    void ToggleQuery() {
        if (!query_edit_ || waiting_ || file_operation_) return;
        query_open_ = !query_open_;
        if (!query_open_) {
            focus_ = QueryToggle;
            SetFocus(window_);
            SetWindowTextW(query_edit_, L"");
            ShowWindow(query_edit_, SW_HIDE);
        }
        Layout();
        if (query_open_) SetFocus(query_edit_);
        Invalidate();
    }

    void SetViewMode(ViewMode mode, bool direct) {
        if (mode == file_view_.view_mode) return;
        const auto previous_grouping = ActiveGrouping();
        const auto previous = FileLayout(offset_).Metrics();
        const int first = previous.column_major
            ? static_cast<int>(offset_ / previous.cell_width) * previous.rows_per_column
            : static_cast<int>(offset_ / previous.cell_height) * previous.columns;
        scroll_.Reset();
        offset_ = 0.0f;
        file_view_.view_mode = mode;
        file_view_.view_mode_direct = direct;
        ++file_view_.view_mode_revision;
        Layout();
        const auto item = FileLayout(0).ItemRect(first);
        offset_ = std::clamp(HorizontalList() ? item.left - rows_.left : item.top - rows_.top, 0.0f, Maximum());
        pressed_row_ = last_clicked_row_ = -1;
        if (cursor_ >= 0) RevealCursor();
        if (previous_grouping != ActiveGrouping()) RefreshProjection();
        Invalidate();
    }

    void Invoke(Control id) {
        if (id == Close || id == Cancel) { CancelPicker(); return; }
        if (file_operation_) return;
        if (id == WindowsDialog) {
            if (!waiting_) {
                if (spec_.on_use_windows) spec_.on_use_windows();
                CloseWindow();
            }
            return;
        }
        if (waiting_) return;
        if (const auto command = ToolbarCommand(id); command != PickerCommand::None) {
            RunCommand(command);
            return;
        }
        switch (id) {
        case Back:
            if (!history_.empty()) StartLoad(history_.back(), true);
            break;
        case Up:
            if (!directory_.empty()) StartLoad(UpDirectory(directory_));
            break;
        case Primary: Submit(); break;
        case CustomButton: if (spec_.on_custom_button) spec_.on_custom_button(Snapshot()); break;
        case Filter: ChooseFilter(); break;
        case Sort:
        case Group: ChooseOrder(id); break;
        case QueryToggle: ToggleQuery(); break;
        case View: ChooseView(); break;
        default: break;
        }
    }

    bool Animate() const {
        return file_renderer_.MotionEnabled() && !snap_motion_ && !done_ &&
            IsWindowVisible(window_) && !IsIconic(window_);
    }

    void ScheduleFrame() {
        if (!Animate() || (!scroll_.Pending() && !tree_.Pending() && !interaction_pending_ &&
                !file_renderer_.SurfaceAnimating())) {
            frame_.Cancel(window_);
            KillTimer(window_, kFrameTimer);
        } else if (frame_.Request(window_)) {
            KillTimer(window_, kFrameTimer);
        } else if (!SetTimer(window_, kFrameTimer, 15, nullptr)) {
            const auto now = AnimationTimeMs();
            scroll_.Tick(offset_, Maximum(), now, false);
            tree_.Tick(now, false);
            snap_motion_ = true;
            Invalidate();
        }
    }

    void TickFrame() {
        const auto now = AnimationTimeMs();
        const bool animate = Animate();
        const bool list_changed = scroll_.Tick(offset_, Maximum(), now, animate);
        if (tree_.Tick(now, animate) || list_changed || interaction_pending_ || file_renderer_.SurfaceAnimating())
            Invalidate();
        else ScheduleFrame();
    }

    void StopFrame() {
        scroll_.Reset();
        tree_.Stop();
        pointer_.reset();
        hover_ = None;
        last_clicked_row_ = -1;
        control_hover_ = {};
        file_scrollbar_hover_ = tooltip_hover_ = {};
        filename_focus_ = {};
        interaction_pending_ = false;
        snap_motion_ = false;
        frame_.Cancel(window_);
        KillTimer(window_, kFrameTimer);
    }

    void ScrollBy(float delta) {
        scroll_.ScrollBy(offset_, delta, Maximum(), ScrollExtent(), AnimationTimeMs(), Animate());
        ScheduleFrame();
        Invalidate();
    }

    void RevealCursor() {
        const auto now = AnimationTimeMs();
        scroll_.Tick(offset_, Maximum(), now, Animate());
        scroll_.Reset();
        const auto item = FileLayout(offset_).ItemRect(cursor_);
        const bool horizontal = HorizontalList();
        const float start = horizontal ? item.left - rows_.left : item.top - rows_.top;
        const float end = horizontal ? item.right - rows_.right : item.bottom - rows_.bottom;
        const float extent = ScrollExtent();
        const float length = horizontal ? item.right - item.left : item.bottom - item.top;
        const float delta = start < 0.0f || length > extent ? start : std::max(0.0f, end);
        scroll_.ScrollBy(offset_, delta, Maximum(), extent, now, Animate() && std::abs(delta) <= extent);
        ScheduleFrame();
        Invalidate();
    }

    void ListKey(WPARAM key) {
        if (request_ || waiting_ || file_operation_) return;
        if (key == VK_RETURN) {
            if (cursor_ >= 0 && EntryAt(cursor_).is_dir) ActivateRow(cursor_);
            else Submit();
            return;
        }
        if (key == VK_BACK) { Invoke(Up); return; }
        if (!Size()) return;
        const bool ctrl = GetKeyState(VK_CONTROL) < 0;
        const bool shift = GetKeyState(VK_SHIFT) < 0;
        if (key == 'A' && ctrl && CanSelectMultiple()) {
            selection_.clear();
            int first_file = -1;
            for (int row = 0; row < static_cast<int>(Size()); ++row) {
                if (EntryAt(row).is_dir) continue;
                if (first_file < 0) first_file = row;
                selection_.insert(file_view_.SourceIndex(row));
            }
            if (first_file >= 0 && !selection_.contains(file_view_.SourceIndex(cursor_))) {
                cursor_ = anchor_ = first_file;
                RevealCursor();
            }
            UpdateFilenameFromSelection();
            Invalidate();
            return;
        }
        if (key == VK_SPACE) { Select(cursor_, false, ctrl); return; }
        const auto layout = FileLayout(offset_);
        const int page = layout.PageDelta();
        int next = cursor_;
        switch (key) {
        case VK_LEFT: if (layout.Metrics().columns > 1) next = layout.MoveIndex(next, -1, 0); else return; break;
        case VK_RIGHT: if (layout.Metrics().columns > 1) next = cursor_ < 0 ? 0 : layout.MoveIndex(next, 1, 0); else return; break;
        case VK_UP: next = layout.MoveIndex(next, 0, -1); break;
        case VK_DOWN: next = cursor_ < 0 ? 0 : layout.MoveIndex(next, 0, 1); break;
        case VK_PRIOR: next -= page; break;
        case VK_NEXT: next += page; break;
        case VK_HOME: next = 0; break;
        case VK_END: next = static_cast<int>(Size()) - 1; break;
        default: return;
        }
        next = std::clamp(next, 0, static_cast<int>(Size()) - 1);
        if (ctrl && CanSelectMultiple() && !shift) cursor_ = next;
        else Select(next, shift, ctrl && shift);
        RevealCursor();
        Invalidate();
    }

    void SearchPrefix(wchar_t ch) {
        if (request_ || waiting_ || !Size() || ch <= L' ' || GetKeyState(VK_CONTROL) < 0) return;
        const auto now = AnimationTimeMs();
        if (now - prefix_time_ > 850) prefix_.clear();
        prefix_time_ = now;
        if (prefix_.size() != 1 || prefix_.front() != ch) prefix_ += ch;
        const auto size = static_cast<int>(Size());
        const int start = std::max(0, cursor_ + (prefix_.size() == 1 ? 1 : 0));
        for (int i = 0; i < size; ++i) {
            const int row = (start + i) % size;
            const auto& name = EntryAt(row).name;
            const auto count = static_cast<int>(prefix_.size());
            if (name.size() >= prefix_.size() &&
                CompareStringOrdinal(name.data(), count, prefix_.data(), count, TRUE) == CSTR_EQUAL) {
                Select(row, false, false);
                RevealCursor();
                break;
            }
        }
    }

    float LayoutFooter(float width, float height) {
        using namespace dialog_style;
        const float margin = Px(kMargin), gap = Px(kGap), control = Px(kControlHeight);
        const float bottom = height - Px(16.0f);
        LayoutDialogActions(painter_, scale_, width, bottom, primary_label_, cancel_label_,
            controls_[Primary], controls_[Cancel]);
        float footer = height - Px(kFooterHeight);
        wrap_custom_button_ = false;
        if (spec_.modal_owner) return footer;

        const auto& custom = spec_.custom_button_label;
        const float fallback_width = painter_.MeasureButtonWidth(windows_label_);
        const float custom_width = custom.empty() ? 0.0f : painter_.MeasureButtonWidth(custom);
        const float custom_gap = custom.empty() ? 0.0f : gap;
        const float desired_width = fallback_width + custom_gap + custom_width;
        float available = std::max(0.0f, Bounds(Cancel).left - gap - margin);
        float row_bottom = bottom;
        float row_height = control;
        const float extra = height - Px(kMinimumHeight);
        const float minimum_width = control + custom_gap + (custom.empty() ? 0.0f : control);
        const bool second_row = desired_width > available &&
            (extra >= control + gap || available < minimum_width);
        if (second_row) {
            available = std::max(0.0f, width - 2 * margin);
            row_bottom -= control + gap;
        }
        const float usable = std::max(0.0f, available - custom_gap);
        const float fallback = std::min(fallback_width, custom.empty() ? usable :
            std::max(usable * 0.5f, usable - control));
        const float custom_left = margin + fallback + custom_gap;
        const float custom_space = std::min(custom_width, usable - fallback);
        if (second_row && custom_space >= control) {
            const float wrapped = painter_.MeasureWrappedButtonHeight(custom, custom_space);
            if (wrapped <= std::max(control, extra - gap)) {
                row_height = wrapped;
                wrap_custom_button_ = wrapped > control;
            }
        }
        controls_[WindowsDialog] = D2D1::RectF(margin, row_bottom - control, margin + fallback, row_bottom);
        controls_[CustomButton] = D2D1::RectF(custom_left, row_bottom - row_height,
            custom_left + custom_space, row_bottom);
        if (second_row) footer -= row_height + gap;
        return footer;
    }

    void Layout() {
        if (!compositor_.Dc() || done_) return;
        using namespace dialog_style;
        const float width = static_cast<float>(compositor_.Width());
        const float height = static_cast<float>(compositor_.Height());
        const float margin = Px(kMargin), gap = Px(kGap), control = Px(kControlHeight);
        const float footer = footer_top_ = LayoutFooter(width, height);
        const float top = Px(kTitleHeight + 12.0f);
        controls_[Close] = DialogCloseRect(width, scale_);
        controls_[Back] = D2D1::RectF(margin, top, margin + control, top + control);
        controls_[Up] = D2D1::RectF(Bounds(Back).right + gap, top, Bounds(Back).right + gap + control, top + control);
        controls_[Address] = D2D1::RectF(Bounds(Up).right + gap, top, width - margin, top + control);
        const float list_top = top + control + Px(12.0f);
        const float name_top = footer - Px(44.0f);
        const float list_bottom = (has_name_ ? name_top : footer) - Px(28.0f);
        const float sidebar_width = Px(width / scale_ < 660.0f ? 176.0f : 244.0f);
        const float list_left = margin + sidebar_width + gap;
        controls_[Locations] = D2D1::RectF(margin, list_top, margin + sidebar_width, list_bottom);
        tree_.Layout(Bounds(Locations), scale_, Animate());
        controls_[View] = D2D1::RectF(width - margin - control, list_top, width - margin, list_top + control);
        float tool_right = Bounds(View).left;
        float full_tools_width = 0.0f;
        for (const auto id : {Sort, Group, QueryToggle})
            full_tools_width += painter_.MeasureButtonWidth(Get(ViewToolLabel(id)), ViewToolGlyph(id), id != QueryToggle);
        show_order_labels_ = tool_right - list_left >= full_tools_width + control * static_cast<float>(kPickerToolbar.size());
        for (const auto id : {QueryToggle, Group, Sort}) {
            const float button = show_order_labels_
                ? painter_.MeasureButtonWidth(Get(ViewToolLabel(id)), ViewToolGlyph(id), id != QueryToggle) : control;
            controls_[id] = D2D1::RectF(tool_right - button, list_top, tool_right, list_top + control);
            tool_right -= button;
        }
        const float toolbar_width = std::max(0.0f, tool_right - list_left);
        const float icon_width = std::min(control, toolbar_width / static_cast<float>(kPickerToolbar.size()));
        const auto new_button = PickerCommandPresentation(PickerCommand::New);
        const float full_new_width = painter_.MeasureButtonWidth(Get(new_button.label), new_button.glyph, true);
        const float new_width = std::min(full_new_width, toolbar_width - icon_width * (ToolbarLast - ToolbarFirst));
        show_new_label_ = new_width + 0.5f >= full_new_width;
        float x = list_left;
        for (int i = ToolbarFirst; i <= ToolbarLast; ++i) {
            const float button_width = i == ToolbarFirst ? new_width : icon_width;
            controls_[i] = D2D1::RectF(x, list_top, x + button_width, list_top + control);
            x += button_width;
        }
        float files_top = list_top + control + gap;
        controls_[Query] = {};
        if (query_open_) {
            controls_[Query] = D2D1::RectF(list_left, files_top, width - margin, files_top + control);
            files_top += control + gap;
        }
        controls_[Files] = D2D1::RectF(list_left, files_top, width - margin, list_bottom);
        rows_ = file_renderer_.FileRowsRect(FileFrame(), file_view_.view_mode);
        notice_rect_ = D2D1::RectF(list_left, list_bottom + Px(1.0f), width - margin,
            (has_name_ ? name_top : footer) - Px(1.0f));
        if (has_name_) {
            const float filter_width = Px(width / scale_ < 550.0f ? 146.0f : 198.0f);
            const float name_end = width - margin - (HasFilter() ? filter_width + gap : 0.0f);
            const float label_width = std::min(painter_.MeasureButtonWidth(name_label_) - Px(8.0f),
                std::max(Px(32.0f), name_end - margin - Px(112.0f)));
            name_label_rect_ = D2D1::RectF(margin, name_top, margin + label_width, name_top + control);
            if (HasFilter()) controls_[Filter] = D2D1::RectF(width - margin - filter_width,
                name_top, width - margin, name_top + control);
            controls_[Name] = D2D1::RectF(name_label_rect_.right, name_top, name_end, name_top + control);
        }
        scroll_.Tick(offset_, Maximum(), AnimationTimeMs(), Animate());
        if (pointer_) UpdateHover(pointer_->x, pointer_->y);
        ScheduleFrame();
        LayoutEdit(address_, Bounds(Address));
        LayoutEdit(filename_, Bounds(Name));
        if (query_open_) LayoutEdit(query_edit_, Bounds(Query));
    }

    void LayoutEdit(HWND edit, D2D1_RECT_F box) {
        if (!edit || box.right <= box.left) return;
        const int height = EditLineHeight(edit, font_, Pixels(dialog_style::kControlHeight));
        SetWindowPos(edit, HWND_TOP, static_cast<int>(std::lround(box.left + Px(8))),
            static_cast<int>(std::lround((box.top + box.bottom - height) * 0.5f)),
            static_cast<int>(std::lround(box.right - box.left - Px(16))), height,
            SWP_NOACTIVATE | SWP_SHOWWINDOW);
        PresentChildEdit(compositor_, compositor_.TextFormat(), Foreground(), Background(), edit);
    }

    void RecreateFont() {
        HFONT next = typography::CreateEditFont(scale_);
        if (!next) return;
        for (HWND edit : {address_, filename_, query_edit_}) if (edit)
            SendMessageW(edit, WM_SETFONT, reinterpret_cast<WPARAM>(next), TRUE);
        if (font_) DeleteObject(font_);
        font_ = next;
    }

    D2D1_COLOR_F Foreground() const { return ChildEditColor(ChildEditTextColor(dark_)); }
    D2D1_COLOR_F Background() const { return ChildEditColor(ChildEditBackColor(dark_)); }

    bool Enabled(Control id) const {
        if (waiting_) return !cancelling_ && (spec_.modal_owner || spec_.begin_cancel) && (id == Close || id == Cancel);
        if (file_operation_) return id == Close || id == Cancel;
        if (const auto command = ToolbarCommand(id); command != PickerCommand::None)
            return PickerCommandEnabled(command, CommandState());
        if (id == Back) return !history_.empty();
        if (id == Up) return !directory_.empty();
        if (id == Primary) return CanPick() && !folder_change_;
        if (id == Filter) return !request_;
        if (id == View) return !request_ || !request_->folder_view;
        if (id == Sort || id == Group) return loaded_ && !folder_change_ && (!request_ || request_->project_only) &&
            (id == Sort || !directory_.empty());
        return true;
    }

    float HoverOpacity(BinaryTransition& track, bool hovered) {
        track.TickHover(hovered, frame_now_, frame_animate_);
        interaction_pending_ |= track.Pending();
        return track.value;
    }

    fluent::ControlState State(Control id) {
        fluent::ControlState state{};
        state.enabled = Enabled(id);
        state.hovered = id == hover_;
        state.pressed = id == pressed_ && state.hovered;
        state.keyboard_focus = id == focus_ && GetFocus() == window_;
        state.hover_progress = HoverOpacity(control_hover_[id], state.enabled && state.hovered);
        return state;
    }

    D2D1_RECT_F FileFrame() const {
        const auto frame = Bounds(Files);
        return D2D1::RectF(frame.left + 1, frame.top + 1, frame.right - 1, frame.bottom - 1);
    }

    void DrawRows(const Theme& theme, bool high_contrast) {
        const auto frame = Bounds(Files);
        const float radius = Px(theme.radius_control);
        const bool focused = focus_ == Files && GetFocus() == window_;
        painter_.FillRoundedRect(frame, radius, theme.surface_card);
        painter_.StrokeRoundedRect(frame, radius, theme.stroke_card);
        file_view_.loading = static_cast<bool>(request_);
        file_view_.scroll_x = HorizontalList() ? offset_ : 0.0f;
        file_view_.scroll_y = HorizontalList() ? 0.0f : offset_;
        file_view_.selected_count = static_cast<int>(selection_.size());
        file_view_.hover_index = waiting_ || dragging_ || !pointer_ ? -1 : HitSource(pointer_->x, pointer_->y);
        const float scrollbar_hover = HoverOpacity(file_scrollbar_hover_, !request_ && !waiting_ &&
            (dragging_ || (pointer_ && OnScrollbar(pointer_->x, pointer_->y))));
        file_renderer_.DrawEmbeddedFileView(file_view_, FileFrame(), theme, focused, scrollbar_hover);
        if (request_ || !Size()) {
            DrawFileFocus(frame, radius, theme, focused, high_contrast);
            if (loaded_ && !request_ && Viewport() >= Px(100.0f)) {
                const auto viewport = filtered_ ? D2D1::SizeF(320.0f, 240.0f) : D2D1::SizeF(512.0f, 360.0f);
                const auto layout = MakePaneEmptyLayout(rows_, scale_, false, viewport.width / viewport.height, false);
                if (high_contrast || !painter_.DrawSvgResource(
                        filtered_ ? IDR_NO_FILTER_RESULTS_SVG : IDR_EMPTY_FOLDER_SVG, layout.art, nullptr,
                        viewport, dark_ ? 1.0f : 0.68f)) {
                    const float size = Px(36.0f);
                    const float x = (layout.art.left + layout.art.right - size) * 0.5f;
                    const float y = (layout.art.top + layout.art.bottom - size) * 0.5f;
                    painter_.DrawGlyph(filtered_ ? L"\xE721" : L"\xE8B7",
                        D2D1::RectF(x, y, x + size, y + size), theme.text_secondary);
                }
                painter_.DrawText(Get(filtered_ ? StringId::NoFilteredResults : StringId::FolderEmpty),
                    layout.title, compositor_.HeaderFormat(), theme.text, fluent::HorizontalAlignment::Center);
                return;
            }
            const auto message = request_ ? StringId::LoadingEllipsis :
                !loaded_ ? StringId::DirectoryUnavailable :
                filtered_ ? StringId::NoFilteredResults : StringId::FolderEmpty;
            painter_.DrawText(Get(message), D2D1::RectF(rows_.left + Px(14), rows_.top + Px(8),
                rows_.right - Px(14), rows_.top + Px(40)), compositor_.TextFormat(), theme.text_secondary);
            return;
        }
        if (!focused || (!high_contrast && !keyboard_file_focus_)) return;
        if (cursor_ < 0) { DrawFileFocus(frame, radius, theme, focused, high_contrast); return; }
        auto focus = FileLayout(offset_).ItemRect(cursor_);
        focus.left += Px(4);
        focus.right -= Px(4);
        focus.top += Px(1);
        focus.bottom -= Px(1);
        auto* dc = compositor_.Dc();
        dc->PushAxisAlignedClip(rows_, D2D1_ANTIALIAS_MODE_ALIASED);
        DrawFileFocus(focus, radius, theme, focused, high_contrast);
        dc->PopAxisAlignedClip();
    }

    void DrawFileFocus(const D2D1_RECT_F& bounds, float radius, const Theme& theme,
                       bool focused, bool high_contrast) {
        if (!focused) return;
        if (high_contrast) painter_.DrawFocusRing(bounds, radius);
        else if (keyboard_file_focus_) painter_.StrokeRoundedRect(bounds, radius, theme.stroke_input_bottom);
    }

    bool Render() {
        if (compositor_.NeedsRecovery()) {
            if (!compositor_.Recover()) return false;
            compositor_.RecreateTextFormats(scale_);
            painter_.SetCompositor(&compositor_);
            file_renderer_.SetCompositor(&compositor_);
            Layout();
        }
        if (!compositor_.Dc()) return false;
        const bool high_contrast = IsHighContrast();
        const auto theme = high_contrast ? MakeHighContrastTheme() : MakeTheme(dark_, accent_);
        frame_now_ = AnimationTimeMs();
        frame_animate_ = Animate();
        interaction_pending_ = false;
        BeginSurface(compositor_, painter_, theme, dark_, high_contrast, backdrop_, scale_);
        auto close = State(Close);
        if (!high_contrast) {
            close.checked = close.keyboard_focus && keyboard_file_focus_;
            close.keyboard_focus = false;
        }
        DrawDialogHeader(compositor_, painter_, theme, scale_, title_,
            spec_.mode == PickerMode::Folder ? L"\xE8B7" : L"\xE8A5", close);
        DrawDialogFooter(compositor_, painter_, theme, footer_top_);
        painter_.DrawButton({Bounds(Back), {}, L"\xE72B", fluent::ButtonKind::Transparent, State(Back), true});
        painter_.DrawButton({Bounds(Up), {}, L"\xE898", fluent::ButtonKind::Transparent, State(Up), true});
        auto address = State(Address);
        address.focused = GetFocus() == address_;
        painter_.DrawTextFieldFrame(Bounds(Address), address, true);
        for (int i = ToolbarFirst; i <= ToolbarLast; ++i) {
            const auto id = static_cast<Control>(i);
            const auto info = PickerCommandPresentation(ToolbarCommand(id));
            const bool has_label = i == ToolbarFirst && show_new_label_;
            const auto label = has_label ? std::wstring_view(Get(info.label)) : std::wstring_view{};
            painter_.DrawButton({Bounds(id), label, info.glyph, fluent::ButtonKind::Transparent,
                State(id), !has_label, i == ToolbarFirst});
        }
        painter_.DrawButton({Bounds(View), {}, ShowsColumnHeader(file_view_.view_mode) ? L"\xE8FD" : L"\xE8A9",
            fluent::ButtonKind::Transparent, State(View), true});
        for (const auto id : {Sort, Group, QueryToggle}) {
            auto state = State(id);
            state.checked = id == QueryToggle && query_open_;
            const auto label = show_order_labels_ ? std::wstring_view(Get(ViewToolLabel(id))) : std::wstring_view{};
            painter_.DrawButton({Bounds(id), label, ViewToolGlyph(id), fluent::ButtonKind::Transparent,
                state, !show_order_labels_, show_order_labels_ && id != QueryToggle});
        }
        if (query_open_) {
            auto state = State(Query);
            state.focused = GetFocus() == query_edit_;
            painter_.DrawTextFieldFrame(Bounds(Query), state, true);
        }
        interaction_pending_ |= tree_.Draw(compositor_, painter_, theme, Enabled(Locations),
            focus_ == Locations && GetFocus() == window_, high_contrast, pointer_, frame_now_, frame_animate_);
        DrawRows(theme, high_contrast);
        if (has_name_) {
            painter_.DrawText(name_label_, name_label_rect_, compositor_.TextFormat(), theme.text);
            auto name = State(Name);
            name.focused = GetFocus() == filename_;
            if (!filename_focus_.initialized) filename_focus_.Tick(name.focused, frame_now_, false, 0.0f);
            name.focus_progress = HoverOpacity(filename_focus_, name.focused);
            painter_.DrawTextFieldFrame(Bounds(Name), name, true);
            if (HasFilter()) painter_.DrawButton({Bounds(Filter), spec_.filters[filter_index_].label, {},
                fluent::ButtonKind::Standard, State(Filter), false, true});
        }
        const bool busy = waiting_ || file_operation_;
        if (busy || !notice_.empty() || !query_error_.empty()) painter_.DrawText(busy ?
            Get(cancel_after_operation_ ? StringId::OperationCenterCancelling : StringId::LoadingEllipsis) :
            (notice_.empty() ? query_error_ : notice_),
            notice_rect_, compositor_.SmallFormat(), busy ? theme.text_secondary : theme.danger);
        if (!spec_.modal_owner)
            painter_.DrawButton({Bounds(WindowsDialog), windows_label_, {}, fluent::ButtonKind::Standard, State(WindowsDialog)});
        if (!spec_.modal_owner && !spec_.custom_button_label.empty()) {
            fluent::ButtonSpec custom{Bounds(CustomButton), spec_.custom_button_label, {},
                fluent::ButtonKind::Standard, State(CustomButton)};
            custom.wrap_text = wrap_custom_button_;
            painter_.DrawButton(custom);
        }
        painter_.DrawButton({Bounds(Cancel), cancel_label_, {}, fluent::ButtonKind::Standard, State(Cancel)});
        painter_.DrawButton({Bounds(Primary), primary_label_, {}, fluent::ButtonKind::Primary, State(Primary)});
        const bool show_tooltip = (ToolbarCommand(hover_) != PickerCommand::None || (hover_ >= Sort && hover_ <= View)) &&
            pressed_ == None && !menu_.IsOpen() && GetTickCount64() - hover_started_ >= 500;
        if (show_tooltip) tooltip_control_ = hover_;
        const float tooltip_opacity = HoverOpacity(tooltip_hover_, show_tooltip);
        if (tooltip_opacity > 0.0f) {
            const auto info = tooltip_control_ >= Sort && tooltip_control_ <= View ? PickerCommandInfo{ViewToolLabel(tooltip_control_), L""} :
                PickerCommandPresentation(ToolbarCommand(tooltip_control_));
            auto text = Get(info.label);
            if (*info.shortcut) text += L" (" + std::wstring(info.shortcut) + L")";
            const auto bounds = Bounds(tooltip_control_);
            const float right = static_cast<float>(compositor_.Width()) - Px(dialog_style::kMargin);
            const float width = std::min(painter_.MeasureButtonWidth(text), right - Bounds(Files).left);
            const float left = std::min(bounds.left, right - width);
            const auto tooltip = D2D1::RectF(left, bounds.bottom + Px(2), left + width, bounds.bottom + Px(30));
            auto* dc = compositor_.Dc();
            if (tooltip_opacity < 1.0f) dc->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), nullptr,
                D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(), tooltip_opacity), nullptr);
            painter_.DrawTooltip(tooltip, text);
            if (tooltip_opacity < 1.0f) dc->PopLayer();
        }
        EndSurface(compositor_);
        const bool snapped = snap_motion_;
        ScheduleFrame();
        if (snapped) snap_motion_ = false;
        return compositor_.CanScheduleAnimationFrame();
    }

    void MoveFocus(bool backwards) {
        keyboard_file_focus_ = true;
        std::vector<DialogFocusTarget> targets;
        for (int i = Back; i < Count; ++i) {
            const auto id = static_cast<Control>(i);
            if (Enabled(id) && Bounds(id).right > Bounds(id).left)
                targets.push_back({id, id == Name ? filename_ : id == Address ? address_ : id == Query ? query_edit_ : nullptr});
        }
        targets.push_back({Close});
        MoveDialogFocus(window_, focus_, targets, backwards);
        if (focus_ == Files && cursor_ >= 0) RevealCursor();
        if (focus_ == Locations) {
            tree_.Focus(Animate());
            ScheduleFrame();
        }
        Invalidate();
    }

    bool Shortcut(WPARAM key, bool alt = false) {
        if ((key == VK_LEFT && alt) || (key == VK_UP && alt)) {
            Invoke(key == VK_LEFT ? Back : Up);
            return true;
        }
        if (GetFocus() == window_ && (focus_ == Files || ToolbarCommand(focus_) != PickerCommand::None)) {
            using enum PickerCommand;
            const bool ctrl = GetKeyState(VK_CONTROL) < 0;
            const bool shift = GetKeyState(VK_SHIFT) < 0;
            auto command = PickerCommand::None;
            if (alt && key == VK_RETURN) command = Properties;
            if (!alt) {
                if (key == VK_APPS || (key == VK_F10 && shift)) { ShowFileMenu({-1, -1}); return true; }
                if (key == VK_F2) command = Rename;
                if (key == VK_DELETE && !shift) command = Recycle;
                if (ctrl && key == 'X' && !shift) command = Cut;
                if (ctrl && key == 'C') command = shift ? CopyPath : Copy;
                if (ctrl && key == 'V' && !shift) command = Paste;
                if (ctrl && shift && key == 'N') command = NewFolder;
            }
            if (command != PickerCommand::None) { RunCommand(command); return true; }
        }
        if (alt) return false;
        if (key == 'F' && GetKeyState(VK_CONTROL) < 0) {
            if (!query_open_) ToggleQuery();
            else SetFocus(query_edit_);
            return true;
        }
        if (key == VK_F4 || (key == 'L' && GetKeyState(VK_CONTROL) < 0)) {
            SetFocus(address_);
            SendMessageW(address_, EM_SETSEL, 0, -1);
            return true;
        }
        if (key == VK_F5) { StartLoad(directory_); return true; }
        return false;
    }

    Control Hit(float x, float y) const {
        for (int i = Close; i < Count; ++i) {
            const auto id = static_cast<Control>(i);
            if (ContainsRect(Bounds(id), x, y)) return id;
        }
        return None;
    }

    bool OnScrollbar(float x, float y) const {
        D2D1_RECT_F track{}, thumb{};
        return !request_ && file_renderer_.FileScrollbarGeometry(FileLayout(0), rows_, offset_, track, thumb) &&
            ContainsRect(track, x, y);
    }

    int HitSource(float x, float y) const {
        if (request_ || !ContainsRect(rows_, x, y) || OnScrollbar(x, y)) return -1;
        return file_renderer_.FileItemFromPoint(file_view_, FileLayout(offset_), rows_, x, y);
    }

    int HitRow(float x, float y) const { return file_view_.ViewIndex(HitSource(x, y)); }

    void DragScrollbar(float x, float y) {
        D2D1_RECT_F track{}, thumb{};
        if (!file_renderer_.FileScrollbarGeometry(FileLayout(0), rows_, offset_, track, thumb)) return;
        const bool horizontal = HorizontalList();
        const float position = horizontal ? x - track.left : y - track.top;
        const float length = horizontal ? thumb.right - thumb.left : thumb.bottom - thumb.top;
        const float travel = ScrollExtent() - length;
        if (travel > 0.0f) offset_ = Maximum() * std::clamp((position - drag_offset_) / travel, 0.0f, 1.0f);
        Invalidate();
    }

    void MouseDown(float x, float y) {
        keyboard_file_focus_ = false;
        last_clicked_row_ = -1;
        UpdateHover(x, y);
        const auto id = Hit(x, y);
        if (id == Address || id == Name || id == Query) {
            SetFocus(id == Address ? address_ : id == Query ? query_edit_ : filename_);
            return;
        }
        if (!Enabled(id) || id == None) return;
        focus_ = pressed_ = id;
        if (id == Files) {
            scroll_.Reset();
            ScheduleFrame();
        } else if (id == Locations) {
            tree_.MouseDown(x, y, IsHighContrast());
            ScheduleFrame();
        }
        pressed_row_ = HitRow(x, y);
        SetFocus(window_);
        SetCapture(window_);
        D2D1_RECT_F track{}, thumb{};
        if (id == Files && !request_ && file_renderer_.FileScrollbarGeometry(FileLayout(0), rows_, offset_, track, thumb) &&
            ContainsRect(track, x, y)) {
            dragging_ = true;
            const bool horizontal = HorizontalList();
            const float point = horizontal ? x : y;
            const float first = horizontal ? thumb.left : thumb.top;
            const float last = horizontal ? thumb.right : thumb.bottom;
            drag_offset_ = point >= first && point < last ? point - first : (last - first) * 0.5f;
            DragScrollbar(x, y);
        }
        Invalidate();
    }

    void MouseUp(float x, float y) {
        const Control down = pressed_;
        const int row = pressed_row_;
        const bool drag = dragging_;
        pressed_ = None;
        pressed_row_ = -1;
        dragging_ = false;
        auto location = down == Locations ? tree_.MouseUp(x, y, Animate()) : std::nullopt;
        if (GetCapture() == window_) ReleaseCapture();
        if (down == Locations) {
            NavigateTree(std::move(location));
            ScheduleFrame();
        } else if (!drag && down != None && down == Hit(x, y)) {
            if (down != Files) Invoke(down);
            else if (row >= 0 && row == HitRow(x, y)) {
                last_clicked_row_ = row;
                Select(row, GetKeyState(VK_SHIFT) < 0, GetKeyState(VK_CONTROL) < 0);
            }
        }
        Invalidate();
    }

    HWND CreateEdit(Control id, const wchar_t* text) {
        HWND edit = CreateChildEdit(window_, text);
        if (!edit) return nullptr;
        SetWindowLongPtrW(edit, GWLP_ID, id);
        SetWindowTheme(edit, L"", L"");
        SendMessageW(edit, EM_SETLIMITTEXT, 32767, 0);
        SendMessageW(edit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(2, 2));
        if (!SetWindowSubclass(edit, EditProc, 1, reinterpret_cast<DWORD_PTR>(this))) {
            DestroyWindow(edit);
            return nullptr;
        }
        return edit;
    }

    static LRESULT CALLBACK EditProc(HWND edit, UINT message, WPARAM wparam, LPARAM lparam, UINT_PTR, DWORD_PTR ref) {
        auto& self = *reinterpret_cast<PickerWindow*>(ref);
        TrackDialogEditComposition(edit, message);
        if (!DialogEditComposing(edit)) {
            if (message == WM_SYSKEYDOWN && self.Shortcut(wparam, true)) return 0;
            if (message == WM_KEYDOWN) {
                if (self.Shortcut(wparam)) return 0;
                if (wparam == VK_RETURN) {
                    if (edit == self.address_) self.AddressEnter();
                    else if (edit == self.query_edit_) {
                        self.focus_ = Files;
                        SetFocus(self.window_);
                    }
                    else self.Submit();
                    return 0;
                }
                if (wparam == VK_ESCAPE) {
                    if (edit == self.query_edit_ && !self.waiting_ && !self.file_operation_) self.ToggleQuery();
                    else self.CancelPicker();
                    return 0;
                }
                if (wparam == VK_TAB) { self.MoveFocus(GetKeyState(VK_SHIFT) < 0); return 0; }
            }
            if (message == WM_CHAR && (wparam == VK_RETURN || wparam == VK_ESCAPE || wparam == VK_TAB)) return 0;
        }
        const auto foreground = self.Foreground();
        const auto background = self.Background();
        auto* format = self.compositor_.TextFormat();
        LRESULT result = 0;
        if (HandleChildEditMessage(self.compositor_, format, foreground, background,
            ChildEditBackBrush(self.edit_brush_), edit, message, wparam, lparam, result)) return result;
        return DefPresentedChildEditProc(self.compositor_, format, foreground, background, edit, message, wparam, lparam);
    }

    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
        auto* self = reinterpret_cast<PickerWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            self = static_cast<PickerWindow*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
            self->window_ = hwnd;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        return self ? self->Handle(message, wparam, lparam) : DefWindowProcW(hwnd, message, wparam, lparam);
    }

    LRESULT Handle(UINT message, WPARAM wparam, LPARAM lparam) {
        if (message == EditMotionFrameMessage()) {
            if (frame_.Consume(window_, wparam)) TickFrame();
            return 0;
        }
        switch (message) {
        case WM_CREATE:
            if (!compositor_.Init(window_, spec_.software_device)) return -1;
            compositor_.RecreateTextFormats(scale_);
            painter_.SetCompositor(&compositor_);
            painter_.SetScale(scale_);
            file_renderer_.SetCompositor(&compositor_);
            file_renderer_.SetScale(scale_);
            file_renderer_.SetIconNotifyWindow(window_);
            file_renderer_.SetListStyle(true, spec_.zebra_rows, false);
            backdrop_ = ApplyBackdrop(window_, dark_);
            edit_brush_ = CreateSolidBrush(ChildEditBackColor(dark_));
            address_ = CreateEdit(Address, spec_.initial_path.c_str());
            query_edit_ = CreateEdit(Query, L"");
            if (has_name_) filename_ = CreateEdit(Name, spec_.filename.c_str());
            if (!address_ || !query_edit_ || (has_name_ && !filename_)) return -1;
            SendMessageW(query_edit_, EM_SETCUEBANNER, TRUE,
                reinterpret_cast<LPARAM>(Get(StringId::FilterPlaceholder).c_str()));
            RecreateFont();
            AddClipboardFormatListener(window_);
            UpdateClipboard();
            return 0;
        case WM_NCCALCSIZE: return 0;
        case WM_NCHITTEST:
            return BorderlessHitTest(window_, lparam, Px(dialog_style::kTitleHeight), Bounds(Close));
        case WM_GETMINMAXINFO: {
            auto& info = *reinterpret_cast<MINMAXINFO*>(lparam);
            MONITORINFO monitor{sizeof(monitor)};
            GetMonitorInfoW(MonitorFromWindow(window_, MONITOR_DEFAULTTONEAREST), &monitor);
            const int work_width = static_cast<int>(monitor.rcWork.right - monitor.rcWork.left);
            const int work_height = static_cast<int>(monitor.rcWork.bottom - monitor.rcWork.top);
            info.ptMinTrackSize = {
                work_width > 0 ? std::min(Pixels(kMinimumWidth), work_width) : Pixels(kMinimumWidth),
                work_height > 0 ? std::min(Pixels(kMinimumHeight), work_height) : Pixels(kMinimumHeight)};
            return 0;
        }
        case WM_SIZE:
            if (wparam == SIZE_MINIMIZED) { StopFrame(); return 0; }
            if (compositor_.Dc()) { compositor_.Resize(LOWORD(lparam), HIWORD(lparam)); Layout(); }
            Invalidate();
            return 0;
        case WM_ENTERSIZEMOVE:
            if (spec_.mode == PickerMode::Open) resize_start_ = SizeInDips();
            return 0;
        case WM_SIZING:
            if (spec_.mode == PickerMode::Open) resized_ = true;
            return TRUE;
        case WM_EXITSIZEMOVE:
            if (resized_) {
                resized_ = false;
                const auto size = SizeInDips();
                if (size.cx > 0 && size.cy > 0 && (size.cx != resize_start_.cx || size.cy != resize_start_.cy)) {
                    RegSetKeyValueW(HKEY_CURRENT_USER, kPickerSizeKey, kOpenPickerSizeValue,
                        REG_BINARY, &size, sizeof(size));
                }
            }
            return 0;
        case WM_MOVE:
            compositor_.UpdateTextRenderingParams(MonitorFromWindow(window_, MONITOR_DEFAULTTONEAREST));
            menu_.SetScale(scale_);
            return 0;
        case WM_DPICHANGED: {
            scale_ = HIWORD(wparam) / 96.0f;
            const auto& rect = *reinterpret_cast<const RECT*>(lparam);
            compositor_.RecreateTextFormats(scale_);
            painter_.SetScale(scale_);
            file_renderer_.SetScale(scale_);
            menu_.SetScale(scale_);
            RecreateFont();
            SetWindowPos(window_, nullptr, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top,
                SWP_NOZORDER | SWP_NOACTIVATE);
            Layout();
            Invalidate();
            return 0;
        }
        case WM_SETTINGCHANGE:
        case WM_THEMECHANGED:
            file_renderer_.RefreshMotionPreferences();
            backdrop_ = ApplyBackdrop(window_, dark_);
            if (edit_brush_) DeleteObject(edit_brush_);
            edit_brush_ = CreateSolidBrush(ChildEditBackColor(dark_));
            for (HWND edit : {address_, filename_, query_edit_}) if (edit) {
                SynchronizeChildEditBackend(compositor_, edit);
                InvalidateRect(edit, nullptr, TRUE);
            }
            TickFrame();
            Invalidate();
            return 0;
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLOREDIT:
            SetTextColor(reinterpret_cast<HDC>(wparam), ChildEditTextColor(dark_));
            SetBkColor(reinterpret_cast<HDC>(wparam), ChildEditBackColor(dark_));
            return reinterpret_cast<LRESULT>(ChildEditBackBrush(edit_brush_));
        case WM_COMMAND:
            if (LOWORD(wparam) == Query && HIWORD(wparam) == EN_CHANGE && query_edit_) {
                query_ = EditValue(query_edit_);
                RefreshProjection();
            }
            if (LOWORD(wparam) == Name && HIWORD(wparam) == EN_CHANGE && !updating_filename_) {
                filename_from_selection_ = false;
                selection_.clear();
                notice_.clear();
            }
            if (HIWORD(wparam) == EN_CHANGE || HIWORD(wparam) == EN_SETFOCUS || HIWORD(wparam) == EN_KILLFOCUS)
                Invalidate();
            return 0;
        case WM_TIMER:
            if (wparam == kWatchTimer) {
                Watch();
                if (window_ && (ToolbarCommand(hover_) != PickerCommand::None || (hover_ >= Sort && hover_ <= View)) && !tooltip_painted_ &&
                    GetTickCount64() - hover_started_ >= 500) { tooltip_painted_ = true; Invalidate(); }
            }
            else if (wparam == kFrameTimer) TickFrame();
            return 0;
        case kPickerListingReady:
            if (request_ && request_->token == wparam) ApplyListing();
            else ApplyTreeListing(wparam);
            return 0;
        case WM_ACTIVATE:
            if (LOWORD(wparam) == WA_INACTIVE) menu_.DismissForDeactivation(reinterpret_cast<HWND>(lparam));
            else if (ready_ && !done_) FocusInitialControl();
            Invalidate();
            return 0;
        case WM_WINDOWPOSCHANGING:
            // Hand off after the fade, while the picker still owns the foreground.
            if (done_ && spec_.on_foreground_close &&
                (reinterpret_cast<const WINDOWPOS*>(lparam)->flags & SWP_HIDEWINDOW) && GetForegroundWindow() == window_) {
                spec_.on_foreground_close();
            }
            break;
        case WM_SYSKEYDOWN:
            if (Shortcut(wparam, true)) return 0;
            break;
        case WM_KEYDOWN:
            if (Shortcut(wparam)) return 0;
            if (wparam == VK_TAB) MoveFocus(GetKeyState(VK_SHIFT) < 0);
            else if (wparam == VK_ESCAPE) CancelPicker();
            else if (focus_ == Files) {
                keyboard_file_focus_ = true;
                ListKey(wparam);
                Invalidate();
            }
            else if (focus_ == Locations && Enabled(Locations)) {
                NavigateTree(tree_.Key(wparam, Animate()));
                ScheduleFrame();
                Invalidate();
            }
            else if (wparam == VK_RETURN || wparam == VK_SPACE) Invoke(static_cast<Control>(focus_));
            return 0;
        case WM_CHAR:
            if (focus_ == Files) SearchPrefix(static_cast<wchar_t>(wparam));
            return 0;
        case WM_MOUSEHWHEEL:
        case WM_MOUSEWHEEL: {
            POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            ScreenToClient(window_, &point);
            UINT lines = 3;
            SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
            const auto x = static_cast<float>(point.x), y = static_cast<float>(point.y);
            const float wheel = (message == WM_MOUSEHWHEEL ? 1 : -1) *
                GET_WHEEL_DELTA_WPARAM(wparam) / static_cast<float>(WHEEL_DELTA);
            if (message == WM_MOUSEWHEEL && ContainsRect(Bounds(Locations), x, y) && Enabled(Locations)) {
                tree_.Scroll(wheel, lines, Animate());
                ScheduleFrame();
                Invalidate();
                return 0;
            }
            if (request_ || !ContainsRect(Bounds(Files), x, y) ||
                (message == WM_MOUSEHWHEEL && !HorizontalList())) return 0;
            if (message == WM_MOUSEWHEEL && (GET_KEYSTATE_WPARAM(wparam) & (MK_CONTROL | MK_SHIFT))) {
                if (Enabled(View)) {
                    const int accumulated = view_wheel_accum_ + GET_WHEEL_DELTA_WPARAM(wparam);
                    const int steps = accumulated / WHEEL_DELTA;
                    view_wheel_accum_ = accumulated % WHEEL_DELTA;
                    if (steps) SetViewMode(ViewModeFromIndex(
                        std::clamp(ViewModeIndex(file_view_.view_mode) - steps, 0, 7)), true);
                }
                return 0;
            }
            view_wheel_accum_ = 0;
            const auto metrics = FileLayout(0).Metrics();
            const float step = lines == WHEEL_PAGESCROLL ? ScrollExtent() :
                (HorizontalList() ? metrics.cell_width / 3.0f : metrics.cell_height) * static_cast<float>(lines);
            if (step > 0.0f) ScrollBy(wheel * step);
            return 0;
        }
        case WM_MOUSEMOVE: {
            const float x = static_cast<float>(GET_X_LPARAM(lparam)), y = static_cast<float>(GET_Y_LPARAM(lparam));
            UpdateHover(x, y);
            tree_.MouseMove(y, IsHighContrast());
            if (dragging_) DragScrollbar(x, y);
            TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, window_, 0};
            TrackMouseEvent(&track);
            Invalidate();
            return 0;
        }
        case WM_MOUSELEAVE:
            hover_ = None;
            pointer_.reset();
            Invalidate();
            return 0;
        case WM_CLIPBOARDUPDATE: UpdateClipboard(); return 0;
        case WM_CONTEXTMENU:
            if (reinterpret_cast<HWND>(wparam) == window_) ShowFileMenu({GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)});
            return 0;
        case WM_LBUTTONDOWN:
            MouseDown(static_cast<float>(GET_X_LPARAM(lparam)), static_cast<float>(GET_Y_LPARAM(lparam)));
            return 0;
        case WM_LBUTTONUP:
            MouseUp(static_cast<float>(GET_X_LPARAM(lparam)), static_cast<float>(GET_Y_LPARAM(lparam)));
            return 0;
        case WM_LBUTTONDBLCLK: {
            const float x = static_cast<float>(GET_X_LPARAM(lparam)), y = static_cast<float>(GET_Y_LPARAM(lparam));
            const int row = HitRow(x, y);
            const int previous = std::exchange(last_clicked_row_, -1);
            if (row >= 0 && row == previous) ActivateRow(row);
            else if (const auto hit = Hit(x, y); hit == Files || hit == Locations) MouseDown(x, y);
            return 0;
        }
        case WM_CAPTURECHANGED:
        case WM_CANCELMODE:
            pressed_ = None;
            pressed_row_ = -1;
            dragging_ = false;
            tree_.CancelPointer();
            Invalidate();
            return 0;
        case WM_CLOSE: CancelPicker(); return 0;
        case WM_SHOWWINDOW:
            if (!wparam) { StopFrame(); file_renderer_.SharedThumbnailCache()->Evict(); }
            break;
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            BeginPaint(window_, &paint);
            Render();
            EndPaint(window_, &paint);
            return 0;
        }
        case WM_ERASEBKGND: return 1;
        case WM_DESTROY:
            done_ = true;
            file_renderer_.SetIconNotifyWindow(nullptr);
            RemoveClipboardFormatListener(window_);
            // GetMessage can dispatch a sent WM_CLOSE without returning a queued message.
            PostThreadMessageW(GetCurrentThreadId(), WM_NULL, 0, 0);
            if (request_) request_->cancelled.store(true, std::memory_order_relaxed);
            frame_.Cancel(window_);
            menu_.Dismiss();
            for (HWND edit : {address_, filename_, query_edit_}) if (IsWindow(edit)) DestroyWindow(edit);
            compositor_.Shutdown();
            return 0;
        case WM_NCDESTROY: {
            const HWND window = window_;
            window_ = nullptr;
            SetWindowLongPtrW(window, GWLP_USERDATA, 0);
            return DefWindowProcW(window, message, wparam, lparam);
        }
        }
        return DefWindowProcW(window_, message, wparam, lparam);
    }

    bool CanSelectMultiple() const {
        return multiple_ && (spec_.multiselect_directory.empty() ||
            path::EqualInsensitive(directory_, spec_.multiselect_directory));
    }

    const FilePickerSpec& spec_;
    D2D1_COLOR_F accent_{};
    bool dark_ = false, has_name_ = false, multiple_ = false;
    ULONGLONG first_frame_deadline_ = 0;
    HWND last_handoff_foreground_ = nullptr;
    int view_wheel_accum_ = 0;
    HWND window_ = nullptr, source_ = nullptr, modal_owner_ = nullptr, address_ = nullptr, filename_ = nullptr, query_edit_ = nullptr;
    HBRUSH edit_brush_ = nullptr;
    HFONT font_ = nullptr;
    Compositor compositor_;
    MainRenderer file_renderer_;
    fluent::Painter painter_;
    FluentMenu menu_;
    FilePickerTree tree_;
    EditFrameScheduler frame_;
    SmoothScroll scroll_;
    Listing request_;
    std::shared_ptr<ops::PickerFileOperations> operations_;
    std::shared_ptr<ops::PickerFileOperation> file_operation_;
    FilePickerResult result_;
    PaneViewModel file_view_;
    std::wstring query_, query_error_;
    std::vector<std::wstring> history_;
    std::unordered_set<int> selection_; // Indices in the directory snapshot.
    std::wstring directory_, notice_, prefix_;
    std::wstring operation_reveal_, reveal_name_;
    bool operation_rename_ = false, file_conflict_open_ = false, context_selection_ = false, reselect_reveal_ = false;
    std::wstring title_, primary_label_, cancel_label_, windows_label_, name_label_;
    std::optional<FolderChange> folder_change_;
    std::optional<bool> pending_close_;
    bool ready_ = false;
    std::array<D2D1_RECT_F, Count> controls_{};
    D2D1_RECT_F rows_{}, notice_rect_{}, name_label_rect_{};
    float footer_top_ = 0.0f;
    size_t applied_filter_ = 0, filter_index_ = 0;
    uint64_t prefix_time_ = 0, hover_started_ = 0;
    uint64_t conflict_token_ = 0;
    float scale_ = 1.0f, offset_ = 0.0f, drag_offset_ = 0.0f;
    SIZE resize_start_{};
    int focus_ = Files, cursor_ = -1, anchor_ = -1, pressed_row_ = -1;
    int last_clicked_row_ = -1;
    Control pressed_ = None, hover_ = None, tooltip_control_ = None;
    std::optional<D2D1_POINT_2F> pointer_;
    std::array<BinaryTransition, Count> control_hover_{};
    BinaryTransition file_scrollbar_hover_, tooltip_hover_, filename_focus_;
    uint64_t frame_now_ = 0;
    bool frame_animate_ = false, interaction_pending_ = false, snap_motion_ = false, keyboard_file_focus_ = false;
    bool done_ = false, accepted_ = false, backdrop_ = false, loaded_ = false, filtered_ = false, waiting_ = false;
    bool cancelling_ = false, submit_locked_ = false, updating_filename_ = false, filename_from_selection_ = false;
    bool initial_focus_ = false;
    bool dragging_ = false, resized_ = false;
    bool clipboard_files_ = false, show_new_label_ = true, tooltip_painted_ = false;
    bool type_filtered_ = false, query_open_ = false, show_order_labels_ = false;
    bool wrap_custom_button_ = false;
    bool cancel_after_operation_ = false;
};

} // namespace

bool ShowFilePicker(HWND source, const FilePickerSpec& spec, bool dark,
                    D2D1_COLOR_F accent, FilePickerResult& result) {
    return PickerWindow(spec, dark, accent).Show(source, result);
}

bool DeferFilePickerHide(HWND picker, HWND target, ULONGLONG& deadline) {
    if (!target) return false;
    const HWND foreground = GetForegroundWindow();
    const auto now = GetTickCount64();
    if (!deadline) {
        if (foreground != picker) return false;
        deadline = now + 750;
    }
    if (foreground && foreground != picker) {
        // Destroying this thread's still-active window can activate a different
        // window. Let the queued deactivation finish before closing it.
        if (foreground != target || GetActiveWindow() != picker) return false;
    }
    if (now >= deadline) return false;
    if (foreground == picker && IsWindowEnabled(target)) SetForegroundWindow(target);
    return true;
}

} // namespace pulse::ui
