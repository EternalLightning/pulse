// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "file_picker_dialog.h"
#include "date_groups.h"
#include "sort_types.h"

#include <atomic>
#include <memory>

namespace pulse::ui {

inline constexpr UINT kPickerListingReady = WM_APP + 0x39;
enum class PickerNavigation { Navigate, Back, Forward };

struct ListingRequest {
    HWND window = nullptr;
    UINT_PTR token = 0;
    std::wstring directory;
    PickerPlacesLoader load_places;
    PickerPlaces places;
    std::shared_ptr<const model::PlacesCatalog> catalog;
    PickerMode mode = PickerMode::Open;
    std::wstring filter;
    std::wstring query, query_error;
    SortColumn sort_column = SortColumn::Name;
    SortDirection sort_direction = SortDirection::Asc;
    FileGrouping grouping;
    model::FolderSortMode folder_sort = model::FolderSortMode::FoldersFirst;
    std::optional<PickerFolderView> folder_view;
    fs::SnapshotPtr base;
    bool project_only = false;
    bool type_filtered = false;
    model::FileVisibility visibility;
    PickerNavigation navigation = PickerNavigation::Navigate;
    std::atomic<bool> cancelled{false};
    std::atomic<bool> ready{false};
    bool succeeded = false;
    bool filtered = false;
    GroupedView projection;
};

using Listing = std::shared_ptr<ListingRequest>;

bool SubmitPickerListing(const Listing& request);

} // namespace pulse::ui
