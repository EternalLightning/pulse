// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once
#include <windows.h>

namespace pulse::index {
// Temporary performance diagnostics are off in normal operation. Explicitly
// opt in before process startup; tests enable this in their own environment.
inline bool IndexDiagnosticsEnabled() noexcept {
    static const bool enabled = [] {
        wchar_t value[2]{};
        return GetEnvironmentVariableW(L"PULSE_INDEX_DIAGNOSTICS", value, 2) == 1 && value[0] == L'1';
    }();
    return enabled;
}
}
