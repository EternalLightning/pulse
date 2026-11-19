// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "ops/ops_manager.h"
#include "shell_host/file_operations.h"
#include "fixture_cleanup.h"
#include <objbase.h>
#include <shlobj.h>
#include <sddl.h>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <clocale>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {
namespace fs = std::filesystem;
using namespace pulse::ops;
int failures = 0;

void Check(bool ok, const char* label) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
    failures += !ok;
}

void Pump() {
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    Sleep(2);
}

bool Wait(const std::function<bool()>& ready, DWORD timeout = 30000) {
    const auto deadline = GetTickCount64() + timeout;
    while (!ready() && GetTickCount64() < deadline) Pump();
    return ready();
}

void Write(const fs::path& path, const std::string& bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!output) throw std::runtime_error("fixture write failed");
}

std::string Read(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return {};
    return {std::istreambuf_iterator<char>(input), {}};
}

std::optional<uint64_t> Hash(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return {};
    uint64_t result = 1469598103934665603ull;
    char block[65536];
    while (input.read(block, sizeof(block)) || input.gcount()) {
        for (std::streamsize i = 0; i < input.gcount(); ++i) {
            result ^= static_cast<unsigned char>(block[i]);
            result *= 1099511628211ull;
        }
    }
    if (!input.eof()) return {};
    return result;
}

OpRequest Request(OpType type, std::initializer_list<fs::path> sources, const fs::path& destination = {}) {
    OpRequest request;
    request.type = type;
    for (const auto& source : sources) request.sources.push_back(source.wstring());
    request.dest_dir = destination.wstring();
    return request;
}

void Diagnostics(const OpStatus& status) {
    std::printf("[DIAG] task=%llu phase=%d hr=0x%08lX completed=%llu/%llu path=%ls error=%ls\n",
        static_cast<unsigned long long>(status.task_id), static_cast<int>(status.phase),
        static_cast<unsigned long>(status.failure_hr), static_cast<unsigned long long>(status.completed_items),
        static_cast<unsigned long long>(status.total_items), status.failed_path.c_str(), status.last_error.c_str());
}

class Session {
public:
    OpsManager manager;
    const fs::path fixture_root;
    std::atomic<bool> pause_after_one{false};
    std::atomic<bool> observed{false};
    explicit Session(const fs::path& root, HWND owner) : fixture_root(fs::absolute(root).lexically_normal()) {
        fs::create_directories(root);
        manager.SetJournalPath((root / L"operation-journal.json").wstring());
        manager.SetUiWindow(owner);
        manager.Start([this] {
            if (!pause_after_one.load()) return;
            const auto status = manager.Status();
            if (!status.active || !status.completed_items || status.completed_items >= status.total_items) return;
            if (!pause_after_one.exchange(false)) return;
            observed.store(true);
            manager.PauseCurrent();
        });
    }
    ~Session() { manager.Stop(); }
    std::optional<OpStatus> Finished(uint64_t id) const {
        for (const auto& status : manager.TaskStatuses()) {
            if (status.task_id == id && !status.active &&
                (status.phase == OpPhase::Completed || status.phase == OpPhase::Failed || status.phase == OpPhase::Cancelled))
                return status;
        }
        return {};
    }
    OpStatus Finish(uint64_t id, const std::function<void()>& present = {}) {
        if (!id) throw std::runtime_error("operation admission failed");
        if (!Wait([&] {
            if (present) present();
            return Finished(id).has_value();
        }, 60000)) {
            Diagnostics(manager.Status());
            manager.CancelCurrent();
            throw std::runtime_error("operation timed out");
        }
        auto status = *Finished(id);
        if (status.phase != OpPhase::Completed) Diagnostics(status);
        return status;
    }
    OpStatus Run(OpRequest request, const std::function<void()>& present = {}) {
        const auto status = Finish(manager.Submit(std::move(request)), present);
        if (!Wait([&] { return !manager.HasPendingFileOperations(); }))
            throw std::runtime_error("operation worker did not become idle");
        return status;
    }
    void ConfirmFixtureDelete() {
        if (const auto pending = manager.PendingDeleteConfirmation()) {
            bool owned = !pending->plan.targets.empty();
            for (const auto& target : pending->plan.targets) {
                owned &= !target.physical_paths.empty();
                for (const auto& path : target.physical_paths) {
                    const auto relative = fs::path(path).lexically_normal().lexically_relative(fixture_root);
                    owned &= !relative.empty() && !relative.is_absolute() && *relative.begin() != L".." && relative != L".";
                }
            }
            manager.ResolveDeleteConfirmation(pending->token, owned);
            if (!owned) Check(false, "deletion confirmation escaped its isolated fixture");
        }
    }
};

#include "operation_permission_cases.h"

void CopyAndMove(const fs::path& root, HWND owner) {
    Session session(root, owner);
    session.manager.SetVerifyCopies(true);
    const auto source = root / L"source", destination = root / L"destination", moved = root / L"moved";
    fs::create_directories(destination);
    fs::create_directories(moved);
    Write(source / L"plain.txt", "plain source");
    Write(source / L"tree/inner/leaf.txt", "nested source");
    fs::create_directories(source / L"tree/empty");
    auto status = session.Run(Request(OpType::Copy, {source / L"plain.txt", source / L"tree"}, destination));
    Check(status.phase == OpPhase::Completed && Read(destination / L"plain.txt") == "plain source" &&
        Read(destination / L"tree/inner/leaf.txt") == "nested source" && fs::is_directory(destination / L"tree/empty") &&
        fs::exists(source / L"tree/inner/leaf.txt"), "Shell copies ordinary files, nested trees and empty directories");
    auto outcomes = session.manager.DrainTransferOutcomes();
    Check(outcomes.size() == 1 && outcomes.front().phase == OpPhase::Completed &&
        !outcomes.front().items.empty() && std::all_of(outcomes.front().items.begin(), outcomes.front().items.end(),
            [](const auto& item) { return item.state == TransferItemState::Completed && item.destination_committed; }),
        "copy outcome confirms actual produced objects");
    Check(!session.manager.DrainCompletions().empty(), "copy publishes a UI completion");
    Check(status.transferred_bytes == fs::file_size(source / L"plain.txt") + fs::file_size(source / L"tree/inner/leaf.txt") &&
        status.total_bytes == status.transferred_bytes, "Shell transfer publishes actual final byte totals");
    status = session.Run(Request(OpType::Move, {destination / L"plain.txt", destination / L"tree"}, moved));
    Check(status.phase == OpPhase::Completed && !fs::exists(destination / L"plain.txt") && !fs::exists(destination / L"tree") &&
        Read(moved / L"plain.txt") == "plain source" && Read(moved / L"tree/inner/leaf.txt") == "nested source",
        "same-volume Shell move removes only confirmed sources");
    outcomes = session.manager.DrainTransferOutcomes();
    Check(outcomes.size() == 1 && std::all_of(outcomes.front().items.begin(), outcomes.front().items.end(),
        [](const auto& item) { return item.state == TransferItemState::Completed && item.source_removed; }),
        "move outcome records source removal");
}

void NativeByteProgress(const fs::path& root) {
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    Check(SUCCEEDED(initialized), "native byte-progress fixture initializes STA");
    if (FAILED(initialized)) return;
    const auto source = root / L"source.bin", destination = root / L"copied.bin";
    Write(source, std::string(8 * 1024 * 1024, 'b'));
    pulse::ipc::ShellOperationRequest request;
    request.items.push_back({source.wstring(), destination.wstring(), {}, {}});
    uint64_t observed = 0, expected = 0;
    size_t callbacks = 0;
    pulse::shell::FileOperationCallbacks progress;
    progress.bytes_progress = [&](uint64_t done, uint64_t total) {
        ++callbacks; observed = (std::max)(observed, done); expected = (std::max)(expected, total);
    };
    const auto result = pulse::shell::ExecuteFileOperation(request, progress);
    Check(SUCCEEDED(result.hr) && Hash(source) == Hash(destination) && callbacks &&
        observed == fs::file_size(source) && expected == observed,
        "native Shell calls our progress adapter with real transferred byte counts");
    CoUninitialize();
}

void OverwriteConflicts(const fs::path& root, HWND owner) {
    Session session(root, owner);
    const auto source = root / L"source" / L"same.txt", destination = root / L"destination";
    fs::create_directories(destination);
    Write(source, "first copy");
    auto status = session.Run(Request(OpType::Copy, {source}, destination));
    Check(status.phase == OpPhase::Completed && Read(destination / source.filename()) == "first copy",
        "initial copy creates the destination and retains its operation history");
    for (const auto attributes : {FILE_ATTRIBUTE_NORMAL, FILE_ATTRIBUTE_READONLY, FILE_ATTRIBUTE_HIDDEN}) {
        const auto incoming = "replacement-" + std::to_string(attributes);
        Write(source, incoming);
        if (!SetFileAttributesW((destination / source.filename()).c_str(), attributes))
            throw std::runtime_error("cannot set isolated overwrite attributes");
        uint64_t handled = 0;
        unsigned conflicts = 0;
        status = session.Run(Request(OpType::Copy, {source}, destination), [&] {
            const auto pending = session.manager.PendingConflict();
            if (!pending || pending->token == handled) return;
            handled = pending->token; ++conflicts;
            session.manager.ResolveConflict(pending->token, ConflictChoice::Replace, false);
        });
        Check(status.phase == OpPhase::Completed && !status.elevation_retry_available && conflicts == 1 && Read(source) == incoming &&
            Read(destination / source.filename()) == incoming,
            "repeated same-name Replace preserves source and overwrites destination bytes");
    }
    Write(source, "moved replacement");
    status = session.Run(Request(OpType::Move, {source}, destination), [&] {
        if (const auto pending = session.manager.PendingConflict())
            session.manager.ResolveConflict(pending->token, ConflictChoice::Replace, false);
    });
    Check(status.phase == OpPhase::Completed && !status.elevation_retry_available && !fs::exists(source) &&
        Read(destination / source.filename()) == "moved replacement",
        "same-name move replaces the existing target and removes only its source");
}

void MergeConflicts(const fs::path& root, HWND owner) {
    Session session(root, owner);
    const auto source = root / L"source/tree/inner", destination = root / L"destination/tree/inner";
    for (const auto* name : {L"replace.txt", L"skip.txt", L"both.txt"}) {
        Write(source / name, "incoming");
        Write(destination / name, "existing");
    }
    Write(source / L"fresh.txt", "fresh");
    size_t offered = 0;
    uint64_t handled = 0;
    auto status = session.Run(Request(OpType::Copy, {root / L"source/tree"}, root / L"destination"), [&] {
        const auto pending = session.manager.PendingConflict();
        if (!pending || pending->token == handled) return;
        handled = pending->token;
        std::vector<ConflictResolution> resolutions;
        for (const auto& row : pending->rows) {
            const auto name = fs::path(row.source).filename();
            const auto choice = name == L"replace.txt" ? ConflictChoice::Replace :
                name == L"both.txt" ? ConflictChoice::KeepBoth : ConflictChoice::Skip;
            resolutions.push_back({row.row_id, choice});
            ++offered;
        }
        Check(session.manager.ResolveConflicts(pending->token, resolutions), "custom UI accepts exact conflict row decisions");
    });
    size_t incoming_copies = 0;
    for (const auto& item : fs::directory_iterator(destination)) {
        if (item.is_regular_file() && Read(item.path()) == "incoming") ++incoming_copies;
    }
    Check(status.phase == OpPhase::Completed && offered == 3 && Read(destination / L"replace.txt") == "incoming" &&
        Read(destination / L"skip.txt") == "existing" && Read(destination / L"both.txt") == "existing" &&
        Read(destination / L"fresh.txt") == "fresh" && incoming_copies == 2,
        "nested merge obeys Replace, Skip and KeepBoth without native conflict prompts");
    const auto outcomes = session.manager.DrainTransferOutcomes();
    Check(outcomes.size() == 1 && std::count_if(outcomes.front().items.begin(), outcomes.front().items.end(),
        [](const auto& item) { return item.state == TransferItemState::Skipped; }) == 1,
        "merge outcome distinguishes the explicitly skipped leaf");
    Write(source / L"late.txt", "incoming late");
    Write(destination / L"late.txt", "offered target");
    uint64_t late_token = 0;
    status = session.Run(Request(OpType::Copy, {source / L"late.txt"}, destination), [&] {
        const auto pending = session.manager.PendingConflict();
        if (!pending || pending->token == late_token) return;
        late_token = pending->token;
        fs::rename(destination / L"late.txt", destination / L"offered-target.txt");
        Write(destination / L"late.txt", "replacement target");
        session.manager.ResolveConflict(pending->token, ConflictChoice::Replace, false);
    });
    Check(status.phase == OpPhase::Failed && Read(destination / L"late.txt") == "replacement target" &&
        Read(destination / L"offered-target.txt") == "offered target",
        "Replace refuses a target replaced while the conflict window was open");

    const auto move_root = root / L"move-undo";
    Session moved(move_root, owner);
    Write(move_root / L"source/tree/inner/leaf.txt", "merged move");
    fs::create_directories(move_root / L"destination/tree/inner");
    auto cut_move = Request(OpType::Move, {move_root / L"source/tree"}, move_root / L"destination");
    const auto cut_source = cut_move.sources.front();
    constexpr uint32_t clipboard_sequence = 17;
    cut_move.cut_clipboard_sequence = clipboard_sequence;
    status = moved.Run(std::move(cut_move));
    Check(status.phase == OpPhase::Completed && !fs::exists(move_root / L"source/tree") && moved.manager.CanUndo(),
        "merged move removes only its empty source containers and retains undo");
    const auto completed = moved.manager.DrainCompletions();
    Check(std::any_of(completed.begin(), completed.end(), [&](const auto& entry) {
        return entry.type == OpType::Move && entry.cut_clipboard_sequence == clipboard_sequence &&
            std::find(entry.sources.begin(), entry.sources.end(), cut_source) != entry.sources.end();
    }), "merged cut publishes removal of the original clipboard root");
    const auto before_undo = moved.manager.Status().completed_ops;
    moved.manager.Undo();
    const bool undone = Wait([&] { return moved.manager.Status().completed_ops > before_undo &&
        !moved.manager.HasPendingFileOperations(); });
    Check(undone && moved.manager.Status().phase == OpPhase::Completed &&
        Read(move_root / L"source/tree/inner/leaf.txt") == "merged move" &&
        !fs::exists(move_root / L"destination/tree/inner/leaf.txt"),
        "merged move undo recreates missing source parents through Shell");
}

void LateRenameConflicts(const fs::path& root, HWND owner) {
    for (const bool batch : {false, true}) {
        const auto test = root / (batch ? L"batch" : L"single");
        Session session(test, owner);
        Write(test / L"gate/source.txt", "gate source");
        Write(test / L"gate/destination/source.txt", "gate existing");
        const auto gate = session.manager.Submit(Request(OpType::Copy, {test / L"gate/source.txt"}, test / L"gate/destination"));
        if (!Wait([&] { return session.manager.PendingConflict().has_value(); }))
            throw std::runtime_error("rename queue gate never reached conflict");
        Write(test / L"a.txt", "rename source a");
        Write(test / L"b.txt", "rename source b");
        auto rename = Request(batch ? OpType::BatchRename : OpType::Rename, {test / L"a.txt"});
        rename.new_name = L"renamed-a.txt";
        if (batch) {
            rename.sources.push_back((test / L"b.txt").wstring());
            rename.new_names = {L"renamed-a.txt", L"renamed-b.txt"};
        }
        const auto task = session.manager.Submit(rename);
        Write(test / L"renamed-a.txt", "late existing object");
        session.manager.CancelCurrent();
        session.Finish(gate);
        const auto status = session.Finish(task, [&] {
            if (const auto conflict = session.manager.PendingConflict(); conflict && conflict->task_id == task)
                session.manager.ResolveConflict(conflict->token, ConflictChoice::Cancel, false);
        });
        Check(status.phase != OpPhase::Completed && Read(test / L"renamed-a.txt") == "late existing object" &&
            Read(test / L"a.txt") == "rename source a" && (!batch || Read(test / L"b.txt") == "rename source b"),
            batch ? "late batch-rename collision never overwrites an existing object" :
                "late rename collision never overwrites an existing object");
        rename.new_name = L"fresh-a.txt";
        if (batch) rename.new_names = {L"fresh-a.txt", L"fresh-b.txt"};
        const auto renamed = session.Run(rename);
        Check(renamed.phase == OpPhase::Completed && !fs::exists(test / L"a.txt") &&
            renamed.total_items == (batch ? 2u : 1u) && renamed.completed_items == renamed.total_items &&
            Read(test / L"fresh-a.txt") == "rename source a" &&
            (!batch || (!fs::exists(test / L"b.txt") && Read(test / L"fresh-b.txt") == "rename source b")),
            batch ? "Shell batch rename publishes both requested new names" : "Shell rename publishes its requested new name");
    }
}

void Cancellation(const fs::path& root, HWND owner) {
    {
        const auto test = root / L"before-start";
        Session session(test, owner);
        Write(test / L"source/a.txt", "incoming");
        Write(test / L"destination/a.txt", "existing");
        const auto task = session.manager.Submit(Request(OpType::Copy, {test / L"source/a.txt"}, test / L"destination"));
        if (!Wait([&] { return session.manager.PendingConflict().has_value(); }))
            throw std::runtime_error("pre-dispatch cancellation never reached conflict");
        session.manager.CancelCurrent();
        const auto status = session.Finish(task);
        Check(status.phase == OpPhase::Cancelled && Read(test / L"destination/a.txt") == "existing" &&
            session.manager.DrainCompletions().empty(), "cancel before Shell dispatch preserves destination and emits no completion");
        fs::rename(test / L"source/a.txt", test / L"source/parked.txt");
        Write(test / L"source/a.txt", "replacement source");
        const auto resumed = session.manager.ResumeIncompleteTransfer(task);
        const auto resumed_status = session.Finish(resumed);
        Check(resumed_status.phase == OpPhase::Failed && Read(test / L"destination/a.txt") == "existing" &&
            Read(test / L"source/a.txt") == "replacement source",
            "continuation refuses a source replaced after cancellation");
    }
    {
        const auto test = root / L"mid-batch";
        Session session(test, owner);
        const auto source = test / L"source", destination = test / L"destination";
        fs::create_directories(destination);
        auto request = Request(OpType::Copy, {}, destination);
        for (int i = 0; i < 64; ++i) {
            const auto file = source / (L"item-" + std::to_wstring(i) + L".bin");
            Write(file, std::string(2 * 1024 * 1024, static_cast<char>('a' + i % 26)));
            request.sources.push_back(file.wstring());
        }
        session.pause_after_one.store(true);
        const auto task = session.manager.Submit(request);
        const bool paused = Wait([&] { return session.manager.Status().phase == OpPhase::Paused || session.Finished(task).has_value(); });
        const auto before = session.manager.Status();
        const auto until = GetTickCount64() + 150;
        while (GetTickCount64() < until) Pump();
        const auto after = session.manager.Status();
        Check(paused && session.observed.load() && before.phase == OpPhase::Paused && after.phase == OpPhase::Paused &&
            before.transferred_bytes == after.transferred_bytes && before.completed_items == after.completed_items,
            "Shell pause acknowledges and holds real batch progress stable");
        session.manager.CancelCurrent();
        const auto status = session.Finish(task);
        size_t produced = 0;
        bool content_valid = true;
        for (const auto& item : fs::directory_iterator(destination)) {
            ++produced;
            const auto actual = Hash(item.path()), expected = Hash(source / item.path().filename());
            content_valid &= actual && expected && actual == expected;
        }
        const auto outcomes = session.manager.DrainTransferOutcomes();
        const auto completions = session.manager.DrainCompletions();
        size_t confirmed = 0;
        if (outcomes.size() == 1) {
            for (const auto& item : outcomes.front().items)
                confirmed += item.state == TransferItemState::Completed && item.destination_committed;
        }
        Check(status.phase == OpPhase::Cancelled && produced > 0 && produced < request.sources.size() && content_valid &&
            outcomes.size() == 1 && outcomes.front().phase == OpPhase::Cancelled && confirmed == produced &&
            !outcomes.front().remaining_sources.empty(), "mid-batch cancel retains only confirmed partial outputs with truthful terminal evidence");
        Check(std::all_of(completions.begin(), completions.end(), [&](const auto& completion) {
            return std::all_of(completion.destinations.begin(), completion.destinations.end(), [&](const auto& path) {
                return fs::exists(path) && Hash(path) == Hash(source / fs::path(path).filename());
            });
        }), "partial completion never advertises missing or incomplete output");
        const auto idle_until = GetTickCount64() + 200;
        while (GetTickCount64() < idle_until) Pump();
        Check(!session.manager.HasPendingFileOperations() && static_cast<size_t>(std::distance(fs::directory_iterator(destination),
            fs::directory_iterator{})) == produced && session.manager.DrainTransferOutcomes().empty(),
            "cancelled batch does not automatically replay remaining sources");
        if (outcomes.size() == 1 && outcomes.front().can_resume) {
            const auto resumed = session.manager.ResumeIncompleteTransfer(task);
            const auto resumed_status = session.Finish(resumed);
            bool exact = true;
            for (const auto& file : request.sources) exact &= Hash(file) == Hash(destination / fs::path(file).filename());
            Check(resumed_status.phase == OpPhase::Completed && exact &&
                static_cast<size_t>(std::distance(fs::directory_iterator(destination), fs::directory_iterator{})) == request.sources.size(),
                "explicit continuation copies untouched sources without replaying confirmed outputs");
        } else Check(false, "cancelled batch exposes an explicit continuation for untouched sources");
    }
}

void UndoIdentity(const fs::path& root, HWND owner) {
    for (const auto type : {OpType::Copy, OpType::Move, OpType::Rename}) {
        const auto test = root / std::to_wstring(static_cast<int>(type));
        Session session(test, owner);
        const auto source = test / L"source.txt", destination = test / L"destination";
        fs::create_directories(destination);
        Write(source, "original object");
        auto request = Request(type, {source}, destination);
        request.new_name = L"renamed.txt";
        const auto produced = type == OpType::Rename ? test / L"renamed.txt" : destination / L"source.txt";
        const auto status = session.Run(request);
        Check(status.phase == OpPhase::Completed && session.manager.CanUndo(), "successful mutation retains session undo identity");
        if (status.phase != OpPhase::Completed || !fs::exists(produced)) continue;
        const auto parked = test / L"parked-original.txt";
        fs::rename(produced, parked);
        Write(produced, "replacement object");
        session.manager.DrainCompletions();
        session.manager.Undo();
        if (!Wait([&] {
            session.ConfirmFixtureDelete();
            return !session.manager.HasPendingFileOperations();
        })) throw std::runtime_error("identity-refusing undo did not finish");
        const bool original_location_correct = type == OpType::Copy ? Read(source) == "original object" : !fs::exists(source);
        Check(Read(produced) == "replacement object" && Read(parked) == "original object" && original_location_correct &&
            session.manager.DrainCompletions().empty(),
            type == OpType::Copy ? "copy undo refuses a replacement at the produced path" :
            type == OpType::Move ? "move undo refuses a replacement at the produced path" :
                "rename undo refuses a replacement at the produced path");
    }
}

void NamesAndAttributes(const fs::path& root, HWND owner) {
    Session session(root, owner);
    const auto source = root / L"大小写 Case.txt";
    Write(source, "case-only rename bytes");
    auto request = Request(OpType::Rename, {source});
    request.new_name = L"大小写 CASE.txt";
    auto status = session.Run(request);
    bool exact_name = false;
    for (const auto& entry : fs::directory_iterator(root))
        exact_name |= entry.path().filename() == request.new_name;
    Check(status.phase == OpPhase::Completed && exact_name && Read(root / request.new_name) == "case-only rename bytes",
        "case-only Unicode rename publishes the exact requested spelling and bytes");
    const auto before_undo = session.manager.Status().completed_ops;
    session.manager.Undo();
    const bool undone = Wait([&] { return session.manager.Status().completed_ops > before_undo &&
        !session.manager.HasPendingFileOperations(); });
    exact_name = false;
    for (const auto& entry : fs::directory_iterator(root)) exact_name |= entry.path().filename() == source.filename();
    Check(undone && session.manager.Status().phase == OpPhase::Completed && exact_name &&
        Read(source) == "case-only rename bytes", "case-only rename undo restores the exact original spelling and bytes");

    Write(root / L"folder Case/leaf.txt", "case-only directory bytes");
    request = Request(OpType::Rename, {root / L"folder Case"});
    request.new_name = L"folder CASE";
    status = session.Run(request);
    exact_name = false;
    for (const auto& entry : fs::directory_iterator(root)) exact_name |= entry.path().filename() == request.new_name;
    Check(status.phase == OpPhase::Completed && exact_name &&
        Read(root / L"folder CASE/leaf.txt") == "case-only directory bytes",
        "case-only directory rename preserves the exact requested spelling and nested bytes");

    const auto readonly = root / L"readonly.bin", copied = root / L"copy", moved = root / L"move";
    fs::create_directory(copied);
    fs::create_directory(moved);
    Write(readonly, "read-only file bytes");
    if (!SetFileAttributesW(readonly.c_str(), FILE_ATTRIBUTE_READONLY))
        throw std::runtime_error("cannot prepare read-only fixture");
    status = session.Run(Request(OpType::Copy, {readonly}, copied));
    Check(status.phase == OpPhase::Completed && Read(readonly) == "read-only file bytes" &&
        Read(copied / readonly.filename()) == "read-only file bytes", "Shell copies a read-only source without changing its bytes");
    request = Request(OpType::Rename, {readonly});
    request.new_name = L"renamed-readonly.dat";
    status = session.Run(request);
    const auto renamed = root / request.new_name;
    Check(status.phase == OpPhase::Completed && !fs::exists(readonly) && Read(renamed) == "read-only file bytes",
        "Shell renames a read-only fixture including its extension");
    status = session.Run(Request(OpType::Move, {renamed}, moved));
    const auto product = moved / renamed.filename();
    Check(status.phase == OpPhase::Completed && !fs::exists(renamed) && Read(product) == "read-only file bytes",
        "Shell moves a read-only fixture");
    status = session.Run(Request(OpType::RealDelete, {product}), [&] { session.ConfirmFixtureDelete(); });
    Check(status.phase == OpPhase::Completed && !fs::exists(product), "confirmed Shell deletion removes a read-only fixture");
    if (fs::exists(copied / readonly.filename()))
        SetFileAttributesW((copied / readonly.filename()).c_str(), FILE_ATTRIBUTE_NORMAL);
}

void CaseRenameRecovery(const fs::path& root) {
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    Check(SUCCEEDED(initialized), "case-rename recovery fixture initializes STA");
    if (FAILED(initialized)) return;
    for (const bool collide : {false, true}) {
        const auto folder = root / (collide ? L"collision" : L"cancel");
        const auto source = folder / L"case.txt", target = folder / L"CASE.txt";
        Write(source, "original case bytes");
        pulse::ipc::ShellOperationRequest request;
        request.kind = pulse::ipc::ShellOperationKind::Rename;
        request.items.push_back({source.wstring(), target.wstring(), {}, {}});
        bool interrupted = false;
        pulse::shell::FileOperationCallbacks callbacks;
        callbacks.cancelled = [&] { return interrupted && !collide; };
        callbacks.progress = [&](float, const std::wstring&, size_t done, size_t) {
            if (!done || interrupted) return;
            interrupted = true;
            if (collide) Write(target, "late replacement");
        };
        const auto result = pulse::shell::ExecuteFileOperation(request, callbacks);
        if (collide) {
            Check(interrupted && FAILED(result.hr) && !result.items.empty() &&
                SUCCEEDED(result.items.front().hr) && result.items.front().source == source.wstring() &&
                Read(result.items.front().destination) == "original case bytes" && Read(target) == "late replacement",
                "late case-rename collision preserves both objects and reports the retained intermediate name");
        } else {
            Check(interrupted && result.cancelled && result.items.empty() && Read(source) == "original case bytes" &&
                std::distance(fs::directory_iterator(folder), fs::directory_iterator{}) == 1,
                "cancelling between case-rename steps restores the original and leaves no temporary name");
        }
    }
    CoUninitialize();
}

fs::path Extended(const fs::path& path) {
    const auto value = path.wstring();
    return value.starts_with(L"\\\\") ? L"\\\\?\\UNC\\" + value.substr(2) : L"\\\\?\\" + value;
}

void LongPaths(const fs::path& root, HWND owner) {
    Session session(root, owner);
    auto source_parent = root / L"source", destination = root / L"destination";
    while (source_parent.wstring().size() < 320) source_parent /= L"long folder 012345678901234567890123456789";
    while (destination.wstring().size() < 320) destination /= L"long destination 01234567890123456789012345";
    const auto source = source_parent / L"长路径 文件.txt", target = destination / source.filename();
    Write(Extended(source), "long-path bytes");
    fs::create_directories(Extended(destination));
    const auto status = session.Run(Request(OpType::Copy, {Extended(source)}, Extended(destination)));
    Check(status.phase == OpPhase::Completed && Read(Extended(source)) == "long-path bytes" &&
        Read(Extended(target)) == "long-path bytes", "Shell copies between extended source and destination paths longer than MAX_PATH");
}

void ShellPathAvailability(const fs::path& root) {
    const auto source = root / L"shell-path.txt";
    Write(source, "isolated Shell parsing probe");
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    Check(SUCCEEDED(initialized), "Shell-path fixture initializes STA");
    if (FAILED(initialized)) return;
    IShellItem* item = nullptr;
    const HRESULT parsed = SHCreateItemFromParsingName(source.c_str(), nullptr, IID_PPV_ARGS(&item));
    std::printf("[DIAG] direct Windows SHCreateItemFromParsingName hr=0x%08lX path=%ls\n", parsed, source.c_str());
    Check(SUCCEEDED(parsed), "Windows Shell parses the isolated provider path without Pulse operation logic");
    if (item) item->Release();
    CoUninitialize();
}

void CreateDeleteRestore(const fs::path& root, HWND owner) {
    Session session(root, owner);
    auto status = session.Run(Request(OpType::CreateFolder, {root / L"created-folder"}));
    Check(status.phase == OpPhase::Completed && fs::is_directory(root / L"created-folder"), "Shell creates a folder");
    status = session.Run(Request(OpType::CreateTextFile, {root / L"created-file.txt"}));
    Check(status.phase == OpPhase::Completed && fs::is_regular_file(root / L"created-file.txt") &&
        fs::file_size(root / L"created-file.txt") == 0, "Shell creates an empty ordinary file");
    Write(root / L"permanent/inner.txt", "permanent fixture");
    status = session.Run(Request(OpType::RealDelete, {root / L"permanent"}), [&] { session.ConfirmFixtureDelete(); });
    Check(status.phase == OpPhase::Completed && !fs::exists(root / L"permanent"), "confirmed permanent delete removes only its fixture tree");
    Write(root / L"recycle.txt", "restore fixture");
    status = session.Run(Request(OpType::RecycleDelete, {root / L"recycle.txt"}), [&] { session.ConfirmFixtureDelete(); });
    Check(status.phase == OpPhase::Completed && !fs::exists(root / L"recycle.txt") && session.manager.CanUndo(),
        "fixture recycle reports an undoable successful deletion");
    if (status.phase == OpPhase::Failed) {
        Check(Read(root / L"recycle.txt") == "restore fixture",
            "refused recycle preserves the source instead of permanently deleting it");
    }
    if (status.phase == OpPhase::Completed && !fs::exists(root / L"recycle.txt")) {
        const auto previous = session.manager.Status().completed_ops;
        session.manager.Undo();
        const bool restored = Wait([&] { return session.manager.Status().completed_ops > previous &&
            !session.manager.HasPendingFileOperations(); });
        Check(restored && session.manager.Status().phase == OpPhase::Completed && Read(root / L"recycle.txt") == "restore fixture",
            "recycle undo restores only the selected fixture with its original bytes");
    }
}

void OptionalCrossVolume(const fs::path& root, HWND owner, const fs::path& parent) {
    if (parent.empty()) {
        std::printf("[SKIP] cross-volume move: no second isolated fixed-volume parent supplied\n");
        return;
    }
    const auto normalized = fs::absolute(parent).lexically_normal();
    DWORD first_serial = 0, second_serial = 0;
    if (!fs::is_directory(normalized) || GetDriveTypeW(normalized.root_path().c_str()) != DRIVE_FIXED ||
        !GetVolumeInformationW(root.root_path().c_str(), nullptr, 0, &first_serial, nullptr, nullptr, nullptr, 0) ||
        !GetVolumeInformationW(normalized.root_path().c_str(), nullptr, 0, &second_serial, nullptr, nullptr, nullptr, 0) ||
        first_serial == second_serial) throw std::runtime_error("cross-volume parent must be an existing directory on a distinct fixed volume");
    const auto second = normalized / root.parent_path().filename();
    if (fs::exists(second)) throw std::runtime_error("cross-volume fixture already exists");
    fs::create_directory(second);
    std::printf("[FIXTURE] second volume: %ls\n", second.c_str());
    Session session(root, owner);
    Write(root / L"source.txt", "cross-volume source");
    auto status = session.Run(Request(OpType::Move, {root / L"source.txt"}, second));
    Check(status.phase == OpPhase::Completed && !fs::exists(root / L"source.txt") && Read(second / L"source.txt") == "cross-volume source",
        "real cross-volume Shell move preserves bytes and removes source");
    status = session.Run(Request(OpType::Move, {second / L"source.txt"}, root));
    Check(status.phase == OpPhase::Completed && !fs::exists(second / L"source.txt") && Read(root / L"source.txt") == "cross-volume source",
        "real cross-volume move returns its isolated fixture");
    if (second.parent_path() == normalized && second.filename() == root.parent_path().filename() && fs::is_empty(second))
        fs::remove(second);
}

void OptionalExfatRead(const fs::path& root, HWND owner) {
    wchar_t drives[512]{};
    GetLogicalDriveStringsW(static_cast<DWORD>(std::size(drives)), drives);
    fs::path source;
    for (const wchar_t* drive = drives; *drive && source.empty(); drive += wcslen(drive) + 1) {
        wchar_t filesystem[64]{};
        if (GetDriveTypeW(drive) != DRIVE_REMOVABLE || !GetVolumeInformationW(drive, nullptr, 0, nullptr, nullptr, nullptr,
                filesystem, static_cast<DWORD>(std::size(filesystem))) || _wcsicmp(filesystem, L"exFAT") != 0) continue;
        WIN32_FIND_DATAW found{};
        HANDLE search = FindFirstFileW((fs::path(drive) / L"*").c_str(), &found);
        if (search == INVALID_HANDLE_VALUE) continue;
        do {
            if (!(found.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) &&
                !found.nFileSizeHigh && found.nFileSizeLow <= 1024 * 1024) {
                source = fs::path(drive) / found.cFileName;
                break;
            }
        } while (FindNextFileW(search, &found));
        FindClose(search);
    }
    if (source.empty()) {
        std::printf("[SKIP] read-only exFAT copy: no small root-level file on a removable exFAT volume\n");
        return;
    }
    Session session(root, owner);
    const auto before = Hash(source);
    const auto status = session.Run(Request(OpType::Copy, {source}, root));
    const auto copied = Hash(root / source.filename()), after = Hash(source);
    Check(status.phase == OpPhase::Completed && before && copied && after && before == copied && before == after,
        "real exFAT source is read only and local Shell copy hash matches");
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    std::setlocale(LC_CTYPE, ".UTF8");
    setvbuf(stdout, nullptr, _IONBF, 0);
    HANDLE token = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        alignas(TOKEN_MANDATORY_LABEL) BYTE storage[1024]{};
        DWORD bytes = 0;
        if (GetTokenInformation(token, TokenIntegrityLevel, storage, sizeof(storage), &bytes)) {
            const auto label = reinterpret_cast<TOKEN_MANDATORY_LABEL*>(storage);
            const auto count = *GetSidSubAuthorityCount(label->Label.Sid);
            std::printf("[DIAG] process integrity RID=0x%lX\n", *GetSidSubAuthority(label->Label.Sid, count - 1));
        }
        CloseHandle(token);
    }
    std::wstring selected;
    fs::path cross_volume_parent;
    fs::path fixture_parent;
    for (int i = 1; i < argc; ++i) {
        const std::wstring_view option(argv[i]);
        if ((option == L"--case" || option == L"--cross-volume-parent" || option == L"--fixture-parent") && i + 1 < argc) {
            if (option == L"--case") selected = argv[++i];
            else if (option == L"--fixture-parent") fixture_parent = argv[++i];
            else cross_volume_parent = argv[++i];
        } else {
            std::printf("usage: pulse_shell_operation_integration_test --case CASE [--fixture-parent DIRECTORY] [--cross-volume-parent DIRECTORY]\nCASES: transfer overwrite permissions permissions-admin (real UAC) merge rename cancel undo names-attributes case-recovery paths shell-path create-delete cross-volume exfat-read native-progress\n");
            return 2;
        }
    }
    if (selected.empty()) { std::printf("[FAIL] select an explicit --case before running file operations\n"); return 2; }
    if (selected != L"transfer" && selected != L"overwrite" && selected != L"permissions" &&
        selected != L"permissions-admin" && selected != L"merge" && selected != L"rename" &&
        selected != L"cancel" && selected != L"undo" && selected != L"create-delete" && selected != L"cross-volume" &&
        selected != L"exfat-read" && selected != L"native-progress" && selected != L"names-attributes" &&
        selected != L"case-recovery" && selected != L"paths" && selected != L"shell-path") return 2;
    const auto run = [&](std::wstring_view name) { return selected == name; };
    auto parent = fs::absolute(fixture_parent.empty() ? fs::path(L"bench_data") : fixture_parent).lexically_normal();
    if (parent.filename().empty() && parent != parent.root_path()) parent = parent.parent_path();
    const auto root = parent / (L"shell-operation-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    if (!fs::create_directory(root)) { std::printf("[FAIL] exclusive fixture creation failed\n"); return 2; }
    std::printf("[FIXTURE] %ls\n", root.c_str());
    const HWND owner = CreateWindowExW(0, L"STATIC", L"Pulse isolated Shell operations", 0,
        0, 0, 0, 0, HWND_MESSAGE, nullptr, nullptr, nullptr);
    Check(owner != nullptr, "isolated message-only Shell owner exists");
    try {
        if (!owner) throw std::runtime_error("Shell owner creation failed");
        if (run(L"transfer")) CopyAndMove(root / L"transfer", owner);
        if (run(L"overwrite")) OverwriteConflicts(root / L"overwrite", owner);
        if (run(L"permissions") || run(L"permissions-admin"))
            PermissionOperations(root / L"permissions", owner, run(L"permissions-admin"));
        if (run(L"native-progress")) NativeByteProgress(root / L"native-progress");
        if (run(L"merge")) MergeConflicts(root / L"merge", owner);
        if (run(L"rename")) LateRenameConflicts(root / L"rename", owner);
        if (run(L"cancel")) Cancellation(root / L"cancel", owner);
        if (run(L"undo")) UndoIdentity(root / L"undo", owner);
        if (run(L"names-attributes")) NamesAndAttributes(root / L"names-attributes", owner);
        if (run(L"case-recovery")) CaseRenameRecovery(root / L"case-recovery");
        if (run(L"paths")) LongPaths(root / L"paths", owner);
        if (run(L"shell-path")) ShellPathAvailability(root / L"shell-path");
        if (run(L"create-delete")) CreateDeleteRestore(root / L"create-delete", owner);
        if (run(L"cross-volume")) OptionalCrossVolume(root / L"cross-volume", owner, cross_volume_parent);
        if (run(L"exfat-read")) OptionalExfatRead(root / L"exfat-read", owner);
    } catch (const std::exception& error) {
        std::printf("[FAIL] fixture aborted: %s\n", error.what());
        ++failures;
    }
    if (owner) DestroyWindow(owner);
    if (!failures && root.parent_path() == parent && root.filename().wstring().starts_with(L"shell-operation-")) {
        std::error_code error;
        pulse::bench::RemoveFixtureTree(root, error);
        if (error) std::printf("[DIAG] cleanup error=%d fixture=%ls\n", error.value(), root.c_str());
        Check(!error && !fs::exists(root), "isolated fixture cleaned");
    } else std::printf("[DIAG] failed fixture retained: %ls\n", root.c_str());
    std::printf("[RESULT] %d failures\n", failures);
    return failures ? 1 : 0;
}
