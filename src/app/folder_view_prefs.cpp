#include "folder_view_prefs.h"
#include "../common/json_utils.h"
#include "../common/path_utils.h"
#include <algorithm>
#include <iterator>

namespace pulse::app {
namespace {

std::wstring FolderKey(const std::wstring& path) {
    if (path.empty() || path.starts_with(L"pulse:")) return {};
    std::wstring key = path::StripExtendedPathPrefix(path);
    std::replace(key.begin(), key.end(), L'/', L'\\');
    // Accept only absolute filesystem paths; virtual views have separate rules.
    if (!(key.size() >= 3 && key[1] == L':' && key[2] == L'\\') &&
        !key.starts_with(L"\\\\")) return {};
    while (key.size() > 3 && key.back() == L'\\') key.pop_back();
    return key;
}

std::wstring JsonKey(ui::ViewMode mode) {
    return std::wstring(L"folder_view_") + ui::ViewModeName(mode);
}

constexpr model::SortColumn kSortColumns[] = {
    model::SortColumn::Name, model::SortColumn::Mtime, model::SortColumn::Type,
    model::SortColumn::Size, model::SortColumn::Path
};

std::wstring SortJsonKey(model::SortColumn column, model::SortDirection direction) {
    constexpr const wchar_t* names[] = {L"name", L"mtime", L"type", L"size", L"path"};
    return std::wstring(L"folder_sort_") + names[static_cast<unsigned>(column)] +
        (direction == model::SortDirection::Desc ? L"_desc" : L"_asc");
}

} // namespace

bool FolderViewPrefs::PathLess::operator()(const std::wstring& a, const std::wstring& b) const {
    return CompareStringOrdinal(a.c_str(), static_cast<int>(a.size()),
                                b.c_str(), static_cast<int>(b.size()), TRUE) == CSTR_LESS_THAN;
}

std::optional<ui::ViewMode> FolderViewPrefs::Find(const std::wstring& path) const {
    const auto it = views_.find(FolderKey(path));
    return it == views_.end() ? std::nullopt : std::optional(it->second);
}

bool FolderViewPrefs::Set(const std::wstring& path, ui::ViewMode mode) {
    const auto key = FolderKey(path);
    if (key.empty() || static_cast<unsigned>(mode) >= 8) return false;
    const auto [it, inserted] = views_.try_emplace(key, mode);
    if (!inserted && it->second == mode) return false;
    it->second = mode;
    return true;
}

std::optional<FolderSortPrefs> FolderViewPrefs::FindSort(const std::wstring& path) const {
    const auto it = sorts_.find(FolderKey(path));
    return it == sorts_.end() ? std::nullopt : std::optional(it->second);
}

bool FolderViewPrefs::SetSort(const std::wstring& path, model::SortColumn column,
                             model::SortDirection direction) {
    const auto key = FolderKey(path);
    if (key.empty() || static_cast<unsigned>(column) >= std::size(kSortColumns) ||
        (direction != model::SortDirection::Asc && direction != model::SortDirection::Desc)) return false;
    const FolderSortPrefs sort{column, direction};
    const auto [it, inserted] = sorts_.try_emplace(key, sort);
    if (!inserted && it->second == sort) return false;
    it->second = sort;
    return true;
}

void FolderViewPrefs::AppendJson(std::wstring& out) const {
    // Group paths by stable view names, using the existing string-array codec.
    for (int i = 0; i < 8; ++i) {
        const auto mode = ui::ViewModeFromIndex(i);
        out += L",\n  \"" + JsonKey(mode) + L"\":[";
        bool first = true;
        for (const auto& [path, saved_mode] : views_) {
            if (saved_mode != mode) continue;
            if (!first) out += L",";
            first = false;
            out += L"\"";
            json::Escape(path, out);
            out += L"\"";
        }
        out += L"]";
    }
    for (const auto column : kSortColumns) {
        for (const auto direction : {model::SortDirection::Asc, model::SortDirection::Desc}) {
            out += L",\n  \"" + SortJsonKey(column, direction) + L"\":[";
            bool first = true;
            for (const auto& [path, sort] : sorts_) {
                if (sort != FolderSortPrefs{column, direction}) continue;
                if (!first) out += L",";
                first = false;
                out += L"\"";
                json::Escape(path, out);
                out += L"\"";
            }
            out += L"]";
        }
    }
}

void FolderViewPrefs::ReadJson(const std::wstring& input) {
    const json::Document doc(input);
    Clear();
    for (int i = 0; i < 8; ++i) {
        const auto mode = ui::ViewModeFromIndex(i);
        for (const auto& path : json::ExtractStringArray(doc, JsonKey(mode)))
            Set(path, mode);
    }
    for (const auto column : kSortColumns) {
        for (const auto direction : {model::SortDirection::Asc, model::SortDirection::Desc}) {
            for (const auto& path : json::ExtractStringArray(doc, SortJsonKey(column, direction)))
                SetSort(path, column, direction);
        }
    }
}

} // namespace pulse::app
