#include "system_integration.h"
#include "shell_window_registry.h"
#include "explorer_window_takeover.h"
#include "shell_selection.h"
#include "app_internal.h"
#include "app_runtime.h"
#include "app_navigation.h"
#include "../common/localization.h"
#include <shlobj.h>
#include <condition_variable>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <thread>

namespace pulse::app {
std::wstring SystemExecutablePath() {
    std::wstring path(32768, L'\0');
    const DWORD n = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (!n || n == path.size()) return {};
    path.resize(n); return path;
}
struct SystemIntegration::Shared {
    struct Task {
        DefaultManagerOptions wanted;
        bool apply = false;
        bool experimental = false;
        bool repair_legacy = false;
    };
    struct Result {
        DefaultManagerOptions actual;
        DefaultManagerResult result;
        bool save = false;
        HRESULT service_error = S_OK;
    };
    HWND hwnd = nullptr;
    std::wstring executable;
    DefaultManagerRegistry registry_backend;
    bool services_enabled = true;
    std::mutex mutex;
    std::condition_variable cv;
    std::optional<Task> task;
    std::optional<Result> result;
    bool stop = false;
    bool pending = false;
    ShellWindowRegistry registry;
    ExplorerWindowTakeover takeover;
    bool registry_started = false;
    bool takeover_started = false;
    std::thread worker;
};
struct SystemIntegration::UiState {
    struct Selection {
        uint64_t key = 0;
        uint64_t token = 0;
        uint64_t generation = 0;
        uint64_t view_generation = 0;
        std::wstring folder;
        std::vector<std::wstring> paths;
        unsigned flags = SVSI_SELECT | SVSI_DESELECTOTHERS | SVSI_ENSUREVISIBLE;
        ULONGLONG deadline = 0;
    };
    std::map<Tab*, uint64_t> identities;
    std::vector<Selection> selections;
    uint64_t next_key = 1;
    ULONGLONG next_publish = 0;
    HRESULT reported_service_error = S_OK;
    std::wstring error;
};
namespace {
DefaultManagerOptions Options(const AppPrefs& prefs) {
    return {prefs.open_folders_in_pulse, prefs.take_over_win_e, prefs.take_over_this_pc};
}
void Submit(const std::shared_ptr<SystemIntegration::Shared>& shared,
            SystemIntegration::Shared::Task task) {
    { std::lock_guard lock(shared->mutex);
        if (shared->stop) return;
        shared->task = std::move(task); shared->pending = true;
    }
    shared->cv.notify_one();
}
void SystemWorker(const std::shared_ptr<SystemIntegration::Shared>& shared) {
    for (;;) {
        SystemIntegration::Shared::Task task;
        { std::unique_lock lock(shared->mutex);
            shared->cv.wait(lock, [&] { return shared->stop || shared->task.has_value(); });
            if (shared->stop) break;
            task = *shared->task; shared->task.reset();
        }
        SystemIntegration::Shared::Result result;
        result.actual = ReadDefaultFileManager(shared->executable, shared->registry_backend);
        if (task.repair_legacy && task.wanted.folders && !result.actual.folders) {
            AppPrefs legacy;
            if (legacy.ReadFolderOpen()) {
                result.actual.folders = true;
                result.result = ApplyDefaultFileManager(shared->executable, result.actual, shared->registry_backend);
                result.actual = ReadDefaultFileManager(shared->executable, shared->registry_backend);
            }
        }
        if (task.apply) {
            result.result = ApplyDefaultFileManager(shared->executable, task.wanted, shared->registry_backend);
            result.actual = ReadDefaultFileManager(shared->executable, shared->registry_backend);
            result.save = true;
        }
        const bool shell_enabled = result.actual.folders || result.actual.this_pc;
        if (shared->services_enabled) {
            if (shell_enabled && !shared->registry_started)
                shared->registry_started = shared->registry.Start(shared->hwnd, kShellSelectionMessage);
            else if (!shell_enabled && shared->registry_started) {
                shared->registry.Stop(); shared->registry_started = false;
            }
            if (task.experimental && !shared->takeover_started)
                shared->takeover_started = shared->takeover.Start(shared->hwnd, kExplorerTakeoverMessage);
            else if (!task.experimental && shared->takeover_started) {
                shared->takeover.Stop(); shared->takeover_started = false;
            }
            if ((shell_enabled && !shared->registry_started) || (task.experimental && !shared->takeover_started))
                result.service_error = E_FAIL;
        }
        { std::lock_guard lock(shared->mutex);
            if (shared->stop) break;
            shared->result = result; shared->pending = shared->task.has_value();
        }
        PostMessageW(shared->hwnd, kSystemIntegrationMessage, 0, 0);
    }
    shared->takeover.Stop(); shared->registry.Stop();
}
std::wstring Failure(const DefaultManagerResult& result) {
    if (result.external_change) return l10n::Get(l10n::StringId::SettingsSystemExternalChange);
    std::wstring message = l10n::Get(l10n::StringId::DefaultFileManagerError);
    if (result.error) {
        wchar_t text[1024]{};
        if (FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
            static_cast<DWORD>(result.error), 0, text, 1024, nullptr)) message += L"\n" + std::wstring(text);
    }
    return message;
}
Tab* FindTab(AppState& s, SystemIntegration::UiState& ui, uint64_t key, bool activate) {
    for (size_t i = 0; i < s.window_tabs.items.size(); ++i) {
        auto& layout = s.window_tabs.items[i];
        if (!layout) continue;
        for (auto& pane : layout->panes) {
            Tab* tab = pane ? pane->ActiveTab() : nullptr;
            const auto found = ui.identities.find(tab);
            if (!tab || found == ui.identities.end() || found->second != key) continue;
            if (activate) {
                if (s.window_tabs.active != i) SwitchTab(s, i);
                FocusPane(s, pane.get());
            }
            return tab;
        }
    }
    return nullptr;
}
bool QueueSelection(AppState& s, SystemIntegration::UiState& ui, uint64_t key, uint64_t token,
                    const std::wstring& folder, std::vector<std::wstring> paths, unsigned flags) {
    Tab* tab = FindTab(s, ui, key, false);
    if (!tab || !SameShellPath(tab->current_path, folder) || ui.selections.size() >= 128) return false;
    tab = FindTab(s, ui, key, true);
    if (!tab || !SameShellPath(tab->current_path, folder)) return false;
    ui.selections.push_back({key, token, tab->pending_generation ? tab->pending_generation : tab->applied_generation,
        tab->view_generation, folder, std::move(paths), flags,
        GetTickCount64() + 14000});
    s.tray_controller.RestoreWindow();
    return true;
}
}
SystemIntegration::SystemIntegration() : ui_(std::make_unique<UiState>()) {}
SystemIntegration::~SystemIntegration() { Stop(); }
void SystemIntegration::Start(AppState& s) {
    if (shared_ || !s.appPrefs.persist || s.safeMode || s.isolatedTest || s.shot.active || s.menushot) return;
    shared_ = std::make_shared<Shared>(); shared_->hwnd = s.hwnd;
    shared_->executable = SystemExecutablePath();
    try { shared_->worker = std::thread(SystemWorker, shared_); }
    catch (...) { shared_.reset(); ui_->error = l10n::Get(l10n::StringId::DefaultFileManagerError); return; }
    Submit(shared_, {Options(s.appPrefs), false, s.appPrefs.experimental_explorer_takeover, true});
}
bool SystemIntegration::StartForTesting(AppState& s, DefaultManagerRegistry registry) {
    if (shared_ || !s.isolatedTest || s.appPrefs.persist || !registry.store) return false;
    shared_ = std::make_shared<Shared>();
    shared_->hwnd = s.hwnd; shared_->executable = SystemExecutablePath();
    shared_->registry_backend = registry; shared_->services_enabled = false;
    try { shared_->worker = std::thread(SystemWorker, shared_); }
    catch (...) { shared_.reset(); return false; }
    Submit(shared_, {Options(s.appPrefs), false, false, false});
    return true;
}
bool SystemIntegration::pending() const {
    if (!shared_) return false;
    std::lock_guard lock(shared_->mutex);
    // Keep controls gated until the UI has consumed the new association state.
    return shared_->pending || shared_->result.has_value();
}
const std::wstring& SystemIntegration::error() const { return ui_->error; }
void SystemIntegration::Toggle(AppState& s, int control) {
    if (s.exit_requested) return;
    if (!shared_) {
        if (!s.isolatedTest && s.appPrefs.persist)
            ui_->error = l10n::Get(l10n::StringId::DefaultFileManagerError);
        return;
    }
    if (pending()) return;
    if (control == 24) {
        s.appPrefs.experimental_explorer_takeover = !s.appPrefs.experimental_explorer_takeover;
        if (!s.appPrefs.Save()) {
            s.appPrefs.experimental_explorer_takeover = !s.appPrefs.experimental_explorer_takeover;
            ui_->error = l10n::Get(l10n::StringId::SettingsSystemApplySaveError); return;
        }
        Submit(shared_, {Options(s.appPrefs), false, s.appPrefs.experimental_explorer_takeover, false});
    } else {
        auto wanted = Options(s.appPrefs);
        if (control == 21) {
            const bool on = !(wanted.folders && wanted.win_e && wanted.this_pc);
            wanted = {on, on, on};
        } else if (control == 3) wanted.folders = !wanted.folders;
        else if (control == 22) wanted.win_e = !wanted.win_e;
        else if (control == 23) wanted.this_pc = !wanted.this_pc;
        else return;
        Submit(shared_, {wanted, true, s.appPrefs.experimental_explorer_takeover, false});
    }
    ui_->error.clear();
}
void SystemIntegration::Publish(AppState& s) {
    if (!shared_ || s.exit_requested) return;
    std::set<Tab*> alive;
    std::vector<ShellWindowEntry> wanted;
    ForEachPane(s, [&](Pane& pane) {
        Tab* tab = pane.ActiveTab();
        if (!tab || (!tab->current_path.empty() && fs::IsVirtualPath(tab->current_path))) return;
        alive.insert(tab);
        auto [it, inserted] = ui_->identities.try_emplace(tab);
        if (inserted) it->second = ui_->next_key++;
        wanted.push_back({it->second, ShellPathText(tab->current_path)});
    });
    for (auto it = ui_->identities.begin(); it != ui_->identities.end();) {
        if (!alive.contains(it->first)) it = ui_->identities.erase(it);
        else ++it;
    }
    shared_->registry.Publish(std::move(wanted));
}
bool SystemIntegration::Tick(AppState& s) {
    if (!shared_ || s.exit_requested) return false;
    bool changed = false;
    std::optional<Shared::Result> result;
    { std::lock_guard lock(shared_->mutex); result.swap(shared_->result); }
    if (result) {
        s.appPrefs.open_folders_in_pulse = result->actual.folders;
        s.appPrefs.take_over_win_e = result->actual.win_e;
        s.appPrefs.take_over_this_pc = result->actual.this_pc;
        if (!result->result) ui_->error = Failure(result->result);
        else if (FAILED(result->service_error)) ui_->error = l10n::Get(l10n::StringId::DefaultFileManagerError);
        if (result->save && !s.appPrefs.Save())
            ui_->error = l10n::Get(l10n::StringId::SettingsSystemApplySaveError);
        Publish(s); changed = true;
    }
    const auto now = GetTickCount64();
    if (now >= ui_->next_publish) { Publish(s); ui_->next_publish = now + 250; }
    for (auto& request : shared_->registry.DrainSelections()) {
        QueueSelection(s, *ui_, request.key, 0, request.folder, {request.path}, request.flags);
        changed = true;
    }
    for (auto& request : shared_->takeover.DrainRequests()) {
        if (!s.appPrefs.experimental_explorer_takeover || !shared_->takeover.IsRequestActive(request.token)) {
            shared_->takeover.Acknowledge(request.token, false); continue;
        }
        OpenFolderInNewTab(s, request.folder.empty() ? kThisPcParsingName : request.folder);
        Publish(s);
        Tab* tab = ActiveTab(s);
        const auto identity = ui_->identities.find(tab);
        if (!tab || identity == ui_->identities.end() || !SameShellPath(tab->current_path, request.folder)) {
            shared_->takeover.Acknowledge(request.token, false); continue;
        }
        if (!QueueSelection(s, *ui_, identity->second, request.token, request.folder,
            std::move(request.selected_paths), SVSI_SELECT | SVSI_DESELECTOTHERS | SVSI_ENSUREVISIBLE))
            shared_->takeover.Acknowledge(request.token, false);
        changed = true;
    }
    for (auto it = ui_->selections.begin(); it != ui_->selections.end();) {
        Tab* tab = FindTab(s, *ui_, it->key, false);
        bool finished = !tab || ShellSelectionIsStale(*tab, it->folder, it->generation, it->view_generation) ||
            now >= it->deadline ||
            (it->token && (!s.appPrefs.experimental_explorer_takeover ||
                !shared_->takeover.IsRequestActive(it->token)));
        bool delivered = false;
        if (!finished && !tab->loading && !tab->pending_generation) {
            finished = true;
            std::vector<int> rows;
            FindTab(s, *ui_, it->key, true);
            // Activation can schedule a refresh. Recheck after it, before ACK;
            // a previously selected cached row is not a completed delivery.
            delivered = !ShellSelectionIsStale(*tab, it->folder, it->generation, it->view_generation) &&
                ApplyShellSelection(*tab, s.places, it->paths, it->flags, rows);
            if (delivered) {
                if (!rows.empty()) EnsureRowVisible(s, *tab, rows.back());
                InvalidateRect(s.hwnd, nullptr, FALSE); changed = true;
            }
        }
        if (finished) {
            if (it->token) shared_->takeover.Acknowledge(it->token, delivered);
            it = ui_->selections.erase(it);
        } else ++it;
    }
    const HRESULT registry_error = shared_->registry.LastError();
    const HRESULT error = FAILED(registry_error) ? registry_error : shared_->takeover.LastError();
    if (FAILED(error) && error != ui_->reported_service_error) {
        ui_->reported_service_error = error;
        ui_->error = l10n::Get(l10n::StringId::DefaultFileManagerError); changed = true;
    } else if (SUCCEEDED(error)) ui_->reported_service_error = S_OK;
    return changed;
}
void SystemIntegration::Stop() {
    if (!shared_) return;
    { std::lock_guard lock(shared_->mutex); shared_->stop = true; shared_->task.reset(); }
    shared_->cv.notify_all();
    if (shared_->worker.joinable()) shared_->worker.join();
    // Leave the shared pointer intact while the shutdown worker runs; UI is gated by exit_requested.
}
} // namespace pulse::app
