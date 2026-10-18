#include "archive_navigation.h"
#include "app_exit.h"
#include "../ops/operation_journal_paths.h"
#include "drive_label_rename.h"
#include "../ops/archive.h"
#include "../ops/drop_staging.h"
#include "locked_operation_prompt.h"
#include "permission_operation_prompt.h"
#include "../common/windows_compat.h"
#include "quick_access.h"
#include "pane_animations.h"
#include "app_visual_animation.h"
#include "app_ui_schedule.h"
#include "sidebar_resize.h"
// app_main.cpp — Pulse UI process entry point, window, input, shot mode.
#include "../ui/ui_compositor.h"
#include "../ui/lumatext_renderer.h"
#include "../ui/ui_renderer.h"
#include "../ui/preview_grab_cursor.h"
#include "../ui/fluent_menu.h"
#include "../ui/drag_drop.h"
#include "../ui/file_operation_dialog.h"
#include "../ui/batch_rename_dialog.h"
#include "../ui/quick_preview_window.h"
#include "../ui/typography.h"
#include "../common/crash_reporter.h"
#include "../common/diagnostics_exporter.h"
#include "../common/localization.h"
#include "../common/user_storage.h"
#include "pulse_version.h"
#include "../fs/fs_enum.h"
#include "../fs/fs_recycle.h"
#include "../fs/fs_snapshot.h"
#include "../fs/fs_watch.h"
#include "../fs/fs_net_cache.h"
#include "app_model.h"
#include "app_worker.h"
#include "snapshot_patch.h"
#include "session.h"
#include "context_menu.h"
#include "context_menu_controller.h"
#include "shell_verbs.h"
#include "places.h"
#include "batch_rename.h"
#include "details_meta.h"
#include "context_menu_prefs.h"
#include "app_prefs.h"
#include "entry_sort.h"
#include "saved_search.h"
#include "search_query.h"
#include "settings_controller.h"
#include "single_instance_coordinator.h"
#include "tray_controller.h"
#include "global_search_controller.h"
#include "tab_controller.h"
#include "link_resolve.h"
#include "../ui/color_picker.h"
#ifdef PULSE_WITH_SELFTEST
#include "selftest_1b2.h"
#endif
#include "resource.h"
#include "../index/index_client.h"
#include "../index/network_agent_client.h"
#include "../index/content_search_client.h"
#include "../ops/ops_manager.h"
#include "../ops/clipboard.h"
#include "../ipc/ctx_menu_util.h"
#include "../common/text_format.h"
#include "../common/path_utils.h"
#include <windows.h>
#include <windowsx.h>
#include <uxtheme.h>
#include <prsht.h>
#include <psapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shlwapi.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cwctype>
#include <cmath>
#include <cstdio>
#include <exception>
#include <process.h>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include "../common/known_folder_labels.h"
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "uuid.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "uxtheme.lib")
#pragma comment(lib, "psapi.lib")
#include "app_internal.h"
#include "duplicate_scan.h"
#include <commctrl.h>
#include <dbt.h> // WM_DEVICECHANGE / DEV_BROADCAST_HDR

using namespace pulse;

uint64_t FileTimeValue(const FILETIME& value) {
    ULARGE_INTEGER result{};
    result.LowPart = value.dwLowDateTime;
    result.HighPart = value.dwHighDateTime;
    return result.QuadPart;
}

void UpdateProcessMetrics(AppState& s) {
    const ULONGLONG now = GetTickCount64();
    if (s.processSampleTick != 0 && now - s.processSampleTick < 500) return;

    FILETIME created{}, exited{}, kernel{}, user{};
    if (GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) {
        const uint64_t processTime = FileTimeValue(kernel) + FileTimeValue(user);
        if (s.processSampleTick != 0 && processTime >= s.lastProcessTime100ns) {
            const double wall100ns = static_cast<double>(now - s.processSampleTick) * 10000.0;
            const DWORD processors = std::max<DWORD>(1, GetActiveProcessorCount(ALL_PROCESSOR_GROUPS));
            s.processCpuPercent = 100.0 * static_cast<double>(processTime - s.lastProcessTime100ns)
                / (wall100ns * processors);
        }
        s.lastProcessTime100ns = processTime;
    }

    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    if (GetProcessMemoryInfo(GetCurrentProcess(),
            reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters))) {
        s.workingSetMb = static_cast<double>(counters.WorkingSetSize) / (1024.0 * 1024.0);
    }
    s.processSampleTick = now;
}

// Index status only reaches the screen through search views, the palette and
// the settings page (see BuildVm); other notifications need no repaint.
static bool IndexStatusVisible(AppState& s) {
    if (s.paletteSearching) return true;
    bool visible = false;
    ForEachPane(s, [&](app::Pane& pane) {
        const app::Tab* tab = pane.ActiveTab();
        std::wstring kind;
        if (tab && app::ParsePulsePath(tab->current_path, &kind, nullptr) &&
            (kind == L"search" || kind == L"saved-search" || kind == L"settings"))
            visible = true;
    });
    return visible;
}

bool Render(AppState& s) {
    auto t0 = std::chrono::steady_clock::now();

    if (s.compositor.NeedsRecovery()) {
        if (!s.compositor.Recover()) return false;
        s.compositor.RecreateTextFormats(s.scale);
        s.renderer.SetCompositor(&s.compositor);
        s.renderer.SetScale(s.scale);
    }

    if (s.hwnd) {
        RECT rc;
        GetClientRect(s.hwnd, &rc);
        int w = rc.right;
        int h = rc.bottom;
        if (w != s.compositor.Width() || h != s.compositor.Height()) {
            s.compositor.Resize(w, h);
        }
    }

    ID2D1DeviceContext* dc = s.compositor.Dc();
    const auto draw_start = std::chrono::steady_clock::now();
    dc->BeginDraw();
    dc->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));

    bool hc = s.shot_high_contrast || ui::IsHighContrast();
    ui::Theme theme = hc ? ui::MakeHighContrastTheme() : ui::MakeTheme(s.darkMode, s.accentColor);

    if (s.scrollAnimating) UpdateSmoothScroll(s);
    const bool tray_animating = TickTrayDeck(s);
    bool pane_animating = false;
    const uint64_t animation_now = app::PaneAnimationTimeMs();
    for (auto& pane : Panes(s)) {
        const bool focused = pane.get() == s.pane;
        pane_animating |= app::TickPaneAnimations(*pane, focused,
            s.filterEditing && focused, animation_now);
    }
    const bool visual_animating = TickVisualAnimations(s);
    const bool address_animating = TickAddressSearchAnimation(s, animation_now);
    if (s.showFps) UpdateProcessMetrics(s);
    ui::WindowViewModel vm = BuildVm(s);
    vm.backdrop_enabled = !hc && s.compositor.UsesTransparentComposition() && s.backdropActive;
    vm.pane.hover_index = s.hoverRow;

    D2D1_RECT_F rect = D2D1::RectF(0, 0, (float)s.compositor.Width(), (float)s.compositor.Height());
    s.renderer.Render(vm, rect, theme);
    s.hitTestVm = std::move(vm);
    s.hitTestVmWidth = s.compositor.Width();
    s.hitTestVmHeight = s.compositor.Height();
    s.hitTestVmValid = true;
    s.notification_toast.Draw(s.compositor, theme, s.scale, hc);

    const HRESULT end_hr = dc->EndDraw();
    const auto draw_end = std::chrono::steady_clock::now();
    s.timing.draw_ms = std::chrono::duration<double, std::milli>(draw_end - draw_start).count();
    if (end_hr == D2DERR_RECREATE_TARGET || end_hr == DXGI_ERROR_DEVICE_REMOVED ||
        end_hr == DXGI_ERROR_DEVICE_RESET || end_hr == DXGI_ERROR_DRIVER_INTERNAL_ERROR) {
        s.compositor.NotifyDeviceLost(end_hr);
        return false;
    }
    const auto present_start = std::chrono::steady_clock::now();
    wchar_t hidden_frame[4]{};
    const bool hidden_capture = s.shot.active &&
        GetEnvironmentVariableW(L"PULSE_TEST_HIDDEN_SHOT", hidden_frame, ARRAYSIZE(hidden_frame)) == 1 &&
        hidden_frame[0] == L'1';
    // An occluded flip chain advances to an unpainted buffer after Present.
    if (!hidden_capture) s.compositor.Present();
    const auto present_end = std::chrono::steady_clock::now();
    s.timing.present_ms = std::chrono::duration<double, std::milli>(present_end - present_start).count();

    if (s.renameIndex >= 0) LayoutRenameOverlay(s);
    if (!s.tagRenameId.empty()) LayoutTagRenameOverlay(s);
    if (s.addressEditing) LayoutAddressEditor(s);
    if (s.filterEditing) LayoutFilterEditor(s);
    CompleteFilterFocus(s);

    if (s.shot.active && !s.timing.first_frame_recorded) {
        s.timing.first_frame_ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - s.shot.start).count();
        s.timing.first_frame_recorded = true;
    }

    auto t1 = std::chrono::steady_clock::now();
    s.lastFrameMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
    // FPS = frames presented per second over a sliding ~1 s window of
    // continuous rendering. Pulse paints on demand: 1/dt of two adjacent
    // paints reported the idle gap as "1 FPS" and back-to-back paints as
    // thousands. An idle gap restarts the window instead of being averaged in,
    // and the value only updates once the window spans enough frames.
    if (s.showFps) {
        using secs = std::chrono::duration<double>;
        constexpr double kIdleGap = 0.25, kWindow = 1.0, kMinSpan = 0.05;
        if (!s.fpsWindow.empty() && secs(t1 - s.fpsWindow.back()).count() > kIdleGap)
            s.fpsWindow.clear();
        s.fpsWindow.push_back(t1);
        while (s.fpsWindow.size() > 2 && secs(t1 - s.fpsWindow.front()).count() > kWindow)
            s.fpsWindow.pop_front();
        const double span = secs(t1 - s.fpsWindow.front()).count();
        if (s.fpsWindow.size() >= 3 && span >= kMinSpan)
            s.lastFps = static_cast<double>(s.fpsWindow.size() - 1) / span;
    } else {
        s.fpsWindow.clear();
        s.lastFps = 0;
    }
    s.ui_schedule.animation_active = tray_animating || pane_animating || visual_animating ||
        address_animating || s.scrollAnimating;
    return s.ui_schedule.animation_active;
}

static void RestoreWindowPreferences(AppState& state, const app::WindowSessionSnapshot& session) {
    state.darkMode = session.dark;
    state.sidebarCollapsedMask = static_cast<uint32_t>(session.sidebar_collapsed);
    state.sidebarHiddenMask = static_cast<uint32_t>(session.sidebar_hidden);
    state.sidebarOrder = session.sidebar_order.empty() ? app::DefaultSidebarOrder() : session.sidebar_order;
    state.sidebarQuickAccessHiddenMask = static_cast<uint32_t>(session.quick_access_hidden);
    state.starredExpanded = session.starred_expanded;
    state.pending_undo_json = session.undo_json;
    state.showDetailsPanel = session.details_panel;
    state.detailsPreviewOnly = session.details_preview_only;
    state.detailsPreviewEnabled = session.details_preview;
    state.detailsPreviewExpansion = session.details_preview_only ? 1.0f : 0.0f;
    state.detailsPanelWidth = static_cast<float>(session.details_panel_width);
    state.renderer.SetDetailsPanelWidth(state.detailsPanelWidth);
    if (state.darkMode) state.themeOverride = ui::ThemeMode::Dark;
}

static app::WindowSessionSnapshot CaptureWindowPreferences(AppState& s, HWND hwnd) {
    app::WindowSessionSnapshot snap;
    WINDOWPLACEMENT wp{ sizeof(wp) };
    if (GetWindowPlacement(hwnd, &wp)) {
        snap.window_rect = wp.rcNormalPosition;
        snap.maximized = (wp.showCmd == SW_SHOWMAXIMIZED);
    }
    snap.dark = s.darkMode;
    snap.tray = s.tray;
    snap.undo_json = s.ops.UndoToJson();
    snap.sidebar_collapsed = static_cast<int>(s.sidebarCollapsedMask);
    snap.sidebar_hidden = static_cast<int>(s.sidebarHiddenMask);
    snap.sidebar_order = app::NormalizeSidebarOrder(s.sidebarOrder);
    snap.quick_access_hidden = static_cast<int>(s.sidebarQuickAccessHiddenMask);
    snap.starred_expanded = s.starredExpanded;
    snap.details_panel = s.showDetailsPanel;
    snap.details_preview_only = s.detailsPreviewOnly;
    snap.details_preview = s.detailsPreviewEnabled;
    snap.details_panel_width = static_cast<int>(std::lround(s.detailsPanelWidth));
    return snap;
}

LRESULT CALLBACK WndProcImpl(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    AppState* s = GetAppState(hwnd);
    if (s && msg == app::TrayController::TaskbarCreatedMessage()) {
        if (!s->exit_requested) s->tray_controller.HandleTaskbarCreated();
        return 0;
    }
    if (s && s->notification_toast.HandleMessage(hwnd, msg, wParam, lParam)) return 0;
    if (s && s->exit_requested) {
        switch (msg) {
        case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
        case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_KEYDOWN:
        case WM_SYSKEYDOWN: case WM_CHAR: case WM_COMMAND: case WM_HOTKEY:
        case WM_CONTEXTMENU: case WM_QUICK_PREVIEW_COMMAND:
            return 0;
        case WM_COPYDATA: return FALSE;
        default: break;
        }
    }

    switch (msg) {
    case WM_NCPAINT:
        // The renderer owns the entire frame, including all caption buttons.
        return 0;

    case WM_NCACTIVATE:
        // Preserve activation semantics without repainting a native caption.
        return DefWindowProcW(hwnd, msg, wParam, -1);

    case WM_NCCALCSIZE: {
        if (wParam && IsZoomed(hwnd)) {
            auto* params = reinterpret_cast<NCCALCSIZE_PARAMS*>(lParam);
            MONITORINFO monitor{sizeof(monitor)};
            if (GetMonitorInfoW(MonitorFromRect(&params->rgrc[0],
                    MONITOR_DEFAULTTONEAREST), &monitor)) {
                // Our custom frame has no invisible maximized border to inset.
                // Use the destination monitor's work area, including taskbar offsets.
                params->rgrc[0] = monitor.rcWork;
            }
        }
        return 0;
    }

    case WM_CREATE: {
        auto cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        s = reinterpret_cast<AppState*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(s));
        s->hwnd = hwnd;
        if (!s->core.instance) s->core.instance = cs->hInstance;
        s->core.windows.Add(hwnd, s);
        if (!s->core.global_ui_owner) s->core.global_ui_owner = hwnd;
        s->shell_verbs_mailbox = std::make_shared<app::WindowResultMailbox<ShellVerbsResult>>(hwnd, WM_SHELL_VERBS);
        s->shell_registry_mailbox = std::make_shared<app::WindowResultMailbox<bool>>(hwnd, WM_SHELL_CACHE_INVALIDATE);
        s->clipboard_paste_mailbox = std::make_shared<app::WindowResultMailbox<bool>>(hwnd, WM_CLIPBOARD_PASTE_RESULT);
        s->external_open_mailbox = std::make_shared<app::WindowResultMailbox<app::ExternalOpenResult>>(
            hwnd, WM_EXTERNAL_OPEN_READY);
        s->dup_volumes_mailbox = std::make_shared<app::WindowResultMailbox<std::vector<index::VolumeInfo>>>(hwnd, WM_DUP_VOLUMES);
        s->dup_volume_cache_pending = false;
        s->tray_controller.SetBeforeRestore([s] {
            s->tray_controller.SetVisible(s->appPrefs.keep_running_on_close || s->appPrefs.global_search_enabled);
            s->compositor.QueueShowTransition();
        });
        if (s->core.global_ui_owner == hwnd) s->tray_controller.Attach(hwnd, cs->hInstance);

        s->scale = s->shot_scale_override > 0.0f
            ? s->shot_scale_override : (float)pulse::compat::WindowDpi(hwnd) / 96.0f;
        s->accentColor = ui::GetAccentColor();
        s->darkMode = ui::ShouldUseDarkMode(s->EffectiveThemeMode());
        s->backdropActive = ui::ApplyWindowEffect(hwnd, ui::WindowEffect::MicaAlt, s->darkMode);

        if (!s->compositor.Init(hwnd)) {
            const std::wstring message = L"Failed to initialize D3D/D2D/DWrite\n\n" +
                s->compositor.InitializationError() + L"\n\nLog: %LOCALAPPDATA%\\Pulse\\pulse_graphics.log";
            MessageBoxW(hwnd, message.c_str(), L"Pulse", MB_OK | MB_ICONERROR);
            return -1;
        }
        s->compositor.RecreateTextFormats(s->scale);
        if (s->shot.active) {
            wchar_t toast_test[2]{};
            if (GetEnvironmentVariableW(L"PULSE_TEST_TOAST", toast_test, 2) == 1 && toast_test[0] == L'1')
                s->notification_toast.Show(hwnd,
                    l10n::Get(l10n::StringId::IndexMigrationIncomplete),
                    l10n::Get(l10n::StringId::IndexMigrationNoSpace));
        }
        s->renderer.SetCompositor(&s->compositor);
        s->renderer.SetScale(s->scale);
        s->renderer.SetIconNotifyWindow(hwnd);
        s->quickPreview.Initialize(hwnd, WM_QUICK_PREVIEW_NAVIGATE,
                                   WM_QUICK_PREVIEW_OPEN, WM_QUICK_PREVIEW_COMMAND,
                                   s->renderer.SharedThumbnailCache());

        s->window_tabs.EnsureDefault();
        BindCurrentLayout(*s);
        s->tabs.SetCallbacks({
            [s](app::Tab& tab) { StartLoadingPath(*s, tab, tab.current_path); },
            [hwnd] { InvalidateRect(hwnd, nullptr, FALSE); },
            [s] { BindCurrentLayout(*s); },
            [s] { RememberLayoutFocus(*s); },
            [s] { return NewTabPath(*s); },
            [s] { return s->appPrefs.close_last_tab_window; },
            [hwnd] { PostMessageW(hwnd, WM_CLOSE, 0, 0); },
        });
        if (s->isolatedTest) {
            s->places.persist = false;
            s->appPrefs.persist = false;
        }
        if (!s->core.shared_preferences_loaded) {
            s->savedSearches.Load();
            s->places.Load();
            ProbePinnedNetworks(*s);
            s->ctxMenuPrefs.Load();
            s->appPrefs.Load();
            s->searchHistory.persist = !s->shot.active && !s->menushot && !s->isolatedTest;
            if (s->searchHistory.persist) s->searchHistory.Load();
            s->core.shared_preferences_loaded = true;
        }
        SyncSavedSearchSidebar(*s);
        s->darkMode = ui::ShouldUseDarkMode(s->EffectiveThemeMode());
        s->showFps = s->forceStatusPerformance || s->appPrefs.show_status_performance;
        s->duplicateScan.scope = static_cast<app::DuplicateScanScope>(
            std::clamp(s->appPrefs.duplicate_scan_scope, 0, 2));
        s->duplicateScan.folder_path = s->appPrefs.duplicate_scan_folder;
        s->duplicateScan.drive_root =
            app::DuplicateScanSession::NormalizeDriveRoot(s->appPrefs.duplicate_scan_drive);
        s->duplicateScan.minimum_file_bytes =
            app::DuplicateScanSession::DefaultMinimumBytes(s->duplicateScan.scope);
        if (s->shot.active && l10n::IsLanguageId(s->shot.language) &&
            s->shot.language != L"system")
            s->appPrefs.language = s->shot.language;
        l10n::Initialize(cs->hInstance, s->appPrefs.language);
        RefreshSidebarModel(*s);
        ui::typography::InvalidateCaches();
        s->compositor.RecreateTextFormats(s->scale);
        if (s->safeMode) {
            s->appPrefs.window_effect = L"none";
            s->appPrefs.background_image.clear();
        } else {
            SeedShellVerbCache(*s);
            if (s->core.global_ui_owner == hwnd) StartShellRegistryWatch(*s);
        }
        s->renderer.SetRowHeightDip(static_cast<float>(s->appPrefs.row_height));
        s->renderer.SetListStyle(s->appPrefs.list_smart_date, s->appPrefs.list_zebra_rows,
                                 s->appPrefs.list_size_bar);
        s->renderer.SetSidebarWidthDip(static_cast<float>(s->appPrefs.sidebar_width));
        s->renderer.SetTrayIconDip(static_cast<float>(s->appPrefs.tray_icon_size));
        ApplyAccentFromPrefs(*s);
        ApplyAppWindowChrome(*s);
        s->index.Start(hwnd, WM_INDEX_NOTIFY, WM_INDEX_SEARCH);
        StartChangeTracking(*s);
        s->networkIndex.Start(hwnd, WM_NETWORK_INDEX_NOTIFY, WM_NETWORK_INDEX_SEARCH);
        s->contentSearch.Start(hwnd, WM_CONTENT_SEARCH, s->contentIndexObserver ? index::ContentAgentMode::Observer : index::ContentAgentMode::Instant);
        s->duplicateSearch.Start(hwnd, WM_DUPLICATE_SCAN, false);
        s->settings.SetServiceInstalled(s->index.ServiceInstalled());
        app::SettingsController::UiCallbacks settings_callbacks;
        settings_callbacks.pick_image = [hwnd](std::wstring& path) {
            return PickImageFile(hwnd, path);
        };
        settings_callbacks.pick_folder = [hwnd](std::wstring& path, std::wstring_view title) {
            const std::wstring owned_title(title);
            return PickFolder(hwnd, path, owned_title.c_str());
        };
        settings_callbacks.apply_effects = [s](app::SettingsEffect effects) {
            ApplySettingsEffects(*s, effects);
        };
        settings_callbacks.toggle_system_integration = [s](int control) {
            AppState* owner = s->core.windows.Lookup(s->core.global_ui_owner);
            if (!owner) owner = s;
            owner->systemIntegration.Toggle(*owner, control);
            InvalidateRect(owner->hwnd, nullptr, FALSE);
        };
        settings_callbacks.task_completion = SettingsCompletion(hwnd);
        settings_callbacks.open_path = [s](const std::wstring& path) {
            s->ops.OpenWith(path);
        };
        settings_callbacks.open_diagnostics = [s] {
            const std::wstring path = app::GetPulseDataDir() + L"\\Diagnostics\\Crashes";
            CreateDirectoryW((app::GetPulseDataDir() + L"\\Diagnostics").c_str(), nullptr);
            CreateDirectoryW(path.c_str(), nullptr);
            s->ops.OpenWith(path);
        };
        settings_callbacks.clear_diagnostics = [hwnd](std::wstring& error) {
            if (MessageBoxW(hwnd,
                    l10n::Get(l10n::StringId::DiagnosticsClearConfirm).c_str(),
                    l10n::Get(l10n::StringId::Diagnostics).c_str(),
                    MB_ICONWARNING | MB_OKCANCEL | MB_DEFBUTTON2) != IDOK)
                return true;
            if (diagnostics::ClearCrashReports(app::GetPulseDataDir(), &error)) return true;
            error = l10n::Get(l10n::StringId::DiagnosticsClearFailed);
            return false;
        };
        settings_callbacks.prepare_diagnostics_export =
            [hwnd](std::wstring& destination, bool& include_service) {
            if (MessageBoxW(hwnd,
                    l10n::Get(l10n::StringId::DiagnosticsPrivacyMessage).c_str(),
                    l10n::Get(l10n::StringId::DiagnosticsPrivacyTitle).c_str(),
                    MB_ICONWARNING | MB_OKCANCEL | MB_DEFBUTTON2) != IDOK)
                return false;
            const int service = MessageBoxW(hwnd,
                l10n::Get(l10n::StringId::DiagnosticsIncludeService).c_str(),
                l10n::Get(l10n::StringId::DiagnosticsPrivacyTitle).c_str(),
                MB_ICONQUESTION | MB_YESNOCANCEL | MB_DEFBUTTON2);
            if (service == IDCANCEL) return false;
            include_service = service == IDYES;
            return PickFolder(hwnd, destination,
                l10n::Get(l10n::StringId::DiagnosticsExportLocation).c_str());
        };
        settings_callbacks.export_diagnostics =
            [](const std::wstring& destination, bool include_service,
               std::wstring& error) {
            WIN32_FIND_DATAW data{};
            HANDLE find = FindFirstFileW((destination + L"\\*").c_str(), &data);
            bool empty = true;
            if (find != INVALID_HANDLE_VALUE) {
                do {
                    if (wcscmp(data.cFileName, L".") != 0 &&
                        wcscmp(data.cFileName, L"..") != 0) {
                        empty = false;
                        break;
                    }
                } while (FindNextFileW(find, &data));
                FindClose(find);
            }
            if (!empty) return false;

            diagnostics::ExportOptions options;
            options.source_root = app::GetPulseDataDir();
            options.include_dumps = true;
            options.require_empty_destination = true;
            if (!include_service) {
                options.destination = destination;
                return diagnostics::Export(options, &error);
            }

            const std::wstring service_dir = destination + L"\\IndexService";
            if (!CreateDirectoryW(service_dir.c_str(), nullptr) ||
                !index::IndexClient::ExportDiagnosticsElevated(service_dir))
                return false;
            const std::wstring user_dir = destination + L"\\User";
            if (!CreateDirectoryW(user_dir.c_str(), nullptr)) return false;
            options.destination = user_dir;
            return diagnostics::Export(options, &error);
        };
        s->settings.BindUi(s->appPrefs, s->ctxMenuPrefs, s->index,
                           s->networkIndex, std::move(settings_callbacks));
        ApplyGlobalSearchSettings(*s);

        if (!s->worker.Start([s](app::WorkResult res) { PostWorkerResult(*s, std::move(res)); })) {
            MessageBoxW(hwnd, L"Pulse could not start its directory worker pool.", L"Pulse",
                       MB_OK | MB_ICONERROR);
            return -1;
        }
        if (!s->isolatedTest && !s->shot.active && !s->menushot) {
            s->worker.EnqueueIo([] { ops::SweepDropStages(ops::DropStageRoot(), false); });
        }
        RequestRecycleOccupancy(*s);

        // Ops layer: queue worker + shell host IPC; notify repaints the status bar.
        const std::wstring data_dir = app::GetPulseDataDir();
        if (!s->isolatedTest && !data_dir.empty()) {
            auto journals = ops::AssignOperationJournalPaths(data_dir, !s->secondary_window);
            if (s->secondary_window) s->ops.SetJournalPath(std::move(journals.window_path));
            else s->ops.SetJournalPaths(std::move(journals.paths));
        }
        s->ops.SetVerifyCopies(s->appPrefs.verify_copies);
        // Shell dialogs (打开方式…/属性) must be owned by the Pulse window.
        s->ops.SetUiWindow(hwnd);
        s->ops.Start([hwnd] { PostMessageW(hwnd, WM_OPS_NOTIFY, 0, 0); });
        s->globalSearchWindow.SetOpenCallback([s](const std::wstring& path, bool location) {
            if (location) s->ops.RevealInExplorer(path);
            else s->ops.OpenWith(path);
        });
        const ops::RecoverySnapshot recovery = s->isolatedTest
            ? ops::RecoverySnapshot{} : s->ops.PendingRecovery();
        if (!recovery.entries.empty()) {
            wchar_t recovery_message[512]{};
            swprintf_s(recovery_message, l10n::Get(l10n::StringId::RecoveryPromptFormat).c_str(),
                recovery.entries.size());
            std::wstring prompt = recovery_message;
            if (recovery.has_uncertain_destructive)
                prompt += l10n::Get(l10n::StringId::RecoveryDestructiveWarning);
            if (MessageBoxW(hwnd, prompt.c_str(), l10n::Get(l10n::StringId::RecoveryTitle).c_str(),
                            MB_ICONWARNING | MB_YESNO | MB_DEFBUTTON2) == IDYES)
                s->ops.RetryRecovery();
            else
                s->ops.DiscardRecovery();
        }
        s->context_menu.SetShellOperations({
            [s](std::vector<std::wstring> paths, HWND owner, bool background, bool extended,
                std::vector<std::wstring> disabled) {
                if (s->safeMode) return uint32_t{0};
                return s->ops.QueryShellMenu(std::move(paths), owner, background, extended,
                                             std::move(disabled));
            },
            [s](uint32_t token) { s->ops.CloseShellMenu(token); },
            [s](uint32_t token, uint32_t command, std::wstring verb, std::wstring text) {
                s->ops.InvokeShellMenu(token, command, std::move(verb), std::move(text));
            },
            [s](const std::wstring& path, const std::wstring& verb) {
                s->ops.ExecuteVerb(path, verb);
            },
            [s](const std::wstring& app_path, const std::wstring& path) {
                s->ops.OpenWithApp(app_path, path);
            },
            [s](const std::wstring& command, const std::wstring& path) {
                s->ops.ExecuteCommand(command, path);
            },
        });
        // Explorer verbs arrive on the shell client's reader thread; hop to
        // the UI thread with an owned payload (freed by the WM handler).
        s->ops.SetShellMenuCallback([hwnd](uint32_t token,
                                           std::vector<ops::ShellMenuItem> items,
                                           bool partial,
                                           std::vector<std::wstring> slow_clsids) {
            auto* payload = new ShellCtxItemsPayload{
                std::move(items), partial, std::move(slow_clsids) };
            if (!PostMessageW(hwnd, WM_SHELLCTX_ITEMS, token,
                              reinterpret_cast<LPARAM>(payload)))
                delete payload;
        });
        if (!s->pending_undo_json.empty()) s->ops.UndoFromJson(s->pending_undo_json);
        s->operationWindow = std::make_unique<ui::FileOperationWindow>();
        ui::FileOperationCallbacks operation_callbacks;
        operation_callbacks.cancel = [hwnd] {
            if (AppState* state = GetAppState(hwnd)) state->ops.CancelCurrent();
        };
        operation_callbacks.pause = [hwnd] {
            if (AppState* state = GetAppState(hwnd)) state->ops.PauseCurrent();
        };
        operation_callbacks.resume = [hwnd] {
            if (AppState* state = GetAppState(hwnd)) state->ops.ResumeCurrent();
        };
        operation_callbacks.dismiss = [hwnd] {
            if (AppState* state = GetAppState(hwnd)) {
                state->operationDismissedTaskId = state->ops.Status().task_id;
                state->operationPinnedByUser = false;
            }
        };
        s->operationWindow->Create(hwnd, std::move(operation_callbacks));
        s->operationWindow->SetTheme(s->darkMode, s->accentColor);

        // OLE drop target: list rows, pane headers, breadcrumbs, sidebar, tray.
        {
            ui::DropTargetCallbacks dcb;
            dcb.drag_over = [hwnd](const std::vector<std::wstring>& srcs, POINT pt,
                                   DWORD keys, DWORD allowed, DWORD preferred) -> DWORD {
                AppState* st = GetAppState(hwnd);
                return st ? ResolveDropTarget(*st, srcs, pt, keys, allowed, preferred) : DROPEFFECT_NONE;
            };
            dcb.drag_leave = [hwnd] {
                if (AppState* st = GetAppState(hwnd)) ClearDropFeedback(*st);
            };
            dcb.drop = [hwnd](const std::vector<std::wstring>& srcs, POINT pt,
                              DWORD keys, DWORD allowed, DWORD preferred) -> DWORD {
                AppState* st = GetAppState(hwnd);
                return st ? DropExecute(*st, srcs, pt, keys, allowed, preferred) : DROPEFFECT_NONE;
            };
            s->dropTarget = new ui::WindowDropTarget(hwnd, std::move(dcb));
            RegisterDragDrop(hwnd, s->dropTarget);
        }

        std::wstring startPath = s->shot.active ? s->shot.path : app::MakeHomePath();
        ResolvedOpenTarget openTarget;
        const bool has_initial_external_target = s->initial_external_open_target.has_value();
        if (has_initial_external_target) {
            openTarget = std::move(*s->initial_external_open_target);
            s->initial_external_open_target.reset();
            startPath = openTarget.folder;
        } else if (!s->shot.active && !s->open_path.empty()) {
            if (ops::IsArchivePath(s->open_path)) startPath = ArchiveViewPath(fs::NormalizePath(s->open_path));
            else {
                openTarget = ResolveOpenTarget(s->open_path);
                startPath = openTarget.folder;
            }
        }
        s->pane->NewTab(startPath);
        if (s->shot.active) {
            s->pane->ActiveTab()->view_mode = s->shot.view_mode;
            if (s->shot.sort_date) {
                s->pane->ActiveTab()->sort_column = ui::SortColumn::Mtime;
                s->pane->ActiveTab()->sort_direction = ui::SortDirection::Desc;
            }
        }
        StartLoadingPath(*s, *s->pane->ActiveTab(), startPath, PathLoadReason::Navigate);
        RememberPath(*s, startPath);
        if (!s->shot.active && (has_initial_external_target || !s->open_path.empty()))
            SelectLaunchedFile(*s, openTarget);

        if (s->core.global_ui_owner == hwnd) s->systemIntegration.Start(*s);

        s->renderer.SetDetailsPanelVisible(s->showDetailsPanel);
        s->renderer.SetDetailsPanelWidth(s->detailsPanelWidth);
        SyncUiScheduling(*s);
        SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER |
            SWP_NOACTIVATE | SWP_FRAMECHANGED);
        return 0;
    }

    case WM_GETMINMAXINFO: {
        // Arrives before WM_CREATE; GWLP_USERDATA is not set yet.
        auto* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
        float sc = s ? s->scale : 1.0f;
        // The window may shrink until only the sidebar rail and the file list are
        // left, the way File Explorer does; below this the toolbar starts to clip.
        mmi->ptMinTrackSize.x = (LONG)(320 * sc);
        mmi->ptMinTrackSize.y = (LONG)(420 * sc);
        MONITORINFO monitor{sizeof(monitor)};
        if (GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &monitor)) {
            mmi->ptMaxPosition.x = monitor.rcWork.left - monitor.rcMonitor.left;
            mmi->ptMaxPosition.y = monitor.rcWork.top - monitor.rcMonitor.top;
            mmi->ptMaxSize.x = monitor.rcWork.right - monitor.rcWork.left;
            mmi->ptMaxSize.y = monitor.rcWork.bottom - monitor.rcWork.top;
            mmi->ptMaxTrackSize.x = std::max(mmi->ptMaxTrackSize.x, mmi->ptMaxSize.x);
            mmi->ptMaxTrackSize.y = std::max(mmi->ptMaxTrackSize.y, mmi->ptMaxSize.y);
        }
        return 0;
    }

    case WM_NCHITTEST: {
        POINT screenPt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        if (s && IsSettingsTab(ActiveTab(*s))) {
            POINT client = screenPt;
            ScreenToClient(hwnd, &client);
            if (client.x >= s->compositor.Width() - 32.0f * s->scale) {
                D2D1_RECT_F track{}, thumb{};
                float maximum = 0.0f;
                if (s->renderer.SettingsScrollbarGeometry(HitTestVm(*s),
                        static_cast<float>(s->compositor.Width()),
                        static_cast<float>(s->compositor.Height()), track, thumb, maximum) &&
                    client.x >= track.left && client.x < track.right &&
                    client.y >= track.top && client.y < track.bottom)
                    return HTCLIENT;
            }
        }
        if (!IsZoomed(hwnd)) {
            RECT wr{};
            GetWindowRect(hwnd, &wr);
            const UINT dpi = pulse::compat::WindowDpi(hwnd);
            const int frameX = pulse::compat::SystemMetricsForDpi(SM_CXSIZEFRAME, dpi)
                             + pulse::compat::SystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
            const int frameY = pulse::compat::SystemMetricsForDpi(SM_CYSIZEFRAME, dpi)
                             + pulse::compat::SystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
            const bool left = screenPt.x >= wr.left && screenPt.x < wr.left + frameX;
            const bool right = screenPt.x < wr.right && screenPt.x >= wr.right - frameX;
            const bool top = screenPt.y >= wr.top && screenPt.y < wr.top + frameY;
            const bool bottom = screenPt.y < wr.bottom && screenPt.y >= wr.bottom - frameY;
            if (top && left) return HTTOPLEFT;
            if (top && right) return HTTOPRIGHT;
            if (bottom && left) return HTBOTTOMLEFT;
            if (bottom && right) return HTBOTTOMRIGHT;
            if (left) return HTLEFT;
            if (right) return HTRIGHT;
            if (top) return HTTOP;
            if (bottom) return HTBOTTOM;
        }
        if (!s) break;
        POINT pt = screenPt;
        ScreenToClient(hwnd, &pt);
        float x = (float)pt.x;
        float y = (float)pt.y;
        float tbH = s->renderer.TitleBarHeight();
        if (y >= 0 && y < tbH) {
            D2D1_RECT_F bounds = D2D1::RectF(0, 0,
                (float)s->compositor.Width(), (float)s->compositor.Height());
            ui::HitTestResult hit = s->renderer.HitTest(HitTestVm(*s), bounds, x, y);
            if (hit.region == ui::HitTestResult::Maximize) return HTMAXBUTTON;
            if (hit.region == ui::HitTestResult::Minimize) return HTMINBUTTON;
            if (hit.region == ui::HitTestResult::Close) return HTCLOSE;
            if (hit.region != ui::HitTestResult::None) return HTCLIENT;
            return HTCAPTION;
        }
        break;
    }

    case WM_NCMOUSEMOVE: {
        if (!s) break;
        POINT pt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        ScreenToClient(hwnd, &pt);
        D2D1_RECT_F bounds = D2D1::RectF(0, 0,
            (float)s->compositor.Width(), (float)s->compositor.Height());
        ui::HitTestResult hit = s->renderer.HitTest(HitTestVm(*s), bounds,
            (float)pt.x, (float)pt.y);
        s->hoverPoint = pt;
        const int region = static_cast<int>(hit.region);
        if (region != s->hoverRegion || hit.index != s->hoverControlIndex) {
            ApplyHoverTarget(*s, hit);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE | TME_NONCLIENT, hwnd, 0 };
        TrackMouseEvent(&tme);
        break;
    }

    case WM_NCMOUSELEAVE:
        if (s) {
            s->hoverRegion = 0;
            s->hoverControlIndex = -1;
            s->hoverSubIndex = -1;
            s->hoverSince = 0;
            s->tooltipText.clear();
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;

    case WM_NCLBUTTONDOWN: {
        if (!s) break;
        if (wParam == HTCAPTION) s->window_caption_dragging = true;
        if (wParam == HTMINBUTTON) {
            ShowWindow(hwnd, SW_MINIMIZE);
            return 0;
        }
        if (wParam == HTMAXBUTTON) {
            ShowWindow(hwnd, IsZoomed(hwnd) ? SW_RESTORE : SW_MAXIMIZE);
            return 0;
        }
        if (wParam == HTCLOSE) {
            RequestAppWindowClose(*s);
            return 0;
        }
        break;
    }

    case WM_EXITSIZEMOVE:
        if (s && s->window_caption_dragging) {
            s->window_caption_dragging = false;
            POINT screen{};
            if (GetCursorPos(&screen)) TryMergeWindowAtScreenPoint(*s, screen);
        }
        return 0;

    case WM_CLOSE: {
        if (s) { RequestAppWindowClose(*s); return 0; }
        break;
    }

    case WM_EXIT_READY:
        if (s) CompleteApplicationExit(*s);
        return 0;

    case WM_NEW_WINDOW: {
        std::unique_ptr<ResolvedOpenTarget> target(
            reinterpret_cast<ResolvedOpenTarget*>(lParam));
        if (!s || !target || s->exit_requested || s->core.exiting) return 0;
        if (s->core.windows.Count() >= kMaxTopLevelWindows) {
            s->notification_toast.ShowError(hwnd, l10n::Get(l10n::StringId::Open),
                l10n::Get(l10n::StringId::OperationFailedMessage));
            return 0;
        }
        const HINSTANCE instance = s->core.instance ? s->core.instance : GetModuleHandleW(nullptr);
        auto child = std::make_shared<AppState>(s->core);
        child->secondary_window = true;
        child->isolatedTest = s->isolatedTest;
        child->safeMode = s->safeMode;
        child->contentIndexObserver = s->contentIndexObserver;
        child->open_path = target->folder;
        child->initial_external_open_target = std::move(*target);
        child->sidebarCollapsedMask = s->sidebarCollapsedMask;
        child->sidebarHiddenMask = s->sidebarHiddenMask;
        child->sidebarQuickAccessHiddenMask = s->sidebarQuickAccessHiddenMask;
        child->sidebarOrder = s->sidebarOrder;
        child->starredExpanded = s->starredExpanded;
        s->core.owned_windows.push_back(child);
        RECT source{};
        if (!GetWindowRect(hwnd, &source)) source = RECT{0, 0, 1600, 960};
        int left = source.left + 32, top = source.top + 32;
        int width = (std::max)(640L, source.right - source.left);
        int height = (std::max)(480L, source.bottom - source.top);
        HWND created = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP,
            app::SingleInstanceCoordinator::WindowClassName(), L"Pulse",
            WS_OVERLAPPEDWINDOW,
            left, top, width, height,
            nullptr, nullptr, instance, child.get());
        if (!created) {
            std::erase(s->core.owned_windows, child);
            return 0;
        }
        HICON icon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_PULSE));
        if (icon) SendMessageW(created, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(icon));
        child->compositor.QueueShowTransition();
        ShowWindow(created, SW_SHOWNORMAL);
        UpdateWindow(created);
        return reinterpret_cast<LRESULT>(child.get());
    }

    case WM_DETACH_LAYOUT_TAB: {
        if (!s || s->core.windows.Count() >= kMaxTopLevelWindows ||
            wParam >= s->window_tabs.items.size()) return 0;
        auto target = std::make_unique<ResolvedOpenTarget>();
        target->this_pc = true;
        auto* child = reinterpret_cast<AppState*>(SendMessageW(hwnd, WM_NEW_WINDOW, 0,
            reinterpret_cast<LPARAM>(target.release())));
        if (child && !TransferLayoutTab(*s, static_cast<size_t>(wParam), *child, true, true))
            RequestWindowClose(*child);
        return 0;
    }

    case WM_COPYDATA: {
        auto* cds = reinterpret_cast<COPYDATASTRUCT*>(lParam);
        std::wstring path;
        if (!s || !app::SingleInstanceCoordinator::DecodeOpenPath(cds, path)) return FALSE;
        if (s->core.exiting) return FALSE;
        if (path.empty()) {
            AppState* recent = s->core.windows.MostRecent();
            AppState& target = recent ? *recent : *s;
            if (target.exit_requested) return FALSE;
            RestoreAppWindow(target);
            return TRUE;
        }
        return RouteExternalFolderOpen(*s, path) ? TRUE : FALSE;
    }

    case app::kSystemIntegrationMessage:
    case app::kShellSelectionMessage:
    case app::kExplorerTakeoverMessage:
        if (s && s->systemIntegration.Tick(*s)) InvalidateRect(hwnd, nullptr, FALSE);
        return 0;

    case app::TrayController::kCallbackMessage: {
        if (!s) return 0;
        const auto result = s->tray_controller.HandleCallback(lParam, [s] {
            AppState* recent = s->core.windows.MostRecent();
            if (recent && !recent->exit_requested) RestoreAppWindow(*recent);
        });
        if (result == app::TrayController::CallbackResult::ExitRequested)
            RequestApplicationExit(*s);
        return 0;
    }

    case WM_DPICHANGED: {
        s->scale = (float)HIWORD(wParam) / 96.0f;
        RECT* rc = reinterpret_cast<RECT*>(lParam);
        SetWindowPos(hwnd, nullptr, rc->left, rc->top,
            rc->right - rc->left, rc->bottom - rc->top,
            SWP_NOZORDER | SWP_NOACTIVATE);
        s->compositor.RecreateTextFormats(s->scale);
        s->renderer.SetScale(s->scale);
        if (s->editFont) {
            DeleteObject(s->editFont);
            s->editFont = nullptr;
        }
        EnsureEditVisuals(*s);
        if (s->hwndAddressEdit) SendMessageW(s->hwndAddressEdit, WM_SETFONT, (WPARAM)s->editFont, TRUE);
        if (s->hwndRenameEdit) SendMessageW(s->hwndRenameEdit, WM_SETFONT, (WPARAM)s->editFont, TRUE);
        if (s->hwndTagRenameEdit) SendMessageW(s->hwndTagRenameEdit, WM_SETFONT, (WPARAM)s->editFont, TRUE);
        if (s->addressEditing) LayoutAddressEditor(*s);
        if (!s->tagRenameId.empty()) LayoutTagRenameOverlay(*s);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }

    case WM_SETTINGCHANGE:
        if (s && (lParam == 0 ||
                  wcscmp(reinterpret_cast<const wchar_t*>(lParam), L"ImmersiveColorSet") == 0 ||
                  wcscmp(reinterpret_cast<const wchar_t*>(lParam), L"HighContrast") == 0)) {
            if (s->EffectiveThemeMode() == ui::ThemeMode::Auto)
                s->darkMode = ui::ShouldUseDarkMode(s->EffectiveThemeMode());
            if (s->appPrefs.accent_rgb.empty())
                ApplyAccentFromPrefs(*s);
            ApplyAppWindowChrome(*s);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;

    case WM_DEVICECHANGE:
        // A USB stick, a mounted image, or any new volume changes the drive
        // list. The sidebar model is built once, so rebuild it here instead of
        // waiting for a language switch or a restart.
        if (s && (wParam == DBT_DEVICEARRIVAL || wParam == DBT_DEVICEREMOVECOMPLETE)) {
            const auto* header = reinterpret_cast<const DEV_BROADCAST_HDR*>(lParam);
            if (!header || header->dbch_devicetype == DBT_DEVTYP_VOLUME) {
                RefreshSidebarModel(*s);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
        }
        return 0;

    case WM_SHOWWINDOW:
        // Sent before the window becomes visible; restore full rate right away.
        if (s && wParam && !IsIconic(hwnd)) SyncUiScheduling(*s);
        break;

    case WM_ACTIVATE:
        if (s && LOWORD(wParam) != WA_INACTIVE) s->core.windows.Touch(hwnd);
        if (s && s->menu && LOWORD(wParam) == WA_INACTIVE)
            s->menu->DismissForDeactivation(reinterpret_cast<HWND>(lParam));
        if (s) s->renderer.NotifyPreviewActivate(LOWORD(wParam) != WA_INACTIVE);
        break;

    case WM_ACTIVATEAPP:
        if (s && !wParam) {
            if (s->menu) s->menu->DismissForDeactivation(nullptr);
            s->detailsPreviewPanning = false;
            s->renderer.EndDetailsPreviewPan();
            if (GetCapture() == hwnd) ReleaseCapture();
        }
        if (s) s->renderer.NotifyPreviewActivate(wParam != 0);
        return 0;

    case WM_MOVE:
        if (s) {
            s->compositor.UpdateTextRenderingParams(
                MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST));
            s->renderer.NotifyPreviewOwnerMoved();
        }
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;

    case WM_SIZE: {
        if (s) {
            s->compositor.Resize(LOWORD(lParam), HIWORD(lParam));
            s->maximized = (wParam == SIZE_MAXIMIZED);
            if (wParam != SIZE_MINIMIZED && IsWindowVisible(hwnd)) SyncUiScheduling(*s);
            if (s->addressEditing) LayoutAddressEditor(*s);
            if (s->filterEditing && !s->filterFocusPending) LayoutFilterEditor(*s);
            if (!s->tagRenameId.empty()) LayoutTagRenameOverlay(*s);
            ClampScroll(*s);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }

    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        const bool animating = s && Render(*s);
        EndPaint(hwnd, &ps);
        // Present throttles animations to the display. Queue the next low-priority
        // paint after EndPaint so high-refresh monitors are not capped by WM_TIMER.
        if (s && (s->scrollAnimating || animating) && s->compositor.CanScheduleAnimationFrame() &&
            IsWindowVisible(hwnd) && !IsIconic(hwnd))
            InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }

    case WM_ERASEBKGND:
        return 1;

    case WM_CTLCOLOREDIT: {
        if (!s) break;
        EnsureEditVisuals(*s);
        HDC hdc = reinterpret_cast<HDC>(wParam);
        SetTextColor(hdc, s->darkMode ? RGB(255, 255, 255) : RGB(26, 26, 26));
        SetBkColor(hdc, s->darkMode ? RGB(30, 30, 30) : RGB(255, 255, 255));
        SetBkMode(hdc, OPAQUE);
        return reinterpret_cast<LRESULT>(s->editBrush);
    }

    case WM_TIMER: {
        if (s && s->ui_schedule.stopped) return 0;
        if (s && wParam == kTimerUiDeadline) {
            KillTimer(hwnd, kTimerUiDeadline);
            s->ui_schedule.deadline = 0;
            if (!s->exit_requested && RunUiDeadlines(*s, GetTickCount64()))
                InvalidateRect(hwnd, nullptr, FALSE);
            SyncUiScheduling(*s);
            return 0;
        }
        if (s && wParam == kTimerUi) {
            if (CompleteApplicationExit(*s)) return 0;
            if (s->exit_requested) return 0;
            SyncUiScheduling(*s);
            bool dirty = false;
            if (s->systemIntegration.Tick(*s)) dirty = true;
            if (TickChangeTracking(*s)) dirty = true;
            if (s->folderSizes.TakeChanged()) dirty = true;
            DrainDirNotifies(*s);
            const ULONGLONG now = GetTickCount64();
            if (RunUiDeadlines(*s, now)) dirty = true;
            // Probe stalled animation presentation at maintenance rate, never
            // spin WM_PAINT when the swap chain is occluded or unavailable.
            if (IsWindowVisible(hwnd) && !IsIconic(hwnd) &&
                (s->ui_schedule.animation_active || s->compositor.NeedsRecovery())) dirty = true;
            if (PumpClipboardAvailability(*s, now)) dirty = true;
            if (app::PumpTrayMetadata(s->tray, s->worker, s->tray_metadata)) dirty = true;
            if (TickAddressSearchMaintenance(*s, now)) dirty = true;
            if (RefreshContentResults(*s)) dirty = true;
            if (PumpRecycleRefresh(*s, now)) dirty = true;
            if (s->home_catalog.ConsumeUpdate()) dirty = true;
            MaybePrefetchCtxMenu(*s);
            s->places.FlushPendingSave(false);
            // Tray animation advances during painting, like smooth scrolling.
            if (app::Tab* tab = ActiveTab(*s)) {
                std::wstring kind, rest;
                if (app::ParsePulsePath(tab->current_path, &kind, &rest) &&
                    kind == L"settings") {
                    const int page = app::SettingsController::PageFromName(rest);
                    if (page == 4 && s->duplicateScan.scanning) dirty = true;
                }
            }
            QueueVisibleTagDiscovery(*s);
            UpdateOperationWindow(*s, false);
            PumpPermissionOperationPrompt(*s);
            if (dirty) InvalidateRect(hwnd, nullptr, FALSE);
            SyncUiScheduling(*s);
        }
        return 0;
    }

    case WM_SETCURSOR: {
        if (!s || LOWORD(lParam) != HTCLIENT) break;
        if (s->starDragActive || s->tagDragActive || s->tabDragging) {
            SetCursor(LoadCursorW(nullptr, IDC_SIZEALL));
            return TRUE;
        }
        POINT pt{};
        GetCursorPos(&pt);
        ScreenToClient(hwnd, &pt);
        if (s->sidebarResizing || SidebarResizeHit(*s, pt.x, pt.y)) {
            SetCursor(LoadCursorW(nullptr, IDC_SIZEWE));
            return TRUE;
        }
        const ui::WindowViewModel& vm = HitTestVm(*s);
        D2D1_RECT_F bounds = D2D1::RectF(0, 0,
            (float)s->compositor.Width(), (float)s->compositor.Height());
        ui::HitTestResult hit = s->renderer.HitTest(vm, bounds, (float)pt.x, (float)pt.y);
        if (s->renderer.DetailsPreviewDragging() ||
            (hit.region == ui::HitTestResult::DetailsPreview && s->renderer.CanDetailsPreviewPan())) {
            SetCursor(ui::PreviewGrabCursor(s->renderer.DetailsPreviewDragging()));
            return TRUE;
        }
        if (hit.region == ui::HitTestResult::DetailsPreviewToggle) {
            SetCursor(LoadCursorW(nullptr, IDC_HAND));
            return TRUE;
        }
        if (s->detailsPanelResizing || hit.region == ui::HitTestResult::DetailsResize) {
            SetCursor(LoadCursorW(nullptr, IDC_SIZEWE));
            return TRUE;
        }
        if (s->columnResizing || hit.region == ui::HitTestResult::ColumnDivider ||
            s->stripResizing || hit.region == ui::HitTestResult::ColumnStripDivider) {
            SetCursor(LoadCursorW(nullptr, IDC_SIZEWE));
            return TRUE;
        }
        const bool splitter = s->splitterDragging || hit.region == ui::HitTestResult::Splitter;
        if (splitter) {
            const bool vertical = s->splitterDragging
                ? (s->splitterOrientation == app::SplitOrientation::Vertical)
                : (hit.index >= 0 && hit.index < static_cast<int>(vm.splitters.size()) &&
                   vm.splitters[static_cast<size_t>(hit.index)].vertical);
            SetCursor(LoadCursorW(nullptr, vertical ? IDC_SIZEWE : IDC_SIZENS));
            return TRUE;
        }
        if (hit.region == ui::HitTestResult::RowFolderSize ||
            hit.region == ui::HitTestResult::FilterClear ||
            hit.region == ui::HitTestResult::AddressSearch ||
            hit.region == ui::HitTestResult::AddressSearchScope ||
            hit.region == ui::HitTestResult::AddressSearchClear ||
            hit.region == ui::HitTestResult::AddressSearchClose ||
            hit.region == ui::HitTestResult::SettingsDupOpen) {
            SetCursor(LoadCursorW(nullptr, IDC_HAND));
            return TRUE;
        }
        if (hit.region == ui::HitTestResult::AddressSearchMode ||
            hit.region == ui::HitTestResult::AddressSearchContent ||
            hit.region == ui::HitTestResult::AddressSearchOptions) {
            SetCursor(LoadCursorW(nullptr, IDC_ARROW));
            return TRUE;
        }
        if (hit.region == ui::HitTestResult::AddressBar) {
            SetCursor(LoadCursorW(nullptr, IDC_IBEAM));
            return TRUE;
        }
        if (hit.region == ui::HitTestResult::StatusBarTask || hit.region == ui::HitTestResult::StatusBarCancelSearch) {
            SetCursor(LoadCursorW(nullptr, IDC_HAND));
            return TRUE;
        }
        if (hit.region == ui::HitTestResult::SettingsAccent) {
            SetCursor(LoadCursorW(nullptr, IDC_HAND));
            return TRUE;
        }
        if (reinterpret_cast<HWND>(wParam) == hwnd) {
            SetCursor(LoadCursorW(nullptr, IDC_ARROW));
            return TRUE;
        }
        break;
    }

    case WM_CANCELMODE:
        if (s && s->menu) s->menu->DismissForDeactivation(nullptr);
        HandleSidebarResize(s, hwnd, msg, lParam);
        if (s) { s->detailsPreviewPanning = false; s->renderer.EndDetailsPreviewPan(); }
        if (GetCapture() == hwnd) ReleaseCapture();
        break;
    case WM_MOUSEMOVE:
        if (HandleSidebarResize(s, hwnd, msg, lParam)) return 0;
        if (HandleDetailsPreviewPointer(s, hwnd, msg, wParam, lParam)) return 0;
        return HandleMouseMove(s, hwnd, msg, wParam, lParam);

    case WM_MOUSELEAVE:
        return HandleMouseLeave(s, hwnd, msg, wParam, lParam);

    case WM_LBUTTONDOWN:
        if (HandleSidebarResize(s, hwnd, msg, lParam)) return 0;
        if (HandleDetailsPreviewPointer(s, hwnd, msg, wParam, lParam)) return 0;
        return HandleLButtonDown(s, hwnd, msg, wParam, lParam);

    case WM_LBUTTONDBLCLK:
        return HandleLButtonDblClk(s, hwnd, msg, wParam, lParam);

    case WM_LBUTTONUP:
        if (HandleSidebarResize(s, hwnd, msg, lParam)) return 0;
        if (HandleDetailsPreviewPointer(s, hwnd, msg, wParam, lParam)) return 0;
        return HandleLButtonUp(s, hwnd, msg, wParam, lParam);

    case WM_CAPTURECHANGED:
        HandleSidebarResize(s, hwnd, msg, lParam);
        if (s) { s->detailsPreviewPanning = false; s->renderer.EndDetailsPreviewPan(); }
        return HandleCaptureChanged(s, hwnd, msg, wParam, lParam);

    case WM_RBUTTONDOWN:
        return HandleRButtonDown(s, hwnd, msg, wParam, lParam);

    case WM_RBUTTONUP:
        return HandleRButtonUp(s, hwnd, msg, wParam, lParam);

    case WM_MOUSEWHEEL:
        return HandleMouseWheel(s, hwnd, msg, wParam, lParam);

    case WM_APPCOMMAND:
        // DefWindowProc translates side-button releases, including child controls.
        if (s && HandleBrowserNavigation(*s, lParam)) return TRUE;
        break;

    case WM_HOTKEY:
        if (s && wParam == GlobalSearchHotkey::kId) {
            if (s->settings.global_search_hotkey_capturing() && IsSettingsTab(ActiveTab(*s))) {
                s->settings.CaptureGlobalSearchHotkey(HIWORD(lParam), LOWORD(lParam));
                InvalidateRect(hwnd, nullptr, FALSE);
            } else ToggleGlobalSearch(*s);
            return 0;
        }
        break;

    case WM_SYSKEYDOWN:
        if (s && HandleGlobalSearchHotkeyCapture(*s, static_cast<UINT>(wParam))) return 0;
        if (s && wParam == L'D' && (GetKeyState(VK_MENU) & 0x8000)) {
            ShowOmnibar(*s, OmnibarMode::Path);
            return 0;
        }
        // Alt+Enter arrives as WM_SYSKEYDOWN, so HandleKeyDown's advertised
        // Properties shortcut (alt && VK_RETURN) was unreachable.
        if (s && wParam == VK_RETURN && (GetKeyState(VK_MENU) & 0x8000))
            return HandleKeyDown(s, hwnd, msg, wParam, lParam);
        break;

    case WM_SYSCHAR:
        if (wParam == L'\r') return 0; // Alt+Enter handled above; no default beep
        break;

    case WM_KEYDOWN:
        if (s && HandleGlobalSearchHotkeyCapture(*s, static_cast<UINT>(wParam))) return 0;
        if (s && wParam == VK_ESCAPE && s->detailsPreviewPanning) {
            s->detailsPreviewPanning = false;
            s->renderer.EndDetailsPreviewPan();
            if (GetCapture() == hwnd) ReleaseCapture();
            return 0;
        }
        return HandleKeyDown(s, hwnd, msg, wParam, lParam);

    case WM_COMMAND: {
        if (s && reinterpret_cast<HWND>(lParam) == s->hwndFilterEdit &&
            HIWORD(wParam) == EN_CHANGE) {
            SyncFilterEditor(*s);
            return 0;
        }
        if (s && reinterpret_cast<HWND>(lParam) == s->hwndAddressEdit &&
            (HIWORD(wParam) == EN_CHANGE || HIWORD(wParam) == EN_UPDATE)) {
            if (HIWORD(wParam) == EN_CHANGE) QueueAddressSearch(*s);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        if (s && wParam == 1001) {
            RefreshActiveTab(*s);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        break;
    }

    case WM_ARCHIVE_RESULT:
        if (s) HandleArchiveResult(*s, lParam);
        else DiscardArchiveResult(lParam);
        return 0;

    case WM_WORKER_RESULT: {
        if (s) {
            ProcessPendingResults(*s);
            ClampScroll(*s);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }

    case WM_RECYCLE_INFO: {
        auto* info = reinterpret_cast<fs::RecycleBinInfo*>(lParam);
        if (s && info && ApplyQueriedRecycleInfo(*s, *info))
            InvalidateRect(hwnd, nullptr, FALSE);
        delete info;
        return 0;
    }

    case WM_OPS_NOTIFY: {
        if (s) {
            for (const auto& error : s->ops.DrainShellLaunchErrors())
                s->notification_toast.ShowError(hwnd, l10n::Get(l10n::StringId::Open), error);
            UpdateOperationWindow(*s, !s->exit_requested && !s->lockedOperationPromptActive &&
                !s->permissionOperationPromptActive);
            const ops::OpStatus st = s->ops.Status();
            if (s->ops.TakeCtxInvokeDone()) {
                if (app::Tab* tab = ActiveTab(*s)) s->store.MarkDirty(tab->current_path);
                RefreshActiveTab(*s, RefreshReason::ShellNotification);
                ScheduleRecycleRefresh(*s);
                RefreshRecycleViews(*s, false);
            }
            if (st.completed_ops != s->opsCompleted) {
                const uint64_t finished_count = st.completed_ops - s->opsCompleted;
                s->opsCompleted = st.completed_ops;
                const auto completions = s->ops.DrainCompletions();
                const uint64_t refused = st.deletes_without_mutation - s->deletesWithoutMutation;
                s->deletesWithoutMutation = st.deletes_without_mutation;
                const bool refresh_view = !completions.empty() || finished_count > refused;
                for (const auto& outcome : s->ops.DrainDeleteOutcomes()) {
                    if (!s->previewDeleteIntent || s->previewDeleteIntent->task_id != outcome.task_id) continue;
                    if (outcome.mutated) s->previewDeleteIntent->succeeded = true;
                    else if (!outcome.uncertain) s->previewDeleteIntent.reset();
                }
                std::vector<std::wstring> tag_metadata_paths;
                for (const auto& completed : completions) {
                    if (completed.volume_label && !completed.sources.empty()) {
                        continue;
                    }
                    // Ask for confirmed index changes on the next tick. Keep any
                    // in-flight request and never invent a deletion before capture.
                    s->changes.last_query = 0;
                    s->changes.last_detail_refresh = 0;
                    // Invalidate both sides of every successful mutation. The
                    // focused tab refreshes below; background tabs must not
                    // reuse a stale snapshot when they are shown later.
                    for (const auto& source : completed.sources) {
                        const std::wstring parent = fs::ParentPath(source);
                        if (!parent.empty()) s->store.MarkDirty(parent);
                    }
                    for (const auto& destination : completed.destinations) {
                        const std::wstring parent = fs::ParentPath(destination);
                        if (!parent.empty()) s->store.MarkDirty(parent);
                    }
                    if (completed.type == ops::OpType::CreateFolder ||
                        completed.type == ops::OpType::CreateTextFile) {
                        for (const auto& source : completed.sources)
                            QueueCreatedItemReveal(*s, source);
                    } else if (completed.type == ops::OpType::Copy) {
                        for (size_t i = 0; i < completed.sources.size() &&
                                           i < completed.destinations.size(); ++i) {
                            s->places.CloneAssignments(completed.sources[i],
                                                       completed.destinations[i]);
                            tag_metadata_paths.push_back(completed.destinations[i]);
                        }
                    } else if (completed.type == ops::OpType::Move ||
                               completed.type == ops::OpType::Rename ||
                               completed.type == ops::OpType::BatchRename) {
                        for (size_t i = 0; i < completed.sources.size() &&
                                           i < completed.destinations.size(); ++i) {
                            if (completed.type == ops::OpType::Rename ||
                                completed.type == ops::OpType::BatchRename) {
                                const auto parent = fs::ParentPath(completed.sources[i]);
                                if (_wcsicmp(parent.c_str(), fs::ParentPath(completed.destinations[i]).c_str()) == 0) {
                                    ForEachPane(*s, [&](app::Pane& pane) {
                                        if (auto* tab = pane.ActiveTab(); tab &&
                                            _wcsicmp(tab->current_path.c_str(), parent.c_str()) == 0) {
                                            tab->HoldEntryRename(PathFindFileNameW(completed.sources[i].c_str()),
                                                                 PathFindFileNameW(completed.destinations[i].c_str()));
                                        }
                                    });
                                }
                            }
                            s->places.RemapPaths(completed.sources[i],
                                                 completed.destinations[i]);
                            tag_metadata_paths.push_back(completed.destinations[i]);
                        }
                        if (completed.type == ops::OpType::Move &&
                            s->pendingCutClipboardSequence != 0) {
                            s->completedCutClipboardPaths.insert(
                                s->completedCutClipboardPaths.end(), completed.sources.begin(),
                                completed.sources.end());
                            const bool all_done = std::all_of(
                                s->pendingCutClipboardPaths.begin(),
                                s->pendingCutClipboardPaths.end(), [&](const std::wstring& expected) {
                                    const std::wstring normalized = fs::NormalizePath(expected);
                                    return std::any_of(s->completedCutClipboardPaths.begin(),
                                        s->completedCutClipboardPaths.end(),
                                        [&](const std::wstring& actual) {
                                            return _wcsicmp(normalized.c_str(), actual.c_str()) == 0;
                                        });
                                });
                            if (all_done) {
                                ops::CompleteCutClipboard(s->pendingCutClipboardSequence,
                                                          s->pendingCutClipboardPaths);
                                s->pendingCutClipboardSequence = 0;
                                s->pendingCutClipboardPaths.clear();
                                s->completedCutClipboardPaths.clear();
                            }
                        }
                    } else if (completed.type == ops::OpType::RecycleDelete ||
                               completed.type == ops::OpType::RealDelete) {
                        for (const auto& source : completed.sources)
                            s->places.RemoveAssignments(source, true);
                        s->duplicateScan.RemoveDeleted(completed.sources);
                        s->tray.RemoveDeleted(completed.sources);
                    }
                    if (completed.type == ops::OpType::RecycleDelete ||
                        completed.type == ops::OpType::RealDelete ||
                        completed.type == ops::OpType::RestoreRecycle ||
                        completed.type == ops::OpType::EmptyRecycle) {
                        bool recycle_visible = false;
                        ForEachPane(*s, [&](app::Pane& pane) {
                            if (IsRecycleTab(pane.ActiveTab())) recycle_visible = true;
                        });
                        const auto n = static_cast<int64_t>(completed.sources.size());
                        ScheduleRecycleRefresh(*s);
                        if (completed.type == ops::OpType::RecycleDelete)
                            BumpRecycleOccupancy(*s, n);
                        else if (completed.type == ops::OpType::EmptyRecycle)
                            ClearRecycleOccupancy(*s);
                        else if (completed.type == ops::OpType::RestoreRecycle ||
                                 (completed.type == ops::OpType::RealDelete &&
                                  recycle_visible))
                            BumpRecycleOccupancy(*s, -n);
                        RefreshRecycleViews(*s, false);
                    }
                    if (completed.type == ops::OpType::Copy || completed.type == ops::OpType::Move)
                        QueueTransferredItemsReveal(*s, completed.destinations);
                    RefreshStarredViews(*s);
                    RefreshRecentViews(*s);
                }
                if (!tag_metadata_paths.empty())
                    QueueTagAds(*s, BuildTagAdsUpdates(s->places, tag_metadata_paths, true));
                // An op finished: refresh the view (watcher also fires, this is immediate).
                app::Tab* tab = ActiveTab(*s);
                if (tab && refresh_view) {
                    s->store.MarkDirty(tab->current_path);
                    RefreshActiveTab(*s, RefreshReason::OperationCompleted);
                }
                for (const auto& completed : completions) {
                    if (completed.volume_label && !completed.sources.empty())
                        ApplyDriveLabelRename(*s, completed.sources.front(), *completed.volume_label);
                }
                if (s->previewDeleteIntent && s->previewDeleteIntent->succeeded) SyncQuickPreview(*s);
            }
            PumpPermissionOperationPrompt(*s);
            if (!s->permissionOperationPromptActive) PumpLockedOperationPrompt(*s);
            RefreshOpenCtxMenu(*s);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }

    case WM_SHELLCTX_ITEMS: {
        auto* payload = reinterpret_cast<ShellCtxItemsPayload*>(lParam);
        const uint32_t token = static_cast<uint32_t>(wParam);
        if (s && payload) {
            const auto completed = s->context_menu.CompleteComQuery(
                token, std::move(payload->items), GetTickCount64(), payload->partial);
            if (completed.accepted) {
                bool prefs_changed = false;
                if (!completed.partial &&
                    s->ctxMenuPrefs.RecordComTiming(completed.cache_key,
                                                    completed.elapsed_ms))
                    prefs_changed = true;
                if (!completed.partial) {
                    for (const auto& clsid : payload->slow_clsids) {
                        if (s->ctxMenuPrefs.RecordComTiming(
                                ipc::HandlerCatalogKey(clsid), 1000))
                            prefs_changed = true;
                    }
                }
                if (prefs_changed) s->ctxMenuPrefs.Save();
                RefreshOpenCtxMenu(*s);
            } else if (!payload->partial) {
                s->ops.CloseShellMenu(token);
            }
        }
        delete payload;
        return 0;
    }

    case WM_SHELL_VERBS: {
        if (s && s->shell_verbs_mailbox) {
            for (auto& result : s->shell_verbs_mailbox->Take(wParam)) {
                if (s->context_menu.CompleteStaticVerbs(result.ext, std::move(result.verbs)))
                    RefreshOpenCtxMenu(*s);
            }
        }
        return 0;
    }

    case WM_SHELL_CACHE_INVALIDATE: {
        if (s && s->shell_registry_mailbox && !s->shell_registry_mailbox->Take(wParam).empty()) {
            s->context_menu.InvalidateCaches();
            SeedShellVerbCache(*s);
        }
        return 0;
    }

    case WM_CLIPBOARD_PASTE_RESULT:
        if (s && s->clipboard_paste_mailbox) {
            for (const bool success : s->clipboard_paste_mailbox->Take(wParam)) {
                if (!success) s->notification_toast.Show(hwnd,
                    l10n::Get(l10n::StringId::OpCopyFailed),
                    l10n::Get(l10n::StringId::OperationFailedMessage));
            }
        }
        return 0;

    case WM_EXTERNAL_OPEN_READY:
        if (s) ApplyExternalOpenResults(*s, wParam);
        return 0;

    case WM_DETACH_PANE: {
        if (!s || lParam < 0 ||
            static_cast<size_t>(lParam) >= s->window_tabs.items.size()) return 0;
        ResolvedOpenTarget empty_target;
        empty_target.this_pc = true;
        auto* payload = new (std::nothrow) ResolvedOpenTarget(std::move(empty_target));
        if (!payload) return 0;
        auto* child = reinterpret_cast<AppState*>(SendMessageW(hwnd, WM_NEW_WINDOW, 0,
            reinterpret_cast<LPARAM>(payload)));
        if (child && !TransferPane(*s, static_cast<size_t>(wParam), *child,
                static_cast<size_t>(lParam), true))
            PostMessageW(child->hwnd, WM_CLOSE, 0, 0);
        return 0;
    }

    case WM_HEADER_DROP_RESULT:
        if (s) HandleHeaderDropResult(*s);
        return 0;
    case WM_DETAILS_META: {
        auto* result = reinterpret_cast<DetailsMetaResult*>(lParam);
        if (s && result && ApplyDetailsMetaResult(*s, *result)) {
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        delete result;
        return 0;
    }

    case WM_CHANGE_TRACKING:
        if (s) ReceiveChangeTracking(*s, static_cast<uint32_t>(wParam), lParam != 0);
        return 0;

    case WM_INDEX_NOTIFY: {
        if (s) {
            const bool ready = s->index.PinyinReady();
            if (ready && !s->pinyinReadyLast && s->appPrefs.search_pinyin) {
                ForEachPane(*s, [&](app::Pane& pane) {
                    auto* tab = pane.ActiveTab();
                    std::wstring kind, rest;
                    if (!tab || !app::ParsePulsePath(tab->current_path, &kind, &rest) ||
                        kind != L"search" || app::SplitSearchQueryText(rest).content.present()) return;
                    if (tab->snapshot && tab->selected_index >= 0 &&
                        static_cast<size_t>(tab->selected_index) < tab->EntryCount())
                        tab->search_preserve_selection = tab->EntryAt(static_cast<size_t>(tab->selected_index)).full_path;
                    RequestSearchPage(*s, *tab, rest, true);
                });
            }
            s->pinyinReadyLast = ready;
            if (IndexStatusVisible(*s)) InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }

    case WM_NETWORK_INDEX_NOTIFY: {
        if (s && IndexStatusVisible(*s)) InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }

    case WM_SETTINGS_TASK_RESULT: {
        auto* result = reinterpret_cast<app::SettingsTaskResult*>(lParam);
        if (s && result) {
            if (!result->ok && result->task.kind ==
                    app::SettingsTaskKind::DiagnosticsExport)
                result->error = l10n::Get(l10n::StringId::DiagnosticsExportFailed);
            const auto effect = s->settings.CompleteTask(*result, s->index.ServiceInstalled());
            if (!result->ok && result->task.kind == app::SettingsTaskKind::ConfigureIndexPath &&
                !result->error.empty()) {
                s->notification_toast.Show(hwnd, l10n::Get(l10n::StringId::IndexLocation), result->error);
            }
            if (effect.refresh_index) s->index.RefreshVolumesAsync();
            if (!effect.pin_network.empty()) {
                s->places.PinNetwork(effect.pin_network, L"");
                RequestUncProbe(*s, effect.pin_network);
            }
            if (!effect.open_path.empty()) {
                s->ops.OpenWith(effect.open_path);
            }
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        delete result;
        return 0;
    }

    case WM_INDEX_SEARCH: {
        if (!s) return 0;
        const uint32_t id = static_cast<uint32_t>(wParam);
        index::SearchResult result;
        if (!s->index.TakeResult(id, result)) return 0;
        AcceptIndexProviderResult(*s, id, std::move(result), false);
        return 0;
    }

    case WM_NETWORK_INDEX_SEARCH: {
        if (!s) return 0;
        const uint32_t id = static_cast<uint32_t>(wParam);
        index::SearchResult result;
        if (!s->networkIndex.TakeResult(id, result)) return 0;
        AcceptIndexProviderResult(*s, id, std::move(result), true);
        return 0;
    }

    case WM_SEARCH_HISTORY:
        if (s && s->addressSearching && GetFocus() == s->hwndAddressEdit)
            ShowAddressSearchHistory(*s);
        return 0;

    case WM_CONTENT_SELECTION:
        if(s) CompleteContentSelection(*s);
        return 0;
    case WM_CONTENT_SEARCH: {
        if (!s) return 0;
        index::ContentSearchUpdate update;
        while (s->contentSearch.TakeUpdate(update))
            ApplyContentSearchUpdate(*s, std::move(update));
        RefreshContentResults(*s);
        return 0;
    }

    case WM_DUPLICATE_SCAN: {
        if (!s) return 0;
        index::ContentSearchUpdate update;
        while (s->duplicateSearch.TakeUpdate(update))
            s->duplicateScan.ApplyUpdate(update.progress, update.hits);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }

    case WM_DUP_VOLUMES: {
        if (s && s->dup_volumes_mailbox) {
            for (auto& volumes : s->dup_volumes_mailbox->Take(wParam))
                ApplyDuplicateVolumeCache(*s, std::move(volumes));
        }
        return 0;
    }

    case WM_QUICK_PREVIEW_NAVIGATE:
        if (s) NavigateQuickPreview(*s, static_cast<int>(wParam));
        return 0;

    case WM_QUICK_PREVIEW_OPEN:
        if (s) OpenSelected(*s);
        return 0;

    case WM_QUICK_PREVIEW_COMMAND:
        if (s) HandleQuickPreviewCommand(*s, static_cast<ui::QuickPreviewAction>(wParam),
                                         (lParam & 1) != 0);
        return 0;

    case WM_NET_PROBE: {
        std::unique_ptr<fs::UncProbeResult> result(reinterpret_cast<fs::UncProbeResult*>(lParam));
        if (s && result) ApplyUncProbeResult(*s, *result);
        else if (s && wParam) PumpUncProbe(*s);
        return 0;
    }

    case WM_TAG_ADS_WARNING: {
        std::unique_ptr<std::vector<std::wstring>> volumes(
            reinterpret_cast<std::vector<std::wstring>*>(lParam));
        if (s && volumes) {
            for (const auto& volume : *volumes) {
                const std::wstring shown = ClipboardPath(volume);
                if (!s->tagFallbackVolumes.insert(shown).second) continue;
                if (app::Tab* tab = ActiveTab(*s)) {
                    tab->SetBannerTitle(app::BannerType::Other, l10n::Get(l10n::StringId::TagsSavedLocally));
                    tab->banner_message = l10n::Get(l10n::StringId::TagMetadataUnsupported);
                    const size_t path_marker = tab->banner_message.find(L"{path}");
                    if (path_marker != std::wstring::npos)
                        tab->banner_message.replace(path_marker, 6, shown);
                }
            }
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }

    case WM_TAG_ADS_DISCOVERED: {
        std::unique_ptr<std::vector<TagAdsDiscovery>> discoveries(
            reinterpret_cast<std::vector<TagAdsDiscovery>*>(lParam));
        if (s && discoveries) {
            const uint64_t before = s->places.TagRevision();
            for (const auto& discovery : *discoveries) {
                const std::wstring key = TagDiscoveryKey(discovery.path);
                s->tagAdsDiscoveryQueued.erase(key);
                if (discovery.readable) {
                    s->tagAdsDiscoveryChecked.Record(key, discovery.version, GetTickCount64());
                    s->places.MergeAdsRecords(discovery.path, discovery.records,
                                              discovery.legacy_names);
                } else s->tagAdsDiscoveryChecked.Erase(key);
            }
            if (s->places.TagRevision() != before)
                InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }

    case WM_NCDESTROY:
        // WM_CREATE failure can bypass the normal WM_DESTROY cleanup path.
        if (s) {
            if (s->shell_verbs_mailbox) s->shell_verbs_mailbox->Close();
            if (s->shell_registry_mailbox) s->shell_registry_mailbox->Close();
            if (s->clipboard_paste_mailbox) s->clipboard_paste_mailbox->Close();
            if (s->external_open_mailbox) s->external_open_mailbox->Close();
            if (s->dup_volumes_mailbox) s->dup_volumes_mailbox->Close();
            s->core.windows.Remove(hwnd);
        }
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        if (s && s->secondary_window) {
            auto& owned = s->core.owned_windows;
            const auto it = std::find_if(owned.begin(), owned.end(),
                [s](const auto& candidate) { return candidate.get() == s; });
            if (it != owned.end()) {
                s->core.retired_windows.push_back(std::move(*it));
                owned.erase(it);
            }
        }
        return DefWindowProcW(hwnd, msg, wParam, lParam);

    case WM_DESTROY: {
        if (s) {
            const bool was_global_owner = s->core.global_ui_owner == hwnd;
            if (was_global_owner) s->core.global_ui_owner = nullptr;
            s->core.windows.Remove(hwnd);
            const bool last_window = s->core.windows.Count() == 0;
            AppState* next_owner = !last_window && was_global_owner
                ? s->core.windows.MostRecent() : nullptr;
            if (s->shell_verbs_mailbox) s->shell_verbs_mailbox->Close();
            if (s->shell_registry_mailbox) s->shell_registry_mailbox->Close();
            if (s->clipboard_paste_mailbox) s->clipboard_paste_mailbox->Close();
            if (s->external_open_mailbox) s->external_open_mailbox->Close();
            if (s->dup_volumes_mailbox) s->dup_volumes_mailbox->Close();
            StopUiScheduling(*s);
            // Destruction paths used by tests also save only after operations settle.
            if (s->exit_thread.joinable()) s->exit_thread.join();
            s->systemIntegration.Stop();
            s->ops.Stop();
            ShutdownGlobalSearch(*s);
            if (was_global_owner || last_window) {
                StopShellRegistryWatch();
            }
            s->watches.Stop();
            s->folderSizes.Stop();
            s->settings.ResetUi();
            s->settings.Stop();
            s->contentSearch.Stop();
            s->duplicateSearch.Stop();
            s->networkIndex.Stop();
            s->changes.client.Stop();
            s->index.Stop();
            ForEachPane(*s, [](app::Pane& pane) {
                if (pane.view.archive_cancel) pane.view.archive_cancel->store(true);
            });
            s->worker.Stop();
            MSG archive_message{};
            while (PeekMessageW(&archive_message, hwnd, WM_ARCHIVE_RESULT, WM_ARCHIVE_RESULT, PM_REMOVE))
                DiscardArchiveResult(archive_message.lParam);
            if (last_window) {
                const bool sweep_drops = !s->isolatedTest && !s->shot.active && !s->menushot;
                std::thread archive_cleanup([sweep_drops] {
                    CleanupArchiveFiles();
                    if (sweep_drops) ops::SweepDropStages(ops::DropStageRoot(), true);
                });
                archive_cleanup.join();
            }

            if (s->dropTarget) {
                RevokeDragDrop(hwnd);
                s->dropTarget->Release();
                s->dropTarget = nullptr;
            }
            s->menu.reset();
            s->operationWindow.reset();
            if (s->hwndAddressEdit) {
                DestroyWindow(s->hwndAddressEdit);
                s->hwndAddressEdit = nullptr;
            }
            if (s->hwndRenameEdit) {
                DestroyWindow(s->hwndRenameEdit);
                s->hwndRenameEdit = nullptr;
            }
            if (s->hwndTagRenameEdit) {
                DestroyWindow(s->hwndTagRenameEdit);
                s->hwndTagRenameEdit = nullptr;
            }
            if (s->hwndFilterEdit) {
                DestroyWindow(s->hwndFilterEdit);
                s->hwndFilterEdit = nullptr;
            }
            if (s->editFont) {
                DeleteObject(s->editFont);
                s->editFont = nullptr;
            }
            if (s->editBrush) {
                DeleteObject(s->editBrush);
                s->editBrush = nullptr;
            }
            // Keep UI preferences without recording open windows, tabs or folders.
            if (!s->shot.active && !s->menushot && !s->isolatedTest) {
                if (last_window) app::SaveSessionPreferences(CaptureWindowPreferences(*s, hwnd));
                s->places.Save();
                s->ctxMenuPrefs.Save();
                s->appPrefs.Save();
            }

            if (was_global_owner) {
                s->tray_controller.Detach();
                if (next_owner && !s->core.exiting) {
                    s->core.global_ui_owner = next_owner->hwnd;
                    next_owner->tray_controller.Attach(next_owner->hwnd, s->core.instance);
                    next_owner->systemIntegration.Start(*next_owner);
                    if (!next_owner->safeMode) StartShellRegistryWatch(*next_owner);
                    ApplyGlobalSearchSettings(*next_owner);
                }
            }
            if (last_window) s->single_instance.Release();

            s->renderer.SetIconNotifyWindow(nullptr);
            s->renderer.SetCompositor(nullptr);
            s->compositor.Shutdown();
            s->hwnd = nullptr;
        }
        if (!s || s->core.windows.Count() == 0) PostQuitMessage(0);
        return 0;
    }
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    __try {
        const LRESULT result = WndProcImpl(hwnd, msg, wParam, lParam);
        // Re-evaluate deadlines after input/completion messages, including ones
        // which cancel or replace a pending task. No per-frame timer reset.
        if (msg != WM_DESTROY && msg != WM_NCDESTROY && IsWindow(hwnd)) {
            if (AppState* state = GetAppState(hwnd)) SyncUiScheduling(*state);
        }
        return result;
    } __except (pulse::crash::AddBreadcrumb(1, msg, static_cast<int32_t>(GetExceptionCode())),
                pulse::crash::ReportFatal(GetExceptionInformation(), "wndproc")) {
        TerminateProcess(GetCurrentProcess(), GetExceptionCode());
        return 0;
    }
}

bool WaitForShotReady(AppState& s) {
    auto deadline = s.shot.start + std::chrono::seconds(5);
    MSG msg{};
    while (std::chrono::steady_clock::now() < deadline) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        ProcessPendingResults(s);
        app::Tab* tab = ActiveTab(s);
        if (tab && !tab->loading && tab->snapshot &&
            (tab->current_path != app::MakeHomePath() || !s.home_catalog.Loading())) {
            return true;
        }
        if (s.hwnd) {
            RedrawWindow(s.hwnd, nullptr, nullptr, RDW_UPDATENOW | RDW_INTERNALPAINT);
        }
        Sleep(20);
    }
    return false;
}

// Staged tag states for GUI-verification shots. Kept out of ShotModeMain's
// SEH frame, which forbids C++ locals with destructors.
void StageTagShotStates(AppState& state) {
    if (state.shot_tag_rename && !state.places.tags.empty()) {
        // Point the rename state at a real loaded tag (read-only): the
        // renderer draws the Fluent field frame on that row.
        state.tagRenameId =
            state.places.tags[1 % state.places.tags.size()].id;
    }
    if (state.shot_tag_drag && state.places.tags.size() >= 4) {
        // Stage a mid-drag frame at slot 0 (the regression case): the third
        // tag floats at the list top; the first slides down mid-flight.
        ui::WindowViewModel vm0 = BuildVm(state);
        int g0 = -1;
        for (int g = 0; g < static_cast<int>(vm0.sidebar.size()); ++g) {
            if (!vm0.sidebar[g].items.empty() && vm0.sidebar[g].items[0].is_tag) {
                g0 = g;
                break;
            }
        }
        D2D1_RECT_F a{}, b{};
        const bool okA = g0 >= 0 &&
            state.renderer.TagItemRect(vm0, static_cast<float>(state.compositor.Width()),
                                       static_cast<float>(state.compositor.Height()),
                                       g0, 0, &a);
        const bool okB = g0 >= 0 &&
            state.renderer.TagItemRect(vm0, static_cast<float>(state.compositor.Width()),
                                       static_cast<float>(state.compositor.Height()),
                                       g0, 1, &b);
        if (okA && okB) {
            const float pitch = b.top - a.top;
            state.tagDragActive = true;
            state.tagDragTag = 2;
            state.tagOrder.resize(state.places.tags.size());
            for (int i = 0; i < static_cast<int>(state.tagOrder.size()); ++i)
                state.tagOrder[static_cast<size_t>(i)] = i;
            state.tagOrder[0] = 2;
            state.tagOrder[1] = 0;
            state.tagOrder[2] = 1;
            state.tagDragY = a.top + pitch * 0.5f;
            state.tagOffsets[state.places.tags[0].name] = -0.5f;
            state.tagGapVisible = true;
            state.tagGapLineY = state.tagGapFrom = state.tagGapTo = a.top;
        }
    }
}

// No C++ objects with destructors here: SEH (__try/__except) forbids unwinding.
// Verification hook for --shot-tray: PULSE_SHOT_TRAY_ACTION=<action>:<ms>.
static void ApplyShotTrayAction(AppState& state) {
    wchar_t tray_action[64]{};
    if (state.shot_tray &&
        GetEnvironmentVariableW(L"PULSE_SHOT_TRAY_ACTION", tray_action, ARRAYSIZE(tray_action))) {
        // Verification hook: <action>:<ms> captures the card stack in the
        // middle of an animation (throw, back, drag:<dx>, dismiss, clear, hover).
        int ms = 0;
        if (wchar_t* colon = wcschr(tray_action, L':')) { ms = _wtoi(colon + 1); *colon = L'\0'; }
        for (int k = 0; k < 60; ++k) { TickTrayDeck(state); Sleep(16); }
        const auto top = TrayDeckEntries(state.tray, static_cast<size_t>(TrayStackTop(state)), 1);
        if (wcscmp(tray_action, L"throw") == 0) {
            ThrowTrayTop(state, 1.0f, 0.0f, 0.0f);
        } else if (wcscmp(tray_action, L"back") == 0) {
            TrayStepBack(state);
        } else if (wcscmp(tray_action, L"drag") == 0 && !top.empty()) {
            state.trayDrag.active = true;
            state.trayDrag.path = top.front().item->path;
            state.trayDrag.dx = static_cast<float>(ms);
            state.trayDrag.dy = 10.0f;
            ms = 400; // let the card settle into the held pose
        } else if (wcscmp(tray_action, L"dismiss") == 0 && !top.empty()) {
            MarkTrayExit(state, { top.front().item->path }, false);
            state.tray.RemoveItem(static_cast<size_t>(top.front().batch),
                                  static_cast<size_t>(top.front().sub));
        } else if (wcscmp(tray_action, L"clear") == 0) {
            std::vector<std::wstring> all;
            for (const auto& b : state.tray.batches())
                for (const auto& item : b.items) all.push_back(item.path);
            MarkTrayExit(state, all, true);
            state.tray.Clear();
        } else if (wcscmp(tray_action, L"hover") == 0) {
            state.hoverRegion = static_cast<int>(ui::HitTestResult::TrayCard);
            ms = std::max(ms, 400);
        }
        const ULONGLONG until = GetTickCount64() + static_cast<ULONGLONG>(std::max(0, ms));
        while (GetTickCount64() < until) { TickTrayDeck(state); Sleep(4); }
        TickTrayDeck(state);
    }
}

int ShotModeMain(AppState& state, HWND hwnd) {
    bool ok = false;
    __try {
        WaitForShotReady(state);
        StageTagShotStates(state);
        if (state.shot_details && state.pane && state.pane->ActiveTab() &&
            state.pane->ActiveTab()->snapshot &&
            !state.pane->ActiveTab()->snapshot->empty()) {
            // Select rows after enumeration so the panel has deterministic data.
            state.pane->ActiveTab()->SelectOnly(0);
            if (state.shot_details_multi) {
                const int count = static_cast<int>(state.pane->ActiveTab()->snapshot->size());
                for (int i = 1; i < std::min(3, count); ++i)
                    state.pane->ActiveTab()->ToggleSelect(i);
            }
        }
        MSG msg{};
        for (int i = 0; i < 20; ++i) {
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            Sleep(40);
        }
        ApplyShotTrayAction(state); // separate frame: __try forbids unwinding objects
        if (state.shot_tooltip) {
            // After the pump: a mouse move during startup would clear this.
            state.hoverRegion = static_cast<int>(ui::HitTestResult::SidebarItem);
            state.hoverLabel = L"Project";
            state.hoverPoint = POINT{ 60, 320 };
            state.tooltipText = state.hoverLabel;
        }
        Render(state);
        ok = state.compositor.SaveSnapshot(state.shot.output.c_str());
    } __except (pulse::crash::ReportFatal(GetExceptionInformation(), "shot")) {
        return 2;
    }
    if (ok) {
        // Drop timing numbers next to the shot for the phase report.
        wchar_t timingPath[MAX_PATH]{};
        wcsncpy_s(timingPath, state.shot.output.c_str(), _TRUNCATE);
        if (wchar_t* dot = wcsrchr(timingPath, L'.')) *dot = L'\0';
        wcscat_s(timingPath, L".timing.txt");
        if (HANDLE f = CreateFileW(timingPath, GENERIC_WRITE, 0, nullptr,
                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr); f != INVALID_HANDLE_VALUE) {
            char buf[256];
            int n = snprintf(buf, sizeof(buf),
                "path=%ls\nfirst_frame_ms=%.2f\nenum_sort_done_ms=%.2f\n",
                state.pane && state.pane->ActiveTab() ? state.pane->ActiveTab()->current_path.c_str() : L"",
                state.timing.first_frame_ms, state.timing.sort_done_ms);
            if (n > 0) {
                DWORD written = 0;
                WriteFile(f, buf, (DWORD)n, &written, nullptr);
            }
            CloseHandle(f);
        }
    }
    DestroyWindow(hwnd);
    MSG msg{};
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return ok ? 0 : 1;
}

bool SkipSingletonFromArgv() {
    for (int i = 1; i < __argc; ++i) {
        if (wcscmp(__wargv[i], L"--selftest") == 0 ||
            wcscmp(__wargv[i], L"--material-selftest") == 0 ||
            wcscmp(__wargv[i], L"--seed-shell-verbs") == 0 ||
            wcscmp(__wargv[i], L"--shot") == 0 ||
            wcscmp(__wargv[i], L"--menushot") == 0 ||
            wcscmp(__wargv[i], L"--test-instance") == 0 ||
            wcscmp(__wargv[i], L"--colorpickshot") == 0 ||
            wcscmp(__wargv[i], L"--colorpickdialog") == 0)
            return true;
    }
    return false;
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int nCmdShow) {
    pulse::crash::Initialize({pulse::crash::ProcessRole::App, false, {}});
    if (__argc > 1 && wcscmp(__wargv[1], L"--file-operation-worker") == 0)
        return ERROR_NOT_SUPPORTED;
    for (int i = 1; i < __argc; ++i) {
        if (wcscmp(__wargv[i], L"--restore-file-manager") == 0) {
            const auto result = app::RestoreDefaultFileManager(app::SystemExecutablePath());
            // A later third-party association is deliberately preserved on uninstall.
            return result.error == ERROR_SUCCESS ? 0 : 1;
        }
    }
    pulse::compat::EnableDpiAwareness();
    // OLE init (drag & drop + clipboard); implies STA COM init.
    OleInitialize(nullptr);
    std::thread known_folder_labels(pulse::path::InitializeKnownFolderLabels);
    known_folder_labels.join();
    for (int i = 1; i < __argc; ++i) {
        if (wcscmp(__wargv[i], L"--material-selftest") == 0) {
            const int rc = ui::RunMaterialSelfTest();
            OleUninitialize();
            return rc;
        }
        if (wcscmp(__wargv[i], L"--seed-shell-verbs") == 0) {
            // The cache stores display text, and the synthesized rows (打开方式…)
            // come from the resource strings: seeding without them wrote empty
            // rows that the menu then dropped.
            l10n::Initialize(hInstance, L"system");
            const int rc = app::SeedMachineStaticVerbCache() ? 0 : 1;
            OleUninitialize();
            return rc;
        }
    }

    AppState state;
    state.safeMode = pulse::crash::SafeModeRequested();
    for (int i = 1; i < __argc; ++i)
        if (wcscmp(__wargv[i], L"--test-instance") == 0) state.isolatedTest = true;
    if (state.isolatedTest) {
        wchar_t test_data[32768]{};
        const DWORD length = GetEnvironmentVariableW(L"PULSE_TEST_DATA_DIR", test_data, ARRAYSIZE(test_data));
        if (length && length < ARRAYSIZE(test_data))
            pulse::storage::OverrideDefaultRootForTesting(test_data);
    }
    for (int i = 1; i < __argc; ++i)
        if (state.isolatedTest && wcscmp(__wargv[i], L"--content-index-observer") == 0) state.contentIndexObserver = true;

#ifdef PULSE_WITH_SELFTEST
    // Optional headless suite: excluded from production builds together with
    // the in-process index engine it exercises.
    for (int i = 1; i < __argc; ++i) {
        if (wcscmp(__wargv[i], L"--selftest") == 0) {
            int rc = app::RunSelfTest1B2();
            OleUninitialize();
            return rc;
        }
    }
#endif

    if (!SkipSingletonFromArgv() &&
        state.single_instance.Acquire() == app::SingleInstanceCoordinator::AcquireResult::Primary) {
        std::wstring migration_error;
        // No window exists yet. Keep filesystem migration off the UI thread.
        std::thread migration([&] {
            pulse::storage::ApplyConfiguration(migration_error);
            std::wstring index_error;
            if (!pulse::storage::ApplyUserIndex(index_error) && migration_error.empty())
                migration_error = std::move(index_error);
        });
        migration.join();
        if (!migration_error.empty()) state.settings.SetError(std::move(migration_error));
    }

    // Only appearance and retained user data survive a launch. Navigation starts fresh.
    app::WindowSessionSnapshot session;
    if (!state.isolatedTest && app::LoadSessionPreferences(session)) {
        RestoreWindowPreferences(state, session);
        state.tray = session.tray;
    }

    bool startup_launch = false;
    // Parse command line.
    for (int i = 1; i < __argc; ++i) {
        if (wcscmp(__wargv[i], L"--startup") == 0) {
            startup_launch = true;
        } else if (wcscmp(__wargv[i], L"--shot") == 0 && i + 1 < __argc) {
            state.shot.active = true;
            state.shot.output = __wargv[++i];
        } else if (wcscmp(__wargv[i], L"--menushot") == 0 && i + 1 < __argc) {
            state.menushot = true;
            state.menushot_out = __wargv[++i];
        } else if (wcscmp(__wargv[i], L"--menushot-search") == 0) {
            state.menushot_search = true;
        } else if (wcscmp(__wargv[i], L"--colorpickshot") == 0 && i + 1 < __argc) {
            state.colorpickshot = true;
            state.colorpickshot_out = __wargv[++i];
            if (i + 1 < __argc && wcscmp(__wargv[i + 1], L"light") == 0) {
                state.colorpickshot_dark = false;
                ++i;
            }
        } else if (wcscmp(__wargv[i], L"--colorpickdialog") == 0) {
            state.colorpickdialog = true;
        } else if (wcscmp(__wargv[i], L"--shot-tray") == 0) {
            state.shot_tray = true;
            // Optional item count: --shot-tray 1 stages a single file.
            if (i + 1 < __argc && __wargv[i + 1][0] >= L'0' && __wargv[i + 1][0] <= L'9')
                state.shot_tray_count = std::max(1, _wtoi(__wargv[++i]));
        } else if (wcscmp(__wargv[i], L"--shot-tab-colors") == 0) {
            state.shot_tab_colors = true;
        } else if (wcscmp(__wargv[i], L"--shot-pinned-tab") == 0) {
            state.shot_pinned_tab = true;
        } else if (wcscmp(__wargv[i], L"--shot-tag-rename") == 0) {
            state.shot_tag_rename = true;
        } else if (wcscmp(__wargv[i], L"--shot-tag-drag") == 0) {
            state.shot_tag_drag = true;
        } else if (wcscmp(__wargv[i], L"--shot-rail") == 0) {
            // GUI verification for the icon rail: every section folded, so a
            // narrow window shows one row per section.
            state.sidebarCollapsedMask = (1u << app::kSidebarSectionCount) - 1u;
        } else if (wcscmp(__wargv[i], L"--shot-tooltip") == 0) {
            // GUI verification for a rail hint; staged right before the shot so
            // the message pump cannot clear it (see ShotModeMain).
            state.shot_tooltip = true;
            state.hoverRegion = static_cast<int>(ui::HitTestResult::SidebarItem);
            state.hoverLabel = L"Project";
            state.hoverPoint = POINT{ 60, 320 };
            state.tooltipText = state.hoverLabel;
        } else if (wcscmp(__wargv[i], L"--shot-details") == 0) {
            state.shot_details = true;
        } else if (wcscmp(__wargv[i], L"--shot-details-multi") == 0) {
            state.shot_details = true;
            state.shot_details_multi = true;
        } else if (wcscmp(__wargv[i], L"--shot-scale") == 0 && i + 1 < __argc) {
            state.shot_scale_override = std::clamp(
                static_cast<float>(_wtof(__wargv[++i])), 1.0f, 2.5f);
        } else if (wcscmp(__wargv[i], L"--shot-language") == 0 && i + 1 < __argc) {
            state.shot.language = __wargv[++i];
        } else if (wcscmp(__wargv[i], L"--shot-high-contrast") == 0) {
            state.shot_high_contrast = true;
        } else if (wcscmp(__wargv[i], L"--dark") == 0) {
            state.shot.force_dark = true;
            state.themeOverride = ui::ThemeMode::Dark;
        } else if (wcscmp(__wargv[i], L"--light") == 0) {
            state.shot.force_dark = false;
            state.themeOverride = ui::ThemeMode::Light;
        } else if (wcscmp(__wargv[i], L"--test-instance") == 0 || wcscmp(__wargv[i], L"--content-index-observer") == 0) {
            continue;
        } else if (wcscmp(__wargv[i], L"--fps") == 0) {
            state.forceStatusPerformance = true;
            state.showFps = true;
        } else if (wcscmp(__wargv[i], L"--view") == 0 && i + 1 < __argc) {
            state.shot.view_mode = ui::ParseViewMode(__wargv[++i]);
        } else if (wcscmp(__wargv[i], L"--shot-sort-date") == 0) {
            state.shot.sort_date = true;
        } else if (wcscmp(__wargv[i], L"--size") == 0 && i + 1 < __argc) {
            int requestedWidth = 0;
            int requestedHeight = 0;
            if (swscanf_s(__wargv[++i], L"%dx%d", &requestedWidth, &requestedHeight) == 2) {
                state.shot.width = std::max(320, requestedWidth);
                state.shot.height = std::max(240, requestedHeight);
            }
        } else if (i == __argc - 1) {
            state.shot.path = __wargv[i];
        } else if (__wargv[i][0] != L'-' && state.shot.path.empty()) {
            state.shot.path = __wargv[i]; // tolerate path not being the last argument
        }
    }
    if (state.shot.active && state.shot.path.empty()) {
        state.shot.path = pulse::path::DefaultNavigationRoot();
    }
    if (!state.shot.active && !state.menushot && !state.colorpickshot &&
        !state.colorpickdialog) {
        state.open_path = state.shot.path;
        state.shot.path.clear();
    }
    state.shot.start = std::chrono::steady_clock::now();

    if (!SkipSingletonFromArgv()) {
        const auto result = state.single_instance.Acquire();
        if (result == app::SingleInstanceCoordinator::AcquireResult::Existing) {
            if (!startup_launch || !state.open_path.empty())
                state.single_instance.ForwardOpenPath(state.open_path);
            OleUninitialize();
            return 0;
        }
    }

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_PULSE));
    wc.hIconSm = reinterpret_cast<HICON>(LoadImageW(
        hInstance, MAKEINTRESOURCEW(IDI_PULSE), IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = app::SingleInstanceCoordinator::WindowClassName();
    RegisterClassExW(&wc);

    int x = CW_USEDEFAULT, y = CW_USEDEFAULT, w = (int)(1600 * state.scale), h = (int)(960 * state.scale);
    if (session.window_rect.right > session.window_rect.left) {
        x = session.window_rect.left;
        y = session.window_rect.top;
        w = session.window_rect.right - session.window_rect.left;
        h = session.window_rect.bottom - session.window_rect.top;
    }
    if (state.shot.active && state.shot.width > 0 && state.shot.height > 0) {
        w = state.shot.width;
        h = state.shot.height;
    }

    HWND hwnd = CreateWindowExW(
        WS_EX_NOREDIRECTIONBITMAP,
        wc.lpszClassName,
        L"Pulse",
        // Keep an overlapped window for the native minimize/restore/open
        // transitions. WM_NCCALCSIZE and WM_NCPAINT still leave the caption to
        // Pulse's renderer.
        WS_OVERLAPPEDWINDOW,
        x, y, w, h,
        nullptr, nullptr, hInstance, &state);

    if (!hwnd) return 1;
    if (wc.hIcon) SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(wc.hIcon));
    if (wc.hIconSm) SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(wc.hIconSm));

    if (session.maximized) nCmdShow = SW_SHOWMAXIMIZED;
    wchar_t hidden_shot[4]{};
    const bool test_hidden = (state.shot.active || state.menushot || state.colorpickshot) &&
        GetEnvironmentVariableW(L"PULSE_TEST_HIDDEN_SHOT", hidden_shot, ARRAYSIZE(hidden_shot)) == 1 &&
        hidden_shot[0] == L'1';
    const bool startup_hidden = startup_launch && state.appPrefs.start_to_tray &&
        state.open_path.empty() && state.tray_controller.StartHidden(session.maximized);
    if (!test_hidden && !startup_hidden && !state.shot.active)
        state.compositor.QueueShowTransition();
    ShowWindow(hwnd, test_hidden || startup_hidden ? SW_HIDE : state.shot.active ? SW_SHOWNORMAL : nCmdShow);
    UpdateWindow(hwnd);

    if (state.menushot) {
        // Render the built-in item menu to a PNG (GUI verification for 1B-2).
        ui::FluentMenu m;
        bool ok = m.Create(hwnd, &state.compositor, state.scale);
        if (ok && state.menushot_search && state.pane && state.pane->ActiveTab()) {
            // BuildFinderItemMenu only checks current_path for the search bit.
            state.pane->ActiveTab()->current_path = L"pulse:search:widgets.json";
        }
        if (ok) {
            m.SetTheme(state.darkMode, state.accentColor);
            auto debug_items = BuildFinderItemMenu(state, true, L"撤销移动 a.txt");
            wchar_t quick_menu[4]{};
            if (GetEnvironmentVariableW(L"PULSE_TEST_QUICK_MENU", quick_menu, ARRAYSIZE(quick_menu)) == 1 &&
                quick_menu[0] == L'1') {
                debug_items = app::BuildBackgroundMenu(false, true, L"撤销移动 a.txt");
                app::AppendBackgroundViewCommands(debug_items, {});
                AppendQuickAccessCommand(state, debug_items,
                    state.shot.path.empty() ? QuickAccessTargets(ActiveTab(state), true)
                                            : std::vector<std::wstring>{state.shot.path});
            }
            for (auto& item : debug_items) {
                if (item.quick_swatches.empty()) continue;
                item.quick_swatches.front().checked = true;
                if (item.quick_swatches.size() > 1) item.quick_swatches[1].mixed = true;
                break;
            }
            // Sample Explorer section: a flat verb plus a software-owned
            // flyout group (renders the › chevron in the shortcut column).
            app::ShellMenuEntry group;
            group.text = L"Bandizip";
            group.children = { { app::CmdShellComBase + 1, L"压缩为 zip", true },
                               { app::CmdShellComBase + 2, L"用 Bandizip 打开", true } };
            app::AppendShellSection(debug_items,
                { { app::CmdShellStaticBase + 0, L"打印", true }, group });
            ok = m.SaveDebugSnapshot(state.menushot_out.c_str(), std::move(debug_items));
        }
        DestroyWindow(hwnd);
        OleUninitialize();
        return ok ? 0 : 1;
    }

    if (state.colorpickshot) {
        // Render the color picker popup to a PNG (GUI verification).
        const bool ok = ui::ColorPickerPopup::SaveDebugSnapshot(
            &state.compositor, state.colorpickshot_out.c_str(),
            state.colorpickshot_dark, state.accentColor, state.scale);
        DestroyWindow(hwnd);
        OleUninitialize();
        return ok ? 0 : 1;
    }

    if (state.colorpickdialog) {
        // Interactive smoke test: open the picker centered on the work area.
        uint32_t rgb = 0x2FDFF1;
        RECT work{};
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
        POINT pt{ (work.left + work.right) / 2, (work.top + work.bottom) / 2 };
        ui::ColorPickerPopup::Pick(hwnd, &state.compositor, nullptr, state.scale, pt,
                                   rgb, state.darkMode, rgb);
        DestroyWindow(hwnd);
        OleUninitialize();
        return 0;
    }

    if (state.shot.active) {
        wchar_t settings_fixture[32]{};
        if (state.isolatedTest && GetEnvironmentVariableW(L"PULSE_TEST_HOME_COLLAPSED", settings_fixture, ARRAYSIZE(settings_fixture))) {
            if (auto* tab = ActiveTab(state)) tab->home_collapsed_mask = static_cast<unsigned>(wcstoul(settings_fixture, nullptr, 10)) & 7u;
        }
        if (state.isolatedTest && GetEnvironmentVariableW(L"PULSE_TEST_SETTINGS_EXPANDED",settings_fixture,ARRAYSIZE(settings_fixture)))
            state.settingsExpanded=static_cast<unsigned>(wcstoul(settings_fixture,nullptr,10)) & 0x1f03u;
        if (state.isolatedTest && GetEnvironmentVariableW(L"PULSE_TEST_SETTINGS_SCROLL",settings_fixture,ARRAYSIZE(settings_fixture))) {
            auto vm=BuildVm(state,false);
            state.settings.SetScroll(static_cast<float>(_wtof(settings_fixture))*state.scale,
                state.renderer.SettingsMaxScroll(vm,static_cast<float>(state.compositor.Width()),static_cast<float>(state.compositor.Height())));
        }
        wchar_t panel_shot[32768]{};
        if (state.isolatedTest && GetEnvironmentVariableW(L"PULSE_TEST_OPTIONS_SHOT", panel_shot, ARRAYSIZE(panel_shot))) {
            ShowAddressSearch(state);
            ShowSearchOptions(state);
        }
        if (state.isolatedTest && GetEnvironmentVariableW(L"PULSE_TEST_HISTORY_SHOT", panel_shot, ARRAYSIZE(panel_shot))) {
            state.searchHistory.Record(L"setup-dia", app::MakeSearchPath(L"setup-dia"));
            state.searchHistory.Record(L"项目预算", app::MakeSearchPath(L"content:项目预算"));
            ShowAddressSearch(state);
            ShowAddressSearchHistory(state);
        }
#ifdef PULSE_WITH_SELFTEST
        wchar_t settings_interactions[32768]{};
        if (state.isolatedTest && GetEnvironmentVariableW(L"PULSE_TEST_SETTINGS_INTERACTIONS",settings_interactions,ARRAYSIZE(settings_interactions))) {
            extern int RunSettingsInteractionTest(AppState&,const wchar_t*);
            const int result=RunSettingsInteractionTest(state,settings_interactions);
            DestroyWindow(hwnd);OleUninitialize();return result;
        }
        wchar_t settings_flow[32768]{};
        if (state.isolatedTest && GetEnvironmentVariableW(L"PULSE_TEST_SETTINGS_FLOW",settings_flow,ARRAYSIZE(settings_flow))) {
            extern int RunSettingsFlowTest(AppState&,const wchar_t*);
            const int result=RunSettingsFlowTest(state,settings_flow);
            DestroyWindow(hwnd);OleUninitialize();return result;
        }
        wchar_t flow_output[32768]{};
        if (state.isolatedTest && GetEnvironmentVariableW(L"PULSE_TEST_SEARCH_FLOW", flow_output, ARRAYSIZE(flow_output))) {
            extern int RunSearchFlowTest(AppState&, const wchar_t*);
            const int result = RunSearchFlowTest(state, flow_output);
            DestroyWindow(hwnd);
            OleUninitialize();
            return result;
        }
#endif
        if (state.shot_tray) {
            // Stage a few real files from the shot folder so the deck is
            // visible in the verification screenshot. Drop any restored
            // session tray first so the shot is deterministic.
            state.tray.Clear();
            std::vector<std::wstring> staged;
            const std::wstring root = fs::NormalizePath(state.shot.path);
            WIN32_FIND_DATAW fd{};
            HANDLE hFind = FindFirstFileW((root + L"\\*").c_str(), &fd);
            if (hFind != INVALID_HANDLE_VALUE) {
                do {
                    if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) continue;
                    if (fd.cFileName[0] == L'.') continue;
                    staged.push_back(root + L"\\" + fd.cFileName);
                    if (staged.size() >= static_cast<size_t>(state.shot_tray_count)) break;
                } while (FindNextFileW(hFind, &fd));
                FindClose(hFind);
            }
            if (!staged.empty()) state.tray.Collect(staged, false);
        }
        if (state.shot_tab_colors && state.pane) {
            NewTab(state, state.shot.path);
            NewTab(state, state.shot.path);
            if (state.window_tabs.items.size() >= 3) {
                // Group 1: tabs 0-1, red, named. Group 2: tab 2, blue, unnamed.
                app::TabGroup g1; g1.id = 1; g1.name = L"设计"; g1.color_rgb = 0xE74856;
                app::TabGroup g2; g2.id = 2; g2.color_rgb = 0x0078D4;
                state.window_tabs.tab_groups.push_back(g1);
                state.window_tabs.tab_groups.push_back(g2);
                state.window_tabs.next_tab_group_id = 3;
                state.window_tabs.items[0]->tab_group = 1;
                state.window_tabs.items[1]->tab_group = 1;
                state.window_tabs.items[2]->tab_group = 2;
                SwitchTab(state, 1); // active grouped tab in the middle
            }
        }
        if (state.shot_pinned_tab && !state.window_tabs.items.empty()) {
            state.window_tabs.items[state.window_tabs.active]->pinned = true;
            state.window_tabs.items[state.window_tabs.active]->title = L"工作";
            state.window_tabs.items[state.window_tabs.active]->marker_rgb = 0x0078D4;
        }
        if (state.shot_details) {
            state.showDetailsPanel = true;
            state.renderer.SetDetailsPanelVisible(true);
        }
        int rc = ShotModeMain(state, hwnd);
        OleUninitialize();
        return rc;
    }

    MSG msg{};
    BOOL ret;
    while ((ret = GetMessageW(&msg, nullptr, 0, 0)) != 0) {
        if (ret == -1) break;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
        // Retired child states survive reentrant DestroyWindow calls until the
        // top-level dispatch has returned and its callbacks can no longer use them.
        state.core.retired_windows.clear();
    }
    OleUninitialize();
    return (int)msg.wParam;
}
