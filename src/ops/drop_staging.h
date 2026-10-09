#pragma once
#include <windows.h>
#include <string>
#include <vector>

namespace pulse::ops {

// Archive managers (7-Zip, WinRAR, Bandizip) drag files out of a temporary
// folder that they delete as soon as the drop returns, while Pulse resolves
// copy conflicts afterwards (#55). Such sources are staged under
// DropStageRoot() before the drop returns.

// The user's temp folder, long form (GetTempPath may answer in 8.3 names).
std::wstring TempDirectory();
// %TEMP%\PulseDrop: one "<pid>-<n>-<tick>" folder per staged drop.
std::wstring DropStageRoot();

// True for entries inside `temp_dir` but outside `stage_root`.
bool IsTemporaryDropSource(const std::wstring& path, const std::wstring& temp_dir,
                           const std::wstring& stage_root);

// Mirrors the temporary entries of `sources` into a new stage folder as hard
// links (copies for read-only files or when linking fails), under a folder
// named like their original parent so dialogs still show it. `staged` gets
// `sources` with each staged entry replaced; entries that are not temporary
// pass through. Failure leaves the entire source list unchanged.
struct DropStageResult {
    bool any = false;
    unsigned long error = 0;
    std::wstring source;
    operator bool() const { return any; }
};
DropStageResult StageDropSources(const std::vector<std::wstring>& sources, const std::wstring& temp_dir,
                      const std::wstring& stage_root, std::vector<std::wstring>& staged);

// Drop must not return while the archive manager owns the source names.
// Run filesystem work on a worker and dispatch only paints while waiting;
// input, close and application callbacks remain queued to avoid reentrancy.

struct DropStageError { DWORD code = ERROR_SUCCESS; std::wstring source; };
struct DropEnumerationApi {
    decltype(&FindFirstFileExW) first = ::FindFirstFileExW;
    decltype(&FindNextFileW) next = ::FindNextFileW;
};
DropStageResult StageDropSourcesOnWorker(HWND owner, const std::vector<std::wstring>& sources,
    const std::wstring& temp_dir, const std::wstring& stage_root, std::vector<std::wstring>& staged,
    const DropEnumerationApi& api = {});
bool StageDropSources(const std::vector<std::wstring>& sources, const std::wstring& temp_dir,
    const std::wstring& stage_root, std::vector<std::wstring>& staged,
    DropStageError* error, const DropEnumerationApi& api = {});

// The Drop submission boundary: callbacks only receive a fully prepared list.
template<class Submit>
DropStageResult SubmitStagedDrop(const std::vector<std::wstring>& sources,
                                const std::wstring& temp_dir, const std::wstring& stage_root,
                                Submit&& submit, HWND owner = nullptr) {
    std::vector<std::wstring> staged;
    auto result = StageDropSourcesOnWorker(owner, sources, temp_dir, stage_root, staged);
    if (!result.error) submit(staged);
    return result;
}

// Removes stage folders whose process has exited, and this process's own
// folders as well when `include_own` (at exit).
void SweepDropStages(const std::wstring& stage_root, bool include_own);

} // namespace pulse::ops
