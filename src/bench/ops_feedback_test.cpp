// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "../ops/ops_manager.h"
#include "../ops/recycle_undo_validation.h"
#include "../ipc/shell_client.h"
#include "../common/localization.h"
#include "../common/file_operation_error.h"
#include "../shell_host/delete_fileop.h"
#include "../shell_host/recycle_fileop.h"
#include <shlobj.h>
#include <aclapi.h>
#include <filesystem>
#include <fstream>
#include <cstdio>
#include <vector>
#include <algorithm>
#include <tlhelp32.h>
#include <atomic>
#include <thread>

namespace pulse::ops {
struct TransferIntegrationProbe {
    static void Initialize(OpsManager& manager) { manager.running_ = true; manager.accepting_ = true; }
    static void Rename(OpsManager& manager, const OpRequest& request, uint64_t id) {
        manager.RunShellOp(request, id);
    }
    static void Transfer(OpsManager& manager, const OpRequest& request, uint64_t id) {
        manager.RunTransfer(request, id);
    }
    static void Delete(OpsManager& manager, const OpRequest& request, uint64_t id) {
        OpsManager::QueueItem item; item.req = request; item.seq = id;
        manager.notify_ = [&] {
            if (const auto confirmation = manager.PendingDeleteConfirmation())
                manager.ResolveDeleteConfirmation(confirmation->token,
                    confirmation->plan.targets.size() == 1 && confirmation->plan.targets.front().physical_paths == request.sources);
        };
        HWND owner = CreateWindowExW(0, L"STATIC", L"PulseDeletePermissionFixture", WS_POPUP,
            0, 0, 100, 100, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        manager.SetUiWindow(owner);
        manager.StartShellClient();
        manager.RunDelete(item);
        ipc::ShellClient::Instance().Stop();
        manager.SetUiWindow(nullptr);
        if (owner) DestroyWindow(owner);
        manager.notify_ = {};
    }
    static bool RunQueued(OpsManager& manager, bool cancel = false) {
        if (manager.queue_.empty()) return false;
        auto item = std::move(manager.queue_.front()); manager.queue_.pop_front();
        if (cancel) manager.CancelCurrent();
        manager.ExecuteFileOperation(item);
        return true;
    }
    static bool ElevationSnapshot(const OpsManager& manager) {
        if (!manager.elevation_retry_) return false;
        const auto& retry = *manager.elevation_retry_;
        return retry.retry_source_references.size() == retry.req.sources.size() &&
            !retry.retry_source_references.empty() && retry.retry_source_references.front().Get() &&
            !retry.retry_transfer_plan.empty() && retry.retry_source_directories && retry.retry_destination_guard;
    }
    static bool ElevatedQueued(const OpsManager& manager) {
        return manager.queue_.size() == 1 && manager.queue_.front().elevated_retry &&
            !manager.queue_.front().lock_retry && !manager.queue_.front().retry_transfer_plan.empty();
    }
    static bool QueueEmpty(const OpsManager& manager) { return manager.queue_.empty(); }
    static void Supersede(OpsManager& manager, uint64_t task_id) {
        manager.SetStatus([&](OpStatus& status) { status.task_id = task_id; });
    }
    static uint32_t BackendResult(const OpsManager& manager) { return static_cast<uint32_t>(manager.Status().failure_hr); }
    static void RefreshUndo(OpsManager& manager) { manager.RefreshRecycleUndoValidity(); }
    static void SeedCachedRecycleUndo(OpsManager& manager, const std::wstring& original, const std::wstring& payload) {
        UndoEntry entry;
        entry.type = OpType::RecycleDelete; entry.sources = {original}; entry.destinations = {payload};
        entry.recycle_verified = true;
        manager.undo_.push_back(entry);
    }
    static void KnownMutation(OpsManager& manager, const std::wstring& path) { manager.InvalidateRecycleUndo({path}); }
    static bool CaptureRetry(OpsManager& manager, const OpRequest& request, HRESULT error) {
        OpsManager::QueueItem retry;
        retry.req = request; retry.seq = 100;
        manager.CaptureLockedFailure(retry, error, request.sources.front(), {});
        return manager.lock_retry_.has_value();
    }
    static void Restore(OpsManager& manager, const OpRequest& request) {
        manager.status_.type = OpType::RestoreRecycle;
        manager.status_.total_items = request.sources.size();
        manager.RunManualRecycleRestore(request, [](const std::wstring& original, const std::wstring& payload) {
            const auto root = std::filesystem::absolute(L"bench_data");
            const auto parent = std::filesystem::path(payload).parent_path();
            return parent.parent_path() == root && parent.filename().wstring().starts_with(L"ops-manual-restore-") &&
                std::filesystem::path(original).parent_path() == parent;
        });
    }
    static void CancelRestore(OpsManager& manager, const OpRequest& request) {
        manager.shell_cancel_requested_ = true;
        Restore(manager, request);
    }
    static void SeedLabel(OpsManager& manager, OpType type, const std::vector<std::wstring>& sources) {
        manager.undo_.clear();
        UndoEntry entry;
        entry.type = type; entry.sources = sources; entry.recycle_verified = true;
        manager.undo_.push_back(std::move(entry));
    }
};
}
namespace {
int failures = 0;
void Check(bool value, const char* name) { std::printf("[%s] %s\n", value ? "PASS" : "FAIL", name); failures += !value; }
void Write(const std::filesystem::path& file, const char* text = "fixture") { std::ofstream(file, std::ios::binary) << text; }
void Index(const std::filesystem::path& index, const std::wstring& original) {
    std::ofstream output(index, std::ios::binary | std::ios::trunc);
    const uint64_t version = 2, size = 7, deleted = 0;
    const uint32_t chars = static_cast<uint32_t>(original.size() + 1);
    output.write(reinterpret_cast<const char*>(&version), sizeof(version));
    output.write(reinterpret_cast<const char*>(&size), sizeof(size));
    output.write(reinterpret_cast<const char*>(&deleted), sizeof(deleted));
    output.write(reinterpret_cast<const char*>(&chars), sizeof(chars));
    output.write(reinterpret_cast<const char*>(original.c_str()), static_cast<std::streamsize>(chars * sizeof(wchar_t)));
}
int PermissionFeedbackFixtures() {
    using namespace pulse::ops;
    pulse::l10n::Initialize(GetModuleHandleW(nullptr), L"en-US");
    const auto parent = std::filesystem::absolute(L"bench_data");
    const auto root = parent / (L"ops-permission-feedback-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    const auto destination = root / L"destination", source = root / L"source.txt";
    std::filesystem::create_directories(destination);
    Write(source, "permission versus sharing fixture");
    struct DirectoryDacl {
        HANDLE handle = INVALID_HANDLE_VALUE;
        PSECURITY_DESCRIPTOR descriptor = nullptr;
        PACL original = nullptr;
        PACL denied = nullptr;
        bool changed = false;
        DWORD Restore() {
            if (!changed) return ERROR_SUCCESS;
            const DWORD result = SetSecurityInfo(handle, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
                nullptr, nullptr, original, nullptr);
            if (result == ERROR_SUCCESS) changed = false;
            return result;
        }
        ~DirectoryDacl() {
            Restore();
            if (denied) LocalFree(denied);
            if (descriptor) LocalFree(descriptor);
            if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
        }
    } dacl;
    dacl.handle = CreateFileW(destination.c_str(), READ_CONTROL | WRITE_DAC,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    Check(dacl.handle != INVALID_HANDLE_VALUE, "permission fixture pins a DACL restoration handle for its isolated directory");
    DWORD acl_error = dacl.handle == INVALID_HANDLE_VALUE ? GetLastError() :
        GetSecurityInfo(dacl.handle, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr,
            &dacl.original, nullptr, &dacl.descriptor);
    Check(acl_error == ERROR_SUCCESS && dacl.descriptor, "permission fixture saves the original directory DACL");
    BYTE world_sid[SECURITY_MAX_SID_SIZE]{};
    DWORD world_bytes = sizeof(world_sid);
    if (acl_error == ERROR_SUCCESS && !CreateWellKnownSid(WinWorldSid, nullptr, world_sid, &world_bytes)) acl_error = GetLastError();
    if (acl_error == ERROR_SUCCESS) {
        EXPLICIT_ACCESSW deny{};
        deny.grfAccessPermissions = FILE_ADD_FILE;
        deny.grfAccessMode = DENY_ACCESS;
        deny.grfInheritance = NO_INHERITANCE;
        deny.Trustee.TrusteeForm = TRUSTEE_IS_SID;
        deny.Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
        deny.Trustee.ptstrName = reinterpret_cast<LPWSTR>(world_sid);
        acl_error = SetEntriesInAclW(1, &deny, dacl.original, &dacl.denied);
    }
    if (acl_error == ERROR_SUCCESS) {
        acl_error = SetSecurityInfo(dacl.handle, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
            nullptr, nullptr, dacl.denied, nullptr);
        dacl.changed = acl_error == ERROR_SUCCESS;
    }
    std::printf("[DIAG] deny FILE_ADD_FILE DACL result=%lu\n", acl_error);
    Check(acl_error == ERROR_SUCCESS && dacl.changed, "permission fixture denies only file creation in its target directory");
    if (dacl.changed) {
        OpsManager manager;
        TransferIntegrationProbe::Initialize(manager);
        OpRequest request;
        request.type = OpType::Copy; request.sources = {source.wstring()}; request.dest_dir = destination.wstring();
        TransferIntegrationProbe::Transfer(manager, request, 201);
        const auto status = manager.Status();
        std::printf("[DIAG] permission Copy phase=%d hr=0x%08X retry=%d owners=%zu\n", static_cast<int>(status.phase),
            static_cast<unsigned>(status.failure_hr), status.lock_retry_available, status.lock_owners.size());
        Check(status.phase == OpPhase::Failed && status.failure_hr == E_ACCESSDENIED &&
            !status.last_error.empty() && !std::filesystem::exists(destination / source.filename()),
            "real Copy reports access denial when the target DACL rejects FILE_ADD_FILE");
        Check(!status.lock_retry_available && status.lock_owners.empty() && status.locked_path.empty() &&
            !manager.IsLockedFailureCurrent(201) && !manager.RetryLockedOperation(201, false),
            "permission failure has neither occupancy dialog state nor cached lock retry");
        Check(status.elevation_retry_available && manager.IsElevationFailureCurrent(201) &&
            TransferIntegrationProbe::ElevationSnapshot(manager),
            "physical permission denial retains exact unfinished plan and source leases for consent");
        Check(status.elevation_source_path == source.wstring() &&
            status.elevation_destination_path == (destination / source.filename()).wstring(),
            "permission endpoints retain complete paths independently of progress labels");
        Check(!manager.RetryElevatedOperation(200), "administrator continuation rejects a different task");
        Check(manager.RetryElevatedOperation(201) && TransferIntegrationProbe::ElevatedQueued(manager) &&
            !manager.RetryElevatedOperation(201), "administrator consent enqueues its exact snapshot only once");
        manager.CancelCurrent();
        Check(TransferIntegrationProbe::QueueEmpty(manager) && !manager.IsElevationFailureCurrent(201),
            "cancellation revokes queued administrator continuation before any worker launch");
        OpsManager stale; TransferIntegrationProbe::Initialize(stale);
        TransferIntegrationProbe::Transfer(stale, request, 203);
        TransferIntegrationProbe::Supersede(stale, 204);
        Check(!stale.IsElevationFailureCurrent(203) && !stale.RetryElevatedOperation(203),
            "superseding status invalidates old administrator consent");
        OpsManager declined; TransferIntegrationProbe::Initialize(declined);
        TransferIntegrationProbe::Transfer(declined, request, 205);
        declined.CancelElevationRetry(205);
        Check(!declined.Status().elevation_retry_available && !declined.RetryElevatedOperation(205) &&
            TransferIntegrationProbe::QueueEmpty(declined), "declined permission prompt dispatches no elevated operation");
    }
    const DWORD restore_error = dacl.Restore();
    std::printf("[DIAG] original DACL restoration result=%lu\n", restore_error);
    Check(restore_error == ERROR_SUCCESS && !dacl.changed, "permission fixture restores its original DACL before sharing comparison and cleanup");
    if (acl_error == ERROR_SUCCESS && restore_error == ERROR_SUCCESS) {
        HANDLE held = CreateFileW(source.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        Check(held != INVALID_HANDLE_VALUE, "sharing comparison holds only the isolated source with an exclusive file handle");
        if (held != INVALID_HANDLE_VALUE) {
            OpsManager manager;
            TransferIntegrationProbe::Initialize(manager);
            OpRequest request;
            request.type = OpType::Copy; request.sources = {source.wstring()}; request.dest_dir = destination.wstring();
            TransferIntegrationProbe::Transfer(manager, request, 202);
            const auto status = manager.Status();
            std::printf("[DIAG] sharing Copy phase=%d hr=0x%08X retry=%d owners=%zu\n", static_cast<int>(status.phase),
                static_cast<unsigned>(status.failure_hr), status.lock_retry_available, status.lock_owners.size());
            Check(status.phase == OpPhase::Failed && IsLockFailure(status.failure_hr) &&
                status.lock_retry_available && manager.IsLockedFailureCurrent(202),
                "real sharing conflict retains occupancy retry state independently of Restart Manager owner discovery");
            Check(!status.elevation_retry_available && !manager.RetryElevatedOperation(202),
                "proven sharing conflict never requests administrator continuation");
            CloseHandle(held);
            Check(manager.RetryLockedOperation(202, false) && TransferIntegrationProbe::RunQueued(manager) &&
                manager.Status().phase == OpPhase::Completed && std::filesystem::exists(destination / source.filename()),
                "real sharing Copy succeeds through its cached plain retry after the fixture handle closes");
        }
    }
    if (acl_error == ERROR_SUCCESS && !dacl.changed) {
        DirectoryDacl deletion_file, deletion_parent;
        const auto deny_access = [&](DirectoryDacl& fixture, const std::filesystem::path& path, DWORD access) {
            fixture.handle = CreateFileW(path.c_str(), READ_CONTROL | WRITE_DAC,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
            if (fixture.handle == INVALID_HANDLE_VALUE) return GetLastError();
            DWORD result = GetSecurityInfo(fixture.handle, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr,
                &fixture.original, nullptr, &fixture.descriptor);
            if (result != ERROR_SUCCESS) return result;
            EXPLICIT_ACCESSW deny{};
            deny.grfAccessPermissions = access; deny.grfAccessMode = DENY_ACCESS; deny.grfInheritance = NO_INHERITANCE;
            deny.Trustee.TrusteeForm = TRUSTEE_IS_SID; deny.Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
            deny.Trustee.ptstrName = reinterpret_cast<LPWSTR>(world_sid);
            result = SetEntriesInAclW(1, &deny, fixture.original, &fixture.denied);
            if (result != ERROR_SUCCESS) return result;
            result = SetSecurityInfo(fixture.handle, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
                nullptr, nullptr, fixture.denied, nullptr);
            fixture.changed = result == ERROR_SUCCESS;
            return result;
        };
        const DWORD parent_error = deny_access(deletion_parent, root, FILE_DELETE_CHILD);
        const DWORD file_error = deny_access(deletion_file, source, DELETE);
        Check(parent_error == ERROR_SUCCESS && file_error == ERROR_SUCCESS,
            "delete permission fixture denies file DELETE and parent DELETE_CHILD with pinned ACL restoration handles");
        if (parent_error == ERROR_SUCCESS && file_error == ERROR_SUCCESS) {
            OpsManager manager; TransferIntegrationProbe::Initialize(manager);
            OpRequest request; request.type = OpType::RealDelete; request.sources = {source.wstring()};
            TransferIntegrationProbe::Delete(manager, request, 203);
            const auto status = manager.Status();
            std::printf("[DIAG] permission Delete phase=%d hr=0x%08X lock_retry=%d owners=%zu elevation=%d\n",
                static_cast<int>(status.phase), static_cast<unsigned>(status.failure_hr),
                status.lock_retry_available, status.lock_owners.size(), status.elevation_retry_available);
            Check(status.phase == OpPhase::Failed &&
                pulse::format::FileOperationMessageCode(status.failure_hr) == ERROR_ACCESS_DENIED &&
                std::filesystem::exists(source), "real delete permission denial preserves its exact source");
            Check(status.lock_owners.empty() && status.locked_path.empty() && !status.lock_retry_available &&
                !manager.IsLockedFailureCurrent(203),
                "own identity leases never classify delete permission denial as an external file lock");
            Check(status.elevation_retry_available && manager.IsElevationFailureCurrent(203) &&
                status.elevation_source_path == source.wstring() && status.elevation_destination_path.empty(),
                "real delete permission denial offers administrator continuation with a complete source-only endpoint");
            manager.CancelElevationRetry(203);
            Check(!manager.IsElevationFailureCurrent(203) && TransferIntegrationProbe::QueueEmpty(manager) &&
                std::filesystem::exists(source), "declining delete elevation queues no operation and retains the source");
        }
        const auto file_restore = deletion_file.Restore(), parent_restore = deletion_parent.Restore();
        Check(file_restore == ERROR_SUCCESS && parent_restore == ERROR_SUCCESS &&
            !deletion_file.changed && !deletion_parent.changed, "delete permission fixture restores both original ACLs before cleanup");
    }
    Check(root.parent_path() == parent && root.filename().wstring().starts_with(L"ops-permission-feedback-"),
        "verify permission fixture cleanup remains under bench_data");
    if (!dacl.changed && root.parent_path() == parent && root.filename().wstring().starts_with(L"ops-permission-feedback-"))
        std::filesystem::remove_all(root);
    return failures ? 1 : 0;
}
int OperationLabelFixtures() {
    using namespace pulse;
    using namespace pulse::ops;
    l10n::Initialize(GetModuleHandleW(nullptr), L"en-US");
    OpsManager manager;
    const std::vector<OpType> types = {OpType::Copy, OpType::Move, OpType::RecycleDelete, OpType::RealDelete,
        OpType::Rename, OpType::CreateFolder, OpType::CreateTextFile, OpType::RestoreRecycle, OpType::EmptyRecycle, OpType::BatchRename};
    const std::vector<std::wstring> english = {L"Copy", L"Move", L"Delete", L"Delete permanently", L"Rename",
        L"New folder", L"New text document", L"Restore", L"Empty Recycle Bin", L"Batch rename"};
    const std::vector<std::wstring> chinese = {L"复制", L"移动", L"删除", L"永久删除", L"重命名", L"新建文件夹",
        L"新建文本文档", L"还原", L"清空回收站", L"批量重命名"};
    for (size_t i = 0; i < types.size(); ++i) {
        l10n::SetLanguage(L"en-US");
        TransferIntegrationProbe::SeedLabel(manager, types[i], {L"C:\\fixture\\one.txt"});
        const auto english_single = manager.UndoLabel();
        Check(english_single.starts_with(L"Undo " + english[i]) && english_single.find(L"撤销") == std::wstring::npos,
            "English undo uses localized action for each single-item operation");
        TransferIntegrationProbe::SeedLabel(manager, types[i], {L"C:\\fixture\\one.txt", L"C:\\fixture\\two.txt"});
        const auto english_multiple = manager.UndoLabel();
        Check(english_multiple.starts_with(L"Undo " + english[i]) &&
            (types[i] == OpType::EmptyRecycle || english_multiple.find(L"one.txt (2 items)") != std::wstring::npos),
            "English undo uses exactly one localized plural item count");
        l10n::SetLanguage(L"zh-CN");
        const auto chinese_multiple = manager.UndoLabel();
        Check(chinese_multiple.starts_with(L"撤销" + chinese[i]) &&
            (types[i] == OpType::EmptyRecycle || chinese_multiple.find(L"one.txt 等 2 项") != std::wstring::npos),
            "Chinese undo switches verbs and plural formats using existing localization state");
        TransferIntegrationProbe::SeedLabel(manager, types[i], {L"C:\\fixture\\one.txt"});
        Check(manager.UndoLabel().starts_with(L"撤销" + chinese[i]),
            "Chinese single-item undo uses localized action without plural count");
    }
    return failures ? 1 : 0;
}
int ManualRestoreFixtures() {
    using namespace pulse;
    using namespace pulse::ops;
    const auto parent = std::filesystem::absolute(L"bench_data");
    const auto root = parent / (L"ops-manual-restore-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    std::filesystem::create_directories(root);
    const auto original = root / L"document.txt";
    const auto payload_a = root / L"$Rfirst", payload_b = root / L"$Rsecond";
    Write(original, "existing target");
    Write(payload_a, "first generation"); Index(root / L"$Ifirst", original.wstring());
    Write(payload_b, "second generation"); Index(root / L"$Isecond", original.wstring());
    RecycleUndoIdentity identity;
    Check(!ValidateRecycleUndoPair(original.wstring(), payload_a.wstring(), identity),
        "Undo still refuses an occupied original destination");
    Check(ValidateRecycleUndoPair(original.wstring(), payload_a.wstring(), identity, nullptr, false),
        "manual restore validates exact recycle mapping with an occupied original destination");
    auto read = [](const std::filesystem::path& file) {
        std::ifstream input(file, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    };
    DWORD code = 0;
    const bool collision_restore = RestoreRecyclePair(original.wstring(), payload_a.wstring(), original.wstring(), identity, code);
    std::printf("[DIAG] collision restored=%d error=%lu payload_exists=%d\n", collision_restore, code,
        std::filesystem::exists(payload_a));
    Check(!collision_restore && (code == ERROR_ALREADY_EXISTS || code == ERROR_FILE_EXISTS) &&
        std::filesystem::exists(payload_a) && read(original) == "existing target",
        "atomic exact restore cannot overwrite an existing target");
    OpsManager manager;
    OpRequest request;
    request.type = OpType::RestoreRecycle;
    request.sources = {original.wstring(), original.wstring()};
    request.recycle_paths = {payload_b.wstring(), payload_a.wstring()};
    TransferIntegrationProbe::Restore(manager, request);
    auto status = manager.Status();
    auto diagnose = [](const OpStatus& state) {
        std::printf("[DIAG] restore phase=%d completed=%llu results=%zu hr=0x%08X\n", static_cast<int>(state.phase),
            static_cast<unsigned long long>(state.completed_items), state.restore_results.size(),
            static_cast<unsigned>(state.failure_hr));
        for (const auto& result : state.restore_results)
            std::printf("[DIAG] payload=%ls restored=%d hr=0x%08X destination=%ls\n", result.recycle_path.c_str(),
                result.restored, static_cast<unsigned>(result.error), result.destination.c_str());
    };
    diagnose(status);
    Check(status.phase == OpPhase::Completed && status.completed_items == 2 && status.restore_results.size() == 2,
        "both selected generations sharing the same original path produce independent success outcomes");
    Check(read(original) == "existing target" && read(root / L"document - 副本.txt") == "second generation" &&
        read(root / L"document - 副本 (2).txt") == "first generation",
        "manual restore preserves existing data and uses exact selected generation with existing copy naming");
    Check(std::filesystem::exists(root / L"$Ifirst") && std::filesystem::exists(root / L"$Isecond") &&
        !std::filesystem::exists(payload_a) && !std::filesystem::exists(payload_b),
        "successful restore moves only exact payloads and retains metadata without bypassing deletion admission");
    const auto completions = manager.DrainCompletions();
    Check(completions.size() == 1 && completions.front().sources.size() == 2 &&
        completions.front().destinations == std::vector<std::wstring>{(root / L"document - 副本.txt").wstring(),
            (root / L"document - 副本 (2).txt").wstring()},
        "completion publishes each actual keep-both destination for refresh and recycle occupancy");
    const auto bad = root / L"$Rwrong", good = root / L"$Rgood";
    Write(bad, "bad identity"); Index(root / L"$Iwrong", (root / L"other.txt").wstring());
    Write(good, "valid sibling"); Index(root / L"$Igood", (root / L"good.txt").wstring());
    request.sources = {original.wstring(), (root / L"good.txt").wstring()};
    request.recycle_paths = {bad.wstring(), good.wstring()};
    TransferIntegrationProbe::Restore(manager, request);
    status = manager.Status();
    diagnose(status);
    Check(status.phase == OpPhase::Failed && status.completed_items == 1 && status.restore_results.size() == 2 &&
        !status.restore_results[0].restored && status.restore_results[1].restored &&
        std::filesystem::exists(bad) && read(root / L"good.txt") == "valid sibling" &&
        status.last_error.find(L"$Rwrong") != std::wstring::npos,
        "mixed restore reports exact failed item and keeps successful sibling completion");
    manager.DrainCompletions();
    const auto original_directory = root / L"folder.with.dot", recycled_directory = root / L"$Rdirectory";
    std::filesystem::create_directory(original_directory); Write(original_directory / L"existing.txt", "existing directory");
    std::filesystem::create_directory(recycled_directory); Write(recycled_directory / L"child.txt", "recycled directory");
    Index(root / L"$Idirectory", original_directory.wstring());
    request.sources = {original_directory.wstring()}; request.recycle_paths = {recycled_directory.wstring()};
    TransferIntegrationProbe::Restore(manager, request);
    status = manager.Status();
    Check(status.phase == OpPhase::Completed && status.restore_results.front().restored &&
        read(original_directory / L"existing.txt") == "existing directory" &&
        read(root / L"folder.with.dot - 副本" / L"child.txt") == "recycled directory",
        "directory conflict preserves both trees and keeps the whole directory name including dots");
    manager.DrainCompletions();
    const auto stale = root / L"$Rstale";
    Write(stale, "before replacement"); Index(root / L"$Istale", (root / L"stale.txt").wstring());
    Check(ValidateRecycleUndoPair((root / L"stale.txt").wstring(), stale.wstring(), identity),
        "stale identity test captures isolated recycle pair");
    MoveFileW(stale.c_str(), (root / L"saved-stale").c_str()); Write(stale, "replacement");
    Check(!RestoreRecyclePair((root / L"stale.txt").wstring(), stale.wstring(),
        (root / L"stale.txt").wstring(), identity, code) && code == ERROR_INVALID_DATA &&
        !std::filesystem::exists(root / L"stale.txt") && read(stale) == "replacement",
        "identity pinning refuses a replaced payload without selecting any other recycle version");
    request.sources = {(root / L"stale.txt").wstring()}; request.recycle_paths = {stale.wstring()};
    request.recycle_undo_identities = {identity};
    TransferIntegrationProbe::Restore(manager, request);
    status = manager.Status();
    Check(status.phase == OpPhase::Failed && !status.restore_results.front().restored &&
        status.failure_hr == HRESULT_FROM_WIN32(ERROR_INVALID_DATA) && read(stale) == "replacement",
        "per-item restore rejects historical recycle identity mismatch before any elevated continuation");
    const auto undo_original = root / L"undo.txt", undo_payload = root / L"$Rundo";
    Write(undo_original, "occupied target"); Write(undo_payload, "undo payload");
    Index(root / L"$Iundo", undo_original.wstring());
    Check(ValidateRecycleUndoPair(undo_original.wstring(), undo_payload.wstring(), identity, nullptr, false),
        "undo collision fixture retains an exact recycle pair identity");
    request.sources = {undo_original.wstring()}; request.recycle_paths = {undo_payload.wstring()};
    request.recycle_undo_identities = {identity}; request.is_undo = true;
    TransferIntegrationProbe::Restore(manager, request); status = manager.Status();
    Check(status.phase == OpPhase::Failed && !status.restore_results.front().restored &&
        read(undo_original) == "occupied target" && read(undo_payload) == "undo payload" &&
        !std::filesystem::exists(root / L"undo - 副本.txt"),
        "undo uses atomic no-replace restoration and never silently substitutes a keep-both target");
    request.is_undo = false; request.recycle_undo_identities.clear();
    request.sources = {(root / L"cancel.txt").wstring()}; request.recycle_paths = {bad.wstring()};
    TransferIntegrationProbe::CancelRestore(manager, request);
    status = manager.Status();
    Check(status.phase == OpPhase::Cancelled && status.restore_results.size() == 1 &&
        !status.restore_results.front().restored && status.restore_results.front().error == HRESULT_FROM_WIN32(ERROR_CANCELLED) &&
        std::filesystem::exists(bad), "cancelled restore leaves all unattempted payloads intact and records cancellation per item");
    {
        OpsManager denied, shared;
        TransferIntegrationProbe::Initialize(denied); TransferIntegrationProbe::Initialize(shared);
        OpRequest copy; copy.type = OpType::Copy; copy.sources = {original.wstring()};
        Check(!TransferIntegrationProbe::CaptureRetry(denied, copy, E_ACCESSDENIED),
            "ownerless access denial has no cached lock retry");
        Check(TransferIntegrationProbe::CaptureRetry(shared, copy, HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION)),
            "explicit sharing failure retains ownerless transfer retry");
    }
    Check(root.parent_path() == parent && root.filename().wstring().starts_with(L"ops-manual-restore-"),
        "verify manual restore fixture cleanup remains under bench_data");
    if (root.parent_path() == parent && root.filename().wstring().starts_with(L"ops-manual-restore-"))
        std::filesystem::remove_all(root);
    return failures ? 1 : 0;
}
int SafeRecycleAdapter(bool expect_refusal) {
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    Check(SUCCEEDED(initialized), "explicit production adapter fixture initializes COM");
    if (FAILED(initialized)) return 1;
    const auto parent = std::filesystem::absolute(L"bench_data");
    const auto root = parent / (L"ops-safe-recycle-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    std::filesystem::create_directories(root);
    const auto file = root / L"fixture.txt", directory = root / L"folder", child = directory / L"child.txt";
    Write(file); std::filesystem::create_directory(directory); Write(child);
    std::atomic<bool> stop{false}, native{false};
    const DWORD thread = GetCurrentThreadId();
    std::thread observer([&] {
        while (!stop.load()) {
            struct Windows { DWORD thread; std::atomic<bool>& native; } windows{thread, native};
            EnumWindows([](HWND window, LPARAM raw) -> BOOL {
                auto& value = *reinterpret_cast<Windows*>(raw);
                DWORD pid = 0;
                const DWORD tid = GetWindowThreadProcessId(window, &pid);
                if (pid == GetCurrentProcessId() && tid == value.thread && IsWindowVisible(window)) {
                    value.native.store(true); PostMessageW(window, WM_CLOSE, 0, 0);
                }
                return TRUE;
            }, reinterpret_cast<LPARAM>(&windows));
            Sleep(10);
        }
    });
    const auto cancelled = pulse::shell::RecycleAuthorized({file.wstring(), directory.wstring()}, [] { return true; });
    Check(cancelled.cancelled && std::filesystem::exists(file) && std::filesystem::exists(child) && cancelled.sources.empty(), "explicit adapter cancellation before execution preserves every selected root");
    const auto result = pulse::shell::RecycleAuthorized({file.wstring()});
    std::printf("[DIAG] production explicit recycle hr=0x%08lX source_preserved=%d actual=%zu path=%ls\n",
        static_cast<unsigned long>(result.error), std::filesystem::exists(file), result.sources.size(), result.failed_path.c_str());
    if (expect_refusal) {
        Check(FAILED(result.error) && !result.cancelled && result.sources.empty() &&
            result.destinations.empty() && std::filesystem::exists(file),
            "Low-integrity recycle refusal never falls through to permanent deletion");
        const auto folder_result = pulse::shell::RecycleAuthorized({directory.wstring(), file.wstring()});
        Check(FAILED(folder_result.error) && !folder_result.cancelled && folder_result.sources.empty() &&
            std::filesystem::exists(child) && std::filesystem::exists(file),
            "directory refusal preserves its children and never processes the later selected root");
    } else {
        Check(SUCCEEDED(result.error) && result.sources == std::vector<std::wstring>{file.wstring()} &&
            result.destinations.size() == 1 && !std::filesystem::exists(file), "explicit production adapter records a verified actual recycle");
        pulse::ops::RecycleUndoIdentity identity;
        const bool exact = result.destinations.size() == 1 &&
            pulse::ops::ValidateRecycleUndoPair(file.wstring(), result.destinations.front(), identity);
        Check(exact && MoveFileW(result.destinations.front().c_str(), file.c_str()), "explicit adapter fixture restores its exact validated payload");
    }
    stop.store(true); observer.join();
    Check(!native.load(), "explicit production recycle adapter never displays an owned native provider window");
    CoUninitialize();
    if (root.parent_path() == parent && root.filename().wstring().starts_with(L"ops-safe-recycle-")) {
        if (std::filesystem::exists(file)) DeleteFileW(file.c_str());
        if (std::filesystem::exists(child)) DeleteFileW(child.c_str());
        RemoveDirectoryW(directory.c_str());
        RemoveDirectoryW(root.c_str());
    }
    return failures ? 1 : 0;
}
int NoNativeShell(bool expect_refusal = false) {
    using namespace pulse::ops;
    const auto root = std::filesystem::absolute(std::filesystem::path(L"bench_data") /
        (L"ops-silent-shell-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64())));
    std::filesystem::create_directories(root);
    const auto recycled = root / L"recycled.txt", permanent = root / L"permanent.txt";
    Write(recycled); Write(permanent);
    const HWND owner = CreateWindowExW(0, L"STATIC", L"Pulse fixture owner", 0, 0, 0, 0, 0,
        HWND_MESSAGE, nullptr, nullptr, nullptr);
    OpsManager manager;
    manager.SetUiWindow(owner); manager.Start([] {});
    std::atomic<bool> stop{false}, saw_native{false};
    std::thread observer([&] {
        while (!stop.load()) {
            std::vector<DWORD> hosts;
            HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
            PROCESSENTRY32W entry{sizeof(entry)};
            if (snapshot != INVALID_HANDLE_VALUE && Process32FirstW(snapshot, &entry)) {
                do {
                    if (entry.th32ParentProcessID == GetCurrentProcessId() &&
                        _wcsicmp(entry.szExeFile, L"Pulse.Shell.exe") == 0) hosts.push_back(entry.th32ProcessID);
                } while (Process32NextW(snapshot, &entry));
            }
            if (snapshot != INVALID_HANDLE_VALUE) CloseHandle(snapshot);
            struct Windows { const std::vector<DWORD>& hosts; std::atomic<bool>& seen; } windows{hosts, saw_native};
            EnumWindows([](HWND window, LPARAM raw) -> BOOL {
                const auto& value = *reinterpret_cast<Windows*>(raw);
                DWORD pid = 0; GetWindowThreadProcessId(window, &pid);
                if (IsWindowVisible(window) && std::find(value.hosts.begin(), value.hosts.end(), pid) != value.hosts.end()) {
                    value.seen.store(true);
                    wchar_t title[256]{}; GetWindowTextW(window, title, ARRAYSIZE(title));
                    std::printf("[DIAG] fixture shell native window PID=%lu title=%ls\n", pid, title);
                    PostMessageW(window, WM_CLOSE, 0, 0); // only our own fixture host's window
                }
                return TRUE;
            }, reinterpret_cast<LPARAM>(&windows));
            Sleep(10);
        }
    });
    auto diagnostics = [&](const char* stage) {
        const auto status = manager.Status();
        const auto pending = manager.PendingDeleteConfirmation();
        std::printf("[DIAG] backend_result=0x%08X\n", TransferIntegrationProbe::BackendResult(manager));
        std::printf("[DIAG] %s task=%llu phase=%d active=%d hr=0x%08lX done=%llu failed=%ls error=%ls pending_token=%llu pending_targets=%zu\n",
            stage, static_cast<unsigned long long>(status.task_id), static_cast<int>(status.phase), status.active,
            static_cast<unsigned long>(status.failure_hr), static_cast<unsigned long long>(status.completed_ops),
            status.failed_path.c_str(), status.last_error.c_str(),
            static_cast<unsigned long long>(pending ? pending->token : 0), pending ? pending->plan.targets.size() : 0);
    };
    auto wait = [&](uint64_t before) {
        const auto deadline = GetTickCount64() + 20000;
        while (GetTickCount64() < deadline && (manager.Status().completed_ops <= before || manager.Status().active)) {
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
            Sleep(5);
        }
        return manager.Status().completed_ops > before && !manager.Status().active;
    };
    OpRequest request; request.type = OpType::RecycleDelete; request.sources = {recycled.wstring()};
    auto before = manager.Status().completed_ops;
    manager.Submit(request);
    const bool recycle_finished = wait(before);
    diagnostics("recycle-finished");
    if (expect_refusal) {
        Check(recycle_finished && manager.Status().phase == OpPhase::Failed && std::filesystem::exists(recycled) &&
            manager.DrainCompletions().empty(), "real recycle refusal preserves the source and reports no false mutation");
        Check(!manager.CanUndo() && manager.UndoLabel().empty(), "real recycle refusal cannot create a false delete undo record");
    } else {
        Check(recycle_finished && manager.Status().phase == OpPhase::Completed && !std::filesystem::exists(recycled),
            "real authorized recycle completes through the production Shell adapter");
        before = manager.Status().completed_ops;
        manager.Undo();
        const bool undo_finished = wait(before);
        diagnostics("undo-finished");
        Check(undo_finished && std::filesystem::exists(recycled), "real exact recycle undo restores only the fixture");
    }
    Check(!saw_native.load(), "ordinary recycle displays no owned native Explorer window");
    request.type = OpType::RealDelete; request.sources = {permanent.wstring()};
    before = manager.Status().completed_ops; manager.Submit(request);
    const auto deadline = GetTickCount64() + 10000;
    while (!manager.PendingDeleteConfirmation() && GetTickCount64() < deadline) Sleep(5);
    const auto confirmation = manager.PendingDeleteConfirmation();
    Check(confirmation.has_value() && std::filesystem::exists(permanent), "permanent deletion requires the Pulse authorization decision before execution");
    if (expect_refusal && confirmation) {
        manager.ResolveDeleteConfirmation(confirmation->token, false);
        Check(wait(before) && std::filesystem::exists(permanent) && manager.Status().phase == OpPhase::Cancelled,
            "declining the Pulse permanent-delete decision preserves the file");
        before = manager.Status().completed_ops; manager.Submit(request);
        const auto retry_deadline = GetTickCount64() + 10000;
        while (!manager.PendingDeleteConfirmation() && GetTickCount64() < retry_deadline) Sleep(5);
        const auto retry_confirmation = manager.PendingDeleteConfirmation();
        Check(retry_confirmation.has_value() && retry_confirmation->token != confirmation->token,
            "a new permanent request requires a fresh Pulse authorization token");
        if (retry_confirmation) manager.ResolveDeleteConfirmation(retry_confirmation->token, true);
    } else if (confirmation) manager.ResolveDeleteConfirmation(confirmation->token, true);
    const bool permanent_finished = wait(before);
    diagnostics("permanent-finished");
    Check(permanent_finished && manager.Status().phase == OpPhase::Completed && !std::filesystem::exists(permanent),
        "confirmed permanent deletion completes through the production Shell adapter");
    Check(!saw_native.load(), "Pulse-confirmed permanent deletion displays no second native Explorer window");
    stop.store(true); observer.join(); manager.Stop();
    if (owner) DestroyWindow(owner);
    if (std::filesystem::exists(recycled)) DeleteFileW(recycled.c_str());
    if (std::filesystem::exists(permanent)) DeleteFileW(permanent.c_str());
    RemoveDirectoryW(root.c_str());
    return failures ? 1 : 0;
}
}
int wmain(int argc, wchar_t** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc == 2 && std::wstring(argv[1]) == L"--manual-restore") return ManualRestoreFixtures();
    if (argc == 2 && std::wstring(argv[1]) == L"--permission-feedback") return PermissionFeedbackFixtures();
    if (argc == 2 && std::wstring(argv[1]) == L"--operation-labels") return OperationLabelFixtures();
    if (argc == 2 && std::wstring(argv[1]) == L"--no-native-shell") return NoNativeShell();
    if (argc == 2 && std::wstring(argv[1]) == L"--no-native-refusal") return NoNativeShell(true);
    if (argc == 2 && std::wstring(argv[1]) == L"--safe-recycle-refusal") return SafeRecycleAdapter(true);
    if (argc == 2 && std::wstring(argv[1]) == L"--safe-recycle-success") return SafeRecycleAdapter(false);
    using namespace pulse::ops;
    const auto root = std::filesystem::absolute(std::filesystem::path(L"bench_data") /
        (L"ops-feedback-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64())));
    std::filesystem::create_directories(root);
    Check(pulse::shell::NeedsPermanentDeleteConfirmation(COPYENGINE_E_RECYCLE_SIZE_TOO_BIG) &&
        !pulse::shell::NeedsPermanentDeleteConfirmation(E_ACCESSDENIED) &&
        !pulse::shell::NeedsPermanentDeleteConfirmation(HRESULT_FROM_WIN32(ERROR_NETWORK_ACCESS_DENIED)),
        "only exact recycling failures request a new Pulse permanent confirmation");
    const auto source = root / L"locked.txt", target = root / L"renamed.txt";
    OpRequest rename;
    rename.type = OpType::Rename; rename.sources = {source.wstring()}; rename.new_name = target.filename().wstring();
    auto held = [&] { return CreateFileW(source.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr); };
    Write(source);
    {
        OpsManager manager; TransferIntegrationProbe::Initialize(manager);
        HANDLE lock = held(); Check(lock != INVALID_HANDLE_VALUE, "hold fixture source without delete sharing");
        TransferIntegrationProbe::Rename(manager, rename, 10);
        const auto status = manager.Status();
        Check(status.phase == OpPhase::Failed && status.lock_retry_available && status.failed_path == source.wstring() &&
            manager.IsLockedFailureCurrent(10), "single locked Rename arms Retry/Cancel even when Restart Manager owners are unavailable");
        Check(!manager.RetryLockedOperation(10, true), "single rename cannot offer process termination");
        if (lock != INVALID_HANDLE_VALUE) CloseHandle(lock);
        Check(manager.RetryLockedOperation(10, false) && TransferIntegrationProbe::RunQueued(manager) &&
            manager.Status().phase == OpPhase::Completed && !std::filesystem::exists(source) && std::filesystem::exists(target),
            "plain rename retry uses source identity and succeeds after holder closes");
        Check(!manager.RetryLockedOperation(10, false), "completed rename retry never replays the original task");
    }
    MoveFileW(target.c_str(), source.c_str());
    {
        OpsManager manager; TransferIntegrationProbe::Initialize(manager);
        HANDLE lock = held(); TransferIntegrationProbe::Rename(manager, rename, 20);
        if (lock != INVALID_HANDLE_VALUE) CloseHandle(lock);
        Check(manager.RetryLockedOperation(20, false) && TransferIntegrationProbe::RunQueued(manager, true) &&
            manager.Status().phase == OpPhase::Cancelled && std::filesystem::exists(source) && !std::filesystem::exists(target),
            "queued rename cancel follows the real generation decision path and never renames");
    }
    {
        OpsManager manager; TransferIntegrationProbe::Initialize(manager);
        HANDLE lock = held(); TransferIntegrationProbe::Rename(manager, rename, 30);
        if (lock != INVALID_HANDLE_VALUE) CloseHandle(lock);
        MoveFileW(source.c_str(), (root / L"old-source.txt").c_str()); Write(source, "new identity");
        Check(manager.RetryLockedOperation(30, false) && TransferIntegrationProbe::RunQueued(manager) &&
            manager.Status().phase == OpPhase::Failed && !manager.Status().lock_retry_available &&
            std::filesystem::exists(source) && !std::filesystem::exists(target), "rename retry refuses a replacement source identity");
    }
    {
        OpsManager manager; TransferIntegrationProbe::Initialize(manager);
        HANDLE lock = held(); TransferIntegrationProbe::Rename(manager, rename, 40);
        if (lock != INVALID_HANDLE_VALUE) CloseHandle(lock);
        Write(target, "new target");
        Check(manager.RetryLockedOperation(40, false) && TransferIntegrationProbe::RunQueued(manager) &&
            manager.Status().phase == OpPhase::Failed && !manager.Status().lock_retry_available && std::filesystem::exists(source),
            "rename retry never overwrites a target that appeared while the prompt was open");
    }
    const auto original = root / L"original.txt", payload = root / L"$Rfixture", index = root / L"$Ifixture";
    Write(payload); Index(index, original.wstring());
    RecycleUndoIdentity identity, current;
    Check(ValidateRecycleUndoPair(original.wstring(), payload.wstring(), identity), "worker recycle validator accepts intact fixture payload/index mapping");
    MoveFileW(payload.c_str(), (root / L"$Rsaved").c_str()); Write(payload, "replacement");
    Check(!ValidateRecycleUndoPair(original.wstring(), payload.wstring(), current, &identity), "worker recycle validator rejects replaced payload file identity");
    DeleteFileW(payload.c_str()); MoveFileW((root / L"$Rsaved").c_str(), payload.c_str());
    Index(index, (root / L"different.txt").wstring());
    Check(!ValidateRecycleUndoPair(original.wstring(), payload.wstring(), current), "worker recycle validator rejects metadata referring to a different original");
    Index(index, original.wstring()); Write(original);
    Check(!ValidateRecycleUndoPair(original.wstring(), payload.wstring(), current), "worker recycle validator refuses an occupied restore destination");
    DeleteFileW(original.c_str()); DeleteFileW(index.c_str());
    Check(!ValidateRecycleUndoPair(original.wstring(), payload.wstring(), current), "external removal of metadata invalidates the recycle undo pair");
    {
        OpsManager manager;
        Check(manager.UndoFromJson(L"[{\"type\":2,\"sup\":true,\"dest\":\"\",\"name\":\"\",\"src\":[\"C:\\\\missing.txt\"],\"dst\":[\"C:\\\\$Recycle.Bin\\\\missing\\\\$Rgone\"]}]"), "persisted recycle undo parses without trusting validity");
        Check(!manager.CanUndo() && manager.UndoLabel().empty(), "cached UI undo availability never exposes an unverified persisted deletion label");
        TransferIntegrationProbe::RefreshUndo(manager);
        Check(!manager.CanUndo() && manager.UndoLabel().empty(), "background verification permanently invalidates a missing recycle item");
    }
    {
        OpsManager manager;
        TransferIntegrationProbe::SeedCachedRecycleUndo(manager, original.wstring(), payload.wstring());
        Check(manager.CanUndo(), "known-mutation fixture starts from an enabled cached undo");
        TransferIntegrationProbe::KnownMutation(manager, payload.wstring());
        Check(!manager.CanUndo() && manager.UndoLabel().empty(), "known exact payload mutation immediately invalidates the stale delete undo label");
    }
    Check(root.parent_path() == std::filesystem::absolute(L"bench_data") && root.filename().wstring().starts_with(L"ops-feedback-"), "verify fixture-only cleanup target");
    if (root.parent_path() == std::filesystem::absolute(L"bench_data") && root.filename().wstring().starts_with(L"ops-feedback-"))
        std::filesystem::remove_all(root);
    return failures ? 1 : 0;
}
