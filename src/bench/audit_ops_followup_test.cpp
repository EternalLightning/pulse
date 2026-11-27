// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "../ops/ops_manager.h"
#include "../ops/drop_staging.h"
#include "../ops/delete_operation.h"
#include "../fs/fs_recycle.h"
#include "../shell_host/permanent_fileop.h"
#include "../common/current_user_security.h"
#include "../common/localization.h"
#include <objbase.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace pulse::ops {
struct DeleteIntegrationProbe {
    static void Seed(OpsManager& manager, const std::vector<std::wstring>& originals,
        const std::vector<std::wstring>& payloads, bool verified = true) {
        UndoEntry entry;
        entry.type = OpType::RecycleDelete; entry.sources = originals; entry.destinations = payloads;
        entry.supported = true; entry.recycle_verified = verified;
        for (size_t i = 0; i < originals.size(); ++i) {
            RecycleUndoIdentity identity;
            ValidateRecycleUndoPair(originals[i], payloads[i], identity, nullptr, false);
            entry.recycle_identities.push_back(identity);
        }
        manager.undo_.clear(); manager.undo_.push_back(std::move(entry));
        ++manager.undo_revision_; manager.accepting_ = true;
        manager.pending_recycle_undo_revision_ = 0;
    }
    static size_t History(const OpsManager& manager) { return manager.undo_.size(); }
    static size_t Remaining(const OpsManager& manager) { return manager.undo_.empty() ? 0 : manager.undo_.back().sources.size(); }
    static size_t Queued(const OpsManager& manager) { return manager.queue_.size(); }
    static OpRequest Take(OpsManager& manager) {
        auto request = manager.queue_.front().req; manager.queue_.pop_front(); return request;
    }
    static void Restore(OpsManager& manager, const OpRequest& request, bool cancelled = false) {
        manager.shell_cancel_requested_.store(cancelled);
        manager.RunManualRecycleRestore(request, +[](const std::wstring&, const std::wstring&) { return true; });
        manager.shell_cancel_requested_.store(false);
        manager.FinishRecycleUndo(request);
    }
    static void Release(OpsManager& manager, const OpRequest& request) { manager.FinishRecycleUndo(request); }
    static void Reject(OpsManager& manager) { manager.accepting_ = false; }
    static void SetNotify(OpsManager& manager, std::function<void()> notify) { manager.notify_ = std::move(notify); }
    static bool RecycleReserved(const OpsManager& manager) { return manager.pending_recycle_undo_revision_ != 0; }
    static void ResumeAcceptance(OpsManager& manager) { manager.accepting_ = true; manager.stopping_.store(false); }
    static void MarkQueuedRetry(OpsManager& manager, bool elevated) {
        manager.queue_.front().elevated_retry = elevated;
        manager.queue_.front().lock_retry = !elevated;
    }
    static void QueueStaleRetry(OpsManager& manager, OpRequest request) {
        ++request.undo_revision;
        OpsManager::QueueItem item;
        item.req = std::move(request); item.elevated_retry = true;
        manager.queue_.push_back(std::move(item));
    }
};
}

namespace {
namespace fs = std::filesystem;
int failures = 0;
void Check(bool good, const char* label) {
    std::cout << (good ? "[PASS] " : "[FAIL] ") << label << '\n';
    failures += !good;
}
std::string Read(const fs::path& name) {
    std::ifstream stream(name, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}
void Pair(const fs::path& original, const fs::path& payload, uint64_t bytes = 22) {
    fs::create_directories(payload.parent_path());
    std::ofstream(payload) << "retained recycle bytes";
    std::ofstream stream(pulse::fs::RecycleIndexPath(payload.wstring()), std::ios::binary);
    const uint64_t version = 2, deleted = 0;
    const auto name = original.wstring() + std::wstring(1, L'\0');
    const auto length = static_cast<uint32_t>(name.size());
    stream.write(reinterpret_cast<const char*>(&version), sizeof(version));
    stream.write(reinterpret_cast<const char*>(&bytes), sizeof(bytes));
    stream.write(reinterpret_cast<const char*>(&deleted), sizeof(deleted));
    stream.write(reinterpret_cast<const char*>(&length), sizeof(length));
    stream.write(reinterpret_cast<const char*>(name.data()), name.size() * sizeof(wchar_t));
}
void RecycleEnumeration(const fs::path& root) {
    const auto sid = pulse::CurrentUserSidString();
    Check(!sid.empty(), "fixture resolves current user SID without touching the real Recycle Bin");
    if (sid.empty()) return;
    const auto bin = root / L"bin", user = bin / sid;
    fs::create_directories(user);
    Pair(root / L"original.txt", user / L"$Rvalid");
    std::vector<pulse::fs::DirEntry> entries;
    const auto check_enumeration = [&](bool complete, size_t count, const char* label) {
        entries.clear();
        const bool strict = pulse::fs::EnumerateRecycleBinAtRoot(bin.wstring(), entries);
        const size_t strict_count = entries.size();
        entries.clear();
        const bool view = pulse::fs::EnumerateRecycleViewAtRoot(bin.wstring(), entries);
        Check(strict == complete && view == complete && strict_count == count && entries.size() == count, label);
    };
    check_enumeration(true, 1,
        "complete synthetic bin enumerates its valid pair");
    std::ofstream(user / L"$Ibroken", std::ios::binary) << "bad";
    std::ofstream(user / L"$Rbroken") << "payload with damaged metadata";
    check_enumeration(false, 1,
        "damaged live metadata prevents complete-success while preserving valid rows");
    fs::remove(user / L"$Ibroken"); fs::remove(user / L"$Rbroken");
    std::ofstream(user / L"$Rorphan") << "orphan bytes";
    check_enumeration(false, 1,
        "orphan payload prevents complete-success while preserving valid rows");
    fs::remove(user / L"$Rorphan");
    const HANDLE locked = CreateFileW((user / L"$Ivalid").c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    Check(locked != INVALID_HANDLE_VALUE, "fixture can lock recycle metadata");
    check_enumeration(false, 0, "unreadable live metadata cannot be reported as an empty complete bin");
    if (locked != INVALID_HANDLE_VALUE) CloseHandle(locked);
    fs::remove(user / L"$Rvalid");
    check_enumeration(true, 0,
        "payload-free indexes retained by exact restore are harmless empty-bin metadata");

    const auto file = user / L"$rfile", folder = user / L"$Rfolder";
    Pair(root / L"file.txt", file, 0);
    Pair(root / L"folder", folder, 0);
    fs::remove(folder);
    fs::create_directory(folder);
    check_enumeration(true, 2, "view finds file and directory payloads including a lowercase prefix");
    bool file_attributes = false, folder_attributes = false;
    for (const auto& entry : entries) {
        if (entry.name == L"file.txt")
            file_attributes = !entry.is_dir && entry.size == fs::file_size(file) &&
                entry.recycle_path == pulse::fs::NormalizePath(file.wstring());
        if (entry.name == L"folder")
            folder_attributes = entry.is_dir && entry.size == 0 &&
                entry.recycle_path == pulse::fs::NormalizePath(folder.wstring());
    }
    Check(file_attributes && folder_attributes, "view reuses directory attributes and the fallback file size");
    entries.clear();
    Check(!pulse::fs::EnumerateRecycleViewAtRoot(bin.wstring(), entries, [&] { return !entries.empty(); }) &&
        entries.size() == 1, "cancelled view scan stops before reading the remaining payloads");
}
void UndoRetention(const fs::path& root) {
    using namespace pulse::ops;
    fs::create_directories(root);
    const auto original = root / L"original.txt", payload = root / L"$Rfirst";
    Pair(original, payload);
    size_t notifications = 0;
    bool notified_can_undo = false;
    OpsManager manager;
    DeleteIntegrationProbe::SetNotify(manager, [&] { ++notifications; notified_can_undo = manager.CanUndo(); });
    DeleteIntegrationProbe::Seed(manager, {original.wstring()}, {payload.wstring()});
    manager.Undo(); manager.Undo();
    Check(DeleteIntegrationProbe::History(manager) == 1 && DeleteIntegrationProbe::Queued(manager) == 1 && !manager.CanUndo(),
        "queued recycle Undo reserves history and suppresses duplicate requests");
    auto request = DeleteIntegrationProbe::Take(manager);
    std::ofstream(original) << "unrelated occupant";
    DeleteIntegrationProbe::Restore(manager, request);
    Check(DeleteIntegrationProbe::History(manager) == 1 && manager.CanUndo() && fs::exists(payload) && Read(original) == "unrelated occupant",
        "failed restore keeps its history and never overwrites an occupied destination");
    Check(notified_can_undo, "failed restore notifies after reservation release so the UI sees Undo re-enabled");
    fs::remove(original);
    manager.Undo(); request = DeleteIntegrationProbe::Take(manager);
    DeleteIntegrationProbe::Restore(manager, request);
    Check(DeleteIntegrationProbe::History(manager) == 0 && !fs::exists(payload) && Read(original) == "retained recycle bytes",
        "retry restores exact retained payload and consumes history only on success");

    const auto cancelled_original = root / L"cancelled.txt", cancelled_payload = root / L"$Rcancelled";
    Pair(cancelled_original, cancelled_payload);
    DeleteIntegrationProbe::Seed(manager, {cancelled_original.wstring()}, {cancelled_payload.wstring()});
    manager.Undo(); request = DeleteIntegrationProbe::Take(manager);
    DeleteIntegrationProbe::Restore(manager, request, true);
    Check(DeleteIntegrationProbe::History(manager) == 1 && manager.CanUndo() && notified_can_undo &&
        fs::exists(cancelled_payload) && !fs::exists(cancelled_original),
        "cancelled restore preserves payload and retryable history");
    DeleteIntegrationProbe::Reject(manager); manager.Undo();
    Check(DeleteIntegrationProbe::History(manager) == 1 && DeleteIntegrationProbe::Queued(manager) == 0 &&
        manager.CanUndo() && notified_can_undo,
        "rejected submission releases the reservation without consuming history");

    const auto partial_original = root / L"partial.txt", partial_payload = root / L"$Rpartial";
    Pair(partial_original, partial_payload);
    DeleteIntegrationProbe::Seed(manager, {cancelled_original.wstring(), partial_original.wstring()},
        {cancelled_payload.wstring(), partial_payload.wstring()});
    std::ofstream(partial_original) << "preserved occupant";
    manager.Undo(); request = DeleteIntegrationProbe::Take(manager);
    DeleteIntegrationProbe::Restore(manager, request);
    Check(DeleteIntegrationProbe::Remaining(manager) == 1 && manager.CanUndo() && fs::exists(partial_payload) &&
        Read(cancelled_original) == "retained recycle bytes" && Read(partial_original) == "preserved occupant",
        "partial restore consumes only successful members and retains the failed member");
    const auto later_original = root / L"later.txt", later_payload = root / L"$Rlater";
    Pair(later_original, later_payload);
    DeleteIntegrationProbe::Seed(manager, {partial_original.wstring(), later_original.wstring()},
        {partial_payload.wstring(), later_payload.wstring()});
    manager.Undo(); request = DeleteIntegrationProbe::Take(manager);
    DeleteIntegrationProbe::Restore(manager, request);
    Check(DeleteIntegrationProbe::Remaining(manager) == 1 && fs::exists(partial_payload) && !fs::exists(later_payload) &&
        Read(later_original) == "retained recycle bytes", "partial completion maps a later successful member to its exact payload identity");

    for (const bool elevated : {false, true}) {
        const auto queued_original = root / (elevated ? L"queued-elevated.txt" : L"queued-locked.txt");
        const auto queued_payload = root / (elevated ? L"$Rqueued-elevated" : L"$Rqueued-locked");
        Pair(queued_original, queued_payload);
        DeleteIntegrationProbe::Seed(manager, {queued_original.wstring()}, {queued_payload.wstring()});
        manager.Undo();
        DeleteIntegrationProbe::MarkQueuedRetry(manager, elevated);
        manager.CancelCurrent();
        Check(DeleteIntegrationProbe::History(manager) == 1 && DeleteIntegrationProbe::Queued(manager) == 0 &&
            !DeleteIntegrationProbe::RecycleReserved(manager) && manager.CanUndo() && notified_can_undo &&
            Read(queued_payload) == "retained recycle bytes" && !fs::exists(queued_original),
            elevated ? "cancelling a queued elevation retry releases its reservation without consuming history" :
                "cancelling a queued lock retry releases its reservation without consuming history");
        manager.Undo();
        Check(DeleteIntegrationProbe::Queued(manager) == 1 && DeleteIntegrationProbe::RecycleReserved(manager),
            "cancelled queued restore can reserve its retained history again");
        request = DeleteIntegrationProbe::Take(manager);
        DeleteIntegrationProbe::Restore(manager, request);
        Check(DeleteIntegrationProbe::History(manager) == 0 && Read(queued_original) == "retained recycle bytes" && !fs::exists(queued_payload),
            "retry after queued cancellation restores the same retained payload");
    }

    const auto shutdown_original = root / L"shutdown.txt", shutdown_payload = root / L"$Rshutdown";
    Pair(shutdown_original, shutdown_payload);
    DeleteIntegrationProbe::Seed(manager, {shutdown_original.wstring()}, {shutdown_payload.wstring()});
    manager.Undo(); request = DeleteIntegrationProbe::Take(manager);
    DeleteIntegrationProbe::QueueStaleRetry(manager, request);
    manager.CancelCurrent();
    Check(DeleteIntegrationProbe::RecycleReserved(manager) && !manager.CanUndo() && DeleteIntegrationProbe::History(manager) == 1,
        "cancelling a stale queued retry cannot release a different active reservation");
    DeleteIntegrationProbe::Release(manager, request);
    manager.Undo();
    manager.BeginShutdown();
    Check(DeleteIntegrationProbe::Queued(manager) == 0 && !DeleteIntegrationProbe::RecycleReserved(manager) &&
        DeleteIntegrationProbe::History(manager) == 1 && manager.CanUndo() && notified_can_undo &&
        Read(shutdown_payload) == "retained recycle bytes" && !fs::exists(shutdown_original),
        "shutdown releases queued recycle reservations and preserves retryable history and payload");
    DeleteIntegrationProbe::ResumeAcceptance(manager);
    manager.Undo(); request = DeleteIntegrationProbe::Take(manager);
    DeleteIntegrationProbe::Restore(manager, request);
    Check(DeleteIntegrationProbe::History(manager) == 0 && Read(shutdown_original) == "retained recycle bytes" && !fs::exists(shutdown_payload),
        "retained shutdown history can restore after accepting operations resumes");

    DeleteIntegrationProbe::Seed(manager, {partial_original.wstring()}, {partial_payload.wstring()}, false);
    DeleteIntegrationProbe::Reject(manager);
    manager.Undo();
    Check(DeleteIntegrationProbe::History(manager) == 1 && DeleteIntegrationProbe::Queued(manager) == 0 &&
        !DeleteIntegrationProbe::RecycleReserved(manager), "closed submission cannot reserve or queue an unverified recycle undo");
    DeleteIntegrationProbe::ResumeAcceptance(manager);
    manager.Undo(); request = DeleteIntegrationProbe::Take(manager);
    Check(DeleteIntegrationProbe::History(manager) == 1 && request.recycle_undo_identities.empty(),
        "worker-validation request keeps unverified recycle history until completion");
    const size_t before_release = notifications;
    DeleteIntegrationProbe::Release(manager, request);
    Check(notifications == before_release + 1, "matching recycle reservation release emits one notification outside the state lock");
    DeleteIntegrationProbe::Release(manager, request);
    Check(notifications == before_release + 1, "repeated recycle reservation release emits no duplicate notification");
}
void PermanentDeletion(const fs::path& root) {
    using namespace pulse;
    FileIdentity identity;
    const auto nested = root / L"nested-selection", nested_child = nested / L"child.txt";
    fs::create_directories(nested); std::ofstream(nested_child) << "nested root fixture";
    ops::OpRequest nested_request; nested_request.type = ops::OpType::RealDelete;
    nested_request.sources = {nested.wstring(), nested_child.wstring()};
    std::wstring planning_error;
    const auto plan = ops::BuildDeletePlan(nested_request, 1, planning_error);
    Check(planning_error.empty() && plan.targets.size() == 1 && plan.targets.front().physical_paths == std::vector<std::wstring>{nested.wstring()},
        "parent and child selection is reduced to one exact non-reparse execution root before authorization");
    ReadPathFileIdentity(nested.wstring(), identity);
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE))) {
        Check(false, "initialize the same COM apartment as the Shell operation worker");
        return;
    }
    const auto result = shell::DeletePermanentAuthorized({nested.wstring()}, {identity}, {});
    CoUninitialize();
    if (FAILED(result.error))
        std::cout << "[DIAG] permanent deletion HRESULT=0x" << std::hex
                  << static_cast<unsigned long>(result.error) << std::dec
                  << " identity_valid=" << identity.Valid() << '\n';
    Check(SUCCEEDED(result.error) && !fs::exists(nested), "normalized search or tray selection deletes its nonempty parent without handle conflicts");
}
void PermanentPlan(const fs::path& root) {
    using namespace pulse::ops;
    const auto directory = root / L"selected";
    fs::create_directories(directory / L"subdir");
    const auto child = directory / L"subdir" / L"child.txt";
    const auto sibling = root / L"selected-other" / L"child.txt";
    const auto case_variant = root / L"Selected" / L"child.txt";
    const auto missing_intermediate = directory / L"missing" / L"child.txt";
    const auto file = directory / L"file.txt";
    const auto file_intermediate = file / L"child.txt";
    std::ofstream(file) << "not a directory";
    const auto selected = pulse::fs::NormalizePath(directory.wstring());

    OpRequest request;
    request.type = OpType::RealDelete;
    const auto check_plan = [&](const std::vector<std::wstring>& expected, const char* label) {
        std::wstring error;
        const auto plan = BuildDeletePlan(request, 1, error);
        std::vector<std::wstring> actual;
        for (const auto& target : plan.targets) actual.push_back(target.path);
        Check(error.empty() && actual == expected, label);
    };
    const auto alias = directory / L"subdir" / L".." / L"alias.txt";
    request.sources = {child.wstring(), sibling.wstring(), case_variant.wstring(), missing_intermediate.wstring(),
        selected, file_intermediate.wstring(), directory.wstring(), alias.wstring()};
    check_plan({sibling.wstring(), case_variant.wstring(), missing_intermediate.wstring(), selected,
        file_intermediate.wstring(), directory.wstring()},
        "normalized ancestry preserves spelling, component, missing-directory and non-directory boundaries and order");

    request.sources.clear();
    const auto other = root / L"other";
    fs::create_directory(other);
    request.delete_targets = {{L"single child", {(other / L"child.txt").wstring()}, false},
        {L"paired parent", {other.wstring(), file.wstring()}, false},
        {L"paired child", {child.wstring(), file.wstring()}, false}, {L"single parent", {selected}, false}};
    check_plan({L"single child", L"paired parent", L"paired child", L"single parent"},
        "synthetic targets with multiple physical paths neither provide nor inherit recursive coverage");

    request.delete_targets.clear();
    request.sources = {selected, (other / L".." / L"lossy.txt").wstring()};
    std::wstring error;
    const auto lossy = BuildDeletePlan(request, 1, error);
    Check(lossy.targets.empty() && !error.empty(), "uncovered lossy Shell path still blocks the plan");

    const auto target = root / L"link-target", link = directory / L"link";
    fs::create_directories(target / L"subdir");
    if (CreateSymbolicLinkW(link.c_str(), target.c_str(),
        SYMBOLIC_LINK_FLAG_DIRECTORY | SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE)) {
        const auto link_child = link / L"subdir" / L"child.txt";
        request.sources = {link.wstring(), link_child.wstring(), selected};
        check_plan({link_child.wstring(), selected}, "recursive coverage stops at selected and intermediate reparse points");
        fs::remove(link);
    } else {
        std::cout << "[SKIP] reparse boundary fixture could not create a directory symlink (" << GetLastError() << ")\n";
    }
}
void PermanentPlanScaling(const fs::path& root) {
    using namespace pulse::ops;
    using Clock = std::chrono::steady_clock;
    OpRequest request;
    request.type = OpType::RealDelete;
    for (const size_t count : {4000, 8000}) {
        while (request.sources.size() < count) {
            const auto name = L"sibling-" + std::to_wstring(request.sources.size()) + L".txt";
            request.sources.push_back((root / name).wstring());
        }
        std::wstring error;
        const auto start = Clock::now();
        const auto plan = BuildDeletePlan(request, 1, error);
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start);
        std::cout << "[TIME] " << count << " permanent sibling targets in " << elapsed.count() << " ms\n";
        bool ordered = plan.targets.size() == count;
        for (size_t i = 0; ordered && i < count; ++i) ordered = plan.targets[i].path == request.sources[i];
        Check(error.empty() && ordered, "large permanent sibling selection retains every target in request order");
    }
}
void RemoteLikeDeletionPlan() {
    using namespace pulse;
    wchar_t windows[MAX_PATH]{};
    const UINT length = GetWindowsDirectoryW(windows, ARRAYSIZE(windows));
    if (!length || length >= ARRAYSIZE(windows) || windows[1] != L':') {
        Check(false, "test has a drive-letter system volume");
        return;
    }
    const wchar_t drive[]{windows[0], L'\0'};
    const std::wstring fixture = std::wstring(drive) + L":\\Pulse-remote-like-plan-fixture\\file.txt";
    ops::OpRequest request;
    request.type = ops::OpType::RecycleDelete;
    request.sources = {fixture};

    SetEnvironmentVariableW(L"PULSE_TEST_REMOTE_LIKE_DRIVES", nullptr);
    std::wstring error;
    const auto local = ops::BuildDeletePlan(request, 1, error);
    Check(error.empty() && local.targets.size() == 1 &&
        local.targets.front().disposition == ops::DeleteDisposition::RecycleRequested,
        "ordinary fixed volume keeps recycle intent");

    const bool configured = SetEnvironmentVariableW(L"PULSE_TEST_DATA_DIR", L"delete-plan-fixture") &&
        SetEnvironmentVariableW(L"PULSE_TEST_REMOTE_LIKE_DRIVES", drive);
    Check(configured, "test process simulates a remote-like mount on the same fixed drive");
    error.clear();
    const auto remote = ops::BuildDeletePlan(request, 2, error);
    Check(error.empty() && remote.targets.size() == 1 &&
        remote.targets.front().disposition == ops::DeleteDisposition::Permanent &&
        remote.targets.front().physical_paths == request.sources && !remote.targets.front().reason.empty(),
        "remote-like mounted volume produces an exact permanent-delete plan with a reason");
    ops::DeleteService gate;
    const auto decision = gate.Prepare(remote);
    Check(decision.decision == ops::DeleteDecision::AwaitingConfirmation &&
        !gate.TakeAccepted(decision.token) && !gate.Resolve(decision.token + 1, true) &&
        gate.Resolve(decision.token, false) && !gate.TakeAccepted(decision.token),
        "simulated mount deletion cannot execute without exact explicit confirmation");
}
void StagedTray(const fs::path& root) {
    const auto temp = root / L"temp", stages = temp / L"PulseDrop", source = temp / L"provider" / L"item.txt";
    fs::create_directories(source.parent_path()); std::ofstream(source) << "provider bytes";
    std::vector<std::wstring> tray;
    const auto staged = pulse::ops::SubmitStagedDrop({source.wstring()}, temp.wstring(), stages.wstring(),
        [&](const auto& prepared) { tray = prepared; });
    fs::remove(source);
    Check(!staged.error && staged.any && tray.size() == 1 && tray.front() != source.wstring() && Read(tray.front()) == "provider bytes",
        "tray preparation retains provider bytes and publishes staged paths before Drop returns");
    pulse::ops::SweepDropStages(stages.wstring(), true);
}
}

int wmain(int argc, wchar_t** argv) {
    pulse::l10n::Initialize(GetModuleHandleW(nullptr), L"en-US");
    if (argc == 2 && std::wstring_view(argv[1]) == L"--remote-like-plan") {
        RemoteLikeDeletionPlan();
        return failures ? 1 : 0;
    }
    const auto parent = fs::absolute(L"bench_data").lexically_normal();
    const auto root = parent / (L"audit-ops-followup-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    fs::create_directories(root);
    const std::wstring mode = argc > 1 ? argv[1] : L"all";
    if (mode != L"all" && mode != L"--recycle-enumeration" && mode != L"--undo-retention" &&
        mode != L"--permanent" && mode != L"--permanent-plan" && mode != L"--staged-tray") Check(false, "unknown fixture mode");
    if (mode == L"all" || mode == L"--recycle-enumeration") RecycleEnumeration(root / L"enumeration");
    if (mode == L"all" || mode == L"--undo-retention") UndoRetention(root / L"undo");
    if (mode == L"all" || mode == L"--permanent") PermanentDeletion(root / L"permanent");
    if (mode == L"all" || mode == L"--permanent-plan") {
        PermanentPlan(root / L"permanent-plan");
        PermanentPlanScaling(root / L"permanent-siblings");
    }
    if (mode == L"all" || mode == L"--staged-tray") StagedTray(root / L"staging");
    if (root.parent_path() == parent && root.filename().wstring().starts_with(L"audit-ops-followup-")) fs::remove_all(root);
    return failures ? 1 : 0;
}
