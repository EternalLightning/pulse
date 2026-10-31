// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once
#include "ops_manager.h"

namespace pulse::ops {
inline bool IsDeleteOperation(OpType type) noexcept {
    return type == OpType::RecycleDelete || type == OpType::RealDelete || type == OpType::EmptyRecycle;
}

bool IsDeleteBackendLost(HRESULT error) noexcept;

// Worker-only read-only preparation. No capability execution probes.
DeletePlan BuildDeletePlan(const OpRequest& request, uint64_t task_id, std::wstring& error);
bool IsDeleteSnapshotComplete(const DeletePlan& plan, const std::vector<std::wstring>& confirmed_paths);
std::vector<std::wstring> MapDeletedTargets(const DeletePlan& plan,
    const std::vector<std::wstring>& confirmed_paths);
}
