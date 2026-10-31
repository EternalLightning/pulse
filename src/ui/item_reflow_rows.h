// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "ui_renderer.h"
#include <string_view>

namespace pulse::ui {

inline std::vector<int> FindRetainedReflowRows(const PaneViewModel& vm,
        const ItemReflowSurface& surface, D2D1_RECT_F viewport) {
    // Spool-backed content rows must never be read synchronously for animation.
    if (vm.content_results) return {};
    if (!vm.filter_text.empty() && !vm.filter_map) return {};
    std::unordered_map<std::wstring_view, std::unordered_set<std::wstring_view>> wanted;
    for (const auto& slot : surface.Slots()) {
        const auto visual = surface.BackgroundVisual(slot.key, slot.bounds, slot.stripe);
        const auto rc = visual.bounds;
        if (slot.source < 0 || visual.exiting || visual.opacity <= 0 ||
            rc.right <= viewport.left || rc.left >= viewport.right ||
            rc.bottom <= viewport.top || rc.top >= viewport.bottom) continue;
        const auto split = slot.key.rfind(L'\n');
        if (split == std::wstring::npos) continue;
        const std::wstring_view key(slot.key);
        wanted[key.substr(split + 1)].insert(key.substr(0, split));
    }
    if (wanted.empty()) return {};
    std::vector<int> rows;
    const size_t count = vm.snapshot ? vm.snapshot->size() : vm.entries.size();
    // Filtered-out identities cannot be retained. Visit only admitted sources,
    // preserving the source order used by unfiltered snapshots and old frames.
    std::vector<int> sorted_sources;
    const auto* sources = vm.filter_map.get();
    if (sources && !std::is_sorted(sources->begin(), sources->end())) {
        sorted_sources = *sources;
        std::sort(sorted_sources.begin(), sorted_sources.end());
        sources = &sorted_sources;
    }
    const size_t candidates = sources ? sources->size() : count;
    for (size_t candidate = 0; candidate < candidates && !wanted.empty(); ++candidate) {
        const size_t source = sources ? static_cast<size_t>((*sources)[candidate]) : candidate;
        if (source >= count) continue;
        const auto& name = vm.snapshot ? (*vm.snapshot)[source].name : vm.entries[source].name;
        const auto found = wanted.find(name);
        if (found == wanted.end()) continue;
        const int view = vm.ViewIndex(static_cast<int>(source));
        if (view < 0) continue;
        const auto& path = vm.snapshot ? (*vm.snapshot)[source].full_path : vm.entries[source].path;
        std::wstring joined;
        if (path.empty() && vm.is_file_system) {
            joined = vm.path;
            if (!joined.empty() && joined.back() != L'\\' && joined.back() != L'/') joined += L'\\';
            joined += name;
        }
        if (!found->second.erase(path.empty() ? std::wstring_view(joined) : std::wstring_view(path))) continue;
        rows.push_back(view);
        if (found->second.empty()) wanted.erase(found);
    }
    return rows;
}

} // namespace pulse::ui
