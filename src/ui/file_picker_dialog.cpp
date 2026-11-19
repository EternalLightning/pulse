// SPDX-License-Identifier: Apache-2.0
#include "file_picker_dialog.h"

#include "animation_clock.h"
#include "dialog_keyboard.h"
#include "dialog_style.h"
#include "edit_host.h"
#include "file_conflict_prompt.h"
#include "file_picker_commands.h"
#include "file_picker_listing.h"
#include "file_picker_tree.h"
#include "fluent_menu.h"
#include "typography.h"
#include "window_helpers.h"
#include "../common/display_path.h"
#include "../common/file_name.h"
#include "../common/file_operation_error.h"
#include "../common/localization.h"
#include "../common/windows_compat.h"
#include "../fs/fs_enum.h"
#include "../ops/clipboard.h"
#include "../ops/picker_file_operations.h"

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
#include <utility>

namespace pulse::ui {
namespace {

using l10n::Get;
using l10n::StringId;
constexpr wchar_t kPickerClass[] = L"PulseFilePicker";
constexpr wchar_t kPickerSizeKey[] = L"Software\\Pulse";
constexpr wchar_t kOpenPickerSizeValue[] = L"OpenFilePickerSizeDip";
constexpr UINT_PTR kWatchTimer = 1;
constexpr UINT_PTR kScrollTimer = 2;
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
    Locations,
    Files, Name, Filter, WindowsDialog, Cancel, Primary, Count
};

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
        if (has_name_ && !spec.filters.empty())
            applied_filter_ = filter_index_ = std::min(spec.filter_index, spec.filters.size() - 1);
    }

    ~PickerWindow() {
        if (file_operation_) file_operation_->Cancel(*operations_);
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
        const int preferred_width = Pixels(std::max(kMinimumWidth, static_cast<float>(preferred.cx)));
        const int preferred_height = Pixels(std::max(kMinimumHeight, static_cast<float>(preferred.cy)));
        const int width = work_width > 0 ? std::min(preferred_width, work_width) : preferred_width;
        const int height = work_height > 0 ? std::min(preferred_height, work_height) : preferred_height;
        if (!CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOOLWINDOW, kPickerClass, title_.c_str(),
            WS_POPUP | WS_SYSMENU | WS_THICKFRAME, has_work_area ? monitor.rcWork.left : CW_USEDEFAULT,
            has_work_area ? monitor.rcWork.top : CW_USEDEFAULT,
            width, height, nullptr, nullptr, instance, this)) return false;
        const int measured_width = std::clamp(MinimumTrackWidth(), width, std::max(width, work_width));
        CenterOwnedWindow(window_, source, measured_width, height);
        if (spec_.should_close && spec_.should_close()) return false;
        if (!SetTimer(window_, kWatchTimer, 80, nullptr)) return false;
        tree_.Initialize(window_, spec_.show_hidden, static_cast<bool>(spec_.current_pulse_directory), spec_.load_places);
        StartLoad(spec_.initial_path);
        ShowDialogWithFade(window_);
        Layout();
        if (spec_.on_ready) spec_.on_ready(window_);
        SetForegroundWindow(window_);
        SetFocus(filename_ ? filename_ : window_);
        if (filename_) SendMessageW(filename_, EM_SETSEL, 0, -1);
        MSG message{};
        while (!done_ && GetModalMessage(message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
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
    size_t Size() const { return entries_.size(); }
    float Viewport() const { return std::max(0.0f, rows_.bottom - rows_.top); }
    float Maximum() const { return std::max(0.0f, static_cast<float>(Size()) * row_height_ - Viewport()); }
    bool CanPick() const { return !request_ && loaded_ && !waiting_ && !file_operation_ && !submit_locked_; }
    bool HasFilter() const { return has_name_ && !spec_.filters.empty(); }

    int MinimumTrackWidth() const {
        if (!compositor_.Dc()) return Pixels(kMinimumWidth);
        const float actions = painter_.MeasureButtonWidth(windows_label_) +
            std::max(Px(80.0f), painter_.MeasureButtonWidth(cancel_label_)) +
            std::max(Px(80.0f), painter_.MeasureButtonWidth(primary_label_));
        return static_cast<int>(std::ceil(std::max(Px(kMinimumWidth),
            actions + Px(2 * (dialog_style::kMargin + dialog_style::kGap)))));
    }

    void StartLoad(std::wstring directory, bool back = false, std::wstring reveal = {}) {
        if (waiting_ || file_operation_) return;
        reveal_name_ = std::move(reveal);
        if (request_) request_->cancelled.store(true, std::memory_order_relaxed);
        request_.reset();
        auto request = std::make_shared<ListingRequest>();
        request->window = window_;
        request->directory = std::move(directory);
        request->mode = spec_.mode;
        request->show_hidden = spec_.show_hidden;
        request->back = back;
        if (HasFilter()) request->filter = spec_.filters[filter_index_].pattern;
        notice_.clear();
        pressed_row_ = hover_row_ = -1;
        scroll_.Reset();
        ScheduleScroll();
        if (SubmitPickerListing(request)) {
            request_ = std::move(request);
        } else {
            filter_index_ = applied_filter_;
            notice_ = Get(StringId::DirectoryWorkerStartFailed);
        }
        Invalidate();
    }

    void ApplyListing() {
        if (!request_ || !request_->ready.load(std::memory_order_acquire)) return;
        auto request = std::move(request_);
        if (!request->succeeded) {
            filter_index_ = applied_filter_;
            notice_ = Get(StringId::DirectoryUnavailable);
            Invalidate();
            return;
        }
        std::set<std::wstring> selections;
        if (loaded_ && directory_ == request->directory) {
            for (const int row : selection_) selections.insert(entries_[static_cast<size_t>(row)].name);
        }
        if (request->back) history_.pop_back();
        else if (loaded_ && directory_ != request->directory) history_.push_back(directory_);
        directory_ = std::move(request->directory);
        entries_ = std::move(request->entries);
        filtered_ = request->filtered;
        applied_filter_ = filter_index_;
        loaded_ = true;
        tree_.Navigate(directory_, Animate());
        ScheduleScroll();
        selection_.clear();
        cursor_ = anchor_ = -1;
        pressed_row_ = hover_row_ = -1;
        offset_ = 0.0f;
        scroll_.Reset();
        prefix_.clear();
        const auto display = directory_.empty() ? Get(StringId::ThisPc) : directory_;
        SetWindowTextW(address_, display.c_str());
        if (GetFocus() == address_) SendMessageW(address_, EM_SETSEL, 0, -1);
        if (!selections.empty()) {
            for (size_t i = 0; i < entries_.size() && !selections.empty(); ++i) {
                if (selections.erase(entries_[i].name)) selection_.insert(static_cast<int>(i));
            }
            if (!selection_.empty()) cursor_ = anchor_ = *selection_.begin();
        }
        if (filename_from_selection_) {
            if (selection_.empty() || entries_[static_cast<size_t>(*selection_.begin())].is_dir) SetFilename({}, false);
            else UpdateFilenameFromSelection();
        }
        if (!reveal_name_.empty()) {
            const auto found = std::find_if(entries_.begin(), entries_.end(), [&](const fs::DirEntry& entry) {
                return entry.name == reveal_name_;
            });
            reveal_name_.clear();
            if (found != entries_.end()) {
                cursor_ = static_cast<int>(found - entries_.begin());
                if (spec_.mode == PickerMode::Open && selection_.empty() && EditValue(filename_).empty())
                    Select(cursor_, false, false);
            }
        }
        if (cursor_ >= 0) RevealCursor();
        Invalidate();
    }

    void CloseWindow(bool accepted = false) {
        if (done_) return;
        accepted_ = accepted;
        done_ = true;
        menu_.Dismiss();
        if (request_) request_->cancelled.store(true, std::memory_order_relaxed);
        frame_.Cancel(window_);
        KillTimer(window_, kWatchTimer);
        KillTimer(window_, kScrollTimer);
        HideComposedDialog(window_);
        DestroyWindow(window_);
    }

    void Watch() {
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
                submit_locked_ = !reply->retry_allowed;
                notice_ = reply->error.empty() ? Get(StringId::PickerSubmitFailed) : std::move(reply->error);
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
        if (!CancelFileOperation()) return;
        if (waiting_) return;
        if (submit_locked_ || !spec_.begin_cancel || !spec_.begin_cancel() || !spec_.poll_submit) {
            CloseWindow();
            return;
        }
        cancelling_ = true;
        SetWaiting(true);
        Invalidate();
    }

    void SetWaiting(bool waiting) {
        waiting_ = waiting;
        for (HWND edit : {address_, filename_}) if (edit) SendMessageW(edit, EM_SETREADONLY, waiting, 0);
    }

    std::wstring EntryPath(int row) const {
        const auto& entry = entries_[static_cast<size_t>(row)];
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
        const bool refresh = !std::holds_alternative<ops::PickerProperties>(request);
        if (ops::SubmitPickerFileOperation(operations_, operation, std::move(request), window_, spec_.verify_copies)) {
            file_operation_ = std::move(operation);
            operation_reveal_ = std::move(reveal);
            operation_rename_ = renamed;
            operation_refresh_ = refresh;
            notice_.clear();
            for (HWND edit : {address_, filename_}) if (edit) SendMessageW(edit, EM_SETREADONLY, TRUE, 0);
        } else notice_ = Get(StringId::OpFailed);
        Invalidate();
    }

    void ApplyFileOperation() {
        if (!file_operation_ || !file_operation_->ready.load(std::memory_order_acquire)) return;
        const auto operation = std::move(file_operation_);
        const auto result = operation->result;
        for (HWND edit : {address_, filename_}) if (edit) SendMessageW(edit, EM_SETREADONLY, FALSE, 0);
        if (std::exchange(cancel_after_operation_, false)) { CancelPicker(); return; }
        if (SUCCEEDED(result) && operation_rename_ && !selection_.empty()) {
            auto& entry = entries_[static_cast<size_t>(*selection_.begin())];
            entry.name = operation_reveal_;
            if (!entry.full_path.empty()) entry.full_path = PathInDirectory(entry.name);
            if (filename_from_selection_) UpdateFilenameFromSelection();
        }
        if (operation_refresh_)
            StartLoad(directory_, false, SUCCEEDED(result) ? std::move(operation_reveal_) : std::wstring{});
        operation_reveal_.clear();
        if (FAILED(result) && result != HRESULT_FROM_WIN32(ERROR_CANCELLED))
            notice_ = Get(StringId::OpFailed) + std::wstring(L" · ") + format::FileOperationErrorText(result);
        UpdateClipboard();
    }

    void RunCommand(PickerCommand command) {
        using enum PickerCommand;
        if (!PickerCommandEnabled(command, CommandState())) return;
        if (command == Refresh) { StartLoad(directory_); return; }
        if (command == Open) { ActivateRow(*selection_.begin()); return; }
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
            for (const int row : selection_) paths.push_back(EntryPath(row));
        if (command == Cut || command == Copy || command == CopyPath) {
            bool success = false;
            if (command == CopyPath) {
                std::wstring text;
                for (const auto& path : paths) {
                    if (!text.empty()) text += L"\r\n";
                    text += path;
                }
                success = ops::WriteClipboardText(text);
            } else success = ops::WriteClipboard(paths, command == Cut);
            notice_ = success ? std::wstring{} : Get(StringId::OpFailed);
            UpdateClipboard();
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
            const auto y = item_menu && cursor_ >= 0 ?
                std::clamp(RowTop(cursor_), rows_.top, std::max(rows_.top, rows_.bottom - row_height_)) : rows_.top;
            point = {static_cast<LONG>(rows_.left + Px(12)), static_cast<LONG>(y + Px(12))};
            ClientToScreen(window_, &point);
        } else {
            POINT client = point;
            ScreenToClient(window_, &client);
            const auto x = static_cast<float>(client.x), y = static_cast<float>(client.y);
            if (!ContainsRect(Bounds(Files), x, y) || OnScrollbar(x, y)) return;
            const int row = HitRow(x, y);
            item_menu = row >= 0;
            if (item_menu && !selection_.contains(row)) Select(row, false, false);
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
        if (!CanPick()) return;
        FilePickerResult candidate;
        candidate.filter_index = filter_index_;
        if (selection_.size() == 1 && entries_[static_cast<size_t>(*selection_.begin())].is_dir) {
            auto folder = EntryPath(*selection_.begin());
            if (has_name_) { StartLoad(std::move(folder)); return; }
            candidate.paths.push_back(std::move(folder));
        } else if (!has_name_) {
            if (!directory_.empty()) candidate.paths.push_back(directory_);
        } else if (filename_from_selection_) {
            for (const int row : selection_) candidate.paths.push_back(EntryPath(row));
        } else {
            std::vector<std::wstring> names;
            if (ParseNames(EditValue(filename_), multiple_, names)) {
                if (names.size() == 1 && names.front().find_first_of(L"\\/:") == std::wstring::npos) {
                    const auto folder = std::find_if(entries_.begin(), entries_.end(), [&](const fs::DirEntry& entry) {
                        return entry.is_dir && _wcsicmp(entry.name.c_str(), names.front().c_str()) == 0;
                    });
                    if (folder != entries_.end()) {
                        SetFilename({}, false);
                        StartLoad(EntryPath(static_cast<int>(folder - entries_.begin())));
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

    void Select(int row, bool shift, bool toggle) {
        if (request_ || waiting_ || file_operation_ || row < 0 || static_cast<size_t>(row) >= Size()) return;
        cursor_ = row;
        if (entries_[static_cast<size_t>(row)].is_dir || !multiple_ || (!shift && !toggle)) {
            selection_ = {row};
            anchor_ = row;
        } else if (shift) {
            if (anchor_ < 0 || entries_[static_cast<size_t>(anchor_)].is_dir) anchor_ = row;
            if (!toggle) selection_.clear();
            if (!selection_.empty() && entries_[static_cast<size_t>(*selection_.begin())].is_dir) selection_.clear();
            for (int i = std::min(anchor_, row); i <= std::max(anchor_, row); ++i) selection_.insert(i);
        } else {
            if (!selection_.empty() && entries_[static_cast<size_t>(*selection_.begin())].is_dir)
                selection_.clear();
            if (!selection_.erase(row)) selection_.insert(row);
            anchor_ = row;
        }
        UpdateFilenameFromSelection();
        Invalidate();
    }

    void UpdateFilenameFromSelection() {
        if (!has_name_) return;
        if (selection_.empty() || entries_[static_cast<size_t>(*selection_.begin())].is_dir) {
            if (spec_.mode == PickerMode::Open && filename_from_selection_) SetFilename({}, false);
            return;
        }
        std::wstring value;
        for (const int row : selection_) {
            const auto& name = entries_[static_cast<size_t>(row)].name;
            if (selection_.size() == 1) { value = name; break; }
            if (value.size() + name.size() + 3 > 32767) break;
            if (!value.empty()) value += L' ';
            value += L'"' + name + L'"';
        }
        SetFilename(value, true);
    }

    void ActivateRow(int row) {
        if (request_ || waiting_ || file_operation_ || row < 0 || static_cast<size_t>(row) >= Size()) return;
        if (entries_[static_cast<size_t>(row)].is_dir) StartLoad(EntryPath(row));
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
        if (tree_.ApplyListing(token, Animate())) {
            ScheduleScroll();
            Invalidate();
        }
    }

    void Invoke(Control id) {
        if (id == Close || id == Cancel) { CancelPicker(); return; }
        if (file_operation_) return;
        if (id == WindowsDialog) {
            if (!waiting_) {
                const auto source = source_;
                CloseWindow();
                if (spec_.on_use_windows) spec_.on_use_windows();
                if (IsWindow(source)) SetForegroundWindow(source);
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
        case Filter: ChooseFilter(); break;
        default: break;
        }
    }

    bool Animate() const {
        BOOL enabled = TRUE;
        SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &enabled, 0);
        return enabled && !IsHighContrast() && IsWindowVisible(window_) && !IsIconic(window_);
    }

    void ScheduleScroll() {
        if (!scroll_.Pending() && !tree_.Pending()) {
            frame_.Cancel(window_);
            KillTimer(window_, kScrollTimer);
        } else if (frame_.Request(window_)) {
            KillTimer(window_, kScrollTimer);
        } else if (!SetTimer(window_, kScrollTimer, 15, nullptr)) {
            const auto now = AnimationTimeMs();
            scroll_.Tick(offset_, Maximum(), now, false);
            tree_.Tick(now, false);
        }
    }

    void ScrollFrame() {
        const auto now = AnimationTimeMs();
        const bool animate = Animate();
        const bool list_changed = scroll_.Tick(offset_, Maximum(), now, animate);
        if (tree_.Tick(now, animate) || list_changed) Invalidate();
        ScheduleScroll();
    }

    void ScrollBy(float delta) {
        scroll_.ScrollBy(offset_, delta, Maximum(), Viewport(), AnimationTimeMs(), Animate());
        ScheduleScroll();
        Invalidate();
    }

    float RowTop(int row) const { return rows_.top + static_cast<float>(row) * row_height_ - offset_; }

    void RevealCursor() {
        const auto now = AnimationTimeMs();
        scroll_.Tick(offset_, Maximum(), now, Animate());
        scroll_.Reset();
        const float y = RowTop(cursor_);
        if (y < rows_.top) ScrollBy(y - rows_.top);
        else if (y + row_height_ > rows_.bottom) ScrollBy(y + row_height_ - rows_.bottom);
        else ScheduleScroll();
    }

    void ListKey(WPARAM key) {
        if (request_ || waiting_ || file_operation_) return;
        if (key == VK_RETURN) {
            if (cursor_ >= 0 && entries_[static_cast<size_t>(cursor_)].is_dir) ActivateRow(cursor_);
            else Submit();
            return;
        }
        if (key == VK_BACK) { Invoke(Up); return; }
        if (!Size()) return;
        const bool ctrl = GetKeyState(VK_CONTROL) < 0;
        const bool shift = GetKeyState(VK_SHIFT) < 0;
        if (key == 'A' && ctrl && multiple_) {
            selection_.clear();
            for (size_t i = 0; i < Size(); ++i) if (!entries_[i].is_dir) selection_.insert(static_cast<int>(i));
            if (!selection_.empty() && !selection_.contains(cursor_)) {
                cursor_ = anchor_ = *selection_.begin();
                RevealCursor();
            }
            UpdateFilenameFromSelection();
            Invalidate();
            return;
        }
        if (key == VK_SPACE) { Select(cursor_, false, ctrl); return; }
        const int page = std::max(1, static_cast<int>(Viewport() / row_height_) - 1);
        int next = cursor_;
        switch (key) {
        case VK_UP: --next; break;
        case VK_DOWN: ++next; break;
        case VK_PRIOR: next -= page; break;
        case VK_NEXT: next += page; break;
        case VK_HOME: next = 0; break;
        case VK_END: next = static_cast<int>(Size()) - 1; break;
        default: return;
        }
        next = std::clamp(next, 0, static_cast<int>(Size()) - 1);
        if (ctrl && multiple_ && !shift) cursor_ = next;
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
            const auto& name = entries_[static_cast<size_t>(row)].name;
            const auto count = static_cast<int>(prefix_.size());
            if (name.size() >= prefix_.size() &&
                CompareStringOrdinal(name.data(), count, prefix_.data(), count, TRUE) == CSTR_EQUAL) {
                Select(row, false, false);
                RevealCursor();
                break;
            }
        }
    }

    void Layout() {
        if (!compositor_.Dc() || done_) return;
        using namespace dialog_style;
        const float width = static_cast<float>(compositor_.Width());
        const float height = static_cast<float>(compositor_.Height());
        const float margin = Px(kMargin), gap = Px(kGap), control = Px(kControlHeight);
        const float footer = height - Px(kFooterHeight);
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
        const float toolbar_width = std::max(0.0f, width - margin - list_left);
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
        const float files_top = list_top + control + gap;
        controls_[Files] = D2D1::RectF(list_left, files_top, width - margin, list_bottom);
        rows_ = D2D1::RectF(list_left + 1, files_top + Px(28.0f), width - margin - 1, list_bottom - 1);
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
        LayoutDialogActions(painter_, scale_, width, height - Px(16.0f), primary_label_, cancel_label_,
            controls_[Primary], controls_[Cancel]);
        controls_[WindowsDialog] = D2D1::RectF(margin, Bounds(Cancel).top,
            margin + painter_.MeasureButtonWidth(windows_label_), Bounds(Cancel).bottom);
        const float overlap = Bounds(WindowsDialog).right + gap - Bounds(Cancel).left;
        if (overlap > 0.0f) {
            controls_[Cancel].left += overlap;
            controls_[Cancel].right += overlap;
            controls_[Primary].left += overlap;
        }
        row_height_ = Px(Theme{}.row_list);
        scroll_.Tick(offset_, Maximum(), AnimationTimeMs(), Animate());
        ScheduleScroll();
        LayoutEdit(address_, Bounds(Address));
        LayoutEdit(filename_, Bounds(Name));
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
        for (HWND edit : {address_, filename_}) if (edit)
            SendMessageW(edit, WM_SETFONT, reinterpret_cast<WPARAM>(next), TRUE);
        if (font_) DeleteObject(font_);
        font_ = next;
    }

    D2D1_COLOR_F Foreground() const { return ChildEditColor(ChildEditTextColor(dark_)); }
    D2D1_COLOR_F Background() const { return ChildEditColor(ChildEditBackColor(dark_)); }

    bool Enabled(Control id) const {
        if (waiting_) return false;
        if (file_operation_) return id == Close || id == Cancel;
        if (const auto command = ToolbarCommand(id); command != PickerCommand::None)
            return PickerCommandEnabled(command, CommandState());
        if (id == Back) return !history_.empty();
        if (id == Up) return !directory_.empty();
        if (id == Primary) return CanPick();
        if (id == Filter) return !request_;
        return true;
    }

    fluent::ControlState State(Control id) const {
        fluent::ControlState state{};
        state.enabled = Enabled(id);
        state.hovered = id == hover_;
        state.pressed = id == pressed_;
        state.keyboard_focus = id == focus_ && GetFocus() == window_;
        return state;
    }

    fluent::ScrollbarSpec Scrollbar() const {
        return {rows_, offset_, Viewport(), static_cast<float>(Size()) * row_height_,
            hover_scrollbar_ || dragging_ ? 1.0f : 0.0f, 1.0f, !request_};
    }

    void DrawRows(const Theme& theme, bool high_contrast) {
        const auto frame = Bounds(Files);
        const float radius = Px(theme.radius_control);
        painter_.FillRoundedRect(frame, radius, theme.surface_card);
        painter_.StrokeRoundedRect(frame, radius, theme.stroke_card);
        painter_.DrawText(Get(StringId::ColumnName), D2D1::RectF(frame.left + Px(14), frame.top,
            frame.right - Px(12), rows_.top), compositor_.SmallFormat(), theme.text_secondary);
        painter_.FillRoundedRect(D2D1::RectF(frame.left + 1, rows_.top - 1, frame.right - 1, rows_.top),
            0, theme.stroke_divider);
        if (request_ || !Size()) {
            if (focus_ == Files && GetFocus() == window_) painter_.DrawFocusRing(frame, radius);
            const auto message = request_ ? StringId::LoadingEllipsis :
                !loaded_ ? StringId::DirectoryUnavailable :
                filtered_ ? StringId::NoFilteredResults : StringId::FolderEmpty;
            painter_.DrawText(Get(message), D2D1::RectF(rows_.left + Px(14), rows_.top + Px(8),
                rows_.right - Px(14), rows_.top + Px(40)), compositor_.TextFormat(), theme.text_secondary);
            return;
        }
        if (cursor_ < 0 && focus_ == Files && GetFocus() == window_) painter_.DrawFocusRing(frame, radius);
        compositor_.Dc()->PushAxisAlignedClip(rows_, D2D1_ANTIALIAS_MODE_ALIASED);
        const int first = static_cast<int>(offset_ / row_height_);
        for (int row = first; static_cast<size_t>(row) < Size(); ++row) {
            const float y = RowTop(row);
            if (y >= rows_.bottom) break;
            const auto& entry = entries_[static_cast<size_t>(row)];
            const auto rect = D2D1::RectF(rows_.left, y, rows_.right - Px(10), y + row_height_);
            fluent::ControlState state{};
            state.selected = selection_.contains(row);
            state.hovered = hover_row_ == row && !waiting_;
            state.pressed = pressed_row_ == row;
            state.keyboard_focus = cursor_ == row && focus_ == Files && GetFocus() == window_;
            painter_.DrawListRowBackground({rect, state});
            const bool highlighted = high_contrast && (state.selected || state.hovered);
            const auto color = highlighted ? theme.text_selected : theme.text;
            const auto glyph = directory_.empty() ? L"\xE7F1" : entry.is_dir ? L"\xE8B7" : L"\xE8A5";
            const float icon = Px(16.0f);
            const float cy = (rect.top + rect.bottom - icon) * 0.5f;
            painter_.DrawGlyph(glyph, D2D1::RectF(rect.left + Px(14), cy, rect.left + Px(14) + icon, cy + icon),
                high_contrast ? color : entry.is_dir ? theme.icon_folder : theme.icon_file);
            painter_.DrawText(entry.name, D2D1::RectF(rect.left + Px(40), rect.top, rect.right - Px(8), rect.bottom),
                compositor_.FileNameFormat(), color);
        }
        compositor_.Dc()->PopAxisAlignedClip();
        painter_.DrawScrollbar(Scrollbar());
    }

    void Render() {
        if (compositor_.NeedsRecovery()) {
            if (!compositor_.Recover()) return;
            compositor_.RecreateTextFormats(scale_);
            painter_.SetCompositor(&compositor_);
            Layout();
        }
        if (!compositor_.Dc()) return;
        const bool high_contrast = IsHighContrast();
        const auto theme = high_contrast ? MakeHighContrastTheme() : MakeTheme(dark_, accent_);
        BeginSurface(compositor_, painter_, theme, dark_, high_contrast, backdrop_, scale_);
        DrawDialogHeader(compositor_, painter_, theme, scale_, title_,
            spec_.mode == PickerMode::Folder ? L"\xE8B7" : L"\xE8A5", State(Close));
        DrawDialogFooter(compositor_, painter_, theme, static_cast<float>(compositor_.Height()) - Px(dialog_style::kFooterHeight));
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
        tree_.Draw(compositor_, painter_, theme, Enabled(Locations),
            focus_ == Locations && GetFocus() == window_, high_contrast);
        DrawRows(theme, high_contrast);
        if (has_name_) {
            painter_.DrawText(name_label_, name_label_rect_, compositor_.TextFormat(), theme.text);
            auto name = State(Name);
            name.focused = GetFocus() == filename_;
            painter_.DrawTextFieldFrame(Bounds(Name), name, true);
            if (HasFilter()) painter_.DrawButton({Bounds(Filter), spec_.filters[filter_index_].label, {},
                fluent::ButtonKind::Standard, State(Filter), false, true});
        }
        const bool busy = waiting_ || file_operation_;
        if (busy || !notice_.empty()) painter_.DrawText(busy ?
            Get(cancel_after_operation_ ? StringId::OperationCenterCancelling : StringId::LoadingEllipsis) : notice_,
            notice_rect_, compositor_.SmallFormat(), busy ? theme.text_secondary : theme.danger);
        painter_.DrawButton({Bounds(WindowsDialog), windows_label_, {}, fluent::ButtonKind::Transparent, State(WindowsDialog)});
        painter_.DrawButton({Bounds(Cancel), cancel_label_, {}, fluent::ButtonKind::Standard, State(Cancel)});
        painter_.DrawButton({Bounds(Primary), primary_label_, {}, fluent::ButtonKind::Primary, State(Primary)});
        if (const auto command = ToolbarCommand(hover_); command != PickerCommand::None &&
            pressed_ == None && !menu_.IsOpen() && GetTickCount64() - hover_started_ >= 500) {
            const auto info = PickerCommandPresentation(command);
            auto text = Get(info.label);
            if (*info.shortcut) text += L" (" + std::wstring(info.shortcut) + L")";
            const auto bounds = Bounds(hover_);
            const float right = static_cast<float>(compositor_.Width()) - Px(dialog_style::kMargin);
            const float width = std::min(painter_.MeasureButtonWidth(text), right - Bounds(Files).left);
            const float left = std::min(bounds.left, right - width);
            painter_.DrawTooltip(D2D1::RectF(left, bounds.bottom + Px(2), left + width, bounds.bottom + Px(30)), text);
        }
        EndSurface(compositor_);
    }

    void MoveFocus(bool backwards) {
        std::vector<DialogFocusTarget> targets;
        for (int i = Back; i < Count; ++i) {
            const auto id = static_cast<Control>(i);
            if (Enabled(id) && Bounds(id).right > Bounds(id).left)
                targets.push_back({id, id == Name ? filename_ : id == Address ? address_ : nullptr});
        }
        targets.push_back({Close});
        MoveDialogFocus(window_, focus_, targets, backwards);
        if (focus_ == Files && cursor_ >= 0) RevealCursor();
        if (focus_ == Locations) {
            tree_.Focus(Animate());
            ScheduleScroll();
        }
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
        return !request_ && Maximum() > 0 && x >= rows_.right - Px(10) && ContainsRect(rows_, x, y);
    }

    int HitRow(float x, float y) const {
        if (request_ || !ContainsRect(rows_, x, y) || OnScrollbar(x, y)) return -1;
        const auto row = static_cast<int>((y - rows_.top + offset_) / row_height_);
        return static_cast<size_t>(row) < Size() ? row : -1;
    }

    void DragScrollbar(float y) {
        const auto thumb = fluent::ScrollbarThumbRect(Scrollbar(), scale_, IsHighContrast());
        const float travel = Viewport() - (thumb.bottom - thumb.top);
        if (travel > 0.0f) offset_ = Maximum() * std::clamp((y - rows_.top - drag_offset_) / travel, 0.0f, 1.0f);
        Invalidate();
    }

    void MouseDown(float x, float y) {
        const auto id = Hit(x, y);
        if (id == Address || id == Name) { SetFocus(id == Address ? address_ : filename_); return; }
        if (!Enabled(id) || id == None) return;
        focus_ = pressed_ = id;
        if (id == Files) {
            scroll_.Reset();
            ScheduleScroll();
        } else if (id == Locations) {
            tree_.MouseDown(x, y, IsHighContrast());
            ScheduleScroll();
        }
        pressed_row_ = HitRow(x, y);
        SetFocus(window_);
        SetCapture(window_);
        if (OnScrollbar(x, y)) {
            dragging_ = true;
            const auto thumb = fluent::ScrollbarThumbRect(Scrollbar(), scale_, IsHighContrast());
            drag_offset_ = y >= thumb.top && y < thumb.bottom ? y - thumb.top : (thumb.bottom - thumb.top) * 0.5f;
            DragScrollbar(y);
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
            ScheduleScroll();
        } else if (!drag && down != None && down == Hit(x, y)) {
            if (down != Files) Invoke(down);
            else if (row >= 0 && row == HitRow(x, y)) Select(row, GetKeyState(VK_SHIFT) < 0, GetKeyState(VK_CONTROL) < 0);
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
                    else self.Submit();
                    return 0;
                }
                if (wparam == VK_ESCAPE) { self.CancelPicker(); return 0; }
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
            if (frame_.Consume(window_, wparam)) ScrollFrame();
            return 0;
        }
        switch (message) {
        case WM_CREATE:
            if (!compositor_.Init(window_, spec_.software_device)) return -1;
            compositor_.RecreateTextFormats(scale_);
            painter_.SetCompositor(&compositor_);
            painter_.SetScale(scale_);
            backdrop_ = ApplyBackdrop(window_, dark_);
            edit_brush_ = CreateSolidBrush(ChildEditBackColor(dark_));
            address_ = CreateEdit(Address, spec_.initial_path.c_str());
            if (has_name_) filename_ = CreateEdit(Name, spec_.filename.c_str());
            if (!address_ || (has_name_ && !filename_)) return -1;
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
                work_width > 0 ? std::min(MinimumTrackWidth(), work_width) : MinimumTrackWidth(),
                work_height > 0 ? std::min(Pixels(kMinimumHeight), work_height) : Pixels(kMinimumHeight)};
            return 0;
        }
        case WM_SIZE:
            if (wparam == SIZE_MINIMIZED) { scroll_.Reset(); tree_.Stop(); ScheduleScroll(); return 0; }
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
            backdrop_ = ApplyBackdrop(window_, dark_);
            if (edit_brush_) DeleteObject(edit_brush_);
            edit_brush_ = CreateSolidBrush(ChildEditBackColor(dark_));
            for (HWND edit : {address_, filename_}) if (edit) {
                SynchronizeChildEditBackend(compositor_, edit);
                InvalidateRect(edit, nullptr, TRUE);
            }
            ScrollFrame();
            Invalidate();
            return 0;
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLOREDIT:
            SetTextColor(reinterpret_cast<HDC>(wparam), ChildEditTextColor(dark_));
            SetBkColor(reinterpret_cast<HDC>(wparam), ChildEditBackColor(dark_));
            return reinterpret_cast<LRESULT>(ChildEditBackBrush(edit_brush_));
        case WM_COMMAND:
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
                if (window_ && ToolbarCommand(hover_) != PickerCommand::None && !tooltip_painted_ &&
                    GetTickCount64() - hover_started_ >= 500) { tooltip_painted_ = true; Invalidate(); }
            }
            else if (wparam == kScrollTimer) ScrollFrame();
            return 0;
        case kPickerListingReady:
            if (request_ && request_->token == wparam) ApplyListing();
            else ApplyTreeListing(wparam);
            return 0;
        case WM_ACTIVATE:
            if (LOWORD(wparam) == WA_INACTIVE) menu_.DismissForDeactivation(reinterpret_cast<HWND>(lparam));
            Invalidate();
            return 0;
        case WM_SYSKEYDOWN:
            if (Shortcut(wparam, true)) return 0;
            break;
        case WM_KEYDOWN:
            if (Shortcut(wparam)) return 0;
            if (wparam == VK_TAB) MoveFocus(GetKeyState(VK_SHIFT) < 0);
            else if (wparam == VK_ESCAPE) CancelPicker();
            else if (focus_ == Files) ListKey(wparam);
            else if (focus_ == Locations && Enabled(Locations)) {
                NavigateTree(tree_.Key(wparam, Animate()));
                ScheduleScroll();
                Invalidate();
            }
            else if (wparam == VK_RETURN || wparam == VK_SPACE) Invoke(static_cast<Control>(focus_));
            return 0;
        case WM_CHAR:
            if (focus_ == Files) SearchPrefix(static_cast<wchar_t>(wparam));
            return 0;
        case WM_MOUSEWHEEL: {
            POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            ScreenToClient(window_, &point);
            UINT lines = 3;
            SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
            const auto x = static_cast<float>(point.x), y = static_cast<float>(point.y);
            const float wheel = -GET_WHEEL_DELTA_WPARAM(wparam) / static_cast<float>(WHEEL_DELTA);
            if (ContainsRect(Bounds(Locations), x, y) && Enabled(Locations)) {
                tree_.Scroll(wheel, lines, Animate());
                ScheduleScroll();
                Invalidate();
                return 0;
            }
            if (request_ || !ContainsRect(Bounds(Files), x, y)) return 0;
            const float step = lines == WHEEL_PAGESCROLL ? Viewport() : row_height_ * static_cast<float>(lines);
            if (step > 0.0f) ScrollBy(wheel * step);
            return 0;
        }
        case WM_MOUSEMOVE: {
            const float x = static_cast<float>(GET_X_LPARAM(lparam)), y = static_cast<float>(GET_Y_LPARAM(lparam));
            const auto hovered = Hit(x, y);
            if (hovered != hover_) {
                hover_ = hovered;
                hover_started_ = GetTickCount64();
                tooltip_painted_ = false;
            }
            tree_.MouseMove(x, y, IsHighContrast());
            hover_row_ = HitRow(x, y);
            hover_scrollbar_ = OnScrollbar(x, y);
            if (dragging_) DragScrollbar(y);
            TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, window_, 0};
            TrackMouseEvent(&track);
            Invalidate();
            return 0;
        }
        case WM_MOUSELEAVE:
            hover_ = None;
            tree_.MouseLeave();
            hover_row_ = -1;
            hover_scrollbar_ = false;
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
        case WM_LBUTTONDBLCLK:
            if (Hit(static_cast<float>(GET_X_LPARAM(lparam)), static_cast<float>(GET_Y_LPARAM(lparam))) == Locations)
                MouseDown(static_cast<float>(GET_X_LPARAM(lparam)), static_cast<float>(GET_Y_LPARAM(lparam)));
            else ActivateRow(HitRow(static_cast<float>(GET_X_LPARAM(lparam)), static_cast<float>(GET_Y_LPARAM(lparam))));
            return 0;
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
            if (!wparam) { scroll_.Reset(); tree_.Stop(); ScheduleScroll(); }
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
            RemoveClipboardFormatListener(window_);
            // GetMessage can dispatch a sent WM_CLOSE without returning a queued message.
            PostThreadMessageW(GetCurrentThreadId(), WM_NULL, 0, 0);
            if (request_) request_->cancelled.store(true, std::memory_order_relaxed);
            frame_.Cancel(window_);
            menu_.Dismiss();
            for (HWND edit : {address_, filename_}) if (IsWindow(edit)) DestroyWindow(edit);
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

    const FilePickerSpec& spec_;
    D2D1_COLOR_F accent_{};
    bool dark_ = false, has_name_ = false, multiple_ = false;
    HWND window_ = nullptr, source_ = nullptr, address_ = nullptr, filename_ = nullptr;
    HBRUSH edit_brush_ = nullptr;
    HFONT font_ = nullptr;
    Compositor compositor_;
    fluent::Painter painter_;
    FluentMenu menu_;
    FilePickerTree tree_;
    EditFrameScheduler frame_;
    SmoothScroll scroll_;
    Listing request_;
    std::shared_ptr<ops::PickerFileOperations> operations_;
    std::shared_ptr<ops::PickerFileOperation> file_operation_;
    FilePickerResult result_;
    std::vector<fs::DirEntry> entries_;
    std::vector<std::wstring> history_;
    std::set<int> selection_;
    std::wstring directory_, notice_, prefix_;
    std::wstring operation_reveal_, reveal_name_;
    bool operation_rename_ = false, operation_refresh_ = false, file_conflict_open_ = false;
    std::wstring title_, primary_label_, cancel_label_, windows_label_, name_label_;
    std::array<D2D1_RECT_F, Count> controls_{};
    D2D1_RECT_F rows_{}, notice_rect_{}, name_label_rect_{};
    size_t applied_filter_ = 0, filter_index_ = 0;
    uint64_t prefix_time_ = 0, hover_started_ = 0;
    uint64_t conflict_token_ = 0;
    float scale_ = 1.0f, row_height_ = 28.0f, offset_ = 0.0f, drag_offset_ = 0.0f;
    SIZE resize_start_{};
    int focus_ = Files, cursor_ = -1, anchor_ = -1, pressed_row_ = -1, hover_row_ = -1;
    Control pressed_ = None, hover_ = None;
    bool done_ = false, accepted_ = false, backdrop_ = false, loaded_ = false, filtered_ = false, waiting_ = false;
    bool cancelling_ = false, submit_locked_ = false, updating_filename_ = false, filename_from_selection_ = false;
    bool dragging_ = false, hover_scrollbar_ = false, resized_ = false;
    bool clipboard_files_ = false, show_new_label_ = true, tooltip_painted_ = false;
    bool cancel_after_operation_ = false;
};

} // namespace

bool ShowFilePicker(HWND source, const FilePickerSpec& spec, bool dark,
                    D2D1_COLOR_F accent, FilePickerResult& result) {
    return PickerWindow(spec, dark, accent).Show(source, result);
}

} // namespace pulse::ui
