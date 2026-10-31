// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "settings_transfer_ui.h"
#include "settings_transfer.h"
#include "app_state.h"
#include "app_commands.h"
#include "app_exit.h"
#include "session.h"
#include "pulse_version.h"
#include "../common/localization.h"
#include "../common/json_utils.h"
#include "../common/utf8_file.h"
#include "../ipc/shell_interaction_client.h"
#include "../ui/file_operation_dialog.h"
#include <atomic>
#include <cmath>
#include <mutex>

namespace pulse {
namespace {
namespace st = app::settings_transfer;
using I = l10n::StringId;
std::atomic_bool transfer_busy{false};
std::mutex disk_mutex;
SettingsTransferView disk_view;
std::wstring latest_backup;
st::ApplyResult startup_result;

std::wstring Stamp(bool iso = false) {
    SYSTEMTIME time{};
    GetLocalTime(&time);
    wchar_t text[32]{};
    swprintf_s(text, iso ? L"%04u-%02u-%02uT%02u:%02u:%02u" : L"%04u%02u%02u-%02u%02u%02u",
        time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond);
    return text;
}

std::wstring Substitute(std::wstring text, std::wstring_view token, const std::wstring& value) {
    const auto at = text.find(token);
    if (at != std::wstring::npos) text.replace(at, token.size(), value);
    return text;
}

void RefreshDisk() {
    const auto dir = app::GetPulseDataDir();
    SettingsTransferView next;
    next.pending = st::HasPendingImport(dir);
    const auto backups = st::ListBackups(dir);
    std::lock_guard lock(disk_mutex);
    latest_backup = backups.empty() ? L"" : backups.front().dir;
    next.has_backup = !backups.empty();
    if (next.has_backup) next.backup_time = st::DisplayTime(backups.front().stamp);
    disk_view = std::move(next);
}

void Toast(AppState& state, const std::wstring& message) {
    state.notification_toast.Show(state.hwnd, l10n::Get(I::SettingsTransfer), message);
}

bool Confirm(AppState& state, I title, const std::wstring& message, I accept) {
    ui::ConfirmDialogSpec spec;
    spec.title = l10n::Get(title);
    spec.message = message;
    spec.confirm_text = l10n::Get(accept);
    spec.note = l10n::Get(I::ImportSettingsNote);
    spec.cancel_is_default = true;
    return ui::ShowConfirmDialog(state.hwnd, spec, state.darkMode, state.accentColor);
}

bool Restart(AppState& state) {
    wchar_t exe[32768]{};
    const DWORD count = GetModuleFileNameW(nullptr, exe, ARRAYSIZE(exe));
    if (!count || count >= ARRAYSIZE(exe)) return false;
    std::wstring command = L"\"" + std::wstring(exe) + L"\" --restart-after " +
        std::to_wstring(GetCurrentProcessId());
    STARTUPINFOW start{sizeof(start)};
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(exe, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
        nullptr, nullptr, &start, &process)) return false;
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    RequestApplicationExit(state);
    return true;
}

void CaptureExport(AppState& state, app::SettingsTask& task) {
    const auto places = state.places.ToJson();
    const json::Document doc(places);
    task.snapshots.emplace_back(L"app.json", state.appPrefs.ToJson());
    task.snapshots.emplace_back(L"context_menu.json", state.ctxMenuPrefs.ToJson());
    task.snapshots.emplace_back(L"places.json", places);
    task.snapshots.emplace_back(L"tags.json", L"{\"version\":2,\"tags\":" +
        json::ExtractRaw(doc, L"tags", json::Kind::Array) + L"}");
    app::WindowSessionSnapshot preferences;
    preferences.sidebar_collapsed = static_cast<int>(state.sidebarCollapsedMask);
    preferences.sidebar_hidden = static_cast<int>(state.sidebarHiddenMask);
    preferences.sidebar_order = app::NormalizeSidebarOrder(state.sidebarOrder);
    preferences.quick_access_hidden = static_cast<int>(state.sidebarQuickAccessHiddenMask);
    preferences.starred_expanded = state.starredExpanded;
    preferences.details_panel = state.showDetailsPanel;
    preferences.details_panel_width = static_cast<int>(std::lround(state.detailsPanelWidth));
    preferences.details_preview_only = state.detailsPreviewOnly;
    preferences.details_preview = state.detailsPreviewEnabled;
    task.snapshots.emplace_back(L"preferences.json", app::SessionPreferencesToJson(preferences));
}
}

SettingsTransferView GetSettingsTransferView() {
    std::lock_guard lock(disk_mutex);
    auto view = disk_view;
    view.busy = transfer_busy.load(std::memory_order_acquire);
    return view;
}

void SettingsTransferAction(AppState& state, int action) {
    if (action < 0 || action > 2 || transfer_busy.exchange(true, std::memory_order_acq_rel)) return;
    struct Release {
        bool started = false;
        ~Release() { if (!started) transfer_busy.store(false, std::memory_order_release); }
    } release;
    const auto view = GetSettingsTransferView();
    if (action == 2 && view.pending) {
        if (!Restart(state)) Toast(state, l10n::Get(I::SettingsRestartFailed));
        return;
    }
    app::SettingsTask task;
    if (action == 0) {
        std::wstring folder;
        if (!PickFolder(state.hwnd, folder, l10n::Get(I::ExportSettingsTitle).c_str())) return;
        task.kind = app::SettingsTaskKind::SettingsExport;
        task.path = folder + L"\\Pulse-settings-" + Stamp() + L"-" + std::to_wstring(GetTickCount64()) + L".json";
        CaptureExport(state, task);
    } else if (action == 1) {
        task.kind = app::SettingsTaskKind::SettingsImport;
        if (!ipc::PickShellPath(state.hwnd, false, l10n::Get(I::ImportSettingsTitle), task.path,
            {L"JSON", L"*.json"})) return;
        if (!Confirm(state, I::ImportSettingsTitle, l10n::Get(I::SettingsTransferDesc), I::ImportSettingsConfirm)) return;
    } else {
        if (!view.has_backup) return;
        task.kind = app::SettingsTaskKind::SettingsImport;
        task.pin = true;
        {
            std::lock_guard lock(disk_mutex);
            task.path = latest_backup;
        }
        if (!Confirm(state, I::RestoreSettingsTitle, Substitute(l10n::Get(I::RestoreSettingsPrompt),
            L"{time}", view.backup_time), I::RestoreSettingsConfirm)) return;
    }
    if (!(release.started = state.settings.StartSettingsTransfer(std::move(task))))
        Toast(state, l10n::Get(action == 0 ? I::SettingsExportFailed : I::SettingsImportFailed));
    state.core.windows.ForEach([](HWND hwnd, AppState&) { InvalidateRect(hwnd, nullptr, FALSE); });
}

bool RunSettingsTransferTask(const app::SettingsTask& task, std::wstring& error) {
    struct ReleaseBusy {
        ~ReleaseBusy() { transfer_busy.store(false, std::memory_order_release); }
    } release;
    const auto dir = app::GetPulseDataDir();
    bool ok = false;
    if (task.kind == app::SettingsTaskKind::SettingsExport) {
        std::wstring text;
        ok = st::BuildBundle(dir, st::kAllCategories, {PULSE_VERSION_STRING, Stamp(true)}, text,
            nullptr, &task.snapshots) && WriteUtf8FileAtomic(task.path, text);
        if (!ok) error = l10n::Get(I::SettingsExportFailed);
    } else if (task.pin) {
        ok = st::StageRestore(dir, task.path);
    } else {
        std::wstring text;
        st::Bundle bundle;
        const auto parsed = ReadUtf8File(task.path, text) ? st::ParseBundle(text, bundle) : st::ParseResult::Invalid;
        if (parsed == st::ParseResult::Ok) ok = st::StageImport(dir, bundle, bundle.categories);
        else error = l10n::Get(parsed == st::ParseResult::Newer ? I::ImportSettingsNewer : I::ImportSettingsInvalid);
    }
    if (!ok && error.empty()) error = l10n::Get(I::SettingsImportFailed);
    RefreshDisk();
    return ok;
}

void FinishSettingsTransfer(AppState& state, const app::SettingsTaskResult& result) {
    if (!result.ok) Toast(state, result.error);
    else if (result.task.kind == app::SettingsTaskKind::SettingsExport)
        Toast(state, Substitute(l10n::Get(I::SettingsExported), L"{file}", result.task.path));
    else Toast(state, l10n::Get(I::SettingsTransferPending));
    state.core.windows.ForEach([](HWND hwnd, AppState&) { InvalidateRect(hwnd, nullptr, FALSE); });
}

bool WaitForSettingsRestart(unsigned long process_id) {
    if (!process_id || process_id == GetCurrentProcessId()) return false;
    HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, process_id);
    if (!process) return GetLastError() == ERROR_INVALID_PARAMETER;
    const DWORD wait = WaitForSingleObject(process, 30000);
    CloseHandle(process);
    return wait == WAIT_OBJECT_0;
}

void ApplyStagedSettingsAtStartup() {
    startup_result = st::ApplyPendingImport(app::GetPulseDataDir(), Stamp());
    RefreshDisk();
}

void ShowSettingsTransferStartupToast(AppState& state) {
    const auto outcome = std::exchange(startup_result.outcome, st::ApplyOutcome::Nothing);
    if (outcome == st::ApplyOutcome::Nothing) return;
    Toast(state, l10n::Get(outcome == st::ApplyOutcome::Imported ? I::SettingsImportedToast :
        outcome == st::ApplyOutcome::Restored ? I::SettingsRestoredToast : I::SettingsApplyFailedToast));
}
}
