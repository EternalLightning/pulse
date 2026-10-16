#include "archive_navigation.h"
#include "archive_dialog.h"
#include "archive_operation_ui.h"
#include "app_state.h"
#include "app_navigation.h"
#include "app_runtime.h"
#include "../ops/archive.h"
#include "entry_sort.h"
#include "../common/localization.h"
#include "../ui/file_operation_dialog.h"
#include <atomic>
#include <algorithm>
#include <filesystem>
#include <map>
#include <mutex>

namespace pulse {
namespace app {
struct ArchiveListing : ops::ArchiveOperationResult {
    std::wstring file, password;
    std::vector<ops::ArchiveEntry> entries;
    std::unordered_set<std::wstring> encrypted;
};
}
namespace {
constexpr std::wstring_view kPrefix = L"pulse:archive:";
std::atomic<uint64_t> next_request{1};
bool CanBrowse(ops::ArchiveOutcome outcome) {
    return outcome == ops::ArchiveOutcome::Success || outcome == ops::ArchiveOutcome::PartialListing;
}
struct Result : ops::ArchiveOperationResult {
    std::wstring view, file, prefix, password, open_path;
    std::wstring source_view;
    uint64_t generation = 0;
    uint64_t source_generation = 0;
    bool extraction = false;
    bool cache_password_on_success = false;
    std::shared_ptr<const app::ArchiveListing> listing;
    fs::SnapshotPtr snapshot;
};
void Split(const std::wstring& view, std::wstring& file, std::wstring& prefix) {
    const auto separator = view.find(L'|', kPrefix.size());
    file = view.substr(kPrefix.size(), separator - kPrefix.size());
    prefix = separator == std::wstring::npos ? L"" : view.substr(separator + 1);
}
COLORREF Accent(const AppState& s) {
    return RGB(static_cast<BYTE>(s.accentColor.r * 255), static_cast<BYTE>(s.accentColor.g * 255),
               static_cast<BYTE>(s.accentColor.b * 255));
}
void Post(HWND hwnd, std::unique_ptr<Result> result) {
    auto* payload = result.release();
    if (!PostMessageW(hwnd, WM_ARCHIVE_RESULT, 0, reinterpret_cast<LPARAM>(payload))) delete payload;
}
fs::SnapshotPtr BuildArchiveSnapshot(const app::ArchiveListing& listing, const std::wstring& prefix,
                                    ui::SortColumn column, ui::SortDirection direction,
                                    app::FolderSortMode folders, const std::atomic_bool& cancel) {
    auto entries = std::make_shared<std::vector<fs::DirEntry>>();
    std::map<std::wstring, fs::DirEntry> children;
    for (const auto& item : listing.entries) {
        if (cancel.load()) return {};
        if (!item.path.starts_with(prefix)) continue;
        const auto tail = item.path.substr(prefix.size());
        if (tail.empty()) continue;
        const auto slash = tail.find(L'/');
        const auto name = tail.substr(0, slash);
        if (name.empty()) continue;
        auto& entry = children[name];
        entry.name = name;
        entry.is_dir = item.is_dir || slash != std::wstring::npos;
        entry.attrs = entry.is_dir ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_READONLY;
        entry.size = entry.is_dir ? 0 : item.size;
        // Implicit directories inherit a child's timestamp until an explicit record arrives.
        if (slash == std::wstring::npos || slash + 1 == tail.size() ||
            (entry.mtime.dwLowDateTime == 0 && entry.mtime.dwHighDateTime == 0)) entry.mtime = item.mtime;
    }
    for (auto& [name, entry] : children) entries->push_back(std::move(entry));
    std::sort(entries->begin(), entries->end(), [&](const auto& a, const auto& b) {
        return app::EntryLess(a, b, column, direction, folders);
    });
    return entries;
}
void RunExtraction(AppState& s, const std::wstring& file, const std::wstring& destination,
                   const std::vector<std::wstring>& selected, const std::wstring& password,
                   const std::wstring& member = {}, const std::wstring& source_view = {},
                   uint64_t source_generation = 0) {
    struct Task {
        std::atomic_bool done{false}, cancel{false};
        std::mutex progress_mutex;
        ops::ArchiveProgress progress;
        double peak_bytes_per_second = 0.0;
        std::unique_ptr<Result> result = std::make_unique<Result>();
    };
    auto task = std::make_shared<Task>();
    task->result->extraction = true;
    task->result->file = file;
    task->result->password = password;
    task->result->source_view = source_view;
    task->result->source_generation = source_generation;
    task->result->cache_password_on_success = !member.empty();
    if (!s.worker.EnqueueIo([task, file, destination, selected, password, member] {
        try {
            static_cast<ops::ArchiveOperationResult&>(*task->result) =
                ops::ExtractArchive(file, destination, selected, password, &task->cancel,
                    [task](const ops::ArchiveProgress& progress) {
                        std::lock_guard lock(task->progress_mutex);
                        task->progress = progress;
                        task->peak_bytes_per_second = (std::max)(task->peak_bytes_per_second, progress.bytes_per_second);
                    });
            if (task->result->outcome == ops::ArchiveOutcome::Success && !member.empty())
                task->result->open_path = (std::filesystem::path(destination) / member).wstring();
        } catch (...) {
            task->result->outcome = ops::ArchiveOutcome::Failed;
            task->result->error = l10n::Get(l10n::StringId::ArchiveExtractFailed);
        }
        task->done.store(true, std::memory_order_release);
    })) {
        task->result->error = l10n::Get(l10n::StringId::ArchiveWorkerUnavailable);
        task->done.store(true, std::memory_order_release);
    }
    const HWND owner = s.hwnd;
    const AppState* expected_state = &s;
    const bool progress_completed = app::ShowArchiveProgressWindow(owner, s.darkMode, Accent(s), task->done, task->cancel,
        [task, file, destination] {
            if (task->done.load(std::memory_order_acquire) &&
                task->result->outcome != ops::ArchiveOutcome::Success &&
                task->result->outcome != ops::ArchiveOutcome::Cancelled)
                return app::ArchiveFailureStatus(*task->result, file, destination);
            std::lock_guard lock(task->progress_mutex);
            auto status = app::ArchiveOperationStatus(task->progress, file, destination, task->cancel.load());
            status.peak_bytes_per_second = task->peak_bytes_per_second;
            return status;
        });
    if (!task->done.load(std::memory_order_acquire)) {
        task->cancel.store(true, std::memory_order_release);
        return;
    }
    if (!IsWindow(owner) || GetAppState(owner) != expected_state) return;
    if (task->result->outcome == ops::ArchiveOutcome::Success &&
        (!progress_completed || task->cancel.load(std::memory_order_acquire))) return;
    HandleArchiveResult(s, reinterpret_cast<LPARAM>(task->result.release()));
}
}
bool IsArchiveView(const std::wstring& path) { return path.starts_with(kPrefix); }
std::wstring ArchiveViewPath(const std::wstring& file, const std::wstring& prefix) {
    return std::wstring(kPrefix) + file + L"|" + prefix;
}
std::wstring ArchiveParent(const std::wstring& path) {
    std::wstring file, prefix;
    Split(path, file, prefix);
    if (prefix.empty()) return fs::ParentPath(file);
    prefix.pop_back();
    const auto slash = prefix.find_last_of(L"/\\");
    return ArchiveViewPath(file, slash == std::wstring::npos ? L"" : prefix.substr(0, slash + 1));
}
void CleanupArchiveFiles() {
    std::error_code error;
    const auto temp = std::filesystem::temp_directory_path(error);
    if (error) return;
    const auto owned = temp / L"PulseArchive" / std::to_wstring(GetCurrentProcessId());
    // Only the current process's private extraction directory is owned here.
    if ((GetFileAttributesW(owned.c_str()) & FILE_ATTRIBUTE_REPARSE_POINT) != 0) return;
    std::filesystem::remove_all(owned, error);
}
void LoadArchiveView(AppState& s, app::Tab& tab, bool refresh) {
    if (tab.archive_cancel) tab.archive_cancel->store(true);
    if (tab.snapshot_path == tab.current_path) CaptureListingSelection(tab);
    tab.ClearSelection();
    std::wstring file, prefix;
    Split(tab.current_path, file, prefix);
    if (tab.archive_file != file) {
        tab.archive_password.clear();
        tab.archive_encrypted.clear();
    }
    if (refresh || (tab.archive_listing &&
        (tab.archive_listing->file != file || tab.archive_listing->password != tab.archive_password)))
        tab.archive_listing.reset();
    tab.archive_file = file;
    tab.archive_prefix = prefix;
    tab.net_readonly = true;
    tab.virtual_title = std::filesystem::path(file).filename().wstring() + (prefix.empty() ? L"" : L" / " + prefix);
    tab.ClearBanner();
    tab.SetSnapshot(nullptr);
    tab.loading = true;
    tab.pending_generation = next_request++;
    tab.archive_cancel = std::make_shared<std::atomic_bool>(false);
    const auto cancel = tab.archive_cancel;
    const auto generation = tab.pending_generation;
    const auto view = tab.current_path, password = tab.archive_password;
    const auto cached_listing = tab.archive_listing;
    const auto column = tab.sort_column;
    const auto direction = tab.sort_direction;
    const auto folders = app::FolderSortModeFromInt(s.appPrefs.folder_sort_mode);
    const HWND hwnd = s.hwnd;
    auto result = std::make_shared<Result>();
    if (!s.worker.EnqueueIo([result, file, prefix, view, password, generation, cancel,
                            cached_listing, column, direction, folders] {
        result->file = file; result->prefix = prefix; result->view = view;
        result->password = password; result->generation = generation;
        try {
            result->listing = cached_listing;
            if (result->listing)
                static_cast<ops::ArchiveOperationResult&>(*result) =
                    static_cast<const ops::ArchiveOperationResult&>(*result->listing);
            if (!result->listing) {
                auto listing = ops::ListArchive(file, password, cancel.get());
                static_cast<ops::ArchiveOperationResult&>(*result) =
                    std::move(static_cast<ops::ArchiveOperationResult&>(listing));
                if (CanBrowse(result->outcome) && !cancel->load()) {
                    auto cache = std::make_shared<app::ArchiveListing>();
                    static_cast<ops::ArchiveOperationResult&>(*cache) =
                        static_cast<const ops::ArchiveOperationResult&>(*result);
                    cache->file = file;
                    cache->password = password;
                    for (auto& item : listing.entries) {
                        if (cancel->load()) return;
                        if (!ops::IsSafeArchiveMember(item.path)) continue;
                        std::replace(item.path.begin(), item.path.end(), L'\\', L'/');
                        if (item.encrypted) cache->encrypted.insert(item.path);
                        cache->entries.push_back(std::move(item));
                    }
                    result->listing = std::move(cache);
                }
            }
            if (result->listing)
                result->snapshot = BuildArchiveSnapshot(*result->listing, prefix, column, direction, folders, *cancel);
            else
                result->snapshot = std::make_shared<std::vector<fs::DirEntry>>();
        } catch (...) {
            result->outcome = ops::ArchiveOutcome::Failed;
            result->error = l10n::Get(l10n::StringId::ArchiveReadFailed);
        }
    }, [hwnd, result] { Post(hwnd, std::make_unique<Result>(std::move(*result))); })) {
        tab.loading = false;
        tab.banner_message = l10n::Get(l10n::StringId::ArchiveWorkerUnavailable);
    }
}
bool OpenArchiveSelection(AppState& s) {
    auto* tab = ActiveTab(s);
    if (!tab || !IsArchiveView(tab->current_path)) return false;
    if (tab->loading || !tab->snapshot) return true;
    const auto indices = tab->SelectedIndices();
    if (indices.size() != 1) return true;
    const auto entry = tab->EntryAt(static_cast<size_t>(indices.front()));
    if (entry.is_dir) NavigateTo(s, ArchiveViewPath(tab->archive_file, tab->archive_prefix + entry.name + L"/"));
    else {
        // Opening a member uses a private temporary extraction, never the source folder.
        const auto file = tab->archive_file, password = tab->archive_password;
        const auto member = tab->archive_prefix + entry.name;
        const auto destination = (std::filesystem::temp_directory_path() / L"PulseArchive" /
            std::to_wstring(GetCurrentProcessId()) / std::to_wstring(next_request++)).wstring();
        auto open_password = password;
        const auto source_view = tab->current_path;
        const auto source_generation = tab->applied_generation;
        const bool needs_password = tab->archive_encrypted.contains(member) && open_password.empty();
        if (needs_password && !app::ShowArchivePasswordDialog(s.hwnd, s.darkMode, Accent(s), open_password)) return true;
        bool source_still_active = false;
        ForEachPane(s, [&](app::Pane& pane) {
            auto& current = pane.view;
            if (current.current_path == source_view && current.archive_file == file &&
                current.applied_generation == source_generation && ActiveTab(s) == &current &&
                !current.loading && current.snapshot) source_still_active = true;
        });
        if (!source_still_active) return true;
        RunExtraction(s, file, destination, {member}, open_password, member, source_view, source_generation);
    }
    return true;
}
void UpdateArchiveOpenPassword(app::Tab& tab, const std::wstring& source_view,
    uint64_t source_generation, const std::wstring& file, const std::wstring& password,
    const ops::ArchiveOperationResult& result) {
    if (tab.current_path != source_view || tab.archive_file != file ||
        tab.applied_generation != source_generation) return;
    if (app::IsArchivePasswordFailure(result)) {
        if (tab.archive_password == password) tab.archive_password.clear();
    } else if (result.outcome == ops::ArchiveOutcome::Success) {
        tab.archive_password = password;
    }
}
void ExtractArchiveSelection(AppState& s, bool all) {
    auto* tab = ActiveTab(s);
    if (!tab || !IsArchiveView(tab->current_path)) return;
    std::vector<std::wstring> selected;
    if (!all) {
        for (int index : tab->SelectedIndices()) {
            const auto entry = tab->EntryAt(static_cast<size_t>(index));
            selected.push_back(tab->archive_prefix + entry.name + (entry.is_dir ? L"/" : L""));
        }
        if (selected.empty()) return;
    }
    app::ArchiveExtractOptions options;
    options.password = tab->archive_password;
    if (!app::ShowArchiveExtractDialog(s.hwnd, s.darkMode, Accent(s), tab->archive_file, all, options)) return;
    auto destination = options.destination;
    if (all && options.create_folder)
        destination = (std::filesystem::path(destination) / std::filesystem::path(tab->archive_file).stem()).wstring();
    const auto file = tab->archive_file;
    RunExtraction(s, file, destination, selected, options.password);
}
void HandleArchiveResult(AppState& s, LPARAM payload) {
    std::unique_ptr<Result> result(reinterpret_cast<Result*>(payload));
    if (!result) return;
    if (result->extraction) {
        if (!result->source_view.empty() && (result->cache_password_on_success ||
            app::IsArchivePasswordFailure(*result))) {
            ForEachPane(s, [&](app::Pane& pane) {
                UpdateArchiveOpenPassword(pane.view, result->source_view, result->source_generation,
                    result->file, result->password, *result);
            });
        }
        if (result->outcome != ops::ArchiveOutcome::Success) return;
        if (!result->open_path.empty() && !s.isolatedTest) {
            if (ops::IsArchivePath(result->open_path)) NavigateTo(s, ArchiveViewPath(result->open_path));
            else s.ops.OpenWith(result->open_path);
        }
        return;
    }
    ForEachPane(s, [&](app::Pane& pane) {
        auto& tab = pane.view;
        {
            if (tab.current_path != result->view || tab.pending_generation != result->generation) return;
            const auto result_view = tab.current_path;
            const auto result_file = tab.archive_file;
            tab.loading = false;
            const bool browsable = CanBrowse(result->outcome);
            const bool offer_password_retry = result->outcome == ops::ArchiveOutcome::Failed &&
                result->password_state == ops::ArchivePasswordState::Unknown && ActiveTab(s) == &tab;
            if (!browsable && !result->password.empty() && tab.archive_password == result->password)
                tab.archive_password.clear();
            if (browsable && result->listing) {
                if (tab.archive_listing != result->listing) {
                    tab.archive_listing = result->listing;
                    tab.archive_encrypted = result->listing->encrypted;
                }
            } else {
                tab.archive_listing.reset();
                tab.archive_encrypted.clear();
            }
            tab.SetSnapshot(result->snapshot);
            tab.RemapSelection(tab.pending_selected_names, tab.pending_selected_name);
            tab.pending_selected_names.clear();
            tab.pending_selected_name.clear();
            tab.snapshot_path = result->view;
            tab.applied_generation = result->generation;
            tab.pending_generation = 0;
            tab.banner_message = result->error;
            tab.SetBannerTitle(result->outcome == ops::ArchiveOutcome::Success ? app::BannerType::None : app::BannerType::Other,
                result->outcome == ops::ArchiveOutcome::Success ? L"" :
                result->outcome == ops::ArchiveOutcome::PartialListing ? std::filesystem::path(result->file).filename().wstring() :
                l10n::Get(l10n::StringId::ArchiveOpenFailed));
            if (!browsable && (result->password_state == ops::ArchivePasswordState::Required || offer_password_retry)) {
                bool retry_with_password = result->password_state == ops::ArchivePasswordState::Required;
                if (offer_password_retry) {
                    ui::ConfirmDialogSpec spec;
                    spec.title = l10n::Get(l10n::StringId::ArchiveOpenFailed);
                    spec.message = l10n::Get(l10n::StringId::ArchivePasswordUnknownRetry);
                    spec.confirm_text = l10n::Get(l10n::StringId::ArchiveEnterPassword);
                    spec.cancel_text = l10n::Get(l10n::StringId::Cancel);
                    spec.cancel_is_default = true;
                    spec.fit_to_work_area = true;
                    spec.rows = {{result->error, false, false}, {result->file, true, true}};
                    retry_with_password = ui::ShowConfirmDialog(s.hwnd, spec, s.darkMode, s.accentColor);
                }
                if (retry_with_password) {
                    std::wstring retry_password;
                    if (app::ShowArchivePasswordDialog(s.hwnd, s.darkMode, Accent(s), retry_password)) {
                        ForEachPane(s, [&](app::Pane& retry_pane) {
                            auto& current = retry_pane.view;
                            if (current.current_path != result_view || current.applied_generation != result->generation ||
                                current.archive_file != result_file || ActiveTab(s) != &current) return;
                            current.archive_password = std::move(retry_password);
                            LoadArchiveView(s, current);
                        });
                    }
                }
                return;
            }
        }
    });
    InvalidateRect(s.hwnd, nullptr, FALSE);
}
void DiscardArchiveResult(LPARAM payload) {
    delete reinterpret_cast<Result*>(payload);
}
}
