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
#include "../common/path_utils.h"
#include "../fs/fs_enum.h"
#include "../ops/archive.h"
#include <windows.h>
#include <new>

namespace pulse {

ResolvedOpenTarget ResolveOpenTarget(std::wstring path) {
    ResolvedOpenTarget target;
    if (app::IsThisPcArgument(path)) {
        target.this_pc = true;
        return target;
    }
    while (!path.empty() && (path.front() == L'"' || path.back() == L'"')) {
        if (path.front() == L'"') path.erase(path.begin());
        if (!path.empty() && path.back() == L'"') path.pop_back();
    }
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
    if (!hwnd || !IsWindow(hwnd)) return nullptr;
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

bool RouteExternalFolderOpen(AppState& s, const std::wstring& raw,
                             uint64_t takeover_token,
                             std::vector<std::wstring> selected_paths,
                             unsigned selection_flags) {
    if (raw.empty() || s.exit_requested || s.core.exiting || !s.external_open_mailbox) return false;
    const auto mode = s.appPrefs.external_open_mode;
    const uint64_t sequence = s.external_open_issued_sequence + 1;
    const auto mailbox = s.external_open_mailbox;
    if (!s.worker.EnqueueIo([raw, mode, sequence, mailbox, takeover_token,
                             selected_paths = std::move(selected_paths), selection_flags] () mutable {
        app::ExternalOpenResult result;
        result.sequence = sequence;
        result.mode = mode;
        result.takeover_token = takeover_token;
        result.selected_paths = std::move(selected_paths);
        result.selection_flags = selection_flags;
        try {
            result.target = ResolveOpenTarget(raw);
            result.resolution_complete = true;
        } catch (...) {}
        mailbox->Publish(std::move(result));
    })) return false;
    s.external_open_issued_sequence = sequence;
    return true;
}

void ApplyExternalOpenResults(AppState& s, WPARAM identity) {
    if (!s.external_open_mailbox) return;
    for (auto& result : s.external_open_mailbox->Take(identity))
        s.external_open_pending.insert_or_assign(result.sequence, std::move(result));
    for (;;) {
        auto next = s.external_open_pending.find(s.external_open_next_apply_sequence);
        if (next == s.external_open_pending.end()) break;
        app::ExternalOpenResult result = std::move(next->second);
        s.external_open_pending.erase(next);
        ++s.external_open_next_apply_sequence;
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
