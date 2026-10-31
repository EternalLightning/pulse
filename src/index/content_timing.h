// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once
#include <windows.h>
#include "document_metrics.h"
#include <cstdint>
#include <memory>
#include <string_view>

namespace pulse::index {
enum class ContentTimingLane : uint32_t { Pdf, Office, Text, Count };
enum class ContentTimingStage : uint32_t {
    CacheWait, MetadataPrepare, FeedEnumerate, QueueWait, MetadataIo, Extract, Match, Count
};

// Query-owned, bounded diagnostics. Workers only update in-memory counters;
// the query thread writes a snapshot at most once per second. No query text or
// extracted content is accepted by this API. Logging failures are ignored.
class ContentTiming {
public:
    struct FileToken { uint64_t id = 0; };
    explicit ContentTiming(uint64_t generation) noexcept;
    ~ContentTiming();
    ContentTiming(const ContentTiming&) = delete;
    ContentTiming& operator=(const ContentTiming&) = delete;

    static uint64_t NowMicros() noexcept;
    // Pass this timestamp through the queue with the job.
    uint64_t Enqueue(ContentTimingLane lane) noexcept;
    // Starts metadata I/O timing and records the preceding queue wait.
    FileToken BeginFile(ContentTimingLane lane, std::wstring_view path, uint64_t enqueued_us) noexcept;
    // Finish the previous file stage, then start this stage. EndFile finishes
    // the last stage. Use MetadataIo -> Extract -> Match around worker calls.
    void FileStage(FileToken token, ContentTimingStage stage) noexcept;
    void EndFile(FileToken token, bool matched, DWORD error = ERROR_SUCCESS,
                 uint64_t reported_bytes = 0, const DocumentReadMetrics* metrics = nullptr) noexcept;
    void ReuseCachedFile(ContentTimingLane lane) noexcept;
    // Seal candidate counts before interpreting a drained lane as complete.
    void EnumerationComplete() noexcept;
    void RecordReadBytes(FileToken token, uint64_t bytes) noexcept;
    void RecordDocumentMetrics(FileToken token, const DocumentReadMetrics& metrics) noexcept;
    // Non-file phases, measured from a previous NowMicros() timestamp.
    void RecordStage(ContentTimingStage stage, uint64_t started_us,
                     ContentTimingLane lane = ContentTimingLane::Count) noexcept;
    void SetCacheMapping(uint64_t budget_bytes, uint64_t effective_bytes) noexcept;
    void FlushProgress(uint64_t scanned, uint64_t total, uint64_t hits, bool force = false) noexcept;
    void Finish(uint64_t scanned, uint64_t total, uint64_t hits, bool cancelled,
                DWORD error = ERROR_SUCCESS) noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
