#pragma once
#include "../app/session.h"
#include "../common/path_utils.h"

#include <cstdio>
#include <string>
#include <vector>

namespace pulse::bench {

inline bool RunSessionLayoutTests() {
    bool passed = true;
    const auto report = [](const char* name, bool result) {
        std::printf("[%s] %s\n", result ? "PASS" : "FAIL", name);
        return result;
    };
    pulse::app::LayoutTabSnapshot saved;
    saved.pinned = true;
    saved.marker_rgb = 0x0078D4;
    saved.group = 4;
    saved.layout = 1;
    saved.focused = 1;
    saved.panes.push_back({ L"C:\\first", pulse::ui::ViewMode::Tiles, { 0.2f, 0.5f, 0.8f }, {} });
    saved.panes.push_back({ L"D:\\loose", pulse::ui::ViewMode::Details, {}, {} });
    saved.panes.push_back({ L"", pulse::ui::ViewMode::Details, {}, {} });
    saved.split_ratios = {0.35f};
    pulse::app::LayoutTab restored;
    std::vector<std::wstring> loaded_paths;
    pulse::app::RestoreLayoutTab(restored, saved,
        [&](pulse::app::Tab& tab, const std::wstring& loaded) {
            tab.current_path = loaded;
            loaded_paths.push_back(loaded);
        });
    passed &= report("session layout restore keeps only the preset pane slots",
        restored.pinned && restored.tab_group == 0 &&
        restored.layout == pulse::app::LayoutPreset::TwoVertical &&
        restored.panes.size() == 2 && restored.focused_index == 1 &&
        loaded_paths.size() >= 2 && loaded_paths[0] == L"C:\\first");
    passed &= report("session layout restore builds the live tree with the saved ratio",
        restored.root && !restored.root->is_leaf && restored.root->ratio == 0.35f &&
        restored.root->first->pane == restored.panes[0].get() &&
        restored.root->second->pane == restored.panes[1].get());
    const auto captured = pulse::app::CaptureLayoutTab(restored);
    auto cloud_saved = saved;
    cloud_saved.panes[0].columns = {140, 120, 90, 88};
    cloud_saved.panes[0].search_columns = {200, 140, 120, 90, 96};
    std::vector<pulse::app::LayoutTabSnapshot> cloud_restored;
    passed &= report("session preserves cloud widths in ordinary and search layouts",
        pulse::app::ParseLayoutTabs(pulse::app::LayoutTabsToJson({cloud_saved}), cloud_restored) &&
        cloud_restored.size() == 1 && cloud_restored[0].panes[0].columns == cloud_saved.panes[0].columns &&
        cloud_restored[0].panes[0].search_columns == cloud_saved.panes[0].search_columns);
    passed &= report("older session columns retain widths with automatic cloud status width",
        pulse::app::ParseLayoutTabs(L"[{\"panes\":[{\"cols\":\"1400000,1200000,900000\","
            L"\"searchCols\":\"2000000,1400000,1200000,900000\"}]}]", cloud_restored) &&
        cloud_restored[0].panes[0].columns == std::array<float, 4>{140, 120, 90, 0} &&
        cloud_restored[0].panes[0].search_columns == std::array<float, 5>{200, 140, 120, 90, 0});
    passed &= report("session layout restore and capture preserve the tab marker",
        restored.marker_rgb == 0x0078D4 && captured.marker_rgb == 0x0078D4);
    passed &= report("session layout capture preserves folder presentation state",
        captured.pinned && captured.layout == 1 && captured.panes.size() == 2 &&
        captured.panes[0].view == pulse::ui::ViewMode::Tiles &&
        captured.panes[0].columns[1] == 0.5f);
    pulse::app::LayoutTab empty_restored;
    pulse::app::RestoreLayoutTab(empty_restored, {},
        [](pulse::app::Tab& tab, const std::wstring& loaded) {
            tab.current_path = loaded;
        });
    passed &= report("session layout restore supplies a usable fallback folder",
        empty_restored.panes.size() == 1 &&
        empty_restored.panes[0]->view.current_path == pulse::path::DefaultNavigationRoot() &&
        empty_restored.root && empty_restored.root->pane == empty_restored.panes[0].get());
    auto invalid_ratios = saved;
    invalid_ratios.split_ratios = {0.2f, 0.8f};
    pulse::app::RestoreLayoutTab(empty_restored, invalid_ratios, {});
    passed &= report("session layout restore defaults mismatched ratios without losing paths",
        empty_restored.root && empty_restored.root->ratio == 0.5f &&
        empty_restored.panes[0]->view.current_path == L"C:\\first");
    auto nested = saved;
    nested.layout = static_cast<int>(pulse::app::LayoutPreset::FourGrid);
    nested.panes.resize(4);
    nested.split_ratios = {0.3f, 0.4f, 0.6f};
    pulse::app::RestoreLayoutTab(empty_restored, nested, {});
    passed &= report("session layout restore applies nested split ratios in capture order",
        empty_restored.root && empty_restored.root->ratio == 0.3f &&
        empty_restored.root->first->ratio == 0.4f && empty_restored.root->second->ratio == 0.6f);
    nested.split_ratios = {-0.2f, 0.4f, 1.0f};
    pulse::app::RestoreLayoutTab(empty_restored, nested, {});
    passed &= report("session layout restore defaults invalid ratios while keeping valid branches",
        empty_restored.root->ratio == 0.5f && empty_restored.root->first->ratio == 0.4f &&
        empty_restored.root->second->ratio == 0.5f);
    pulse::app::WindowTabs restored_tabs;
    std::vector<pulse::app::GroupSessionSnapshot> groups{
        { 4, L"work", 0x00112233, false },
        { 4, L"duplicate", 0x00445566, true },
        { -1, L"invalid", 0, false },
    };
    saved.pinned = false;
    saved.group = 4;
    pulse::app::RestoreWindowTabs(restored_tabs, { saved }, groups, 0,
        [&](pulse::app::Tab& tab, const std::wstring& loaded) {
            tab.current_path = loaded;
        });
    passed &= report("session window tabs restore validates groups",
        restored_tabs.tab_groups.size() == 1 &&
        restored_tabs.next_tab_group_id == 5 && restored_tabs.items.size() == 1 &&
        restored_tabs.items[0]->tab_group == 4);
    int fallback_loads = 0;
    pulse::app::RestoreWindowTabs(restored_tabs, {}, {}, 0,
        [&](pulse::app::Tab& tab, const std::wstring& path) {
            ++fallback_loads;
            tab.current_path = path;
        });
    passed &= report("empty window restore loads the same fallback and builds its tree",
        fallback_loads == 1 && restored_tabs.Active() && restored_tabs.Active()->root &&
        restored_tabs.Active()->ActiveFolder()->current_path == pulse::path::DefaultNavigationRoot());
    int retired_tabs = 0;
    pulse::app::LayoutTab* retired = nullptr;
    restored_tabs.before_close = [&](pulse::app::LayoutTab& tab) {
        ++retired_tabs;
        retired = &tab;
    };
    auto* first_tab = restored_tabs.Active();
    restored_tabs.NewTab(L"D:\\second");
    restored_tabs.items.front()->pinned = true;
    restored_tabs.CloseTab(0);
    passed &= report("pinned tab close does not retire live work", retired_tabs == 0);
    restored_tabs.items.front()->pinned = false;
    restored_tabs.CloseTab(0);
    passed &= report("all model tab closes retire the selected tab before erasing it",
        retired_tabs == 1 && retired == first_tab && restored_tabs.items.size() == 1);
    restored_tabs.CloseTab(0);
    passed &= report("model refuses to retire its surviving singleton", retired_tabs == 1);
    std::vector<pulse::app::LayoutTabSnapshot> excess_tabs(pulse::app::kMaxSessionLayoutTabs + 1);
    std::vector<pulse::app::GroupSessionSnapshot> excess_groups(pulse::app::kMaxSessionTabGroups + 1);
    for (size_t i = 0; i < excess_groups.size(); ++i) excess_groups[i].id = static_cast<int>(i + 1);
    fallback_loads = 0;
    pulse::app::RestoreWindowTabs(restored_tabs, excess_tabs, excess_groups, 0,
        [&](pulse::app::Tab&, const std::wstring&) { ++fallback_loads; });
    passed &= report("direct session restoration bounds tabs groups and loader requests",
        restored_tabs.items.size() == pulse::app::kMaxSessionLayoutTabs &&
        restored_tabs.tab_groups.size() == pulse::app::kMaxSessionTabGroups &&
        fallback_loads == static_cast<int>(pulse::app::kMaxSessionLayoutTabs));
    return passed;
}

} // namespace pulse::bench
