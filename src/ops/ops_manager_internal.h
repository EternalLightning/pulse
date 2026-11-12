// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once
#include "ops_manager.h"
#include <algorithm>

namespace pulse::ops {

inline bool IsMutationOperation(OpType type) {
    return type == OpType::Move || type == OpType::Rename || type == OpType::BatchRename;
}

inline bool RequiresProducedIdentity(OpType type) {
    return IsMutationOperation(type) || type == OpType::Copy ||
        type == OpType::CreateFolder || type == OpType::CreateTextFile;
}

inline bool HasProducedIdentities(size_t count, const std::vector<FileIdentity>& identities) {
    return count != 0 && identities.size() == count &&
        std::all_of(identities.begin(), identities.end(), [](const auto& id) { return id.Valid(); });
}

} // namespace pulse::ops
