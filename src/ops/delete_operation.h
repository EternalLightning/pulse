#pragma once
#include "ops_manager.h"

namespace pulse::ops {
inline bool IsDeleteOperation(OpType type) noexcept {
    return type == OpType::RecycleDelete || type == OpType::RealDelete || type == OpType::EmptyRecycle;
}

// Worker-only read-only preparation. No capability execution probes.
DeletePlan BuildDeletePlan(const OpRequest& request, uint64_t task_id, std::wstring& error);
bool IsDeleteSnapshotComplete(const DeletePlan& plan, const std::vector<std::wstring>& confirmed_paths);
std::vector<std::wstring> MapDeletedTargets(const DeletePlan& plan,
    const std::vector<std::wstring>& confirmed_paths);
}
