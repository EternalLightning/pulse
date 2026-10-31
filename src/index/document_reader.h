// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once
#include "../common/text_decode.h"
#include "document_metrics.h"
#include <functional>
#include <memory>

namespace pulse::index {
class DocumentReadPool;
// One query shares up to four warm parsers across its PDF/Office readers.
std::shared_ptr<DocumentReadPool> CreateDocumentReadPool();
bool IsOfficeDocumentExtension(std::wstring_view extension) noexcept;
bool IsExtractedDocumentExtension(std::wstring_view extension) noexcept;
bool IsIndexedContentExtension(std::wstring_view extension) noexcept;
// A foreground scan owns its parser and releases it when the task returns.
// Nested scopes on the same thread share the parser.
class DocumentReadSession {
public:
    DocumentReadSession();
    explicit DocumentReadSession(const std::shared_ptr<DocumentReadPool>& pool);
    ~DocumentReadSession();
    DocumentReadSession(const DocumentReadSession&) = delete;
    DocumentReadSession& operator=(const DocumentReadSession&) = delete;
private:
    std::shared_ptr<DocumentReadPool> previous_pool_;
    bool replaced_pool_ = false;
};
// Background indexing retains the bounded, idle-expiring parser.
bool ReadSearchableDocument(const std::wstring& path, uint64_t maximum_bytes,
                            std::wstring& output, uint64_t& bytes_read, DWORD* error,
                            text::Encoding encoding = text::Encoding::Auto,
                            const std::function<bool()>& cancelled = {},
                            std::wstring_view stop_needle = {}, bool case_sensitive = false,
                            DocumentReadMetrics* metrics = nullptr);
}
