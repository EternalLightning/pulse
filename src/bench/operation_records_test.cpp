#include "../ops/ops_manager.h"
#include <algorithm>
#include <cstdio>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <stdexcept>

namespace pulse::ops {
struct OperationRecordsTestProbe {
    static void Initialize(OpsManager& manager) { manager.running_ = true; }

    static void ExecuteNext(OpsManager& manager) {
        auto item = std::move(manager.queue_.front());
        manager.queue_.pop_front();
        manager.active_item_ = item;
        manager.ExecuteFileOperation(item);
        manager.active_item_.reset();
    }

    static uint64_t BeginNext(OpsManager& manager) {
        auto item = std::move(manager.queue_.front());
        manager.queue_.pop_front();
        manager.active_item_ = item;
        manager.transfer_active_.store(item.req.type == OpType::Copy || item.req.type == OpType::Move);
        manager.transfer_cancel_.store(false);
        manager.transfer_pause_.store(false);
        manager.SetStatus([&](OpStatus& status) {
            status.task_id = item.seq; status.type = item.req.type;
            status.active = true; status.phase = OpPhase::Scanning;
            status.last_error.clear(); status.failure_hr = S_OK;
        });
        return item.seq;
    }

    static void Finish(OpsManager& manager, uint64_t id, OpPhase phase) {
        manager.SetStatus([&](OpStatus& status) {
            status.task_id = id; status.active = false; status.phase = phase;
            ++status.completed_ops;
        });
        manager.active_item_.reset();
        manager.transfer_active_.store(false);
    }

    static bool Paused(const OpsManager& manager) { return manager.transfer_pause_.load(); }
    static bool Cancelled(const OpsManager& manager) { return manager.transfer_cancel_.load(); }
    static void Republish(OpsManager& manager) {
        manager.SetStatus([](OpStatus& status) { status.elevation_retry_available = false; });
    }
};

struct TransferBatchTestProbe {
    static inline unsigned copy_calls = 0;
    static inline std::wstring last_copy_source;
    static HRESULT WINAPI FailSecondCopy(PCWSTR source, PCWSTR destination, COPYFILE2_EXTENDED_PARAMETERS* parameters) {
        if (last_copy_source != source) { last_copy_source = source; ++copy_calls; }
        if (copy_calls == 2) return HRESULT_FROM_WIN32(ERROR_WRITE_FAULT);
        return CopyFile2(source, destination, parameters);
    }
    static void FailSecond(OpsManager& manager) {
        copy_calls = 0; last_copy_source.clear();
        manager.transfer_copy_file_ = FailSecondCopy;
    }
    static void RestoreCopy(OpsManager& manager) { manager.transfer_copy_file_ = CopyFile2; }
    static std::vector<std::wstring> UndoDestinations(const OpsManager& manager) {
        return manager.undo_.empty() ? std::vector<std::wstring>{} : manager.undo_.back().destinations;
    }
    static void ForceCopyMove(OpsManager& manager, std::function<void(const std::wstring&)> cleanup) {
        manager.force_copy_move_ = true;
        manager.before_source_cleanup_ = std::move(cleanup);
    }
    static void Run(OpsManager& manager, const OpRequest& request, uint64_t id,
        std::function<void()> notify = {}) {
        manager.running_ = true;
        manager.notify_ = std::move(notify);
        OpsManager::QueueItem item;
        item.seq = id; item.req = request;
        manager.active_item_ = item;
        manager.RunTransfer(request, id);
        manager.active_item_.reset();
        manager.notify_ = {};
    }
};
}

namespace {
int failures = 0;
void Check(bool ok, const char* message) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", message);
    if (!ok) ++failures;
}

const pulse::ops::OpStatus* Find(const std::vector<pulse::ops::OpStatus>& tasks, uint64_t id) {
    const auto found = std::find_if(tasks.begin(), tasks.end(), [id](const auto& task) { return task.task_id == id; });
    return found == tasks.end() ? nullptr : &*found;
}

using Path = std::filesystem::path;
struct TransferFixture {
    Path root = std::filesystem::current_path() / L"bench_data" /
        (L"operation-records-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    TransferFixture() { std::filesystem::create_directories(root); }
    ~TransferFixture() { std::error_code error; std::filesystem::remove_all(root, error); }
    Path Directory(const wchar_t* name) {
        const auto path = root / name;
        std::filesystem::create_directories(path);
        return path;
    }
};
void Write(const Path& path, const std::string& text) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file << text;
    if (!file) throw std::runtime_error("fixture write failed");
}
std::string Read(const Path& path) {
    std::ifstream file(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}
bool RenameFixtureObject(const Path& source, const Path& destination) {
    if (MoveFileW(source.c_str(), destination.c_str())) return true;
    HANDLE handle = CreateFileW(source.c_str(), DELETE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false;
    const auto name = destination.wstring();
    std::vector<unsigned char> buffer(offsetof(FILE_RENAME_INFO, FileName) + name.size() * sizeof(wchar_t));
    auto* rename = reinterpret_cast<FILE_RENAME_INFO*>(buffer.data());
    rename->FileNameLength = static_cast<DWORD>(name.size() * sizeof(wchar_t));
    std::memcpy(rename->FileName, name.data(), rename->FileNameLength);
    const bool renamed = SetFileInformationByHandle(handle, FileRenameInfo, rename, static_cast<DWORD>(buffer.size())) != FALSE;
    const DWORD error = renamed ? ERROR_SUCCESS : GetLastError();
    CloseHandle(handle);
    SetLastError(error);
    return renamed;
}
pulse::ops::OpRequest Request(const Path& destination, const std::vector<Path>& sources,
    pulse::ops::OpType type = pulse::ops::OpType::Copy) {
    pulse::ops::OpRequest request;
    request.type = type; request.dest_dir = destination.wstring();
    for (const auto& source : sources) request.sources.push_back(source.wstring());
    return request;
}
const pulse::ops::TransferItemResult* Item(const pulse::ops::TransferOutcome& outcome, const Path& source) {
    const auto found = std::find_if(outcome.items.begin(), outcome.items.end(), [&](const auto& item) {
        return item.source == source.wstring();
    });
    return found == outcome.items.end() ? nullptr : &*found;
}

void BatchAndOutcomeFixtures() {
    using namespace pulse::ops;
    using Probe = TransferBatchTestProbe;
    TransferFixture fixture;
    const auto source = fixture.Directory(L"mixed-source");
    const auto destination = fixture.Directory(L"mixed-destination");
    const std::vector<Path> sources{source / L"replace.txt", source / L"skip.txt", source / L"both.txt"};
    for (const auto& file : sources) { Write(file, "new-" + file.filename().string()); Write(destination / file.filename(), "old"); }
    {
        OpsManager manager;
        unsigned dialogs = 0;
        uint64_t accepted_token = 0;
        std::vector<ConflictResolution> accepted_rows;
        bool metadata_ok = false, rejected_stale = false, rejected_duplicate = false, rejected_missing = false, accepted = false;
        Probe::Run(manager, Request(destination, sources), 2001, [&] {
            const auto pending = manager.PendingConflict();
            if (!pending || accepted_token == pending->token) return;
            accepted_token = pending->token; ++dialogs;
            std::set<uint64_t> ids;
            metadata_ok = pending->rows.size() == 3;
            for (const auto& row : pending->rows) {
                metadata_ok = metadata_ok && ids.insert(row.row_id).second && row.source_size > 3 &&
                    row.destination_size == 3 && !row.source_is_directory && !row.destination_is_directory;
                const auto name = Path(row.source).filename();
                accepted_rows.push_back({row.row_id, name == L"replace.txt" ? ConflictChoice::Replace :
                    name == L"skip.txt" ? ConflictChoice::Skip : ConflictChoice::KeepBoth});
            }
            rejected_stale = !manager.ResolveConflicts(pending->token + 99, accepted_rows);
            auto duplicate = accepted_rows;
            duplicate.back().row_id = duplicate.front().row_id;
            rejected_duplicate = !manager.ResolveConflicts(pending->token, duplicate);
            auto missing = accepted_rows; missing.pop_back();
            rejected_missing = !manager.ResolveConflicts(pending->token, missing);
            accepted = manager.ResolveConflicts(pending->token, accepted_rows);
        });
        const auto outcomes = manager.DrainTransferOutcomes();
        Check(dialogs == 1 && metadata_ok && accepted && rejected_stale && rejected_duplicate && rejected_missing &&
            !manager.ResolveConflicts(accepted_token, accepted_rows), "batch token and immutable rows reject stale, duplicate and incomplete answers");
        const auto* both = outcomes.size() == 1 ? Item(outcomes.front(), sources[2]) : nullptr;
        Check(manager.Status().phase == OpPhase::Completed && Read(destination / L"replace.txt") == "new-replace.txt" &&
            Read(destination / L"skip.txt") == "old" && Read(destination / L"both.txt") == "old" && both &&
            both->destination != (destination / L"both.txt").wstring() && Read(Path(both->destination)) == "new-both.txt",
            "one real batch executes mixed Replace, Skip and KeepBoth without overwriting skipped or kept originals");
        Check(outcomes.size() == 1 && outcomes.front().task_id == 2001 && outcomes.front().phase == OpPhase::Completed &&
            outcomes.front().can_restage && outcomes.front().remaining_sources == std::vector<std::wstring>{sources[1].wstring()} &&
            Item(outcomes.front(), sources[1])->state == TransferItemState::Skipped &&
            Item(outcomes.front(), sources[0])->destination_committed,
            "outcome reports skipped source exactly while successful products remain committed");
        Check(manager.DrainTransferOutcomes().empty(), "transfer outcomes drain exactly once");
    }
    {
        const auto cancel_destination = fixture.Directory(L"cancel-destination");
        for (const auto& file : sources) Write(cancel_destination / file.filename(), "untouched");
        OpsManager manager;
        uint64_t handled = 0;
        bool refused_after_cancel = false;
        Probe::Run(manager, Request(cancel_destination, sources), 2002, [&] {
            const auto pending = manager.PendingConflict();
            if (!pending || handled == pending->token) return;
            handled = pending->token;
            manager.CancelCurrent();
            std::vector<ConflictResolution> rows;
            for (const auto& row : pending->rows) rows.push_back({row.row_id, ConflictChoice::Replace});
            refused_after_cancel = !manager.ResolveConflicts(pending->token, rows);
        });
        const auto outcomes = manager.DrainTransferOutcomes();
        Check(refused_after_cancel && outcomes.size() == 1 && outcomes.front().phase == OpPhase::Cancelled &&
            outcomes.front().remaining_sources.size() == 3 && Read(cancel_destination / L"replace.txt") == "untouched" &&
            Read(cancel_destination / L"skip.txt") == "untouched" && Read(cancel_destination / L"both.txt") == "untouched",
            "cancelled conflict token cannot authorize writes and preserves exact pending sources");
    }
    for (const bool replace_identity : {false, true}) {
        const auto changed_destination = fixture.Directory(replace_identity ? L"identity-changed" : L"metadata-changed");
        const auto displaced_destination = fixture.Directory(L"external-destination-objects") / L"previous-object.txt";
        Write(changed_destination / L"replace.txt", "old");
        OpsManager manager;
        uint64_t handled = 0;
        bool mutation_ok = true;
        DWORD mutation_error = ERROR_SUCCESS;
        Probe::Run(manager, Request(changed_destination, {sources[0]}), replace_identity ? 2004 : 2003, [&] {
            const auto pending = manager.PendingConflict();
            if (!pending || handled == pending->token) return;
            handled = pending->token;
            if (replace_identity) mutation_ok = RenameFixtureObject(changed_destination / L"replace.txt",
                displaced_destination);
            if (!mutation_ok) mutation_error = GetLastError();
            Write(changed_destination / L"replace.txt", "changed-during-dialog");
            manager.ResolveConflicts(pending->token, {{pending->rows.front().row_id, ConflictChoice::Replace}});
        });
        const auto outcomes = manager.DrainTransferOutcomes();
        if (!mutation_ok || manager.Status().phase != OpPhase::Failed || outcomes.empty() ||
            outcomes.front().failure_hr != HRESULT_FROM_WIN32(ERROR_FILE_INVALID))
            std::printf("conflict mutation diagnostics: renamed=%d error=%lu phase=%d hr=%08lx\n",
                mutation_ok, mutation_error, static_cast<int>(manager.Status().phase),
                outcomes.empty() ? 0ul : static_cast<unsigned long>(outcomes.front().failure_hr));
        Check(mutation_ok && manager.Status().phase == OpPhase::Failed && outcomes.size() == 1 &&
            outcomes.front().failure_hr == HRESULT_FROM_WIN32(ERROR_FILE_INVALID) && !outcomes.front().can_restage &&
            Read(changed_destination / L"replace.txt") == "changed-during-dialog",
            replace_identity ? "destination identity replacement during batch confirmation fails closed" :
            "destination content and metadata changes during batch confirmation fail closed");
    }
    {
        const auto legacy_destination = fixture.Directory(L"legacy-apply-all");
        for (const auto& file : sources) Write(legacy_destination / file.filename(), "legacy-original");
        OpsManager manager;
        unsigned dialogs = 0;
        Probe::Run(manager, Request(legacy_destination, sources), 2010, [&] {
            const auto pending = manager.PendingConflict();
            if (!pending) return;
            ++dialogs;
            manager.ResolveConflict(pending->token, ConflictChoice::Skip, true);
        });
        const auto outcomes = manager.DrainTransferOutcomes();
        Check(dialogs == 1 && outcomes.size() == 1 && outcomes.front().phase == OpPhase::Completed &&
            outcomes.front().remaining_sources.size() == 3 && Read(legacy_destination / L"replace.txt") == "legacy-original" &&
            Read(legacy_destination / L"both.txt") == "legacy-original",
            "legacy single-conflict apply-to-all still skips all remaining conflicts");
    }
    {
        const auto nested_source = fixture.Directory(L"parent-source") / L"tree";
        std::filesystem::create_directories(nested_source);
        Write(nested_source / L"child.txt", "nested-content");
        const auto nested_destination = fixture.Directory(L"parent-destination");
        Write(nested_destination / L"tree", "existing-file");
        OpsManager manager;
        unsigned dialogs = 0;
        Probe::Run(manager, Request(nested_destination, {nested_source}), 2005, [&] {
            const auto pending = manager.PendingConflict();
            if (!pending) return;
            ++dialogs;
            std::vector<ConflictResolution> rows;
            for (const auto& row : pending->rows) rows.push_back({row.row_id, ConflictChoice::KeepBoth});
            manager.ResolveConflicts(pending->token, rows);
        });
        const auto outcomes = manager.DrainTransferOutcomes();
        const auto* child = outcomes.size() == 1 ? Item(outcomes.front(), nested_source / L"child.txt") : nullptr;
        Check(dialogs == 1 && manager.Status().phase == OpPhase::Completed && child &&
            Path(child->destination).parent_path() != nested_destination / L"tree" &&
            Read(Path(child->destination)) == "nested-content" && Read(nested_destination / L"tree") == "existing-file",
            "directory KeepBoth remaps descendant destinations and leaves the original parent object intact");
    }
    {
        const auto partial_source = fixture.Directory(L"partial-source") / L"tree";
        std::filesystem::create_directories(partial_source);
        Write(partial_source / L"a.txt", "first"); Write(partial_source / L"b.txt", "second"); Write(partial_source / L"c.txt", "third");
        const auto partial_destination = fixture.Directory(L"partial-destination");
        OpsManager manager;
        Probe::FailSecond(manager);
        Probe::Run(manager, Request(partial_destination, {partial_source}), 2006);
        Write(partial_source / L"new-after-task.txt", "must-not-enter-remainder");
        const auto outcomes = manager.DrainTransferOutcomes();
        bool exact = outcomes.size() == 1 && outcomes.front().phase == OpPhase::Failed && outcomes.front().plan_complete &&
            !outcomes.front().can_restage && outcomes.front().can_resume && outcomes.front().remaining_sources.size() == 2;
        unsigned completed = 0;
        if (!outcomes.empty()) for (const auto& item : outcomes.front().items) {
            if (item.directory) continue;
            const bool remaining = std::find(outcomes.front().remaining_sources.begin(), outcomes.front().remaining_sources.end(), item.source) !=
                outcomes.front().remaining_sources.end();
            if (item.state == TransferItemState::Completed) {
                ++completed; exact = exact && !remaining && item.destination_committed && Read(Path(item.destination)) == Read(Path(item.source));
            } else exact = exact && remaining;
        }
        if (!outcomes.empty()) for (const auto& remaining : outcomes.front().remaining_sources)
            exact = exact && Path(remaining).parent_path() == partial_source && Path(remaining).filename() != L"new-after-task.txt";
        Check(exact && completed == 1, "partial directory failure returns only exact unfinished planned leaves without rescanning the root");
        Write(partial_destination / L"tree" / L"external-after-task.txt", "must-not-be-in-undo");
        Probe::RestoreCopy(manager);
        const bool was_available = manager.CanResumeTransfer(2006);
        const auto resumed_id = manager.ResumeIncompleteTransfer(2006);
        Check(was_available && resumed_id && !manager.CanResumeTransfer(2006) &&
            !manager.ResumeIncompleteTransfer(2006), "ID-bound frozen recovery is submitted once with a new task ID");
        if (resumed_id) OperationRecordsTestProbe::ExecuteNext(manager);
        const auto resumed = manager.DrainTransferOutcomes();
        const auto undo_destinations = Probe::UndoDestinations(manager);
        Check(resumed.size() == 1 && resumed.front().task_id == resumed_id && resumed.front().phase == OpPhase::Completed &&
            resumed.front().remaining_sources.empty() && !std::filesystem::exists(partial_destination / L"a.txt") &&
            Read(partial_destination / L"tree" / L"a.txt") == "first" &&
            Read(partial_destination / L"tree" / L"b.txt") == "second" &&
            Read(partial_destination / L"tree" / L"c.txt") == "third" &&
            !std::filesystem::exists(partial_destination / L"tree" / L"new-after-task.txt") &&
            Read(partial_destination / L"tree" / L"external-after-task.txt") == "must-not-be-in-undo" &&
            undo_destinations.size() == 2 && std::none_of(undo_destinations.begin(), undo_destinations.end(), [&](const auto& path) {
                return path == (partial_destination / L"tree").wstring();
            }), "frozen recovery preserves relative destinations, ignores new source children and limits undo to actual resumed products");
    }
    {
        const auto move_source = fixture.Directory(L"move-source") / L"whole-tree";
        std::filesystem::create_directories(move_source);
        Write(move_source / L"child.txt", "moved-whole");
        const auto move_destination = fixture.Directory(L"move-destination");
        OpsManager manager;
        Probe::Run(manager, Request(move_destination, {move_source}, OpType::Move), 2007);
        const auto outcomes = manager.DrainTransferOutcomes();
        const auto* moved = outcomes.size() == 1 ? Item(outcomes.front(), move_source) : nullptr;
        Check(outcomes.size() == 1 && outcomes.front().phase == OpPhase::Completed && outcomes.front().remaining_sources.empty() &&
            moved && moved->state == TransferItemState::Completed && moved->destination_committed && moved->source_removed &&
            !std::filesystem::exists(move_source) && Read(move_destination / L"whole-tree" / L"child.txt") == "moved-whole",
            "whole-directory move records successful atomic rename and has no unfinished descendants");
    }
    for (const bool change_source : {true, false}) {
        const auto changed_source = fixture.Directory(change_source ? L"resume-source-change" : L"resume-target-source") / L"tree";
        std::filesystem::create_directories(changed_source);
        Write(changed_source / L"a.txt", "A"); Write(changed_source / L"b.txt", "B"); Write(changed_source / L"c.txt", "C");
        const auto changed_destination = fixture.Directory(change_source ? L"resume-source-destination" : L"resume-target-change");
        OpsManager manager;
        Probe::FailSecond(manager);
        const uint64_t failed_id = change_source ? 2011 : 2012;
        Probe::Run(manager, Request(changed_destination, {changed_source}), failed_id);
        const auto failed = manager.DrainTransferOutcomes();
        bool prepared = failed.size() == 1 && failed.front().can_resume && !failed.front().remaining_sources.empty();
        Path altered;
        std::string preserved;
        if (prepared) {
            const auto* pending = Item(failed.front(), Path(failed.front().remaining_sources.front()));
            prepared = pending != nullptr;
            if (pending) {
                altered = change_source ? Path(pending->source) : Path(pending->destination);
                if (change_source) {
                    WIN32_FILE_ATTRIBUTE_DATA original{};
                    prepared = GetFileAttributesExW(altered.c_str(), GetFileExInfoStandard, &original) != FALSE;
                    preserved = Read(altered);
                    prepared = MoveFileW(altered.c_str(), (altered.wstring() + L".original").c_str()) != FALSE && prepared;
                    Write(altered, preserved);
                    HANDLE handle = CreateFileW(altered.c_str(), FILE_WRITE_ATTRIBUTES,
                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
                    prepared = handle != INVALID_HANDLE_VALUE && prepared;
                    if (handle != INVALID_HANDLE_VALUE) {
                        prepared = SetFileTime(handle, nullptr, nullptr, &original.ftLastWriteTime) != FALSE && prepared;
                        CloseHandle(handle);
                    }
                } else { preserved = "external-target"; Write(altered, preserved); }
            }
        }
        Probe::RestoreCopy(manager);
        const auto resumed_id = manager.ResumeIncompleteTransfer(failed_id);
        if (resumed_id) OperationRecordsTestProbe::ExecuteNext(manager);
        const auto resumed = manager.DrainTransferOutcomes();
        Check(prepared && resumed_id && resumed.size() == 1 && resumed.front().phase == OpPhase::Failed &&
            !resumed.front().can_resume && Read(altered) == preserved,
            change_source ? "frozen recovery rejects a replaced source even when size and modification time match" :
            "frozen recovery rejects a newly appeared destination and preserves its external content");
    }
    {
        const auto incomplete_source = fixture.Directory(L"scan-incomplete-source");
        const auto incomplete_destination = fixture.Directory(L"scan-incomplete-destination");
        Write(incomplete_source / L"valid.txt", "valid-source");
        OpsManager manager;
        Probe::Run(manager, Request(incomplete_destination, {incomplete_source / L"valid.txt", incomplete_source / L"missing.txt"}), 2008);
        const auto outcomes = manager.DrainTransferOutcomes();
        Check(outcomes.size() == 1 && outcomes.front().phase == OpPhase::Failed && !outcomes.front().plan_complete &&
            !outcomes.front().can_restage && !std::filesystem::exists(incomplete_destination / L"valid.txt"),
            "incomplete initial scan cannot offer automatic reconstruction and performs no copy");
    }
    {
        const auto cleanup_source = fixture.Directory(L"cleanup-source") / L"item.txt";
        Write(cleanup_source, "original-object");
        const auto cleanup_destination = fixture.Directory(L"cleanup-destination");
        const auto displaced_source = fixture.Directory(L"external-source-objects") / L"item.txt";
        OpsManager manager;
        bool swapped = false;
        DWORD swap_error = ERROR_SUCCESS;
        Probe::ForceCopyMove(manager, [&](const std::wstring& path) {
            swapped = RenameFixtureObject(path, displaced_source);
            if (!swapped) swap_error = GetLastError();
            if (swapped) Write(Path(path), "replacement-object");
        });
        Probe::Run(manager, Request(cleanup_destination, {cleanup_source}, OpType::Move), 2009);
        const auto outcomes = manager.DrainTransferOutcomes();
        const auto* item = outcomes.size() == 1 ? Item(outcomes.front(), cleanup_source) : nullptr;
        if (!swapped || outcomes.empty() || outcomes.front().phase != OpPhase::Failed || !item ||
            !item->destination_committed || item->source_removed)
            std::printf("move cleanup diagnostics: swapped=%d error=%lu phase=%d hr=%08lx committed=%d removed=%d\n",
                swapped, swap_error, static_cast<int>(manager.Status().phase),
                outcomes.empty() ? 0ul : static_cast<unsigned long>(outcomes.front().failure_hr),
                item && item->destination_committed, item && item->source_removed);
        Check(swapped && outcomes.size() == 1 && outcomes.front().phase == OpPhase::Failed && !outcomes.front().can_restage &&
            item && item->state == TransferItemState::Incomplete && item->destination_committed && !item->source_removed &&
            Read(cleanup_source) == "replacement-object" && Read(cleanup_destination / L"item.txt") == "original-object",
            "move cleanup identity failure preserves replacement source and records the already committed destination");
    }
}
}

int main() {
    using namespace pulse::ops;
    using Probe = OperationRecordsTestProbe;
    OpsManager manager;
    Probe::Initialize(manager);
    OpRequest rejected;
    rejected.type = OpType::Rename;
    rejected.sources = {L"operation-records-source"};
    rejected.new_name = L"invalid/name"; // rejected before any filesystem operation
    const auto first = manager.Submit(rejected);
    const auto second = manager.Submit(rejected);
    auto tasks = manager.TaskStatuses();
    Check(tasks.size() == 2 && Find(tasks, first) && Find(tasks, second) &&
        Find(tasks, first)->phase == OpPhase::Queued && !Find(tasks, first)->active &&
        Find(tasks, first)->source_label == rejected.sources.front() &&
        Find(tasks, first)->destination_label == rejected.new_name,
        "submitted tasks retain distinct IDs and queued source/destination snapshots");
    Probe::ExecuteNext(manager);
    Check(manager.Status().phase == OpPhase::Failed, "invalid rename reaches a real terminal operation status");
    Probe::ExecuteNext(manager);
    tasks = manager.TaskStatuses();
    Check(tasks.size() == 2 && Find(tasks, first) && Find(tasks, second) &&
        Find(tasks, first)->phase == OpPhase::Failed && Find(tasks, second)->phase == OpPhase::Failed,
        "terminal results survive execution of the next task");
    Check(Find(tasks, first) && Find(tasks, first)->source_label == rejected.sources.front(),
        "terminal history preserves the full submitted source path");

    OpRequest transfer;
    transfer.type = OpType::Copy;
    transfer.sources = {L"operation-records-copy-source"};
    transfer.dest_dir = L"operation-records-destination";
    const auto current = manager.Submit(transfer);
    const auto queued = manager.Submit(transfer);
    Check(Probe::BeginNext(manager) == current, "fixture starts the next queued transfer status");
    manager.ClearFinishedTasks();
    tasks = manager.TaskStatuses();
    Check(tasks.size() == 2 && Find(tasks, current) && Find(tasks, current)->active &&
        Find(tasks, queued) && Find(tasks, queued)->phase == OpPhase::Queued,
        "clear removes terminal results while preserving active and queued tasks");
    Check(Find(tasks, current) && Find(tasks, current)->source_label == transfer.sources.front() &&
        Find(tasks, current)->destination_label == transfer.dest_dir,
        "active task snapshot preserves both full paths independently of legacy labels");
    Check(!manager.PauseTask(second) && !manager.ResumeTask(second) && !manager.CancelTask(second) &&
        !Probe::Paused(manager) && !Probe::Cancelled(manager),
        "stale task IDs cannot pause, resume or cancel the active transfer");
    Check(!manager.CancelTask(queued), "queued cancellation does not mutate the operation queue");
    Check(manager.PauseTask(current) && Probe::Paused(manager), "matching active transfer can be paused");
    Check(manager.ResumeTask(current) && !Probe::Paused(manager), "matching active transfer can be resumed");
    Check(manager.CancelTask(current) && Probe::Cancelled(manager) && !Probe::Paused(manager) &&
        manager.Status().phase == OpPhase::Cancelling && !manager.PauseTask(current),
        "matching cancellation clears pause and cannot be reversed by a later pause");

    Probe::Finish(manager, current, OpPhase::Cancelled);
    Check(Probe::BeginNext(manager) == queued, "next transfer has a distinct active task ID");
    Check(!manager.CancelTask(current) && !Probe::Cancelled(manager),
        "late cancellation cannot reach the next transfer");
    Probe::Finish(manager, queued, OpPhase::Completed);
    const auto completed_ops = manager.Status().completed_ops;
    manager.ClearFinishedTasks();
    Probe::Republish(manager);
    Check(manager.TaskStatuses().empty() && manager.Status().completed_ops == completed_ops,
        "cleared terminal status stays cleared without changing legacy completion counters");

    const auto still_queued = manager.Submit(transfer);
    for (uint64_t id = 1000; id < 1070; ++id)
        Probe::Finish(manager, id, id % 2 ? OpPhase::Cancelled : OpPhase::Failed);
    tasks = manager.TaskStatuses();
    Check(tasks.size() == 65 && !Find(tasks, 1000) && Find(tasks, 1006) && Find(tasks, 1069) &&
        Find(tasks, still_queued) && Find(tasks, still_queued)->phase == OpPhase::Queued,
        "terminal history is bounded at 64 while queued tasks are retained");
    manager.ClearFinishedTasks();
    tasks = manager.TaskStatuses();
    Check(tasks.size() == 1 && tasks.front().task_id == still_queued && tasks.front().phase == OpPhase::Queued,
        "clearing bounded history never consumes pending work");
    try { BatchAndOutcomeFixtures(); }
    catch (const std::exception& error) { std::printf("[FAIL] fixture exception: %s\n", error.what()); ++failures; }
    std::printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
