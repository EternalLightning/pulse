// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once
#include "../model/batch_rename.h"

namespace pulse::app {
using model::BatchRenameRule;
using model::BatchRenameStatus;
using model::BatchRenameItem;
using model::SplitFileName;
using model::IsValidFileName;
using model::BatchRenamePatternUsesIndex;
using model::ApplyBatchRenameRule;
using model::PreviewBatchRename;
} // namespace pulse::app
