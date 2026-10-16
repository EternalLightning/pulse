#pragma once

#include "FluentTokens.h"
#include "fluent_components.h"
#include "ui_compositor.h"
#include "../ops/ops_manager.h"

#include <deque>
#include <functional>
#include <string>

namespace pulse::ui {

#ifdef PULSE_UI_TEST_HOOKS
inline constexpr UINT kConfirmSnapshotMessage = WM_APP + 0x2B1;
inline constexpr UINT kConfirmInspectMessage = WM_APP + 0x2B2;
struct ConfirmDialogInspection {
    size_t row_count = 0;
    float scroll = 0.0f;
    float max_scroll = 0.0f;
    D2D1_RECT_F content{};
    D2D1_RECT_F primary{};
    D2D1_RECT_F cancel{};
    D2D1_RECT_F alternate{};
};
#endif

struct FileOperationCallbacks {
    std::function<void()> cancel;
    std::function<void()> pause;
    std::function<void()> resume;
    std::function<void()> dismiss;
};

struct FileOperationWindowOptions {
    bool use_summary_title = false;
    bool estimated_bytes = false;
    bool item_progress_known = true;
    bool modal_owner = false;
};

#ifdef PULSE_UI_TEST_HOOKS
struct FileOperationProgressInspection {
    std::wstring title, subtitle, file_line, percent, remaining_label, remaining, time, speed, peak;
    bool show_pause = false;
    bool indeterminate = false;
    D2D1_RECT_F details{}, pause{}, cancel{};
};
#endif

class FileOperationWindow {
public:
    FileOperationWindow();
    ~FileOperationWindow();

    bool Create(HWND owner, FileOperationCallbacks callbacks, FileOperationWindowOptions options = {});
    void Destroy();
    void SetTheme(bool dark, D2D1_COLOR_F accent);
    void Update(const ops::OpStatus& status);
    void Show(bool activate = true);
    void Hide();
    bool IsVisible() const;

private:
#ifdef PULSE_UI_TEST_HOOKS
    friend struct FileOperationFailureProbe;
#endif
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
    LRESULT HandleMessage(UINT message, WPARAM wparam, LPARAM lparam);
    void ApplyWindowTheme();
    void Render(bool present = true);
    void ResizeForDetails(bool preserve_center);
    float DetailBlockHeight() const;
    void ScrollFailureDetails(float delta);
    int HitTestControl(float x, float y) const;
    void RequestClose();

    HWND hwnd_ = nullptr;
    HWND owner_ = nullptr;
    Compositor compositor_;
    fluent::Painter painter_;
    FileOperationCallbacks callbacks_;
    FileOperationWindowOptions options_;
#ifdef PULSE_UI_TEST_HOOKS
    FileOperationProgressInspection progress_inspection_;
#endif
    ops::OpStatus status_;
    std::deque<double> speed_history_;
    ULONGLONG speed_sample_tick_ = 0;
    bool dark_ = false;
    bool backdrop_enabled_ = false;
    bool detailed_ = false;
    bool positioned_ = false;
    bool cancel_requested_ = false;
    float scale_ = 1.0f;
    float failure_scroll_ = 0.0f;
    float failure_scroll_max_ = 0.0f;
    D2D1_COLOR_F accent_ = HexColor(0x0078D4);
    int hover_ = 0;
    int pressed_ = 0;
};

struct ConflictDialogResult {
    ops::ConflictChoice choice = ops::ConflictChoice::Cancel;
    bool apply_to_all = false;
};

ConflictDialogResult ShowFileConflictDialog(HWND owner,
                                            const ops::ConflictItemInfo& conflict,
                                            bool dark,
                                            D2D1_COLOR_F accent);

struct ConfirmDialogRow {
    std::wstring text;
    bool is_path = true;
    bool boxed = true;
};

struct ConfirmDialogSpec {
    std::wstring title;
    std::wstring message;
    std::wstring confirm_text;
    std::wstring cancel_text;
    bool danger = false;
    bool cancel_is_default = false;
    // Deletion may expire/cancel while the native modal message loop is open.
    std::function<bool()> still_valid;
    bool fit_to_work_area = false;
    std::wstring checkbox_text;
    bool* checkbox_checked = nullptr; // optional result; changed only on acceptance
    std::wstring alternate_text;
    bool* alternate_selected = nullptr;
    std::vector<ConfirmDialogRow> rows;
    std::wstring note;
};

ConfirmDialogSpec BuildLinkConfirmationSpec(const ops::ConflictItemInfo& conflict);

struct LockedItemDialogText {
    std::wstring title;
    std::wstring message; // {name} is replaced with the failed path's leaf
    std::wstring close_hint;
    std::wstring end_hint;
    std::wstring retry;
    std::wstring end_retry;
    std::wstring cancel;
};
enum class LockedItemChoice { Cancel, Retry, EndAndRetry };
LockedItemChoice ShowLockedItemDialog(HWND owner, const ops::OpStatus& status,
    const LockedItemDialogText& text, bool dark, D2D1_COLOR_F accent,
    std::function<bool()> still_valid);

bool ShowRenameLockedDialog(HWND owner, const ops::OpStatus& status, bool dark,
    D2D1_COLOR_F accent, std::function<bool()> still_valid);

bool ShowConfirmDialog(HWND owner, const ConfirmDialogSpec& spec, bool dark,
                       D2D1_COLOR_F accent);

} // namespace pulse::ui
