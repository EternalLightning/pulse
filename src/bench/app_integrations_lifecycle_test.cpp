// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "../app/device_registry.h"
#include "../app/cloud_refresh_paths.h"
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <thread>
#include <utility>

namespace probe {
int failures = 0;
void Check(bool ok, const char* name) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
    failures += !ok;
}
std::mutex queue_mutex;
std::deque<std::function<void()>> queue;
std::function<void()> Take() {
    std::lock_guard lock(queue_mutex);
    if (queue.empty()) return {};
    auto task = std::move(queue.front()); queue.pop_front(); return task;
}
size_t Queued() { std::lock_guard lock(queue_mutex); return queue.size(); }
struct Gate {
    std::mutex mutex;
    std::condition_variable cv;
    bool entered = false, released = false;
    void Block() {
        std::unique_lock lock(mutex);
        entered = true; cv.notify_all();
        cv.wait_for(lock, std::chrono::seconds(4), [&] { return released; });
    }
    bool Entered() {
        std::unique_lock lock(mutex);
        return cv.wait_for(lock, std::chrono::seconds(2), [&] { return entered; });
    }
    void Release() { std::lock_guard lock(mutex); released = true; cv.notify_all(); }
} old_list, stop_list;
std::atomic<unsigned> source_calls{0};
class Source final : public pulse::fs::DeviceSource {
    std::vector<pulse::fs::DeviceInfo> ListDevices() override {
        const auto call = ++source_calls;
        if (call == 1) old_list.Block();
        if (call == 3) stop_list.Block();
        pulse::fs::DeviceInfo info;
        info.key = call == 1 ? L"old" : call == 3 ? L"stopped" : L"new";
        info.name = info.key;
        return {info};
    }
    pulse::fs::DeviceError Enumerate(const pulse::fs::DevicePath&, const pulse::fs::DeviceBatchSink&,
        const std::atomic_bool&) override { return pulse::fs::DeviceError::Failed; }
    pulse::fs::DeviceItemCaps Caps(const pulse::fs::DevicePath&) override { return {}; }
    HRESULT ShellItem(const pulse::fs::DevicePath&, IShellItem**) override { return E_NOTIMPL; }
};
}
namespace pulse::fs {
DeviceWorker::DeviceWorker(uint32_t) {}
DeviceWorker::~DeviceWorker() = default;
void DeviceWorker::Post(std::function<void()> task, uint32_t, std::function<void()>) {
    std::lock_guard lock(probe::queue_mutex); probe::queue.push_back(std::move(task));
}
void DeviceWorker::Stop(uint32_t) {}
std::unique_ptr<DeviceSource> MakeShellDeviceSource() { return std::make_unique<probe::Source>(); }
bool AnyPortableDevicePresent() { return false; }
}
namespace pulse::app {
struct DeviceRegistryTestPeer {
    static std::unique_ptr<DeviceRegistry> Make() {
        auto registry = std::unique_ptr<DeviceRegistry>(new DeviceRegistry);
        registry->started_ = true; return registry;
    }
    static void Queue(DeviceRegistry& registry) { DeviceRegistry::OnTimer(nullptr, &registry, nullptr); }
    static void SeedCache(DeviceRegistry& registry) {
        std::lock_guard lock(registry.mutex_); registry.folder_ids_[L"sentinel"] = L"current-id";
    }
    static bool CacheRetained(DeviceRegistry& registry) {
        std::lock_guard lock(registry.mutex_); return registry.folder_ids_.contains(L"sentinel");
    }
};
}
int main() {
    using probe::Check;
    using pulse::app::CloudRefreshParents;
    using Peer = pulse::app::DeviceRegistryTestPeer;
    SetEnvironmentVariableW(L"PULSE_TEST_DATA_DIR", nullptr);
    auto registry = Peer::Make();
    Peer::Queue(*registry);
    auto old_task = probe::Take();
    std::thread old([task = std::move(old_task)] { task(); });
    Check(probe::old_list.Entered(), "A is blocked inside actual registry source call");
    Peer::Queue(*registry);
    probe::Take()();
    const auto newest = registry->Devices();
    const auto revision = registry->Revision();
    Check(newest.size() == 1 && newest.front().key == L"new" && revision == 1,
        "B publishes while A is blocked");
    Peer::SeedCache(*registry);
    probe::old_list.Release(); old.join();
    Check(registry->Devices() == newest && registry->Revision() == revision &&
        Peer::CacheRetained(*registry), "late A cannot replace B, bump revision or clear current IDs");
    std::wstring name;
    Check(!registry->Describe(L"old", &name, nullptr), "late A cannot enter remembered device history");
    Peer::Queue(*registry);
    auto stop_task = probe::Take();
    std::thread stopped([task = std::move(stop_task)] { task(); });
    Check(probe::stop_list.Entered(), "C is blocked before Stop");
    registry->Stop();
    probe::stop_list.Release(); stopped.join();
    Check(registry->Devices() == newest && registry->Revision() == revision &&
        Peer::CacheRetained(*registry), "Stop rejects a late in-flight result");
    const auto queue_size = probe::Queued();
    Peer::Queue(*registry);
    Check(probe::Queued() == queue_size, "timer callback after Stop queues no new work");
    auto queued_registry = Peer::Make();
    const auto before = probe::source_calls.load();
    Peer::Queue(*queued_registry); Peer::Queue(*queued_registry);
    probe::Take()();
    Check(probe::source_calls == before, "superseded queued request never enters source");
    queued_registry->Stop(); probe::Take()();
    Check(probe::source_calls == before && queued_registry->Revision() == 0,
        "queued request after Stop never enters source or publishes");
    std::vector<std::wstring> same;
    for (int i = 0; i < 1000; ++i) same.push_back(L"C:\\cloud\\folder\\file" + std::to_wstring(i));
    Check(CloudRefreshParents(same).size() == 1, "1000 same-directory paths produce one refresh parent");
    const auto cross = CloudRefreshParents({L"C:\\cloud\\a\\1", L"C:\\cloud\\b\\2", L"C:\\cloud\\a\\3"});
    Check(cross.size() == 2 && cross[0] == pulse::fs::ParentPath(L"C:\\cloud\\a\\1") &&
        cross[1] == pulse::fs::ParentPath(L"C:\\cloud\\b\\2"), "cross-directory refresh retains first-seen order");
    Check(CloudRefreshParents({L"C:/Cloud/Folder/one", L"c:\\cloud\\folder\\two",
        L"\\\\?\\C:\\CLOUD\\FOLDER\\three", L"C:\\Cloud\\other\\..\\Folder\\four"}).size() == 1,
        "case, slashes, ordinary dot segments and extended drive paths deduplicate");
    Check(CloudRefreshParents({L"\\\\server\\share\\folder\\one",
        L"\\\\?\\UNC\\SERVER\\SHARE\\FOLDER\\two"}).size() == 1, "ordinary and extended UNC parents deduplicate");
    Check(CloudRefreshParents({L"C:\\cloud\\folder\\one", L"C:\\cloud\\folder2\\two"}).size() == 2,
        "sibling prefix boundary remains distinct");
    Check(CloudRefreshParents({L"\\\\?\\C:\\cloud\\folder\\.\\one", L"\\\\?\\C:\\cloud\\folder\\two"}).size() == 2,
        "literal extended-namespace dot parent is not collapsed");
    const auto roots = CloudRefreshParents({L"C:\\one", L"c:/two", L"\\\\?\\C:\\three"});
    Check(roots.size() == 1 && roots[0] == L"\\\\?\\C:\\", "drive-root parent preserves required slash");
    Check(CloudRefreshParents({}).empty(), "empty completion refreshes no directory");
    std::printf("RESULT failures=%d\n", probe::failures);
    return probe::failures ? 1 : 0;
}
