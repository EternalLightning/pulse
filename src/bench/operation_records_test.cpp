// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "../ops/ops_manager.h"
#include <algorithm>
#include <cstdio>
#include <cstring>

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
}

int main(int argc, char** argv) {
    if (argc > 2 || (argc == 2 && std::strcmp(argv[1], "--task-status") != 0)) {
        std::fprintf(stderr, "Unsupported test filter. Use --task-status or no arguments.\n");
        return 2;
    }
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
    std::printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
