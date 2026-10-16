#pragma once

#include <string>
#include <windows.h>

namespace pulse::diagnostics {

struct ExportOptions {
    std::wstring source_root;
    std::wstring destination;
    bool include_dumps = true;
    bool require_empty_destination = true;
};

// Null restores the Win32 enumerator; used only by isolated failure fixtures.
void SetFindNextForTesting(decltype(&FindNextFileW) next);

bool Export(const ExportOptions& options, std::wstring* error = nullptr);
bool ClearCrashReports(const std::wstring& source_root, std::wstring* error = nullptr);

} // namespace pulse::diagnostics
