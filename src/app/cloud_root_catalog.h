#pragma once
#include "cloud_roots.h"
#include <memory>

namespace pulse::app {
// Registry/Shell display-string and root probes are performed by the worker.
void RequestCloudRootRefresh(std::vector<CloudRoot> seeds = {});
std::shared_ptr<const std::vector<CloudRoot>> CloudRootCatalog();
uint64_t CloudRootCatalogRevision();
} // namespace pulse::app
