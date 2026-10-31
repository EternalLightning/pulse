// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <string>
#include <unordered_map>

namespace pulse::app {

// Slot ownership and per-path result freshness have separate lifetimes.
struct UncProbeScheduler {
    struct Request { uint64_t id; std::wstring path; };
    uint64_t active_id = 0;
    std::wstring active_path;
    std::deque<Request> queued;
    std::unordered_map<std::wstring, uint64_t> latest;

    static uint64_t NextId() {
        static std::atomic<uint64_t> next{1};
        uint64_t id = next.fetch_add(1);
        if (!id) id = next.fetch_add(1);
        return id;
    }

    uint64_t Begin(const std::wstring& path = {}) {
        active_id = NextId();
        active_path = path;
        if (!path.empty()) latest[path] = active_id;
        return active_id;
    }

    bool Enqueue(std::wstring path) {
        if ((active_id && active_path == path)) return false;
        for (const auto& request : queued) if (request.path == path) return false;
        const auto id = NextId();
        latest[path] = id;
        queued.push_back({id, std::move(path)});
        return true;
    }

    const Request* Pending() const {
        return active_id || queued.empty() ? nullptr : &queued.front();
    }

    void BeginPending() {
        active_id = queued.front().id;
        active_path = std::move(queued.front().path);
        queued.pop_front();
    }

    bool IsActive(uint64_t id) const { return id != 0 && id == active_id; }
    bool Finish(uint64_t id) {
        if (!IsActive(id)) return false;
        active_id = 0;
        active_path.clear();
        return true;
    }

    bool Accept(const std::wstring& path, uint64_t id, bool final = true) {
        const auto found = latest.find(path);
        if (!id || found == latest.end() || found->second != id) return false;
        if (final) latest.erase(found);
        return true;
    }
};

} // namespace pulse::app
