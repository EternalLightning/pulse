// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

// fs_enum.cpp
#include "fs_enum.h"
#include "drive_kind.h"
#include "../common/unique_resource.h"
#include "../common/drive_display_name.h"
#include <windows.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <lm.h>
#include <algorithm>
#include <sstream>
#include <stdexcept>
#include <string_view>

#pragma comment(lib, "shlwapi.lib")

namespace pulse::fs {

namespace {
struct CloseWinHandle { void operator()(HANDLE value) const noexcept { CloseHandle(value); } };
struct CloseFindHandle { void operator()(HANDLE value) const noexcept { FindClose(value); } };
struct FreeNetBuffer { void operator()(BYTE* value) const noexcept { NetApiBufferFree(value); } };
using WinHandleOwner = UniqueResource<HANDLE, nullptr, CloseWinHandle>;
using FindHandleOwner = UniqueResource<HANDLE, nullptr, CloseFindHandle>;
using NetBufferOwner = UniqueResource<BYTE*, nullptr, FreeNetBuffer>;
}

using NTSTATUS = LONG;
constexpr NTSTATUS STATUS_SUCCESS = 0;
constexpr NTSTATUS STATUS_NO_MORE_FILES = static_cast<NTSTATUS>(0x80000006L);
constexpr NTSTATUS kStatusNotImplemented = static_cast<NTSTATUS>(0xC0000002L);
constexpr NTSTATUS kStatusNotSupported = static_cast<NTSTATUS>(0xC00000BBL);

struct NtUnicodeString {
    USHORT Length;
    USHORT MaximumLength;
    PWSTR Buffer;
};

struct NtObjectAttributes {
    ULONG Length;
    HANDLE RootDirectory;
    NtUnicodeString* ObjectName;
    ULONG Attributes;
    PVOID SecurityDescriptor;
    PVOID SecurityQualityOfService;
};

struct NtIoStatusBlock {
    union {
        NTSTATUS Status;
        PVOID Pointer;
    };
    ULONG_PTR Information;
};

using NtPioApcRoutine = VOID (NTAPI*)(PVOID ApcContext, NtIoStatusBlock* IoStatusBlock, ULONG Reserved);

struct NtFileFullDirInformation {
    ULONG NextEntryOffset;
    ULONG FileIndex;
    LARGE_INTEGER CreationTime;
    LARGE_INTEGER LastAccessTime;
    LARGE_INTEGER LastWriteTime;
    LARGE_INTEGER ChangeTime;
    LARGE_INTEGER EndOfFile;
    LARGE_INTEGER AllocationSize;
    ULONG FileAttributes;
    ULONG FileNameLength;
    ULONG EaSize;
    WCHAR FileName[1];
};

enum NtFileInformationClass : int {
    NtFileFullDirectoryInformation = 2,
};

constexpr ULONG NT_FILE_LIST_DIRECTORY = 0x0001;
constexpr ULONG NT_SYNCHRONIZE = 0x00100000;
constexpr ULONG NT_FILE_OPEN = 1;
constexpr ULONG NT_FILE_DIRECTORY_FILE = 0x00000001;
constexpr ULONG NT_OBJ_CASE_INSENSITIVE = 0x00000040;

#define NtInitializeObjectAttributes(p, n, a, r, s) { \
    (p)->Length = sizeof(NtObjectAttributes); \
    (p)->RootDirectory = r; \
    (p)->Attributes = a; \
    (p)->ObjectName = n; \
    (p)->SecurityDescriptor = s; \
    (p)->SecurityQualityOfService = NULL; \
}

using NtCreateFile_t = NTSTATUS (NTAPI*)(
    PHANDLE FileHandle,
    ACCESS_MASK DesiredAccess,
    NtObjectAttributes* ObjectAttributes,
    NtIoStatusBlock* IoStatusBlock,
    PLARGE_INTEGER AllocationSize,
    ULONG FileAttributes,
    ULONG ShareAccess,
    ULONG CreateDisposition,
    ULONG CreateOptions,
    PVOID EaBuffer,
    ULONG EaLength);

using NtQueryDirectoryFile_t = NTSTATUS (NTAPI*)(
    HANDLE FileHandle,
    HANDLE Event,
    NtPioApcRoutine ApcRoutine,
    PVOID ApcContext,
    NtIoStatusBlock* IoStatusBlock,
    PVOID FileInformationBuffer,
    ULONG Length,
    NtFileInformationClass FileInformationClass,
    BOOLEAN ReturnSingleEntry,
    NtUnicodeString* FileName,
    BOOLEAN RestartScan);

class NativeEnumerationError : public EnumerationError {
public:
    NativeEnumerationError(std::string message, DWORD code, bool compatible)
        : EnumerationError(std::move(message), code), compatible_(compatible) {}
    bool CanFallback() const noexcept { return compatible_; }
private:
    bool compatible_;
};

static DWORD NativeErrorCode(NTSTATUS status) {
    using ConvertStatus = ULONG (WINAPI*)(NTSTATUS);
    static const auto convert = reinterpret_cast<ConvertStatus>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlNtStatusToDosError"));
    return convert ? convert(status) : ERROR_MR_MID_NOT_FOUND;
}

[[noreturn]] static void ThrowNativeError(const char* api, NTSTATUS status, bool query) {
    // MS-SMB2 3.3.5.18 distinguishes unsupported classes (NOT_SUPPORTED)
    // from invalid classes (INVALID_INFO_CLASS), parameters and access failures.
    const bool compatible = query && (status == kStatusNotImplemented || status == kStatusNotSupported);
    const DWORD error = NativeErrorCode(status);
    std::ostringstream message;
    message << api << " failed, status=0x" << std::hex << static_cast<ULONG>(status)
            << std::dec << ", error=" << error;
    throw NativeEnumerationError(message.str(), error, compatible);
}

#ifdef PULSE_FS_ENUM_TESTING
namespace {
struct EnumerationInjection {
    LONG status = 0;
    bool query = true;
    bool after_batch = false;
    bool api_unavailable = false;
    size_t fallback_attempts = 0;
};
thread_local EnumerationInjection enumeration_injection;
}
void EnumerationTestProbe::Reset() { enumeration_injection = {}; }
void EnumerationTestProbe::FailNative(LONG status, bool query, bool after_batch) {
    enumeration_injection = {status, query, after_batch};
}
void EnumerationTestProbe::FailApi() { enumeration_injection = {}; enumeration_injection.api_unavailable = true; }
size_t EnumerationTestProbe::FallbackAttempts() { return enumeration_injection.fallback_attempts; }
#endif

struct NtApi {
    NtCreateFile_t create_file = nullptr;
    NtQueryDirectoryFile_t query_directory = nullptr;
};

static const NtApi& InitNtApi() {
    // C++ local-static publication is synchronized, and exceptions retry the
    // initializer rather than exposing one populated function pointer.
    static const NtApi api = [] {
        const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        if (!ntdll) throw NativeEnumerationError("ntdll.dll not loaded", ERROR_MOD_NOT_FOUND, true);
        NtApi result;
        result.create_file = reinterpret_cast<NtCreateFile_t>(GetProcAddress(ntdll, "NtCreateFile"));
        result.query_directory = reinterpret_cast<NtQueryDirectoryFile_t>(GetProcAddress(ntdll, "NtQueryDirectoryFile"));
        if (!result.create_file || !result.query_directory)
            throw NativeEnumerationError("NtCreateFile / NtQueryDirectoryFile not found", ERROR_PROC_NOT_FOUND, true);
        return result;
    }();
    return api;
}

bool IsVirtualPath(const std::wstring& path) {
    return path.starts_with(L"pulse:");
}

bool IsUncPath(const std::wstring& path) {
    return path.starts_with(L"\\\\?\\UNC\\") ||
           (path.starts_with(L"\\\\") && !path.starts_with(L"\\\\?\\"));
}

std::wstring NormalizePath(std::wstring path) {
    if (path.empty()) return path;
    if (IsVirtualPath(path)) return path;
    // Replace forward slashes with backslashes.
    std::replace(path.begin(), path.end(), L'/', L'\\');
    if (path.starts_with(L"\\\\?\\") || path.starts_with(L"\\\\.\\")) {
        // Drive roots must keep a trailing slash: \\?\E: is not a valid directory.
        if (path.starts_with(L"\\\\?\\") && path.size() == 6 && path[5] == L':') path += L'\\';
        return path;
    }
    // Resolve ordinary Win32 dot segments and drive-relative paths before
    // opting into the extended namespace, which gives those segments literal meaning.
    wchar_t full[32768];
    const DWORD n = GetFullPathNameW(path.c_str(), (DWORD)_countof(full), full, nullptr);
    if (n == 0 || n >= _countof(full)) return path;
    path.assign(full, n);
    if (path.starts_with(L"\\\\")) return L"\\\\?\\UNC\\" + path.substr(2);
    // Remove trailing backslash except for drive root.
    if (path.size() > 1 && path.back() == L'\\' && path[path.size() - 2] != L':')
        path.pop_back();
    return L"\\\\?\\" + path;
}

std::wstring ParentPath(const std::wstring& path) {
    std::wstring normalized = NormalizePath(path);
    std::wstring_view view = normalized;
    // Strip long-path prefix for find_last_of logic.
    std::wstring_view core = view;
    if (core.starts_with(L"\\\\?\\UNC\\")) {
        core.remove_prefix(8);
    } else if (core.starts_with(L"\\\\?\\")) {
        core.remove_prefix(4);
    }
    std::wstring temp(core);
    auto pos = temp.find_last_of(L'\\');
    if (pos == std::wstring::npos || pos == 0) return normalized; // no parent
    temp.resize(pos);
    if (temp.size() == 2 && temp[1] == L':') temp += L'\\'; // drive root
    if (view.starts_with(L"\\\\?\\UNC\\")) return std::wstring(L"\\\\?\\UNC\\") + temp;
    if (view.starts_with(L"\\\\?\\")) return std::wstring(L"\\\\?\\") + temp;
    return temp;
}

std::wstring StripLnkSuffix(const std::wstring& name) {
    if (name.size() < 4) return name;
    if (_wcsicmp(name.c_str() + name.size() - 4, L".lnk") != 0) return name;
    return name.substr(0, name.size() - 4);
}

static std::string PathDiagnostic(const std::wstring& path) {
    const int count = WideCharToMultiByte(CP_UTF8, 0, path.data(), static_cast<int>(path.size()),
        nullptr, 0, nullptr, nullptr);
    std::string text(static_cast<size_t>(count), '\0');
    if (count) WideCharToMultiByte(CP_UTF8, 0, path.data(), static_cast<int>(path.size()),
        text.data(), count, nullptr, nullptr);
    return text;
}

struct DirectoryEnumerator::Impl {
    std::wstring path;
    FindHandleOwner find;
    bool started = false, ended = false, read_any = false;
    bool pending = false;
    WIN32_FIND_DATAW pending_data{};
    explicit Impl(const std::wstring& p) : path(NormalizePath(p)) {
        if (p.empty() || IsVirtualPath(p))
            throw EnumerationError("DirectoryEnumerator requires a filesystem directory", ERROR_INVALID_NAME);
    }
};

DirectoryEnumerator::DirectoryEnumerator(const std::wstring& path) : impl_(std::make_unique<Impl>(path)) {}
DirectoryEnumerator::~DirectoryEnumerator() = default;
DirectoryEnumerator::DirectoryEnumerator(DirectoryEnumerator&&) noexcept = default;
DirectoryEnumerator& DirectoryEnumerator::operator=(DirectoryEnumerator&&) noexcept = default;

DirectoryRead DirectoryEnumerator::Next(DirEntry& entry, const std::function<bool()>& cancelled) {
    auto& state = *impl_;
    for (;;) {
        if (cancelled && cancelled()) return DirectoryRead::Cancelled;
        if (state.ended) return DirectoryRead::End;
        WIN32_FIND_DATAW data{};
        const char* api = "FindNextFileW";
        bool available = false;
        if (state.pending) {
            data = state.pending_data;
            state.pending = false;
            available = true;
        } else if (!state.started) {
            state.started = true;
            api = "FindFirstFileExW";
            std::wstring pattern = state.path;
            if (!pattern.ends_with(L"\\")) pattern += L"\\";
            pattern += L"*";
            const HANDLE handle = FindFirstFileExW(pattern.c_str(), FindExInfoBasic,
                &data, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
            available = handle != INVALID_HANDLE_VALUE;
            if (available) {
                state.find.reset(handle);
                state.read_any = true;
            }
        } else {
            available = FindNextFileW(state.find.get(), &data) != FALSE;
        }
        if (!available) {
            const DWORD error = GetLastError();
            state.ended = true;
            state.find.reset();
            if (cancelled && cancelled()) return DirectoryRead::Cancelled;
            if (error == ERROR_NO_MORE_FILES ||
                (error == ERROR_FILE_NOT_FOUND && std::string_view(api) == "FindFirstFileExW"))
                return DirectoryRead::End;
            std::ostringstream message;
            message << api << " failed, error=" << error << " | " << PathDiagnostic(state.path);
            throw EnumerationError(message.str(), error);
        }
        if (cancelled && cancelled()) {
            state.pending_data = data;
            state.pending = true;
            return DirectoryRead::Cancelled;
        }
        if (data.cFileName[0] == L'.' &&
            (data.cFileName[1] == L'\0' || (data.cFileName[1] == L'.' && data.cFileName[2] == L'\0')))
            continue;
        entry = {};
        entry.name = data.cFileName;
        entry.size = (static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
        entry.mtime = data.ftLastWriteTime;
        entry.attrs = data.dwFileAttributes;
        entry.is_dir = (entry.attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
        entry.is_reparse = (entry.attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
        entry.cloud_recall = (entry.attrs & FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS) != 0;
        return DirectoryRead::Entry;
    }
}

bool DirectoryEnumerator::ReadAny() const noexcept { return impl_->read_any; }

static bool EnumerateFindFirstFileEx(const std::wstring& path, std::vector<DirEntry>& out,
                                    const std::function<bool()>& cancelled) {
    DirectoryEnumerator cursor(path);
    DirEntry entry;
    for (;;) {
        const auto read = cursor.Next(entry, cancelled);
        if (read == DirectoryRead::Cancelled || (cancelled && cancelled())) return false;
        if (read == DirectoryRead::End) return true;
        out.push_back(std::move(entry));
    }
}

static bool EnumerateNtQuery(const std::wstring& path, std::vector<DirEntry>& out,
                             const std::function<bool()>& cancelled) {
    if (cancelled && cancelled()) return false;
#ifdef PULSE_FS_ENUM_TESTING
    if (enumeration_injection.api_unavailable)
        throw NativeEnumerationError("NtCreateFile / NtQueryDirectoryFile not found", ERROR_PROC_NOT_FOUND, true);
    if (enumeration_injection.status && !enumeration_injection.query)
        ThrowNativeError("NtCreateFile", enumeration_injection.status, false);
#endif
    const auto& api = InitNtApi();

    std::wstring target = path;
    // Convert Win32 long-path prefix to NT object prefix.
    if (target.starts_with(L"\\\\?\\")) {
        target = L"\\??\\" + target.substr(4);
    }
    if (target.ends_with(L"\\") && !(target.size() == 7 && target[5] == L':')) target.pop_back();

    NtUnicodeString us{};
    us.Buffer = const_cast<PWSTR>(target.c_str());
    us.Length = static_cast<USHORT>(target.size() * sizeof(WCHAR));
    us.MaximumLength = static_cast<USHORT>((target.size() + 1) * sizeof(WCHAR));

    NtObjectAttributes oa{};
    NtInitializeObjectAttributes(&oa, &us, NT_OBJ_CASE_INSENSITIVE, nullptr, nullptr);

    NtIoStatusBlock iosb{};
    HANDLE h;
    NTSTATUS status = api.create_file(
        &h,
        NT_FILE_LIST_DIRECTORY | NT_SYNCHRONIZE,
        &oa,
        &iosb,
        nullptr,
        FILE_ATTRIBUTE_NORMAL,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NT_FILE_OPEN,
        NT_FILE_DIRECTORY_FILE,
        nullptr,
        0);

    if (status != STATUS_SUCCESS) ThrowNativeError("NtCreateFile", status, false);

    const WinHandleOwner directory_owner(h);
    if (cancelled && cancelled()) return false;
    constexpr SIZE_T kBufSize = 64 * 1024;
    std::vector<BYTE> buffer(kBufSize);
    bool restart = true;
    HANDLE hEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!hEvent) {
        std::ostringstream message;
        const DWORD error = GetLastError();
        message << "CreateEventW failed, error=" << error;
        throw EnumerationError(message.str(), error);
    }
    const WinHandleOwner event_owner(hEvent);

    for (;;) {
        if (cancelled && cancelled()) return false;
#ifdef PULSE_FS_ENUM_TESTING
        if (enumeration_injection.status && enumeration_injection.query &&
            (!enumeration_injection.after_batch || !restart))
            ThrowNativeError("NtQueryDirectoryFile", enumeration_injection.status, true);
#endif
        iosb = {};
        status = api.query_directory(
            h,
            hEvent,
            nullptr,
            nullptr,
            &iosb,
            buffer.data(),
            static_cast<ULONG>(kBufSize),
            NtFileFullDirectoryInformation,
            FALSE,
            nullptr,
            restart ? TRUE : FALSE);

        restart = false;

        if (status == STATUS_PENDING) {
            while (WaitForSingleObject(hEvent, 50) == WAIT_TIMEOUT) {
                if (cancelled && cancelled()) {
                    CancelIoEx(h, nullptr);
                    // A provider can ignore cancellation. Keep IOSB/buffer and
                    // owners alive until it physically completes the request.
                    WaitForSingleObject(hEvent, INFINITE);
                    break;
                }
            }
            status = iosb.Status;
        }

        // The native request must finish before releasing its buffer/IOSB.
        if (cancelled && cancelled()) return false;
        if (status == STATUS_NO_MORE_FILES) break;
        if (status != STATUS_SUCCESS) ThrowNativeError("NtQueryDirectoryFile", status, true);

        auto* info = reinterpret_cast<NtFileFullDirInformation*>(buffer.data());
        for (;;) {
            if (cancelled && cancelled()) return false;
            DirEntry e;
            e.name.assign(info->FileName, info->FileNameLength / sizeof(WCHAR));
            if (!(e.name.size() == 1 && e.name[0] == L'.') &&
                !(e.name.size() == 2 && e.name[0] == L'.' && e.name[1] == L'.')) {
                e.size = static_cast<uint64_t>(info->EndOfFile.QuadPart);
                e.mtime.dwLowDateTime = info->LastWriteTime.LowPart;
                e.mtime.dwHighDateTime = info->LastWriteTime.HighPart;
                e.attrs = info->FileAttributes;
                e.is_dir = (info->FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
                e.is_reparse = (info->FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
                e.cloud_recall = (info->FileAttributes & FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS) != 0;
                out.push_back(std::move(e));
            }

            if (info->NextEntryOffset == 0) break;
            info = reinterpret_cast<NtFileFullDirInformation*>(
                reinterpret_cast<BYTE*>(info) + info->NextEntryOffset);
        }
    }
    return true;
}

// "This PC" view (empty path): one entry per logical drive, label matches
// the sidebar ("Label (C:)" or a localized fallback). full_path is set so
// the app layer can navigate/open without joining parent+name.
static bool EnumerateThisPc(std::vector<DirEntry>& out, const std::function<bool()>& cancelled) {
    DWORD drives = GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
        if (cancelled && cancelled()) return false;
        if (!(drives & (1 << i))) continue;
        wchar_t root[4] = { wchar_t(L'A' + i), L':', L'\\', L'\0' };
        DirEntry e;
        e.drive_type = GetDriveTypeW(root);
        if (IsRemoteLikeDrive(root)) {
            e.drive_type = DRIVE_REMOTE;
            const auto label = path::ReadMappedDriveLabel(root);
            e.name = label && !label->empty() ? *label + L" (" + std::wstring(root, 2) + L")"
                                             : std::wstring(root, 2);
        } else {
            wchar_t volume_name[MAX_PATH + 1] = {};
            GetVolumeInformationW(root, volume_name, MAX_PATH, nullptr, nullptr, nullptr, nullptr, 0);
            e.name = volume_name[0] ? std::wstring(volume_name) + L" (" + root[0] + L":)"
                                    : std::wstring(root, 2);
        }
        e.full_path = NormalizePath(root);
        e.is_dir = true;
        e.attrs = FILE_ATTRIBUTE_DIRECTORY;
        if (cancelled && cancelled()) return false;
        out.push_back(std::move(e));
    }
    return true;
}

// Server-root view (\\server, normalized as \\?\UNC\server with no further
// separator): one entry per disk share, enumerated via NetShareEnum. Hidden
// admin shares (name ends in '$') are skipped. full_path is set so the app
// layer can navigate into a share without joining parent+name.
static bool EnumerateServerShares(const std::wstring& server, std::vector<DirEntry>& out,
                                  const std::function<bool()>& cancelled) {
    std::wstring host = L"\\\\" + server;
    BYTE* buf = nullptr;
    DWORD read = 0, total = 0;
    NET_API_STATUS status = NetShareEnum(
        const_cast<LPWSTR>(host.c_str()), 1, &buf, MAX_PREFERRED_LENGTH,
        &read, &total, nullptr);
    const NetBufferOwner buffer_owner(buf);
    if (cancelled && cancelled()) return false;
    if (status != NERR_Success) {
        std::ostringstream oss;
        oss << "NetShareEnum failed, error=" << status;
        throw std::runtime_error(oss.str());
    }
    const auto* infos = reinterpret_cast<const SHARE_INFO_1*>(buf);
    for (DWORD i = 0; i < read; ++i) {
        if (cancelled && cancelled()) return false;
        const DWORD type = infos[i].shi1_type & ~(STYPE_SPECIAL | STYPE_TEMPORARY);
        if (type != STYPE_DISKTREE) continue;
        std::wstring name = infos[i].shi1_netname;
        if (name.empty() || name.back() == L'$') continue;
        DirEntry e;
        e.name = name;
        e.full_path = NormalizePath(host + L"\\" + name);
        e.is_dir = true;
        e.attrs = FILE_ATTRIBUTE_DIRECTORY;
        out.push_back(std::move(e));
    }
    return true;
}

bool EnumerateDirectory(const std::wstring& path, std::vector<DirEntry>& out,
                        const std::function<bool()>& cancelled) {
    out.clear();
    const auto finish = [&](bool complete) {
        if (!complete || (cancelled && cancelled())) { out.clear(); return false; }
        return true;
    };
    if (cancelled && cancelled()) return false;
    if (path.empty()) {
        return finish(EnumerateThisPc(out, cancelled));
    }
    std::wstring normalized = NormalizePath(path);
    if (normalized.starts_with(L"\\\\?\\UNC\\")) {
        std::wstring rest = normalized.substr(8);
        while (!rest.empty() && rest.back() == L'\\') rest.pop_back();
        if (!rest.empty() && rest.find(L'\\') == std::wstring::npos) {
            return finish(EnumerateServerShares(rest, out, cancelled));
        }
    }
    try {
        return finish(EnumerateNtQuery(normalized, out, cancelled));
    } catch (const NativeEnumerationError& primary) {
        out.clear();
        if (cancelled && cancelled()) return false;
        const std::string context = std::string(primary.what()) + " | " + PathDiagnostic(normalized);
        if (!primary.CanFallback()) throw EnumerationError("NT primary: " + context, primary.Code());
        OutputDebugStringA(("Pulse fs: NT enumeration capability fallback: " + context + "\n").c_str());
#ifdef PULSE_FS_ENUM_TESTING
        ++enumeration_injection.fallback_attempts;
#endif
        try {
            return finish(EnumerateFindFirstFileEx(normalized, out, cancelled));
        } catch (const EnumerationError& fallback) {
            out.clear();
            if (cancelled && cancelled()) return false;
            throw EnumerationError("NT primary: " + context + "; Win32 fallback: " + fallback.what(), fallback.Code());
        } catch (...) {
            out.clear();
            throw;
        }
    } catch (const EnumerationError& primary) {
        out.clear();
        if (cancelled && cancelled()) return false;
        throw EnumerationError(std::string(primary.what()) + " | " + PathDiagnostic(normalized), primary.Code());
    } catch (...) {
        out.clear();
        throw;
    }
}

} // namespace pulse::fs
