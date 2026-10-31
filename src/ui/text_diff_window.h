// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once

// Stand-alone text compare window opened from the file context menu.
// Native caption (DWM dark title bar), D2D client area: toolbar, file headers,
// side-by-side or unified diff with character highlights, minimap, status bar.

#include "ui_compositor.h"
#include "fluent_components.h"
#include "smooth_scroll.h"
#include "../app/text_diff.h"
#include "../app/window_result_mailbox.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace pulse::ui {

class TextDiffWindow {
    friend struct TextDiffWindowTestPeer;
public:
    TextDiffWindow() = default;
    ~TextDiffWindow();
    TextDiffWindow(const TextDiffWindow&) = delete;
    TextDiffWindow& operator=(const TextDiffWindow&) = delete;

    // Opens (or reuses) the window for a new pair and starts the comparison.
    void Show(HWND owner, const std::wstring& left, const std::wstring& right, bool dark,
              D2D1_COLOR_F accent);
    void Close();
    bool visible() const noexcept;
    HWND hwnd() const noexcept { return hwnd_; }

private:
    enum class Hit {
        None, ModeSplit, ModeUnified, OnlyChanges, IgnoreWs, IgnoreCase, Swap, Prev, Next,
        Body, Minimap
    };
    struct ViewRow {
        int row = -1;        // DiffRow index; -1 for a fold row
        int8_t side = -1;    // unified only: 0 = left text, 1 = right text, 2 = same (both numbers)
        int fold_first = -1; // fold row: first hidden DiffRow
        int fold_count = 0;
    };
    struct FileInfo {
        std::wstring name, folder, time;
        uint64_t mtime = 0;
    };
    struct JobResult {
        uint64_t generation = 0;
        uint64_t mtime[2]{};
        std::shared_ptr<const diff::DiffSide> left, right;
        diff::DiffResult result;
    };
    struct CharSpans {
        std::vector<diff::CharSpan> left, right;
    };

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
    LRESULT HandleMessage(UINT message, WPARAM wparam, LPARAM lparam);
    bool EnsureWindow(HWND owner);
    void StartJob(bool reload);
    void StopJob();
    void AdoptResult(WPARAM identity);
    void RebuildView();
    void Layout();
    bool Render(const wchar_t* snapshot_path = nullptr);
    void RecreateFormats();
    void EnsureHatch();
    Hit HitTest(float x, float y, int* view_row) const;
    void OnClick(float x, float y, bool shift);
    void ScrollTo(float y);
    void ScrollBy(float dy) { ScrollTo(scroll_y_ + dy); }
    void ScrollX(float dx);
    void WheelScroll(float dx, float dy);
    float ScrollMaximumY() const;
    float ScrollMaximumX() const;
    void ResetScrollAnimation();
    void UpdateScrollTimer();
    void GoHunk(int index);
    void StepHunk(int delta);
    void CopySelection();
    void UpdateTitle();
    float Measure(std::wstring_view text, IDWriteTextFormat* format) const;
    const CharSpans& SpansFor(int row);
    float RowHeight() const noexcept { return 20.0f * scale_; }
    float ContentHeight() const noexcept {
        return static_cast<float>(view_.size()) * RowHeight();
    }
    bool Ready() const noexcept {
        return !computing_ && left_ && right_ && left_->status == diff::LoadStatus::Ok &&
               right_->status == diff::LoadStatus::Ok;
    }
    Theme CurrentTheme() const;

    HWND hwnd_ = nullptr;
    Compositor compositor_;
    fluent::Painter painter_;
    float scale_ = 1.0f;
    bool dark_ = false;
    D2D1_COLOR_F accent_{0.0f, 0.47f, 0.83f, 1.0f};

    ComPtr<IDWriteTextFormat> mono_;
    ComPtr<IDWriteTextFormat> gutter_;
    ComPtr<IDWriteTextFormat> ui_;
    ComPtr<IDWriteTextFormat> ui_bold_;
    ComPtr<IDWriteTextFormat> small_;
    ComPtr<ID2D1SolidColorBrush> brush_;
    ComPtr<ID2D1BitmapBrush> hatch_;
    bool hatch_dark_ = false;
    float char_w_ = 8.0f;

    // Inputs
    std::wstring path_[2];
    FileInfo info_[2];
    diff::DiffOptions options_{};
    bool unified_ = false;
    bool only_changes_ = false;

    // Worker
    std::shared_ptr<std::atomic<bool>> cancel_;
    std::shared_ptr<app::WindowResultMailbox<std::unique_ptr<JobResult>>> mailbox_;
    uint64_t generation_ = 0;
    bool computing_ = false;
    bool first_result_ = true;

    // Results
    std::shared_ptr<const diff::DiffSide> left_, right_;
    diff::DiffResult result_;
    std::vector<ViewRow> view_;
    std::vector<int> row_to_view_;
    std::set<int> expanded_folds_;
    std::unordered_map<int, CharSpans> spans_;
    int max_cols_ = 0;
    bool truncated_lines_ = false;
    int number_digits_ = 1;

    // View state
    float scroll_y_ = 0.0f;
    float scroll_x_ = 0.0f;
    SmoothScroll smooth_scroll_y_, smooth_scroll_x_;
    int current_hunk_ = -1;
    int sel_anchor_ = -1;
    int sel_end_ = -1;
    Hit hover_ = Hit::None;
    int hover_row_ = -1;
    Hit pressed_ = Hit::None;
    Hit keyboard_focus_ = Hit::None;
    bool minimap_drag_ = false;
    bool tracking_leave_ = false;
    bool tooltip_visible_ = false;

    // Layout (pixels)
    D2D1_RECT_F toolbar_{}, header_{}, body_{}, minimap_{}, status_{};
    D2D1_RECT_F mode_split_{}, mode_unified_{}, only_changes_rc_{}, ignore_ws_rc_{},
        ignore_case_rc_{}, swap_rc_{}, prev_rc_{}, next_rc_{}, position_rc_{}, summary_rc_{};
};

} // namespace pulse::ui
