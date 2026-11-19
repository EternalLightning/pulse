// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <windows.h>
#include <d2d1.h>

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace pulse::ui {

enum class PickerMode { Open, Save, Folder };

struct PickerFilter {
    std::wstring label;
    std::wstring pattern;
};

struct FilePickerResult {
    std::vector<std::wstring> paths;
    size_t filter_index = 0;
};

struct PickerSubmitReply {
    bool accepted = false;
    bool retry_allowed = true;
    std::wstring error;
};

struct PickerPlace {
    std::wstring label;
    std::wstring directory;
    std::wstring glyph;
};

struct PickerPlaces {
    std::vector<PickerPlace> onedrive;
    std::vector<PickerPlace> quick_access;
    std::vector<PickerPlace> network;
};

using PickerPlacesLoader = std::function<PickerPlaces(const std::function<bool()>& cancelled)>;

struct FilePickerSpec {
    PickerMode mode = PickerMode::Open;
    std::wstring title;
    std::wstring ok_button_label;
    std::wstring file_name_label;
    std::wstring initial_path;
    std::wstring filename;
    std::vector<PickerFilter> filters;
    size_t filter_index = 0;
    bool allow_multiselect = false;
    std::wstring multiselect_directory;
    bool show_hidden = false;
    bool software_device = false;
    bool verify_copies = false;
    bool cover_source = false;

    // Called on a worker when opening the picker.
    PickerPlacesLoader load_places;

    // Callback work must be nonblocking. An index of zero is the first filter.
    std::function<bool(const FilePickerResult&)> begin_submit;
    std::function<bool()> begin_cancel;
    std::function<std::optional<PickerSubmitReply>()> poll_submit;
    std::function<bool()> should_close;
    std::function<void(HWND)> on_ready;
    std::function<void()> on_use_windows;
    std::function<std::wstring()> current_pulse_directory;
};

// The source HWND supplies placement. It retains its own lifetime and input state.
bool ShowFilePicker(HWND source, const FilePickerSpec& spec, bool dark,
                    D2D1_COLOR_F accent, FilePickerResult& result);

} // namespace pulse::ui
