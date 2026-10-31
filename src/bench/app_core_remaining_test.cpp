// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "../app/home_catalog.h"
#include "../app/app_model.h"
#include "../app/places.h"
#include "../app/details_attribute_toggle.h"
#include "../common/localization.h"
#include "../ipc/shell_query_client.h"
#include <shlobj.h>
#include <aclapi.h>
#include <atomic>
#include <cstdio>
#include <thread>

namespace {
int failures = 0;
DWORD main_thread = 0;
std::atomic<DWORD> volume_thread{0};
std::atomic<int> volume_calls{0};
HANDLE entered[2]{};
HANDLE release_query[2]{};

void Check(bool passed, const char* message) {
    std::printf("[%s] %s\n", passed ? "PASS" : "FAIL", message);
    failures += !passed;
}

DWORD WINAPI TestLogicalDrives() { return 1u << 2; }
UINT WINAPI TestDriveType(LPCWSTR) { return DRIVE_FIXED; }
HRESULT WINAPI TestKnownFolder(REFKNOWNFOLDERID, DWORD, HANDLE, PWSTR* path) {
    *path = nullptr;
    return E_FAIL;
}
BOOL WINAPI TestVolumeInfo(LPCWSTR, LPWSTR label, DWORD count, LPDWORD,
                          LPDWORD, LPDWORD, LPWSTR, DWORD) {
    volume_thread.store(GetCurrentThreadId());
    const int call = volume_calls.fetch_add(1);
    if (call < 2) {
        SetEvent(entered[call]);
        WaitForSingleObject(release_query[call], 5000);
    }
    wcsncpy_s(label, count, call == 0 ? L"old" : L"new", _TRUNCATE);
    return TRUE;
}
BOOL WINAPI TestDiskSpace(LPCWSTR, PULARGE_INTEGER available,
                         PULARGE_INTEGER total, PULARGE_INTEGER free) {
    available->QuadPart = free->QuadPart = 50;
    total->QuadPart = 1000;
    return TRUE;
}
}

// Compile the production catalog with only its external provider calls replaced.
// This executable never links to or changes the configured build's objects.
#define GetLogicalDrives TestLogicalDrives
#define GetDriveTypeW TestDriveType
#define SHGetKnownFolderPath TestKnownFolder
#define GetVolumeInformationW TestVolumeInfo
#define GetDiskFreeSpaceExW TestDiskSpace
#include "../app/home_catalog.cpp"
#undef GetLogicalDrives
#undef GetDriveTypeW
#undef SHGetKnownFolderPath
#undef GetVolumeInformationW
#undef GetDiskFreeSpaceExW

namespace pulse::l10n {
const std::wstring& Get(StringId id) {
    static const std::wstring capacity = L"%s / %s";
    static const std::wstring text = L"fixture";
    return id == StringId::HomeDriveSpaceFormat ? capacity : text;
}
}
namespace pulse::ipc {
bool QueryShell(const ShellQueryRequest&, ShellQueryResult&, const std::function<bool()>&, DWORD) {
    return false;
}
}

namespace {
void CatalogChecks() {
    using namespace pulse;
    for (int i = 0; i < 2; ++i) {
        entered[i] = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        release_query[i] = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    }
    app::HomeCatalog catalog;
    const ULONGLONG start = GetTickCount64();
    catalog.Refresh();
    Check(GetTickCount64() - start < 500, "AC05 refresh returns without waiting for blocked volume metadata");
    Check(WaitForSingleObject(entered[0], 3000) == WAIT_OBJECT_0,
        "AC05 controlled volume query starts");
    Check(volume_thread.load() != main_thread, "AC05 volume query runs off the UI thread");
    app::SidebarModel sidebar;
    app::SidebarEntry drive;
    drive.path = L"\\\\?\\C:\\";
    drive.label = L"C:";
    sidebar.drives.push_back(drive);
    MSG message{};
    PeekMessageW(&message, nullptr, 0, 0, PM_NOREMOVE);
    int heartbeats = 0;
    for (int i = 0; i < 50; ++i) {
        PostThreadMessageW(main_thread, WM_APP + 200, 0, 0);
        catalog.FillDriveLabels(sidebar);
        while (PeekMessageW(&message, nullptr, WM_APP + 200, WM_APP + 200, PM_REMOVE))
            ++heartbeats;
    }
    Check(heartbeats == 50, "AC05 UI heartbeat and sidebar reads continue during delayed query");
    catalog.Refresh();
    SetEvent(release_query[0]);
    Check(WaitForSingleObject(entered[1], 3000) == WAIT_OBJECT_0,
        "AC05 a refresh during I/O schedules the current generation");
    catalog.FillDriveLabels(sidebar);
    Check(sidebar.drives.front().label == L"C:", "AC05 superseded volume result cannot replace the new model");
    SetEvent(release_query[1]);
    const ULONGLONG deadline = GetTickCount64() + 3000;
    while (catalog.Loading() && GetTickCount64() < deadline) Sleep(1);
    catalog.FillDriveLabels(sidebar);
    Check(!catalog.Loading() && sidebar.drives.front().label == L"new (C:)",
        "AC05 current generation publishes the new volume label");
    Check(sidebar.drives.front().danger && sidebar.drives.front().used_ratio > 0.94f &&
        sidebar.drives.front().used_ratio < 0.96f && !sidebar.drives.front().detail.empty(),
        "AC05 background capacity preserves sidebar ratio and low-space warning");
    for (int i = 0; i < 2; ++i) {
        CloseHandle(entered[i]);
        CloseHandle(release_query[i]);
    }
}

void AttributeChecks(const std::wstring& root) {
    using pulse::app::ToggleDetailsAttribute;
    Check(CreateDirectoryW(root.c_str(), nullptr) != FALSE, "IP01 create isolated attribute fixture");
    const auto path = root + L"\\attributes.txt";
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE | FILE_WRITE_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, CREATE_NEW, 0, nullptr);
    Check(file != INVALID_HANDLE_VALUE, "IP01 create isolated file");
    FILETIME original{};
    original.dwLowDateTime = 0x12345678;
    original.dwHighDateTime = 0x01d00000;
    if (file != INVALID_HANDLE_VALUE) {
        Check(SetFileTime(file, nullptr, nullptr, &original) != FALSE, "IP01 set known modification time");
        CloseHandle(file);
    }
    Check(ToggleDetailsAttribute(path, FILE_ATTRIBUTE_HIDDEN) == ERROR_SUCCESS &&
        (GetFileAttributesW(path.c_str()) & FILE_ATTRIBUTE_HIDDEN), "IP01 background helper toggles hidden on");
    Check(ToggleDetailsAttribute(path, FILE_ATTRIBUTE_READONLY) == ERROR_SUCCESS &&
        (GetFileAttributesW(path.c_str()) & FILE_ATTRIBUTE_READONLY), "IP01 background helper toggles read-only on");
    Check(ToggleDetailsAttribute(path, FILE_ATTRIBUTE_HIDDEN) == ERROR_SUCCESS &&
        !(GetFileAttributesW(path.c_str()) & FILE_ATTRIBUTE_HIDDEN) &&
        (GetFileAttributesW(path.c_str()) & FILE_ATTRIBUTE_READONLY), "IP01 toggling hidden preserves read-only");
    Check(ToggleDetailsAttribute(path, FILE_ATTRIBUTE_READONLY) == ERROR_SUCCESS &&
        !(GetFileAttributesW(path.c_str()) & FILE_ATTRIBUTE_READONLY), "IP01 toggles read-only off");
    WIN32_FILE_ATTRIBUTE_DATA data{};
    Check(GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data) &&
        CompareFileTime(&data.ftLastWriteTime, &original) == 0, "IP01 toggling attributes preserves modification time");
    std::thread first([&] { for (int i = 0; i < 10; ++i) ToggleDetailsAttribute(path, FILE_ATTRIBUTE_HIDDEN); });
    std::thread second([&] { for (int i = 0; i < 10; ++i) ToggleDetailsAttribute(path, FILE_ATTRIBUTE_READONLY); });
    first.join();
    second.join();
    Check(!(GetFileAttributesW(path.c_str()) & (FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN)),
        "IP01 concurrent repeated toggles preserve parity for both attributes");
    Check(ToggleDetailsAttribute(root + L"\\missing.txt", FILE_ATTRIBUTE_HIDDEN) == ERROR_FILE_NOT_FOUND,
        "IP01 missing target reports the filesystem error");
    Check(ToggleDetailsAttribute(path, FILE_ATTRIBUTE_DIRECTORY) == ERROR_INVALID_PARAMETER,
        "IP01 unsupported flags are rejected");
    PSECURITY_DESCRIPTOR original_security = nullptr;
    PACL original_acl = nullptr;
    const DWORD security_error = GetNamedSecurityInfoW(path.c_str(), SE_FILE_OBJECT,
        DACL_SECURITY_INFORMATION, nullptr, nullptr, &original_acl, nullptr, &original_security);
    BYTE world_sid[SECURITY_MAX_SID_SIZE]{};
    DWORD world_sid_size = sizeof(world_sid);
    EXPLICIT_ACCESSW denied{};
    denied.grfAccessPermissions = FILE_WRITE_ATTRIBUTES;
    denied.grfAccessMode = DENY_ACCESS;
    denied.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    denied.Trustee.ptstrName = reinterpret_cast<LPWSTR>(world_sid);
    PACL denied_acl = nullptr;
    const bool secured = security_error == ERROR_SUCCESS &&
        CreateWellKnownSid(WinWorldSid, nullptr, world_sid, &world_sid_size) &&
        SetEntriesInAclW(1, &denied, original_acl, &denied_acl) == ERROR_SUCCESS &&
        SetNamedSecurityInfoW(const_cast<LPWSTR>(path.c_str()), SE_FILE_OBJECT,
            DACL_SECURITY_INFORMATION, nullptr, nullptr, denied_acl, nullptr) == ERROR_SUCCESS;
    Check(secured && ToggleDetailsAttribute(path, FILE_ATTRIBUTE_HIDDEN) == ERROR_ACCESS_DENIED,
        "IP01 write-attributes denial reports access failure");
    if (original_security) {
        Check(SetNamedSecurityInfoW(const_cast<LPWSTR>(path.c_str()), SE_FILE_OBJECT,
            DACL_SECURITY_INFORMATION, nullptr, nullptr, original_acl, nullptr) == ERROR_SUCCESS,
            "IP01 restore isolated file permissions");
        LocalFree(original_security);
    }
    if (denied_acl) LocalFree(denied_acl);
    Check(ToggleDetailsAttribute(root, FILE_ATTRIBUTE_HIDDEN) == ERROR_SUCCESS &&
        (GetFileAttributesW(root.c_str()) & FILE_ATTRIBUTE_HIDDEN), "IP01 directory attributes use the same handle operation");
    ToggleDetailsAttribute(root, FILE_ATTRIBUTE_HIDDEN);
    SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);
    Check(DeleteFileW(path.c_str()) && RemoveDirectoryW(root.c_str()) &&
        GetFileAttributesW(root.c_str()) == INVALID_FILE_ATTRIBUTES, "IP01 fixture files and directory are removed");
}
}

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return 2;
    main_thread = GetCurrentThreadId();
    CatalogChecks();
    AttributeChecks(argv[1]);
    std::printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
