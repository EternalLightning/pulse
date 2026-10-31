// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "../app/system_ctx_registry.h"
#include "../app/system_ctx_model.h"
#include "../app/places.h"
#include "../common/localization.h"
#include <aclapi.h>
#include <sddl.h>
#include <cstdio>
#include <filesystem>

namespace {
std::wstring fixture_directory;
int failures = 0;
void Check(bool ok, const char* name) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
    failures += !ok;
}
}
// The model's default storage and language queries stay inside the fixture.
namespace pulse::storage { std::wstring ConfigurationRoot() { return fixture_directory; } }
namespace pulse::app { std::wstring GetPulseDataDir() { return fixture_directory; } }
namespace pulse::l10n { Language effective_language() noexcept { return Language::EnUS; } }

namespace {
using namespace pulse::app::sysctx;
using pulse::app::SystemCtxModel;
namespace fs = std::filesystem;

bool Set(const std::wstring& path, const wchar_t* name, const std::wstring& value) {
    HKEY key = nullptr;
    const auto status = RegCreateKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, nullptr, 0,
                                       KEY_ALL_ACCESS, nullptr, &key, nullptr);
    if (status != ERROR_SUCCESS) return false;
    const auto write = RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                                     static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
    return write == ERROR_SUCCESS;
}
bool Has(const std::wstring& path) {
    HKEY key = nullptr;
    const auto status = RegOpenKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, KEY_READ, &key);
    if (key) RegCloseKey(key);
    return status == ERROR_SUCCESS;
}
bool Is(const std::wstring& path, const wchar_t* name, const std::wstring& value) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) return false;
    wchar_t text[1024]{}; DWORD bytes = sizeof(text), type = 0;
    const auto status = RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<BYTE*>(text), &bytes);
    RegCloseKey(key);
    return status == ERROR_SUCCESS && type == REG_SZ && text == value;
}
bool Binary(const std::wstring& path, bool write) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, KEY_ALL_ACCESS, &key) != ERROR_SUCCESS) return false;
    const BYTE expected[]{0, 0xff, 0, 0x34};
    bool ok = false;
    if (write) ok = RegSetValueExW(key, L"Binary", 0, REG_BINARY, expected, sizeof(expected)) == ERROR_SUCCESS;
    else {
        BYTE value[sizeof(expected)]{}; DWORD bytes = sizeof(value), type = 0;
        ok = RegQueryValueExW(key, L"Binary", nullptr, &type, value, &bytes) == ERROR_SUCCESS &&
             type == REG_BINARY && bytes == sizeof(value) && memcmp(value, expected, sizeof(value)) == 0;
    }
    RegCloseKey(key); return ok;
}
bool Seed(const std::wstring& path) {
    HKEY empty = nullptr;
    const auto status = RegCreateKeyExW(HKEY_CURRENT_USER, (path + L"\\empty").c_str(), 0, nullptr, 0,
                                       KEY_ALL_ACCESS, nullptr, &empty, nullptr);
    if (empty) RegCloseKey(empty);
    return status == ERROR_SUCCESS && Set(path, nullptr, L"original") && Set(path, L"Named", L"kept") &&
           Set(path + L"\\command", nullptr, L"C:\\PulseMissing_20261030\\gone.exe \"%1\"") && Binary(path, true);
}
bool Full(const std::wstring& path) {
    return Is(path, nullptr, L"original") && Is(path, L"Named", L"kept") &&
           Is(path + L"\\command", nullptr, L"C:\\PulseMissing_20261030\\gone.exe \"%1\"") &&
           Has(path + L"\\empty") && Binary(path, false);
}
bool Ready(SystemCtxModel& model) {
    const auto deadline = GetTickCount64() + 10000;
    while (GetTickCount64() < deadline) {
        const auto view = model.View();
        if (view && view->state == pulse::ui::SysCtxView::State::Ready) return true;
        Sleep(5);
    }
    return false;
}
int Row(SystemCtxModel& model, const std::wstring& name) {
    const auto view = model.View();
    for (size_t i = 0; i < view->rows.size(); ++i)
        if (model.RowRegistryPath(static_cast<int>(i)).ends_with(L"\\" + name)) return static_cast<int>(i);
    return -1;
}
void Reset(const Roots& roots, const std::wstring& relative) {
    RegDeleteTreeW(HKEY_CURRENT_USER, (roots.user_classes + L"\\" + relative).c_str());
    RegDeleteTreeW(HKEY_CURRENT_USER, (roots.user_trash + L"\\" + relative).c_str());
    Check(Seed(roots.user_classes + L"\\" + relative), "seed exact nested registry fixture");
    *roots.tree_move_fault_for_testing = {};
}
Op Delete(const std::wstring& relative) {
    Op op; op.kind = Kind::Verb; op.root = Root::User; op.action = Action::Delete; op.key_path = relative;
    return op;
}
Op Recover(const Op& op) { Op recover = op; recover.recover_tree = true; return Inverse(recover); }

void Ads() {
    const auto file = fixture_directory + L"\\ads.txt";
    HANDLE base = CreateFileW(file.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW, 0, nullptr);
    const char body[] = "original file identity"; DWORD written = 0;
    Check(base != INVALID_HANDLE_VALUE && WriteFile(base, body, sizeof(body), &written, nullptr) &&
          written == sizeof(body), "create isolated NTFS base file");
    BY_HANDLE_FILE_INFORMATION before{};
    GetFileInformationByHandle(base, &before); CloseHandle(base);
    std::vector<pulse::model::TagAdsRecord> tags{{L"id", L"Tag", 0x123456}};
    Check(pulse::model::WriteTagAdsV2(file, tags), "write real tag ADS");
    HANDLE stream = CreateFileW((file + L":Pulse.Tag").c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                 nullptr, OPEN_EXISTING, 0, nullptr);
    const bool deleted = pulse::model::WriteTagAdsV2(file, {});
    const DWORD error = GetLastError();
    Check(stream != INVALID_HANDLE_VALUE && !deleted && error == ERROR_SHARING_VIOLATION,
          "clearing last tag reports ADS sharing failure instead of success");
    Check(pulse::model::ReadTagAdsV2(file).size() == 1, "failed deletion retains readable ADS recovery material");
    CloseHandle(stream);
    Check(pulse::model::WriteTagAdsV2(file, {}) && pulse::model::ReadTagAdsV2(file).empty() &&
          pulse::model::WriteTagAdsV2(file, {}), "retry deletes ADS; explicitly absent stream is successful");
    base = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    BY_HANDLE_FILE_INFORMATION after{}; char read[sizeof(body)]{}; DWORD count = 0;
    Check(GetFileInformationByHandle(base, &after) && before.dwVolumeSerialNumber == after.dwVolumeSerialNumber &&
          before.nFileIndexHigh == after.nFileIndexHigh && before.nFileIndexLow == after.nFileIndexLow &&
          ReadFile(base, read, sizeof(read), &count, nullptr) && count == sizeof(body) &&
          memcmp(read, body, sizeof(body)) == 0, "ADS failure and retry preserve base file identity and bytes");
    CloseHandle(base);
}

void Qualification(Roots roots, const std::wstring& executable) {
    const auto good = roots.user_classes + L"\\*\\shell\\Working";
    const auto gone = roots.user_classes + L"\\*\\shell\\Gone";
    Check(Set(good + L"\\command", nullptr, L"\"" + executable + L"\" \"%1\"") &&
          Set(good, L"Icon", fixture_directory + L"\\absent.ico") && Seed(gone), "seed working command with absent icon and absent command");
    auto items = Enumerate(roots, BuiltinScenes().front());
    bool working = false, missing = false;
    for (const auto& item : items) {
        if (item.key_name == L"Working") working = !item.missing && item.target_state == TargetState::Present &&
                                                    item.icon_state == TargetState::Missing;
        if (item.key_name == L"Gone") missing = item.missing && item.target_state == TargetState::Missing;
    }
    Check(working && missing, "registry enumeration separates absent icon from absent execution target");
    {
        SystemCtxModel model;
        Check(model.SetRegistryRootsForTesting(roots), "model accepts only isolated registry roots");
        model.SetDataDirectoryForTesting(fixture_directory + L"\\Qualification");
        model.EnsureLoaded(); Check(Ready(model), "model scan completes"); model.SelectScene(0);
        const int good_row = Row(model, L"Working"), gone_row = Row(model, L"Gone");
        Check(good_row >= 0 && gone_row >= 0 && !model.RowDeletable(good_row) && model.RowDeletable(gone_row) &&
              model.View()->rows[static_cast<size_t>(good_row)].subtitle.find(L"Icon missing") != std::wstring::npos,
              "model retains working command, explains absent icon and allows only absent program cleanup");
        Check(model.DeleteSceneLeftovers() == 1 && !model.RowDeleting(good_row), "bulk cleanup queues only the confirmed absent program");
    }
    // Make a handler CLSID unreadable; access denied must not become absence.
    const std::wstring clsid = L"{A288BD67-B524-4A16-BB89-8887A3CE1184}";
    const auto cls = roots.user_classes + L"\\CLSID\\" + clsid;
    Check(Set(cls + L"\\InprocServer32", nullptr, executable) &&
          Set(roots.user_classes + L"\\*\\shellex\\ContextMenuHandlers\\Denied", nullptr, clsid), "seed unreadable handler fixture");
    HKEY key = nullptr; RegOpenKeyExW(HKEY_CURRENT_USER, cls.c_str(), 0, KEY_ALL_ACCESS, &key);
    DWORD bytes = 0; RegGetKeySecurity(key, DACL_SECURITY_INFORMATION, nullptr, &bytes);
    std::vector<BYTE> previous(bytes);
    const bool saved = RegGetKeySecurity(key, DACL_SECURITY_INFORMATION,
        reinterpret_cast<PSECURITY_DESCRIPTOR>(previous.data()), &bytes) == ERROR_SUCCESS;
    PSECURITY_DESCRIPTOR denied = nullptr;
    const bool converted = ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(D;;KR;;;WD)", SDDL_REVISION_1, &denied, nullptr) != FALSE;
    const bool secured = saved && converted && RegSetKeySecurity(key, DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, denied) == ERROR_SUCCESS;
    bool unavailable = false;
    if (secured) for (const auto& item : Enumerate(roots, BuiltinScenes().front()))
        if (item.key_name == L"Denied") unavailable = !item.missing && item.target_state == TargetState::Unavailable;
    Check(secured && unavailable, "access-denied CLSID is unavailable and never a cleanup leftover");
    Check(saved && RegSetKeySecurity(key, DACL_SECURITY_INFORMATION,
          reinterpret_cast<PSECURITY_DESCRIPTOR>(previous.data())) == ERROR_SUCCESS, "restore isolated handler DACL before cleanup");
    if (denied) LocalFree(denied); if (key) RegCloseKey(key);
    RegDeleteTreeW(HKEY_CURRENT_USER, good.c_str()); RegDeleteTreeW(HKEY_CURRENT_USER, gone.c_str());
    RegDeleteTreeW(HKEY_CURRENT_USER, (roots.user_classes + L"\\*\\shellex").c_str());
}

void Moves(Roots roots) {
    const std::wstring relative = L"*\\shell\\Move";
    const auto live = roots.user_classes + L"\\" + relative, trash = roots.user_trash + L"\\" + relative;
    const Op op = Delete(relative);
    auto& fault = *roots.tree_move_fault_for_testing;
    Reset(roots, relative);
    Check(Apply(roots, op) == OpResult::Ok && !Has(live) && Full(trash) &&
          Apply(roots, Inverse(op)) == OpResult::Ok && Full(live) && !Has(trash), "normal delete and undo preserve complete tree");
    Reset(roots, relative); fault.fail_copy_call = 1; fault.partial_copy = true;
    Check(Apply(roots, op) == OpResult::Failed && Full(live) && !Has(trash), "initial partial copy with successful cleanup is ordinary failure");
    Reset(roots, relative); fault.fail_copy_call = 1; fault.partial_copy = true; fault.fail_delete_call = 1;
    const auto partial_copy = Apply(roots, op); const DWORD copy_error = GetLastError();
    Check(partial_copy == OpResult::RecoveryRequired && copy_error == ERROR_WRITE_FAULT && Full(live) && Has(trash),
          "failed partial-copy cleanup retains both paths and original write error");
    Op inverse = Recover(op); std::vector<Op> parsed;
    Check(ParseBatch(SerializeBatch({inverse}), parsed) && parsed.size() == 1 && parsed[0].recover_tree,
          "explicit recovery mode survives helper batch serialization");
    const auto backup = ExportReg(roots, {inverse});
    Check(!backup.empty() && backup.find(live) != std::wstring::npos && backup.find(trash) != std::wstring::npos,
          "recovery backup includes both partial and complete trees");
    fault = {};
    Check(Apply(roots, inverse) == OpResult::Ok && Full(live) && !Has(trash), "inverse retires partial copy while retaining complete original");
    Reset(roots, relative); fault.fail_delete_call = 1; fault.partial_delete = true; fault.fail_copy_call = 2;
    const auto partial_delete = Apply(roots, op); const DWORD delete_error = GetLastError();
    Check(partial_delete == OpResult::RecoveryRequired && delete_error == ERROR_ACCESS_DENIED && !Full(live) && Full(trash),
          "partial source deletion and failed rollback retain complete recovery copy");
    fault = {};
    Check(Apply(roots, inverse) == OpResult::Ok && Full(live) && !Has(trash), "inverse repairs partial original from full copy without conflict");
    Reset(roots, relative); fault.fail_delete_call = 1; fault.fail_second_delete_call = 2; fault.partial_delete = true;
    Check(Apply(roots, op) == OpResult::RecoveryRequired && Full(live) && Has(trash), "rollback cleanup failure is RecoveryRequired with original restored");
    fault = {};
    Check(Apply(roots, inverse) == OpResult::Ok && Full(live) && !Has(trash), "recovery handles a partially cleaned rollback copy");
    Reset(roots, relative); fault.fail_delete_call = 1; fault.partial_delete = true; fault.fail_copy_call = 2;
    Check(Apply(roots, op) == OpResult::RecoveryRequired && Set(live, L"Named", L"external"), "inject external edit after uncertain move");
    fault = {};
    Check(Apply(roots, inverse) == OpResult::Conflict && Is(live, L"Named", L"external") && Full(trash),
          "conflicting external value preserves both trees and recovery material");
    Reset(roots, relative); fault.fail_copy_call = 1; fault.partial_copy = true; fault.fail_delete_call = 1;
    Check(Apply(roots, op) == OpResult::RecoveryRequired, "seed recovery cleanup retry");
    fault = {}; fault.fail_delete_call = 1; fault.partial_delete = true;
    Check(Apply(roots, inverse) == OpResult::RecoveryRequired && Full(live) && Has(trash),
          "failed recovery retirement keeps complete restored original");
    fault = {};
    Check(Apply(roots, inverse) == OpResult::Ok && Full(live) && !Has(trash), "a second recovery retry completes safely");
    Check(!ExportReg(roots, {inverse}).empty() && Apply(roots, inverse) == OpResult::Ok && Full(live),
          "unknown outcome with original already restored has a backup and idempotent inverse");
    RegDeleteTreeW(HKEY_CURRENT_USER, live.c_str());
    Check(!ParseBatch(L"PULSE-SYSCTX 2\nenable\tverb\tuser\t*\\shell\\Move\trecover\t-\n", parsed),
          "helper rejects recovery mode for a value-marker operation");
    for (const Kind kind : {Kind::ShellEx, Kind::DragDrop}) {
        const std::wstring group = kind == Kind::ShellEx ? L"ContextMenuHandlers" : L"DragDropHandlers";
        Op toggle; toggle.kind = kind; toggle.root = Root::User; toggle.action = Action::Disable;
        toggle.key_path = L"*\\shellex\\" + group + L"\\MoveHandler";
        const auto original = roots.user_classes + L"\\" + toggle.key_path;
        const auto moved = roots.user_classes + L"\\" + KeyPathAfter(toggle);
        Check(Seed(original), "seed recoverable handler move");
        fault = {}; fault.fail_delete_call = 1; fault.partial_delete = true; fault.fail_copy_call = 2;
        Check(Apply(roots, toggle) == OpResult::RecoveryRequired && Full(moved),
              "handler move failure retains its full dashed-group copy");
        const Op restore = Recover(toggle);
        Check(ParseBatch(SerializeBatch({restore}), parsed) && parsed.front().recover_tree,
              "handler recovery inverse survives helper serialization");
        fault = {};
        Check(Apply(roots, restore) == OpResult::Ok && Full(original) && !Has(moved),
              "handler recovery restores original group and prunes moved copy");
        RegDeleteTreeW(HKEY_CURRENT_USER, original.c_str());
    }
}

void ModelRecovery(Roots roots) {
    const std::wstring relative = L"*\\shell\\ModelRecovery";
    const auto live = roots.user_classes + L"\\" + relative, trash = roots.user_trash + L"\\" + relative;
    Reset(roots, relative);
    auto& fault = *roots.tree_move_fault_for_testing;
    {
        SystemCtxModel model;
        Check(model.SetRegistryRootsForTesting(roots), "recovery model uses isolated roots");
        model.SetDataDirectoryForTesting(fixture_directory + L"\\RecoveryModel");
        model.EnsureLoaded(); Check(Ready(model), "recovery model scan completes"); model.SelectScene(0);
        fault.fail_delete_call = 1; fault.partial_delete = true; fault.fail_copy_call = 2;
        model.DeleteRow(Row(model, L"ModelRecovery")); model.ApplyPending();
        Check(Ready(model) && model.View()->message_error && model.View()->undo_steps == 1 &&
              model.View()->pending == 0 && !Full(live) && Full(trash), "model records recovery state and retains undo after uncertain tree move");
        fault = {}; model.Undo();
        Check(Ready(model) && !model.View()->message_error && model.View()->undo_steps == 0 && Full(live) && !Has(trash),
              "model undo restores partial tree using recorded recovery mode");
        model.DeleteRow(Row(model, L"ModelRecovery")); model.ApplyPending();
        Check(Ready(model) && model.View()->undo_steps == 1 && Full(trash), "model ordinary delete succeeds before failing undo");
        fault = {}; fault.fail_delete_call = 1; fault.partial_delete = true; fault.fail_copy_call = 2;
        model.Undo();
        Check(Ready(model) && model.View()->message_error && model.View()->undo_steps == 1 && Full(live) && !Full(trash),
              "failed ordinary undo keeps original recovery record");
        fault = {}; model.Undo();
        Check(Ready(model) && !model.View()->message_error && model.View()->undo_steps == 0 && Full(live) && !Has(trash),
              "retry undo upgrades the retained record to recovery mode and completes");
    }
}
}
int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return 2;
    const std::wstring token = std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(GetTickCount64());
    fixture_directory = std::wstring(argv[1]) + L"\\integrations_" + token;
    if (!CreateDirectoryW(fixture_directory.c_str(), nullptr)) return 2;
    const std::wstring registry = L"Software\\PulseTest\\Integrations_" + token;
    HKEY key = nullptr; DWORD disposition = 0;
    const auto created = RegCreateKeyExW(HKEY_CURRENT_USER, registry.c_str(), 0, nullptr, 0, KEY_ALL_ACCESS,
                                         nullptr, &key, &disposition);
    if (created != ERROR_SUCCESS || disposition != REG_CREATED_NEW_KEY) {
        if (key) RegCloseKey(key);
        std::printf("[FAIL] exclusive private root creation status=%ld disposition=%lu\n", created, disposition);
        return 2;
    }
    RegCloseKey(key);
    std::printf("[INIT] registry=HKCU\\%ls files=%ls\n", registry.c_str(), fixture_directory.c_str());
    Roots roots; roots.user = roots.machine = HKEY_CURRENT_USER;
    roots.user_classes = registry + L"\\User"; roots.machine_classes = registry + L"\\Machine";
    roots.user_trash = registry + L"\\TrashUser"; roots.machine_trash = registry + L"\\TrashMachine";
    roots.file_exts = registry + L"\\FileExts"; roots.sendto_dir = fixture_directory + L"\\SendTo";
    roots.resolve_strings = roots.check_owner = false;
    roots.tree_move_fault_for_testing = std::make_shared<TreeMoveFaultForTesting>();
    wchar_t executable[32768]{}; GetModuleFileNameW(nullptr, executable, ARRAYSIZE(executable));
    Ads(); Qualification(roots, executable); Moves(roots); ModelRecovery(roots);
    const auto removed = RegDeleteTreeW(HKEY_CURRENT_USER, registry.c_str());
    HKEY verify = nullptr;
    const auto absent = RegOpenKeyExW(HKEY_CURRENT_USER, registry.c_str(), 0, KEY_READ, &verify);
    if (verify) RegCloseKey(verify);
    Check(removed == ERROR_SUCCESS && absent == ERROR_FILE_NOT_FOUND, "private registry root cleanup is verified by reopening");
    std::error_code error; fs::remove_all(fixture_directory, error);
    Check(!error && !fs::exists(fixture_directory), "unique filesystem fixtures and test backups are removed");
    std::printf("[RESULT] failures=%d\n", failures);
    return failures ? 1 : 0;
}
