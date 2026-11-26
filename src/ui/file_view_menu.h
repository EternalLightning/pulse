// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "fluent_menu.h"
#include "sort_types.h"
#include "view_layout.h"
#include "../common/localization.h"

namespace pulse::ui {

inline FluentMenuItem FileOrderMenuItem(int command, l10n::StringId label, bool selected, bool enabled) {
    FluentMenuItem row;
    row.command = command;
    row.text = l10n::Get(label);
    row.enabled = enabled;
    row.radio_group = true;
    row.radio = selected;
    return row;
}

inline constexpr SortColumn kFileSortColumns[] = {
    SortColumn::Name, SortColumn::Mtime, SortColumn::Type, SortColumn::Size, SortColumn::Path
};
inline constexpr GroupMode kFileGroupModes[] = {
    GroupMode::Auto, GroupMode::None, GroupMode::DateModified, GroupMode::Type, GroupMode::Size
};

inline std::vector<FluentMenuItem> BuildFileSortChoices(SortColumn column, SortDirection direction,
    int command_base, bool enabled = true, bool show_path = false, bool indexed_search = false) {
    using I = l10n::StringId;
    constexpr I labels[] = {I::ColumnName, I::ColumnModified, I::ColumnType, I::ColumnSize, I::ColumnPath};
    std::vector<FluentMenuItem> items;
    for (int i = 0; i < (show_path ? 5 : 4); ++i) {
        const auto field = kFileSortColumns[i];
        items.push_back(FileOrderMenuItem(command_base + i, labels[i], enabled && column == field,
            enabled && !(indexed_search && (field == SortColumn::Type || field == SortColumn::Path))));
    }
    items.back().separator_after = true;
    items.push_back(FileOrderMenuItem(command_base + 5, I::SortAscending, enabled && direction == SortDirection::Asc, enabled));
    items.push_back(FileOrderMenuItem(command_base + 6, I::SortDescending, enabled && direction == SortDirection::Desc, enabled));
    return items;
}

inline std::vector<FluentMenuItem> BuildFileGroupingChoices(GroupMode mode, SortDirection direction,
    int command_base, int ascending_command, int descending_command, bool enabled = true) {
    using I = l10n::StringId;
    constexpr I labels[] = {I::GroupAuto, I::GroupNone, I::GroupDate, I::GroupType, I::GroupSize};
    std::vector<FluentMenuItem> items;
    for (int i = 0; i < 5; ++i)
        items.push_back(FileOrderMenuItem(command_base + i, labels[i], kFileGroupModes[i] == mode, enabled));
    items.back().separator_after = true;
    const bool can_order = enabled && mode != GroupMode::Auto && mode != GroupMode::None;
    items.push_back(FileOrderMenuItem(ascending_command, I::GroupAscending, direction == SortDirection::Asc, can_order));
    items.push_back(FileOrderMenuItem(descending_command, I::GroupDescending, direction == SortDirection::Desc, can_order));
    return items;
}

inline std::vector<FluentMenuItem> BuildFileViewChoices(ViewMode current, int command_base) {
    using I = l10n::StringId;
    constexpr I labels[] = {I::ViewExtraLarge, I::ViewLarge, I::MediumIcons, I::ViewSmall,
        I::ViewList, I::ViewDetails, I::ViewTiles, I::ViewContent};
    constexpr const wchar_t* glyphs[] = {L"\xE7F4", L"\xE7F4", L"\xE7F4", L"\xECA5",
        L"\xEA37", L"\xE8A5", L"\xECA5", L"\xE8FD"};
    std::vector<FluentMenuItem> items;
    items.reserve(8);
    for (int i = 0; i < 8; ++i) {
        FluentMenuItem row;
        row.command = command_base + i;
        row.text = l10n::Get(labels[i]);
        row.glyph = glyphs[i];
        if (i == 0) row.glyph_scale = 1.16f;
        else if (i == 2) row.glyph_scale = 0.82f;
        row.radio = ViewModeIndex(current) == i;
        items.push_back(std::move(row));
    }
    return items;
}

} // namespace pulse::ui
