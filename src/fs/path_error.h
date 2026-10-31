// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once
#include <windows.h>

namespace pulse::fs {
enum class PathError { None, Offline, AccessDenied, Cancelled, Busy, Provider };

inline PathError ClassifyPathError(DWORD error) noexcept {
    switch (error) {
    case ERROR_SUCCESS: return PathError::None;
    case ERROR_ACCESS_DENIED:
    case ERROR_LOGON_FAILURE:
    case ERROR_ACCOUNT_RESTRICTION:
    case ERROR_PASSWORD_EXPIRED: return PathError::AccessDenied;
    case ERROR_OPERATION_ABORTED:
    case ERROR_CANCELLED: return PathError::Cancelled;
    case ERROR_BUSY:
    case ERROR_NOT_ENOUGH_MEMORY:
    case ERROR_NO_SYSTEM_RESOURCES: return PathError::Busy;
    case ERROR_BAD_NETPATH:
    case ERROR_NETNAME_DELETED:
    case ERROR_CONNECTION_UNAVAIL:
    case ERROR_NETWORK_UNREACHABLE:
    case ERROR_HOST_UNREACHABLE:
    case ERROR_CONNECTION_ABORTED:
    case ERROR_CONNECTION_REFUSED:
    case ERROR_SEM_TIMEOUT:
    case ERROR_TIMEOUT: return PathError::Offline;
    default: return PathError::Provider;
    }
}
}
