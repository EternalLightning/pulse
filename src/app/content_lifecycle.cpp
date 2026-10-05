#include "app_internal.h"
#include "../common/localization.h"

namespace pulse {
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
