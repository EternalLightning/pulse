// SPDX-License-Identifier: Apache-2.0
#include "file_picker_listing.h"
#include "../common/display_path.h"

#include <shlwapi.h>
#include <algorithm>

namespace pulse::ui {
namespace {

void ReadDirectory(ListingRequest& request) {
    auto& entries = request.entries;
    if (request.directory.empty()) {
        const DWORD drives = GetLogicalDrives();
        for (DWORD i = 0; i < 26; ++i) {
            if (!(drives & (DWORD{1} << i))) continue;
            fs::DirEntry entry;
            entry.name = {static_cast<wchar_t>(L'A' + i), L':', L'\\'};
            entry.is_dir = true;
            entries.push_back(std::move(entry));
        }
        request.succeeded = true;
        return;
    }

    const auto cancelled = [&request] { return request.cancelled.load(std::memory_order_relaxed); };
    if (!fs::EnumerateDirectory(request.directory, entries, cancelled)) return;
    std::erase_if(entries, [&](const fs::DirEntry& entry) {
        if (!request.show_hidden && (entry.attrs & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM))) return true;
        if (entry.is_dir) return false;
        if (request.mode == PickerMode::Folder) return true;
        const bool excluded = !request.filter.empty() &&
            PathMatchSpecExW(entry.name.c_str(), request.filter.c_str(), PMSF_MULTIPLE) != S_OK;
        request.filtered |= excluded;
        return excluded;
    });
    std::sort(entries.begin(), entries.end(), [](const fs::DirEntry& left, const fs::DirEntry& right) {
        if (left.is_dir != right.is_dir) return left.is_dir;
        const int order = CompareStringEx(LOCALE_NAME_USER_DEFAULT, NORM_IGNORECASE | SORT_DIGITSASNUMBERS,
            left.name.c_str(), -1, right.name.c_str(), -1, nullptr, nullptr, 0);
        return order == CSTR_LESS_THAN || ((!order || order == CSTR_EQUAL) && left.name < right.name);
    });
    request.directory = path::FriendlyPathText(fs::NormalizePath(std::move(request.directory)));
    request.succeeded = true;
}

void CALLBACK LoadDirectory(PTP_CALLBACK_INSTANCE, void* argument) {
    const std::unique_ptr<Listing> work(static_cast<Listing*>(argument));
    auto& request = **work;
    DWORD previous = 0;
    const BOOL changed_error_mode = SetThreadErrorMode(SEM_FAILCRITICALERRORS, &previous);
    try {
        const auto cancelled = [&] { return request.cancelled.load(std::memory_order_relaxed); };
        if (!cancelled()) {
            if (request.load_places) request.places = request.load_places(cancelled);
            else ReadDirectory(request);
        }
    } catch (const std::exception&) {
        request.succeeded = false;
    }
    if (changed_error_mode) SetThreadErrorMode(previous, nullptr);
    request.ready.store(true, std::memory_order_release);
    if (!request.cancelled.load(std::memory_order_relaxed))
        PostMessageW(request.window, kPickerListingReady, request.token, 0);
}

} // namespace

bool SubmitPickerListing(const Listing& request) {
    static std::atomic<UINT_PTR> next_token{1};
    request->token = next_token.fetch_add(1, std::memory_order_relaxed);
    auto work = std::make_unique<Listing>(request);
    if (!TrySubmitThreadpoolCallback(LoadDirectory, work.get(), nullptr)) return false;
    work.release();
    return true;
}

} // namespace pulse::ui
