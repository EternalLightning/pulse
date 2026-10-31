// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once
#include "../ops/ops_manager.h"
#include "../common/localization.h"
#include <algorithm>
#include <cstdio>
#include <cwchar>
#include <filesystem>
#include <iterator>
#include <string>
#include <vector>

namespace pulse::ops {

struct MutationUndoTestProbe {
    static void Execute(OpsManager& manager, OpRequest request, bool cancelled = false,
                        bool force_copy = false) {
        OpsManager::QueueItem item;
        {
            std::lock_guard lock(manager.mutex_);
            manager.running_ = true;
            manager.accepting_ = true;
            item.seq = manager.next_seq_++;
        }
        manager.stopping_.store(cancelled);
        manager.force_copy_move_ = force_copy;
        item.req = std::move(request);
        manager.ExecuteFileOperation(item);
        manager.stopping_.store(false);
    }
    static void Record(OpsManager& manager, const OpRequest& request,
                       const std::vector<std::wstring>& destinations,
                       const std::vector<FileIdentity>* identities) {
        manager.PushUndo(request, &destinations, identities);
    }
    static std::vector<OpRequest> TakeInverses(OpsManager& manager, bool accepting = true) {
        {
            std::lock_guard lock(manager.mutex_);
            manager.accepting_ = accepting;
        }
        manager.Undo();
        std::lock_guard lock(manager.mutex_);
        std::vector<OpRequest> requests;
        while (!manager.queue_.empty()) {
            requests.push_back(std::move(manager.queue_.front().req));
            manager.queue_.pop_front();
        }
        return requests;
    }
    static std::vector<UndoEntry> History(OpsManager& manager) {
        std::lock_guard lock(manager.mutex_);
        return {manager.undo_.begin(), manager.undo_.end()};
    }
    static bool ExecuteQueued(OpsManager& manager) {
        OpsManager::QueueItem item;
        {
            std::lock_guard lock(manager.mutex_);
            if (manager.queue_.empty()) return false;
            item = std::move(manager.queue_.front());
            manager.queue_.pop_front();
        }
        manager.ExecuteFileOperation(item);
        return true;
    }
    static void Recover(OpsManager& manager, RecoveryEntry recovery) {
        std::lock_guard lock(manager.mutex_);
        manager.pending_recovery_.push_back(std::move(recovery));
    }
};

inline int RunMutationUndoRegressions(const std::wstring& fixture_root) {
    int passed = 0, failed = 0;
    const auto check = [&](bool ok, const wchar_t* label) {
        wprintf(L"[%s] %s\n", ok ? L"PASS" : L"FAIL", label);
        ok ? ++passed : ++failed;
    };
    const auto exists = [](const std::wstring& path) {
        return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
    };
    const auto write = [](const std::wstring& path, const std::string& bytes) {
        const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, CREATE_NEW,
            FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return false;
        DWORD written = 0;
        const bool ok = WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) &&
            written == bytes.size();
        CloseHandle(file);
        return ok;
    };
    const auto read = [](const std::wstring& path) {
        const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return std::string{};
        char bytes[256]{};
        DWORD length = 0;
        const bool ok = ReadFile(file, bytes, sizeof(bytes), &length, nullptr) != FALSE;
        CloseHandle(file);
        return ok ? std::string(bytes, length) : std::string{};
    };
    const auto identity = [](const std::wstring& path) {
        FileIdentity result;
        ReadPathFileIdentity(path, result);
        return result;
    };
    const auto mkdir = [](const std::wstring& path) {
        std::error_code error;
        std::filesystem::create_directories(path, error);
        return !error;
    };
    const auto same_mapping = [](const std::vector<UndoEntry>& before,
                                 const std::vector<UndoEntry>& after) {
        if (before.size() != after.size()) return false;
        for (size_t i = 0; i < before.size(); ++i) {
            if (before[i].type != after[i].type || before[i].sources != after[i].sources ||
                before[i].destinations != after[i].destinations ||
                before[i].produced_identities != after[i].produced_identities ||
                before[i].cleanup_directories != after[i].cleanup_directories) return false;
        }
        return true;
    };
    const auto inverse_matches = [](const std::vector<OpRequest>& inverses, const UndoEntry& history) {
        size_t matched = 0;
        for (const auto& inverse : inverses) {
            if (!inverse.is_undo || !inverse.undo_revision ||
                inverse.sources.size() != inverse.undo_produced_identities.size() ||
                inverse.undo_cleanup_directories != history.cleanup_directories) return false;
            for (size_t j = 0; j < inverse.sources.size(); ++j) {
                const auto found = std::find(history.destinations.begin(), history.destinations.end(), inverse.sources[j]);
                if (found == history.destinations.end()) return false;
                const size_t i = static_cast<size_t>(found - history.destinations.begin());
                if (i >= history.produced_identities.size() ||
                    inverse.undo_produced_identities[j] != history.produced_identities[i]) return false;
                if (history.type == OpType::Move) {
                    if (j >= inverse.new_names.size() || inverse.new_names[j] != history.sources[i]) return false;
                } else {
                    const auto original_name = std::filesystem::path(history.sources[i]).filename().wstring();
                    const auto name = inverse.type == OpType::BatchRename && j < inverse.new_names.size()
                        ? inverse.new_names[j] : inverse.new_name;
                    if (name != original_name || std::filesystem::path(inverse.sources[j]).parent_path() !=
                        std::filesystem::path(history.sources[i]).parent_path()) return false;
                }
                ++matched;
            }
        }
        return matched == history.sources.size();
    };
    const auto execute_inverses = [](OpsManager& manager, std::vector<OpRequest> inverses,
                                     bool cancelled = false, bool force_copy = false) {
        for (auto& inverse : inverses)
            MutationUndoTestProbe::Execute(manager, std::move(inverse), cancelled, force_copy);
    };
    const std::string original_bytes = "historical mutation product";
    const std::string replacement_bytes = "same-name replacement must remain";
    if (!mkdir(fixture_root)) {
        check(false, L"isolated mutation Undo fixture created");
        return 1;
    }

    for (const auto type : {OpType::Move, OpType::Rename, OpType::BatchRename}) {
        const auto dir = fixture_root + L"\\identity-" + std::to_wstring(static_cast<int>(type));
        const auto target = dir + L"\\target";
        const auto original = dir + L"\\full.original.name.txt";
        check(mkdir(target) && write(original, original_bytes), L"mutation identity fixture created");
        const auto original_id = identity(original);
        OpRequest request;
        request.type = type; request.sources = {original}; request.dest_dir = target;
        request.new_name = L"renamed.product.txt"; request.new_names = {request.new_name};
        OpsManager manager;
        MutationUndoTestProbe::Execute(manager, request);
        const auto product = type == OpType::Move ? target + L"\\full.original.name.txt"
            : dir + L"\\renamed.product.txt";
        const auto product_id = identity(product);
        check(manager.Status().phase == OpPhase::Completed && !exists(original) &&
            read(product) == original_bytes && product_id.Valid() && product_id == original_id,
            L"production Move/Rename/BatchRename commits the actual original object");
        const auto history = MutationUndoTestProbe::History(manager);
        check(history.size() == 1 && history.front().sources == request.sources &&
            history.front().destinations == std::vector<std::wstring>{product} &&
            history.front().produced_identities == std::vector<FileIdentity>{product_id},
            L"successful mutation records the historical product identity and actual paths");
        const auto json = manager.UndoToJson();
        OpsManager restored;
        const bool loaded = restored.UndoFromJson(json);
        check(loaded && restored.CanUndo() && same_mapping(history, MutationUndoTestProbe::History(restored)),
            L"mutation history JSON round-trip retains identities and path mappings");
        auto inverses = MutationUndoTestProbe::TakeInverses(restored);
        check(!history.empty() && inverse_matches(inverses, history.front()),
            L"mutation inverse carries the historical identity, original destination and reservation");
        check(same_mapping(history, MutationUndoTestProbe::History(restored)) && !restored.CanUndo() &&
            MutationUndoTestProbe::TakeInverses(restored).empty(),
            L"queued mutation Undo reserves history and rejects a duplicate Undo");

        const auto displaced = product + L".historical";
        const bool replaced = MoveFileExW(product.c_str(), displaced.c_str(), 0) &&
            write(product, replacement_bytes);
        const auto replacement_id = identity(product);
        check(replaced && replacement_id.Valid() && replacement_id != product_id,
            L"replacement fixture retains historical object elsewhere and has a distinct FileID");
        if (replaced && !inverses.empty()) {
            execute_inverses(restored, std::move(inverses));
            const auto status = restored.Status();
            check(status.phase == OpPhase::Failed && FAILED(status.failure_hr) && !status.last_error.empty() &&
                !exists(original) && read(product) == replacement_bytes && identity(product) == replacement_id &&
                read(displaced) == original_bytes && identity(displaced) == product_id,
                L"mutation Undo fails closed and leaves replacement paths, identities and bytes intact");
            check(same_mapping(history, MutationUndoTestProbe::History(restored)),
                L"identity rejection preserves the historical mutation mapping");
        }

        auto legacy_json = json;
        const auto produced = legacy_json.find(L",\"produced\":[");
        const auto produced_end = produced == std::wstring::npos ? std::wstring::npos : legacy_json.find(L']', produced);
        if (produced_end != std::wstring::npos) legacy_json.erase(produced, produced_end - produced + 1);
        OpsManager legacy;
        check(legacy.UndoFromJson(legacy_json) && !legacy.CanUndo() &&
            MutationUndoTestProbe::TakeInverses(legacy).empty() && read(product) == replacement_bytes,
            L"legacy mutation JSON without historical identity parses but cannot authorize current objects");
        for (const bool invalid : {false, true}) {
            OpsManager unknown;
            const std::vector<std::wstring> destinations{product};
            const std::vector<FileIdentity> invalid_ids{FileIdentity{}};
            MutationUndoTestProbe::Record(unknown, request, destinations, invalid ? &invalid_ids : nullptr);
            check(!unknown.CanUndo() && MutationUndoTestProbe::TakeInverses(unknown).empty() &&
                !MutationUndoTestProbe::History(unknown).empty(),
                L"missing or invalid mutation product identity retains history without enabling Undo");
        }
    }

    for (const auto type : {OpType::Move, OpType::Rename, OpType::BatchRename}) {
        const auto dir = fixture_root + L"\\stale-revision-" + std::to_wstring(static_cast<int>(type));
        const auto target = dir + L"\\target", original = dir + L"\\original.txt";
        const auto product = type == OpType::Move ? target + L"\\original.txt" : dir + L"\\renamed.txt";
        check(mkdir(target) && write(original, original_bytes), L"stale mutation inverse fixture created");
        OpsManager manager;
        OpRequest request;
        request.type = type; request.sources = {original}; request.dest_dir = target;
        request.new_name = L"renamed.txt"; request.new_names = {request.new_name};
        MutationUndoTestProbe::Execute(manager, request);
        const auto product_id = identity(product);
        const auto history = MutationUndoTestProbe::History(manager);
        const auto json = manager.UndoToJson();
        auto inverses = MutationUndoTestProbe::TakeInverses(manager);
        check(!inverses.empty() && manager.UndoFromJson(json), L"history reload changes revision after a mutation inverse is dequeued");
        execute_inverses(manager, std::move(inverses));
        check(manager.Status().phase == OpPhase::Failed && manager.Status().failure_hr == E_ACCESSDENIED &&
            !exists(original) && read(product) == original_bytes && identity(product) == product_id &&
            same_mapping(history, MutationUndoTestProbe::History(manager)) && manager.CanUndo(),
            L"stale mutation inverse refuses execution, preserves exact historical product and releases old reservation");
        inverses = MutationUndoTestProbe::TakeInverses(manager);
        check(!inverses.empty(), L"fresh mutation Undo is available after stale inverse rejection");
        execute_inverses(manager, std::move(inverses));
        check(manager.Status().phase == OpPhase::Completed && read(original) == original_bytes &&
            identity(original) == product_id && !exists(product) && MutationUndoTestProbe::History(manager).empty(),
            L"fresh revision mutation Undo restores the historical object after stale request rejection");
    }

    {
        const auto dir = fixture_root + L"\\invalid-cleanup-json";
        const auto target = dir + L"\\target", original = dir + L"\\original.txt", product = target + L"\\original.txt";
        check(mkdir(target) && write(original, original_bytes), L"invalid cleanup JSON fixture created");
        OpsManager manager;
        OpRequest request;
        request.type = OpType::Move; request.sources = {original}; request.dest_dir = target;
        MutationUndoTestProbe::Execute(manager, request);
        const auto history = MutationUndoTestProbe::History(manager);
        const auto product_id = identity(product);
        const auto json = manager.UndoToJson();
        auto invalid_json = json;
        const auto cleanup = invalid_json.find(L"\"cleanup\":[]");
        if (cleanup != std::wstring::npos)
            invalid_json.replace(cleanup, std::wstring(L"\"cleanup\":[]").size(),
                L"\"cleanup\":[\"not-an-actual-destination\"]");
        check(cleanup != std::wstring::npos && !manager.UndoFromJson(invalid_json) &&
            manager.UndoToJson() == json && same_mapping(history, MutationUndoTestProbe::History(manager)) &&
            manager.CanUndo() && !exists(original) && read(product) == original_bytes && identity(product) == product_id,
            L"cleanup JSON outside actual destinations is rejected without changing current history or disk objects");
    }

    {
        const auto dir = fixture_root + L"\\shutdown-queued-inverse";
        const auto original = dir + L"\\original.txt", product = dir + L"\\renamed.txt";
        check(mkdir(dir) && write(original, original_bytes), L"shutdown queued mutation inverse fixture created");
        OpsManager manager;
        OpRequest request;
        request.type = OpType::Rename; request.sources = {original}; request.new_name = L"renamed.txt";
        MutationUndoTestProbe::Execute(manager, request);
        const auto history = MutationUndoTestProbe::History(manager);
        const auto product_id = identity(product);
        manager.Undo();
        check(manager.HasPendingFileOperations() && !manager.CanUndo(), L"ordinary mutation inverse queued before shutdown reserves history");
        manager.BeginShutdown();
        check(!manager.HasPendingFileOperations() && manager.CanUndo() &&
            same_mapping(history, MutationUndoTestProbe::History(manager)) && !exists(original) &&
            read(product) == original_bytes && identity(product) == product_id,
            L"shutdown discards queued mutation inverse, releases reservation and preserves history without starting threads");
    }

    for (const auto type : {OpType::Move, OpType::Rename, OpType::BatchRename}) {
        const auto dir = fixture_root + L"\\recovery-inverse-" + std::to_wstring(static_cast<int>(type));
        const auto target = dir + L"\\target", original = dir + L"\\original.txt";
        const auto product = type == OpType::Move ? target + L"\\original.txt" : dir + L"\\renamed.txt";
        check(mkdir(target) && write(original, original_bytes), L"recovered mutation inverse fixture created");
        OpsManager manager;
        OpRequest request;
        request.type = type; request.sources = {original}; request.dest_dir = target;
        request.new_name = L"renamed.txt"; request.new_names = {request.new_name};
        MutationUndoTestProbe::Execute(manager, request);
        const auto product_id = identity(product);
        const auto history = MutationUndoTestProbe::History(manager);
        const auto json = manager.UndoToJson();
        auto inverses = MutationUndoTestProbe::TakeInverses(manager);
        check(!inverses.empty(), L"recovery fixture captures a production mutation inverse recipe");
        if (inverses.empty()) continue;
        RecoveryEntry recovery;
        recovery.sequence = 71; recovery.request = std::move(inverses.front()); recovery.was_active = false;
        recovery.request.undo_revision = 0;
        recovery.request.undo_produced_identities.clear();
        OpsManager restored;
        check(restored.UndoFromJson(json) && restored.CanUndo(), L"ordinary historical Undo remains valid independently of recovered inverse journal");
        MutationUndoTestProbe::Recover(restored, recovery);
        const bool accepted = restored.RetryRecovery();
        const auto pending = restored.PendingRecovery();
        const auto& reservation_error = l10n::Get(l10n::StringId::OperationUndoReservationMissing);
        check(!accepted && !restored.HasPendingFileOperations() && pending.entries.size() == 1 &&
            pending.entries.front().sequence == recovery.sequence &&
            pending.entries.front().request.type == recovery.request.type &&
            pending.entries.front().request.is_undo && pending.entries.front().request.sources == recovery.request.sources &&
            restored.Status().phase == OpPhase::Failed && !reservation_error.empty() &&
            restored.Status().last_error == reservation_error &&
            same_mapping(history, MutationUndoTestProbe::History(restored)) && restored.CanUndo() &&
            !exists(original) && read(product) == original_bytes && identity(product) == product_id,
            L"recovered mutation inverse without live reservation is rejected before enqueue and retains recovery plus historical objects");
        check(!restored.RetryRecovery() && !restored.HasPendingFileOperations() && restored.PendingRecovery().entries.size() == 1,
            L"repeated recovery rejection never reauthorizes or silently consumes the recovered mutation inverse");
    }

    for (const auto type : {OpType::Rename, OpType::BatchRename}) {
        const auto dir = fixture_root + L"\\retry-" + std::to_wstring(static_cast<int>(type));
        const auto original = dir + L"\\original.txt", product = dir + L"\\renamed.txt";
        check(mkdir(dir) && write(original, original_bytes), L"rename retry fixture created");
        OpsManager manager;
        OpRequest request;
        request.type = type; request.sources = {original}; request.new_name = L"renamed.txt";
        request.new_names = {request.new_name};
        MutationUndoTestProbe::Execute(manager, request);
        const auto history = MutationUndoTestProbe::History(manager);
        const auto product_id = identity(product);
        check(write(original, replacement_bytes), L"inverse rename destination conflict fixture created");
        const auto blocker_id = identity(original);
        auto inverse = MutationUndoTestProbe::TakeInverses(manager);
        check(!inverse.empty(), L"conflicting inverse rename is admitted for production validation");
        execute_inverses(manager, std::move(inverse));
        check(manager.Status().phase == OpPhase::Failed && same_mapping(history, MutationUndoTestProbe::History(manager)) &&
            manager.CanUndo() && read(original) == replacement_bytes && identity(original) == blocker_id &&
            read(product) == original_bytes && identity(product) == product_id,
            L"inverse Rename/BatchRename conflict preserves both objects and retryable history");
        check(DeleteFileW(original.c_str()) != FALSE, L"only isolated conflicting fixture removed");
        inverse = MutationUndoTestProbe::TakeInverses(manager);
        check(!inverse.empty(), L"inverse rename retry remains available after conflict");
        execute_inverses(manager, std::move(inverse), true);
        check(manager.Status().phase == OpPhase::Cancelled && same_mapping(history, MutationUndoTestProbe::History(manager)) &&
            manager.CanUndo() && !exists(original) && identity(product) == product_id,
            L"cancelled inverse Rename/BatchRename keeps historical mapping for retry");
        check(MutationUndoTestProbe::TakeInverses(manager, false).empty() &&
            same_mapping(history, MutationUndoTestProbe::History(manager)) && manager.CanUndo() && !exists(original),
            L"Submit rejection releases the mutation reservation without consuming history");
        inverse = MutationUndoTestProbe::TakeInverses(manager);
        check(!inverse.empty(), L"inverse rename remains admitted after cancellation and Submit rejection");
        execute_inverses(manager, std::move(inverse));
        check(manager.Status().phase == OpPhase::Completed && read(original) == original_bytes &&
            identity(original) == product_id && !exists(product) && MutationUndoTestProbe::History(manager).empty(),
            L"successful inverse rename alone consumes history and restores original object");
    }

    for (const int retry_scenario : {0, 1, 2}) {
        const bool replace_before_retry = retry_scenario == 1;
        const bool cancel_queued_retry = retry_scenario == 2;
        const auto dir = fixture_root + (cancel_queued_retry ? L"\\rename-lock-retry-cancel" :
            (replace_before_retry ? L"\\rename-lock-replacement" : L"\\rename-lock-retry"));
        const auto original = dir + L"\\original.txt", product = dir + L"\\renamed.txt";
        check(mkdir(dir) && write(original, original_bytes), L"locked inverse Rename fixture created");
        OpsManager manager;
        OpRequest request;
        request.type = OpType::Rename; request.sources = {original}; request.new_name = L"renamed.txt";
        MutationUndoTestProbe::Execute(manager, request);
        const auto product_id = identity(product);
        const auto history = MutationUndoTestProbe::History(manager);
        const HANDLE blocker = CreateFileW(product.c_str(), GENERIC_READ, FILE_SHARE_READ,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        check(blocker != INVALID_HANDLE_VALUE, L"production inverse source held with a real delete-sharing denial");
        auto inverses = MutationUndoTestProbe::TakeInverses(manager);
        check(!inverses.empty(), L"locked inverse Rename is queued");
        execute_inverses(manager, std::move(inverses));
        const auto failed_status = manager.Status();
        check(failed_status.phase == OpPhase::Failed && failed_status.lock_retry_available &&
            manager.IsLockedFailureCurrent(failed_status.task_id) &&
            same_mapping(history, MutationUndoTestProbe::History(manager)) && manager.CanUndo() &&
            !exists(original) && read(product) == original_bytes,
            L"sharing failure retains mutation history and offers the production plain lock retry");
        if (blocker != INVALID_HANDLE_VALUE) CloseHandle(blocker);
        const auto displaced = product + L".historical";
        FileIdentity replacement_id;
        if (replace_before_retry) {
            check(MoveFileExW(product.c_str(), displaced.c_str(), 0) && write(product, replacement_bytes),
                L"locked Rename retry replacement preserves historical object elsewhere");
            replacement_id = identity(product);
            check(replacement_id.Valid() && replacement_id != product_id,
                L"locked Rename retry replacement has a distinct object identity");
        }
        const bool accepted = manager.RetryLockedOperation(failed_status.task_id, false);
        if (cancel_queued_retry) {
            check(accepted && !manager.CanUndo(), L"queued mutation lock retry reserves its historical mapping");
            manager.CancelCurrent();
            check(!MutationUndoTestProbe::ExecuteQueued(manager) && manager.CanUndo() &&
                same_mapping(history, MutationUndoTestProbe::History(manager)) && !exists(original) && identity(product) == product_id,
                L"cancelling a lock retry before dequeue releases reservation without consuming or modifying history");
            inverses = MutationUndoTestProbe::TakeInverses(manager);
            check(!inverses.empty(), L"ordinary Undo is available after queued lock retry cancellation");
            execute_inverses(manager, std::move(inverses));
        } else {
            check(accepted && MutationUndoTestProbe::ExecuteQueued(manager),
                L"public plain lock retry executes the queued production request with its original metadata");
        }
        if (replace_before_retry) {
            check(manager.Status().phase == OpPhase::Failed && !exists(original) &&
                read(product) == replacement_bytes && identity(product) == replacement_id &&
                read(displaced) == original_bytes && identity(displaced) == product_id &&
                same_mapping(history, MutationUndoTestProbe::History(manager)),
                L"lock retry never reauthorizes a replacement and retains historical mapping");
        } else {
            check(manager.Status().phase == OpPhase::Completed && !exists(product) &&
                read(original) == original_bytes && identity(original) == product_id &&
                MutationUndoTestProbe::History(manager).empty(),
                L"plain lock retry restores the historical object and consumes history on actual success");
        }
    }

    {
        const auto dir = fixture_root + L"\\batch-partial";
        const auto a = dir + L"\\a.original.txt", b = dir + L"\\b.original.txt";
        const auto a_product = dir + L"\\a.renamed.txt", b_product = dir + L"\\b.renamed.txt";
        check(mkdir(dir) && write(a, original_bytes) && write(b, original_bytes), L"partial batch Undo fixture created");
        OpsManager manager;
        OpRequest request;
        request.type = OpType::BatchRename; request.sources = {a, b};
        request.new_names = {L"a.renamed.txt", L"b.renamed.txt"};
        MutationUndoTestProbe::Execute(manager, request);
        const auto a_id = identity(a_product), b_id = identity(b_product);
        const auto history = MutationUndoTestProbe::History(manager);
        check(write(b, replacement_bytes), L"one batch inverse target blocked by an isolated file");
        auto inverses = MutationUndoTestProbe::TakeInverses(manager);
        check(!history.empty() && inverse_matches(inverses, history.front()),
            L"batch inverse matches every actual product with its own historical identity");
        execute_inverses(manager, std::move(inverses));
        const auto remaining = MutationUndoTestProbe::History(manager);
        check(read(a) == original_bytes && identity(a) == a_id && !exists(a_product) &&
            read(b) == replacement_bytes && read(b_product) == original_bytes && identity(b_product) == b_id,
            L"partial batch Undo restores the successful item and preserves the conflicted item");
        check(remaining.size() == 1 && remaining.front().sources == std::vector<std::wstring>{b} &&
            remaining.front().destinations == std::vector<std::wstring>{b_product} &&
            remaining.front().produced_identities == std::vector<FileIdentity>{b_id} && manager.CanUndo(),
            L"partial batch Undo consumes only successful mappings and retains aligned pending identities");
        check(DeleteFileW(b.c_str()) != FALSE, L"isolated batch retry blocker removed");
        OpsManager restored;
        check(restored.UndoFromJson(manager.UndoToJson()) && restored.CanUndo() &&
            same_mapping(remaining, MutationUndoTestProbe::History(restored)),
            L"partially consumed batch history persists with the remaining object's identity");
        inverses = MutationUndoTestProbe::TakeInverses(restored);
        check(!remaining.empty() && inverse_matches(inverses, remaining.front()),
            L"batch retry contains only the pending historical product");
        execute_inverses(restored, std::move(inverses));
        check(read(a) == original_bytes && identity(a) == a_id && read(b) == original_bytes && identity(b) == b_id &&
            !exists(b_product) && MutationUndoTestProbe::History(restored).empty(),
            L"batch retry restores pending item without repeating or damaging the previously successful item");
    }

    {
        const auto dir = fixture_root + L"\\batch-chain";
        const auto a = dir + L"\\a.txt", b = dir + L"\\b.txt", c = dir + L"\\c.txt";
        check(mkdir(dir) && write(a, original_bytes) && write(b, replacement_bytes),
            L"dependent batch rename fixture created");
        const auto a_id = identity(a), b_id = identity(b);
        OpsManager manager;
        OpRequest request;
        request.type = OpType::BatchRename; request.sources = {b, a};
        request.new_names = {L"c.txt", L"b.txt"};
        MutationUndoTestProbe::Execute(manager, request);
        check(!exists(a) && identity(b) == a_id && identity(c) == b_id,
            L"production batch rename commits dependent no-overwrite name mappings");
        auto inverses = MutationUndoTestProbe::TakeInverses(manager);
        std::vector<std::wstring> order;
        for (const auto& inverse : inverses) order.insert(order.end(), inverse.sources.begin(), inverse.sources.end());
        check(order == std::vector<std::wstring>{b, c},
            L"batch Undo reverses mutation order so dependent destinations become available");
        execute_inverses(manager, std::move(inverses));
        check(manager.Status().phase == OpPhase::Completed && read(a) == original_bytes && identity(a) == a_id &&
            read(b) == replacement_bytes && identity(b) == b_id && !exists(c) &&
            MutationUndoTestProbe::History(manager).empty(),
            L"reverse batch Undo restores both dependent names and exact historical objects");
    }

    for (const bool force_copy : {false, true}) {
        const auto dir = fixture_root + (force_copy ? L"\\move-copy-delete" : L"\\move-keep-both");
        const auto target = dir + L"\\target", original = dir + L"\\full.original.name.txt";
        const auto occupied = target + L"\\full.original.name.txt";
        check(mkdir(target) && write(original, original_bytes) && write(occupied, replacement_bytes),
            L"Move KeepBoth identity fixture created");
        const auto original_id = identity(original), occupied_id = identity(occupied);
        OpsManager manager;
        OpRequest request;
        request.type = OpType::Move; request.sources = {original}; request.dest_dir = target;
        request.collision_policy = CollisionPolicy::KeepBoth;
        MutationUndoTestProbe::Execute(manager, request, false, force_copy);
        const auto history = MutationUndoTestProbe::History(manager);
        const auto product = history.size() == 1 && history.front().destinations.size() == 1
            ? history.front().destinations.front() : std::wstring{};
        const auto product_id = identity(product);
        check(manager.Status().phase == OpPhase::Completed && !exists(original) && product != occupied &&
            read(product) == original_bytes && read(occupied) == replacement_bytes && identity(occupied) == occupied_id,
            L"KeepBoth Move commits the actual distinct destination and preserves the existing file");
        check(product_id.Valid() && (!force_copy || product_id != original_id) && history.size() == 1 &&
            history.front().produced_identities == std::vector<FileIdentity>{product_id},
            L"native and forced copy-delete Move record destination identity rather than stale source identity");
        OpsManager restored;
        check(restored.UndoFromJson(manager.UndoToJson()) && restored.CanUndo() &&
            same_mapping(history, MutationUndoTestProbe::History(restored)),
            L"KeepBoth and forced copy-delete Move identities survive JSON persistence");
        auto inverses = MutationUndoTestProbe::TakeInverses(restored);
        check(!history.empty() && inverse_matches(inverses, history.front()),
            L"KeepBoth Move inverse restores the exact full original filename using target identity");
        execute_inverses(restored, std::move(inverses), false, force_copy);
        check(restored.Status().phase == OpPhase::Completed && read(original) == original_bytes && !exists(product) &&
            read(occupied) == replacement_bytes && identity(occupied) == occupied_id &&
            MutationUndoTestProbe::History(restored).empty(),
            L"KeepBoth and forced copy-delete Move Undo restores original bytes and consumes only completed history");
    }

    {
        const auto dir = fixture_root + L"\\directory-root";
        const auto original = dir + L"\\folder", target = dir + L"\\target";
        const auto product = target + L"\\folder";
        check(mkdir(original) && mkdir(target) && write(original + L"\\child.txt", original_bytes),
            L"same-volume directory root fixture created");
        const auto root_id = identity(original), child_id = identity(original + L"\\child.txt");
        OpsManager manager;
        OpRequest request;
        request.type = OpType::Move; request.sources = {original}; request.dest_dir = target;
        MutationUndoTestProbe::Execute(manager, request);
        const auto history = MutationUndoTestProbe::History(manager);
        check(!exists(original) && identity(product) == root_id && history.size() == 1 &&
            history.front().produced_identities == std::vector<FileIdentity>{root_id},
            L"same-volume directory root rename captures the historical root product identity");
        auto inverses = MutationUndoTestProbe::TakeInverses(manager);
        check(!history.empty() && inverse_matches(inverses, history.front()),
            L"directory inverse is authorized by the historical root identity");
        execute_inverses(manager, std::move(inverses));
        check(manager.Status().phase == OpPhase::Completed && !exists(product) && identity(original) == root_id &&
            identity(original + L"\\child.txt") == child_id && read(original + L"\\child.txt") == original_bytes &&
            MutationUndoTestProbe::History(manager).empty(),
            L"unchanged directory root Undo retains root and child identities through the native rename path");
    }

    for (const bool replace_pending : {false, true}) {
        const auto dir = fixture_root + (replace_pending ? L"\\directory-partial-replacement" : L"\\directory-partial-retry");
        const auto original = dir + L"\\folder", target = dir + L"\\target", product = target + L"\\folder";
        const auto a = original + L"\\a.txt", b = original + L"\\b.txt";
        const auto product_a = product + L"\\a.txt", product_b = product + L"\\b.txt";
        check(mkdir(original) && mkdir(target) && write(a, original_bytes) && write(b, original_bytes),
            L"partial directory mutation fixture created");
        const auto root_id = identity(original), a_id = identity(a), b_id = identity(b);
        OpsManager manager;
        OpRequest request;
        request.type = OpType::Move; request.sources = {original}; request.dest_dir = target;
        MutationUndoTestProbe::Execute(manager, request);
        check(identity(product) == root_id && mkdir(original),
            L"directory product has historical root identity and original destination now requires a merge");
        const HANDLE blocker = CreateFileW(product_b.c_str(), GENERIC_READ, FILE_SHARE_READ,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        check(blocker != INVALID_HANDLE_VALUE, L"one pending directory leaf held by a real delete-sharing denial");
        auto inverses = MutationUndoTestProbe::TakeInverses(manager);
        check(!inverses.empty(), L"partial directory inverse queued");
        execute_inverses(manager, std::move(inverses));
        check(read(a) == original_bytes && identity(a) == a_id && !exists(product_a) && !exists(b) &&
            identity(product_b) == b_id && manager.CanUndo(),
            L"partial directory Undo restores only the unlocked leaf and leaves locked content retryable");
        if (blocker != INVALID_HANDLE_VALUE) CloseHandle(blocker);
        const auto remaining = MutationUndoTestProbe::History(manager);
        bool aligned = remaining.size() == 1 && !remaining.front().sources.empty() &&
            remaining.front().sources.size() == remaining.front().destinations.size() &&
            remaining.front().sources.size() == remaining.front().produced_identities.size();
        bool has_pending_leaf = false;
        if (aligned) {
            const auto& entry = remaining.front();
            for (size_t i = 0; i < entry.sources.size(); ++i) {
                const auto expected = entry.destinations[i] == product_b ? b_id : root_id;
                aligned = aligned && entry.sources[i] != a && entry.destinations[i] != product_a &&
                    entry.produced_identities[i] == expected &&
                    entry.produced_identities[i] == identity(entry.destinations[i]);
                has_pending_leaf = has_pending_leaf || (entry.sources[i] == b && entry.destinations[i] == product_b &&
                    entry.produced_identities[i] == b_id);
            }
        }
        check(aligned && has_pending_leaf,
            L"partial directory history retains only pending mappings with historical leaf and root identities aligned");
        OpsManager restored;
        check(restored.UndoFromJson(manager.UndoToJson()) && restored.CanUndo() &&
            same_mapping(remaining, MutationUndoTestProbe::History(restored)),
            L"partial directory history JSON preserves captured pending identities");
        const auto displaced = dir + L"\\historical-b.txt";
        FileIdentity replacement_id;
        if (replace_pending) {
            check(MoveFileExW(product_b.c_str(), displaced.c_str(), 0) && write(product_b, replacement_bytes),
                L"pending directory leaf replacement preserves historical leaf outside the retry root");
            replacement_id = identity(product_b);
        }
        inverses = MutationUndoTestProbe::TakeInverses(restored);
        check(!remaining.empty() && inverse_matches(inverses, remaining.front()),
            L"persisted partial directory inverse carries every pending historical identity");
        execute_inverses(restored, std::move(inverses));
        if (replace_pending) {
            check(restored.Status().phase == OpPhase::Failed && !exists(b) &&
                read(product_b) == replacement_bytes && identity(product_b) == replacement_id &&
                read(dir + L"\\historical-b.txt") == original_bytes && identity(dir + L"\\historical-b.txt") == b_id &&
                identity(a) == a_id && same_mapping(remaining, MutationUndoTestProbe::History(restored)),
                L"partial directory retry rejects replaced leaf without moving it indirectly through the pending root");
        } else {
            check(restored.Status().phase == OpPhase::Completed && !exists(product) &&
                read(a) == original_bytes && identity(a) == a_id && read(b) == original_bytes && identity(b) == b_id &&
                MutationUndoTestProbe::History(restored).empty(),
                L"partial directory retry restores only pending leaf and removes empty product root");
        }
    }

    {
        const auto dir = fixture_root + L"\\directory-cleanup-new-child";
        const auto original = dir + L"\\folder", target = dir + L"\\target", product = target + L"\\folder";
        const auto a = original + L"\\a.txt", b = original + L"\\b.txt";
        const auto product_b = product + L"\\b.txt", added = product + L"\\unrecorded.txt";
        check(mkdir(original) && mkdir(target) && write(a, original_bytes) && write(b, original_bytes),
            L"directory cleanup new-child fixture created");
        const auto a_id = identity(a), b_id = identity(b), root_id = identity(original);
        OpsManager manager;
        OpRequest request;
        request.type = OpType::Move; request.sources = {original}; request.dest_dir = target;
        MutationUndoTestProbe::Execute(manager, request);
        check(mkdir(original), L"directory cleanup new-child fixture requires a merge inverse");
        const HANDLE blocker = CreateFileW(product_b.c_str(), GENERIC_READ, FILE_SHARE_READ,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        check(blocker != INVALID_HANDLE_VALUE, L"directory cleanup has a genuinely pending locked leaf");
        auto inverses = MutationUndoTestProbe::TakeInverses(manager);
        execute_inverses(manager, std::move(inverses));
        check(identity(a) == a_id && !exists(b) && identity(product_b) == b_id && manager.CanUndo(),
            L"directory cleanup fixture first inverse restores one leaf and retains another");
        if (blocker != INVALID_HANDLE_VALUE) CloseHandle(blocker);
        check(write(added, replacement_bytes), L"unrecorded child introduced only after partial Undo captured its pending history");
        const auto added_id = identity(added);
        OpsManager restored;
        check(restored.UndoFromJson(manager.UndoToJson()) && restored.CanUndo(),
            L"pending leaf and cleanup-only root persist before new-child retry");
        inverses = MutationUndoTestProbe::TakeInverses(restored);
        execute_inverses(restored, std::move(inverses));
        const auto cleanup_history = MutationUndoTestProbe::History(restored);
        check(restored.Status().phase == OpPhase::Failed && identity(a) == a_id && identity(b) == b_id &&
            !exists(product_b) && read(added) == replacement_bytes && identity(added) == added_id &&
            !exists(original + L"\\unrecorded.txt"),
            L"pending leaf restores successfully while cleanup refuses nonempty root without scanning or moving new child");
        check(cleanup_history.size() == 1 && cleanup_history.front().sources == std::vector<std::wstring>{original} &&
            cleanup_history.front().destinations == std::vector<std::wstring>{product} &&
            cleanup_history.front().produced_identities == std::vector<FileIdentity>{root_id} &&
            cleanup_history.front().cleanup_directories == std::vector<std::wstring>{product} && restored.CanUndo(),
            L"partial cleanup consumes restored pending leaf but retains only identity-bound cleanup root");
        OpsManager cleanup_retry;
        check(cleanup_retry.UndoFromJson(restored.UndoToJson()) &&
            same_mapping(cleanup_history, MutationUndoTestProbe::History(cleanup_retry)),
            L"cleanup-only history JSON preserves its no-descendant-rescan marker");
        inverses = MutationUndoTestProbe::TakeInverses(cleanup_retry);
        execute_inverses(cleanup_retry, std::move(inverses));
        check(cleanup_retry.Status().phase == OpPhase::Failed && cleanup_retry.CanUndo() &&
            same_mapping(cleanup_history, MutationUndoTestProbe::History(cleanup_retry)) &&
            read(added) == replacement_bytes && identity(added) == added_id && !exists(original + L"\\unrecorded.txt"),
            L"repeating cleanup-only Undo refuses new child and retains retryable history without replaying successful leaves");
        const auto saved_original = dir + L"\\saved-original";
        check(MoveFileExW(original.c_str(), saved_original.c_str(), 0) != FALSE,
            L"cleanup-only original destination displaced within the isolated fixture");
        inverses = MutationUndoTestProbe::TakeInverses(cleanup_retry);
        execute_inverses(cleanup_retry, std::move(inverses));
        check(cleanup_retry.Status().phase == OpPhase::Failed &&
            same_mapping(cleanup_history, MutationUndoTestProbe::History(cleanup_retry)) &&
            read(added) == replacement_bytes && identity(added) == added_id && !exists(original + L"\\unrecorded.txt") &&
            identity(saved_original + L"\\a.txt") == a_id && identity(saved_original + L"\\b.txt") == b_id,
            L"cleanup-only root cannot use same-volume rename fast path to move new child when original destination disappears");
    }

    {
        wchar_t source_volume[MAX_PATH]{};
        DWORD source_serial = 0;
        const bool source_local = GetVolumePathNameW(fixture_root.c_str(), source_volume, MAX_PATH) &&
            GetDriveTypeW(source_volume) == DRIVE_FIXED &&
            GetVolumeInformationW(source_volume, nullptr, 0, &source_serial, nullptr, nullptr, nullptr, 0);
        std::vector<std::wstring> candidate_bases;
        wchar_t temp[32768]{};
        const DWORD temp_length = GetTempPathW(static_cast<DWORD>(std::size(temp)), temp);
        if (temp_length && temp_length < std::size(temp)) candidate_bases.emplace_back(temp, temp_length);
        wchar_t drives[1024]{};
        const DWORD drives_length = GetLogicalDriveStringsW(static_cast<DWORD>(std::size(drives)), drives);
        if (drives_length && drives_length < std::size(drives)) {
            for (const wchar_t* drive = drives; *drive; drive += wcslen(drive) + 1)
                candidate_bases.emplace_back(drive);
        }
        std::wstring cross_root;
        if (source_local) {
            for (const auto& base : candidate_bases) {
                wchar_t volume[MAX_PATH]{};
                DWORD serial = 0;
                if (!GetVolumePathNameW(base.c_str(), volume, MAX_PATH) || GetDriveTypeW(volume) != DRIVE_FIXED ||
                    !GetVolumeInformationW(volume, nullptr, 0, &serial, nullptr, nullptr, nullptr, 0) ||
                    serial == source_serial) continue;
                const auto candidate = (std::filesystem::path(base) /
                    (L"pulse-mutation-cross-volume-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                        std::to_wstring(GetTickCount64()))).wstring();
                std::error_code error;
                if (std::filesystem::create_directory(candidate, error) && !error) {
                    cross_root = candidate;
                    break;
                }
            }
        }
        if (cross_root.empty()) {
            wprintf(L"[SKIP] real cross-volume Move: no writable second local volume available for an isolated fixture\n");
        } else {
            const auto original = fixture_root + L"\\real-cross-volume.txt";
            const auto product = cross_root + L"\\real-cross-volume.txt";
            wprintf(L"[ENV] real cross-volume Move fixture: %s -> %s\n", original.c_str(), product.c_str());
            check(write(original, original_bytes), L"real cross-volume Move source fixture created");
            const auto source_id = identity(original);
            OpsManager manager;
            OpRequest request;
            request.type = OpType::Move; request.sources = {original}; request.dest_dir = cross_root;
            MutationUndoTestProbe::Execute(manager, request);
            const auto product_id = identity(product);
            const auto history = MutationUndoTestProbe::History(manager);
            check(manager.Status().phase == OpPhase::Completed && !exists(original) &&
                read(product) == original_bytes && source_id.Valid() && product_id.Valid() &&
                product_id.value.VolumeSerialNumber != source_id.value.VolumeSerialNumber && history.size() == 1 &&
                history.front().produced_identities == std::vector<FileIdentity>{product_id},
                L"real cross-volume Move commits and records the new destination volume's historical identity");
            OpsManager restored;
            check(restored.UndoFromJson(manager.UndoToJson()) && restored.CanUndo() &&
                same_mapping(history, MutationUndoTestProbe::History(restored)),
                L"real cross-volume Move identities survive JSON round-trip");
            auto inverses = MutationUndoTestProbe::TakeInverses(restored);
            check(!history.empty() && inverse_matches(inverses, history.front()),
                L"real cross-volume inverse carries actual target identity and original full path");
            execute_inverses(restored, std::move(inverses));
            check(restored.Status().phase == OpPhase::Completed && read(original) == original_bytes && !exists(product) &&
                MutationUndoTestProbe::History(restored).empty(),
                L"real cross-volume Undo restores original bytes and consumes completed history");
            std::error_code error;
            std::filesystem::remove_all(cross_root, error);
            check(!error && !exists(cross_root), L"isolated real cross-volume destination fixture cleaned");
        }
    }

    std::error_code error;
    std::filesystem::remove_all(fixture_root, error);
    check(!error && !exists(fixture_root), L"isolated mutation Undo fixtures cleaned");
    wprintf(L"== mutation Undo: %d passed, %d failed ==\n", passed, failed);
    return failed == 0 ? 0 : 1;
}

} // namespace pulse::ops
