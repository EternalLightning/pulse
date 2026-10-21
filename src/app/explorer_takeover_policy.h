#pragma once

#include <cstdint>

namespace pulse::app::explorer_takeover {

inline constexpr uint32_t kIdlePollMs = 1000;
inline constexpr uint32_t kCandidatePollMs = 80;
inline constexpr uint32_t kStabilityMs = 450;
inline constexpr uint32_t kInitialSelectionGraceMs = 800;
inline constexpr uint32_t kLateSelectionGraceMs = 1500;
inline constexpr uint32_t kCandidateTimeoutMs = 15000;

struct TabObservation {
    bool tab_structure_required = true;
    bool enumeration_complete = false;
    uint32_t shell_view_count = 0;
    uint32_t native_tab_count = 0;
};

inline bool IsSingleTabSafe(const TabObservation& source) {
    return source.enumeration_complete && source.shell_view_count == 1 &&
        source.native_tab_count <= 1 &&
        (!source.tab_structure_required || source.native_tab_count == 1);
}

struct SourceObservation {
    bool same_identity = false;
    bool single_tab = false;
    bool supported = false;
    bool same_folder = false;
    bool same_selection = false;
    bool visible = true;
    bool input_unchanged = false;
    bool shift = false;
    bool submitted_empty_selection = false;
    bool current_selection_nonempty = false;
    uint64_t view_age_ms = 0;
};

enum class PendingDecision { Wait, Retire, Replace };

inline PendingDecision DecidePending(const SourceObservation& source) {
    if (!source.same_identity || !source.single_tab || !source.supported ||
        !source.same_folder || !source.input_unchanged || source.shift)
        return PendingDecision::Retire;
    if (source.same_selection) return PendingDecision::Wait;
    // A visible source may have been used through accessibility or another
    // input path. Only an unexposed, short empty-to-selection update is retried.
    return !source.visible && source.submitted_empty_selection &&
        source.current_selection_nonempty && source.view_age_ms <= kLateSelectionGraceMs
        ? PendingDecision::Replace : PendingDecision::Retire;
}

inline bool CanCloseAfterAcknowledgment(const SourceObservation& source,
                                       bool delivered, bool stopped) {
    return delivered && !stopped && source.same_selection &&
        DecidePending(source) == PendingDecision::Wait;
}

inline bool CanSubmit(uint64_t stable_ms, uint64_t view_age_ms, bool has_selection) {
    return stable_ms >= kStabilityMs &&
        (has_selection || view_age_ms >= kInitialSelectionGraceMs);
}

inline uint32_t NextScanDelay(bool has_candidates) {
    return has_candidates ? kCandidatePollMs : kIdlePollMs;
}

} // namespace pulse::app::explorer_takeover
