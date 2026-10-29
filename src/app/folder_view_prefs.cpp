#include "folder_view_prefs.h"
#include "../common/json_utils.h"
#include "../common/path_utils.h"
#include <algorithm>
#include <iterator>
#include <cmath>
#include <cwchar>

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

std::wstring GroupJsonKey(ui::GroupMode mode, model::SortDirection direction) {
    constexpr const wchar_t* names[] = {L"auto", L"none", L"date", L"type", L"size"};
    return std::wstring(L"folder_group_") + names[static_cast<unsigned>(mode)] +
        (direction == model::SortDirection::Desc ? L"_desc" : L"_asc");
}

bool ValidColumns(const std::array<float, 4>& columns) {
    // Legacy ratios (0..1) remain valid automatic widths. Manual widths use
    // DIP in the current renderer; reject corrupt or unbounded persisted data.
    return std::all_of(columns.begin(), columns.end(), [](float v) {
        return std::isfinite(v) && v >= 0.0f && (v <= 1.0f || (v >= 24.0f && v <= 4096.0f));
    });
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

std::optional<FolderGroupPrefs> FolderViewPrefs::FindGroup(const std::wstring& path) const {
    const auto it = groups_.find(FolderKey(path));
    return it == groups_.end() ? std::nullopt : std::optional(it->second);
}

bool FolderViewPrefs::SetGroup(const std::wstring& path, ui::GroupMode mode,
                              model::SortDirection direction) {
    const auto key = FolderKey(path);
    if (key.empty() || static_cast<unsigned>(mode) > static_cast<unsigned>(ui::GroupMode::Size) ||
        (direction != model::SortDirection::Asc && direction != model::SortDirection::Desc)) return false;
    const FolderGroupPrefs prefs{mode, direction};
    const auto [it, inserted] = groups_.try_emplace(key, prefs);
    if (!inserted && it->second == prefs) return false;
    it->second = prefs;
    return true;
}

std::optional<std::array<float, 4>> FolderViewPrefs::FindColumns(const std::wstring& path) const {
    const auto it = columns_.find(FolderKey(path));
    return it == columns_.end() ? std::nullopt : std::optional(it->second);
}

bool FolderViewPrefs::SetColumns(const std::wstring& path, const std::array<float, 4>& columns) {
    const auto key = FolderKey(path);
    if (key.empty() || !ValidColumns(columns)) return false;
    const auto [it, inserted] = columns_.try_emplace(key, columns);
    if (!inserted && it->second == columns) return false;
    it->second = columns;
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
    for (unsigned i = 0; i <= static_cast<unsigned>(ui::GroupMode::Size); ++i) {
        for (const auto direction : {model::SortDirection::Asc, model::SortDirection::Desc}) {
            const auto mode = static_cast<ui::GroupMode>(i);
            out += L",\n  \"" + GroupJsonKey(mode, direction) + L"\":[";
            bool first = true;
            for (const auto& [path, group] : groups_) {
                if (group != FolderGroupPrefs{mode, direction}) continue;
                if (!first) out += L",";
                first = false;
                out += L"\"";
                json::Escape(path, out);
                out += L"\"";
            }
            out += L"]";
        }
    }
    out += L",\n  \"folder_columns\":[";
    bool first = true;
    for (const auto& [path, columns] : columns_) {
        if (!first) out += L",";
        first = false;
        out += L"{\"path\":\"";
        json::Escape(path, out);
        out += L"\",\"widths\":[";
        for (size_t i = 0; i < columns.size(); ++i) {
            if (i) out += L",";
            out += std::to_wstring(columns[i]);
        }
        out += L"]}";
    }
    out += L"]";
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
    for (unsigned i = 0; i <= static_cast<unsigned>(ui::GroupMode::Size); ++i) {
        const auto mode = static_cast<ui::GroupMode>(i);
        for (const auto direction : {model::SortDirection::Asc, model::SortDirection::Desc})
            for (const auto& path : json::ExtractStringArray(doc, GroupJsonKey(mode, direction)))
                SetGroup(path, mode, direction);
    }
    const auto* stored_columns = doc.Find(L"folder_columns");
    if (stored_columns && stored_columns->kind == json::Kind::Array) {
        for (const auto& item : stored_columns->children) {
            const auto* path = item.Find(L"path");
            const auto* widths = item.Find(L"widths");
            if (!path || path->kind != json::Kind::String || !widths ||
                widths->kind != json::Kind::Array ||
                (widths->children.size() != 3 && widths->children.size() != 4)) continue;
            std::array<float, 4> columns{};
            bool valid = true;
            for (size_t i = 0; i < widths->children.size(); ++i) {
                const auto& value = widths->children[i];
                wchar_t* end = nullptr;
                columns[i] = std::wcstof(value.text.c_str(), &end);
                if (value.kind != json::Kind::Number || end == value.text.c_str() || *end) valid = false;
            }
            if (valid) SetColumns(path->text, columns);
        }
    }
}

} // namespace pulse::app
