// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once
#include <windows.h>
#include <functional>
#include <string>
#include <vector>

namespace pulse::ops {

struct LockOwner {
    DWORD pid = 0;
    FILETIME start_time{};
    std::wstring app_name;
    std::wstring image_path;
    bool can_terminate = false;
};

struct LockReport {
    std::wstring path;
    std::vector<LockOwner> owners;
};

using LockCancelled = std::function<bool()>;
// Explicit sharing failures permit retry even when Restart Manager finds no owner.
bool IsLockFailure(HRESULT error) noexcept;
// Access denial or a nonempty directory needs owner evidence before showing a lock.
bool MayBeLockFailure(HRESULT error) noexcept;
// Reports external processes only; the current process is not owner evidence.
// Blocking filesystem/Restart Manager work; call only from an operation worker.
LockReport ProbeFileLocks(HRESULT error, const std::wstring& failed_item,
    const std::vector<std::wstring>& candidates, const std::wstring& protected_directory,
    const LockCancelled& cancelled = {}, std::wstring* diagnostics = nullptr);

enum class EndLockResult { Ended, AlreadyGone, Refused, Failed, Cancelled };
// Rechecks process identity and protection even if can_terminate was supplied by a caller.
EndLockResult EndLockOwner(const LockOwner& owner, const std::wstring& protected_directory,
    const LockCancelled& cancelled = {}, DWORD timeout_ms = 3000, DWORD* error = nullptr);
std::wstring LockOwnerDescription(const LockOwner& owner);
std::wstring LockProtectedDirectory();

}
