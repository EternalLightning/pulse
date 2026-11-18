// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "../app/home_catalog.h"
#include "../app/app_model.h"
#include "../app/drive_change.h"
#include "../app/places.h"
#include "../app/details_attribute_toggle.h"
#include "../common/localization.h"
#include "../common/drive_display_name.h"
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
bool include_network_drive = false;
HANDLE network_entered = nullptr;
HANDLE release_network = nullptr;

void Check(bool passed, const char* message) {
    std::printf("[%s] %s\n", passed ? "PASS" : "FAIL", message);
    failures += !passed;
}

DWORD WINAPI TestLogicalDrives() { return (1u << 2) | (include_network_drive ? 1u << 25 : 0); }
UINT WINAPI TestDriveType(LPCWSTR root) { return root[0] == L'Z' ? DRIVE_REMOTE : DRIVE_FIXED; }
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
BOOL WINAPI TestDiskSpace(LPCWSTR root, PULARGE_INTEGER available,
                         PULARGE_INTEGER total, PULARGE_INTEGER free) {
    if (root[0] == L'Z') return FALSE;
    available->QuadPart = free->QuadPart = 50;
    total->QuadPart = 1000;
    return TRUE;
}
}

namespace pulse::path {
std::optional<std::wstring> TestMappedDriveLabel(const std::wstring&) {
    SetEvent(network_entered);
    WaitForSingleObject(release_network, 5000);
    return L"network";
}
}

// Compile the production catalog with only its external provider calls replaced.
// This executable never links to or changes the configured build's objects.
#define GetLogicalDrives TestLogicalDrives
#define GetDriveTypeW TestDriveType
#define SHGetKnownFolderPath TestKnownFolder
#define GetVolumeInformationW TestVolumeInfo
#define GetDiskFreeSpaceExW TestDiskSpace
#define ReadMappedDriveLabel TestMappedDriveLabel
#include "../app/home_catalog.cpp"
#undef GetLogicalDrives
#undef GetDriveTypeW
#undef SHGetKnownFolderPath
#undef GetVolumeInformationW
#undef GetDiskFreeSpaceExW
#undef ReadMappedDriveLabel

namespace pulse::l10n {
const std::wstring& Get(StringId id) {
    static const std::wstring capacity = L"%s / %s";
    static const std::wstring loading = L"loading";
    static const std::wstring text = L"fixture";
    if (id == StringId::HomeDriveSpaceFormat) return capacity;
    return id == StringId::HomeLoading ? loading : text;
}
}
namespace pulse::ipc {
bool QueryShell(const ShellQueryRequest&, ShellQueryResult&, const std::function<bool()>&, DWORD) {
    return false;
}
}

namespace {
void DriveChangeChecks() {
    using namespace pulse::app;
    SidebarModel sidebar;
    SidebarEntry drive;
    drive.is_drive = true;
    drive.path = L"\\\\?\\D:\\";
    sidebar.drives.push_back(drive);
    drive.is_drive = false;
    drive.path = L"C:\\phone";
    sidebar.drives.push_back(drive);
    Check(SidebarDriveMask(sidebar) == (1u << 3), "drive mask excludes portable-device rows");
    Check(ShouldRefreshDriveList(DBT_DEVNODES_CHANGED, nullptr, 0, SidebarDriveMask(sidebar)),
        "virtual-drive removal refreshes when only a device-node broadcast arrives");
    sidebar.drives.clear();
    Check(!ShouldRefreshDriveList(DBT_DEVNODES_CHANGED, nullptr, 0, SidebarDriveMask(sidebar)),
        "repeated device-node broadcasts do not restart drive enumeration");
    DEV_BROADCAST_VOLUME volume{};
    volume.dbcv_size = sizeof(volume);
    volume.dbcv_devicetype = DBT_DEVTYP_VOLUME;
    volume.dbcv_flags = DBTF_MEDIA;
    const auto* header = reinterpret_cast<const DEV_BROADCAST_HDR*>(&volume);
    Check(ShouldRefreshDriveList(DBT_DEVICEARRIVAL, header, 0, 0) &&
        ShouldRefreshDriveList(DBT_DEVICEREMOVECOMPLETE, header, 0, 0),
        "media insertion and ejection refresh even when drive letters stay the same");
    DEV_BROADCAST_HDR interface_header{};
    interface_header.dbch_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
    Check(!ShouldRefreshDriveList(DBT_DEVICEARRIVAL, &interface_header, 0, 0) &&
        !ShouldRefreshDriveList(DBT_DEVICEQUERYREMOVE, header, 0, 0),
        "portable-device and query-removal messages do not refresh logical drives");
}

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

void NetworkCatalogChecks() {
    using namespace pulse;
    include_network_drive = true;
    network_entered = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    release_network = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    app::HomeCatalog catalog;
    catalog.Refresh();
    Check(WaitForSingleObject(network_entered, 3000) == WAIT_OBJECT_0,
        "mapped drive query blocks after local metadata is published");
    const auto make_sidebar = [] {
        app::SidebarModel sidebar;
        for (const auto* root : {L"\\\\?\\C:\\", L"\\\\?\\Z:\\"}) {
            app::SidebarEntry drive;
            drive.path = root;
            drive.detail = l10n::Get(l10n::StringId::HomeLoading);
            sidebar.drives.push_back(std::move(drive));
        }
        return sidebar;
    };
    auto sidebar = make_sidebar();
    catalog.FillDriveLabels(sidebar);
    Check(sidebar.drives[0].used_ratio > 0.94f &&
        sidebar.drives[1].detail == l10n::Get(l10n::StringId::HomeLoading),
        "only the blocked mapped drive shows loading on initial publication");
    catalog.Refresh();
    sidebar = make_sidebar();
    catalog.FillDriveLabels(sidebar);
    ui::PaneViewModel pane;
    pane.is_home = true;
    catalog.Fill(pane);
    Check(catalog.Loading() && pane.home_loading &&
        sidebar.drives[0].used_ratio > 0.94f && sidebar.drives[0].danger &&
        sidebar.drives[0].detail != l10n::Get(l10n::StringId::HomeLoading) &&
        sidebar.drives[1].detail == l10n::Get(l10n::StringId::HomeLoading),
        "refresh during blocked network I/O retains local sidebar capacity and warning");
    model::HomeQuickAccessPrefs quick_access;
    quick_access.hidden = model::kHomeQuickAccessHiddenMask;
    quick_access.AddFolder(L"C:\\Pulse home quick access fixture");
    catalog.Refresh(quick_access);
    catalog.Fill(pane);
    Check(catalog.Loading() && pane.home_loading && !pane.home_cards.empty() &&
        pane.home_cards.front().path == quick_access.folders.front() &&
        std::count_if(pane.home_cards.begin(), pane.home_cards.end(), [](const auto& card) { return card.group == 0; }) == 1,
        "home quick access changes apply immediately while mapped-drive metadata remains blocked");
    SetEvent(release_network);
    const auto deadline = GetTickCount64() + 3000;
    while (catalog.Loading() && GetTickCount64() < deadline) Sleep(1);
    catalog.FillDriveLabels(sidebar);
    Check(!catalog.Loading() && sidebar.drives[0].used_ratio > 0.94f &&
        sidebar.drives[1].detail == l10n::Get(l10n::StringId::HomeCapacityUnavailable) &&
        sidebar.drives[1].used_ratio < 0.0f,
        "unavailable network capacity leaves the resolved local drive intact");
    CloseHandle(network_entered);
    CloseHandle(release_network);
    include_network_drive = false;
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
    DriveChangeChecks();
    CatalogChecks();
    NetworkCatalogChecks();
    if (wcscmp(argv[1], L"--drives-only") != 0) AttributeChecks(argv[1]);
    std::printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
