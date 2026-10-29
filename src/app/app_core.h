#pragma once

#include "single_instance_coordinator.h"
#include "window_registry.h"
#include "session.h"
#include "app_prefs.h"
#include "places.h"
#include "context_menu_prefs.h"
#include "search_history.h"
#include "saved_search.h"
#include "network_recent_store.h"
#include "../ui/render_caches.h"
#include <memory>
#include <vector>

namespace pulse {

inline constexpr size_t kMaxTopLevelWindows = 32;

struct AppState;

// State that must outlive and be visible to every top-level window in one
// Pulse process. Window-local services remain in AppState until their worker
// ownership and callback lifetimes can be safely shared.
struct AppCore {
    HINSTANCE instance = nullptr;
    app::SingleInstanceCoordinator single_instance;
    app::BasicWindowRegistry<AppState> windows;
    ui::RenderCaches render_caches;
    // Process-wide user data: every window renders and edits the same
    // preferences, pinned/recent places, context-menu preferences, and tray.
    app::AppPrefs app_prefs;
    app::PlacesCatalog places;
    app::ContextMenuPrefs context_menu_prefs;
    app::StagingTray tray;
    app::SearchHistory search_history;
    app::SearchHistoryWriter search_history_writer;
    app::SavedSearchStore saved_searches;
    std::shared_ptr<app::NetworkRecentStore> network_recent;
    bool shared_preferences_loaded = false;
    bool exiting = false;
    // Global UI services (tray, global hotkey, shell registry watch) follow this
    // HWND as windows are closed, regardless of which window was created first.
    HWND global_ui_owner = nullptr;
    // Keep destroyed AppState objects alive until the outer message dispatch
    // unwinds; WM_NCDESTROY can run reentrantly from modal UI callbacks.
    std::vector<std::shared_ptr<AppState>> owned_windows;
    std::vector<std::shared_ptr<AppState>> retired_windows;
};

struct AppCoreOwner {
    explicit AppCoreOwner(std::unique_ptr<AppCore> core = nullptr) noexcept
        : owned_core(std::move(core)) {}
    std::unique_ptr<AppCore> owned_core;
};

} // namespace pulse
