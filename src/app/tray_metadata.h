// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once
#include "app_model.h"
#include <mutex>

namespace pulse::app {
class WorkerPool;

struct TrayMetadataMailbox {
    std::mutex mutex;
    std::vector<TrayMetadataResult> completed;
};

struct TrayMetadataWorkerState {
    std::shared_ptr<TrayMetadataMailbox> mailbox = std::make_shared<TrayMetadataMailbox>();
};

// Queries filesystem facts; requests retain no tray or UI objects.
void ResolveTrayMetadata(std::vector<TrayMetadataResult>& results);
// UI timer: applies completed facts, then submits pending paths to the I/O pool.
bool PumpTrayMetadata(StagingTray& tray, WorkerPool& worker, TrayMetadataWorkerState& state);
} // namespace pulse::app
