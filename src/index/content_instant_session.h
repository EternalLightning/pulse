// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once
#include "content_index.h"

namespace pulse::index {
// Deterministic transport/search injection for the instant-session regression fixture.
// Production callers leave this null and use the real feed, watches and scanner.
struct ContentInstantSessionHooks {
    using Enqueue = std::function<void(const std::wstring&, bool)>;
    using Fail = std::function<void(ContentSubscriptionFailure, DWORD)>;
    std::function<void(Enqueue, Fail)> arm;
    std::function<bool(const ContentSearchRequest&, const std::atomic<bool>&, ContentBatchCallback)> search;
};
// Owns only the active query and its change subscription; never runs a body writer.
bool RunInstantContentSession(ContentIndex& index, ContentSearchRequest request,
    const std::atomic<bool>& cancelled, ContentBatchCallback callback,
    const ContentInstantSessionHooks* hooks = nullptr);
}
