// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace pulse::ui {

// Counts retained payload bytes, not allocator or graphics-driver overhead.
class ImageCacheBudget {
public:
    explicit ImageCacheBudget(size_t limit) : limit_(limit) {}
    size_t Bytes() const { return bytes_.load(std::memory_order_relaxed); }
    size_t Limit() const { return limit_; }
    bool OverBudget() const { return Bytes() > limit_; }
private:
    friend class ImageCacheCharge;
    std::atomic<size_t> bytes_{0};
    const size_t limit_;
};

class ImageCacheCharge {
public:
    explicit ImageCacheCharge(ImageCacheBudget& budget) : budget_(budget) {}
    ~ImageCacheCharge() { SetBytes(0); }
    ImageCacheCharge(const ImageCacheCharge&) = delete;
    ImageCacheCharge& operator=(const ImageCacheCharge&) = delete;
    size_t Bytes() const { return bytes_; }
    void SetBytes(size_t bytes) {
        if (bytes > bytes_) budget_.bytes_.fetch_add(bytes - bytes_, std::memory_order_relaxed);
        else if (bytes < bytes_) budget_.bytes_.fetch_sub(bytes_ - bytes, std::memory_order_relaxed);
        bytes_ = bytes;
    }
    bool TrySetBytes(size_t bytes) {
        if (bytes <= bytes_) { SetBytes(bytes); return true; }
        const size_t extra = bytes - bytes_;
        auto used = budget_.bytes_.load(std::memory_order_relaxed);
        do {
            if (used > budget_.limit_ || extra > budget_.limit_ - used) return false;
        } while (!budget_.bytes_.compare_exchange_weak(used, used + extra, std::memory_order_relaxed));
        bytes_ = bytes;
        return true;
    }
private:
    ImageCacheBudget& budget_;
    size_t bytes_ = 0;
};

inline size_t BitmapPayloadBytes(uint32_t width, uint32_t height) {
    const uint64_t pixels = static_cast<uint64_t>(width) * height;
    return pixels > std::numeric_limits<size_t>::max() / 4
        ? std::numeric_limits<size_t>::max() : static_cast<size_t>(pixels) * 4;
}

inline ImageCacheBudget& ThumbnailImageBudget() {
    static ImageCacheBudget budget(64ull * 1024 * 1024);
    return budget;
}
inline ImageCacheBudget& ShellBitmapBudget() {
    static ImageCacheBudget budget(16ull * 1024 * 1024);
    return budget;
}
inline ImageCacheBudget& AnimationBitmapBudget() {
    static ImageCacheBudget budget(64ull * 1024 * 1024);
    return budget;
}

} // namespace pulse::ui
