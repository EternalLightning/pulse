// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once
#include "content_search.h"
#include "../common/text_decode.h"
#include <memory>

namespace pulse::index {
enum class ContentAgentMode { Instant, LegacyWriter, Observer };
struct ContentIndexRoot {
    std::wstring path;
    text::Encoding encoding = text::Encoding::Auto;
};
struct ContentIndexConfig {
    std::vector<ContentIndexRoot> roots;
    std::vector<std::wstring> excluded_directories{L".git", L"node_modules", L"target", L"build", L"dist"};
    std::vector<std::wstring> excluded_paths;
    bool shared_scope = false;
    text::Encoding default_encoding = text::Encoding::Auto;
    uint64_t maximum_file_bytes = 64ull * 1024 * 1024;
    uint64_t maximum_document_bytes = 512ull * 1024 * 1024;
};
struct ContentIndexRootStatus {
    enum class State : uint32_t { Waiting, Scanning, Ready, Offline, AccessFailed, Recovering };
    std::wstring path;
    uint64_t indexed_files = 0;
    uint64_t skipped_files = 0;
    DWORD error = 0;
    bool available = false;
    bool indexing = false;
    State state = State::Waiting;
};
struct ContentIndexStatus {
    uint64_t revision = 0;
    uint64_t change_sequence = 0;
    uint64_t indexed_files = 0;
    uint64_t skipped_files = 0;
    uint64_t errors = 0;
    uint64_t pending_files = 0;
    uint64_t indexed_bytes = 0;
    bool paused = false;
    bool indexing = false;
    DWORD error = 0;
    std::wstring current_root;
    std::wstring coverage;
    std::vector<ContentIndexRootStatus> root_status;
};
ContentIndexConfig LoadContentIndexConfig();
// Failure preserves config; GetLastError describes unreadable/corrupt storage.
bool LoadContentIndexConfig(ContentIndexConfig& config);
bool LoadContentIndexConfig(const std::wstring& database_path, ContentIndexConfig& config);
bool SaveInstantContentConfig(ContentIndexConfig& config);
bool SaveInstantContentConfig(const std::wstring& database_path, ContentIndexConfig& config);
bool IsInternalContentPath(const std::wstring& path);
// Immutable per-index scan hooks; callbacks execute on the indexing worker.
struct ContentIndexScanHooks {
    ContentSearchEnumerationHooks enumeration;
    std::function<bool(const std::wstring&, WIN32_FILE_ATTRIBUTE_DATA&)> attributes;
};
// Bounded SSD/HDD-aware decoding and one transactional writer,
// and independent read-only WAL connections for queries.
class ContentIndex {
public:
    explicit ContentIndex(std::wstring database_path = {}, bool read_only = false);
    ContentIndex(std::wstring database_path, ContentAgentMode mode);
    ContentIndex(std::wstring database_path, ContentAgentMode mode, ContentIndexScanHooks hooks);
    ~ContentIndex();
    ContentIndex(const ContentIndex&) = delete;
    ContentIndex& operator=(const ContentIndex&) = delete;
    bool Configure(const ContentIndexConfig& config);
    ContentIndexConfig Configuration() const;
    ContentIndexStatus Status() const;
    void Pause(bool paused);
    void Rebuild();
    bool Search(const ContentSearchRequest&, const std::atomic<bool>&, ContentBatchCallback);
    bool SearchTask(const ContentSearchRequest&, const std::atomic<bool>&, ContentBatchCallback);
    bool WaitForRevision(uint64_t revision, const std::atomic<bool>& cancelled, DWORD timeout_ms);
    bool WaitUntilIdle(DWORD timeout_ms); // bounded fixture verification
private:
    bool SearchCached(const ContentSearchRequest&, const std::atomic<bool>&, ContentBatchCallback, void* snapshot);
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
size_t MatchCachedContent(std::wstring_view text, const ContentSearchRequest& request, const std::atomic<bool>* cancelled = nullptr);
ContentHit MakeCachedContentHit(const std::wstring& path, uint64_t size, uint64_t modified,
                               std::wstring_view content, size_t match);
} // namespace pulse::index
