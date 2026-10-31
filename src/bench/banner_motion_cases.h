// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once
#include "../app/banner_animation.h"
#include "../ui/banner_reveal.h"

template<class Check>
inline void RunBannerMotionChecks(Check&& check) {
    using pulse::app::BannerAnimation;
    check(pulse::ui::BannerRevealOpacity(0.5f, 18.0f) == 0.0f &&
        pulse::ui::BannerRevealOpacity(0.8f, 28.8f) > 0.05f,
        "banner content and actions wait for enough height to show a complete line");
    check(pulse::ui::BannerRevealOpacity(-1.0f, 36.0f) == 1.0f &&
        pulse::ui::BannerRevealOpacity(1.0f, 36.0f) == 1.0f &&
        pulse::ui::BannerRevealOpacity(0.0f, 0.0f) == 0.0f,
        "default, fully open and collapsed banners have exact reveal endpoints");
    BannerAnimation banner;
    banner.Tick(L"C:\\first", L"", L"", L"", 0, 1000, true);
    check(banner.expansion.value == 0.0f && !banner.expansion.Pending(),
        "initially empty banner has no expansion");
    banner.Tick(L"C:\\first", L"Network", L"Unavailable", L"\\\\server", 2, 1010, true);
    check(banner.expansion.value == 0.0f && banner.expansion.Pending() && banner.message == L"Unavailable",
        "entering banner retains text and schedules its first animation frame");
    banner.Tick(L"C:\\first", L"Network", L"Unavailable", L"\\\\server", 2, 1100, true);
    const float entering = banner.expansion.value;
    check(entering > 0.0f && entering < 1.0f,
        "entering banner has intermediate height and opacity");
    banner.Tick(L"C:\\first", L"Network", L"Unavailable", L"\\\\server", 2, 1190, true);
    check(banner.expansion.value == 1.0f && !banner.expansion.Pending(),
        "entering banner settles at full height");
    banner.Tick(L"C:\\first", L"", L"", L"", 0, 1200, true);
    check(banner.expansion.Pending() && banner.title == L"Network" && banner.message == L"Unavailable" &&
        banner.network_search_root == L"\\\\server" && banner.kind == 2,
        "removal preserves all painted banner content while it collapses");
    banner.Tick(L"C:\\first", L"", L"", L"", 0, 1250, true);
    const float exiting = banner.expansion.value;
    banner.Tick(L"C:\\first", L"New", L"Recovered", L"", 0, 1250, true);
    check(exiting > 0.0f && exiting < 1.0f && banner.expansion.value == exiting &&
        banner.expansion.target == 1.0f && banner.message == L"Recovered",
        "same-page reversal preserves sampled height and immediately uses the current content");
    banner.Tick(L"C:\\first", L"New", L"Recovered", L"", 0, 1430, true);
    banner.Tick(L"C:\\first", L"", L"", L"", 0, 1440, true);
    banner.Tick(L"C:\\first", L"", L"", L"", 0, 1620, true);
    check(banner.expansion.value == 0.0f && !banner.expansion.Pending() && banner.message.empty() &&
        banner.title.empty() && banner.network_search_root.empty(),
        "collapsed banner releases retained content");
    banner.Tick(L"C:\\second", L"Destination", L"Visible", L"", 0, 1630, true);
    check(banner.MatchesPath(L"C:\\second") && banner.expansion.value == 1.0f && !banner.expansion.Pending(),
        "navigation initializes a visible destination banner immediately");
    banner.Tick(L"C:\\third", L"", L"", L"", 0, 1640, true);
    check(banner.expansion.value == 0.0f && banner.message.empty() && !banner.expansion.Pending(),
        "navigation to an empty banner does not collapse the previous page content");
    banner.Tick(L"C:\\third", L"", L"Visible", L"", 0, 1650, true);
    banner.Tick(L"C:\\third", L"", L"Visible", L"", 0, 1680, false);
    check(banner.expansion.value == 1.0f && !banner.expansion.Pending(),
        "reduced-motion or high-contrast policy snaps an in-flight banner expansion");
    banner.Tick(L"C:\\third", L"", L"", L"", 0, 1690, false);
    check(banner.expansion.value == 0.0f && banner.message.empty() && !banner.expansion.Pending(),
        "disabled motion removes a banner and its retained text immediately");
}
