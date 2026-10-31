// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "../common/path_utils.h"
#include "session.h"
#include "tab_group_id.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <unordered_set>

namespace pulse::app {

namespace {

void CollectTreePanes(const SplitContainer& node, std::vector<Pane*>& out) {
    if (node.is_leaf) {
        if (node.pane) out.push_back(node.pane);
        return;
    }
    if (node.first) CollectTreePanes(*node.first, out);
    if (node.second) CollectTreePanes(*node.second, out);
}

void CollectTreeRatios(const SplitContainer& node, std::vector<float>& out) {
    if (node.is_leaf) return;
    out.push_back(node.ratio);
    if (node.first) CollectTreeRatios(*node.first, out);
    if (node.second) CollectTreeRatios(*node.second, out);
}

void RestoreTreeRatios(SplitContainer& node, const std::vector<float>& ratios, size_t& index) {
    if (node.is_leaf) return;
    const float ratio = ratios[index++];
    node.ratio = std::isfinite(ratio) && ratio > 0.0f && ratio < 1.0f ? ratio : 0.5f;
    if (node.first) RestoreTreeRatios(*node.first, ratios, index);
    if (node.second) RestoreTreeRatios(*node.second, ratios, index);
}

} // namespace

LayoutTabSnapshot CaptureLayoutTab(const LayoutTab& tab) {
    LayoutTabSnapshot snapshot;
    snapshot.title = tab.title;
    snapshot.marker_rgb = tab.marker_rgb;
    snapshot.pinned = tab.pinned;
    snapshot.group = tab.tab_group;
    snapshot.layout = static_cast<int>(tab.layout);
    snapshot.focused = tab.focused_index;
    snapshot.target = tab.target_index;
    std::vector<Pane*> vis;
    if (tab.root) {
        CollectTreeRatios(*tab.root, snapshot.split_ratios);
        CollectTreePanes(*tab.root, vis);
    } else {
        vis.reserve(tab.panes.size());
        for (const auto& pane : tab.panes) vis.push_back(pane.get());
    }
    snapshot.panes.reserve(vis.size());
    for (const Pane* pane : vis) {
        const Tab* view = pane ? pane->ActiveTab() : nullptr;
        if (!view) {
            snapshot.panes.emplace_back();
            continue;
        }
        snapshot.panes.push_back({view->current_path, view->view_mode,
            view->details_column_dividers, view->search_column_dividers,
            view->column_layout, view->column_widths_dip});
    }
    return snapshot;
}

void RestoreLayoutTab(LayoutTab& tab, const LayoutTabSnapshot& snapshot,
                      const SessionTabLoader& load_tab) {
    tab.title = snapshot.title;
    tab.marker_rgb = snapshot.marker_rgb;
    tab.pinned = snapshot.pinned;
    tab.tab_group = snapshot.pinned ? 0 : snapshot.group;
    tab.layout = static_cast<LayoutPreset>(std::clamp(snapshot.layout, 0, 4));
    tab.focused_index = snapshot.focused;
    tab.target_index = snapshot.target;
    tab.panes.clear();
    tab.root.reset();
    const size_t need = LayoutPresetCount(tab.layout);
    for (size_t i = 0; i < snapshot.panes.size() && i < need; ++i) {
        const auto& saved = snapshot.panes[i];
        auto pane = std::make_unique<Pane>();
        pane->view.current_path = saved.path; // An empty saved path represents This PC.
        pane->view.view_mode = saved.view;
        pane->view.details_column_dividers = saved.columns;
        pane->view.search_column_dividers = saved.search_columns;
        pane->view.column_layout = saved.column_layout;
        pane->view.column_widths_dip = saved.column_widths;
        Tab* raw = pane->ActiveTab();
        tab.panes.push_back(std::move(pane));
        if (load_tab) load_tab(*raw, saved.path);
    }
    if (tab.panes.empty()) {
        auto pane = std::make_unique<Pane>();
        pane->focused = true;
        pane->view.current_path = pulse::path::DefaultNavigationRoot();
        tab.panes.push_back(std::move(pane));
        if (load_tab) load_tab(*tab.panes.front()->ActiveTab(), tab.panes.front()->view.current_path);
    }
    while (tab.panes.size() < need) {
        auto pane = std::make_unique<Pane>();
        const Tab& source = *tab.panes.front()->ActiveTab();
        pane->view.view_mode = source.view_mode;
        pane->view.details_column_dividers = source.details_column_dividers;
        pane->view.search_column_dividers = source.search_column_dividers;
        pane->view.column_layout = source.column_layout;
        pane->view.column_widths_dip = source.column_widths_dip;
        const std::wstring clone = source.current_path;
        pane->view.current_path = clone;
        Tab* raw = pane->ActiveTab();
        tab.panes.push_back(std::move(pane));
        if (load_tab) load_tab(*raw, clone);
    }
    tab.focused_index = std::clamp(tab.focused_index, 0,
                                   static_cast<int>(tab.panes.size()) - 1);
    if (tab.target_index < -1 || tab.target_index >= static_cast<int>(tab.panes.size())) tab.target_index = -1;
    for (size_t i = 0; i < tab.panes.size(); ++i) {
        tab.panes[i]->focused = static_cast<int>(i) == tab.focused_index;
        tab.panes[i]->target = static_cast<int>(i) == tab.target_index;
    }
    RebuildLayoutRoot(tab);
    if (tab.root && snapshot.split_ratios.size() == need - 1) {
        size_t index = 0;
        RestoreTreeRatios(*tab.root, snapshot.split_ratios, index);
    }
}

void RestoreWindowTabs(WindowTabs& tabs,
                       const std::vector<LayoutTabSnapshot>& layout_tabs,
                       const std::vector<GroupSessionSnapshot>& groups,
                       int active_index,
                       const SessionTabLoader& load_tab) {
    tabs.items.clear();
    tabs.tab_groups.clear();
    std::unordered_set<int> valid_groups;
    int max_group = 0;
    for (const auto& saved : groups) {
        if (tabs.tab_groups.size() == kMaxSessionTabGroups) break;
        if (saved.id <= 0 || !valid_groups.insert(saved.id).second) continue;
        tabs.tab_groups.push_back({saved.id, saved.name, saved.color_rgb, saved.collapsed});
        max_group = std::max(max_group, saved.id);
    }
    tabs.next_tab_group_id = NextTabGroupId(max_group);
    for (const auto& saved : layout_tabs) {
        if (tabs.items.size() == kMaxSessionLayoutTabs) break;
        // Loading can reuse requests from panes already registered in the window.
        tabs.items.push_back(std::make_unique<LayoutTab>());
        auto& tab = tabs.items.back();
        RestoreLayoutTab(*tab, saved, load_tab);
        if (tab->tab_group != 0 && !valid_groups.contains(tab->tab_group))
            tab->tab_group = 0;
    }
    if (tabs.items.empty()) {
        auto tab = std::make_unique<LayoutTab>();
        RestoreLayoutTab(*tab, {}, load_tab);
        tabs.items.push_back(std::move(tab));
    }
    tabs.active = 0;
    if (active_index >= 0 && active_index < static_cast<int>(tabs.items.size()))
        tabs.active = static_cast<size_t>(active_index);
    NormalizeGroupRuns(tabs);
}

} // namespace pulse::app
