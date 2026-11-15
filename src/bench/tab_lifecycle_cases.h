// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once
#include "../app/app_model.h"
#include "../common/path_utils.h"

#include <cstdio>

namespace pulse::bench {

inline bool RunTabLifecycleTests() {
    bool passed = true;
    const auto report = [](const char* name, bool result) {
        std::printf("[%s] %s\n", result ? "PASS" : "FAIL", name);
        return result;
    };
    pulse::app::WindowTabs restored_tabs;
    restored_tabs.EnsureDefault();
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
    pulse::app::WindowTabs location_tabs;
    location_tabs.NewTab(L"");
    passed &= report("new tab preserves the empty This PC address",
        location_tabs.Active()->ActiveFolder()->current_path.empty());
    const auto duplicate_path = location_tabs.Active()->ActiveFolder()->current_path;
    location_tabs.NewTabAt(1, duplicate_path);
    passed &= report("duplicating This PC through indexed insertion preserves its address",
        location_tabs.items.size() == 2 && location_tabs.Active()->ActiveFolder()->current_path.empty());
    const auto default_path = pulse::path::DefaultNavigationRoot();
    location_tabs.NewTab(default_path);
    passed &= report("an explicit default folder opens with the canonical filesystem address",
        location_tabs.Active()->ActiveFolder()->current_path == pulse::fs::NormalizePath(default_path));
    location_tabs.NewTab(L"E:\\group-member");
    auto* active_member = location_tabs.Active();
    location_tabs.items[0]->pinned = location_tabs.items[1]->pinned = true;
    location_tabs.items[0]->tab_group = location_tabs.items[3]->tab_group = 7;
    location_tabs.items[1]->tab_group = 8;
    pulse::app::NormalizeGroupRuns(location_tabs);
    passed &= report("group normalization excludes pins and preserves the pinned prefix and active identity",
        location_tabs.items[0]->pinned && location_tabs.items[1]->pinned &&
        !location_tabs.items[2]->pinned && !location_tabs.items[3]->pinned &&
        location_tabs.items[0]->tab_group == 0 && location_tabs.items[1]->tab_group == 0 &&
        location_tabs.Active() == active_member && active_member->tab_group == 7);
    return passed;
}

} // namespace pulse::bench
