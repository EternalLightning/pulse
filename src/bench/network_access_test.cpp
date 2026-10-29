#include "../app/network_connect.h"
#include "../app/network_locations.h"
#include <iostream>

int main() {
    using namespace pulse::app;
    int failed = 0;
    const auto check = [&](bool okay, const char* name) {
        std::cout << (okay ? "[PASS] " : "[FAIL] ") << name << '\n';
        if (!okay) ++failed;
    };
    check(IsNetworkAuthenticationError(ERROR_LOGON_FAILURE) && IsNetworkAuthenticationError(ERROR_ACCESS_DENIED),
        "only authentication and permission failures can request credentials");
    check(!IsNetworkAuthenticationError(ERROR_BAD_NETPATH) && !IsNetworkAuthenticationError(ERROR_CANCELLED) &&
        !IsNetworkAuthenticationError(ERROR_NETWORK_UNREACHABLE), "offline and canceled listings never request credentials");
    check(UncShareRoot(L"\\\\server\\share\\child") == L"\\\\server\\share" &&
        UncShareRoot(L"\\\\?\\UNC\\server\\share\\child") == L"\\\\server\\share" &&
        UncShareRoot(L"Z:\\").empty(), "authentication targets UNC shares without touching mapped drives");
    check(BeginNetworkShareRequest(L"\\\\SERVER\\SHARE") && !BeginNetworkShareRequest(L"\\\\server\\share"),
        "parallel windows deduplicate credential requests by share");
    EndNetworkShareRequest(L"\\\\server\\share");
    check(BeginNetworkShareRequest(L"\\\\server\\share"), "completed or canceled connection releases the share for explicit retry");
    EndNetworkShareRequest(L"\\\\server\\share");
    NetworkRequestIdentity identity{1, 2, 3, 4, L"\\\\server\\share"};
    NetworkConnectResult result{identity, ERROR_CANCELLED};
    const int first_tab = 0, second_tab = 0;
    check(NetworkResultMatches(result, identity, identity, &first_tab, &first_tab, false),
        "current canceled result can update the original tab with retry feedback");
    auto stale = identity;
    for (int field = 0; field != 5; ++field) {
        stale = identity;
        if (field == 0) ++stale.window;
        if (field == 1) ++stale.tab;
        if (field == 2) ++stale.view;
        if (field == 3) ++stale.listing;
        if (field == 4) stale.path += L"\\different";
        check(!NetworkResultMatches(result, identity, stale, &first_tab, &first_tab, false),
            "window, tab, view, listing and path changes reject old connection results");
    }
    check(!NetworkResultMatches(result, identity, identity, &first_tab, &second_tab, false) &&
        !NetworkResultMatches(result, identity, identity, &first_tab, &first_tab, true),
        "copied tab identities and superseding loading requests cannot receive a result");
    NetworkConnectMailbox mailbox;
    mailbox.Publish(result);
    check(mailbox.Take().size() == 1 && mailbox.Take().empty(), "result is consumed once");
    mailbox.Publish(result);
    mailbox.Close();
    mailbox.Publish(result);
    check(mailbox.Take().empty(), "window close drains queued results and rejects late provider completion");
    using D = NetworkDiscoveryState;
    check(SummarizeNetworkDiscoveryProfiles({}) == D::Unknown &&
        SummarizeNetworkDiscoveryProfiles({D::Disabled, D::Unknown}) == D::Unknown &&
        SummarizeNetworkDiscoveryProfiles({D::Enabled, D::Disabled}) == D::Unknown,
        "unknown or mixed active profiles never claim discovery is disabled");
    check(SummarizeNetworkDiscoveryProfiles({D::Disabled, D::Disabled}) == D::Disabled &&
        SummarizeNetworkDiscoveryProfiles({D::Enabled, D::Enabled}) == D::Enabled,
        "discovery notice requires every active firewall profile to be explicitly disabled");
    return failed ? 1 : 0;
}
