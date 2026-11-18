// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
#pragma once
#include "fs_enum.h"

namespace pulse::fs {
inline constexpr wchar_t kLibrariesPath[] = L"pulse:shell:libraries";
inline constexpr wchar_t kControlPanelPath[] = L"pulse:shell:controlpanel";
inline constexpr wchar_t kApplicationsPath[] = L"pulse:shell:apps";
inline constexpr wchar_t kLibraryPrefix[] = L"pulse:library:";
inline constexpr wchar_t kShellItemPrefix[] = L"pulse:shell-item:";

// Only the listed roots and a concrete .library-ms description are browsable.
bool IsShellNamespacePath(const std::wstring& path);
bool IsLibraryPath(const std::wstring& path);
bool IsShellItemEntry(const DirEntry& entry);
std::wstring ShellNamespaceParent(const std::wstring& path);

// Called by the directory worker, never the UI thread. Shell calls run on an
// on-demand STA lane; cancellation and its watchdog discard incomplete rows.
HRESULT EnumerateShellNamespace(const std::wstring& path, std::vector<DirEntry>& out,
                               const std::function<bool()>& cancelled = {});

// Shared by real libraries and focused fixtures. No partial-success snapshot.
HRESULT EnumerateLibraryLocations(const std::vector<std::wstring>& locations,
                                 std::vector<DirEntry>& out,
                                 const std::function<bool()>& cancelled = {});
}
