// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "file_picker_dialog.h"
#include "../fs/fs_enum.h"

#include <atomic>
#include <memory>

namespace pulse::ui {

inline constexpr UINT kPickerListingReady = WM_APP + 0x39;

struct ListingRequest {
    HWND window = nullptr;
    UINT_PTR token = 0;
    std::wstring directory;
    PickerPlacesLoader load_places;
    PickerPlaces places;
    PickerMode mode = PickerMode::Open;
    std::wstring filter;
    bool show_hidden = false;
    bool back = false;
    std::atomic<bool> cancelled{false};
    std::atomic<bool> ready{false};
    bool succeeded = false;
    bool filtered = false;
    std::vector<fs::DirEntry> entries;
};

using Listing = std::shared_ptr<ListingRequest>;

bool SubmitPickerListing(const Listing& request);

} // namespace pulse::ui
