// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "fluent_components.h"
#include <span>
#include <string>
#include <vector>

namespace pulse::ui {
inline constexpr wchar_t kToolbarCustomizationWindowClass[] = L"PulseToolbarCustomization";

struct ToolbarCustomizationAction {
    int command = 0;
    std::wstring text, glyph, category;
    fluent::MenuPictogram pictogram = fluent::MenuPictogram::None;
};

struct ToolbarCustomizationSpec {
    std::wstring title, available_label, current_label;
    std::wstring add_text, remove_text, restore_text, save_text, cancel_text, up_text, down_text;
    std::vector<ToolbarCustomizationAction> actions;
    std::vector<int> order, default_order;
};

struct ToolbarCustomizationResult {
    bool accepted = false;
    std::vector<int> order;
};

class ToolbarCustomizationModel {
public:
    explicit ToolbarCustomizationModel(const ToolbarCustomizationSpec& spec);
    const std::vector<int>& Order() const { return order_; }
    bool Contains(int command) const;
    bool Add(int command);
    bool Remove(int command);
    // Move by stable ID. Zero means after the last action.
    bool Move(int command, int before_command);
    void Reset();
private:
    std::vector<int> allowed_, order_, defaults_;
};

ToolbarCustomizationResult ShowToolbarCustomization(HWND owner, const ToolbarCustomizationSpec& spec,
    bool dark, D2D1_COLOR_F accent);

// Read-only UI-thread hooks also work in the ordinary production build.
inline constexpr UINT kToolbarCustomizationInspectMessage = WM_APP + 0x2E4;
inline constexpr UINT kToolbarCustomizationSnapshotMessage = WM_APP + 0x2E5;
struct ToolbarCustomizationInspection {
    float scale = 1.0f;
    D2D1_RECT_F available{}, current{}, add{}, remove{}, up{}, down{}, restore{}, primary{}, cancel{}, close{};
    int focus = 0, available_selection = 0, current_selection = 0, dragging_command = 0;
    bool add_enabled = false, remove_enabled = false, captured = false;
    float available_scroll = 0, current_scroll = 0;
    bool available_scrolling = false, current_scrolling = false, motion_enabled = false;
    D2D1_RECT_F available_thumb{}, current_thumb{};
    float available_focus_opacity = 0, primary_hover_opacity = 0, close_hover_opacity = 0;
    bool interaction_pending = false;
    std::vector<int> order;
    struct Row {
        int command = 0;
        D2D1_RECT_F bounds{};
        bool category = false;
        float hover_opacity = 0, selection_opacity = 0;
    };
    std::vector<Row> available_rows, current_rows;
};
} // namespace pulse::ui
