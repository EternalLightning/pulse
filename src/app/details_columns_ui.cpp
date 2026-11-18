// SPDX-License-Identifier: Apache-2.0
#include "details_columns_ui.h"
#include "app_internal.h"
#include "../ui/details_column_labels.h"

namespace pulse {
void ShowDetailsColumnsMenu(AppState& s, POINT point, int pane_index) {
    auto* pane = PaneAtSlot(s, pane_index);
    auto* tab = pane ? pane->ActiveTab() : nullptr;
    if (!tab || !EnsureMenu(s)) return;
    ui::PaneViewModel vm;
    app::FillPaneViewModel(vm, *pane, &s.places);
    if (vm.is_search) return;
    using K = model::DetailsColumnKind;
    auto prefs = tab->details_columns.configured ? tab->details_columns : model::DefaultDetailsColumns();
    std::vector<ui::FluentMenuItem> items;
    for (size_t index = 2; index < model::kDetailsColumnCount; ++index) {
        const auto kind = static_cast<K>(index);
        ui::FluentMenuItem item;
        item.text = ui::DetailsColumnLabel(kind);
        auto position = std::find(prefs.order.begin(), prefs.order.end(), kind);
        const bool visible = position != prefs.order.end();
        for (const auto& action : {std::pair{1, l10n::Pick(L"显示", L"Show")},
                                   std::pair{2, l10n::Pick(L"向前移动", L"Move earlier")},
                                   std::pair{3, l10n::Pick(L"向后移动", L"Move later")}}) {
            ui::FluentMenuItem child;
            child.text = action.second; child.command = static_cast<int>(index) * 10 + action.first;
            child.checked = action.first == 1 && visible;
            child.enabled = action.first == 1 || (visible && (action.first == 2
                ? position != prefs.order.begin() + 1 : position + 1 != prefs.order.end()));
            item.children.push_back(std::move(child));
        }
        items.push_back(std::move(item));
    }
    items.back().separator_after = true;
    ui::FluentMenuItem reset; reset.command = 1;
    reset.text = l10n::Pick(L"恢复默认列", L"Reset columns"); items.push_back(std::move(reset));
    s.menu->SetTheme(s.darkMode, s.accentColor);
    const int action = s.menu->TrackPopup(point, std::move(items));
    if (!action) return;
    if (action == 1) prefs = {};
    else {
        const auto kind = static_cast<K>(action / 10);
        auto position = std::find(prefs.order.begin(), prefs.order.end(), kind);
        if (action % 10 == 1) {
            if (position == prefs.order.end()) prefs.order.push_back(kind);
            else prefs.order.erase(position);
        } else if (action % 10 == 2 && position != prefs.order.end() && position != prefs.order.begin() + 1)
            std::iter_swap(position, position - 1);
        else if (action % 10 == 3 && position != prefs.order.end() && position + 1 != prefs.order.end())
            std::iter_swap(position, position + 1);
    }
    tab->details_columns = std::move(prefs);
    RememberFolderColumns(s, *tab);
    InvalidateRect(s.hwnd, nullptr, FALSE);
}
}
