#pragma once
#include <windows.h>
#include <atomic>
#include <cstdint>
#include <cstddef>
#include <string>
#include <optional>
#include <vector>
#include "archive_progress.h"
#ifdef PULSE_ARCHIVE_PROCESS_TEST
#include <functional>
#endif

namespace pulse::ops {
struct ArchiveEntry {
    std::wstring path;
    bool is_dir = false;
    uint64_t size = 0;
    bool encrypted = false;
    FILETIME mtime{};
};
enum class ArchiveOutcome { Success, Failed, Cancelled, OutputLimit, PartialListing };
// Required is encryption evidence, not a verdict that a supplied password is wrong.
enum class ArchivePasswordState { Unknown, NotRequired, Required };
struct ArchiveOperationResult {
    ArchiveOutcome outcome = ArchiveOutcome::Failed;
    std::optional<DWORD> exit_code;
    ArchivePasswordState password_state = ArchivePasswordState::Unknown;
    std::wstring error;
};
struct ArchiveResult : ArchiveOperationResult {
    std::vector<ArchiveEntry> entries;
};
#ifdef PULSE_ARCHIVE_PROCESS_TEST
struct ArchiveProcessTestResult { ArchiveOutcome outcome; std::optional<DWORD> exit_code; std::wstring output; };
ArchiveProcessTestResult RunArchiveProcessForTest(const std::wstring& executable,
    const std::vector<std::wstring>& args, size_t output_limit, const std::atomic_bool* cancel = nullptr,
    const std::function<void(std::string_view)>& progress = {});
ArchiveResult ParseArchiveListingForTest(const std::wstring& output, DWORD exit_code);
enum class ArchiveExtractionPoint { DestinationCaptured, SelectionWritten, BeforeExtract, ProcessProgress };
using ArchiveExtractionHook = std::function<void(ArchiveExtractionPoint,
    const std::wstring& destination, const std::wstring& selection)>;
ArchiveOperationResult ExtractArchiveForTest(const std::wstring& path, const std::wstring& destination,
    const ArchiveExtractionHook& hook, const std::atomic_bool* cancel = nullptr,
    const ArchiveProgressCallback& progress = {}, const std::vector<std::wstring>& selected_paths = {});
#endif
bool IsArchivePath(const std::wstring& path);
bool IsSafeArchiveMember(const std::wstring& path);
// Blocking APIs: call from a worker, never the window thread.
ArchiveResult ListArchive(const std::wstring& path, const std::wstring& password = {},
    const std::atomic_bool* cancel = nullptr);
// Empty selection extracts all; a trailing slash selects a directory recursively.
// Existing destination files are skipped. Unsafe names and links reject the archive.
// Extracts directly to destination; failures/cancellation may leave partial output.
// A persistent journal admits only unchanged extractor-reported residuals for
// replacement on retry. Other existing files retain skip-existing semantics.
ArchiveOperationResult ExtractArchive(const std::wstring& path, const std::wstring& destination,
    const std::vector<std::wstring>& selected_paths, const std::wstring& password,
    const std::atomic_bool* cancel = nullptr, const ArchiveProgressCallback& progress = {});
// Compatibility adapter; callers requiring cancellation/password details use the typed result.
bool ExtractArchive(const std::wstring& path, const std::wstring& destination,
    const std::vector<std::wstring>& selected_paths, const std::wstring& password,
    std::wstring& error, const std::atomic_bool* cancel = nullptr);
}
