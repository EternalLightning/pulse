// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "duplicate_scan.h"
#include "app_internal.h"
#include "../fs/fs_enum.h"
#include "../common/localization.h"

#include <thread>

namespace pulse {

void PersistDuplicateScanPrefs(AppState& s) {
    s.appPrefs.duplicate_scan_scope = static_cast<int>(s.duplicateScan.scope);
    s.appPrefs.duplicate_scan_folder = s.duplicateScan.folder_path;
    s.appPrefs.duplicate_scan_drive = s.duplicateScan.drive_root;
    s.appPrefs.Save();
}

void RequestDuplicateVolumeCache(AppState& s, bool force) {
    if (!s.hwnd || !s.dup_volumes_mailbox || s.dup_volume_cache_pending) return;
    if (!force && s.dup_volume_cache_tick != 0 &&
        GetTickCount64() - s.dup_volume_cache_tick <= 2000)
        return;
    s.dup_volume_cache_pending = true;
    const auto mailbox = s.dup_volumes_mailbox;
    std::thread([mailbox] {
        index::IndexConfig config;
        mailbox->Publish(index::EnumerateLocalVolumes(config));
    }).detach();
}

void ApplyDuplicateVolumeCache(AppState& s, std::vector<index::VolumeInfo> volumes) {
    s.dup_volume_cache = std::move(volumes);
    s.dup_volume_cache_tick = GetTickCount64();
    s.dup_volume_cache_pending = false;
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

void StartDuplicateScan(AppState& s) {
    auto& session = s.duplicateScan;
    if (session.scope == app::DuplicateScanScope::AllFixed && s.dup_volume_cache.empty()) {
        RequestDuplicateVolumeCache(s, true);
        return;
    }
    const auto& volumes = s.dup_volume_cache;
    if (session.scope == app::DuplicateScanScope::Folder && session.folder_path.empty()) {
        std::wstring path;
        if (!PickFolder(s.hwnd, path, l10n::Get(l10n::StringId::DupFolderPlaceholder).c_str()))
            return;
        session.folder_path = std::move(path);
    }
    if (session.scope == app::DuplicateScanScope::Drive && session.drive_root.empty()) {
        for (const auto& volume : volumes) {
            if (volume.mount_point.empty()) continue;
            session.drive_root = app::DuplicateScanSession::NormalizeDriveRoot(volume.mount_point);
            break;
        }
    }
    auto roots = app::DuplicateScanSession::ResolveRoots(
        session.scope, session.folder_path, session.drive_root, volumes);
    if (roots.empty()) return;
    PersistDuplicateScanPrefs(s);
    session.ResetResults();
    session.generation += 1;
    session.scanning = true;
    index::ContentSearchRequest request;
    request.generation = session.generation;
    request.mode = index::ContentSearchMode::Duplicates;
    request.maximum_hits = 10000;
    request.roots = roots;
    request.root = roots.front();
    request.recursive = true;
    request.skip_system_locations = session.scope != app::DuplicateScanScope::Folder;
    request.minimum_file_bytes = session.minimum_file_bytes;
    s.duplicateSearch.SearchAsync(std::move(request));
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

void CancelDuplicateScan(AppState& s) {
    s.duplicateSearch.Cancel();
    s.duplicateScan.scanning = false;
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

void RecycleDuplicateGroup(AppState& s, size_t group) {
    auto paths = s.duplicateScan.FilesToDelete(group);
    if (paths.empty()) return;
    ops::OpRequest req;
    req.type = ops::OpType::RecycleDelete;
    req.delete_origin = ops::DeleteOrigin::Duplicates;
    req.sources = std::move(paths);
    s.ops.Submit(std::move(req));
}

void RecycleAllDuplicateExtras(AppState& s) {
    auto paths = s.duplicateScan.AllFilesToDelete();
    if (paths.empty()) return;
    ops::OpRequest req;
    req.type = ops::OpType::RecycleDelete;
    req.delete_origin = ops::DeleteOrigin::Duplicates;
    req.sources = std::move(paths);
    s.ops.Submit(std::move(req));
}

void OpenDuplicateLocation(AppState& s, size_t group, size_t file) {
    if (group >= s.duplicateScan.groups.size()) return;
    const auto& files = s.duplicateScan.groups[group].files;
    if (file >= files.size()) return;
    ResolvedOpenTarget target;
    target.file_path = fs::NormalizePath(files[file].path);
    target.folder = fs::ParentPath(target.file_path);
    const size_t slash = target.file_path.find_last_of(L"\\/");
    target.file_leaf = slash == std::wstring::npos
        ? target.file_path : target.file_path.substr(slash + 1);
    if (target.folder.empty() || target.file_leaf.empty()) return;
    NavigateTo(s, target.folder);
    SelectLaunchedFile(s, target);
}

} // namespace pulse
