// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

// app_runtime.h — Shared layout accessors and window-model helpers.
#pragma once
#include "app_state.h"
#include "external_open.h"
#include "tab_transfer.h"

namespace pulse {

bool PumpClipboardAvailability(AppState& s, ULONGLONG now);
bool CanPasteIntoCurrent(AppState& s, bool refresh = true);

inline app::LayoutTab& LiveLayout(AppState& s) {
    s.window_tabs.EnsureDefault();
    return *s.window_tabs.Active();
}

inline const app::LayoutTab* LiveLayout(const AppState& s) {
    return s.window_tabs.Active();
}

inline std::vector<std::unique_ptr<app::Pane>>& Panes(AppState& s) {
    return LiveLayout(s).panes;
}

inline std::unique_ptr<app::SplitContainer>& Root(AppState& s) {
    return LiveLayout(s).root;
}

inline const std::unique_ptr<app::SplitContainer>& Root(const AppState& s) {
    static const std::unique_ptr<app::SplitContainer> empty;
    const app::LayoutTab* tab = LiveLayout(s);
    return tab ? tab->root : empty;
}

inline app::LayoutPreset& LayoutOf(AppState& s) {
    return LiveLayout(s).layout;
}

template<typename Fn>
inline void ForEachPane(AppState& s, Fn&& fn) {
    for (auto& owned : s.window_tabs.items) {
        if (!owned) continue;
        for (auto& pane : owned->panes) {
            if (pane) fn(*pane);
        }
    }
}

inline app::Tab* ActiveTab(AppState& s) {
    return s.pane ? s.pane->ActiveTab() : nullptr;
}

bool ApplyDetailsMetaResult(AppState& s, DetailsMetaResult& result);
bool ShouldRequestDetailsMeta(const AppState& s, const app::Tab& tab, const std::wstring& path);
void RememberLayoutFocus(AppState& s);
std::vector<std::wstring> VisibleFolderPaths(const AppState& s);
void SyncVisibleWatches(AppState& s);
void BindCurrentLayout(AppState& s);
// Consumes already-resolved metadata; selection performs no filesystem queries.
void SelectLaunchedFile(AppState& s, const ResolvedOpenTarget& target);
void RestoreAppWindow(AppState& s);
void OpenFolderInNewTab(AppState& s, const std::wstring& raw);
void OpenFolderInNewTab(AppState& s, const ResolvedOpenTarget& target);
bool RouteExternalFolderOpen(AppState& s, const std::wstring& raw,
    uint64_t takeover_token = 0, std::vector<std::wstring> selected_paths = {},
    unsigned selection_flags = 0);
void ApplyExternalOpenResults(AppState& s, WPARAM identity);
AppState* PostOpenFolderInNewWindow(AppState& s, const std::wstring& raw);
AppState* PostOpenFolderInNewWindow(AppState& s, const ResolvedOpenTarget& target);
void PostWorkerResult(AppState& s, app::WorkResult res);
D2D1_RECT_F FocusedPaneRect(const AppState& s);
app::Pane* PaneAtSlot(AppState& s, int index);
D2D1_RECT_F ListRect(const AppState& s);
bool ScrollbarGeometry(const AppState& s, const ui::PaneViewModel& pane,
                              D2D1_RECT_F& track, D2D1_RECT_F& thumb, float& maxScroll);
bool HorizontalScrollbarGeometry(const AppState& s, const ui::PaneViewModel& pane,
                                         D2D1_RECT_F& track, D2D1_RECT_F& thumb,
                                         float& maxScroll);
void RememberPath(AppState& s, const std::wstring& path);
void RecordRecentOpen(AppState& s, const std::wstring& path,
                             app::PlaceItemKind kind);
void FillPaneSlots(AppState& s, ui::WindowViewModel& vm);
int TrayDeckCap(const AppState& s);
std::vector<TrayDeckEntry> TrayDeckEntries(const app::StagingTray& tray, size_t offset,
                                                  size_t cap);
int TrayItemTotalCount(const app::StagingTray& tray);
std::wstring TrayDisplayPath(const std::wstring& path);
std::wstring TrayItemName(const std::wstring& path);
int TrayDeckHoverIndex(const AppState& s);
// Staging tray card stack (cyclic): index of the top card, throw the top card
// to the back (dir ±1, release offset in DIPs), bring the last card back on
// top, smoke burst for a dismiss, and exit hints for the next tick.
int TrayStackTop(const AppState& s);
void ThrowTrayTop(AppState& s, float dir, float dx, float dy);
void TrayStepBack(AppState& s);
void ReleaseTrayDrag(AppState& s, bool commit);
void SpawnTrayPuffs(AppState& s);
void MarkTrayExit(AppState& s, const std::vector<std::wstring>& paths, bool clearing);
void RefreshStarredViews(AppState& s);
void RefreshRecentViews(AppState& s);
bool IsRecycleTab(const app::Tab* tab);
std::wstring RecycleOccupancyText(const fs::RecycleBinInfo& info);
void ApplyRecycleOccupancy(AppState& s);
void RequestRecycleOccupancy(AppState& s);
bool ApplyQueriedRecycleInfo(AppState& s, const fs::RecycleBinInfo& info);
void RefreshRecycleViews(AppState& s, bool query_occupancy = true);
void ScheduleRecycleRefresh(AppState& s);
bool PumpRecycleRefresh(AppState& s, ULONGLONG now);
void BumpRecycleOccupancy(AppState& s, int64_t delta);
void ClearRecycleOccupancy(AppState& s);
bool ToggleStarred(AppState& s, const std::wstring& target,
                          app::PlaceItemKind kind = app::PlaceItemKind::Unknown);
std::wstring DetailsAttributeText(DWORD attrs);
double TrayAnimationNow();
bool TickTrayDeck(AppState& s, double now = TrayAnimationNow());
// Apply model preferences, synchronize the title/columns and advance details
// requests before drawing. This also covers programmatic selection and snapshots.
void PrepareViewState(AppState& s);
// Reads prepared business state; false skips the more expensive status text.
// Presentation caches and visible-item registration may still be populated.
ui::WindowViewModel BuildVm(AppState& s, bool include_status = true);
// Displayed layout for pointer hit tests. Rebuilt on demand after a resize or
// before the first frame; never used for state-changing clicks.
const ui::WindowViewModel& HitTestVm(AppState& s);
std::wstring TooltipForHover(AppState& s);
// Records what the pointer is over. Both mouse-move paths (client and frame)
// must call this, or a hover field silently stops updating.
void ApplyHoverTarget(AppState& s, const ui::HitTestResult& hit);
std::wstring EntryFullPath(const app::Tab& tab, int index);
std::vector<std::wstring> SelectedFullPaths(const app::Tab& tab);
std::wstring TagDiscoveryKey(std::wstring path);
void QueueVisibleTagDiscovery(AppState& s);
std::wstring SelectedFullPath(AppState& s);
void SyncSavedSearchSidebar(AppState& s);
} // namespace pulse
