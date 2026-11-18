// SPDX-License-Identifier: Apache-2.0
// Adapted from chenzai666/pulse at fc6dae4a2737c2bc3014972fb24d30219bd7624f.
// Modified for this Pulse distribution. See LICENSE and NOTICE.

#pragma once

#include "../ui/file_picker_dialog.h"

#include <functional>
#include <string>
#include <vector>

namespace pulse::app {

enum class NativeDialogKind { Open, Save, Folder };

struct NativeFileDialog {
    HWND window = nullptr;
    DWORD process_id = 0;
    NativeDialogKind kind = NativeDialogKind::Open;
    std::wstring title;
    std::wstring ok_button_label;
    std::wstring initial_path;
    std::wstring filename;
    std::vector<ui::PickerFilter> filters;
    size_t filter_index = 0;
    bool allow_multiselect = false;
    DWORD window_thread_id = 0;
    FILETIME process_created{};
    HWND filename_window = nullptr;
    HWND filename_parent = nullptr;
    int filename_id = 0;
    HWND confirm_window = nullptr;
    HWND cancel_window = nullptr;
    HWND filter_window = nullptr;
    bool legacy_folder = false;
};

// Inspect and prepare on a COM-initialized worker on the source desktop.
// The caller verifies foreground ownership before opening the replacement.
bool InspectNativeFileDialog(HWND window, NativeFileDialog& dialog, std::wstring& reason);
// Window and control identity only. Safe to poll while a native modal prompt is active.
bool NativeDialogStillMatches(const NativeFileDialog& dialog);

bool PrepareNativeDialogSelection(const NativeFileDialog& dialog, const std::vector<std::wstring>& paths,
    size_t filter_index, NativeFileDialog& prepared, std::wstring& reason,
    const std::function<bool()>& cancelled);
// The native dialog owns validation. A modern folder dialog uses the provider's COM invoke pattern.
bool ConfirmNativeDialogSelection(const NativeFileDialog& prepared, std::wstring& reason,
    const std::function<bool()>& cancelled, bool& confirmation_attempted);
bool CancelNativeDialog(const NativeFileDialog& dialog, std::wstring& reason,
    const std::function<bool()>& cancelled = {});

} // namespace pulse::app
