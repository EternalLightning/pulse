#include "../ops/operation_elevation_adapter.h"
#include "../common/localization.h"
#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
#include <cstdio>
#include <filesystem>
#include <fstream>

#ifndef PULSE_OPERATION_ELEVATION_TESTING
#error This fixture must define PULSE_OPERATION_ELEVATION_TESTING.
#endif

namespace {
using namespace pulse::ops;
namespace fs = std::filesystem;
int failures = 0;
void Check(bool success, const char* message) {
    std::printf("[%s] %s\n", success ? "PASS" : "FAIL", message);
    if (!success)
        ++failures;
}
std::wstring SelfPath() {
    std::wstring value(32768, L'\0');
    const DWORD count = GetModuleFileNameW(nullptr, value.data(), static_cast<DWORD>(value.size()));
    value.resize(count);
    return value;
}
bool CleanupFixture(const fs::path& root, const std::wstring& prefix) {
    if (root.empty())
        return true;
    std::error_code error;
    const auto temporary = fs::weakly_canonical(fs::temp_directory_path(), error);
    if (error)
        return false;
    const auto resolved = fs::weakly_canonical(root, error);
    const DWORD attributes = GetFileAttributesW(root.c_str());
    if (error || resolved.parent_path() != temporary || !resolved.filename().wstring().starts_with(prefix) ||
        attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
        return false;
    fs::remove_all(resolved, error);
    return !error;
}
void WriteFile(const fs::path& path, const std::string& value) {
    std::ofstream out(path, std::ios::binary);
    out << value;
    if (!out)
        throw std::runtime_error("fixture write failed");
}
void WriteRecycleIndex(const fs::path& path, const fs::path& original, uint64_t size) {
    std::ofstream out(path, std::ios::binary);
    const uint64_t version = 2;
    FILETIME deleted{};
    GetSystemTimeAsFileTime(&deleted);
    const auto name = original.wstring();
    const auto length = static_cast<uint32_t>(name.size() + 1);
    out.write(reinterpret_cast<const char*>(&version), sizeof(version));
    out.write(reinterpret_cast<const char*>(&size), sizeof(size));
    out.write(reinterpret_cast<const char*>(&deleted), sizeof(deleted));
    out.write(reinterpret_cast<const char*>(&length), sizeof(length));
    out.write(reinterpret_cast<const char*>(name.c_str()), length * sizeof(wchar_t));
    if (!out)
        throw std::runtime_error("fixture recycle index write failed");
}
std::string ReadFile(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), {});
}
struct Fixture {
    fs::path root;
    HWND window = nullptr;
    OpsManager manager;
    bool reject_delete = false;
    Fixture() {
        root = fs::temp_directory_path() /
               (L"pulse-elevation-adapter-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                std::to_wstring(GetTickCount64()));
        fs::create_directories(root / L"source");
        fs::create_directories(root / L"destination");
        window = CreateWindowExW(0, L"STATIC", L"Pulse elevation fixture", 0, 0, 0, 0, 0, HWND_MESSAGE,
                                 nullptr, GetModuleHandleW(nullptr), nullptr);
        manager.SetUiWindow(window);
        manager.Start([this] {
            if (const auto conflict = manager.PendingConflict())
                manager.ResolveConflict(conflict->token, ConflictChoice::KeepBoth, false);
            if (const auto confirmation = manager.PendingDeleteConfirmation()) {
                bool isolated = true;
                for (const auto& target : confirmation->plan.targets)
                    for (const auto& path : target.physical_paths)
                        isolated = isolated && path.starts_with(root.wstring() + L"\\");
                manager.ResolveDeleteConfirmation(confirmation->token, isolated && !reject_delete);
            }
        });
    }
    bool Cleanup() {
        manager.Stop();
        if (window) {
            DestroyWindow(window);
            window = nullptr;
        }
        if (root.empty())
            return true;
        const bool cleaned = CleanupFixture(root, L"pulse-elevation-adapter-");
        Check(cleaned, "isolated fixture cleanup stays inside temporary root");
        root.clear();
        return cleaned;
    }
    ~Fixture() {
        Cleanup();
    }
    bool Run(OpRequest request, const std::function<void()>& before_launch = {}, bool consent = false) {
        const auto helper = consent ? (fs::path(SelfPath()).parent_path() / L"Pulse.Shell.exe").wstring() : SelfPath();
        OperationElevationAdapter::RunFixtureForTest(manager, request, helper, consent, before_launch);
        const auto status = manager.Status();
        if (status.phase != OpPhase::Completed)
            std::wprintf(L"fixture phase=%u hr=0x%08lx path=%ls error=%ls\n",
                         static_cast<unsigned>(status.phase), static_cast<unsigned long>(status.failure_hr),
                         status.failed_path.c_str(), status.last_error.c_str());
        return status.phase == OpPhase::Completed;
    }
};

void RunPartialContinuation() {
    Fixture fixture;
    const auto source = fixture.root / L"source";
    const auto destination = fixture.root / L"destination";
    WriteFile(source / L"partial-first.txt", "first remains unchanged");
    WriteFile(source / L"partial-second.txt", "second continues");
    OpRequest partial;
    partial.type = OpType::Copy;
    partial.dest_dir = destination.wstring();
    partial.sources = {(source / L"partial-first.txt").wstring(), (source / L"partial-second.txt").wstring()};
    Check(OperationElevationAdapter::PartialCopyContinuationForTest(fixture.manager, partial, SelfPath()) &&
              ReadFile(destination / L"partial-first.txt") == "first remains unchanged" &&
              ReadFile(destination / L"partial-second.txt") == "second continues",
          "permission continuation imports the real worker result with cumulative 2/2 progress");
    size_t partial_outputs = 0;
    for (const auto& entry : fs::directory_iterator(destination))
        if (entry.path().filename().wstring().starts_with(L"partial-first"))
            ++partial_outputs;
    Check(partial_outputs == 1,
          "partial continuation creates no keep-both duplicate for already completed source");
}

int RunNormal() {
    std::puts("Same-integrity adapter fixture: real operation engine and transport; no UAC or "
              "protected-system-directory claim.");
    Check(OperationElevationAdapter::HelperPathChecksForTest(),
          "production helper selection preserves spaces, long and UNC paths without a self fallback");
    {
        Fixture missing;
        const auto original = missing.root / L"source" / L"retained.txt";
        const auto helper = (missing.root / L"missing" / L"Pulse.Shell.exe").wstring();
        WriteFile(original, "retained source");
        OpRequest request;
        request.type = OpType::Copy;
        request.sources = {original.wstring()};
        request.dest_dir = (missing.root / L"destination").wstring();
        const auto undo_before = missing.manager.UndoToJson();
        OperationElevationAdapter::RunFixtureForTest(missing.manager, request, helper, true);
        const auto status = missing.manager.Status();
        Check(status.phase == OpPhase::Failed && status.failure_hr == HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND) &&
            status.failed_path == helper && !status.last_error.empty() &&
            ReadFile(original) == "retained source" && !fs::exists(missing.root / L"destination" / L"retained.txt") &&
            missing.manager.DrainCompletions().empty() && missing.manager.UndoToJson() == undo_before,
            "missing Shell helper reports its path without elevation, mutation, self fallback or undo history");
    }
    Check(OperationElevationAdapter::CodecChecksForTest(),
          "bounded codec rejects truncation, trailing bytes, enums, device paths and invalid identities");
    Fixture fixture;
    Check(OperationElevationAdapter::UnknownRecoveryPersistenceForTest(
              (fixture.root / L"unknown-recovery.json").wstring()),
          "unknown rename/create/restore and active elevated recovery survive journal roundtrip and refuse replay");
    const auto source = fixture.root / L"source";
    const auto destination = fixture.root / L"destination";
    WriteFile(source / L"copy.txt", "copy bytes");
    OpRequest copy;
    copy.type = OpType::Copy;
    copy.sources = {(source / L"copy.txt").wstring()};
    copy.dest_dir = destination.wstring();
    Check(fixture.Run(copy) && ReadFile(destination / L"copy.txt") == "copy bytes",
          "copy uses retained plan and imports actual completion");
    auto completions = fixture.manager.DrainCompletions();
    Check(completions.size() == 1 && completions[0].sources == copy.sources && completions[0].task_id == 1,
          "copy completion imported exactly once with parent task identity");
    Check(OperationElevationAdapter::CopyUndoHistoryValidForTest(fixture.manager),
          "copy undo history preserves actual produced identity through JSON roundtrip");
    Check(fixture.Run(copy), "keep-both conflict returns parent UI decision to worker");
    completions = fixture.manager.DrainCompletions();
    Check(completions.size() == 1 && completions[0].destinations.size() == 1 &&
              completions[0].destinations[0] != (destination / L"copy.txt").wstring() &&
              ReadFile(completions[0].destinations[0]) == "copy bytes",
          "keep-both result reports actual destination");
    WriteFile(source / L"move.txt", "move bytes");
    OpRequest move = copy;
    move.type = OpType::Move;
    move.sources = {(source / L"move.txt").wstring()};
    Check(fixture.Run(move) && !fs::exists(source / L"move.txt") &&
              ReadFile(destination / L"move.txt") == "move bytes",
          "move reuses engine and preserves completion identities");
    fixture.manager.DrainCompletions();
    Check(OperationElevationAdapter::RunUndoFixtureForTest(fixture.manager, SelfPath()) &&
              fs::exists(source / L"move.txt") && !fs::exists(destination / L"move.txt"),
          "move inverse consumes parent history and keeps historical identities in helper");
    fixture.manager.DrainCompletions();
    RunPartialContinuation();
    WriteFile(source / L"rename.txt", "rename bytes");
    OpRequest rename;
    rename.type = OpType::Rename;
    rename.sources = {(source / L"rename.txt").wstring()};
    rename.new_name = L"renamed.txt";
    Check(fixture.Run(rename) && !fs::exists(source / L"rename.txt") &&
              ReadFile(source / L"renamed.txt") == "rename bytes",
          "rename validates historical source identity at mutation");
    fixture.manager.DrainCompletions();
    Check(OperationElevationAdapter::RunUndoFixtureForTest(fixture.manager, SelfPath()) &&
              fs::exists(source / L"rename.txt") && !fs::exists(source / L"renamed.txt"),
          "rename inverse uses parent undo history without helper history reservation");
    fixture.manager.DrainCompletions();
    WriteFile(source / L"batch-a.txt", "a");
    WriteFile(source / L"batch-b.txt", "b");
    OpRequest batch;
    batch.type = OpType::BatchRename;
    batch.sources = {(source / L"batch-a.txt").wstring(), (source / L"batch-b.txt").wstring()};
    batch.new_names = {L"batch-x.txt", L"batch-y.txt"};
    Check(fixture.Run(batch) && ReadFile(source / L"batch-x.txt") == "a" &&
              ReadFile(source / L"batch-y.txt") == "b",
          "batch rename passes each historical source identity");
    fixture.manager.DrainCompletions();
    const auto recycle = fixture.root / L"recycle";
    fs::create_directory(recycle);
    const auto restore_original = destination / L"restored.txt";
    const auto restore_payload = recycle / L"$Rrestore.txt";
    const auto restore_index = recycle / L"$Irestore.txt";
    WriteFile(restore_payload, "restore bytes");
    WriteRecycleIndex(restore_index, restore_original, 13);
    RecycleUndoIdentity restore_identity;
    Check(ValidateRecycleUndoPair(restore_original.wstring(), restore_payload.wstring(), restore_identity),
          "isolated recycle pair captures real historical payload and index identities");
    pulse::FileIdentity index_before, index_after;
    const bool index_captured = pulse::ReadPathFileIdentity(restore_index.wstring(), index_before);
    OpRequest restore;
    restore.type = OpType::RestoreRecycle;
    restore.sources = {restore_original.wstring()};
    restore.recycle_paths = {restore_payload.wstring()};
    restore.recycle_undo_identities = {restore_identity};
    Check(fixture.Run(restore) && ReadFile(restore_original) == "restore bytes" &&
              !fs::exists(restore_payload) && index_captured &&
              pulse::ReadPathFileIdentity(restore_index.wstring(), index_after) && index_before == index_after,
          "worker restores exact recycle payload through pinned target parent and retains exact index");
    completions = fixture.manager.DrainCompletions();
    Check(completions.size() == 1 && completions[0].type == OpType::RestoreRecycle &&
              completions[0].sources == restore.sources && completions[0].destinations == restore.sources,
          "actual recycle restore imports one proven completion");
    WriteFile(restore_payload, "undo retained payload");
    WriteRecycleIndex(restore_index, restore_original, 21);
    Check(ValidateRecycleUndoPair(restore_original.wstring(), restore_payload.wstring(),
                                 restore_identity, nullptr, false),
          "occupied undo destination still captures exact isolated pair identities");
    restore.is_undo = true;
    restore.recycle_undo_identities = {restore_identity};
    Check(!fixture.Run(restore) && ReadFile(restore_original) == "restore bytes" &&
              ReadFile(restore_payload) == "undo retained payload" && fs::exists(restore_index),
          "worker recycle undo keeps atomic no-replace when original path is occupied");
    Check(fixture.manager.DrainCompletions().empty(), "failed recycle undo imports no completion");
    restore.is_undo = false;
    const bool changed_index_restored = fixture.Run(restore, [&] {
        fs::rename(restore_index, recycle / L"retained-old-index.txt");
        WriteRecycleIndex(restore_index, restore_original, 21);
    });
    Check(!changed_index_restored && ReadFile(restore_original) == "restore bytes" &&
              ReadFile(restore_payload) == "undo retained payload" &&
              fixture.manager.DrainCompletions().empty(),
          "worker refuses substituted recycle metadata without moving historical payload");
    Check(ValidateRecycleUndoPair(restore_original.wstring(), restore_payload.wstring(),
                                 restore_identity, nullptr, false),
          "replacement metadata can be captured only for a fresh isolated test request");
    restore.recycle_undo_identities = {restore_identity};
    const bool changed_payload_restored = fixture.Run(restore, [&] {
        fs::rename(restore_payload, recycle / L"retained-old-payload.txt");
        WriteFile(restore_payload, "replacement payload");
    });
    Check(!changed_payload_restored && ReadFile(restore_original) == "restore bytes" &&
              ReadFile(restore_payload) == "replacement payload" &&
              fixture.manager.DrainCompletions().empty(),
          "worker refuses substituted recycle payload using authenticated historical object reference");
    WriteFile(source / L"swapped.txt", "historical");
    copy.sources = {(source / L"swapped.txt").wstring()};
    const bool swapped_completed = fixture.Run(copy, [&] {
        fs::rename(source / L"swapped.txt", source / L"retained-old.txt");
        WriteFile(source / L"swapped.txt", "replacement");
    });
    Check(!swapped_completed && !fs::exists(destination / L"swapped.txt") &&
              ReadFile(source / L"swapped.txt") == "replacement",
          "replacement during consent gap is rejected without copying current same-name object");
    Check(fixture.manager.DrainCompletions().empty(), "identity refusal adds no completion or replay");
    OpRequest create;
    create.type = OpType::CreateFolder;
    create.sources = {(destination / L"created-folder").wstring()};
    Check(fixture.Run(create) && fs::is_directory(destination / L"created-folder"),
          "folder creation uses elevated engine shell backend");
    fixture.manager.DrainCompletions();
    create.type = OpType::CreateTextFile;
    create.sources = {(destination / L"created.txt").wstring()};
    Check(fixture.Run(create) && fs::exists(destination / L"created.txt"),
          "text creation returns produced identity");
    fixture.manager.DrainCompletions();
    OpRequest deletion;
    deletion.type = OpType::RealDelete;
    deletion.sources = {(destination / L"created.txt").wstring()};
    fixture.reject_delete = true;
    Check(!fixture.Run(deletion) && fs::exists(destination / L"created.txt") &&
              fixture.manager.Status().phase == OpPhase::Cancelled,
          "permanent-delete rejection returns through parent confirmation bridge");
    fixture.reject_delete = false;
    Check(fixture.Run(deletion) && !fs::exists(destination / L"created.txt"),
          "explicit fixture confirmation admits permanent deletion");
    completions = fixture.manager.DrainCompletions();
    Check(completions.size() == 1 && completions[0].type == OpType::RealDelete && completions[0].task_id == 1,
          "delete imports proven mutation completion exactly once");
    fixture.Cleanup();
    return failures ? 1 : 0;
}

struct FixtureAcl {
    std::wstring path;
    PSECURITY_DESCRIPTOR original = nullptr;
    PACL original_acl = nullptr;
    FixtureAcl(const fs::path& folder) : path(folder.wstring()) {
        if (GetNamedSecurityInfoW(path.data(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr,
                                  &original_acl, nullptr, &original) != ERROR_SUCCESS)
            throw std::runtime_error("read fixture ACL failed");
        HANDLE token = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
            throw std::runtime_error("open fixture token failed");
        DWORD size = 0;
        GetTokenInformation(token, TokenUser, nullptr, 0, &size);
        std::vector<BYTE> storage(size);
        const bool user_read = GetTokenInformation(token, TokenUser, storage.data(), size, &size) != FALSE;
        CloseHandle(token);
        if (!user_read)
            throw std::runtime_error("read fixture user failed");
        BYTE administrators[SECURITY_MAX_SID_SIZE]{};
        DWORD administrators_size = sizeof(administrators);
        if (!CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr, administrators, &administrators_size))
            throw std::runtime_error("create fixture administrators SID failed");
        EXPLICIT_ACCESSW entries[2]{};
        entries[0].grfAccessPermissions = FILE_ALL_ACCESS;
        entries[0].grfAccessMode = SET_ACCESS;
        entries[0].grfInheritance = SUB_CONTAINERS_AND_OBJECTS_INHERIT;
        entries[0].Trustee.TrusteeForm = TRUSTEE_IS_SID;
        entries[0].Trustee.TrusteeType = TRUSTEE_IS_GROUP;
        entries[0].Trustee.ptstrName = reinterpret_cast<LPWSTR>(administrators);
        entries[1].grfAccessPermissions = FILE_GENERIC_READ | FILE_GENERIC_EXECUTE;
        entries[1].grfAccessMode = SET_ACCESS;
        entries[1].grfInheritance = SUB_CONTAINERS_AND_OBJECTS_INHERIT;
        entries[1].Trustee.TrusteeForm = TRUSTEE_IS_SID;
        entries[1].Trustee.TrusteeType = TRUSTEE_IS_USER;
        entries[1].Trustee.ptstrName =
            reinterpret_cast<LPWSTR>(reinterpret_cast<TOKEN_USER*>(storage.data())->User.Sid);
        PACL acl = nullptr;
        if (SetEntriesInAclW(2, entries, nullptr, &acl) != ERROR_SUCCESS)
            throw std::runtime_error("create fixture ACL failed");
        const DWORD error = SetNamedSecurityInfoW(
            path.data(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
            nullptr, nullptr, acl, nullptr);
        LocalFree(acl);
        if (error != ERROR_SUCCESS)
            throw std::runtime_error("apply fixture ACL failed");
    }
    bool Restore() {
        if (!original)
            return true;
        const DWORD error = SetNamedSecurityInfoW(
            path.data(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION | UNPROTECTED_DACL_SECURITY_INFORMATION,
            nullptr, nullptr, original_acl, nullptr);
        LocalFree(original);
        original = nullptr;
        return error == ERROR_SUCCESS;
    }
    ~FixtureAcl() {
        if (!Restore())
            Check(false, "restore original fixture ACL during cleanup");
    }
};
int RunUacFixture() {
    std::puts("Production runas fixture: consent is requested once; all files are isolated temporary fixture "
              "files.");
    Fixture fixture;
    const auto protected_dir = fixture.root / L"destination";
    WriteFile(fixture.root / L"source" / L"protected-copy.txt", "UAC fixture content");
    FixtureAcl acl(protected_dir);
    const auto denied_path = protected_dir / L"medium-denied.txt";
    HANDLE denied = CreateFileW(denied_path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    const DWORD denied_error = denied == INVALID_HANDLE_VALUE ? GetLastError() : ERROR_SUCCESS;
    if (denied != INVALID_HANDLE_VALUE)
        CloseHandle(denied);
    Check(denied == INVALID_HANDLE_VALUE && denied_error == ERROR_ACCESS_DENIED,
          "fixture destination denies normal-token creation");
    if (denied != INVALID_HANDLE_VALUE || denied_error != ERROR_ACCESS_DENIED)
        return 1;
    OpRequest request;
    request.type = OpType::Copy;
    request.sources = {(fixture.root / L"source" / L"protected-copy.txt").wstring()};
    request.dest_dir = protected_dir.wstring();
    const auto undo_before = fixture.manager.UndoToJson();
    const bool completed = fixture.Run(request, {}, true);
    const auto status = fixture.manager.Status();
    const bool cancelled = status.phase == OpPhase::Cancelled;
    const auto completions = fixture.manager.DrainCompletions();
    if (cancelled) {
        Check(status.failure_hr == S_OK && !completed,
              "user-declined UAC continuation reports cancellation");
        Check(!fs::exists(protected_dir / L"protected-copy.txt"),
              "user-declined UAC continuation creates no destination file");
        Check(completions.empty(), "user-declined UAC continuation imports no completion");
        Check(fixture.manager.UndoToJson() == undo_before,
              "user-declined UAC continuation adds no undo history");
        HANDLE still_denied = CreateFileW(denied_path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                          FILE_ATTRIBUTE_NORMAL, nullptr);
        const DWORD still_denied_error = still_denied == INVALID_HANDLE_VALUE ? GetLastError() : ERROR_SUCCESS;
        if (still_denied != INVALID_HANDLE_VALUE)
            CloseHandle(still_denied);
        Check(still_denied == INVALID_HANDLE_VALUE && still_denied_error == ERROR_ACCESS_DENIED,
              "normal-token destination creation remains denied after UAC cancellation");
    } else {
        Check(completed && ReadFile(protected_dir / L"protected-copy.txt") == "UAC fixture content",
              "one production UAC continuation completes bytes into ACL-protected fixture");
        Check(completions.size() == 1 && completions[0].task_id == 1,
              "production UAC continuation imports exactly one proven completion");
    }
    Check(acl.Restore(), "restore protected fixture original ACL");
    fixture.Cleanup();
    if (!failures) {
        std::puts(cancelled ? "RESULT=UAC_CANCELLED" : "RESULT=UAC_COMPLETED");
        if (cancelled)
            std::puts("Exit 0 validates user cancellation only; successful elevated mutation remains unverified.");
    } else
        std::puts("RESULT=UAC_FAILED");
    return failures ? 1 : 0;
}
int LaunchIsolatedUacFixture() {
    const auto root =
        fs::temp_directory_path() / (L"pulse-uac-adapter-host-" + std::to_wstring(GetCurrentProcessId()) +
                                     L"-" + std::to_wstring(GetTickCount64()));
    fs::create_directory(root);
    struct Cleanup {
        fs::path root;
        ~Cleanup() {
            if (!root.empty())
                Check(CleanupFixture(root, L"pulse-uac-adapter-host-"),
                      "isolated UAC helper cleanup stays inside temporary root");
        }
    } cleanup{root};
    fs::copy_file(SelfPath(), root / L"Pulse.exe");
    fs::copy_file(fs::path(SelfPath()).parent_path() / L"Pulse.Shell.exe", root / L"Pulse.Shell.exe");
    std::wstring command = L"\"" + (root / L"Pulse.exe").wstring() + L"\" --uac-fixture-internal";
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    if (!CreateProcessW((root / L"Pulse.exe").c_str(), command.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, root.c_str(), &startup, &process))
        return 1;
    CloseHandle(process.hThread);
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD result = 1;
    GetExitCodeProcess(process.hProcess, &result);
    CloseHandle(process.hProcess);
    const auto log = root / L"uac-fixture.log";
    if (fs::exists(log))
        std::printf("%s", ReadFile(log).c_str());
    else
        Check(false, "read native UAC fixture assertion log");
    Check(CleanupFixture(root, L"pulse-uac-adapter-host-"),
          "remove isolated native UAC helper after restoring ACL");
    cleanup.root.clear();
    return result == 0 && failures == 0 ? 0 : 1;
}
} // namespace
int wmain(int argc, wchar_t** argv) {
    if (argc > 1 && std::wstring_view(argv[1]) == L"--file-operation-worker")
        return OperationElevationAdapter::RunWorker(argc, argv);
    pulse::l10n::Initialize(GetModuleHandleW(nullptr), L"en-US");
    try {
        if (argc > 1 && std::wstring_view(argv[1]) == L"--partial-continuation") {
            RunPartialContinuation();
            return failures ? 1 : 0;
        }
        if (argc > 1 && std::wstring_view(argv[1]) == L"--uac-fixture")
            return LaunchIsolatedUacFixture();
        if (argc > 1 && std::wstring_view(argv[1]) == L"--uac-fixture-internal") {
            FILE* output = nullptr;
            const auto log = fs::path(SelfPath()).parent_path() / L"uac-fixture.log";
            if (_wfreopen_s(&output, log.c_str(), L"w", stdout) != 0 || !output)
                return 1;
            return RunUacFixture();
        }
        return RunNormal();
    } catch (const std::exception& error) {
        std::printf("[FAIL] fixture exception: %s\n", error.what());
        return 1;
    }
}
