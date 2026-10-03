#include "../ops/ops_manager.h"
#include <filesystem>
#include <fstream>
#include <cstdio>
#include <algorithm>

namespace pulse::ops {
struct TransferIntegrationProbe {
    static void Initialize(OpsManager& manager) {
        manager.running_ = true;
        manager.accepting_ = true;
        manager.notify_ = [&manager] {
            if (const auto conflict = manager.PendingConflict())
                manager.ResolveConflict(conflict->token, ConflictChoice::KeepBoth, false);
        };
    }
    static void Diagnostics(const OpsManager& manager) {
        const auto status = manager.Status();
        std::printf("[DIAG] task=%llu phase=%d hr=0x%08lX failed=%ls locked=%ls owners=%zu error=%ls\n",
            static_cast<unsigned long long>(status.task_id), static_cast<int>(status.phase),
            static_cast<unsigned long>(status.failure_hr), status.failed_path.c_str(),
            status.locked_path.c_str(), status.lock_owners.size(), status.last_error.c_str());
        for (const auto& owner : status.lock_owners)
            std::printf("[DIAG] PID=%lu terminable=%d image=%ls\n", owner.pid, owner.can_terminate,
                owner.image_path.c_str());
        if (!manager.lock_retry_) return;
        std::printf("[DIAG] retained_plan=%zu completed=%zu endpoint_candidates=%zu\n",
            manager.lock_retry_->retry_transfer_plan.size(), manager.lock_retry_->retry_completed_sources.size(),
            manager.lock_retry_->retry_probe_paths.size());
    }
    static void Run(OpsManager& manager, const OpRequest& request, uint64_t id) {
        manager.RunTransfer(request, id);
        Diagnostics(manager);
        if (manager.Status().phase != OpPhase::Failed) return;
        auto candidates = request.sources;
        if (!manager.Status().failed_path.empty()) candidates = {manager.Status().failed_path};
        else for (const auto& source : request.sources)
            candidates.push_back((std::filesystem::path(request.dest_dir) / std::filesystem::path(source).filename()).wstring());
        for (const auto& path : candidates) {
            std::wstring diagnostics;
            const auto report = ProbeFileLocks(manager.Status().failure_hr, path, {path},
                LockProtectedDirectory(), {}, &diagnostics);
            std::printf("[DIAG] direct_probe=%ls owners=%zu\n%ls", path.c_str(), report.owners.size(), diagnostics.c_str());
        }
    }
    static bool RunQueued(OpsManager& manager, bool cancel_after_dequeue = false) {
        if (manager.queue_.empty()) return false;
        auto item = std::move(manager.queue_.front());
        manager.queue_.pop_front();
        if (cancel_after_dequeue) manager.CancelCurrent();
        // Exactly the WorkerThread decision path, not a test-only bool reset.
        manager.ExecuteFileOperation(item);
        Diagnostics(manager);
        return true;
    }
    static bool QueueEmpty(const OpsManager& manager) { return manager.queue_.empty(); }
    static bool QueueFixtureEnd(OpsManager& manager, uint64_t id, const std::vector<DWORD>& pids) {
        const auto status = manager.Status();
        if (status.lock_owners.size() != pids.size() || std::any_of(status.lock_owners.begin(),
            status.lock_owners.end(), [&](const auto& owner) {
                return std::find(pids.begin(), pids.end(), owner.pid) == pids.end();
            })) return false; // never end an unexpected/user process discovered by RM
        return manager.RetryLockedOperation(id, true);
    }
    static DWORD RefuseSecondQueuedOwner(OpsManager& manager) {
        if (manager.queue_.empty() || manager.queue_.front().close_first.size() < 2) return 0;
        auto& owner = manager.queue_.front().close_first[1];
        owner.image_path += L".stale";
        return owner.pid;
    }
    static bool DeleteRemaining(OpsManager& manager, const OpRequest& request,
        const std::wstring& done, const std::wstring& remaining, uint64_t id) {
        OpsManager::QueueItem admitted;
        admitted.req = request; admitted.seq = id;
        DeletePlan accepted;
        accepted.targets = {{done, {done}, DeleteDisposition::Permanent, {}},
            {remaining, {remaining}, DeleteDisposition::Permanent, {}}};
        auto retry = OpsManager::RemainingDeleteRetry(admitted, accepted, {done});
        const bool exact = retry.req.sources == std::vector<std::wstring>{remaining} &&
            retry.req.delete_targets.size() == 1 && retry.req.delete_targets.front().path == remaining;
        auto locks = manager.CaptureLockedFailure(retry, HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION),
            remaining, [] { return false; });
        manager.SetStatus([&](OpStatus& status) {
            status.active = false; status.phase = OpPhase::Failed; status.task_id = id;
            status.locked_path = locks.path; status.lock_owners = locks.owners;
        });
        return exact && !locks.owners.empty();
    }
    static bool QueuedDeleteIsOnly(const OpsManager& manager, const std::wstring& remaining) {
        return !manager.queue_.empty() && manager.queue_.front().req.sources == std::vector<std::wstring>{remaining};
    }
    static void InjectFailure(OpsManager& manager, const OpRequest& request,
        const std::vector<TransferPlanEntry>& plan, const std::vector<std::wstring>& completed, uint64_t id) {
        // Decision-only mode does not claim RM discovery. A self identity is
        // intentionally refused by EndLockOwner's production protection checks.
        OpsManager::QueueItem retry;
        retry.req = request; retry.seq = id; retry.retry_transfer_plan = plan;
        retry.retry_completed_sources = completed;
        retry.retry_failure_hr = HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION);
        retry.retry_failed_path = plan.empty() ? L"" : plan.back().source;
        manager.lock_retry_ = retry;
        LockOwner self;
        self.pid = GetCurrentProcessId(); self.image_path = L"test-only self identity"; self.can_terminate = true;
        FILETIME exit{}, kernel{}, user{};
        GetProcessTimes(GetCurrentProcess(), &self.start_time, &exit, &kernel, &user);
        manager.SetStatus([&](OpStatus& status) {
            status.active = false; status.phase = OpPhase::Failed; status.task_id = id;
            status.locked_path = retry.retry_failed_path; status.lock_owners = {self};
        });
    }
    static bool DeleteDecisionOnly(const OpRequest& request, const std::wstring& done, const std::wstring& remaining) {
        OpsManager::QueueItem admitted;
        admitted.req = request;
        DeletePlan accepted;
        accepted.targets = {{done, {done}, DeleteDisposition::Permanent, {}},
            {remaining, {remaining}, DeleteDisposition::Permanent, {}}};
        const auto retry = OpsManager::RemainingDeleteRetry(admitted, accepted, {done});
        return retry.req.sources == std::vector<std::wstring>{remaining} && retry.req.delete_targets.size() == 1;
    }
};
}

namespace {
int failures = 0;
void Check(bool value, const char* label) {
    std::printf("[%s] %s\n", value ? "PASS" : "FAIL", label);
    if (!value) ++failures;
}
void Write(const std::filesystem::path& path, const std::string& text) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << text;
}
std::string Read(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
std::wstring Executable() {
    std::wstring path(32768, L'\0');
    const auto size = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    path.resize(size);
    return path;
}
int Hold(int argc, wchar_t** argv) {
    if (argc != 6) return 2;
    HANDLE ready = OpenEventW(EVENT_MODIFY_STATE, FALSE, argv[3]);
    HANDLE stop = OpenEventW(SYNCHRONIZE, FALSE, argv[4]);
    const DWORD sharing = static_cast<DWORD>(_wcstoui64(argv[5], nullptr, 10));
    HANDLE held = CreateFileW(argv[2], GENERIC_READ, sharing, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (!ready || !stop || held == INVALID_HANDLE_VALUE) return 2;
    SetEvent(ready);
    const auto result = WaitForSingleObject(stop, 60000);
    CloseHandle(held); CloseHandle(ready); CloseHandle(stop);
    return result == WAIT_OBJECT_0 ? 0 : 3;
}
class Holder {
public:
    Holder(const std::filesystem::path& image, const std::filesystem::path& file, DWORD sharing = 0) {
        const auto identity = std::to_wstring(GetCurrentProcessId()) + L"_" +
            std::to_wstring(GetTickCount64()) + L"_" + std::to_wstring(++serial_);
        const auto ready_name = L"Local\\PulseRetryReady_" + identity;
        const auto stop_name = L"Local\\PulseRetryStop_" + identity;
        ready_ = CreateEventW(nullptr, TRUE, FALSE, ready_name.c_str());
        stop_ = CreateEventW(nullptr, TRUE, FALSE, stop_name.c_str());
        std::wstring command = L"\"" + image.wstring() + L"\" --hold \"" + file.wstring() +
            L"\" \"" + ready_name + L"\" \"" + stop_name + L"\" " + std::to_wstring(sharing);
        STARTUPINFOW startup{sizeof(startup)};
        launched_ = CreateProcessW(image.c_str(), command.data(), nullptr, nullptr, FALSE,
            CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process_) != FALSE;
        ready_ok_ = launched_ && WaitForSingleObject(ready_, 5000) == WAIT_OBJECT_0;
    }
    ~Holder() {
        Stop();
        if (process_.hThread) CloseHandle(process_.hThread);
        if (process_.hProcess) CloseHandle(process_.hProcess);
        if (ready_) CloseHandle(ready_);
        if (stop_) CloseHandle(stop_);
    }
    bool Ready() const { return ready_ok_; }
    DWORD Pid() const { return process_.dwProcessId; }
    bool Running() const { return process_.hProcess && WaitForSingleObject(process_.hProcess, 0) == WAIT_TIMEOUT; }
    void Stop() {
        if (stop_) SetEvent(stop_);
        if (process_.hProcess) WaitForSingleObject(process_.hProcess, 5000);
    }
private:
    static inline unsigned serial_ = 0;
    HANDLE ready_ = nullptr, stop_ = nullptr;
    PROCESS_INFORMATION process_{};
    bool launched_ = false, ready_ok_ = false;
};
int Decisions() {
    using namespace pulse::ops;
    const auto fixture = std::filesystem::current_path() / L"bench_data" /
        (L"pulse_retry_decisions_" + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(GetTickCount64()));
    const auto source = fixture / L"source", destination = fixture / L"destination";
    const auto subtree = source / L"tree" / L"nested", actual = destination / L"tree" / L"nested - 副本";
    std::filesystem::create_directories(subtree);
    std::filesystem::create_directories(actual);
    Write(subtree / L"A.txt", "new A"); Write(subtree / L"B.txt", "pending B");
    Write(subtree / L"new-unrelated.txt", "not selected"); Write(actual / L"A.txt", "successful A");
    std::vector<TransferPlanEntry> plan;
    for (const auto& pair : {std::pair{source / L"tree", destination / L"tree"},
            std::pair{subtree, actual}, std::pair{subtree / L"A.txt", actual / L"A.txt"},
            std::pair{subtree / L"B.txt", actual / L"B.txt"}}) {
        TransferPlanEntry entry;
        entry.source = pair.first.wstring(); entry.destination = pair.second.wstring();
        entry.directory = std::filesystem::is_directory(pair.first);
        entry.destination_preexisting = entry.directory;
        if (!entry.directory) entry.bytes = std::filesystem::file_size(pair.first);
        plan.push_back(entry);
    }
    OpRequest request;
    request.sources = {(source / L"tree").wstring()}; request.dest_dir = destination.wstring();
    request.collision_policy = CollisionPolicy::KeepBoth;
    {
        OpsManager manager;
        TransferIntegrationProbe::Initialize(manager);
        TransferIntegrationProbe::InjectFailure(manager, request, plan, {(subtree / L"A.txt").wstring()}, 10);
        Check(manager.RetryLockedOperation(10, false) && TransferIntegrationProbe::RunQueued(manager),
            "decision-only plain retry executes the exact retained plan");
        Check(manager.Status().phase == OpPhase::Completed && Read(actual / L"A.txt") == "successful A" &&
            Read(actual / L"B.txt") == "pending B" && !std::filesystem::exists(actual / L"new-unrelated.txt"),
            "decision-only retry preserves nested destination, completed A, and excludes new file");
    }
    for (const bool dequeued : {false, true}) {
        OpsManager manager;
        TransferIntegrationProbe::Initialize(manager);
        TransferIntegrationProbe::InjectFailure(manager, request, plan, {}, 20);
        Check(manager.RetryLockedOperation(20, true), "decision-only queue an end retry containing self-protected identity");
        if (dequeued) {
            Check(TransferIntegrationProbe::RunQueued(manager, true) && manager.Status().phase == OpPhase::Cancelled,
                "decision-only generation revokes dequeue/preparation boundary through real worker path");
        } else {
            manager.CancelCurrent();
            Check(TransferIntegrationProbe::QueueEmpty(manager) && !TransferIntegrationProbe::RunQueued(manager),
                "decision-only cancel removes the queued retry before execution");
        }
        Check(Read(actual / L"A.txt") == "successful A", "decision-only cancellation did not replay successful A");
    }
    {
        OpsManager manager;
        TransferIntegrationProbe::Initialize(manager);
        TransferIntegrationProbe::InjectFailure(manager, request, plan, {(subtree / L"A.txt").wstring()}, 30);
        Check(manager.RetryLockedOperation(30, true) && TransferIntegrationProbe::RunQueued(manager),
            "decision-only protected owner refusal runs on real preparation path");
        const auto failed = manager.Status();
        Check(failed.phase == OpPhase::Failed && manager.IsLockedFailureCurrent(failed.task_id),
            "decision-only termination refusal re-arms plain retry even when RM returns no owners");
        Check(manager.RetryLockedOperation(failed.task_id, false) && TransferIntegrationProbe::RunQueued(manager) &&
            manager.Status().phase == OpPhase::Completed && Read(actual / L"A.txt") == "successful A",
            "decision-only re-armed retry preserves completed filtering");
    }
    const auto replace_source = source / L"replace.txt", replace_target = destination / L"replace.txt";
    Write(replace_source, "new replacement"); Write(replace_target, "old replacement");
    {
        HANDLE held = CreateFileW(replace_target.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        Check(held != INVALID_HANDLE_VALUE, "decision-only local target denies delete sharing");
        OpsManager manager;
        TransferIntegrationProbe::Initialize(manager);
        OpRequest replace;
        replace.sources = {replace_source.wstring()}; replace.dest_dir = destination.wstring();
        replace.collision_policy = CollisionPolicy::Replace;
        TransferIntegrationProbe::Run(manager, replace, 40);
        const auto failed = manager.Status();
        Check(failed.phase == OpPhase::Failed && failed.failure_hr == HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION) &&
            failed.failed_path == replace_target.wstring() && Read(replace_target) == "old replacement",
            "decision-only Replace captures exact target HRESULT/path before temporary cleanup without requiring RM");
        if (held != INVALID_HANDLE_VALUE) CloseHandle(held);
    }
    const auto changed_source = source / L"changed.txt", changed_target = destination / L"changed.txt";
    Write(changed_source, "planned file");
    TransferPlanEntry changed;
    changed.source = changed_source.wstring(); changed.destination = changed_target.wstring();
    DeleteFileW(changed_source.c_str());
    std::filesystem::create_directory(changed_source);
    Write(changed_source / L"unrelated-child.txt", "must remain outside operation");
    {
        OpsManager manager;
        TransferIntegrationProbe::Initialize(manager);
        OpRequest move;
        move.type = OpType::Move; move.sources = {changed_source.wstring()}; move.dest_dir = destination.wstring();
        TransferIntegrationProbe::InjectFailure(manager, move, {changed}, {}, 50);
        Check(manager.RetryLockedOperation(50, false) && TransferIntegrationProbe::RunQueued(manager) &&
            manager.Status().phase == OpPhase::Failed && manager.Status().failure_hr == HRESULT_FROM_WIN32(ERROR_INVALID_DATA) &&
            std::filesystem::exists(changed_source / L"unrelated-child.txt") && !std::filesystem::exists(changed_target),
            "decision-only retry rejects file-to-directory change rather than fast-moving an unrelated subtree");
    }
    OpRequest deletion;
    deletion.type = OpType::RealDelete;
    deletion.sources = {(subtree / L"A.txt").wstring(), (subtree / L"B.txt").wstring()};
    Check(TransferIntegrationProbe::DeleteDecisionOnly(deletion, deletion.sources[0], deletion.sources[1]),
        "decision-only production delete remaining filter removes successful root A");
    const auto cleanup = std::filesystem::absolute(fixture).lexically_normal();
    Check(cleanup.parent_path() == std::filesystem::current_path() / L"bench_data" &&
        cleanup.filename() == fixture.filename(), "verify decision fixture cleanup target");
    if (cleanup.parent_path() == std::filesystem::current_path() / L"bench_data" &&
        cleanup.filename() == fixture.filename()) std::filesystem::remove_all(cleanup);
    return failures ? 1 : 0;
}
}

int wmain(int argc, wchar_t** argv) {
    using namespace pulse::ops;
    if (argc > 1 && std::wstring(argv[1]) == L"--hold") return Hold(argc, argv);
    if (argc == 2 && std::wstring(argv[1]) == L"--decisions") return Decisions();
    const auto fixture = std::filesystem::current_path() / L"bench_data" /
        (L"pulse_retry_fixture_" + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(GetTickCount64()));
    const auto source = fixture / L"source";
    const auto destination = fixture / L"destination";
    std::filesystem::create_directories(source);
    std::filesystem::create_directories(destination);
    const auto a = source / L"A.txt", b = source / L"B.txt";
    Write(a, "original A"); Write(b, "original B");
    HANDLE held = CreateFileW(b.c_str(), GENERIC_WRITE, FILE_SHARE_WRITE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    Check(held != INVALID_HANDLE_VALUE, "hold source B without read sharing");
    {
        OpsManager manager;
        TransferIntegrationProbe::Initialize(manager);
        OpRequest request;
        request.type = OpType::Copy;
        request.sources = {a.wstring(), b.wstring()}; request.dest_dir = destination.wstring();
        TransferIntegrationProbe::Run(manager, request, 100);
        auto failed = manager.Status();
        Check(failed.phase == OpPhase::Failed && !failed.lock_owners.empty() &&
            Read(destination / L"A.txt") == "original A", "copy A completes before locked B fails");
        Check(!manager.RetryLockedOperation(99, false), "stale task IDs cannot queue a retry");
        Check(!manager.RetryLockedOperation(100, true), "Pulse itself is never terminated by a retry");
        CloseHandle(held); held = INVALID_HANDLE_VALUE;
        Write(a, "new source A after failure");
        Check(manager.RetryLockedOperation(100, false) && TransferIntegrationProbe::RunQueued(manager),
            "plain retry queues the failed operation after the lock closes");
        Check(manager.Status().phase == OpPhase::Completed && Read(destination / L"A.txt") == "original A" &&
            Read(destination / L"B.txt") == "original B", "copy retry processes B without copying successful A again");
        Check(!manager.RetryLockedOperation(100, false), "completed retry invalidates the previous failed task");
    }
    DeleteFileW((destination / L"A.txt").c_str()); DeleteFileW((destination / L"B.txt").c_str());
    Write(a, "move A");
    held = CreateFileW(b.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    {
        OpsManager manager;
        TransferIntegrationProbe::Initialize(manager);
        OpRequest request;
        request.type = OpType::Move;
        request.sources = {a.wstring(), b.wstring()}; request.dest_dir = destination.wstring();
        TransferIntegrationProbe::Run(manager, request, 200);
        Check(manager.Status().phase == OpPhase::Failed && !std::filesystem::exists(a) &&
            std::filesystem::exists(b), "move A completes before B's missing delete share fails");
        CloseHandle(held); held = INVALID_HANDLE_VALUE;
        Check(manager.RetryLockedOperation(200, false) && TransferIntegrationProbe::RunQueued(manager),
            "partial move retries only its remaining work");
        Check(manager.Status().phase == OpPhase::Completed && Read(destination / L"A.txt") == "move A" &&
            !std::filesystem::exists(b), "move retry does not fail on the already-moved A");
    }
    if (held != INVALID_HANDLE_VALUE) CloseHandle(held);

    const auto holder_image = fixture / L"FixtureHolder.exe";
    std::filesystem::copy_file(Executable(), holder_image);
    const auto replace_source = source / L"replace.txt";
    const auto replace_target = destination / L"replace.txt";
    Write(replace_source, "replacement"); Write(replace_target, "old target");
    {
        Holder holder(holder_image, replace_target, FILE_SHARE_READ | FILE_SHARE_WRITE);
        Check(holder.Ready(), "destination holder is ready without delete sharing");
        OpsManager manager;
        TransferIntegrationProbe::Initialize(manager);
        OpRequest request;
        request.sources = {replace_source.wstring()}; request.dest_dir = destination.wstring();
        request.collision_policy = CollisionPolicy::Replace;
        TransferIntegrationProbe::Run(manager, request, 300);
        const auto failed = manager.Status();
        Check(failed.phase == OpPhase::Failed && IsLockFailure(failed.failure_hr) &&
            failed.failed_path == replace_target.wstring() && failed.locked_path == replace_target.wstring() &&
            std::any_of(failed.lock_owners.begin(), failed.lock_owners.end(), [&](const auto& owner) {
                return owner.pid == holder.Pid();
            }), "Replace PinLeaf preserves target HRESULT/path and finds target owner");
        Check(TransferIntegrationProbe::QueueFixtureEnd(manager, 300, {holder.Pid()}), "fixture-only target owner can queue end retry");
        manager.CancelCurrent();
        Check(TransferIntegrationProbe::QueueEmpty(manager) && !TransferIntegrationProbe::RunQueued(manager) &&
            holder.Running() && Read(replace_target) == "old target",
            "cancel revokes queued retry without killing or replacing anything");
    }
    {
        Holder holder(holder_image, replace_target, FILE_SHARE_READ | FILE_SHARE_WRITE);
        OpsManager manager;
        TransferIntegrationProbe::Initialize(manager);
        OpRequest request;
        request.sources = {replace_source.wstring()}; request.dest_dir = destination.wstring();
        request.collision_policy = CollisionPolicy::Replace;
        TransferIntegrationProbe::Run(manager, request, 310);
        Check(holder.Ready() && TransferIntegrationProbe::QueueFixtureEnd(manager, 310, {holder.Pid()}) &&
            TransferIntegrationProbe::RunQueued(manager, true), "cancel fixture at dequeue/preparation boundary");
        Check(manager.Status().phase == OpPhase::Cancelled && holder.Running() &&
            Read(replace_target) == "old target", "generation check uses real worker decision: no kill/no transfer");
    }
    {
        Holder holder(holder_image, replace_target, FILE_SHARE_READ | FILE_SHARE_WRITE);
        OpsManager manager;
        TransferIntegrationProbe::Initialize(manager);
        OpRequest request;
        request.sources = {replace_source.wstring()}; request.dest_dir = destination.wstring();
        request.collision_policy = CollisionPolicy::Replace;
        TransferIntegrationProbe::Run(manager, request, 320);
        holder.Stop();
        Check(manager.RetryLockedOperation(320, false) && TransferIntegrationProbe::RunQueued(manager) &&
            manager.Status().phase == OpPhase::Completed && Read(replace_target) == "replacement",
            "plain retry safely replaces target after its owner closes");
    }

    const auto tree = source / L"tree", nested = tree / L"nested";
    const auto target_tree = destination / L"tree";
    std::filesystem::create_directories(nested);
    std::filesystem::create_directories(target_tree);
    Write(nested / L"A.txt", "nested A"); Write(nested / L"B.txt", "nested B");
    Write(target_tree / L"nested", "existing file blocks directory name");
    {
        Holder holder(holder_image, nested / L"B.txt");
        OpsManager manager;
        TransferIntegrationProbe::Initialize(manager);
        OpRequest request;
        request.sources = {tree.wstring()}; request.dest_dir = destination.wstring();
        request.collision_policy = CollisionPolicy::KeepBoth;
        TransferIntegrationProbe::Run(manager, request, 400);
        const auto actual = target_tree / L"nested - 副本";
        const auto failed = manager.Status();
        Check(holder.Ready() && failed.phase == OpPhase::Failed && Read(actual / L"A.txt") == "nested A",
            "nested KeepBoth commits A to remapped subtree before locked B");
        Write(nested / L"A.txt", "changed after failure");
        Write(nested / L"unrelated.txt", "not in original plan");
        holder.Stop();
        Check(manager.RetryLockedOperation(400, false) && TransferIntegrationProbe::RunQueued(manager) &&
            manager.Status().phase == OpPhase::Completed && Read(actual / L"A.txt") == "nested A" &&
            Read(actual / L"B.txt") == "nested B" && !std::filesystem::exists(actual / L"unrelated.txt") &&
            !std::filesystem::exists(target_tree / L"nested - 副本 (2)"),
            "retry preserves actual nested target, skips A, and never rescans new files");
    }

    const auto shared = source / L"multi-owner.txt";
    Write(shared, "shared by fixture holders");
    {
        Holder first(holder_image, shared, FILE_SHARE_READ), second(holder_image, shared, FILE_SHARE_READ);
        OpsManager manager;
        TransferIntegrationProbe::Initialize(manager);
        OpRequest request;
        request.type = OpType::Move; request.sources = {shared.wstring()}; request.dest_dir = destination.wstring();
        TransferIntegrationProbe::Run(manager, request, 500);
        Check(first.Ready() && second.Ready() && manager.Status().lock_owners.size() == 2 &&
            TransferIntegrationProbe::QueueFixtureEnd(manager, 500, {first.Pid(), second.Pid()}), "multi-owner retry contains only two controlled holder identities");
        const DWORD remaining_pid = TransferIntegrationProbe::RefuseSecondQueuedOwner(manager);
        Check(remaining_pid != 0 && TransferIntegrationProbe::RunQueued(manager), "inject stale image for second fixture owner");
        const auto failed = manager.Status();
        Check(failed.phase == OpPhase::Failed && manager.IsLockedFailureCurrent(failed.task_id) &&
            failed.lock_owners.size() == 1 && failed.lock_owners.front().pid == remaining_pid &&
            ((first.Running() && !second.Running()) || (!first.Running() && second.Running())) &&
            std::filesystem::exists(shared), "partial end failure recaptures remaining owner and re-arms plan without transfer");
        first.Stop(); second.Stop();
        Check(manager.RetryLockedOperation(failed.task_id, false) && TransferIntegrationProbe::RunQueued(manager) &&
            manager.Status().phase == OpPhase::Completed && !std::filesystem::exists(shared),
            "plain retry after partial termination failure keeps remaining move plan");
    }
    const auto deleted_a = source / L"deleted-A.txt", delete_b = source / L"delete-B.txt";
    Write(delete_b, "remaining selected root");
    {
        Holder holder(holder_image, delete_b);
        OpsManager manager;
        TransferIntegrationProbe::Initialize(manager);
        OpRequest request;
        request.type = OpType::RealDelete; request.sources = {deleted_a.wstring(), delete_b.wstring()};
        Check(holder.Ready() && TransferIntegrationProbe::DeleteRemaining(manager, request,
            deleted_a.wstring(), delete_b.wstring(), 600) && manager.RetryLockedOperation(600, false) &&
            TransferIntegrationProbe::QueuedDeleteIsOnly(manager, delete_b.wstring()),
            "delete retry decision preserves only authorized remaining B, never successful A");
        manager.CancelCurrent();
        Check(TransferIntegrationProbe::QueueEmpty(manager) && holder.Running(), "cancel delete retry does not touch fixture holder");
    }
    // All holders have exited; this is the unique absolute directory created by
    // this test, not a user-selected path or the surrounding bench_data tree.
    const auto cleanup = std::filesystem::absolute(fixture).lexically_normal();
    Check(cleanup.parent_path() == std::filesystem::current_path() / L"bench_data" &&
        cleanup.filename() == fixture.filename(), "verify fixture-only recursive cleanup target");
    if (cleanup.parent_path() == std::filesystem::current_path() / L"bench_data" &&
        cleanup.filename() == fixture.filename()) std::filesystem::remove_all(cleanup);
    return failures ? 1 : 0;
}
