// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once
#include "cloud_roots.h"
#include <memory>

namespace pulse::app {
// Registry/Shell display-string and root probes are performed by the worker.
void RequestCloudRootRefresh(std::vector<CloudRoot> seeds = {});
std::shared_ptr<const std::vector<CloudRoot>> CloudRootCatalog();
uint64_t CloudRootCatalogRevision();
} // namespace pulse::app
