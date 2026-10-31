// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "cloud_root_catalog.h"
#include <atomic>
#include <thread>

namespace pulse::app {
namespace {
std::atomic<bool> refreshing{false};
std::atomic<ULONGLONG> last_refresh{0};
std::atomic<uint64_t> revision{0};
std::atomic<std::shared_ptr<const std::vector<CloudRoot>>> catalog;
}

void RequestCloudRootRefresh(std::vector<CloudRoot> seeds) {
    if (GetTickCount64() - last_refresh.load() < 30000 || refreshing.exchange(true)) return;
    try {
        std::thread([seeds = std::move(seeds)]() mutable {
            try {
                const auto registry = CloudRegistry::System();
                auto sync = ReadSyncRootCloudRoots(registry);
                std::vector<std::wstring> paths;
                for (const auto& root : sync) paths.push_back(root.path);
                for (const auto& seed : seeds) paths.push_back(seed.path);
                const auto directory = [](const std::wstring& path) {
                    const DWORD attrs = GetFileAttributesW(path.c_str());
                    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY);
                };
                std::vector<CloudRoot> roots;
                MergeCloudRoots(roots, ReadNavigationPaneCloudRoots(registry), directory);
                MergeCloudRoots(roots, std::move(sync), directory);
                MergeCloudRoots(roots, std::move(seeds), directory);
                SortCloudRoots(roots);
                PublishCloudFilesRoots(std::move(paths));
                PublishCloudRootLabels(roots);
                catalog.store(std::make_shared<const std::vector<CloudRoot>>(std::move(roots)));
                revision.fetch_add(1);
            } catch (...) {}
            last_refresh.store(GetTickCount64());
            refreshing.store(false);
        }).detach();
    } catch (...) { refreshing.store(false); }
}

std::shared_ptr<const std::vector<CloudRoot>> CloudRootCatalog() { return catalog.load(); }
uint64_t CloudRootCatalogRevision() { return revision.load(); }
} // namespace pulse::app
