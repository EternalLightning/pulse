// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

// thumbnail_disk_cache.h - Expensive grid thumbnails kept across sessions.
//
// %LOCALAPPDATA%\Pulse\thumbs\<xx>\<hash>.pt, one small file per thumbnail:
// JPEG (quality 90) for opaque pictures, PNG when there is transparency. Only
// results that were slow to make are written (video posters, HEIC, RAW, PDF,
// large photos, files on network shares); cheap ones are faster to decode
// again than to read back. The key holds the path, file size, modification
// time and pixel size, so an edited file simply misses. Nothing is written for
// EFS-encrypted files or removable media. Least recently used entries go once
// the folder passes 512 MB (thumbnail performance plan in plans/).
// PULSE_TEST_THUMB_CACHE=off disables it; any other value is the folder.
#pragma once
#include "ui_compositor.h"
#include <vector>
#include <mutex>
#include <string>

namespace pulse::ui {

struct StoredThumbnail {
    std::vector<uint8_t> pixels;
    uint32_t width = 0, height = 0, source_width = 0, source_height = 0, duration_ms = 0;
};
struct ThumbnailStoreKey {
    const std::wstring& path;
    DWORD attrs;
    uint32_t pixels;
    uint64_t modified, size;
    bool keep_always = false;
    HANDLE cancel = nullptr;
};
struct ThumbnailFileIdentity {
    uint64_t volume = 0, index = 0, modified = 0, size = 0;
    bool operator==(const ThumbnailFileIdentity&) const = default;
};
bool ReadThumbnailFileIdentity(const ThumbnailStoreKey& key, ThumbnailFileIdentity& identity);
class ThumbnailDiskCache final {
public:
    static constexpr uint64_t kBudgetBytes = 512ull << 20;
    static constexpr uint32_t kMinDecodeMs = 30;
    static ThumbnailDiskCache& Shared();

    bool Load(const ThumbnailStoreKey& key, StoredThumbnail& out);
    void Save(const ThumbnailStoreKey& key, uint32_t decode_ms, const StoredThumbnail& thumb,
              const ThumbnailFileIdentity* expected = nullptr);
    // Deletes every entry (Settings). Entries being written meanwhile survive.
    void Clear();
    uint64_t BytesOnDisk();
    // Empty when the cache is turned off or has no folder.
    const std::wstring& Root();

private:
    ThumbnailDiskCache() = default;
    std::wstring EntryPath(const ThumbnailStoreKey& key) const;
    void ScanLocked();
    void TrimLocked();

    std::once_flag root_once_;
    std::wstring root_;
    std::mutex mutex_;
    bool scanned_ = false;
    uint64_t bytes_ = 0;
};

} // namespace pulse::ui
