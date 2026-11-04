// SPDX-License-Identifier: Apache-2.0
#include "../ui/image_cache_budget.h"
#include "../ui/thumbnail_cache.h"
#include <array>
#include <cstdio>
#include <memory>
#include <thread>
#include <vector>

namespace {
int failures = 0;
void Check(bool value, const char* label) {
    std::printf("[%s] %s\n", value ? "PASS" : "FAIL", label);
    failures += !value;
}
}

namespace pulse::ui {
struct ThumbnailCacheTestAccess {
    static void Insert(ThumbnailCache& cache, const std::wstring& key, size_t bytes, bool visible) {
        ThumbnailCache::Request request;
        request.key = key;
        request.id = cache.next_id_++;
        request.epoch = cache.epoch_.load();
        ThumbnailCache::Pending pending;
        pending.id = request.id;
        pending.interests[0] = {true, false, 0, cache.consumers_[0].epoch};
        cache.pending_[key] = std::move(pending);
        if (visible) cache.consumers_[0].visible_keys.insert(key);
        ThumbnailCache::Item item;
        item.cost = bytes;
        Check(cache.StoreResult(request, std::move(item)), "synthetic thumbnail result is accepted without decoding");
    }
    static void Run() {
        constexpr size_t mib = 1024 * 1024;
        const size_t baseline = ThumbnailImageBudget().Bytes();
        {
            std::array<ThumbnailCache, 5> windows;
            for (auto& window : windows) Insert(window, L"visible", 16 * mib, true);
            Check(ThumbnailImageBudget().Bytes() == baseline + 80 * mib,
                "five windows share one byte count and retain their visible working sets");
            { ThumbnailCache::FrameScope frame(windows[0]); }
            Check(windows[0].items_.empty() && ThumbnailImageBudget().Bytes() == baseline + 64 * mib,
                "leaving a view reclaims its idle pixels under process-wide pressure");
            Insert(windows[0], L"idle", 8 * mib, false);
            Check(windows[0].items_.empty() && ThumbnailImageBudget().Bytes() == baseline + 64 * mib,
                "idle thumbnails cannot add another per-window allowance above the process budget");
            windows[1].Reset();
            Check(ThumbnailImageBudget().Bytes() == baseline + 48 * mib,
                "reset releases exactly that cache's process charge");
        }
        Check(ThumbnailImageBudget().Bytes() == baseline,
            "window destruction releases every remaining thumbnail charge");
    }
};
}

int main() {
    using namespace pulse::ui;
    Check(BitmapPayloadBytes(256, 256) == 256 * 256 * 4 &&
          BitmapPayloadBytes(UINT32_MAX, UINT32_MAX) == std::numeric_limits<size_t>::max(),
          "bitmap accounting follows pixel dimensions and saturates overflow");
    ImageCacheBudget budget(128);
    {
        ImageCacheCharge first(budget), second(budget);
        Check(first.TrySetBytes(96) && !second.TrySetBytes(64) && budget.Bytes() == 96,
            "hard reservations reject excess bytes without charging a failed allocation");
        second.SetBytes(64);
        Check(budget.Bytes() == 160 && budget.OverBudget(),
            "soft accounting records simultaneous visible allocations above the idle budget");
        first.SetBytes(32);
        Check(budget.Bytes() == 96 && second.TrySetBytes(96) && budget.Bytes() == 128,
            "releasing an old allocation makes its bytes available to another owner");
    }
    Check(budget.Bytes() == 0, "charge destruction restores the budget");
    {
        auto atlas = std::make_shared<ImageCacheCharge>(budget);
        Check(atlas->TrySetBytes(64), "reserve one atlas payload");
        std::vector<std::shared_ptr<ImageCacheCharge>> slices(100, atlas);
        auto retained_frame = slices.back();
        atlas.reset(); slices.clear();
        Check(budget.Bytes() == 64, "shared slices and retained frames charge one atlas until its final owner releases it");
        retained_frame.reset();
        Check(budget.Bytes() == 0, "last shared atlas owner releases its payload charge");
    }
    {
        ImageCacheBudget concurrent(4096);
        std::array<std::vector<std::unique_ptr<ImageCacheCharge>>, 8> owners;
        std::array<std::thread, 8> threads;
        for (size_t index = 0; index < threads.size(); ++index) {
            threads[index] = std::thread([&, index] {
                for (;;) {
                    auto charge = std::make_unique<ImageCacheCharge>(concurrent);
                    if (!charge->TrySetBytes(32)) break;
                    owners[index].push_back(std::move(charge));
                }
            });
        }
        for (auto& thread : threads) thread.join();
        Check(concurrent.Bytes() == concurrent.Limit(), "concurrent reservations fill but never exceed the process budget");
        for (auto& owner : owners) owner.clear();
        Check(concurrent.Bytes() == 0, "concurrent owners release the full shared budget");
    }
    ThumbnailCacheTestAccess::Run();
    return failures ? 1 : 0;
}
