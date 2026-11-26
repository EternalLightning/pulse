// SPDX-License-Identifier: Apache-2.0
#include "file_picker_listing.h"
#include "../common/display_path.h"
#include "../common/localization.h"
#include "../model/entry_sort.h"
#include "../model/item_filter.h"
#include "../model/places.h"

#include <shlwapi.h>
#include <algorithm>
#include <numeric>

namespace pulse::ui {
namespace {

bool ReadDirectory(ListingRequest& request, std::vector<fs::DirEntry>& entries) {
    if (request.directory.empty()) {
        const DWORD drives = GetLogicalDrives();
        for (DWORD i = 0; i < 26; ++i) {
            if (!(drives & (DWORD{1} << i))) continue;
            fs::DirEntry entry;
            entry.name = {static_cast<wchar_t>(L'A' + i), L':', L'\\'};
            entry.is_dir = true;
            entries.push_back(std::move(entry));
        }
        return true;
    }

    const auto cancelled = [&request] { return request.cancelled.load(std::memory_order_relaxed); };
    if (!fs::EnumerateDirectory(request.directory, entries, cancelled)) return false;
    std::erase_if(entries, [&](const fs::DirEntry& entry) {
        if (!request.visibility.Includes(entry.attrs)) return true;
        if (entry.is_dir) return false;
        if (request.mode == PickerMode::Folder) return true;
        const bool excluded = !request.filter.empty() &&
            PathMatchSpecExW(entry.name.c_str(), request.filter.c_str(), PMSF_MULTIPLE) != S_OK;
        request.type_filtered |= excluded;
        return excluded;
    });
    request.directory = path::FriendlyPathText(fs::NormalizePath(std::move(request.directory)));
    return true;
}

void ProjectDirectory(ListingRequest& request) {
    if (!request.project_only) {
        std::vector<fs::DirEntry> entries;
        if (!ReadDirectory(request, entries)) return;
        request.base = std::make_shared<const std::vector<fs::DirEntry>>(std::move(entries));
    }
    auto& projection = request.projection;
    auto& rows = projection.indices;
    request.filtered = request.type_filtered || !request.query.empty();
    if (request.query.empty()) {
        rows.resize(request.base->size());
        std::iota(rows.begin(), rows.end(), 0);
    } else {
        const auto filter = model::CompileCatalogItemFilter(request.query, request.catalog.get());
        if (!filter.Valid()) {
            request.query_error = l10n::Get(l10n::StringId::ItemFilterInvalid);
            if (!filter.error_token.empty()) request.query_error += L" " + filter.error_token;
            request.succeeded = true;
            return;
        }
        std::wstring prefix;
        const bool needs_path = filter.HasTagConditions() || std::any_of(filter.metadata.begin(), filter.metadata.end(),
            [](const auto& term) { return term.name_in_path; });
        const model::ItemFilterTagLookup lookup = [&](const std::wstring& full_path) {
            return request.catalog ? request.catalog->TagIndicesForPath(full_path) : nullptr;
        };
        if (needs_path) {
            prefix = request.directory;
            if (!prefix.empty() && prefix.back() != L'\\') prefix += L'\\';
        }
        for (size_t source = 0; source < request.base->size(); ++source) {
            if (request.cancelled.load(std::memory_order_relaxed)) return;
            const auto& entry = (*request.base)[source];
            const auto full_path = needs_path ? (entry.full_path.empty() ? prefix + entry.name : entry.full_path) : std::wstring{};
            if (filter.Matches(entry, full_path, lookup)) rows.push_back(static_cast<int>(source));
        }
    }
    if (request.cancelled.load(std::memory_order_relaxed)) return;
    std::sort(rows.begin(), rows.end(), [&](int left, int right) {
        return model::EntryLess((*request.base)[static_cast<size_t>(left)], (*request.base)[static_cast<size_t>(right)],
            request.sort_column, request.sort_direction, request.folder_sort, request.directory);
    });
    const auto [mode, direction] = request.grouping;
    if (mode != GroupMode::None && !rows.empty()) {
        SYSTEMTIME today{};
        if (mode == GroupMode::DateModified) GetLocalTime(&today);
        projection = BuildGroupedView(request.base, &rows, mode, direction, today);
    } else if (!rows.empty()) {
        projection.source_to_view.assign(request.base->size(), -1);
        for (size_t row = 0; row < rows.size(); ++row)
            projection.source_to_view[static_cast<size_t>(rows[row])] = static_cast<int>(row);
    }
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
            else ProjectDirectory(request);
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
