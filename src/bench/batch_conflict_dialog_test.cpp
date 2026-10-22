#include "../ui/batch_conflict_dialog.h"
#include <algorithm>
#include <cstdio>
#include <filesystem>

namespace pulse::ui {
struct BatchConflictDialogTestPeer {
    static int Find(const BatchConflictDialog& dialog, BatchConflictDialog::Action action,
        size_t row = 0, ops::ConflictChoice choice = ops::ConflictChoice::KeepBoth) {
        for (size_t i = 0; i < dialog.controls_.size(); ++i) {
            const auto& control = dialog.controls_[i];
            if (control.action == action && (action != BatchConflictDialog::Action::Select || control.row == row) &&
                ((action != BatchConflictDialog::Action::Select && action != BatchConflictDialog::Action::Apply) || control.choice == choice))
                return static_cast<int>(i);
        }
        return -1;
    }
    static bool Click(BatchConflictDialog& dialog, int index) {
        if (index < 0) return false;
        const auto before = dialog.controls_[index];
        if (before.action == BatchConflictDialog::Action::Select) {
            if (before.bounds.top < dialog.viewport_.top) dialog.Scroll(before.bounds.top - dialog.viewport_.top - 4 * dialog.scale_);
            else if (before.bounds.bottom > dialog.viewport_.bottom) dialog.Scroll(before.bounds.bottom - dialog.viewport_.bottom + 4 * dialog.scale_);
        }
        const auto rect = dialog.controls_[index].bounds;
        const int x = static_cast<int>((rect.left + rect.right) * 0.5f), y = static_cast<int>((rect.top + rect.bottom) * 0.5f);
        SendMessageW(dialog.hwnd_, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(x, y));
        SendMessageW(dialog.hwnd_, WM_LBUTTONUP, 0, MAKELPARAM(x, y));
        return true;
    }
    static int Run(HWND owner, const std::filesystem::path& output) {
        int failures = 0;
        auto check = [&](bool passed, const char* label) {
            std::printf("[%s] %s\n", passed ? "PASS" : "FAIL", label);
            if (!passed) ++failures;
        };
        const HWND foreground = GetForegroundWindow();
        bool valid = true;
        ops::ConflictItemInfo conflict;
        conflict.token = 42; conflict.task_id = 7; conflict.still_valid = [&] { return valid; };
        SYSTEMTIME modified{2026, 10, 0, 22, 10, 30, 0, 0};
        FILETIME time{}; SystemTimeToFileTime(&modified, &time);
        for (uint64_t i = 1; i <= 9; ++i) {
            ops::ConflictRowInfo row;
            row.row_id = i;
            const std::wstring name = i == 2 ? L"中文与 English 的长文件名-for-reading-and-truncation-12345678901234567890.txt" :
                L"report-" + std::to_wstring(i) + L".txt";
            row.source = L"C:\\isolated-fixture\\source\\a-long-parent-directory-that-must-not-hide-the-file-name\\" + name;
            row.destination = L"D:\\isolated-fixture\\existing\\" + name;
            row.source_size = 4096 * i; row.destination_size = 2048 * i;
            row.source_modified = time; row.destination_modified = time;
            row.source_is_directory = i == 3; row.destination_is_directory = i == 3;
            row.source_is_reparse = i == 4;
            conflict.rows.push_back(std::move(row));
        }
        BatchConflictLabels labels;
        BatchConflictDialog dialog;
        check(dialog.Initialize(owner, conflict, labels, false, HexColor(0x0078D4), false),
            "initialize isolated ordinary batch conflict dialog");
        if (!dialog.hwnd_) return 2;
        check(!IsWindowVisible(dialog.hwnd_) && IsWindowEnabled(owner), "fixture leaves its owner enabled and never shows or activates the dialog");
        check(dialog.selections_.size() == conflict.rows.size() && std::all_of(dialog.selections_.begin(), dialog.selections_.end(),
            [](const auto& row) { return row.choice == ops::ConflictChoice::KeepBoth; }), "every row defaults to keep both");
        check(Click(dialog, Find(dialog, BatchConflictDialog::Action::Apply, 0, ops::ConflictChoice::Replace)) &&
            std::all_of(dialog.selections_.begin(), dialog.selections_.end(), [](const auto& row) { return row.choice == ops::ConflictChoice::Replace; }),
            "apply all replace updates every row through actual mouse messages");
        check(Click(dialog, Find(dialog, BatchConflictDialog::Action::Select, 1, ops::ConflictChoice::Skip)) &&
            dialog.selections_[1].choice == ops::ConflictChoice::Skip && dialog.selections_[0].choice == ops::ConflictChoice::Replace,
            "per-row skip overrides the batch choice without changing other rows");
        dialog.focus_ = Find(dialog, BatchConflictDialog::Action::Select, 2, ops::ConflictChoice::KeepBoth);
        SendMessageW(dialog.hwnd_, WM_KEYDOWN, VK_SPACE, 0);
        check(dialog.selections_[2].choice == ops::ConflictChoice::KeepBoth, "Space selects a focused row choice");
        dialog.focus_ = Find(dialog, BatchConflictDialog::Action::Cancel);
        SendMessageW(dialog.hwnd_, WM_KEYDOWN, VK_TAB, 0);
        check(dialog.controls_[dialog.focus_].action == BatchConflictDialog::Action::Confirm, "Tab moves from cancel to explicit batch confirmation");
        check(dialog.Metadata(conflict.rows[2], true).find(labels.folder) != std::wstring::npos &&
            dialog.Metadata(conflict.rows[3], true).find(labels.reparse) != std::wstring::npos,
            "directory and reparse metadata use supplied labels without filesystem probes");
        for (const float scale : {1.0f, 1.5f, 2.0f}) for (const int width : {760, 360}) for (const bool dark : {false, true}) {
            dialog.scale_ = scale; dialog.dark_ = dark; dialog.painter_.SetScale(scale);
            dialog.compositor_.RecreateTextFormats(scale);
            dialog.compositor_.Resize(static_cast<int>(width * scale), static_cast<int>(520 * scale));
            dialog.Layout();
            check(dialog.Render(false), "production batch renderer draws before Present at narrow width theme and DPI");
            const auto name = L"batch-" + std::to_wstring(static_cast<int>(scale * 100)) + L"-" + std::to_wstring(width) +
                (dark ? L"-dark.png" : L"-light.png");
            check(dialog.compositor_.SaveSnapshot((output / name).c_str()), "save offscreen batch conflict layout evidence");
            bool safe = true;
            for (const auto& control : dialog.controls_) {
                safe &= control.bounds.left >= 0 && control.bounds.right <= dialog.compositor_.Width();
                if (control.action == BatchConflictDialog::Action::Select) {
                    const auto row = dialog.rows_[control.row];
                    const float details_height = width >= 620 ? 108 * scale : 216 * scale;
                    safe &= control.bounds.top >= row.top + details_height && control.bounds.bottom <= row.bottom;
                    safe &= control.bounds.left >= row.left && control.bounds.right <= row.right;
                } else if (control.action == BatchConflictDialog::Action::Apply) safe &= control.bounds.bottom <= dialog.viewport_.top;
                else safe &= control.bounds.top >= dialog.viewport_.bottom;
            }
            for (size_t i = 0; i < dialog.controls_.size(); ++i) for (size_t j = i + 1; j < dialog.controls_.size(); ++j) {
                const auto& a = dialog.controls_[i]; const auto& b = dialog.controls_[j];
                if (a.action == BatchConflictDialog::Action::Select && b.action != a.action) continue;
                if (b.action == BatchConflictDialog::Action::Select && a.action != b.action) continue;
                safe &= !(a.bounds.left < b.bounds.right && a.bounds.right > b.bounds.left &&
                    a.bounds.top < b.bounds.bottom && a.bounds.bottom > b.bounds.top);
            }
            check(safe, "file metadata row choices and fixed footer stay in separate non-overlapping areas");
            check(dialog.content_height_ > dialog.viewport_.bottom - dialog.viewport_.top, "long batch list retains a bounded scroll viewport");
        }
        dialog.scale_ = 1; dialog.painter_.SetScale(1); dialog.compositor_.RecreateTextFormats(1);
        dialog.compositor_.Resize(360, 520); dialog.Layout();
        SendMessageW(dialog.hwnd_, WM_KEYDOWN, VK_END, 0);
        check(dialog.scroll_ == dialog.content_height_ - (dialog.viewport_.bottom - dialog.viewport_.top), "End reveals the final conflict and clamps scroll");
        SendMessageW(dialog.hwnd_, WM_KEYDOWN, VK_HOME, 0);
        check(dialog.scroll_ == 0, "Home returns to the first conflict");
        dialog.focus_ = Find(dialog, BatchConflictDialog::Action::Select, conflict.rows.size() - 1, ops::ConflictChoice::Skip) - 1;
        SendMessageW(dialog.hwnd_, WM_KEYDOWN, VK_TAB, 0);
        check(dialog.scroll_ > 0 && dialog.controls_[dialog.focus_].bounds.bottom <= dialog.viewport_.bottom,
            "keyboard focus scrolls an offscreen row choice into view");
        dialog.focus_ = Find(dialog, BatchConflictDialog::Action::Confirm);
        SendMessageW(dialog.hwnd_, WM_KEYDOWN, VK_RETURN, 0);
        check(dialog.done_ && dialog.result_ && dialog.result_->size() == conflict.rows.size() &&
            (*dialog.result_)[1].row_id == 2 && (*dialog.result_)[1].choice == ops::ConflictChoice::Skip &&
            (*dialog.result_)[2].choice == ops::ConflictChoice::KeepBoth,
            "Enter confirms one complete ordered row-ID resolution snapshot with overrides");
        dialog.Destroy();
        {
            BatchConflictDialog cancelled;
            check(cancelled.Initialize(owner, conflict, labels, true, HexColor(0x0078D4), false), "initialize cancellation fixture");
            SendMessageW(cancelled.hwnd_, WM_KEYDOWN, VK_ESCAPE, 0);
            check(cancelled.done_ && !cancelled.result_, "Escape cancels the entire batch without returning selections");
        }
        {
            BatchConflictDialog expired;
            check(expired.Initialize(owner, conflict, labels, false, HexColor(0x0078D4), false), "initialize live token fixture");
            valid = false;
            SendMessageW(expired.hwnd_, WM_TIMER, 1, 0);
            check(expired.done_ && !expired.result_, "still_valid expiry cancels through the real timer message");
        }
        {
            valid = true;
            BatchConflictDialog expired_at_confirm;
            check(expired_at_confirm.Initialize(owner, conflict, labels, false, HexColor(0x0078D4), false), "initialize confirm race fixture");
            valid = false;
            expired_at_confirm.focus_ = Find(expired_at_confirm, BatchConflictDialog::Action::Confirm);
            SendMessageW(expired_at_confirm.hwnd_, WM_KEYDOWN, VK_RETURN, 0);
            check(expired_at_confirm.done_ && !expired_at_confirm.result_, "confirmation rechecks token validity and cannot accept expired data");
        }
        valid = true;
        auto invalid = conflict; invalid.token = 0;
        BatchConflictDialog invalid_token;
        check(!invalid_token.Initialize(owner, invalid, labels, false, HexColor(0x0078D4), false), "zero conflict token is rejected before creating a window");
        invalid = conflict; invalid.rows[1].row_id = invalid.rows[0].row_id;
        BatchConflictDialog duplicate;
        check(!duplicate.Initialize(owner, invalid, labels, false, HexColor(0x0078D4), false), "duplicate row IDs cannot produce ambiguous decisions");
        invalid = conflict; invalid.link_confirmation = true;
        BatchConflictDialog linked;
        check(!linked.Initialize(owner, invalid, labels, false, HexColor(0x0078D4), false), "link confirmation is excluded from the ordinary batch UI");
        check(GetForegroundWindow() == foreground && IsWindowEnabled(owner), "all hidden fixtures preserve foreground and owner enabled state");
        std::printf("Batch conflict dialog: %d failure(s)\n", failures);
        return failures ? 1 : 0;
    }
};
}

int main() {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const auto output = std::filesystem::absolute(L"bench_data/batch-conflict-dialog");
    std::filesystem::create_directories(output);
    HWND owner = CreateWindowExW(WS_EX_NOACTIVATE, L"STATIC", L"Batch conflict isolated fixture", WS_OVERLAPPEDWINDOW,
        -20000, -20000, 960, 720, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    const int result = owner ? pulse::ui::BatchConflictDialogTestPeer::Run(owner, output) : 2;
    if (owner) DestroyWindow(owner);
    if (SUCCEEDED(initialized)) CoUninitialize();
    return result;
}
