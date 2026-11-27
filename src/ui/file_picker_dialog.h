// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "view_layout.h"
#include "../model/details_columns.h"
#include "../model/entry_sort.h"
#include "../model/file_visibility.h"
#include <windows.h>
#include <d2d1.h>

#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace pulse::model { class PlacesCatalog; }

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

struct FilePickerSnapshot {
    std::wstring directory;
    std::wstring filename;
    size_t filter_index = 0;
    std::vector<std::wstring> selections;
    bool operator==(const FilePickerSnapshot&) const = default;
};

struct PickerSubmitReply {
    bool accepted = false;
    bool retry_allowed = true;
    std::wstring error;
    bool silent = false;
};

struct PickerPlace {
    std::wstring label;
    std::wstring directory;
    std::wstring glyph;
    D2D1_COLOR_F icon_color{};
    bool use_path_icon = false;
    bool native_folder = false;
};

struct PickerPlaces {
    std::vector<PickerPlace> onedrive;
    std::vector<PickerPlace> open_tabs;
    std::vector<PickerPlace> quick_access;
    std::vector<PickerPlace> network;
    std::shared_ptr<const model::PlacesCatalog> catalog;
    std::vector<PickerPlace> portable_devices;
};

using PickerPlacesLoader = std::function<PickerPlaces(const std::function<bool()>& cancelled)>;

struct PickerFolderView {
    ViewMode mode = ViewMode::Details;
    model::SortColumn sort_column = model::SortColumn::Name;
    model::SortDirection sort_direction = model::SortDirection::Asc;
    GroupMode group_mode = GroupMode::Auto;
    model::SortDirection group_direction = model::SortDirection::Desc;
    std::array<float, 4> column_dividers{};
    model::DetailsColumnPrefs columns;
};

struct FilePickerSpec {
    PickerMode mode = PickerMode::Open;
    std::wstring title;
    std::wstring ok_button_label;
    std::wstring file_name_label;
    std::wstring custom_button_label;
    std::wstring initial_path;
    std::wstring filename;
    std::vector<PickerFilter> filters;
    size_t filter_index = 0;
    bool allow_multiselect = false;
    std::wstring multiselect_directory;
    model::FileVisibility visibility;
    bool zebra_rows = true;
    bool software_device = false;
    bool verify_copies = false;
    bool cover_source = false;
    bool modal_owner = false;
    bool external_handoff = false;
    model::FolderSortMode folder_sort = model::FolderSortMode::FoldersFirst;
    std::function<PickerFolderView(const std::wstring&)> folder_view;

    // Called on a worker when opening the picker.
    PickerPlacesLoader load_places;

    // Callback work must be nonblocking. An index of zero is the first filter.
    std::function<bool(const FilePickerResult&)> begin_submit;
    std::function<bool()> begin_cancel;
    std::function<std::optional<PickerSubmitReply>()> poll_submit;
    std::function<bool()> should_close;
    std::function<void(HWND)> on_ready;
    std::function<void(const FilePickerSnapshot&)> on_snapshot;
    std::function<bool(const FilePickerSnapshot&, const std::wstring&)> begin_folder_change;
    std::function<std::optional<bool>()> poll_folder_change;
    std::function<void(const FilePickerSnapshot&)> on_custom_button;
    std::function<void()> on_use_windows;
    std::function<void(std::wstring)> on_open_native_folder;
    std::function<bool(HWND, bool accepted)> defer_hide;
    std::function<void()> on_foreground_close;
    std::function<std::wstring()> current_pulse_directory;
};

// The source HWND supplies placement. It retains its own lifetime and input state.
bool ShowFilePicker(HWND source, const FilePickerSpec& spec, bool dark,
                    D2D1_COLOR_F accent, FilePickerResult& result);

// Recheck on the existing picker watch timer until the activation has settled.
bool DeferFilePickerHide(HWND picker, HWND target, ULONGLONG& deadline);

} // namespace pulse::ui
