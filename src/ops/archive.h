#pragma once
#include <windows.h>
#include <atomic>
#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

namespace pulse::ops {
struct ArchiveEntry {
    std::wstring path;
    bool is_dir = false;
    uint64_t size = 0;
    bool encrypted = false;
    FILETIME mtime{};
};
struct ArchiveResult {
    std::vector<ArchiveEntry> entries;
    std::wstring error;
    bool password_required = false;
};
#ifdef PULSE_ARCHIVE_PROCESS_TEST
struct ArchiveProcessTestResult { DWORD code; std::wstring output; };
ArchiveProcessTestResult RunArchiveProcessForTest(const std::wstring& executable,
    const std::vector<std::wstring>& args, size_t output_limit, const std::atomic_bool* cancel = nullptr);
#endif
bool IsArchivePath(const std::wstring& path);
bool IsSafeArchiveMember(const std::wstring& path);
// Blocking APIs: call from a worker, never the window thread.
ArchiveResult ListArchive(const std::wstring& path, const std::wstring& password = {},
    const std::atomic_bool* cancel = nullptr);
// Empty selection extracts all; a trailing slash selects a directory recursively.
// Existing destination files are skipped. Unsafe names and links reject the archive.
bool ExtractArchive(const std::wstring& path, const std::wstring& destination,
    const std::vector<std::wstring>& selected_paths, const std::wstring& password,
    std::wstring& error, const std::atomic_bool* cancel = nullptr);
}
