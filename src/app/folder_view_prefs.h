#pragma once

#include "../ui/view_layout.h"
#include "../model/sort_types.h"
#include <map>
#include <optional>
#include <string>
#include <array>

namespace pulse::app {

struct FolderSortPrefs {
    model::SortColumn column = model::SortColumn::Name;
    model::SortDirection direction = model::SortDirection::Asc;
    bool operator==(const FolderSortPrefs&) const = default;
};

struct FolderGroupPrefs {
    ui::GroupMode mode = ui::GroupMode::Auto;
    model::SortDirection direction = model::SortDirection::Desc;
    bool operator==(const FolderGroupPrefs&) const = default;
};

class FolderViewPrefs {
public:
    std::optional<ui::ViewMode> Find(const std::wstring& path) const;
    bool Set(const std::wstring& path, ui::ViewMode mode);
    std::optional<FolderSortPrefs> FindSort(const std::wstring& path) const;
    bool SetSort(const std::wstring& path, model::SortColumn column, model::SortDirection direction);
    std::optional<FolderGroupPrefs> FindGroup(const std::wstring& path) const;
    bool SetGroup(const std::wstring& path, ui::GroupMode mode, model::SortDirection direction);
    std::optional<std::array<float, 4>> FindColumns(const std::wstring& path) const;
    bool SetColumns(const std::wstring& path, const std::array<float, 4>& columns);
    void Clear() { views_.clear(); sorts_.clear(); groups_.clear(); columns_.clear(); }
    void AppendJson(std::wstring& out) const;
    void ReadJson(const std::wstring& json);

private:
    struct PathLess {
        bool operator()(const std::wstring& a, const std::wstring& b) const;
    };
    std::map<std::wstring, ui::ViewMode, PathLess> views_;
    std::map<std::wstring, FolderSortPrefs, PathLess> sorts_;
    std::map<std::wstring, FolderGroupPrefs, PathLess> groups_;
    std::map<std::wstring, std::array<float, 4>, PathLess> columns_;
};

} // namespace pulse::app
