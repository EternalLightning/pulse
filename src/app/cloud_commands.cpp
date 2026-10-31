// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "cloud_commands.h"
#include "cloud_refresh_paths.h"
#include "cloud_root_catalog.h"
#include "cloud_sync_probe.h"
#include "app_commands.h"
#include "app_navigation.h"
#include "archive_navigation.h"
#include "../fs/cloud_state.h"
#include "../common/localization.h"
#include <thread>

namespace pulse {
namespace {
void RefreshCloudPaths(AppState& s, const std::vector<std::wstring>& paths) {
    for (const auto& parent : app::CloudRefreshParents(paths)) {
        s.store.MarkDirty(parent);
        RefreshPath(s, parent);
    }
}

bool CloudSelection(const app::Tab& tab) {
    if (tab.net_readonly || IsArchiveView(tab.current_path) || tab.content_results || !tab.snapshot || !tab.SelectedCount()) return false;
    for (const int index : tab.SelectedIndices()) {
        if (index < 0 || index >= static_cast<int>(tab.EntryCount())) return false;
        const auto& entry = tab.EntryAt(static_cast<size_t>(index));
        if (entry.change_record_only || !entry.link_target.empty() || !app::IsUnderCloudFilesRoot(EntryFullPath(tab, index))) return false;
    }
    return true;
}

void RunPin(AppState& s, fs::CloudPinAction action) {
    const auto* tab = ActiveTab(s);
    if (!tab || !CloudSelection(*tab)) return;
    std::vector<std::wstring> paths;
    for (const int index : tab->SelectedIndices()) paths.push_back(EntryFullPath(*tab, index));
    if (s.cloud_pin_cancel) s.cloud_pin_cancel->store(true);
    auto cancel = std::make_shared<std::atomic<bool>>(false);
    s.cloud_pin_cancel = cancel;
    if (!s.cloud_pin_mailbox)
        s.cloud_pin_mailbox = std::make_shared<app::WindowResultMailbox<app::CloudPinCompletion>>(s.hwnd, WM_CLOUD_PIN_DONE);
    const auto mailbox = s.cloud_pin_mailbox;
    try {
        std::thread([paths = std::move(paths), action, cancel, mailbox] {
            app::CloudPinCompletion completion;
            completion.paths = paths;
            try { completion.result = fs::ApplyCloudPin(paths, action, cancel.get()); }
            catch (...) { completion.result.failed = paths.size(); }
            mailbox->Publish(std::move(completion));
        }).detach();
    } catch (...) {
        s.notification_toast.ShowError(s.hwnd, l10n::Get(l10n::StringId::CloudPinFailed), {});
    }
}
}

bool HandleCloudCommand(AppState& s, int command) {
    if (command == app::CmdCloudKeep || command == app::CmdCloudFree) {
        RunPin(s, command == app::CmdCloudKeep ? fs::CloudPinAction::Keep : fs::CloudPinAction::Free);
        return true;
    }
    if (command == app::CmdCloudCancelDownload) { s.cloud_preview_intent.reset(); s.cloud_downloads.Cancel(); return true; }
    if (command != app::CmdCloudDownload) return false;
    const auto* tab = ActiveTab(s);
    if (!tab || !tab->snapshot || tab->SelectedCount() != 1 || IsArchiveView(tab->current_path) || tab->content_results) return true;
    const auto selected = tab->SelectedIndices();
    if (selected.empty()) return true;
    const auto& entry = tab->EntryAt(static_cast<size_t>(selected.front()));
    if (entry.is_dir || entry.change_record_only || !entry.link_target.empty() || !fs::IsCloudPlaceholder(entry.attrs)) return true;
    const auto path = EntryFullPath(*tab, selected.front());
    s.cloud_preview_intent = app::CloudPreviewIntent{tab->network_identity, tab->view_generation,
        tab->selection_revision, tab->current_path, path};
    if (!s.cloud_downloads.Start(path, entry.size, s.hwnd, WM_CLOUD_DOWNLOAD)) {
        s.cloud_preview_intent.reset();
        s.notification_toast.ShowError(s.hwnd, l10n::Get(l10n::StringId::CloudDownloadFailed), {});
    }
    InvalidateRect(s.hwnd, nullptr, FALSE);
    return true;
}

void AppendCloudCommands(const AppState& s, const app::Tab& tab, std::vector<ui::FluentMenuItem>& items) {
    const auto add = [&](int command, l10n::StringId label, const wchar_t* glyph) {
        ui::FluentMenuItem item; item.command = command; item.text = l10n::Get(label); item.glyph = glyph;
        items.push_back(std::move(item));
    };
    if (CloudSelection(tab)) {
        add(app::CmdCloudKeep, l10n::StringId::CloudAlwaysKeep, L"\xE73E");
        add(app::CmdCloudFree, l10n::StringId::CloudFreeSpace, L"\xE753");
    }
    if (tab.snapshot && tab.SelectedCount() == 1 && !IsArchiveView(tab.current_path) && !tab.content_results) {
        const auto selected = tab.SelectedIndices();
        if (!selected.empty()) {
            const auto& entry = tab.EntryAt(static_cast<size_t>(selected.front()));
            if (!entry.is_dir && !entry.change_record_only && entry.link_target.empty() && fs::IsCloudPlaceholder(entry.attrs)) {
                const bool downloading = s.cloud_downloads.Percent(EntryFullPath(tab, selected.front())) >= 0;
                add(downloading ? app::CmdCloudCancelDownload : app::CmdCloudDownload,
                    downloading ? l10n::StringId::CloudDownloadCancel : l10n::StringId::CloudDownloadPreview, L"\xE896");
            }
        }
    }
}

void HandleCloudDownloadResults(AppState& s, WPARAM identity) {
    for (const auto& result : s.cloud_downloads.Take(identity)) {
        if (result.event == app::CloudDownloadEvent::Done) {
            const auto* tab = ActiveTab(s);
            const auto intent = s.cloud_preview_intent;
            ui::QuickPreviewItem item;
            const bool preview_current = intent && tab && tab->network_identity == intent->tab &&
                tab->view_generation == intent->generation && tab->selection_revision == intent->selection &&
                tab->current_path == intent->folder && SelectedQuickPreviewItem(s, item) &&
                item.path == intent->path && item.path == result.path;
            RefreshCloudPaths(s, {result.path});
            s.renderer.SharedThumbnailCache()->Evict();
            if (preview_current && result.attrs != INVALID_FILE_ATTRIBUTES &&
                !fs::IsCloudPlaceholder(result.attrs) && !(result.attrs & FILE_ATTRIBUTE_DIRECTORY)) {
                item.attrs = result.attrs; item.size = result.size; item.modified = result.modified;
                if (s.quickPreview.visible()) s.quickPreview.Update(item);
                else if (!s.showDetailsPanel || !s.detailsPreviewEnabled) {
                    const auto effect = s.appPrefs.background_image.empty()
                        ? ui::WindowEffectFromId(s.appPrefs.window_effect) : ui::WindowEffect::None;
                    s.quickPreview.Show(item, s.darkMode, effect, s.safeMode);
                }
            }
            s.cloud_preview_intent.reset();
        } else if (result.event == app::CloudDownloadEvent::Failed) {
            s.cloud_preview_intent.reset();
            s.notification_toast.ShowError(s.hwnd, l10n::Get(l10n::StringId::CloudDownloadFailed),
                std::to_wstring(result.error));
        } else if (result.event == app::CloudDownloadEvent::Cancelled) {
            s.cloud_preview_intent.reset();
        }
    }
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

void HandleCloudPinResults(AppState& s, WPARAM identity) {
    if (!s.cloud_pin_mailbox) return;
    for (const auto& completion : s.cloud_pin_mailbox->Take(identity)) {
        RefreshCloudPaths(s, completion.paths);
        if (completion.result.failed)
            s.notification_toast.ShowError(s.hwnd, l10n::Get(l10n::StringId::CloudPinFailed),
                std::to_wstring(completion.result.failed));
    }
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

bool TickCloudStatus(AppState& s) {
    bool dirty = false;
    const uint64_t roots_revision = app::CloudRootCatalogRevision();
    if (s.cloud_roots_revision != roots_revision) {
        s.cloud_roots_revision = roots_revision;
        if (const auto roots = app::CloudRootCatalog()) {
            s.sidebar.cloud.clear();
            for (const auto& root : *roots) {
                app::SidebarEntry entry;
                entry.path = root.path; entry.label = root.label;
                entry.glyph = L"\xE753"; entry.fallback = L"Cloud";
                entry.color = ui::HexColor(app::CloudRootColor(root));
                s.sidebar.cloud.push_back(std::move(entry));
            }
        }
        dirty = true;
    }
    const uint64_t sync_revision = app::CloudSyncProbe::Instance().Revision();
    if (s.cloud_sync_revision != sync_revision) {
        s.cloud_sync_revision = sync_revision;
        dirty = true;
    }
    const ULONGLONG now = GetTickCount64();
    if (now - s.cloud_status_tick >= app::CloudSyncProbe::kSyncingRecheckMs) {
        s.cloud_status_tick = now;
        if (const auto* layout = s.window_tabs.Active())
            for (const auto& pane : layout->panes)
                if (const auto* tab = pane->ActiveTab(); tab && app::IsUnderCloudFilesRoot(tab->current_path)) {
                    dirty = true; break;
                }
    }
    return dirty;
}

void CloseCloudTasks(AppState& s) {
    s.cloud_downloads.Close();
    if (s.cloud_pin_cancel) s.cloud_pin_cancel->store(true);
    if (s.cloud_pin_mailbox) s.cloud_pin_mailbox->Close();
}
} // namespace pulse
