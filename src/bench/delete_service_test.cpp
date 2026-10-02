#include "../ops/delete_service.h"

#include <array>
#include <atomic>
#include <barrier>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace pulse::ops;

void Check(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

// All paths and dispositions are synthetic. No filesystem proof or side effect occurs.
DeleteTarget MakeTarget(std::wstring path, DeleteDisposition disposition) {
    return { path, { path }, disposition, L"Mock disposition, not a filesystem guarantee." };
}

DeletePlan MakePlan(std::vector<DeleteTarget> targets,
                    DeleteOrigin origin = DeleteOrigin::Selection,
                    uint64_t task_id = 42) {
    return { task_id, origin, std::move(targets) };
}

DeletePlan SinglePlan(DeleteDisposition disposition) {
    return MakePlan({ MakeTarget(L"C:\\mock\\item.txt", disposition) });
}

bool SamePlan(const DeletePlan& left, const DeletePlan& right) {
    if (left.task_id != right.task_id || left.origin != right.origin ||
        left.targets.size() != right.targets.size()) {
        return false;
    }
    for (size_t index = 0; index < left.targets.size(); ++index) {
        const auto& a = left.targets[index];
        const auto& b = right.targets[index];
        if (a.path != b.path || a.physical_paths != b.physical_paths ||
            a.disposition != b.disposition || a.reason != b.reason) {
            return false;
        }
    }
    return true;
}

void CheckBlocked(DeleteService& service, DeletePlan plan) {
    const auto prepared = service.Prepare(std::move(plan));
    Check(prepared.decision == DeleteDecision::Blocked, "plan must be blocked");
    Check(prepared.token == 0, "blocked preparation must not issue a token");
    Check(!prepared.reason.empty(), "blocked preparation must explain why");
    Check(!service.Pending(), "blocked preparation must not expose confirmation");
    Check(!service.Resolve(0, true), "zero token must not resolve");
    Check(!service.Resolve(1, true), "blocked plan must not be confirmable");
    Check(!service.TakeAccepted(0), "zero token must not admit a plan");
    Check(!service.TakeAccepted(1), "blocked plan must not admit a plan");
    Check(!service.WasRejected(0), "zero token must not be a rejection record");
}

void RecycleIntentIsNotProof() {
    DeleteService service;
    const auto plan = SinglePlan(DeleteDisposition::RecycleRequested);
    const auto prepared = service.Prepare(plan);
    Check(prepared.decision == DeleteDecision::Admitted && !service.Pending(), "ordinary recycle intent must not be blocked as unknown");
    const auto accepted = service.TakeAccepted(prepared.token);
    Check(accepted && SamePlan(*accepted, plan), "Shell-warning intent is retained without claiming positive recycle proof");
    Check(!service.TakeAccepted(prepared.token), "recycle request remains one-shot");
    const auto mixed = service.Prepare(MakePlan({MakeTarget(L"C:\\mock\\a", DeleteDisposition::RecycleRequested),
        MakeTarget(L"C:\\mock\\b", DeleteDisposition::Permanent)}));
    Check(mixed.decision == DeleteDecision::AwaitingConfirmation, "explicit permanent roots still require Pulse confirmation");
}

void PermanentRejectsByDefault() {
    DeleteService service;
    const auto plan = MakePlan({
        MakeTarget(L"C:\\mock\\first.txt", DeleteDisposition::Permanent),
        MakeTarget(L"C:\\mock\\second.txt", DeleteDisposition::Permanent)
    });
    const auto prepared = service.Prepare(plan);
    Check(prepared.decision == DeleteDecision::AwaitingConfirmation,
          "all-permanent plan must require explicit confirmation");
    Check(prepared.token != 0, "confirmation must have a nonzero token");
    Check(!service.TakeAccepted(prepared.token), "pending plan is not admitted by default");
    const auto pending = service.Pending();
    Check(pending && pending->token == prepared.token && SamePlan(pending->plan, plan),
          "confirmation must retain the whole permanent plan");
    Check(service.Resolve(prepared.token, false), "current pending rejection must resolve");
    Check(service.WasRejected(prepared.token), "worker must observe rejection");
    Check(!service.Pending(), "rejected plan must stop being pending");
    Check(!service.Resolve(prepared.token, true), "rejection must not be reversed by replay");
    Check(!service.Resolve(prepared.token, false), "rejection must not resolve twice");
    Check(!service.TakeAccepted(prepared.token), "rejected plan must not be admitted");
}

void PermanentAcceptsExplicitly() {
    DeleteService service;
    const auto plan = MakePlan({
        MakeTarget(L"C:\\mock\\first.txt", DeleteDisposition::Permanent),
        MakeTarget(L"C:\\mock\\second.txt", DeleteDisposition::Permanent)
    }, DeleteOrigin::Preview, 1234);
    const auto prepared = service.Prepare(plan);
    Check(prepared.decision == DeleteDecision::AwaitingConfirmation, "confirmation required");
    Check(service.Resolve(prepared.token, true), "explicit acceptance must resolve");
    Check(!service.Pending(), "resolved plan must stop being pending");
    Check(!service.WasRejected(prepared.token), "accepted plan is not rejected");
    Check(!service.Resolve(prepared.token, true), "acceptance must not replay");
    Check(!service.Resolve(prepared.token, false), "resolved token cannot be reversed");
    const auto accepted = service.TakeAccepted(prepared.token);
    Check(accepted && SamePlan(*accepted, plan), "accepted snapshot must match the exact plan");
    Check(!service.TakeAccepted(prepared.token), "accepted snapshot must be consumed once");
}

void UnknownAlwaysBlocks() {
    DeleteService service;
    const auto plan = MakePlan({
        MakeTarget(L"C:\\mock\\first.txt", DeleteDisposition::Unknown),
        MakeTarget(L"C:\\mock\\second.txt", DeleteDisposition::Unknown)
    });
    CheckBlocked(service, plan);

    DeleteTarget default_target;
    default_target.path = L"C:\\mock\\default-unknown.txt";
    default_target.physical_paths = { default_target.path };
    Check(default_target.disposition == DeleteDisposition::Unknown,
          "default disposition must be unknown");
    CheckBlocked(service, MakePlan({ default_target }));
}

void MixedUnknownHasPriority() {
    for (size_t unknown_index = 0; unknown_index < 3; ++unknown_index) {
        DeleteService service;
        auto plan = MakePlan({
            MakeTarget(L"C:\\mock\\first.txt", DeleteDisposition::Permanent),
            MakeTarget(L"C:\\mock\\second.txt", DeleteDisposition::Recyclable),
            MakeTarget(L"C:\\mock\\third.txt", DeleteDisposition::Permanent)
        });
        plan.targets[unknown_index].disposition = DeleteDisposition::Unknown;
        plan.targets[unknown_index].reason = L"Mock: positive recyclability proof unavailable.";
        const auto prepared = service.Prepare(plan);
        Check(prepared.decision == DeleteDecision::Blocked && prepared.token == 0,
              "any unknown must block the whole mixed batch");
        Check(prepared.reason == plan.targets[unknown_index].reason,
              "unknown reason must win over a permanent confirmation");
        Check(!service.Pending() && !service.TakeAccepted(prepared.token),
              "mixed unknown must not expose confirmation or admission");
    }

    DeleteService service;
    auto invalid = MakeTarget(L"", DeleteDisposition::Permanent);
    auto unknown = MakeTarget(L"C:\\mock\\unknown.txt", DeleteDisposition::Unknown);
    unknown.reason = L"Mock unknown has priority even after an invalid target.";
    const auto prepared = service.Prepare(MakePlan({ invalid, unknown }));
    Check(prepared.decision == DeleteDecision::Blocked && prepared.token == 0 &&
          prepared.reason == unknown.reason, "validation must not hide a later unknown");
    Check(!service.Pending(), "unknown/invalid mixed plan must not be pending");
}

void MixedKnownRetainsWholePlan() {
    DeleteService service;
    auto first = MakeTarget(L"C:\\mock\\recyclable.txt", DeleteDisposition::Recyclable);
    first.physical_paths.push_back(L"C:\\mock\\recyclable-sidecar.txt");
    const auto plan = MakePlan({
        first,
        MakeTarget(L"C:\\mock\\permanent.txt", DeleteDisposition::Permanent),
        MakeTarget(L"C:\\mock\\last.txt", DeleteDisposition::Recyclable)
    }, DeleteOrigin::Duplicates, 9999);
    const auto prepared = service.Prepare(plan);
    Check(prepared.decision == DeleteDecision::AwaitingConfirmation,
          "known mixed batch must require permanent confirmation");
    const auto pending = service.Pending();
    Check(pending && SamePlan(pending->plan, plan), "mixed confirmation must preserve all targets");
    Check(!service.TakeAccepted(prepared.token), "mixed batch cannot admit a recyclable subset");
    Check(service.Resolve(prepared.token, true), "mixed confirmation must resolve");
    const auto accepted = service.TakeAccepted(prepared.token);
    Check(accepted && SamePlan(*accepted, plan), "mixed accepted snapshot must retain the full batch");

    const auto second = service.Prepare(plan);
    Check(service.Resolve(second.token, false), "mixed batch rejection must resolve");
    Check(!service.TakeAccepted(second.token), "mixed rejection cannot admit a recyclable subset");
}

void MockRecyclableAdmitsOnlySnapshot() {
    DeleteService service;
    const auto plan = MakePlan({
        MakeTarget(L"C:\\mock\\first.txt", DeleteDisposition::Recyclable),
        MakeTarget(L"C:\\mock\\second.txt", DeleteDisposition::Recyclable)
    }, DeleteOrigin::Selection, 0);
    const auto prepared = service.Prepare(plan);
    Check(prepared.decision == DeleteDecision::Admitted && prepared.token != 0,
          "all mock recyclable targets should be admitted with a token");
    Check(!service.Pending(), "mock recyclable admission does not need confirmation");
    Check(!service.Resolve(prepared.token, true) && !service.Resolve(prepared.token, false),
          "already admitted plan must not accept confirmation calls");
    const auto accepted = service.TakeAccepted(prepared.token);
    Check(accepted && SamePlan(*accepted, plan), "mock recyclable admission must supply exact snapshot");
    Check(!service.TakeAccepted(prepared.token), "mock recyclable snapshot must be consumed once");
    Check(!service.WasRejected(prepared.token), "consumed plan is not a rejection");
}

void StaleReplayedAndMismatchedTokens() {
    DeleteService service;
    const auto first = service.Prepare(SinglePlan(DeleteDisposition::Permanent));
    const auto second = service.Prepare(SinglePlan(DeleteDisposition::Permanent));
    Check(first.token != 0 && second.token > first.token, "new preparation must have a new token");
    Check(!service.Resolve(0, true) && !service.Resolve(first.token, true) &&
          !service.Resolve(second.token + 1, true), "zero/stale/mismatched resolve must be ineffective");
    Check(!service.Resolve(first.token, false) && !service.WasRejected(first.token),
          "stale rejection must not change current state");
    Check(!service.TakeAccepted(0) && !service.TakeAccepted(first.token) &&
          !service.TakeAccepted(second.token + 1), "invalid tokens must not consume plans");
    const auto pending = service.Pending();
    Check(pending && pending->token == second.token, "invalid tokens must leave current pending intact");
    Check(service.Resolve(second.token, true), "correct current token must still resolve");
    Check(!service.Resolve(second.token, true), "resolved token must not replay");
    Check(!service.TakeAccepted(first.token) && !service.TakeAccepted(second.token + 1),
          "invalid take must not consume current acceptance");
    Check(service.TakeAccepted(second.token).has_value(), "correct token must take acceptance");
    Check(!service.TakeAccepted(second.token), "consumed token must not replay");

    const auto third = service.Prepare(SinglePlan(DeleteDisposition::Recyclable));
    const auto fourth = service.Prepare(SinglePlan(DeleteDisposition::Permanent));
    Check(third.token > second.token && fourth.token > third.token, "tokens must not be reused");
    Check(!service.TakeAccepted(third.token), "new preparation must invalidate untaken admission");
    Check(!service.Resolve(third.token, false), "old admission must not resolve against new pending");
    Check(service.Resolve(fourth.token, false) && service.WasRejected(fourth.token),
          "new current token must remain independently rejectable");
}

void PendingSnapshotIsolation() {
    DeleteService service;
    auto input = MakePlan({
        MakeTarget(L"C:\\mock\\first.txt", DeleteDisposition::Permanent),
        MakeTarget(L"C:\\mock\\second.txt", DeleteDisposition::Recyclable)
    }, DeleteOrigin::Recovery, 8080);
    const auto expected = input;
    const auto prepared = service.Prepare(input);
    input.task_id = 1;
    input.origin = DeleteOrigin::Selection;
    input.targets.clear();

    auto snapshot = service.Pending();
    Check(snapshot && SamePlan(snapshot->plan, expected), "Prepare must own an independent plan");
    snapshot->token = 0;
    snapshot->plan.task_id = 2;
    snapshot->plan.origin = DeleteOrigin::Undo;
    snapshot->plan.targets[0].path.clear();
    snapshot->plan.targets[0].physical_paths.clear();
    snapshot->plan.targets[0].disposition = DeleteDisposition::Unknown;
    snapshot->plan.targets[0].reason = L"External mutation";
    snapshot->plan.targets.pop_back();
    const auto later = service.Pending();
    Check(later && later->token == prepared.token && SamePlan(later->plan, expected),
          "modifying Pending copy must not mutate token or retained plan");
    Check(service.Resolve(prepared.token, true), "original pending token must still resolve");
    const auto accepted = service.TakeAccepted(prepared.token);
    Check(accepted && SamePlan(*accepted, expected), "accepted plan must ignore all external mutation");
}

void CancelPendingAndUntakenAdmission() {
    DeleteService service;
    service.Cancel();
    Check(!service.Pending() && !service.WasRejected(0), "canceling empty service must be harmless");

    const auto pending = service.Prepare(SinglePlan(DeleteDisposition::Permanent));
    service.Cancel();
    service.Cancel();
    Check(service.WasRejected(pending.token), "worker must observe pending cancellation");
    Check(!service.Pending() && !service.Resolve(pending.token, true) &&
          !service.Resolve(pending.token, false) && !service.TakeAccepted(pending.token),
          "canceled pending must not resolve or admit");

    const auto direct = service.Prepare(SinglePlan(DeleteDisposition::Recyclable));
    Check(!service.WasRejected(pending.token), "new preparation must invalidate cancellation record");
    service.Cancel();
    Check(service.WasRejected(direct.token) && !service.TakeAccepted(direct.token),
          "cancel must revoke untaken direct admission");

    const auto confirmed = service.Prepare(SinglePlan(DeleteDisposition::Permanent));
    Check(service.Resolve(confirmed.token, true), "pending acceptance must resolve");
    service.Cancel();
    Check(service.WasRejected(confirmed.token) && !service.TakeAccepted(confirmed.token),
          "cancel must revoke untaken confirmed admission");

    const auto consumed = service.Prepare(SinglePlan(DeleteDisposition::Recyclable));
    Check(service.TakeAccepted(consumed.token).has_value(), "snapshot must be taken before final cancel");
    service.Cancel();
    Check(!service.WasRejected(consumed.token) && !service.TakeAccepted(consumed.token),
          "cancel cannot revoke or reissue a consumed snapshot");
}

void RejectionRecordHasCurrentLifetime() {
    DeleteService service;
    const auto first = service.Prepare(SinglePlan(DeleteDisposition::Permanent));
    Check(service.Resolve(first.token, false), "first rejection must resolve");
    service.Cancel();
    Check(service.WasRejected(first.token), "Cancel must preserve current explicit rejection");
    Check(!service.WasRejected(first.token + 1) && !service.WasRejected(0),
          "rejection query must require exact nonzero token");
    const auto second = service.Prepare(SinglePlan(DeleteDisposition::Permanent));
    Check(!service.WasRejected(first.token) && !service.WasRejected(second.token),
          "new pending lifecycle must discard old rejection");
    service.Cancel();
    Check(service.WasRejected(second.token), "current cancellation must be queryable");
    CheckBlocked(service, SinglePlan(DeleteDisposition::Unknown));
    Check(!service.WasRejected(second.token), "blocked Prepare must invalidate rejection lifecycle");
}

void BlockedPrepareInvalidatesOldTokens() {
    DeleteService service;
    const auto pending = service.Prepare(SinglePlan(DeleteDisposition::Permanent));
    CheckBlocked(service, MakePlan({}));
    Check(!service.Resolve(pending.token, true) && !service.TakeAccepted(pending.token),
          "invalid Prepare must invalidate old pending token");

    const auto accepted = service.Prepare(SinglePlan(DeleteDisposition::Recyclable));
    CheckBlocked(service, SinglePlan(DeleteDisposition::Unknown));
    Check(!service.TakeAccepted(accepted.token), "unknown Prepare must invalidate untaken acceptance");

    const auto next = service.Prepare(SinglePlan(DeleteDisposition::Permanent));
    Check(next.token > accepted.token, "blocked preparation must never cause token reuse");
    Check(service.Resolve(next.token, true) && service.TakeAccepted(next.token).has_value(),
          "valid preparation after blocked plans must remain functional");
}

void InvalidPlansBlockWholeBatch() {
    DeleteService service;
    CheckBlocked(service, MakePlan({}));
    for (const auto disposition : { DeleteDisposition::Permanent, DeleteDisposition::Recyclable }) {
        for (size_t invalid_index = 0; invalid_index < 2; ++invalid_index) {
            for (int invalid_kind = 0; invalid_kind < 4; ++invalid_kind) {
                auto plan = MakePlan({
                    MakeTarget(L"C:\\mock\\first.txt", disposition),
                    MakeTarget(L"C:\\mock\\second.txt", disposition)
                });
                auto& target = plan.targets[invalid_index];
                switch (invalid_kind) {
                case 0:
                    target.path.clear();
                    break;
                case 1:
                    target.physical_paths.clear();
                    break;
                case 2:
                    target.physical_paths.insert(target.physical_paths.begin(), L"");
                    break;
                default:
                    target.physical_paths.push_back(L"");
                    break;
                }
                CheckBlocked(service, std::move(plan));
            }
        }
    }

    auto invalid_disposition = SinglePlan(DeleteDisposition::Permanent);
    invalid_disposition.targets[0].disposition = static_cast<DeleteDisposition>(-1);
    CheckBlocked(service, std::move(invalid_disposition));
}

void LongAndUncPathsAreRetained() {
    DeleteService service;
    std::wstring long_path = L"\\\\?\\C:\\mock\\";
    long_path.append(12000, L'x');
    long_path += L".txt";
    const auto plan = MakePlan({
        MakeTarget(long_path, DeleteDisposition::Permanent),
        MakeTarget(L"\\\\mock-server\\mock-share\\\u6587\u4ef6.txt", DeleteDisposition::Recyclable),
        MakeTarget(L"\\\\?\\UNC\\mock-server\\mock-share\\extended.txt", DeleteDisposition::Permanent)
    }, DeleteOrigin::Preview, 7000);
    const auto prepared = service.Prepare(plan);
    Check(prepared.decision == DeleteDecision::AwaitingConfirmation,
          "nonempty long/UNC paths must be structurally valid");
    const auto pending = service.Pending();
    Check(pending && SamePlan(pending->plan, plan), "long/UNC confirmation must preserve exact paths");
    Check(service.Resolve(prepared.token, true), "mock long/UNC plan must confirm");
    const auto accepted = service.TakeAccepted(prepared.token);
    Check(accepted && SamePlan(*accepted, plan), "long/UNC accepted paths must not normalize or truncate");
}

void RecyclePairCountsAsOneLogicalTarget() {
    DeleteService service;
    const DeleteTarget pair {
        L"C:\\mock-original\\restorable.txt",
        {
            L"C:\\$Recycle.Bin\\S-1-5-21-mock\\$Rmock.txt",
            L"C:\\$Recycle.Bin\\S-1-5-21-mock\\$Imock.txt"
        },
        DeleteDisposition::Permanent,
        L"Mock EmptyRecycle permanent pair. No real recycle bin is accessed."
    };
    const auto plan = MakePlan({ pair }, DeleteOrigin::EmptyRecycle, 500);
    const auto prepared = service.Prepare(plan);
    Check(prepared.decision == DeleteDecision::AwaitingConfirmation, "mock bin pair must confirm");
    const auto pending = service.Pending();
    Check(pending && pending->plan.targets.size() == 1 &&
          pending->plan.targets[0].physical_paths.size() == 2 && SamePlan(pending->plan, plan),
          "$R/$I pair must remain one logical target with both physical paths");
    Check(service.Resolve(prepared.token, true), "mock bin pair must resolve");
    const auto accepted = service.TakeAccepted(prepared.token);
    Check(accepted && accepted->targets.size() == 1 && SamePlan(*accepted, plan),
          "accepted bin pair must not flatten into two logical targets");
}

void AllOriginsUseSameGate() {
    constexpr std::array origins {
        DeleteOrigin::Selection, DeleteOrigin::Preview, DeleteOrigin::Duplicates,
        DeleteOrigin::Undo, DeleteOrigin::Recovery, DeleteOrigin::EmptyRecycle
    };
    for (const auto origin : origins) {
        DeleteService service;
        auto plan = SinglePlan(DeleteDisposition::Unknown);
        plan.origin = origin;
        CheckBlocked(service, plan);
        plan.targets[0].disposition = DeleteDisposition::Permanent;
        const auto permanent = service.Prepare(plan);
        Check(permanent.decision == DeleteDecision::AwaitingConfirmation,
              "no Pulse origin may bypass permanent confirmation");
        Check(service.Resolve(permanent.token, false) && !service.TakeAccepted(permanent.token),
              "no Pulse origin may bypass rejection");
        plan.targets[0].disposition = DeleteDisposition::Recyclable;
        const auto recyclable = service.Prepare(plan);
        Check(recyclable.decision == DeleteDecision::Admitted, "mock positive origin case must admit");
        const auto accepted = service.TakeAccepted(recyclable.token);
        Check(accepted && SamePlan(*accepted, plan), "every origin must retain its exact snapshot");
    }
}

void ConcurrentTakesConsumeOnce() {
    for (int iteration = 0; iteration < 32; ++iteration) {
        DeleteService service;
        const auto plan = SinglePlan(DeleteDisposition::Recyclable);
        const auto prepared = service.Prepare(plan);
        std::atomic<unsigned> admissions = 0;
        std::atomic<unsigned> corrupt_snapshots = 0;
        std::barrier start(9);
        {
            std::array<std::jthread, 8> workers;
            for (auto& worker : workers) {
                worker = std::jthread([&] {
                    start.arrive_and_wait();
                    const auto accepted = service.TakeAccepted(prepared.token);
                    if (accepted) {
                        admissions.fetch_add(1, std::memory_order_relaxed);
                        if (!SamePlan(*accepted, plan)) {
                            corrupt_snapshots.fetch_add(1, std::memory_order_relaxed);
                        }
                    }
                });
            }
            start.arrive_and_wait();
        }
        Check(admissions.load() == 1, "concurrent consumers must admit exactly once");
        Check(corrupt_snapshots.load() == 0, "concurrent take must preserve exact snapshot");
        Check(!service.TakeAccepted(prepared.token), "raced token must stay consumed");
    }
}

void ConcurrentResolveCancelNeverDoubleAdmits() {
    for (int iteration = 0; iteration < 128; ++iteration) {
        DeleteService service;
        const auto plan = SinglePlan(DeleteDisposition::Permanent);
        const auto prepared = service.Prepare(plan);
        std::atomic<unsigned> resolutions = 0;
        std::atomic<unsigned> admissions = 0;
        std::atomic<unsigned> corrupt_snapshots = 0;
        std::barrier start(6);
        auto resolve = [&] {
            start.arrive_and_wait();
            if (service.Resolve(prepared.token, true)) {
                resolutions.fetch_add(1, std::memory_order_relaxed);
            }
        };
        auto take = [&] {
            start.arrive_and_wait();
            const auto accepted = service.TakeAccepted(prepared.token);
            if (accepted) {
                admissions.fetch_add(1, std::memory_order_relaxed);
                if (!SamePlan(*accepted, plan)) {
                    corrupt_snapshots.fetch_add(1, std::memory_order_relaxed);
                }
            }
        };
        {
            std::jthread first_resolve(resolve);
            std::jthread second_resolve(resolve);
            std::jthread first_take(take);
            std::jthread second_take(take);
            std::jthread cancel([&] {
                start.arrive_and_wait();
                service.Cancel();
            });
            start.arrive_and_wait();
        }
        Check(resolutions.load() <= 1, "racing Resolve must succeed at most once");
        Check(admissions.load() <= 1, "racing Resolve/Cancel must never double-admit");
        Check(corrupt_snapshots.load() == 0, "racing admission must preserve exact plan");
        Check(!service.Pending() && !service.Resolve(prepared.token, true) &&
              !service.TakeAccepted(prepared.token), "resolved/canceled/consumed race must stay final");
        Check(service.WasRejected(prepared.token) == (admissions.load() == 0),
              "cancellation must reject unless the snapshot was already consumed");
    }
}

} // namespace

int main() {
    struct TestCase {
        const char* name;
        void (*run)();
    };
    const TestCase tests[] {
        { "recycle intent admits only to Shell warning policy", RecycleIntentIsNotProof },
        { "all permanent: default rejection", PermanentRejectsByDefault },
        { "all permanent: explicit acceptance", PermanentAcceptsExplicitly },
        { "all unknown: fail closed", UnknownAlwaysBlocks },
        { "mixed unknown: batch-wide priority", MixedUnknownHasPriority },
        { "mixed permanent/recyclable: full explicit plan", MixedKnownRetainsWholePlan },
        { "all MOCK recyclable: one-shot exact snapshot", MockRecyclableAdmitsOnlySnapshot },
        { "stale/replayed/mismatched/zero tokens", StaleReplayedAndMismatchedTokens },
        { "Pending and caller snapshot isolation", PendingSnapshotIsolation },
        { "Cancel pending and untaken admission", CancelPendingAndUntakenAdmission },
        { "current-token rejection lifecycle", RejectionRecordHasCurrentLifetime },
        { "blocked Prepare invalidates old tokens", BlockedPrepareInvalidatesOldTokens },
        { "invalid targets block the whole batch", InvalidPlansBlockWholeBatch },
        { "long and UNC paths retain exact values", LongAndUncPathsAreRetained },
        { "$R/$I pair is one logical target", RecyclePairCountsAsOneLogicalTarget },
        { "all origins including Undo/Recovery use gate", AllOriginsUseSameGate },
        { "concurrent TakeAccepted consumes once", ConcurrentTakesConsumeOnce },
        { "concurrent Resolve/Cancel never double-admits", ConcurrentResolveCancelNeverDoubleAdmits }
    };
    unsigned total = 0;
    unsigned failures = 0;
    for (const auto& test : tests) {
        ++total;
        try {
            test.run();
            std::cout << "[PASS] " << test.name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cout << "[FAIL] " << test.name << ": " << error.what() << '\n';
        } catch (...) {
            ++failures;
            std::cout << "[FAIL] " << test.name << ": unexpected exception\n";
        }
    }
    std::cout << "DeleteService: " << total - failures << '/' << total << " passed; "
              << failures << " failed. All dispositions and paths are mocks.\n";
    return failures == 0 ? 0 : 1;
}
