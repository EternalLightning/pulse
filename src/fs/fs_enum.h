// fs_enum.h — Directory enumeration: NtQueryDirectoryFile primary,
// FindFirstFileExW+LARGE_FETCH fallback. No Shell COM, no reparse following.
#pragma once
#include <windows.h>
#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace pulse::fs {

struct DirEntry {
    std::wstring name;
    uint64_t size = 0;
    FILETIME mtime{};
    DWORD attrs = 0;
    UINT drive_type = DRIVE_UNKNOWN; // This PC roots only; ordinary folders keep UNKNOWN.
    bool is_dir = false;
    bool is_reparse = false;
    bool cloud_recall = false;
    std::wstring full_path; // set for virtual views (search/tag); empty = parent+name
    std::wstring shell_id; // Absolute Shell ID list for portable-device namespace items.
    uint8_t pc_device = 0;
    std::wstring recycle_path; // $R payload when listing pulse:recycle; not a .lnk target
    bool change_record_only = false; // Historical deleted/moved-out item, never a file operation source.
    std::wstring change_type_text;
    bool change_deleted = false;
    std::wstring change_old_path;
    // Resolved .lnk target (empty = not a link or unresolvable). The fields
    // above always describe the .lnk file itself; these describe the target.
    std::wstring link_target;
    uint64_t link_target_size = 0;
    FILETIME link_target_mtime{};
    bool link_target_is_dir = false;
};

// pulse:tag: / pulse:search: / pulse:workspace: — not filesystem paths.
bool IsVirtualPath(const std::wstring& path);
bool IsUncPath(const std::wstring& path);

enum class NetStatus { Unknown = 0, Online, Slow, Offline };

// Canonical long-path prefix for Win32 APIs.
std::wstring NormalizePath(std::wstring path);
std::wstring ParentPath(const std::wstring& path);

// "计算图形.dwg.lnk" -> "计算图形.dwg" for display.
std::wstring StripLnkSuffix(const std::wstring& name);

// A filesystem-only, single-directory cursor. Recursion/reparse policy belongs
// to the caller; empty paths and UNC server roots are not virtual views here.
// Streaming scans use FindFirst to preserve per-entry scheduling/cancellation;
// snapshot enumeration retains the NT primary + FindFirst fallback below.
enum class DirectoryRead { Entry, End, Cancelled };
class EnumerationError : public std::runtime_error {
public:
    EnumerationError(std::string message, DWORD code) : std::runtime_error(std::move(message)), code_(code) {}
    DWORD Code() const noexcept { return code_; }
private:
    DWORD code_;
};
class DirectoryEnumerator {
public:
    explicit DirectoryEnumerator(const std::wstring& path);
    ~DirectoryEnumerator();
    DirectoryEnumerator(DirectoryEnumerator&&) noexcept;
    DirectoryEnumerator& operator=(DirectoryEnumerator&&) noexcept;
    DirectoryEnumerator(const DirectoryEnumerator&) = delete;
    DirectoryEnumerator& operator=(const DirectoryEnumerator&) = delete;
    // Lazy open; cancellation is checked before open and between entries.
    // Errors retain API, path and numeric Win32 error.
    DirectoryRead Next(DirEntry& entry, const std::function<bool()>& cancelled = {});
    // A successful open may only have yielded dot entries before a read error.
    bool ReadAny() const noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

#ifdef PULSE_FS_ENUM_TESTING
struct EnumerationTestProbe {
    static void Reset();
    static void FailNative(LONG status, bool query = true, bool after_batch = false);
    static void FailApi();
    static size_t FallbackAttempts();
};
#endif

// Returns false on cooperative cancellation and clears out; failures throw.
// NT failures retain status and Win32 error; only unavailable native APIs and
// explicitly unsupported directory queries retry through FindFirstFileExW.
// Cancellation cannot force a blocking filesystem/provider call to return.
bool EnumerateDirectory(const std::wstring& path, std::vector<DirEntry>& out,
                        const std::function<bool()>& cancelled = {});

} // namespace pulse::fs
