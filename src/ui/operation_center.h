// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once

#include "../common/localization.h"
#include "fluent_components.h"
#include "menu_backdrop.h"
#include "edit_frame_scheduler.h"
#include "smooth_scroll.h"
#include "../ops/ops_manager.h"
#include <array>
#include <functional>
#include <vector>

namespace pulse::ui {

// Labels are supplied by the application's localization layer; array indices
// follow OpType and OpPhase. All methods and callbacks run on the UI thread.
struct OperationCenterLabels {
    std::wstring title = l10n::Get(l10n::StringId::OperationCenter), empty = l10n::Get(l10n::StringId::OperationCenterEmpty);
    std::wstring current = l10n::Get(l10n::StringId::OperationCenterCurrent), queued = l10n::Get(l10n::StringId::OperationCenterQueued), recent = l10n::Get(l10n::StringId::OperationCenterRecent);
    std::wstring clear_finished = l10n::Get(l10n::StringId::OperationCenterClear), details = l10n::Get(l10n::StringId::ViewDetails);
    std::wstring cancel = l10n::Get(l10n::StringId::Cancel), pause = l10n::Get(l10n::StringId::OpPause), resume = l10n::Get(l10n::StringId::OpResume);
    std::wstring open_destination = l10n::Get(l10n::StringId::OperationCenterDestination);
    std::wstring resume_incomplete = l10n::Get(l10n::StringId::ResumeIncompleteTransfer);
    std::wstring remaining_time = l10n::Get(l10n::StringId::OpRemainingTime), remaining_items = l10n::Get(l10n::StringId::OpRemainingItems);
    std::wstring remaining_size = l10n::Get(l10n::StringId::OpRemainingSize), current_speed = l10n::Get(l10n::StringId::OpCurrentSpeed), estimating = l10n::Get(l10n::StringId::OpCalculating);
    std::array<std::wstring, 10> type_names{
        l10n::Get(l10n::StringId::Copy), l10n::Get(l10n::StringId::OperationMove), l10n::Get(l10n::StringId::OperationCenterRecycle), l10n::Get(l10n::StringId::Delete), l10n::Get(l10n::StringId::Rename), l10n::Get(l10n::StringId::NewFolder),
        l10n::Get(l10n::StringId::NewTextDocument), l10n::Get(l10n::StringId::Restore), l10n::Get(l10n::StringId::EmptyRecycleConfirmTitle), l10n::Get(l10n::StringId::BatchRename)};
    std::array<std::wstring, 11> phase_names{
        l10n::Get(l10n::StringId::OperationCenterQueued), l10n::Get(l10n::StringId::OpScanning), l10n::Get(l10n::StringId::OpWaitingConflict), l10n::Get(l10n::StringId::OperationCenterRunning), l10n::Get(l10n::StringId::OpPaused),
        l10n::Get(l10n::StringId::OperationCenterVerifying), l10n::Get(l10n::StringId::OperationCenterCancelling), l10n::Get(l10n::StringId::OpCompleted), l10n::Get(l10n::StringId::OpFailedBadge), l10n::Get(l10n::StringId::OperationCenterWaiting), l10n::Get(l10n::StringId::OperationCenterCancelled)};
};

struct OperationCenterCallbacks {
    std::function<void()> clear_finished;
    std::function<void(uint64_t)> details, cancel, pause, resume;
    std::function<void(const std::wstring&)> open_destination;
    std::function<bool(uint64_t)> can_resume_incomplete;
    std::function<void(uint64_t)> resume_incomplete;
};

class OperationCenter {
    friend struct OperationCenterTestPeer;
public:
    OperationCenter() = default;
    ~OperationCenter();
    OperationCenter(const OperationCenter&) = delete;
    OperationCenter& operator=(const OperationCenter&) = delete;
    bool Create(HWND owner, Compositor* owner_compositor);
    void SetCallbacks(OperationCenterCallbacks callbacks);
    void SetTheme(bool dark, D2D1_COLOR_F accent);
    void SetLabels(OperationCenterLabels labels);
    void Update(std::vector<ops::OpStatus> tasks);
    void Toggle(RECT screen_anchor);
    void Hide(bool animate = true);
    bool IsVisible() const { return open_; }
    void DismissForDeactivation(HWND activated);
    void Destroy();
    // Uses the production offscreen renderer without showing or activating it.
    bool SaveDebugSnapshot(const wchar_t* path, int width_dip = 472, int height_dip = 600);

private:
    enum class Action { Clear, Details, Cancel, Pause, Resume, Destination, ResumeIncomplete };
    struct Button {
        D2D1_RECT_F bounds{};
        Action action{};
        uint64_t task_id = 0;
        std::wstring text, path;
        float content_top = 0, content_bottom = 0;
    };
    struct Card {
        size_t task = 0;
        D2D1_RECT_F bounds{};
        std::wstring section;
        float section_top = 0, title_right = 0, source_y = 0, destination_y = 0, item_y = 0;
        float error_y = 0, progress_y = 0, statistics_y = 0, footer_y = 0;
        int statistics_columns = 3;
        bool paired_paths = false;
    };
    struct CardPosition { uint64_t task_id = 0; float top = 0; };
    struct SectionPosition { std::wstring label; float top = 0; };
    struct DepartingCard {
        uint64_t task_id = 0;
        ComPtr<ID2D1Bitmap1> bitmap;
        D2D1_RECT_F bounds{};
        float opacity = 1;
    };
    static constexpr float kOpenDurationMs = 200, kCloseDurationMs = 140, kCardDurationMs = 220;
    // Compatibility fallback when the monitor-paced high-resolution scheduler is unavailable.
    static constexpr UINT kAnimationIntervalMs = USER_TIMER_MINIMUM;
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    void Layout();
    void Position(RECT anchor);
    void UpdateHeight(bool animate);
    void UpdateVerticalPosition();
    bool Render(bool allow_capture = true);
    void Present();
    void Tick();
    bool RequestAnimationFrame();
    void BeginVisibility(bool closing);
    void PrepareCardTransition(const std::vector<ops::OpStatus>& next);
    void AnimateCardLayout();
    void DrawDepartingCards();
    void FinishCardTransition();
    void Scroll(float delta, bool smooth = false);
    bool TickScroll(uint64_t now);
    void SetScrollOffset(float value);
    void StopScroll();
    void ScrollAt(float y);
    void FocusButton(int direction);
    void Invoke(int index);
    int Hit(float x, float y) const;
    bool AnimationsEnabled() const;
    float ButtonWidth(const std::wstring& text) const;
    bool HasFinished() const;
    void Redraw();
    void Text(const std::wstring& text, D2D1_RECT_F rect, D2D1_COLOR_F color, bool small_text = true, bool wrap = false);

    HWND owner_ = nullptr, hwnd_ = nullptr;
    Compositor* compositor_ = nullptr;
    fluent::Painter painter_;
    OperationCenterLabels labels_;
    OperationCenterCallbacks callbacks_;
    std::vector<ops::OpStatus> tasks_;
    std::vector<Card> cards_;
    std::vector<Button> buttons_;
    std::vector<CardPosition> card_positions_;
    std::vector<SectionPosition> section_positions_;
    std::vector<DepartingCard> departing_cards_;
    MenuBackdropSource backdrop_source_;
    MenuBackdrop backdrop_;
    ComPtr<ID2D1Device> render_device_;
    ComPtr<ID2D1Bitmap1> render_bitmap_, readback_bitmap_;
    HDC memory_ = nullptr;
    HBITMAP dib_ = nullptr;
    void* bits_ = nullptr;
    int dib_width_ = 0, dib_height_ = 0, width_ = 0, height_ = 0;
    int target_height_ = 0, resize_from_height_ = 0;
    int base_x_ = 0, base_y_ = 0, hover_ = -1, pressed_ = -1, focused_ = -1;
    float scale_ = 1, scroll_ = 0, content_height_ = 0, body_height_ = 0;
    bool dark_ = false, open_ = false, closing_ = false, snapshot_ = false, acrylic_ = false;
    bool force_solid_ = false;
    bool dragging_scrollbar_ = false;
    SmoothScroll scroll_motion_;
    EditFrameScheduler frame_scheduler_;
    bool resizing_ = false;
    bool cards_animating_ = false, card_scroll_pending_ = false;
    float card_progress_ = 1, card_scroll_from_ = 0;
    int motion_override_ = -1; // Isolated fixture override; production follows Windows.
    D2D1_COLOR_F accent_ = HexColor(0x0078D4);
    ULONGLONG animation_start_ = 0;
    ULONGLONG resize_start_ = 0;
    ULONGLONG card_animation_start_ = 0;
    ULONGLONG origin_click_tick_ = 0;
    float visibility_ = 0, animation_from_ = 0;
    RECT anchor_{};
    RECT work_area_{};
};

} // namespace pulse::ui
