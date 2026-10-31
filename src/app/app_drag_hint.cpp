// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "app_internal.h"

namespace pulse {

void ClearHeaderDropHint(AppState& s) {
    std::lock_guard lock(s.header_drop_query->mutex);
    ++s.header_drop_query->hint_generation;
    s.header_drop_query->hint_sources.clear();
    s.header_drop_query->folder_hint.clear();
    s.header_drop_query->hint_ready = false;
}

std::wstring HeaderDropHint(AppState& s, const std::vector<std::wstring>& sources) {
    if (sources.empty()) return {};
    const auto state = s.header_drop_query;
    uint64_t generation = 0;
    bool start = false;
    {
        std::lock_guard lock(state->mutex);
        if (state->hint_sources != sources) {
            generation = ++state->hint_generation;
            state->hint_sources = sources;
            state->folder_hint.clear();
            state->hint_ready = false;
            start = true;
        }
    }
    if (start) {
        const std::weak_ptr<HeaderDropQueryState> weak = state;
        const HWND hwnd = s.hwnd;
        const bool accepted = s.worker.EnqueueIo([weak, hwnd, generation, sources] {
            const auto cancelled = [weak, generation] {
                const auto current = weak.lock();
                if (!current) return true;
                std::lock_guard lock(current->mutex);
                return current->hint_generation != generation;
            };
            auto folder = ui::FirstDroppableFolder(sources, cancelled);
            if (const auto current = weak.lock()) {
                std::lock_guard lock(current->mutex);
                if (current->hint_generation != generation) return;
                current->folder_hint = std::move(folder);
                current->hint_ready = true;
                PostMessageW(hwnd, WM_HEADER_DROP_RESULT, 0, 0);
            }
        });
        if (!accepted) {
            std::lock_guard lock(state->mutex);
            if (state->hint_generation == generation) state->hint_sources.clear();
        }
    }
    std::lock_guard lock(state->mutex);
    if (!state->folder_hint.empty()) return state->folder_hint;
    for (const auto& source : sources)
        if (ui::LooksLikeFolderShortcut(source)) return source;
    // Pending metadata is a navigation hint; the final drop validates it again.
    return state->hint_ready ? std::wstring() : sources.front();
}

} // namespace pulse
