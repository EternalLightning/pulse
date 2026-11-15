// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "../ops/ops_manager.h"
#include "../ops/operation_journal_paths.h"
#include "../common/json_utils.h"
#include "../common/utf8_file.h"
#include <windows.h>
#include <cstdio>
#include <filesystem>
#include <set>
#include <string_view>

using namespace pulse;

int RunJsonUndoParserRegressions();

namespace pulse::ops {
struct OperationJournalProbe {
    static bool HasJournal(const OpsManager& manager) { return bool(manager.journal_); }
    static void Persist(OpsManager& manager) { manager.PersistJournal(); }
    static std::wstring Serialize(const OpsManager& manager, const std::wstring& path) {
        return manager.JournalJsonLocked(path);
    }
    static void ActivateNext(OpsManager& manager, bool elevated) {
        manager.active_item_ = std::move(manager.queue_.front());
        manager.queue_.pop_front();
        manager.active_item_->elevated_retry = elevated;
    }
    static void QueueCopy(OpsManager& manager, const std::wstring& source) {
        OpsManager::QueueItem item;
        item.seq = manager.next_seq_++;
        item.req.type = OpType::Copy;
        item.req.sources = {source};
        manager.queue_.push_back(std::move(item));
    }
    static size_t Queued(const OpsManager& manager) { return manager.queue_.size(); }
};
}

namespace {
int failures = 0;

void Check(bool ok, const wchar_t* message) {
    std::wprintf(L"[%s] %s\n", ok ? L"PASS" : L"FAIL", message);
    if (!ok) ++failures;
}

std::wstring TestDirectory() {
    wchar_t root[MAX_PATH]{};
    DWORD length = GetEnvironmentVariableW(L"PULSE_TEST_FIXTURE_ROOT", root, ARRAYSIZE(root));
    if (!length || length >= ARRAYSIZE(root)) {
        length = GetTempPathW(ARRAYSIZE(root), root);
        if (!length || length >= ARRAYSIZE(root)) return {};
    }
    static std::atomic<uint64_t> sequence{0};
    const std::wstring base = (std::filesystem::path(root) / (L"pulse-ops-journal-" +
        std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()) + L"-" +
        std::to_wstring(sequence.fetch_add(1)))).wstring();
    std::error_code ec;
    std::filesystem::create_directories(base, ec);
    if (ec) std::fwprintf(stderr, L"[FAIL] create fixture %s: error=%d\n", base.c_str(), ec.value());
    return ec ? std::wstring{} : base;
}

std::wstring Entry(uint64_t sequence, const wchar_t* source) {
    return L"{\"seq\":\"" + std::to_wstring(sequence) +
        L"\",\"active\":false,\"outcome_unknown\":false,\"type\":0,\"policy\":0,"
        L"\"undo\":false,\"sources\":[\"" + source + L"\"],\"dest\":\"C:\\\\dest\"}";
}

bool WriteJournal(const std::wstring& path, const std::vector<std::wstring>& entries) {
    std::wstring text = L"{\"schema\":1,\"entries\":[";
    for (size_t i = 0; i < entries.size(); ++i) {
        if (i) text += L",";
        text += entries[i];
    }
    text += L"]}";
    return WriteUtf8FileAtomic(path, text);
}

bool Exists(const std::wstring& path) {
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

void Cleanup(const std::wstring& root) {
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc == 2 && std::wstring_view(argv[1]) == L"--json-parser-only")
        return RunJsonUndoParserRegressions();
    const std::wstring root = TestDirectory();
    if (root.empty()) {
        std::fwprintf(stderr, L"[FAIL] unable to create operation-journal fixture directory\n");
        return 2;
    }

    ops::OpsManager shell_worker;
    ops::OperationJournalProbe::QueueCopy(shell_worker, root + L"\\worker-source.txt");
    ops::OperationJournalProbe::Persist(shell_worker);
    shell_worker.DiscardRecovery();
    Check(!ops::OperationJournalProbe::HasJournal(shell_worker) &&
          shell_worker.PendingRecovery().entries.empty(),
          L"unconfigured Shell workers persist and discard without allocating recovery journals");

    {
        const auto primary = root + L"\\canonical-primary.json", secondary = root + L"\\canonical-secondary.json";
        ops::OpsManager canonical;
        canonical.SetJournalPaths({L"", primary, primary, secondary});
        ops::OperationJournalProbe::QueueCopy(canonical, L"C:\\fixture\\canonical-source.txt");
        const auto serialized = ops::OperationJournalProbe::Serialize(canonical, primary);
        Check(!serialized.empty() && ops::OperationJournalProbe::Serialize(canonical, secondary).empty(),
              L"the first nonempty unique journal path owns new requests");
        canonical.SetJournalPath(primary);
        Check(ops::OperationJournalProbe::Serialize(canonical, primary) == serialized,
              L"single-path and path-list configuration share the same primary journal rule");
        canonical.SetJournalPath(L"");
        ops::OperationJournalProbe::Persist(canonical);
        Check(ops::OperationJournalProbe::Serialize(canonical, primary).empty() &&
              !std::filesystem::exists(primary) && !std::filesystem::exists(secondary),
              L"empty journal configuration cannot revive a previously configured path");
    }

    const auto allocated_directory = root + L"\\allocated";
    std::filesystem::create_directory(allocated_directory);
    const auto allocated_first = ops::AssignOperationJournalPaths(allocated_directory, false);
    const auto allocated_second = ops::AssignOperationJournalPaths(allocated_directory, false);
    Check(!allocated_first.window_path.empty() && !allocated_second.window_path.empty() &&
          allocated_first.window_path != allocated_second.window_path,
          L"new windows reserve unique journal paths");
    ops::OpsManager fresh_first;
    ops::OpsManager fresh_second;
    fresh_first.SetJournalPath(allocated_first.window_path);
    fresh_second.SetJournalPath(allocated_second.window_path);
    Check(ops::OperationJournalProbe::HasJournal(fresh_first) &&
          ops::OperationJournalProbe::HasJournal(fresh_second),
          L"application windows allocate recovery state when their journal is configured");
    ops::OperationJournalProbe::QueueCopy(fresh_first, root + L"\\pending-source.txt");
    ops::OperationJournalProbe::Persist(fresh_first);
    ops::OperationJournalProbe::Persist(fresh_second);
    ops::OpsManager fresh_reopened;
    fresh_reopened.SetJournalPath(allocated_first.window_path);
    Check(fresh_reopened.PendingRecovery().entries.size() == 1 &&
          !Exists(allocated_second.window_path),
          L"a newly reserved journal persists pending work while another empty window closes");

    const std::wstring first_path = root + L"\\operations-window-a.json";
    const std::wstring second_path = root + L"\\operations-window-b.json";
    Check(WriteJournal(first_path, {Entry(1, L"C:\\\\first.txt")}) &&
          WriteJournal(second_path, {Entry(1, L"C:\\\\second.txt")}),
          L"create independent manager journals with colliding local sequences");

    ops::OpsManager first;
    ops::OpsManager second;
    first.SetJournalPath(first_path);
    second.SetJournalPath(second_path);
    Check(first.PendingRecovery().entries.size() == 1 && second.PendingRecovery().entries.size() == 1,
          L"each manager loads only its own recovery journal");
    first.DiscardRecovery();
    ops::OperationJournalProbe::Persist(second);
    Check(!Exists(first_path) && Exists(second_path),
          L"discarding one window journal leaves the other manager's file intact");
    ops::OpsManager reopened_second;
    reopened_second.SetJournalPath(second_path);
    Check(reopened_second.PendingRecovery().entries.size() == 1,
          L"closing another manager preserves its pending recovery for restart");

    const std::wstring legacy_path = root + L"\\operations.json";
    const std::wstring third_path = root + L"\\operations-window-c.json";
    const std::wstring unknown =
        L"{\"seq\":\"9\",\"active\":false,\"type\":99,\"future_data\":\"keep\"}";
    Check(WriteJournal(legacy_path, {Entry(7, L"C:\\\\legacy.txt")}) &&
          WriteJournal(third_path, {Entry(7, L"C:\\\\third.txt"), unknown}),
          L"create legacy and orphaned per-window recovery journals");

    const auto assignment = ops::AssignOperationJournalPaths(root, true);
    Check(assignment.window_path == legacy_path && assignment.paths.size() == 3,
          L"recovery owner receives the legacy journal and every orphaned window journal");
    ops::OpsManager owner;
    owner.SetJournalPaths(assignment.paths);
    const auto recovered = owner.PendingRecovery();
    std::set<uint64_t> sequences;
    for (const auto& entry : recovered.entries) sequences.insert(entry.sequence);
    Check(recovered.entries.size() == 3 && sequences.size() == 3,
          L"aggregated recovery retains all recognized entries and remaps duplicate sequence ids");
    ops::OperationJournalProbe::Persist(owner);
    std::wstring preserved_unknown;
    Check(ReadUtf8File(third_path, preserved_unknown) &&
          preserved_unknown.find(unknown) != std::wstring::npos,
          L"unrecognized recovery records remain in their source journal");

    const auto high_path = root + L"\\high-sequence.json";
    Check(WriteJournal(high_path, {Entry(UINT64_MAX - 1, L"C:\\\\high.txt")}),
          L"create a recovery journal with a near-overflow sequence");
    ops::OpsManager high_sequence;
    high_sequence.SetJournalPath(high_path);
    const auto high_recovery = high_sequence.PendingRecovery();
    Check(high_recovery.entries.size() == 1 && high_recovery.entries[0].sequence < UINT64_MAX / 2,
          L"near-overflow recovery sequence is remapped without losing the record");

    const auto deletion_path = root + L"\\deletion.json";
    const std::wstring deletion = LR"({"seq":"5","active":true,"type":3,"policy":0,"sources":["C:\\fixture\\\u6C49\uD83D\uDE00"],"delete_targets":[{"path":"C:\\fixture\\\u6C49\uD83D\uDE00","roots":["C:\\fixture\\physical"],"recycle_item":true}]})";
    Check(WriteJournal(deletion_path, {deletion}), L"write isolated active deletion recovery record");
    ops::OpsManager deletion_recovery;
    deletion_recovery.SetJournalPath(deletion_path);
    const auto deletions = deletion_recovery.PendingRecovery();
    Check(deletions.entries.size() == 1 && deletions.entries[0].request.sources ==
          std::vector<std::wstring>{L"C:\\fixture\\汉\U0001F600"} &&
          deletions.entries[0].request.delete_targets.size() == 1 &&
          deletions.entries[0].request.delete_targets[0].path == L"C:\\fixture\\汉\U0001F600" &&
          deletions.entries[0].request.delete_targets[0].physical_paths ==
          std::vector<std::wstring>{L"C:\\fixture\\physical"} &&
          deletions.entries[0].request.delete_targets[0].recycle_item,
          L"recovery reads nested deletion targets and Unicode directly from parsed values");
    Check(!deletion_recovery.RetryRecovery(),
          L"active deletion recovery still refuses automatic replay");

    for (const auto type : {ops::OpType::Rename, ops::OpType::BatchRename, ops::OpType::Copy, ops::OpType::Move}) {
        for (const bool active : {false, true}) {
            const auto path = root + L"\\replay-" + std::to_wstring(static_cast<int>(type)) +
                (active ? L"-active.json" : L"-queued.json");
            const bool rename = type == ops::OpType::Rename || type == ops::OpType::BatchRename;
            const std::wstring entry = L"{\"seq\":\"1\",\"active\":" + std::wstring(active ? L"true" : L"false") +
                L",\"outcome_unknown\":false,\"type\":" + std::to_wstring(static_cast<int>(type)) +
                L",\"policy\":0,\"sources\":[\"C:\\\\fixture\\\\original.txt\"],\"name\":\"renamed.txt\","
                L"\"names\":[\"renamed.txt\"],\"dest\":\"" + std::wstring(rename ? L"" : L"C:\\\\fixture\\\\destination") + L"\"}";
            Check(WriteJournal(path, {entry}), L"write isolated active or queued mutation recovery record");
            ops::OpsManager recovery;
            recovery.SetJournalPath(path);
            const bool retried = recovery.RetryRecovery();
            Check(recovery.PendingRecovery().entries.size() == 1 && retried == !active &&
                ops::OperationJournalProbe::Queued(recovery) == (active ? 0u : 1u),
                rename ? L"only a rename that never started can be queued without historical identity proof" :
                    L"queued copy or move needs no temporary-file reconciliation; active transfer remains blocked");
        }
    }
    {
        const auto path = root + L"\\unknown-queued.json";
        auto unknown_entry = Entry(1, L"C:\\\\fixture\\\\unknown.txt");
        const auto unknown_flag = unknown_entry.find(L"\"outcome_unknown\":false");
        unknown_entry.replace(unknown_flag, 23, L"\"outcome_unknown\":true");
        Check(WriteJournal(path, {unknown_entry}), L"write queued operation with unknown outcome");
        ops::OpsManager recovery;
        recovery.SetJournalPath(path);
        Check(!recovery.RetryRecovery() && ops::OperationJournalProbe::Queued(recovery) == 0,
            L"unknown outcome still forbids replay even when the active flag is clear");
    }

    const auto corrupt_path = root + L"\\corrupt.json";
    const std::wstring corrupt = LR"({"schema":1,"entries":[{"seq":"6","type":0,"sources":["\uD800"]}]})";
    Check(WriteUtf8FileAtomic(corrupt_path, corrupt), L"write isolated damaged recovery journal");
    ops::OpsManager corrupt_recovery;
    corrupt_recovery.SetJournalPath(corrupt_path);
    ops::OperationJournalProbe::Persist(corrupt_recovery);
    std::wstring retained;
    Check(corrupt_recovery.PendingRecovery().entries.empty() && ReadUtf8File(corrupt_path, retained) &&
          retained == corrupt,
          L"invalid recovery input is preserved byte-for-byte instead of overwritten or replayed");

    const auto serialized_path = root + L"\\serialized.json";
    ops::OpsManager serialized;
    serialized.SetJournalPath(serialized_path);
    Check(ops::OperationJournalProbe::Serialize(serialized, serialized_path).empty(),
          L"an empty journal produces no serialized payload");
    ops::OperationJournalProbe::QueueCopy(serialized, L"C:\\fixture\\quoted\"汉\U0001F600\n");
    ops::OperationJournalProbe::ActivateNext(serialized, true);
    const auto active_json = ops::OperationJournalProbe::Serialize(serialized, serialized_path);
    const json::Document active_document(active_json);
    const auto* active_entries = active_document.Find(L"entries");
    Check(active_entries && active_entries->children.size() == 1 &&
          json::Bool(active_entries->children[0].Find(L"active")) &&
          json::Bool(active_entries->children[0].Find(L"outcome_unknown")) &&
          json::Strings(active_entries->children[0].Find(L"sources")) ==
          std::vector<std::wstring>{L"C:\\fixture\\quoted\"汉\U0001F600\n"},
          L"direct serialization preserves escapes and marks an active elevated request as uncertain");

    const auto opaque_path = root + L"\\opaque-only.json";
    Check(WriteJournal(opaque_path, {unknown}), L"write an isolated journal containing only an unknown record");
    ops::OpsManager opaque_only;
    opaque_only.SetJournalPath(opaque_path);
    ops::OperationJournalProbe::Persist(opaque_only);
    Check(ReadUtf8File(opaque_path, retained) && retained.find(unknown) != std::wstring::npos,
          L"an unknown-only journal remains nonempty and is never deleted as an empty journal");

    failures += RunJsonUndoParserRegressions();
    Cleanup(root);
    std::wprintf(L"\n== operation journal test: %d failed ==\n", failures);
    return failures == 0 ? 0 : 1;
}
