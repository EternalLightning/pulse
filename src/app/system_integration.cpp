// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

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
    SystemTakeoverTestCallbacks test_takeover;
    bool registry_started = false;
    bool takeover_started = false;
    std::map<uint64_t, ULONGLONG> pending_takeover_tokens;
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
    struct Destination {
        uint64_t key = 0;
        uint64_t generation = 0;
        uint64_t view_generation = 0;
        std::wstring folder;
        ULONGLONG deadline = 0;
    };
    std::map<Tab*, uint64_t> identities;
    std::vector<Selection> selections;
    // Retain completed bindings briefly: a replacement can arrive after the
    // first ACK but must still use that exact pane rather than opening another.
    std::map<uint64_t, Destination> takeover_destinations;
    uint64_t next_key = 1;
    ULONGLONG next_publish = 0;
    HRESULT reported_service_error = S_OK;
    std::wstring error;
};
namespace {
bool TakeoverActive(const std::shared_ptr<SystemIntegration::Shared>& shared, uint64_t token) {
    return shared->test_takeover.active ? shared->test_takeover.active(token)
        : shared->takeover.IsRequestActive(token);
}
void AcknowledgeTakeover(const std::shared_ptr<SystemIntegration::Shared>& shared,
                         uint64_t token, bool delivered) {
    if (shared->test_takeover.acknowledge) shared->test_takeover.acknowledge(token, delivered);
    else shared->takeover.Acknowledge(token, delivered);
}
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
struct LocatedTab { AppState* window = nullptr; Tab* tab = nullptr; };
LocatedTab FindTab(AppState& owner, SystemIntegration::UiState& ui, uint64_t key, bool activate) {
    LocatedTab found_tab;
    owner.core.windows.ForEach([&](HWND, AppState& window) {
        if (found_tab.tab) return;
        for (size_t i = 0; i < window.window_tabs.items.size() && !found_tab.tab; ++i) {
            auto& layout = window.window_tabs.items[i];
            if (!layout) continue;
            for (auto& pane : layout->panes) {
                Tab* tab = pane ? pane->ActiveTab() : nullptr;
                const auto found = ui.identities.find(tab);
                if (!tab || found == ui.identities.end() || found->second != key) continue;
                if (activate) {
                    if (window.window_tabs.active != i) SwitchTab(window, i);
                    FocusPane(window, pane.get());
                }
                found_tab = {&window, tab};
                break;
            }
        }
    });
    return found_tab;
}
bool QueueSelection(AppState& owner, SystemIntegration::UiState& ui, uint64_t key, uint64_t token,
                    const std::wstring& folder, std::vector<std::wstring> paths, unsigned flags) {
    auto located = FindTab(owner, ui, key, false);
    Tab* tab = located.tab;
    if (!tab || !SameShellPath(tab->current_path, folder) || ui.selections.size() >= 128) return false;
    located = FindTab(owner, ui, key, true);
    tab = located.tab;
    if (!tab || !SameShellPath(tab->current_path, folder)) return false;
    ui.selections.push_back({key, token, tab->pending_generation ? tab->pending_generation : tab->applied_generation,
        tab->view_generation, folder, std::move(paths), flags,
        GetTickCount64() + 14000});
    RestoreAppWindow(*located.window);
    return true;
}
void RememberTakeoverDestination(SystemIntegration::UiState& ui, uint64_t token) {
    const auto& selection = ui.selections.back();
    ui.takeover_destinations.insert_or_assign(token, SystemIntegration::UiState::Destination{
        selection.key, selection.generation, selection.view_generation, selection.folder,
        GetTickCount64() + 15000});
}
bool HasPendingTakeover(const std::shared_ptr<SystemIntegration::Shared>& shared, uint64_t token) {
    if (!shared || !token) return false;
    std::lock_guard lock(shared->mutex);
    return shared->pending_takeover_tokens.contains(token);
}
void RemovePendingTakeover(const std::shared_ptr<SystemIntegration::Shared>& shared, uint64_t token) {
    if (!shared || !token) return;
    std::lock_guard lock(shared->mutex);
    shared->pending_takeover_tokens.erase(token);
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
bool SystemIntegration::StartForTesting(AppState& s, DefaultManagerRegistry registry,
                                         SystemTakeoverTestCallbacks takeover) {
    if (shared_ || !s.isolatedTest || s.appPrefs.persist || !registry.store) return false;
    const bool any = takeover.active || takeover.acknowledge || takeover.drain || takeover.route;
    if (any && (!takeover.active || !takeover.acknowledge || !takeover.drain || !takeover.route)) return false;
    shared_ = std::make_shared<Shared>();
    shared_->hwnd = s.hwnd; shared_->executable = SystemExecutablePath();
    shared_->registry_backend = registry; shared_->services_enabled = false;
    shared_->test_takeover = std::move(takeover);
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
    s.core.windows.ForEach([&](HWND, AppState& window) {
        ForEachPane(window, [&](Pane& pane) {
            Tab* tab = pane.ActiveTab();
            if (!tab || (!tab->current_path.empty() && fs::IsVirtualPath(tab->current_path))) return;
            alive.insert(tab);
            auto [it, inserted] = ui_->identities.try_emplace(tab);
            if (inserted) it->second = ui_->next_key++;
            wanted.push_back({it->second, ShellPathText(tab->current_path)});
        });
    });
    for (auto it = ui_->identities.begin(); it != ui_->identities.end();) {
        if (!alive.contains(it->first)) it = ui_->identities.erase(it);
        else ++it;
    }
    shared_->registry.Publish(std::move(wanted));
}
bool SystemIntegration::IsExternalOpenActive(uint64_t takeover_token) const {
    return shared_ && HasPendingTakeover(shared_, takeover_token) &&
        TakeoverActive(shared_, takeover_token);
}
void SystemIntegration::CompleteExternalOpen(AppState& owner, AppState* destination,
                                             const ExternalOpenResult& request, bool opened) {
    if (!shared_ || !request.takeover_token || !HasPendingTakeover(shared_, request.takeover_token)) return;
    if (!opened || !destination || !TakeoverActive(shared_, request.takeover_token)) {
        RemovePendingTakeover(shared_, request.takeover_token);
        AcknowledgeTakeover(shared_, request.takeover_token, false);
        return;
    }
    Publish(owner);
    Tab* tab = ActiveTab(*destination);
    const auto identity = ui_->identities.find(tab);
    if (!tab || identity == ui_->identities.end() ||
        !QueueSelection(owner, *ui_, identity->second, request.takeover_token,
            request.target.folder, request.selected_paths, request.selection_flags)) {
        RemovePendingTakeover(shared_, request.takeover_token);
        AcknowledgeTakeover(shared_, request.takeover_token, false);
    } else {
        RememberTakeoverDestination(*ui_, request.takeover_token);
        std::lock_guard lock(shared_->mutex);
        if (auto it = shared_->pending_takeover_tokens.find(request.takeover_token);
            it != shared_->pending_takeover_tokens.end()) it->second = GetTickCount64() + 14000;
    }
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
    std::erase_if(ui_->takeover_destinations, [now](const auto& entry) {
        return now >= entry.second.deadline;
    });
    std::vector<uint64_t> expired_takeovers;
    {
        std::lock_guard lock(shared_->mutex);
        for (auto it = shared_->pending_takeover_tokens.begin(); it != shared_->pending_takeover_tokens.end();) {
            if (now >= it->second) {
                expired_takeovers.push_back(it->first);
                it = shared_->pending_takeover_tokens.erase(it);
            } else ++it;
        }
    }
    for (const uint64_t token : expired_takeovers)
        AcknowledgeTakeover(shared_, token, false);
    if (now >= ui_->next_publish) { Publish(s); ui_->next_publish = now + 250; }
    for (auto& request : shared_->registry.DrainSelections()) {
        QueueSelection(s, *ui_, request.key, 0, request.folder, {request.path}, request.flags);
        changed = true;
    }
    auto requests = shared_->test_takeover.drain ? shared_->test_takeover.drain()
        : shared_->takeover.DrainRequests();
    for (auto& request : requests) {
        if (!s.appPrefs.experimental_explorer_takeover || !TakeoverActive(shared_, request.token)) {
            AcknowledgeTakeover(shared_, request.token, false); continue;
        }
        auto previous = ui_->takeover_destinations.find(request.replaces_token);
        if ((request.replaces_token && previous == ui_->takeover_destinations.end()) ||
            (!request.replaces_token && ui_->takeover_destinations.size() >= 256)) {
            AcknowledgeTakeover(shared_, request.token, false); continue;
        }
        {
            std::lock_guard lock(shared_->mutex);
            shared_->pending_takeover_tokens.insert_or_assign(request.token, now + 14000);
        }
        if (previous != ui_->takeover_destinations.end()) {
            const auto destination = previous->second;
            ui_->takeover_destinations.erase(previous);
            if (destination.key && SameShellPath(destination.folder, request.folder)) {
                const auto located = FindTab(s, *ui_, destination.key, false);
                if (located.tab &&
                    !ShellSelectionIsStale(*located.tab, destination.folder,
                        destination.generation, destination.view_generation) &&
                    QueueSelection(s, *ui_, destination.key, request.token,
                        request.folder, std::move(request.selected_paths),
                        SVSI_SELECT | SVSI_DESELECTOTHERS | SVSI_ENSUREVISIBLE)) {
                    RememberTakeoverDestination(*ui_, request.token);
                } else {
                    RemovePendingTakeover(shared_, request.token);
                    AcknowledgeTakeover(shared_, request.token, false);
                }
                changed = true;
                continue;
            }
        }
        ui_->takeover_destinations.insert_or_assign(request.token,
            UiState::Destination{0, 0, 0, request.folder, now + 15000});
        const bool routed = shared_->test_takeover.route ? shared_->test_takeover.route(request)
            : RouteExternalFolderOpen(s,
                request.folder.empty() ? kThisPcParsingName : request.folder,
                request.token, std::move(request.selected_paths),
                SVSI_SELECT | SVSI_DESELECTOTHERS | SVSI_ENSUREVISIBLE);
        if (!routed) {
            RemovePendingTakeover(shared_, request.token);
            AcknowledgeTakeover(shared_, request.token, false);
        }
        changed = true;
    }
    for (auto it = ui_->selections.begin(); it != ui_->selections.end();) {
        auto located = FindTab(s, *ui_, it->key, false);
        Tab* tab = located.tab;
        const auto delivery_invalid = [&] {
            return !tab || !SameShellPath(tab->current_path, it->folder) ||
                tab->view_generation != it->view_generation || GetTickCount64() >= it->deadline ||
                (!it->token && ShellSelectionIsStale(*tab, it->folder, it->generation, it->view_generation)) ||
                (it->token && (!s.appPrefs.experimental_explorer_takeover ||
                    !TakeoverActive(shared_, it->token)));
        };
        bool finished = delivery_invalid();
        bool delivered = false;
        if (!finished && !tab->loading && !tab->pending_generation) {
            located = FindTab(s, *ui_, it->key, true);
            tab = located.tab;
            finished = delivery_invalid();
            if (!finished) {
                // Activation and background refreshes can replace the listing
                // without changing the destination. Wait for the final snapshot.
                it->generation = tab->pending_generation ? tab->pending_generation : tab->applied_generation;
                if (!tab->loading && !tab->pending_generation) {
                    finished = true;
                    std::vector<int> rows;
                    delivered = ApplyShellSelection(*tab, s.places, it->paths, it->flags, rows);
                    if (delivered) {
                        if (auto binding = ui_->takeover_destinations.find(it->token);
                            binding != ui_->takeover_destinations.end()) binding->second.generation = it->generation;
                        if (!rows.empty()) EnsureRowVisible(*located.window, *tab, rows.back());
                        InvalidateRect(located.window->hwnd, nullptr, FALSE); changed = true;
                    }
                }
            }
        }
        if (finished) {
            if (it->token) {
                RemovePendingTakeover(shared_, it->token);
                AcknowledgeTakeover(shared_, it->token, delivered);
            }
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
    {
        std::lock_guard lock(shared_->mutex);
        for (const auto& [token, deadline] : shared_->pending_takeover_tokens) {
            (void)deadline;
            AcknowledgeTakeover(shared_, token, false);
        }
        shared_->pending_takeover_tokens.clear();
        shared_->stop = true;
        shared_->task.reset();
    }
    shared_->cv.notify_all();
    if (shared_->worker.joinable()) shared_->worker.join();
    // Leave the shared pointer intact while the shutdown worker runs; UI is gated by exit_requested.
}
} // namespace pulse::app
