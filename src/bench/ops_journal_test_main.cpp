#include "../ops/ops_manager.h"
#include "../ops/operation_journal_paths.h"
#include "../common/utf8_file.h"
#include <windows.h>
#include <cstdio>
#include <filesystem>
#include <set>

using namespace pulse;

namespace pulse::ops {
struct OperationJournalProbe {
    static void Persist(OpsManager& manager) { manager.PersistJournal(); }
    static void QueueCopy(OpsManager& manager, const std::wstring& source) {
        OpsManager::QueueItem item;
        item.seq = manager.next_seq_++;
        item.req.type = OpType::Copy;
        item.req.sources = {source};
        manager.queue_.push_back(std::move(item));
    }
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

int wmain() {
    const std::wstring root = TestDirectory();
    if (root.empty()) {
        std::fwprintf(stderr, L"[FAIL] unable to create operation-journal fixture directory\n");
        return 2;
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
          preserved_unknown.find(L"future_data") != std::wstring::npos,
          L"unrecognized recovery records remain in their source journal");

    const auto high_path = root + L"\\high-sequence.json";
    Check(WriteJournal(high_path, {Entry(UINT64_MAX - 1, L"C:\\\\high.txt")}),
          L"create a recovery journal with a near-overflow sequence");
    ops::OpsManager high_sequence;
    high_sequence.SetJournalPath(high_path);
    const auto high_recovery = high_sequence.PendingRecovery();
    Check(high_recovery.entries.size() == 1 && high_recovery.entries[0].sequence < UINT64_MAX / 2,
          L"near-overflow recovery sequence is remapped without losing the record");

    Cleanup(root);
    std::wprintf(L"\n== operation journal test: %d failed ==\n", failures);
    return failures == 0 ? 0 : 1;
}
