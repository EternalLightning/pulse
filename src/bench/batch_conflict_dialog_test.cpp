// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "../ui/batch_conflict_dialog.h"
#include "../ui/conflict_details.h"
#include "../ui/animation_clock.h"
#include "../common/localization.h"
#include <cmath>
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <cstring>

namespace pulse::ui {
struct BatchConflictDialogTestPeer {
    static BatchConflictLabels Labels() {
        using I = l10n::StringId;
        BatchConflictLabels labels;
        labels.title = l10n::Get(I::OpConflictTitle);
        labels.instruction = l10n::Get(I::BatchConflictInstruction);
        labels.source = l10n::Get(I::OpSource);
        labels.destination = l10n::Get(I::OpExisting);
        labels.replace = l10n::Get(I::OpReplaceFile);
        labels.skip = l10n::Get(I::OpSkipFile);
        labels.keep_both = l10n::Get(I::OpKeepBoth);
        labels.apply_all = l10n::Get(I::BatchConflictApplyAll);
        labels.confirm = l10n::Get(I::BatchConflictContinue);
        labels.cancel = l10n::Get(I::Cancel);
        labels.folder = l10n::Get(I::TypeFolder);
        labels.reparse = l10n::Get(I::BatchConflictLink);
        labels.unknown = l10n::Get(I::BatchConflictUnknown);
        return labels;
    }
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
    static void Wheel(BatchConflictDialog& dialog, int delta) {
        SendMessageW(dialog.hwnd_, WM_MOUSEWHEEL, MAKEWPARAM(0, static_cast<WORD>(delta)), 0);
    }
    static void AdvanceScroll(BatchConflictDialog& dialog, uint64_t elapsed) {
        const auto now = AnimationTimeMs();
        dialog.scroll_started_ = now - elapsed;
        dialog.TickScroll(now);
    }
    static int RunScroll(HWND owner, const std::filesystem::path& output) {
        int failures = 0;
        auto check = [&](bool passed, const char* label) {
            std::printf("[%s] %s\n", passed ? "PASS" : "FAIL", label);
            if (!passed) ++failures;
        };
        ops::ConflictItemInfo conflict;
        bool valid = true;
        conflict.token = 52; conflict.still_valid = [&] { return valid; };
        for (uint64_t id = 1; id <= 128; ++id) {
            ops::ConflictRowInfo row;
            row.row_id = id;
            row.source = L"C:\\fixture\\source\\report-" + std::to_wstring(id) + L".pdf";
            row.destination = L"D:\\fixture\\existing\\report-" + std::to_wstring(id) + L".pdf";
            conflict.rows.push_back(row);
        }
        for (const float scale : {1.0f, 2.0f}) for (const int width : {760, 360}) for (const bool dark : {false, true}) {
            std::printf("Scroll case: dpi=%d width=%d dark=%d rows=128\n", static_cast<int>(scale * 96), width, dark);
            BatchConflictDialog dialog;
            check(dialog.Initialize(owner, conflict, Labels(), dark, HexColor(0x0078D4), false), "initialize hidden long-list scroll fixture");
            if (!dialog.hwnd_) return 2;
            dialog.scale_ = scale; dialog.painter_.SetScale(scale); dialog.compositor_.RecreateTextFormats(scale);
            dialog.compositor_.Resize(static_cast<int>(width * scale), static_cast<int>(720 * scale));
            dialog.Layout(); dialog.animations_enabled_ = true;
            auto reset = [&] { dialog.Scroll(-dialog.content_height_); };
            Wheel(dialog, -WHEEL_DELTA);
            check(dialog.scroll_ == 0 && dialog.scroll_animating_ && std::abs(dialog.scroll_target_ - 72 * scale) < 0.01f,
                "wheel starts an animation without immediately jumping the visible list");
            AdvanceScroll(dialog, 40);
            const float intermediate = dialog.scroll_;
            check(intermediate > 0 && intermediate < 72 * scale, "wheel has a real intermediate visual offset");
            const auto first = dialog.rows_[0];
            const auto replace_index = Find(dialog, BatchConflictDialog::Action::Select, 0, ops::ConflictChoice::Replace);
            check(std::abs(first.top - (dialog.viewport_.top - intermediate)) < 0.02f &&
                dialog.controls_[replace_index].bounds.top >= dialog.DetailsRect(first).bottom,
                "animated row rendering and button hit geometry share the same displayed offset");
            Wheel(dialog, -WHEEL_DELTA);
            check(std::abs(dialog.scroll_target_ - 144 * scale) < 0.01f, "consecutive wheel input accumulates the remaining travel");
            const float before_reverse = dialog.scroll_;
            Wheel(dialog, WHEEL_DELTA);
            const float reverse_target = dialog.scroll_target_;
            AdvanceScroll(dialog, 40);
            check(reverse_target < before_reverse && dialog.scroll_ < before_reverse, "reverse wheel input responds in the new direction immediately");
            AdvanceScroll(dialog, 160);
            check(!dialog.scroll_animating_ && dialog.scroll_ == reverse_target, "animation lands exactly on its target and stops scheduling frames");
            reset();
            for (int pulse = 0; pulse < 20; ++pulse) Wheel(dialog, -WHEEL_DELTA);
            check(dialog.scroll_animating_ && std::abs(dialog.scroll_target_ - 20 * 72 * scale) < 0.01f,
                "rapid wheel bursts preserve accumulated travel without restarting the animation timer");
            reset(); Wheel(dialog, -30);
            check(std::abs(dialog.scroll_target_ - 18 * scale) < 0.01f, "high-resolution wheel deltas retain fractional-notch travel");
            reset(); dialog.animations_enabled_ = false; Wheel(dialog, -WHEEL_DELTA);
            check(!dialog.scroll_animating_ && std::abs(dialog.scroll_ - 72 * scale) < 0.01f, "disabled motion scrolls directly without leaving an animation timer");
            dialog.animations_enabled_ = true;
            reset(); Wheel(dialog, -WHEEL_DELTA); AdvanceScroll(dialog, 40);
            const auto button = dialog.controls_[replace_index].bounds;
            const int x = static_cast<int>((button.left + button.right) / 2), y = static_cast<int>((button.top + button.bottom) / 2);
            const float displayed = dialog.scroll_;
            SendMessageW(dialog.hwnd_, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(x, y));
            check(!dialog.scroll_animating_ && dialog.scroll_ == displayed, "button press freezes the displayed list without jumping to its pending target");
            SendMessageW(dialog.hwnd_, WM_LBUTTONUP, 0, MAKELPARAM(x, y));
            check(dialog.selections_[0].choice == ops::ConflictChoice::Replace, "click during scroll selects the button visible under the pointer");
            dialog.selections_[0].choice = ops::ConflictChoice::KeepBoth;
            SendMessageW(dialog.hwnd_, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(x, y));
            Wheel(dialog, -WHEEL_DELTA); AdvanceScroll(dialog, 40);
            SendMessageW(dialog.hwnd_, WM_LBUTTONUP, 0, MAKELPARAM(x, y));
            check(dialog.selections_[0].choice == ops::ConflictChoice::KeepBoth, "moving a pressed row cancels its pending click instead of selecting another action");
            Wheel(dialog, -WHEEL_DELTA);
            dialog.focus_ = Find(dialog, BatchConflictDialog::Action::Select, 127, ops::ConflictChoice::Skip) - 1;
            SendMessageW(dialog.hwnd_, WM_KEYDOWN, VK_TAB, 0);
            check(!dialog.scroll_animating_ && dialog.controls_[dialog.focus_].bounds.bottom <= dialog.viewport_.bottom + 0.02f,
                "keyboard focus stops wheel animation and reveals the requested row immediately");
            Wheel(dialog, WHEEL_DELTA);
            SendMessageW(dialog.hwnd_, WM_KEYDOWN, VK_HOME, 0);
            check(!dialog.scroll_animating_ && dialog.scroll_ == 0, "Home cancels pending animation and reaches the first row immediately");
            Wheel(dialog, -WHEEL_DELTA);
            SendMessageW(dialog.hwnd_, WM_KEYDOWN, VK_END, 0);
            const float maximum = dialog.content_height_ - (dialog.viewport_.bottom - dialog.viewport_.top);
            check(!dialog.scroll_animating_ && dialog.scroll_ == maximum, "End cancels pending animation and reaches the last row exactly");
            Wheel(dialog, -WHEEL_DELTA);
            check(!dialog.scroll_animating_ && dialog.scroll_ == maximum, "wheel input beyond the last row does not start an overscroll animation");
            reset(); Wheel(dialog, -WHEEL_DELTA);
            const int bar_x = static_cast<int>(dialog.viewport_.right - 5 * scale);
            const int bar_y = static_cast<int>((dialog.viewport_.top + dialog.viewport_.bottom) / 2);
            SendMessageW(dialog.hwnd_, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(bar_x, bar_y));
            const float drag_offset = dialog.scroll_;
            SendMessageW(dialog.hwnd_, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(bar_x, bar_y + static_cast<int>(20 * scale)));
            check(dialog.dragging_ && !dialog.scroll_animating_ && dialog.scroll_ > drag_offset, "scrollbar drag follows the pointer directly and cancels wheel easing");
            SendMessageW(dialog.hwnd_, WM_LBUTTONUP, 0, MAKELPARAM(bar_x, bar_y));
            reset(); Wheel(dialog, -WHEEL_DELTA);
            SendMessageW(dialog.hwnd_, WM_SIZE, SIZE_RESTORED, MAKELPARAM(static_cast<int>(width * scale), static_cast<int>(720 * scale)));
            check(!dialog.scroll_animating_ && dialog.scroll_ >= 0 && dialog.scroll_ <= dialog.content_height_, "resize cancels animation before rebuilding the viewport");
            reset();
            std::vector<double> draw_times;
            bool rendered = true;
            for (int frame = 0; frame < 12; ++frame) {
                dialog.Scroll(12 * scale);
                const auto draw_started = std::chrono::steady_clock::now();
                rendered &= dialog.Render(false);
                draw_times.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - draw_started).count());
            }
            std::sort(draw_times.begin(), draw_times.end());
            check(rendered, "production renderer draws moving rows without requiring Present");
            std::printf("DRAW dpi=%d width=%d dark=%d median_ms=%.2f p95_ms=%.2f max_ms=%.2f\n",
                static_cast<int>(scale * 96), width, dark, draw_times[draw_times.size()/2], draw_times[(draw_times.size()-1)*95/100], draw_times.back());
            // Measure real WM_TIMER dispatches, production drawing and Present with a hidden window.
            reset(); Wheel(dialog, -2 * WHEEL_DELTA);
            std::vector<double> intervals, render_times;
            const auto deadline = AnimationTimeMs() + 1500;
            auto previous = AnimationTimeMs();
            bool monotonic = true;
            while (dialog.scroll_animating_ && AnimationTimeMs() < deadline) {
                MsgWaitForMultipleObjects(0, nullptr, FALSE, 20, QS_ALLINPUT);
                MSG message{};
                while (PeekMessageW(&message, dialog.hwnd_, 0, 0, PM_REMOVE)) {
                    const bool frame = message.message == WM_TIMER && message.wParam == 2;
                    const auto started = std::chrono::steady_clock::now();
                    const auto now = AnimationTimeMs();
                    const float before = dialog.scroll_;
                    DispatchMessageW(&message);
                    if (frame) {
                        intervals.push_back(static_cast<double>(now - previous)); previous = now;
                        render_times.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count());
                        monotonic &= dialog.scroll_ >= before && dialog.scroll_ <= 144 * scale;
                    }
                }
            }
            check(!dialog.scroll_animating_ && intervals.size() >= 3 && monotonic && std::abs(dialog.scroll_ - 144 * scale) < 0.01f,
                "real timer messages render multiple bounded frames and complete the wheel gesture");
            if (!intervals.empty()) {
                std::sort(intervals.begin(), intervals.end()); std::sort(render_times.begin(), render_times.end());
                std::printf("CADENCE dpi=%d width=%d dark=%d frames=%zu interval_median_ms=%.1f interval_p95_ms=%.1f interval_max_ms=%.1f render_p95_ms=%.2f\n",
                    static_cast<int>(scale * 96), width, dark, intervals.size(), intervals[intervals.size()/2],
                    intervals[(intervals.size()-1)*95/100], intervals.back(), render_times[(render_times.size()-1)*95/100]);
            }
            if (scale == 1 && width == 760 && dark) {
                reset(); Wheel(dialog, -4 * WHEEL_DELTA);
                for (const uint64_t elapsed : {0, 40, 80, 120, 160}) {
                    AdvanceScroll(dialog, elapsed);
                    check(dialog.Render(false) && dialog.compositor_.SaveSnapshot((output / (L"smooth-scroll-" + std::to_wstring(elapsed) + L".png")).c_str()),
                        "capture production-rendered wheel animation checkpoints");
                }
            }
            reset(); Wheel(dialog, -WHEEL_DELTA);
            if (dark) SendMessageW(dialog.hwnd_, WM_CLOSE, 0, 0);
            else { valid = false; SendMessageW(dialog.hwnd_, WM_TIMER, 1, 0); valid = true; }
            const float closed_offset = dialog.scroll_;
            SendMessageW(dialog.hwnd_, WM_TIMER, 2, 0);
            check(dialog.done_ && !dialog.result_ && !dialog.scroll_animating_ && dialog.scroll_ == closed_offset,
                "close or token expiry stops animation and ignores a queued animation timer");
        }
        std::printf("Batch conflict smooth scroll: %d failure(s)\n", failures);
        return failures ? 1 : 0;
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
        const auto original_source = conflict.rows[0].source;
        conflict.rows[0].source = L"\\\\?\\" + original_source;
        conflict.rows[1].destination = L"\\\\?\\UNC\\server\\share\\" + std::wstring(L"网络共享\\") + L"report-2.txt";
        const auto labels = Labels();
        BatchConflictDialog dialog;
        check(dialog.Initialize(owner, conflict, labels, false, HexColor(0x0078D4), false),
            "initialize isolated ordinary batch conflict dialog");
        if (!dialog.hwnd_) return 2;
        check(!IsWindowVisible(dialog.hwnd_) && IsWindowEnabled(owner), "fixture leaves its owner enabled and never shows or activates the dialog");
        check(dialog.selections_.size() == conflict.rows.size() && std::all_of(dialog.selections_.begin(), dialog.selections_.end(),
            [](const auto& row) { return row.choice == ops::ConflictChoice::KeepBoth; }), "every row defaults to keep both");
        dialog.dark_ = true;
        const auto original_rows = dialog.conflict_.rows;
        const std::wstring preview_names[]{L"同步电机计算题-答案全.pdf", L"模拟题.pdf", L"直流电动机和直流发电机--20221103.pdf"};
        for (size_t i = 0; i < 3; ++i) {
            auto& row = dialog.conflict_.rows[i];
            row.source = L"\\\\?\\Z:\\Study\\电机学\\2024下课件\\" + preview_names[i];
            row.destination = L"\\\\?\\E:\\Projects\\Thesis\\电机学\\2024下课件\\" + preview_names[i];
            row.source_size = row.destination_size = 717619 + i * 400000;
            row.source_is_directory = row.destination_is_directory = false;
        }
        check(dialog.Render(false) && dialog.compositor_.SaveSnapshot((output / L"batch-default-dark.png").c_str()),
            "capture the default keep-both state with the production renderer");
        dialog.conflict_.rows = original_rows;
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
        for (const auto language : {L"zh-CN", L"en-US"}) {
            l10n::SetLanguage(language);
            dialog.labels_ = Labels();
            for (const float scale : {1.0f, 1.25f, 1.5f, 2.0f}) for (const int width : {760, 620, 360, 340}) for (const bool dark : {false, true}) {
                dialog.scale_ = scale; dialog.dark_ = dark; dialog.painter_.SetScale(scale);
                dialog.compositor_.RecreateTextFormats(scale);
                dialog.compositor_.Resize(static_cast<int>(width * scale), static_cast<int>(720 * scale));
                dialog.scroll_ = 0; dialog.Layout();
                check(dialog.Render(false), "production batch renderer draws before Present at narrow width theme and DPI");
                const auto name = L"batch-" + std::wstring(language) + L"-" + std::to_wstring(static_cast<int>(scale * 100)) + L"-" + std::to_wstring(width) +
                    (dark ? L"-dark.png" : L"-light.png");
                check(dialog.compositor_.SaveSnapshot((output / name).c_str()), "save offscreen batch conflict layout evidence");
                bool safe = true;
                for (const auto& control : dialog.controls_) {
                    safe &= control.bounds.left >= 0 && control.bounds.right <= dialog.compositor_.Width();
                    if (control.action == BatchConflictDialog::Action::Select) {
                        const auto row = dialog.rows_[control.row];
                        const auto details = dialog.DetailsRect(row);
                        safe &= control.bounds.top >= details.bottom && control.bounds.bottom <= row.bottom;
                        safe &= control.bounds.left >= row.left && control.bounds.right <= row.right;
                    } else if (control.action == BatchConflictDialog::Action::Apply) safe &= control.bounds.bottom <= dialog.viewport_.top;
                    else if (control.action == BatchConflictDialog::Action::Close) safe &= control.bounds.bottom < dialog.viewport_.top;
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
                bool right_aligned = true;
                for (const auto& control : dialog.controls_) {
                    if (control.action != BatchConflictDialog::Action::Select && control.action != BatchConflictDialog::Action::Apply) continue;
                    const bool last_on_line = std::none_of(dialog.controls_.begin(), dialog.controls_.end(), [&](const auto& other) {
                        return other.action == control.action && (other.action != BatchConflictDialog::Action::Select || other.row == control.row) &&
                            std::abs(other.bounds.top - control.bounds.top) < 0.1f && other.bounds.left > control.bounds.left;
                    });
                    const float expected_right = control.action == BatchConflictDialog::Action::Apply ?
                        static_cast<float>(dialog.compositor_.Width()) - dialog.viewport_.left : dialog.DetailsRect(dialog.rows_[control.row]).right;
                    if (last_on_line) right_aligned &= std::abs(control.bounds.right - expected_right) < 0.1f;
                }
                check(right_aligned, "every batch and per-item action line aligns to the right including wrapped rows");
                check(dialog.content_height_ > dialog.viewport_.bottom - dialog.viewport_.top, "long batch list retains a bounded scroll viewport");
            }
        }
        check(path::StripExtendedPathPrefix(dialog.conflict_.rows[0].source) == original_source &&
            path::StripExtendedPathPrefix(dialog.conflict_.rows[1].destination) == L"\\\\server\\share\\网络共享\\report-2.txt" &&
            dialog.conflict_.rows[0].source == conflict.rows[0].source && dialog.conflict_.rows[1].destination == conflict.rows[1].destination,
            "display paths remove extended drive and UNC prefixes while operation paths remain intact");
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
            BatchConflictDialog closed;
            check(closed.Initialize(owner, conflict, labels, true, HexColor(0x0078D4), false), "initialize title-bar close fixture");
            check(Click(closed, Find(closed, BatchConflictDialog::Action::Close)) && closed.done_ && !closed.result_,
                "title-bar close cancels without accepting pending selections");
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
        if (GetForegroundWindow() != foreground || !IsWindowEnabled(owner)) {
            const HWND current = GetForegroundWindow();
            DWORD initial_pid = 0, current_pid = 0;
            GetWindowThreadProcessId(foreground, &initial_pid); GetWindowThreadProcessId(current, &current_pid);
            std::printf("Foreground diagnostic: before=%p pid=%lu after=%p pid=%lu fixture_pid=%lu owner_enabled=%d\n",
                static_cast<void*>(foreground), initial_pid, static_cast<void*>(current), current_pid, GetCurrentProcessId(), IsWindowEnabled(owner));
        }
        check(GetForegroundWindow() == foreground && IsWindowEnabled(owner), "all hidden fixtures preserve foreground and owner enabled state");
        std::printf("Batch conflict dialog: %d failure(s)\n", failures);
        return failures ? 1 : 0;
    }
};
}

int main(int argc, char** argv) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    pulse::l10n::Initialize(GetModuleHandleW(nullptr), L"zh-CN");
    const auto output = std::filesystem::absolute(L"bench_data/batch-conflict-dialog");
    std::filesystem::create_directories(output);
    HWND owner = CreateWindowExW(WS_EX_NOACTIVATE, L"STATIC", L"Batch conflict isolated fixture", WS_OVERLAPPEDWINDOW,
        -20000, -20000, 960, 720, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    const bool scroll_only = argc > 1 && std::strcmp(argv[1], "--scroll") == 0;
    const int result = !owner ? 2 : scroll_only ? pulse::ui::BatchConflictDialogTestPeer::RunScroll(owner, output) :
        pulse::ui::BatchConflictDialogTestPeer::Run(owner, output);
    if (owner) DestroyWindow(owner);
    if (SUCCEEDED(initialized)) CoUninitialize();
    return result;
}
