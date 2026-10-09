#pragma once

#include "../ui/view_layout.h"
#include "../model/sort_types.h"
#include <map>
#include <optional>
#include <string>

namespace pulse::app {

struct FolderSortPrefs {
    model::SortColumn column = model::SortColumn::Name;
    model::SortDirection direction = model::SortDirection::Asc;
    bool operator==(const FolderSortPrefs&) const = default;
};

class FolderViewPrefs {
public:
    std::optional<ui::ViewMode> Find(const std::wstring& path) const;
    bool Set(const std::wstring& path, ui::ViewMode mode);
    std::optional<FolderSortPrefs> FindSort(const std::wstring& path) const;
    bool SetSort(const std::wstring& path, model::SortColumn column, model::SortDirection direction);
    void Clear() { views_.clear(); sorts_.clear(); }
    void AppendJson(std::wstring& out) const;
    void ReadJson(const std::wstring& json);

private:
    struct PathLess {
        bool operator()(const std::wstring& a, const std::wstring& b) const;
    };
    std::map<std::wstring, ui::ViewMode, PathLess> views_;
    std::map<std::wstring, FolderSortPrefs, PathLess> sorts_;
};

} // namespace pulse::app
