// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "../app/header_drop_effect.h"
#include <iostream>
#include <string>

int main() {
    using namespace pulse::app;
    int failures = 0;
    const auto check = [&](bool ok, const char* label) {
        std::cout << (ok ? "[PASS] " : "[FAIL] ") << label << '\n';
        if (!ok) ++failures;
    };
    const DWORD all = DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK;
    check(HeaderDropNavigationEffect(all) == DROPEFFECT_COPY &&
          HeaderDropNavigationEffect(DROPEFFECT_MOVE | DROPEFFECT_LINK) == DROPEFFECT_LINK &&
          HeaderDropNavigationEffect(DROPEFFECT_MOVE) == DROPEFFECT_NONE,
          "deferred navigation never reports MOVE and never authorizes OLE source deletion");

    std::wstring source = L"original shortcut";
    int submissions = 0;
    const auto mutate = [&] { ++submissions; source.clear(); };
    check(!RunHeaderDropFallback(DROPEFFECT_COPY, DROPEFFECT_MOVE, mutate) &&
          source == L"original shortcut" && submissions == 0,
          "requested MOVE is rejected after COPY acceptance without touching source");
    check(!RunHeaderDropFallback(DROPEFFECT_LINK, DROPEFFECT_COPY, mutate) &&
          !RunHeaderDropFallback(DROPEFFECT_COPY, DROPEFFECT_LINK, mutate) &&
          source == L"original shortcut" && submissions == 0,
          "LINK effect mismatch never runs fallback or mutates source");
    check(RunHeaderDropFallback(DROPEFFECT_COPY, DROPEFFECT_COPY, [&] { ++submissions; }) &&
          submissions == 1 && source == L"original shortcut",
          "accepted COPY runs one fallback while retaining original source");
    return failures ? 1 : 0;
}
