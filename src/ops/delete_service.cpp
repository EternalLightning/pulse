#include "delete_service.h"

#include <limits>
#include <utility>

namespace pulse::ops {

DeletePreparation DeleteService::Prepare(DeletePlan plan) {
    std::lock_guard lock(mutex_);
    state_ = State::Empty;
    current_token_ = 0;
    plan_.reset();

    if (plan.targets.empty()) {
        return { DeleteDecision::Blocked, 0, L"The deletion plan has no targets." };
    }

    bool has_unknown = false;
    bool has_permanent = false;
    std::wstring unknown_reason;
    std::wstring invalid_reason;
    for (const auto& target : plan.targets) {
        switch (target.disposition) {
        case DeleteDisposition::Recyclable:
        case DeleteDisposition::RecycleRequested:
            break;
        case DeleteDisposition::Permanent:
            has_permanent = true;
            break;
        case DeleteDisposition::Unknown:
        default:
            if (!has_unknown) {
                unknown_reason = target.reason.empty()
                    ? L"Recyclability is not proven for every target."
                    : target.reason;
            }
            has_unknown = true;
            break;
        }

        if (target.path.empty() && invalid_reason.empty()) {
            invalid_reason = L"A deletion target has no logical path.";
        }
        if (target.physical_paths.empty() && invalid_reason.empty()) {
            invalid_reason = L"A deletion target has no physical paths.";
        }
        for (const auto& path : target.physical_paths) {
            if (path.empty() && invalid_reason.empty()) {
                invalid_reason = L"A deletion target has an empty physical path.";
            }
        }
    }

    // Unknown always wins, including a batch that would otherwise need confirmation.
    if (has_unknown) {
        return { DeleteDecision::Blocked, 0, std::move(unknown_reason) };
    }
    if (!invalid_reason.empty()) {
        return { DeleteDecision::Blocked, 0, std::move(invalid_reason) };
    }
    if (last_token_ == std::numeric_limits<uint64_t>::max()) {
        return { DeleteDecision::Blocked, 0, L"Deletion admission tokens are exhausted." };
    }

    // Only a fully validated batch becomes the retained snapshot. Never wrap a token.
    plan_.emplace(std::move(plan));
    current_token_ = ++last_token_;
    state_ = has_permanent ? State::AwaitingConfirmation : State::Accepted;
    return {
        has_permanent ? DeleteDecision::AwaitingConfirmation : DeleteDecision::Admitted,
        current_token_,
        {}
    };
}

std::optional<DeleteConfirmation> DeleteService::Pending() const {
    std::lock_guard lock(mutex_);
    if (state_ != State::AwaitingConfirmation || !plan_) {
        return std::nullopt;
    }
    return DeleteConfirmation{ current_token_, *plan_ };
}

bool DeleteService::Resolve(uint64_t token, bool accepted) {
    std::lock_guard lock(mutex_);
    if (token == 0 || token != current_token_ || state_ != State::AwaitingConfirmation) {
        return false;
    }
    if (accepted) {
        state_ = State::Accepted;
    } else {
        state_ = State::Rejected;
        plan_.reset();
    }
    return true;
}

std::optional<DeletePlan> DeleteService::TakeAccepted(uint64_t token) {
    std::lock_guard lock(mutex_);
    if (token == 0 || token != current_token_ || state_ != State::Accepted || !plan_) {
        return std::nullopt;
    }
    auto accepted = std::move(plan_);
    plan_.reset();
    state_ = State::Consumed;
    return accepted;
}

bool DeleteService::WasRejected(uint64_t token) const {
    std::lock_guard lock(mutex_);
    return token != 0 && token == current_token_ && state_ == State::Rejected;
}

void DeleteService::Cancel() {
    std::lock_guard lock(mutex_);
    if (state_ == State::AwaitingConfirmation || state_ == State::Accepted) {
        state_ = State::Rejected;
        plan_.reset();
    }
}

} // namespace pulse::ops
