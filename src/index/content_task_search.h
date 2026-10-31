// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once
#include "content_index.h"
#include "content_timing.h"
#include <optional>

namespace pulse::index {
struct ContentTaskCache {
    std::function<std::wstring(const std::wstring &)> version;
    std::function<bool(const std::wstring &)> excluded;
    std::function<bool(const std::wstring &, uint64_t, uint64_t, const std::wstring &)> fresh;
    bool native_version = false;
};
using ContentTaskReader = std::function<bool(const std::wstring &, uint64_t, std::wstring &, uint64_t &, DWORD *,
                                             text::Encoding, const std::function<bool()> &)>;
// Test hooks run on the enumerator thread and return real find handles; failures preserve GetLastError.
struct ContentTaskEnumerationHooks {
    std::function<HANDLE(const std::wstring &, WIN32_FIND_DATAW &)> first;
    std::function<bool(HANDLE, WIN32_FIND_DATAW &)> next;
};
// Callbacks run only on the calling thread. No content is retained after return.
bool RunContentTaskSupplement(const ContentIndexConfig &, const ContentSearchRequest &, const std::atomic<bool> &,
                              const ContentTaskCache &, ContentSearchProgress, size_t initial_hits,
                              ContentBatchCallback, ContentTaskReader reader = {}, ContentTiming* timing = nullptr,
                              const ContentTaskEnumerationHooks* enumeration_hooks = nullptr);
} // namespace pulse::index
