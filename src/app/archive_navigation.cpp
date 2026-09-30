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
namespace {
constexpr std::wstring_view kPrefix = L"pulse:archive:";
std::atomic<uint64_t> next_request{1};
struct Result {
    std::wstring view, file, prefix, password, error, open_path;
    uint64_t generation = 0;
    bool password_required = false, extraction = false;
    std::vector<ops::ArchiveEntry> entries;
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
const wchar_t* Label(const wchar_t* zh, const wchar_t* en) {
    return l10n::effective_language() == l10n::Language::EnUS ? en : zh;
}
void RunExtraction(AppState& s, const std::wstring& file, const std::wstring& destination,
                   const std::vector<std::wstring>& selected, const std::wstring& password,
                   const std::wstring& member = {}) {
    struct Task {
        std::atomic_bool done{false}, cancel{false};
        std::unique_ptr<Result> result = std::make_unique<Result>();
    };
    auto task = std::make_shared<Task>();
    s.worker.EnqueueIo([task, file, destination, selected, password, member] {
        task->result->extraction = true;
        try {
            if (ops::ExtractArchive(file, destination, selected, password, task->result->error, &task->cancel)
                    && !member.empty())
                task->result->open_path = (std::filesystem::path(destination) / member).wstring();
        } catch (...) { task->result->error = L"Unable to extract archive."; }
        task->done.store(true, std::memory_order_release);
    });
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
void LoadArchiveView(AppState& s, app::Tab& tab) {
    if (tab.archive_cancel) tab.archive_cancel->store(true);
    if (tab.snapshot_path == tab.current_path) CaptureListingSelection(tab);
    tab.ClearSelection();
    std::wstring file, prefix;
    Split(tab.current_path, file, prefix);
    if (tab.archive_file != file) tab.archive_password.clear();
    tab.archive_file = file;
    tab.archive_prefix = prefix;
    tab.net_readonly = true;
    tab.virtual_title = std::filesystem::path(file).filename().wstring() + (prefix.empty() ? L"" : L" / " + prefix);
    tab.banner_title.clear();
    tab.banner_message.clear();
    tab.SetSnapshot(nullptr);
    tab.loading = true;
    tab.pending_generation = next_request++;
    tab.archive_cancel = std::make_shared<std::atomic_bool>(false);
    const auto cancel = tab.archive_cancel;
    const auto generation = tab.pending_generation;
    const auto view = tab.current_path, password = tab.archive_password;
    const HWND hwnd = s.hwnd;
    s.worker.EnqueueIo([hwnd, file, prefix, view, password, generation, cancel] {
        auto result = std::make_unique<Result>();
        result->file = file; result->prefix = prefix; result->view = view;
        result->password = password; result->generation = generation;
        try {
            auto listing = ops::ListArchive(file, password, cancel.get());
            result->entries = std::move(listing.entries);
            result->error = std::move(listing.error);
            result->password_required = listing.password_required;
        } catch (...) { result->error = L"Unable to read archive."; }
        Post(hwnd, std::move(result));
    });
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
        if (!result->error.empty()) MessageBoxW(s.hwnd, result->error.c_str(), L"Pulse", MB_OK | MB_ICONERROR);
        else if (!result->open_path.empty()) {
            if (ops::IsArchivePath(result->open_path)) NavigateTo(s, ArchiveViewPath(result->open_path));
            else s.ops.OpenWith(result->open_path);
        } else MessageBoxW(s.hwnd, Label(L"解压完成。已有同名文件已保留。", L"Extraction complete. Existing files were kept."), L"Pulse", MB_OK);
        return;
    }
    ForEachPane(s, [&](app::Pane& pane) {
        auto& tab = pane.view;
        {
            if (tab.current_path != result->view || tab.pending_generation != result->generation) return;
            tab.loading = false;
            if (result->password_required && !result->error.empty() && ActiveTab(s) == &tab) {
                if (app::ShowArchivePasswordDialog(s.hwnd, s.darkMode, Accent(s), tab.archive_password)) {
                    LoadArchiveView(s, tab);
                    return;
                }
            }
            auto entries = std::make_shared<std::vector<fs::DirEntry>>();
            std::map<std::wstring, fs::DirEntry> children;
            tab.archive_encrypted.clear();
            for (auto item : result->entries) {
                if (!ops::IsSafeArchiveMember(item.path)) continue;
                std::replace(item.path.begin(), item.path.end(), L'\\', L'/');
                if (item.encrypted) tab.archive_encrypted.insert(item.path);
                if (!item.path.starts_with(result->prefix)) continue;
                const auto tail = item.path.substr(result->prefix.size());
                if (tail.empty()) continue;
                const auto slash = tail.find(L'/');
                const auto name = tail.substr(0, slash);
                if (name.empty()) continue;
                auto& entry = children[name];
                entry.name = name; entry.is_dir = item.is_dir || slash != std::wstring::npos;
                entry.attrs = entry.is_dir ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_READONLY;
                entry.size = entry.is_dir ? 0 : item.size;
                // Implicit directories inherit a child's timestamp until an explicit directory record arrives.
                if (slash == std::wstring::npos || slash + 1 == tail.size() ||
                    (entry.mtime.dwLowDateTime == 0 && entry.mtime.dwHighDateTime == 0)) entry.mtime = item.mtime;
            }
            for (auto& [name, entry] : children) entries->push_back(std::move(entry));
            std::sort(entries->begin(), entries->end(), [&](const auto& a, const auto& b) {
                return app::EntryLess(a, b, tab.sort_column, tab.sort_direction);
            });
            tab.SetSnapshot(entries);
            tab.RemapSelection(tab.pending_selected_names, tab.pending_selected_name);
            tab.pending_selected_names.clear();
            tab.pending_selected_name.clear();
            tab.snapshot_path = result->view;
            tab.applied_generation = result->generation;
            tab.pending_generation = 0;
            tab.banner_message = result->error;
            tab.banner_title = result->error.empty() ? L"" : Label(L"无法打开压缩包", L"Unable to open archive");
        }
    });
    InvalidateRect(s.hwnd, nullptr, FALSE);
}
void DiscardArchiveResult(LPARAM payload) {
    delete reinterpret_cast<Result*>(payload);
}
}
