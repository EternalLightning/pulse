// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once
#include "../common/text_decode.h"
#include <functional>
#include <memory>

namespace pulse::index {
struct TaskTextMetadata {
    uint64_t size = 0, modified = 0;
    DWORD attributes = 0;
    // Only valid during the preflight callback, from the handle used to read.
    std::wstring_view version;
};
struct TaskTextLiteralResult {
    // Empty on no match; otherwise a bounded window containing the literal.
    std::wstring body;
    // Validated plain-text FileVersion; empty on failure or preflight skip.
    std::wstring version;
    uint64_t bytes_read = 0;
    uint32_t skipped_lines = 0;
    bool skipped = false;
};
// One query on one worker. Reuses matcher preparation and bounded scratch
// buffers across files; cancellation and destruction release scratch storage.
// Locale must remain unchanged during this query, as with other query matchers.
class TaskTextLiteralReader {
public:
    TaskTextLiteralReader(std::wstring_view needle, bool case_sensitive);
    ~TaskTextLiteralReader();
    TaskTextLiteralReader(const TaskTextLiteralReader&) = delete;
    TaskTextLiteralReader& operator=(const TaskTextLiteralReader&) = delete;
    bool Read(const std::wstring& path, uint64_t maximum_bytes,
              TaskTextLiteralResult& result, DWORD* error, text::Encoding encoding,
              const std::function<bool()>& cancelled,
              const std::function<bool(const TaskTextMetadata&)>& preflight = {});
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
// Foreground positive single-literal/phrase queries only. Does not replace full
// extraction for indexing or complex queries. Checks the same binary sample,
// size limit, encoding validity and file identity as text::ReadFile.
bool ReadTaskTextLiteral(const std::wstring& path, uint64_t maximum_bytes,
                         TaskTextLiteralResult& result, DWORD* error,
                         text::Encoding encoding, const std::function<bool()>& cancelled,
                         std::wstring_view needle, bool case_sensitive);
}
