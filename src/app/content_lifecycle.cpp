// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "app_internal.h"
#include "content_results_ui.h"
#include "../common/localization.h"

namespace pulse {
void TeardownTabSearch(AppState& s, const app::Tab& tab) {
    CancelNetworkAuthenticationForTab(s, tab);
    CancelContentSelection(s, tab);
    if (!tab.search_session_id) return;
    s.index.CancelSession(tab.search_session_id);
    s.networkIndex.CancelSession(tab.search_session_id);
    s.contentSearch.Cancel(tab.search_session_id);
    std::erase_if(s.pendingIndexSearches, [&](const auto& pending) {
        return pending.second.query.session_id == tab.search_session_id;
    });
}

void MarkContentSearchStopped(app::Tab& tab) {
    tab.search_content_stopped = true;
    tab.pending_generation = tab.search_live_generation = 0;
    tab.search_content_active = tab.search_awaiting_content = tab.search_loading_more = false;
    tab.loading = false;
    tab.SetBannerTitle(app::BannerType::Other, l10n::Get(l10n::StringId::ContentSearchStopped));
    tab.banner_message.clear();
}

void SuspendContentSearches(AppState& s) {
    s.addressLiveDue = 0;
    s.contentSearch.Suspend();
    ForEachPane(s, [&](app::Pane& pane) {
        auto& tab = pane.view;
        if (!tab.search_input_content && !tab.search_content_active && !tab.content_results && !tab.search_live_generation) return;
        MarkContentSearchStopped(tab);
    });
}
}
