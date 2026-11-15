// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "ui_compositor.h"
#include <functional>
#include <string>
#include <vector>

namespace pulse::ui {

struct InputDialogField {
    int id = 100;
    std::wstring label, text, cue;
    float top = 100.0f;
    size_t limit = 255;
    bool password = false;
};

struct InputDialogValidation {
    bool enabled = true;
    std::wstring error;
};

struct InputDialogSpec {
    const wchar_t* window_class = L"PulseInputDialog";
    std::wstring title, glyph, confirm_text;
    float width = 350.0f, height = 224.0f;
    int close_id = 101;
    std::vector<InputDialogField> fields;
    bool select_all = false;
    std::wstring checkbox_text;
    bool checked = false;
    std::wstring browse_text;
    std::function<bool(HWND, std::wstring&)> browse;
    std::function<InputDialogValidation(const std::vector<std::wstring>&)> validate;
};

struct InputDialogResult {
    bool accepted = false;
    std::vector<std::wstring> values;
    bool checked = false;
};

InputDialogResult ShowInputDialog(HWND owner, const InputDialogSpec& spec,
                                 bool dark, D2D1_COLOR_F accent);

#ifdef PULSE_UI_TEST_HOOKS
// Nonzero wParam renders without Present so UI-thread readbacks retain the frame.
inline constexpr UINT kInputDialogInspectMessage = WM_APP + 0x2D0;
inline constexpr UINT kInputDialogSnapshotMessage = WM_APP + 0x2D1;
struct InputDialogFieldInspection {
    int id = 0;
    HWND edit = nullptr;
    D2D1_RECT_F bounds{};
    bool password = false;
    bool custom = false;
};
struct InputDialogInspection {
    float scale = 1.0f, title_font_size = 0.0f;
    DWRITE_FONT_WEIGHT title_weight = DWRITE_FONT_WEIGHT_NORMAL;
    D2D1_RECT_F close{}, primary{}, cancel{}, checkbox{}, browse{};
    bool enabled = false, checked = false, high_contrast = false;
    int focus = 0;
    std::wstring checkbox_text, error;
    std::vector<InputDialogFieldInspection> fields;
    Compositor* compositor = nullptr;
};
#endif
} // namespace pulse::ui
