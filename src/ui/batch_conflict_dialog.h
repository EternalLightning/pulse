#pragma once
#include "window_helpers.h"
#include "../ops/ops_manager.h"
#include <optional>
#include <vector>

namespace pulse::ui {

struct BatchConflictLabels {
    std::wstring title = L"Resolve file conflicts";
    std::wstring instruction = L"Choose an action for each item. Keep both is selected by default.";
    std::wstring source = L"Source", destination = L"Existing destination";
    std::wstring replace = L"Replace", skip = L"Skip", keep_both = L"Keep both";
    std::wstring apply_all = L"Apply to every row", confirm = L"Continue", cancel = L"Cancel";
    std::wstring folder = L"Folder", reparse = L"Link", unknown = L"Unknown";
};

std::optional<std::vector<ops::ConflictResolution>> ShowBatchFileConflictDialog(
    HWND owner, const ops::ConflictItemInfo& conflict, const BatchConflictLabels& labels,
    bool dark, D2D1_COLOR_F accent);

// Implementation type is shared only with the isolated production-renderer fixture.
class BatchConflictDialog {
    friend struct BatchConflictDialogTestPeer;
public:
    ~BatchConflictDialog();
    std::optional<std::vector<ops::ConflictResolution>> Show(HWND owner,
        const ops::ConflictItemInfo& conflict, const BatchConflictLabels& labels,
        bool dark, D2D1_COLOR_F accent);

private:
    enum class Action { Apply, Select, Confirm, Cancel };
    struct Control {
        D2D1_RECT_F bounds{};
        Action action = Action::Cancel;
        size_t row = 0;
        ops::ConflictChoice choice = ops::ConflictChoice::KeepBoth;
        std::wstring text;
    };
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    bool Initialize(HWND owner, const ops::ConflictItemInfo& conflict,
        const BatchConflictLabels& labels, bool dark, D2D1_COLOR_F accent, bool interactive);
    bool Valid() const;
    void Layout();
    bool Render(bool present = true);
    void Text(const std::wstring& text, const D2D1_RECT_F& bounds, D2D1_COLOR_F color, bool heading = false);
    void Scroll(float delta);
    void ScrollAt(float y);
    void Focus(int direction);
    int Hit(float x, float y) const;
    void Invoke(int control);
    void Complete(bool accepted);
    std::wstring Metadata(const ops::ConflictRowInfo& row, bool source) const;
    void Destroy();

    HWND owner_ = nullptr, hwnd_ = nullptr;
    Compositor compositor_;
    fluent::Painter painter_;
    ops::ConflictItemInfo conflict_;
    BatchConflictLabels labels_;
    std::vector<ops::ConflictResolution> selections_;
    std::vector<Control> controls_;
    std::vector<D2D1_RECT_F> rows_;
    std::optional<std::vector<ops::ConflictResolution>> result_;
    D2D1_RECT_F viewport_{};
    float scale_ = 1, scroll_ = 0, content_height_ = 0, row_height_ = 0;
    float header_height_ = 0, footer_height_ = 0;
    int hover_ = -1, pressed_ = -1, focus_ = -1;
    bool dark_ = false, backdrop_ = false, done_ = false, owner_disabled_ = false;
    bool dragging_ = false;
    D2D1_COLOR_F accent_ = HexColor(0x0078D4);
};

} // namespace pulse::ui
