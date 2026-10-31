// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "app/app_internal.h"
#include "app/app_network_access.h"
#include <iostream>
#include <memory>

namespace pulse {
int RunNetworkAccessApplicationChecks() {
    int failures = 0;
    const auto check = [&](bool okay, const char* name) {
        std::cout << (okay ? "[PASS] " : "[FAIL] ") << name << std::endl;
        if (!okay) ++failures;
    };
    auto state = std::make_unique<AppState>();
    auto& s = *state;
    s.isolatedTest = true;
    s.places.persist = s.appPrefs.persist = s.ctxMenuPrefs.persist = s.searchHistory.persist = false;
    s.hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"STATIC", L"Network access fixture",
        WS_POPUP, -30000, -30000, 1000, 700, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    check(s.hwnd != nullptr, "network application fixture creates an isolated hidden owner");
    if (!s.hwnd) return failures;
    s.window_tabs.NewTab(app::MakeHomePath());
    s.pane = s.window_tabs.Active()->panes.front().get();
    auto& tab = *ActiveTab(s);
    s.network_access = std::make_shared<app::NetworkAccessState>();
    auto network = s.network_access;
    tab.current_path = L"\\\\fixture-server\\share";
    tab.loading = false;
    RequestNetworkAuthentication(s, tab, ERROR_ACCESS_DENIED);
    check(network->pending.empty(), "a hidden owner cannot launch a Windows credential prompt");
    const auto reset = [&] {
        CancelNetworkAuthentication(s);
        tab.current_path = L"\\\\pulse-network-fixture.invalid\\share";
        tab.view_generation = 10;
        tab.applied_generation = 20;
        tab.pending_generation = 0;
        tab.net_readonly = true;
        tab.loading = false;
        tab.banner_title = L"fixture marker";
        tab.banner_message = L"fixture details";
    };
    const auto identity = [&] {
        return app::NetworkRequestIdentity{network->epoch, tab.network_identity,
            tab.view_generation, tab.applied_generation, tab.current_path};
    };
    const auto enqueue = [&](app::NetworkRequestIdentity expected, const app::Tab* expected_tab = nullptr) {
        network->pending[expected.tab] = {expected, expected_tab ? expected_tab : &tab,
            std::make_shared<std::atomic_bool>(false)};
        network->mailbox->Publish({expected, ERROR_CANCELLED});
    };
    reset();
    auto expected = identity();
    enqueue(expected);
    PumpNetworkAccess(s);
    check(tab.banner_title == l10n::Get(l10n::StringId::CannotOpen) &&
        tab.banner_message == l10n::Get(l10n::StringId::NetworkSignInCanceled) && network->pending.empty() &&
        tab.applied_generation == 20 && !tab.loading,
        "production pump handles current cancellation once without a new listing or credential loop");
    const auto canceled_message = tab.banner_message;
    PumpNetworkAccess(s);
    check(tab.banner_message == canceled_message && network->pending.empty(), "second pump does not replay canceled results");

    for (int stale_field = 0; stale_field != 5; ++stale_field) {
        reset();
        expected = identity();
        enqueue(expected);
        const auto original_id = tab.network_identity;
        if (stale_field == 0) ++tab.view_generation;
        if (stale_field == 1) ++tab.network_identity;
        if (stale_field == 2) ++tab.applied_generation;
        if (stale_field == 3) tab.current_path += L"\\changed";
        if (stale_field == 4) tab.loading = true;
        PumpNetworkAccess(s);
        check(tab.banner_title == L"fixture marker" && tab.banner_message == L"fixture details",
            "production pump rejects stale view, tab, listing, path or superseding loading state");
        tab.network_identity = original_id;
    }
    reset();
    app::Tab different_tab;
    enqueue(identity(), &different_tab);
    PumpNetworkAccess(s);
    check(tab.banner_title == L"fixture marker", "production pump rejects another tab pointer even with copied identities");
    reset();
    expected = identity();
    expected.window = app::NextNetworkIdentity();
    enqueue(expected);
    PumpNetworkAccess(s);
    check(tab.banner_title == L"fixture marker", "production pump rejects an earlier window epoch");

    reset();
    expected = identity();
    network->pending[expected.tab] = {expected, &tab, std::make_shared<std::atomic_bool>(false)};
    RefreshPath(s, tab.current_path, RefreshReason::Background);
    check(tab.applied_generation == 20 && tab.pending_generation == 0 && tab.net_readonly &&
        network->pending.contains(tab.network_identity), "background polling preserves the credential wait identity");
    const auto probe_id = s.probe_scheduler.Begin(tab.current_path);
    fs::UncProbeResult probe;
    probe.probe_id = probe_id;
    probe.unc = tab.current_path;
    probe.status = fs::NetStatus::Online;
    probe.error_code = ERROR_SUCCESS;
    ApplyUncProbeResult(s, probe);
    check(tab.net_readonly && tab.banner_title == L"fixture marker", "late reachability probe cannot clear authentication state");

    reset();
    tab.pending_generation = 30;
    network->foreground_listings[tab.network_identity] = 30;
    s.explicit_entry_refreshes[&tab] = {tab.current_path, 30};
    RefreshPath(s, tab.current_path, RefreshReason::Background);
    check(tab.pending_generation == 30 && network->foreground_listings[tab.network_identity] == 30 &&
        s.explicit_entry_refreshes[&tab].generation == 30,
        "background polling cannot replace or consume the original F5 foreground authorization");
    s.explicit_entry_refreshes.clear();

    reset();
    enqueue(identity());
    CancelNetworkAccess(s);
    network->mailbox->Publish({identity(), ERROR_CANCELLED});
    PumpNetworkAccess(s);
    check(tab.banner_title == L"fixture marker" && network->mailbox->Take().empty() && network->pending.empty(),
        "window shutdown rejects both queued and late provider completions");

    s.network_access = std::make_shared<app::NetworkAccessState>();
    tab.current_path = app::MakeHomePath();
    tab.loading = false;
    tab.net_readonly = false;
    for (const auto discovery : {app::NetworkDiscoveryState::Unknown, app::NetworkDiscoveryState::Enabled,
            app::NetworkDiscoveryState::Disabled}) {
        s.network_access->discovery = discovery;
        const auto vm = BuildVm(s, false);
        check(vm.pane.is_home && vm.pane.network_discovery_disabled == (discovery == app::NetworkDiscoveryState::Disabled),
            "production Home view shows discovery notice only for cached explicit Disabled state");
    }
    CancelNetworkAccess(s);
    DestroyWindow(s.hwnd);
    s.hwnd = nullptr;
    return failures;
}
}
