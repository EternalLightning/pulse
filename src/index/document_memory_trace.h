// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once
#include "document_metrics.h"
#include <psapi.h>
#include <cstdio>
#include <string>

namespace pulse::index {
// Explicit, bounded diagnostic only. Sampling and writing do not allocate from
// the parser heap, so tracing near the job limit cannot itself throw bad_alloc.
class DocumentMemoryTrace {
public:
    DocumentMemoryTrace() noexcept {
        try {
            const DWORD count = GetEnvironmentVariableW(L"PULSE_DOCUMENT_DIAGNOSTICS_DIR", nullptr, 0);
            if (!count || count > 32768) return;
            std::wstring directory(count, L'\0');
            const DWORD read = GetEnvironmentVariableW(L"PULSE_DOCUMENT_DIAGNOSTICS_DIR", directory.data(), count);
            if (!read || read >= count) return;
            directory.resize(read);
            const auto attributes = GetFileAttributesW(directory.c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY)) return;
            const auto path = directory + L"\\parser-" + std::to_wstring(GetCurrentProcessId()) +
                L"-" + std::to_wstring(GetTickCount64()) + L".jsonl";
            file_ = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        } catch (...) {}
    }
    ~DocumentMemoryTrace() { if (file_ != INVALID_HANDLE_VALUE) CloseHandle(file_); }
    void Sample(uint64_t sequence, const char* phase, std::wstring_view path, DWORD error = 0) noexcept {
        if (file_ == INVALID_HANDLE_VALUE || sequence > 4096) return;
        if (written_bytes_ + path.size() * 6 + 512 > 8 * 1024 * 1024) {
            CloseHandle(file_); file_ = INVALID_HANDLE_VALUE;
            return;
        }
        PROCESS_MEMORY_COUNTERS_EX memory{};
        memory.cb = sizeof(memory);
        const bool known = K32GetProcessMemoryInfo(GetCurrentProcess(),
            reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory)) != FALSE;
        char buffer[4096]{};
        const int size = _snprintf_s(buffer, sizeof(buffer), _TRUNCATE,
            "{\"pid\":%lu,\"sequence\":%llu,\"phase\":\"%s\",\"at_us\":%llu,"
            "\"private_bytes\":%llu,\"peak_private_bytes\":%llu,\"memory_known\":%s,\"error\":%lu,\"path\":\"",
            GetCurrentProcessId(), static_cast<unsigned long long>(sequence), phase,
            static_cast<unsigned long long>(DocumentMicros()),
            static_cast<unsigned long long>(memory.PrivateUsage),
            static_cast<unsigned long long>(memory.PeakPagefileUsage), known ? "true" : "false", error);
        if (size < 0 || !Write(buffer, static_cast<DWORD>(size))) return;
        size_t used = 0;
        for (const auto character : path) {
            if (used + 6 > sizeof(buffer)) { if (!Write(buffer, static_cast<DWORD>(used))) return; used = 0; }
            // JSON UTF-16 escapes retain long paths and surrogate pairs exactly.
            static constexpr char hex[] = "0123456789abcdef";
            const auto value = static_cast<uint16_t>(character);
            buffer[used++] = '\\'; buffer[used++] = 'u';
            buffer[used++] = hex[(value >> 12) & 15]; buffer[used++] = hex[(value >> 8) & 15];
            buffer[used++] = hex[(value >> 4) & 15]; buffer[used++] = hex[value & 15];
        }
        if (used && !Write(buffer, static_cast<DWORD>(used))) return;
        Write("\"}\n", 3);
    }
private:
    bool Write(const char* data, DWORD size) noexcept {
        DWORD written = 0;
        if (WriteFile(file_, data, size, &written, nullptr) && written == size) {
            written_bytes_ += written;
            return true;
        }
        CloseHandle(file_); file_ = INVALID_HANDLE_VALUE;
        return false;
    }
    HANDLE file_ = INVALID_HANDLE_VALUE;
    uint64_t written_bytes_ = 0;
};
}
