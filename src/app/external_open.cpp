// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "external_open.h"
#include "archive_navigation.h"
#include "app_internal.h"
#include "app_navigation.h"
#include "app_runtime.h"
#include "default_file_manager.h"
#include "system_integration.h"
#include "../fs/drive_kind.h"
#include "../fs/fs_enum.h"
#include "../ops/archive.h"
#include <algorithm>
#include <windows.h>
#include <new>

namespace pulse {
namespace {

std::wstring_view StripOpenQuotes(std::wstring_view path) {
    while (!path.empty() && path.front() == L'"') path.remove_prefix(1);
    while (!path.empty() && path.back() == L'"') path.remove_suffix(1);
    return path;
}

app::WorkerPool::IoLane ExternalOpenLane(const std::wstring& raw) {
    using Lane = app::WorkerPool::IoLane;
    if (app::IsThisPcArgument(raw)) return Lane::Local;
    const auto path = fs::NormalizePath(std::wstring(StripOpenQuotes(raw)));
    const wchar_t letter = fs::DriveLetterOf(path);
    if (!letter) return Lane::Provider;
    // Inspect the DOS mapping without opening or querying the volume itself.
    const wchar_t device[] = {letter, L':', L'\0'};
    wchar_t target[512]{};
    return QueryDosDeviceW(device, target, ARRAYSIZE(target)) &&
        std::wstring_view(target).starts_with(L"\\Device\\HarddiskVolume")
        ? Lane::Local : Lane::Provider;
}

} // namespace

ResolvedOpenTarget ResolveOpenTarget(std::wstring path) {
    ResolvedOpenTarget target;
    if (app::IsThisPcArgument(path)) {
        target.this_pc = true;
        return target;
    }
    path = StripOpenQuotes(path);
    if (path.empty()) return target;

    const DWORD attrs = GetFileAttributesW(path.c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        if (ops::IsArchivePath(path)) {
            target.folder = ArchiveViewPath(fs::NormalizePath(path));
            return target;
        }
        target.file_path = fs::NormalizePath(path);
        const size_t slash = target.file_path.find_last_of(L"\\/");
        target.file_leaf = slash == std::wstring::npos
            ? target.file_path : target.file_path.substr(slash + 1);
        target.folder = fs::ParentPath(target.file_path);
    } else {
        target.folder = fs::NormalizePath(path);
    }
    return target;
}

AppState* PostOpenFolderInNewWindow(AppState& s, const std::wstring& raw) {
    return PostOpenFolderInNewWindow(s, ResolveOpenTarget(raw));
}

AppState* PostOpenFolderInNewWindow(AppState& s, const ResolvedOpenTarget& target) {
    HWND hwnd = s.core.windows.MostRecentHwnd();
    if (!hwnd) hwnd = s.hwnd;
    if (!IsWindow(hwnd)) return nullptr;
    std::unique_ptr<ResolvedOpenTarget> open_target(
        new (std::nothrow) ResolvedOpenTarget(target));
    if (!open_target) return nullptr;
    auto* created = reinterpret_cast<AppState*>(SendMessageW(
        hwnd, WM_NEW_WINDOW, 0, reinterpret_cast<LPARAM>(open_target.get())));
    // WM_NEW_WINDOW consumes the payload before SendMessage returns.
    open_target.release();
    return created;
}

namespace {

bool QueueOpenResolution(AppState& s, const std::wstring& raw, app::ExternalOpenResult result) {
    if (raw.empty() || s.exit_requested || s.core.exiting || !s.external_open_mailbox) return false;
    const uint64_t sequence = s.external_open_issued_sequence + 1;
    result.sequence = sequence;
    const auto mailbox = s.external_open_mailbox;
    if (!s.worker.EnqueueIo([raw, result = std::move(result), mailbox] () mutable {
        try {
            result.target = ResolveOpenTarget(raw);
            result.resolution_complete = true;
        } catch (...) {}
        mailbox->Publish(std::move(result));
    }, {}, ExternalOpenLane(raw))) return false;
    s.external_open_issued_sequence = sequence;
    return true;
}

void ApplyResolvedStartupOpen(AppState& s, const app::ExternalOpenResult& request) {
    if (s.exit_requested || s.core.exiting) return;
    app::Tab* tab = nullptr;
    ForEachPane(s, [&](app::Pane& pane) {
        if (pane.view.network_identity == request.startup_tab_identity &&
            pane.view.navigation_revision == request.startup_navigation_revision)
            tab = &pane.view;
    });
    if (!tab) return;
    StartLoadingPath(s, *tab, request.resolution_complete ? request.target.folder : app::MakeHomePath(),
        PathLoadReason::Navigate);
    if (!request.target.file_leaf.empty())
        SelectLaunchedFile(s, request.target, tab);
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

AppState* ApplyResolvedExternalOpen(AppState& s, const app::ExternalOpenResult& request) {
    if (!request.resolution_complete) return nullptr;
    const auto& target = request.target;
    switch (request.mode) {
    case app::ExternalOpenMode::NewWindow:
        return PostOpenFolderInNewWindow(s, target);
    case app::ExternalOpenMode::ReuseOpenedLocation: {
        AppState* match = nullptr;
        s.core.windows.ForEach([&](HWND, AppState& candidate) {
            if (match) return;
            if (!target.folder.empty()) {
                if (ActivateExistingFolderTab(candidate, target.folder)) match = &candidate;
                return;
            }
            for (size_t i = 0; i < candidate.window_tabs.items.size() && !match; ++i) {
                auto& layout = *candidate.window_tabs.items[i];
                for (auto& pane : layout.panes) {
                    const app::Tab* tab = pane ? pane->ActiveTab() : nullptr;
                    if (!tab || !tab->current_path.empty()) continue;
                    SwitchTab(candidate, i);
                    FocusPane(candidate, pane.get());
                    match = &candidate;
                    break;
                }
            }
        });
        if (match) {
            ShowWindow(match->hwnd, SW_RESTORE);
            SetForegroundWindow(match->hwnd);
            SelectLaunchedFile(*match, target);
            InvalidateRect(match->hwnd, nullptr, FALSE);
            return match;
        }
        break;
    }
    case app::ExternalOpenMode::Tab:
        break;
    }
    AppState* destination = s.core.windows.MostRecent();
    AppState& target_window = destination ? *destination : s;
    if (request.mode == app::ExternalOpenMode::Tab) {
        RestoreAppWindow(target_window);
        if (target.this_pc) OpenTabAt(target_window, L"");
        else if (!target.folder.empty()) NewTab(target_window, target.folder);
        else return nullptr;
        if (!target.folder.empty()) SelectLaunchedFile(target_window, target);
        return &target_window;
    }
    OpenFolderInNewTab(target_window, target);
    return &target_window;
}
}

bool RouteStartupOpen(AppState& s, const app::Tab& tab) {
    app::ExternalOpenResult result;
    result.startup_tab_identity = tab.network_identity;
    result.startup_navigation_revision = tab.navigation_revision;
    return QueueOpenResolution(s, s.open_path, std::move(result));
}

bool RouteExternalFolderOpen(AppState& s, const std::wstring& raw,
                             uint64_t takeover_token,
                             std::vector<std::wstring> selected_paths,
                             unsigned selection_flags) {
    app::ExternalOpenResult result;
    result.mode = s.appPrefs.external_open_mode;
    result.takeover_token = takeover_token;
    result.selected_paths = std::move(selected_paths);
    result.selection_flags = selection_flags;
    return QueueOpenResolution(s, raw, std::move(result));
}

void ApplyExternalOpenResults(AppState& s, WPARAM identity) {
    if (!s.external_open_mailbox) return;
    auto ready = s.external_open_mailbox->Take(identity);
    // Preserve order among ready requests without waiting for a blocked probe.
    std::ranges::sort(ready, {}, &app::ExternalOpenResult::sequence);
    for (const auto& result : ready) {
        if (result.startup_tab_identity) {
            ApplyResolvedStartupOpen(s, result);
            continue;
        }
        if (result.takeover_token && !s.systemIntegration.IsExternalOpenActive(result.takeover_token)) {
            s.systemIntegration.CompleteExternalOpen(s, nullptr, result, false);
            continue;
        }
        AppState* destination = ApplyResolvedExternalOpen(s, result);
        if (result.takeover_token)
            s.systemIntegration.CompleteExternalOpen(s, destination, result,
                destination != nullptr);
    }
}

} // namespace pulse
