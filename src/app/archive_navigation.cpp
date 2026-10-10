#include "archive_navigation.h"
#include "archive_dialog.h"
#include "app_state.h"
#include "app_navigation.h"
#include "app_runtime.h"
#include "../ops/archive.h"
#include "entry_sort.h"
#include "../common/localization.h"
#include <atomic>
#include <filesystem>
#include <map>

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
    uint64_t generation = 0;
    bool extraction = false;
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
                   const std::wstring& member = {}) {
    struct Task {
        std::atomic_bool done{false}, cancel{false};
        std::unique_ptr<Result> result = std::make_unique<Result>();
    };
    auto task = std::make_shared<Task>();
    task->result->extraction = true;
    if (!s.worker.EnqueueIo([task, file, destination, selected, password, member] {
        try {
            static_cast<ops::ArchiveOperationResult&>(*task->result) =
                ops::ExtractArchive(file, destination, selected, password, &task->cancel);
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
    if (app::ShowArchiveProgressDialog(s.hwnd, s.darkMode, Accent(s), task->done, task->cancel))
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
        if (tab->archive_encrypted.contains(member) && open_password.empty() && !app::ShowArchivePasswordDialog(s.hwnd, s.darkMode, Accent(s), open_password))
            return true;
        tab->archive_password = open_password;
        RunExtraction(s, file, destination, {member}, open_password, member);
    }
    return true;
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
        if (result->outcome == ops::ArchiveOutcome::Cancelled) return;
        if (result->outcome != ops::ArchiveOutcome::Success) {
            const auto error = result->error.empty() ? l10n::Get(l10n::StringId::ArchiveExtractFailed) : result->error;
            MessageBoxW(s.hwnd, error.c_str(), L"Pulse", MB_OK | MB_ICONERROR);
        }
        else if (!result->open_path.empty()) {
            if (ops::IsArchivePath(result->open_path)) NavigateTo(s, ArchiveViewPath(result->open_path));
            else s.ops.OpenWith(result->open_path);
        }
        return;
    }
    ForEachPane(s, [&](app::Pane& pane) {
        auto& tab = pane.view;
        {
            if (tab.current_path != result->view || tab.pending_generation != result->generation) return;
            tab.loading = false;
            const bool browsable = CanBrowse(result->outcome);
            bool retry_with_password = result->password_state == ops::ArchivePasswordState::Required;
            if (result->outcome == ops::ArchiveOutcome::Failed &&
                result->password_state == ops::ArchivePasswordState::Unknown && ActiveTab(s) == &tab) {
                const auto message = result->error + L"\n\n" + l10n::Get(l10n::StringId::ArchivePasswordUnknownRetry);
                retry_with_password = MessageBoxW(s.hwnd, message.c_str(), L"Pulse",
                    MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) == IDYES;
            }
            if (!browsable && retry_with_password && ActiveTab(s) == &tab) {
                if (app::ShowArchivePasswordDialog(s.hwnd, s.darkMode, Accent(s), tab.archive_password)) {
                    LoadArchiveView(s, tab);
                    return;
                }
            }
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
        }
    });
    InvalidateRect(s.hwnd, nullptr, FALSE);
}
void DiscardArchiveResult(LPARAM payload) {
    delete reinterpret_cast<Result*>(payload);
}
}
