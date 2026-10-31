// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once
#include <cstdint>
#include "document_metrics.h"

namespace pulse::index::document {
inline constexpr uint32_t kMagic = 0x39444350;
inline constexpr uint64_t kMaximumFileBytes = 512ull * 1024 * 1024;
inline constexpr uint32_t kMaximumTextChars = 8u * 1024 * 1024;
inline constexpr uint32_t kTimeoutMs = 30000;
inline constexpr uint32_t kIdleMs = 30000;
inline constexpr uint64_t kMemoryBytes = 256ull * 1024 * 1024;
inline constexpr uint64_t kRetirePrivateBytes = kMemoryBytes * 3 / 4;
inline constexpr DWORD kPdfiumOomExitCode = 0xe0000008;
struct FileVersion {
    uint64_t volume = 0, id = 0, bytes = 0, modified = 0, changed = 0;
    bool operator==(const FileVersion&) const = default;
};
inline uint32_t PdfEngineMode() noexcept {
    wchar_t value[16]{};
    const DWORD length = GetEnvironmentVariableW(L"PULSE_PDF_ENGINE", value, ARRAYSIZE(value));
    if (length >= ARRAYSIZE(value)) return 3;
    return wcscmp(value, L"pdfium") == 0 ? 1u : wcscmp(value, L"ifilter") == 0 ? 2u :
        (!length || wcscmp(value, L"auto") == 0) ? 0u : 3u;
}
struct Request {
    uint32_t magic = kMagic;
    uint32_t path_chars = 0;
    uint64_t maximum_bytes = kMaximumFileBytes;
    uint32_t stop_needle_chars = 0;
    uint32_t flags = 0;
    FileVersion expected_version;
    uint32_t expected_config = 0, override_engine = 0; // 2 only for verified fresh IFilter fallback.
};
struct Begin {
    uint32_t magic = kMagic, size = sizeof(Begin);
    uint32_t error = 0, flags = 0; // Bit 0: complete file version available.
    FileVersion version;
    uint32_t engine = 0, config_mode = 0; // Actual engine and caller configuration are distinct.
};
struct Response {
    uint32_t magic = kMagic;
    uint32_t error = 0;
    uint32_t text_chars = 0;
    uint32_t reserved = 0;
    uint64_t file_bytes = 0;
    DocumentReadMetrics metrics;
};
struct Completion {
    uint32_t magic = kMagic, size = sizeof(Completion);
    uint32_t flags = 0, reserved = 0; // Bit 0: private known; bit 1: retire.
    uint64_t private_bytes = 0;
};
static_assert(sizeof(Request) == 72);
static_assert(sizeof(Begin) == 64);
static_assert(sizeof(Completion) == 24);
}
