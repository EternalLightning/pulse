// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
#pragma once

#include "../app/app_internal.h"
#include <algorithm>
#include <cstdio>
#include <limits>

inline int RunTrayDeckWindowChecks() {
    using namespace pulse;
    int failures = 0;
    size_t checks = 0;
    const auto verify = [&](const app::StagingTray& tray) {
        // Keep the former flattening implementation as an independent oracle.
        std::vector<TrayDeckEntry> all;
        const auto& batches = tray.batches();
        for (int b = static_cast<int>(batches.size()) - 1; b >= 0; --b) {
            const auto& items = batches[static_cast<size_t>(b)].items;
            for (size_t sub = 0; sub < items.size(); ++sub)
                all.push_back({b, static_cast<int>(sub), &items[sub]});
        }
        const size_t n = all.size();
        for (size_t offset : {size_t{0}, size_t{1}, n / 2, n, n + 1,
                             std::numeric_limits<size_t>::max()}) {
            for (size_t cap : {size_t{0}, size_t{1}, size_t{8}, n, n + 3,
                              std::numeric_limits<size_t>::max()}) {
                const auto actual = TrayDeckEntries(tray, offset, cap);
                bool equal = actual.size() == std::min(cap, n);
                for (size_t i = 0; equal && i < actual.size(); ++i) {
                    const auto& expected = all[(offset % n + i) % n];
                    equal = actual[i].batch == expected.batch && actual[i].sub == expected.sub &&
                        actual[i].item == expected.item;
                }
                ++checks;
                if (!equal) {
                    ++failures;
                    std::printf("[FAIL] tray window total=%zu offset=%zu cap=%zu\n", n, offset, cap);
                }
            }
        }
    };
    app::StagingTray tray;
    verify(tray);
    tray.Collect({L"C:\\tray-window-fixture\\one"}, false);
    verify(tray);
    tray.Collect({L"C:\\tray-window-fixture\\two", L"C:\\tray-window-fixture\\three",
                  L"C:\\tray-window-fixture\\four"}, true);
    verify(tray);
    std::vector<std::wstring> paths;
    for (int i = 0; i < 2048; ++i)
        paths.push_back(L"C:\\tray-window-fixture\\large-" + std::to_wstring(i));
    tray.Collect(paths, false);
    verify(tray);
    tray.RemoveItem(1, 1);
    verify(tray);
    tray.RemoveBatch(0);
    verify(tray);
    tray.Clear();
    verify(tray);
    std::printf("[%s] tray cyclic window matches flattening order and item identity (%zu checks)\n",
        failures ? "FAIL" : "PASS", checks);
    return failures ? 1 : 0;
}
