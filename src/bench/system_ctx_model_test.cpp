// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "../app/system_ctx_model.h"
#include "../app/system_ctx_apply.h"
#include "../common/localization.h"
#include "../common/user_storage.h"

#include <cstdio>
#include <exception>
#include <filesystem>
#include <string>
#include <vector>

namespace pulse::app {
std::wstring GetPulseDataDir() { return storage::ConfigurationRoot(); }
}

namespace {

using pulse::app::SystemCtxModel;
using namespace pulse::app::sysctx;
namespace fs = std::filesystem;
int failures = 0;

void Check(bool ok, const char* name) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++failures;
}

void PrintStartupEnvironment() {
    for (const wchar_t* name : {L"TEMP", L"TMP"}) {
        std::wstring value(32768, L'\0');
        const DWORD length = GetEnvironmentVariableW(name, value.data(), static_cast<DWORD>(value.size()));
        std::printf("[INIT] %ls=%ls (length=%lu)\n", name,
                    length && length < value.size() ? value.c_str() : L"<unset or too long>", length);
    }
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        std::printf("[INIT] OpenProcessToken error=%lu\n", GetLastError());
        return;
    }
    const auto read_token = [&](TOKEN_INFORMATION_CLASS kind, void* data, DWORD bytes, const char* name) {
        DWORD size = 0;
        if (!GetTokenInformation(token, kind, data, bytes, &size))
            std::printf("[INIT] GetTokenInformation(%s) error=%lu\n", name, GetLastError());
    };
    TOKEN_ELEVATION elevation{MAXDWORD};
    DWORD container = MAXDWORD, virtualized = MAXDWORD, integrity = MAXDWORD;
    read_token(TokenElevation, &elevation, sizeof(elevation), "elevation");
    read_token(TokenIsAppContainer, &container, sizeof(container), "app container");
    read_token(TokenVirtualizationEnabled, &virtualized, sizeof(virtualized), "virtualization");
    DWORD size = 0;
    GetTokenInformation(token, TokenIntegrityLevel, nullptr, 0, &size);
    std::vector<BYTE> label(size);
    if (size && GetTokenInformation(token, TokenIntegrityLevel, label.data(), size, &size)) {
        const PSID sid = reinterpret_cast<const TOKEN_MANDATORY_LABEL*>(label.data())->Label.Sid;
        if (IsValidSid(sid) && *GetSidSubAuthorityCount(sid))
            integrity = *GetSidSubAuthority(sid, static_cast<DWORD>(*GetSidSubAuthorityCount(sid) - 1));
    }
    std::printf("[INIT] token: integrity=%lu elevated=%lu restricted=%d app_container=%lu virtualized=%lu\n",
                integrity, elevation.TokenIsElevated, IsTokenRestricted(token), container, virtualized);
    CloseHandle(token);
}

struct ScratchFixture {
    std::wstring registry, directory;
    bool owns_registry = false;
    bool owns_directory = false;
    bool cleaned = false;
    bool Cleanup() {
        if (cleaned) return true;
        cleaned = true;
        bool ok = true;
        if (owns_registry) {
            const LSTATUS deleted = RegDeleteTreeW(HKEY_CURRENT_USER, registry.c_str());
            ok = deleted == ERROR_SUCCESS || deleted == ERROR_FILE_NOT_FOUND;
            if (!ok) std::printf("[FAIL] fixture cleanup: RegDeleteTreeW error=%ld path=%ls\n", deleted, registry.c_str());
        }
        if (owns_directory) {
            std::error_code error;
            fs::remove_all(directory, error);
            if (error) std::printf("[FAIL] fixture cleanup: remove_all error=%d path=%ls\n", error.value(), directory.c_str());
            ok = !error && ok;
        }
        return ok;
    }
    ~ScratchFixture() {
        Cleanup();
        SetVelocitySupportedForTesting(-1);
    }
};

bool SetString(const std::wstring& key, const wchar_t* name, const std::wstring& value) {
    HKEY opened = nullptr;
    const LSTATUS created = RegCreateKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, nullptr, 0, KEY_SET_VALUE,
                                            nullptr, &opened, nullptr);
    if (created != ERROR_SUCCESS) {
        std::printf("[FAIL] fixture: RegCreateKeyExW error=%ld path=%ls\n", created, key.c_str());
        return false;
    }
    const LSTATUS written = RegSetValueExW(opened, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                                            static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(opened);
    if (written != ERROR_SUCCESS)
        std::printf("[FAIL] fixture: RegSetValueExW error=%ld path=%ls\n", written, key.c_str());
    return written == ERROR_SUCCESS;
}

bool HasKey(const std::wstring& key) {
    HKEY opened = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, KEY_READ, &opened) != ERROR_SUCCESS) return false;
    RegCloseKey(opened);
    return true;
}

bool WaitReady(SystemCtxModel& model) {
    const ULONGLONG deadline = GetTickCount64() + 10000;
    while (GetTickCount64() < deadline) {
        const auto view = model.View();
        if (view && view->state == pulse::ui::SysCtxView::State::Ready) return true;
        Sleep(5);
    }
    const auto view = model.View();
    if (view) std::printf("[FAIL] fixture wait timed out: state=%d rows=%zu pending=%d message=%ls\n",
                          static_cast<int>(view->state), view->rows.size(), view->pending, view->message.c_str());
    else std::printf("[FAIL] fixture wait timed out without a view model\n");
    return false;
}

int FindRow(SystemCtxModel& model, std::wstring_view suffix) {
    const auto view = model.View();
    for (size_t i = 0; i < view->rows.size(); ++i) {
        if (model.RowRegistryPath(static_cast<int>(i)).ends_with(suffix)) return static_cast<int>(i);
    }
    return -1;
}

bool Enabled(const Roots& roots, Root root, const std::wstring& name) {
    for (const auto& item : Enumerate(roots, BuiltinScenes().front()))
        if (item.root == root && item.key_name == name) return item.enabled;
    return false;
}

bool RejectSave(const pulse::app::ContextMenuPrefs&) { return false; }
bool AcceptSave(const pulse::app::ContextMenuPrefs&) { return true; }

size_t BackupCount(const std::wstring& directory) {
    std::error_code error;
    size_t count = 0;
    for (fs::directory_iterator it(directory, error), end; !error && it != end; it.increment(error))
        if (it->path().extension() == L".reg") ++count;
    return count;
}

} // namespace

int Run() {
    wchar_t temporary[MAX_PATH]{};
    const DWORD temporary_length = GetTempPathW(ARRAYSIZE(temporary), temporary);
    if (!temporary_length || temporary_length >= ARRAYSIZE(temporary)) {
        std::printf("[FAIL] fixture: GetTempPathW length=%lu capacity=%lu error=%lu\n", temporary_length,
                    static_cast<DWORD>(ARRAYSIZE(temporary)), temporary_length ? ERROR_INSUFFICIENT_BUFFER : GetLastError());
        return 2;
    }
    const std::wstring token = std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(GetTickCount64());
    const std::wstring directory = std::wstring(temporary) + L"PulseSystemCtxModel_" + token;
    const std::wstring registry = L"Software\\PulseTest\\SystemCtxModel_" + token;
    std::printf("[INIT] temporary=%ls fixture=%ls registry=HKCU\\%ls\n", temporary, directory.c_str(), registry.c_str());
    PrintStartupEnvironment();
    ScratchFixture fixture;
    fixture.directory = directory;
    fixture.registry = registry;
    HKEY probe = nullptr;
    DWORD disposition = 0;
    const LSTATUS probed = RegCreateKeyExW(HKEY_CURRENT_USER, registry.c_str(), 0, nullptr, 0,
                                          KEY_READ | KEY_WRITE | DELETE, nullptr, &probe, &disposition);
    if (probed != ERROR_SUCCESS) {
        std::printf("[FAIL] fixture: RegCreateKeyExW scratch root error=%ld path=%ls\n", probed, registry.c_str());
        return 2;
    }
    RegCloseKey(probe);
    if (disposition != REG_CREATED_NEW_KEY) {
        std::printf("[FAIL] fixture: scratch registry root already exists; refusing to reuse or remove it\n");
        return 2;
    }
    fixture.owns_registry = true;
    std::error_code fs_error;
    const bool directory_created = fs::create_directory(directory, fs_error);
    if (!directory_created || fs_error) {
        std::printf("[FAIL] fixture: create_directory path=%ls error=%d message=%s created=%d\n",
                    directory.c_str(), fs_error.value(), fs_error.message().c_str(), directory_created);
        return 2;
    }
    fixture.owns_directory = true;
    if (!fs::create_directory(directory + L"\\SendTo", fs_error) || fs_error) {
        std::printf("[FAIL] fixture: create_directory path=%ls\\SendTo error=%d message=%s\n",
                    directory.c_str(), fs_error.value(), fs_error.message().c_str());
        return 2;
    }
    pulse::storage::OverrideDefaultRootForTesting(directory + L"\\Storage");
    pulse::l10n::Initialize(GetModuleHandleW(nullptr), L"en-US");
    SetVelocitySupportedForTesting(1);

    Roots roots;
    roots.user = roots.machine = HKEY_CURRENT_USER;
    roots.user_classes = registry + L"\\User";
    roots.machine_classes = registry + L"\\Machine";
    roots.file_exts = registry + L"\\FileExts";
    roots.user_trash = registry + L"\\TrashUser";
    roots.machine_trash = registry + L"\\TrashMachine";
    roots.sendto_dir = directory + L"\\SendTo";
    roots.resolve_strings = roots.check_owner = false;
    const std::wstring user = roots.user_classes + L"\\*\\shell\\UserVerb";
    const std::wstring machine = roots.machine_classes + L"\\*\\shell\\MachineVerb";
    const bool seeded = SetString(user, L"MUIVerb", L"User command") &&
              SetString(user + L"\\command", nullptr, L"C:\\PulseSystemCtxMissing\\gone.exe") &&
              SetString(machine, L"MUIVerb", L"Machine command") &&
              SetString(machine + L"\\command", nullptr, L"C:\\PulseSystemCtxMissing\\gone.exe");
    Check(seeded, "create only isolated user and simulated machine registrations");
    if (!seeded) return 2;
    const std::wstring blocker = directory + L"\\Blocked";
    HANDLE blocked = CreateFileW(blocker.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, 0, nullptr);
    Check(blocked != INVALID_HANDLE_VALUE, "create backup directory obstruction");
    if (blocked == INVALID_HANDLE_VALUE) {
        std::printf("[FAIL] fixture: CreateFileW error=%lu path=%ls\n", GetLastError(), blocker.c_str());
        return 2;
    }
    CloseHandle(blocked);

    {
        SystemCtxModel model;
        Check(!model.SetRegistryRootsForTesting(Roots{}), "test root override refuses production registry roots");
        const bool isolated = model.SetRegistryRootsForTesting(roots);
        Check(isolated, "model accepts isolated registry and file roots");
        if (!isolated) return 1;
        model.SetDataDirectoryForTesting(directory + L"\\Data");
        model.EnsureLoaded();
        const bool ready = WaitReady(model);
        Check(ready, "background scan completes");
        if (!ready) return 1;
        model.SelectScene(0);
        Check(model.View()->rows.size() == 2 && Enabled(roots, Root::User, L"UserVerb") &&
                  Enabled(roots, Root::Machine, L"MachineVerb"),
              "scan exposes both scopes without changing registrations");
        const int user_row = FindRow(model, L"UserVerb");
        const int machine_row = FindRow(model, L"MachineVerb");
        Check(user_row >= 0 && machine_row >= 0, "row registry paths use the actual isolated roots");
        model.ToggleRow(user_row);
        model.ToggleRow(machine_row);
        Check(model.View()->pending == 2 && model.View()->pending_admin == 1 &&
                  Enabled(roots, Root::User, L"UserVerb") && Enabled(roots, Root::Machine, L"MachineVerb"),
              "queue both scopes without writing before Apply");

        model.SetDataDirectoryForTesting(blocker + L"\\Child");
        model.ApplyPending();
        Check(WaitReady(model), "backup failure completes the worker");
        auto view = model.View();
        Check(view->pending == 2 && view->undo_steps == 0 && view->message_error &&
                  view->message.find(L"Registry backup failed") != std::wstring::npos &&
                  view->message.find(L"(") != std::wstring::npos && Enabled(roots, Root::User, L"UserVerb") &&
                  Enabled(roots, Root::Machine, L"MachineVerb"),
              "failed backup reports a Windows error, preserves whole queue and writes neither scope");

        model.SetDataDirectoryForTesting(directory + L"\\Data");
        model.ApplyPending();
        Check(WaitReady(model), "retry after fixing backup destination completes");
        view = model.View();
        Check(view->pending == 0 && view->undo_steps == 1 && !view->message_error &&
                  !Enabled(roots, Root::User, L"UserVerb") && !Enabled(roots, Root::Machine, L"MachineVerb") &&
                  BackupCount(model.BackupDirectory()) == 1,
              "retry backs up the full batch before applying user and simulated machine changes");

        model.SetDataDirectoryForTesting(blocker + L"\\Child");
        model.Undo();
        Check(WaitReady(model) && model.View()->undo_steps == 1 && model.View()->message_error &&
                  !Enabled(roots, Root::User, L"UserVerb") && !Enabled(roots, Root::Machine, L"MachineVerb"),
              "failed undo backup preserves undo step and both current registrations");
        model.SetDataDirectoryForTesting(directory + L"\\Data");
        model.Undo();
        Check(WaitReady(model) && model.View()->undo_steps == 0 && Enabled(roots, Root::User, L"UserVerb") &&
                  Enabled(roots, Root::Machine, L"MachineVerb"),
              "undo retries successfully and restores both registrations");

        model.TogglePulseRow(FindRow(model, L"UserVerb"));
        const auto first = model.PendingPulseChanges();
        const auto second = model.PendingPulseChanges();
        model.ReportError(L"Save failed");
        Check(first.hidden.size() == 1 && first.hidden == second.hidden && model.View()->pending_pulse == 1 &&
                  model.View()->message_error && model.PendingPulseChanges().hidden == first.hidden,
              "reading Pulse changes and reporting a save failure never consumes pending settings");
        pulse::app::ContextMenuPrefs prefs;
        const auto before_preferences = prefs.item_enabled;
        model.ToggleRow(FindRow(model, L"MachineVerb"));
        Check(!pulse::ApplySystemContextChanges(model, prefs, RejectSave) &&
                  prefs.item_enabled == before_preferences && model.View()->pending == 2 &&
                  model.View()->pending_pulse == 1 && model.View()->state == pulse::ui::SysCtxView::State::Ready &&
                  model.View()->message_error && Enabled(roots, Root::Machine, L"MachineVerb"),
              "failed Pulse preference save preserves live settings and both queues before any registry write");
        const bool applied = pulse::ApplySystemContextChanges(model, prefs, AcceptSave) && WaitReady(model);
        const auto saved = first.hidden.empty() ? prefs.item_enabled.end() : prefs.item_enabled.find(first.hidden.front().first);
        Check(applied &&
                  model.PendingPulseChanges().empty() && model.View()->pending == 0 &&
                  !model.View()->message_error && !Enabled(roots, Root::Machine, L"MachineVerb") &&
                  saved != prefs.item_enabled.end() && !saved->second,
              "successful preference save commits Pulse settings and then applies the registry queue");
        model.Undo();
        Check(WaitReady(model) && model.View()->undo_steps == 0 && Enabled(roots, Root::Machine, L"MachineVerb"),
              "undo restores the registry portion of a mixed Pulse preference batch");

        const int leftover = FindRow(model, L"UserVerb");
        Check(model.RowDeletable(leftover), "missing program registration exposes leftover removal");
        model.DeleteRow(leftover);
        Check(model.View()->pending == 1 && HasKey(user), "leftover removal waits in the queue");
        model.ApplyPending();
        Check(WaitReady(model) && !HasKey(user) && HasKey(roots.user_trash + L"\\*\\shell\\UserVerb") &&
                  model.View()->undo_steps == 1,
              "leftover removal moves the complete key to isolated recoverable trash");
        model.Undo();
        Check(WaitReady(model) && HasKey(user + L"\\command") && model.View()->undo_steps == 0,
              "undo restores the leftover registration and its command subtree");

        model.ToggleRow(FindRow(model, L"UserVerb"));
        model.ToggleRow(FindRow(model, L"MachineVerb"));
        Check(RegDeleteTreeW(HKEY_CURRENT_USER, machine.c_str()) == ERROR_SUCCESS,
              "simulate a registration disappearing after scan");
        const size_t previous_backups = BackupCount(model.BackupDirectory());
        model.ApplyPending();
        const bool missing_ready = WaitReady(model);
        const auto missing_view = model.View();
        const bool user_unchanged = Enabled(roots, Root::User, L"UserVerb");
        const size_t remaining_backups = BackupCount(model.BackupDirectory());
        const bool export_aborted = missing_ready && missing_view->pending == 2 && missing_view->message_error &&
                                    missing_view->message.find(L"(2)") != std::wstring::npos &&
                                    user_unchanged && remaining_backups == previous_backups;
        Check(export_aborted,
              "failure exporting a later key aborts before the earlier key changes and preserves all pending ops");
        if (!export_aborted)
            std::printf("[DIAG] missing-key export: ready=%d pending=%d error=%d user_enabled=%d backups_before=%zu backups_after=%zu message=%ls\n",
                        missing_ready, missing_view->pending, missing_view->message_error, user_unchanged,
                        previous_backups, remaining_backups, missing_view->message.c_str());
        model.DiscardPending();
        Check(model.View()->pending == 0, "explicit discard clears the failed queue");
    }

    {
        Roots recovery_roots = roots;
        recovery_roots.user_classes = registry + L"\\RecoveryUser";
        recovery_roots.machine_classes = registry + L"\\RecoveryMachine";
        recovery_roots.value_write_fault_for_testing = std::make_shared<ValueWriteFaultForTesting>();
        auto& fault = *recovery_roots.value_write_fault_for_testing;
        Check(SetString(recovery_roots.user_classes + L"\\*\\shell\\RecoveryUser", L"MUIVerb", L"User") &&
              SetString(recovery_roots.machine_classes + L"\\*\\shell\\RecoveryMachine", L"MUIVerb", L"Machine"),
              "create isolated registrations for rollback and elevated-outcome faults");
        SystemCtxModel model;
        if (!model.SetRegistryRootsForTesting(recovery_roots)) return 2;
        model.SetDataDirectoryForTesting(directory + L"\\RecoveryData");
        model.EnsureLoaded();
        if (!WaitReady(model)) return 2;
        model.SelectScene(0);
        model.ToggleRow(FindRow(model, L"RecoveryUser"));
        fault = {};
        fault.fail_write_call = 2;
        model.ApplyPending();
        Check(WaitReady(model) && model.View()->pending == 1 && model.View()->undo_steps == 0 &&
              model.View()->message_error && Enabled(recovery_roots, Root::User, L"RecoveryUser"),
              "a single-op second-write failure rolls back and retains its pending retry without false undo");
        fault = {};
        model.ApplyPending();
        Check(WaitReady(model) && model.View()->pending == 0 && model.View()->undo_steps == 1 &&
              !Enabled(recovery_roots, Root::User, L"RecoveryUser"), "the rolled-back operation retries successfully");
        fault = {};
        fault.fail_write_call = 2;
        model.Undo();
        Check(WaitReady(model) && model.View()->undo_steps == 1 && model.View()->message_error &&
              !Enabled(recovery_roots, Root::User, L"RecoveryUser"),
              "a mid-undo failure restores the disabled state and preserves the original undo step");
        fault = {};
        model.Undo();
        Check(WaitReady(model) && model.View()->undo_steps == 0 && Enabled(recovery_roots, Root::User, L"RecoveryUser"),
              "retrying the failed undo restores the original registration");
        model.ToggleRow(FindRow(model, L"RecoveryUser"));
        fault = {};
        fault.fail_write_call = 2;
        fault.fail_rollback_call = 2;
        model.ApplyPending();
        Check(WaitReady(model) && model.View()->pending == 0 && model.View()->undo_steps == 1 &&
              model.View()->message_error && model.View()->message.find(L"unknown final state") != std::wstring::npos &&
              !Enabled(recovery_roots, Root::User, L"RecoveryUser"),
              "failed rollback reports uncertainty and records UI recovery instead of dropping partial writes");
        fault = {};
        model.Undo();
        Check(WaitReady(model) && model.View()->undo_steps == 0 && Enabled(recovery_roots, Root::User, L"RecoveryUser"),
              "UI undo recovers a partially written forward operation");

        Check(model.SetElevatedRunnerForTesting([](const auto& ops, auto& results, std::wstring& error) {
            results.assign(ops.size(), OpResult::Failed);
            error = L"Simulated cancelled administrator permission";
            return false;
        }), "inject cancelled UAC only into an isolated model");
        model.ToggleRow(FindRow(model, L"RecoveryUser"));
        model.ToggleRow(FindRow(model, L"RecoveryMachine"));
        model.ApplyPending();
        Check(WaitReady(model) && model.View()->pending == 1 && model.View()->undo_steps == 1 &&
              !Enabled(recovery_roots, Root::User, L"RecoveryUser") &&
              Enabled(recovery_roots, Root::Machine, L"RecoveryMachine") &&
              model.View()->message.find(L"current-user change(s) completed") != std::wstring::npos,
              "cancelled UAC retains machine pending work and the completed user's undo");
        model.DiscardPending();
        model.Undo();
        Check(WaitReady(model) && model.View()->undo_steps == 0 && Enabled(recovery_roots, Root::User, L"RecoveryUser"),
              "completed user changes remain undoable after cancelling elevation");
        Check(model.SetElevatedRunnerForTesting([&](const auto& ops, auto& results, std::wstring& error) {
            if (ops.empty() || Apply(recovery_roots, ops.front()) != OpResult::Ok) return false;
            results.assign(ops.size(), OpResult::RecoveryRequired);
            error = L"Simulated helper final-result write failure";
            return false;
        }), "inject result loss after an actual isolated machine-scope mutation");
        model.ToggleRow(FindRow(model, L"RecoveryUser"));
        model.ToggleRow(FindRow(model, L"RecoveryMachine"));
        model.ApplyPending();
        Check(WaitReady(model) && model.View()->pending == 0 && model.View()->undo_steps == 1 &&
              model.View()->message_error && model.View()->message.find(L"unknown final state") != std::wstring::npos &&
              !Enabled(recovery_roots, Root::User, L"RecoveryUser") &&
              !Enabled(recovery_roots, Root::Machine, L"RecoveryMachine"),
              "lost elevated results preserve both known user successes and uncertain machine recovery");
        Check(model.SetElevatedRunnerForTesting({}), "restore normal isolated elevation execution");
        model.Undo();
        Check(WaitReady(model) && model.View()->undo_steps == 0 && Enabled(recovery_roots, Root::User, L"RecoveryUser") &&
              Enabled(recovery_roots, Root::Machine, L"RecoveryMachine"),
              "one UI undo restores mixed known and uncertain changes from their original snapshots");
    }

    {
        Roots budget_roots = roots;
        budget_roots.user_classes = registry + L"\\BudgetUser";
        budget_roots.machine_classes = registry + L"\\BudgetMachine";
        const size_t count = kMaxBatchOperations + 1;
        bool created = true;
        for (size_t i = 0; i < count; ++i) {
            created = SetString(budget_roots.user_classes + L"\\*\\shell\\Budget" + std::to_wstring(i),
                                L"MUIVerb", L"Budget " + std::to_wstring(i)) && created;
        }
        Check(created, "create an isolated registry batch just beyond the helper limit");
        if (!created) return 2;
        SystemCtxModel model;
        const bool isolated = model.SetRegistryRootsForTesting(budget_roots);
        Check(isolated, "use isolated registry roots for batch budget");
        if (!isolated) return 1;
        model.SetDataDirectoryForTesting(directory + L"\\BudgetData");
        model.EnsureLoaded();
        const bool ready = WaitReady(model);
        Check(ready, "batch budget scan completes");
        if (!ready) return 1;
        model.SelectScene(0);
        bool queued = true;
        for (size_t i = 0; i < count; ++i) {
            const int row = FindRow(model, L"\\Budget" + std::to_wstring(i));
            queued = row >= 0 && queued;
            if (row >= 0) model.ToggleRow(row);
        }
        Check(queued && model.View()->pending == static_cast<int>(count), "queue every oversized batch entry");
        model.ApplyPending();
        const auto view = model.View();
        const auto entries = Enumerate(budget_roots, BuiltinScenes().front());
        bool unchanged = entries.size() == count;
        for (const auto& entry : entries) unchanged = entry.enabled && unchanged;
        Check(view->state == pulse::ui::SysCtxView::State::Ready && view->message_error &&
                  view->pending == static_cast<int>(count) && view->undo_steps == 0 && unchanged &&
                  BackupCount(model.BackupDirectory()) == 0,
              "reject an oversized batch before backup or registry writes and preserve the whole queue");
    }

    Check(fixture.Cleanup() && !HasKey(registry) && !fs::exists(directory), "isolated registry and filesystem fixtures are removed");
    std::printf("%s (%d failures)\n", failures ? "FAILED" : "ALL PASSED", failures);
    return failures ? 1 : 0;
}

int wmain() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("[INIT] system context model test starting\n");
    try { return Run(); }
    catch (const std::exception& error) {
        std::printf("[FAIL] fixture exception: %s\n", error.what());
        return 2;
    }
}
