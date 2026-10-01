#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace pulse::ops {

// Recyclable is a caller-supplied proof, not a promise established by this class.
// Production builders without positive proof must use Unknown; tests may mock it.
enum class DeleteDisposition { Recyclable, Permanent, Unknown };
enum class DeleteOrigin { Selection, Preview, Duplicates, Undo, Recovery, EmptyRecycle };

struct DeleteTarget {
    std::wstring path;
    std::vector<std::wstring> physical_paths;
    DeleteDisposition disposition = DeleteDisposition::Unknown;
    std::wstring reason;
};

struct DeletePlan {
    uint64_t task_id = 0;
    DeleteOrigin origin = DeleteOrigin::Selection;
    std::vector<DeleteTarget> targets;
};

struct DeleteConfirmation {
    uint64_t token = 0;
    DeletePlan plan;
};

enum class DeleteDecision { Blocked, AwaitingConfirmation, Admitted };

struct DeletePreparation {
    DeleteDecision decision = DeleteDecision::Blocked;
    uint64_t token = 0;
    std::wstring reason;
};

class DeleteService {
public:
    // Every call invalidates the previous token, including a blocked preparation.
    DeletePreparation Prepare(DeletePlan plan);

    // Only an unresolved confirmation is exposed, as an independent value copy.
    std::optional<DeleteConfirmation> Pending() const;

    // Returns true only when this call resolves the current pending token.
    // Rejection is a successful resolution, but never an admission.
    bool Resolve(uint64_t token, bool accepted);

    // Admission is consumable exactly once; callers execute only this snapshot.
    std::optional<DeletePlan> TakeAccepted(uint64_t token);
    bool WasRejected(uint64_t token) const;

    // Cancels unconsumed admission/confirmation; cannot revoke an already taken plan.
    // Timeout and worker termination are the caller's responsibility.
    void Cancel();

private:
    enum class State { Empty, AwaitingConfirmation, Accepted, Rejected, Consumed };

    mutable std::mutex mutex_;
    State state_ = State::Empty;
    uint64_t last_token_ = 0;
    uint64_t current_token_ = 0;
    std::optional<DeletePlan> plan_;
};

} // namespace pulse::ops
