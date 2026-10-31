// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once

#include "../app/app_model.h"
#include "../app/places.h"
#include <cstdio>

inline int RunWorkspaceBadgeChecks() {
    using namespace pulse;
    int checks = 0, failures = 0;
    const auto check = [&](bool passed, const char* message) {
        ++checks;
        failures += !passed;
        std::printf("[%s] %s\n", passed ? "PASS" : "FAIL", message);
    };
    l10n::Initialize(GetModuleHandleW(nullptr), L"zh-CN");
    app::PlacesCatalog places;
    places.persist = false;
    places.PinWorkspace(L"C:\\fixture\\Pulse", L"Pulse", 0, {});
    places.PinWorkspace(L"C:\\fixture\\Other", L"Other", 0, {});
    places.PinWorkspace(L"C:\\fixture\\Pulse\\nested", L"Nested", 0, {});
    places.PinWorkspace(L"\\\\server\\share\\Project", L"Server", 0, {});
    places.PinWorkspace(L"D:\\", L"Drive", 0, {});
    places.active_workspace = 0;
    app::WindowTabs tabs;
    tabs.NewTab(app::MakeHomePath());
    auto& pane = *tabs.Active()->FocusedPane();
    app::SidebarModel sidebar;
    const auto current_is = [&](const app::Pane& source, int expected) {
        const auto vm = app::BuildWindowViewModel(source, sidebar, true, false, true, &places);
        for (const auto& group : vm.sidebar) {
            if (group.id != static_cast<int>(app::SidebarSectionId::Workspaces)) continue;
            int rows = 0;
            for (const auto& item : group.items) {
                if (item.indent) continue;
                const int index = rows++;
                const bool server = index == 3;
                const auto badge = index == expected
                    ? l10n::Get(server ? l10n::StringId::CurrentServer : l10n::StringId::Current)
                    : server ? l10n::Get(l10n::StringId::Server) : std::wstring{};
                if (item.badge != badge) return false;
            }
            return rows == static_cast<int>(places.workspaces.size());
        }
        return false;
    };
    check(current_is(pane, -1), "home hides the remembered workspace badge");
    const auto navigate = [&](const wchar_t* path, int expected, const char* message) {
        pane.ActiveTab()->NavigateTo(path);
        check(current_is(pane, expected), message);
    };
    navigate(L"C:\\fixture\\Pulse", 0, "workspace root shows Current");
    navigate(L"C:/FIXTURE/pulse/src/", 0, "descendants match case-insensitively with normalized separators");
    navigate(L"\\\\?\\C:\\fixture\\Pulse\\src", 0, "extended local paths match the same workspace");
    navigate(L"C:\\fixture\\Pulse-other", -1, "a shared name prefix cannot activate a workspace");
    navigate(L"C:\\fixture", -1, "the parent directory is outside the workspace");
    navigate(L"C:\\fixture\\Other\\src", 1, "manual navigation ignores the remembered workspace index");
    navigate(L"C:\\fixture\\Pulse\\nested\\src", 2, "the most specific nested workspace gets the sole Current badge");
    navigate(L"C:\\fixture\\Pulse\\src", 0, "leaving a nested workspace restores the enclosing workspace badge");
    navigate(L"\\\\SERVER\\share\\project\\src", 3, "UNC descendants show Current and Server together");
    navigate(L"\\\\?\\UNC\\server\\share\\Project", 3, "extended UNC roots match ordinary UNC workspace paths");
    navigate(L"\\\\server\\share\\Project-other", -1, "UNC name prefixes retain only the Server badge");
    navigate(L"\\\\server\\other\\Project", -1, "a different share cannot activate a workspace");
    navigate(L"D:\\", 4, "a drive-root workspace matches its own root");
    navigate(L"D:\\folder", 4, "a drive-root workspace includes its descendants");
    for (const auto* path : {L"", L"pulse:home", L"pulse:settings", L"pulse:recycle", L"pulse:network",
                              L"pulse:search:path:C:\\fixture\\Pulse test"})
        navigate(path, -1, "This PC and virtual locations hide Current");
    places.active_workspace = -1;
    navigate(L"C:\\fixture\\Pulse", 0, "direct navigation works with no remembered workspace");
    places.active_workspace = 999;
    check(current_is(pane, 0), "a stale saved index cannot affect the visible Current badge");
    tabs.NewTab(app::MakeHomePath());
    check(current_is(*tabs.Active()->FocusedPane(), -1), "switching to a home tab hides Current while the project tab stays open");
    tabs.active = 0;
    check(current_is(pane, 0), "switching back to the project tab restores Current");
    app::Pane other_pane;
    other_pane.NewTab(L"C:\\fixture\\Other");
    check(current_is(other_pane, 1) && current_is(pane, 0),
        "each focused pane or window derives its own badge from its visible path");
    check(places.active_workspace == 999, "rendering a badge does not rewrite the saved workspace selection");
    places.workspaces[0].root += L'\\';
    check(current_is(pane, 0), "a saved root with a trailing separator still matches its directory");
    std::printf("WORKSPACE BADGES checks=%d failures=%d\n", checks, failures);
    return failures ? 1 : 0;
}
