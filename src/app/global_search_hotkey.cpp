// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "global_search_hotkey.h"

namespace pulse {
void GlobalSearchHotkey::Reset() {
    if (registered_) UnregisterHotKey(window_, kId);
    registered_ = false;
    window_ = nullptr;
    error_ = ERROR_SUCCESS;
}

bool GlobalSearchHotkey::Update(HWND window, bool enabled, UINT modifiers, UINT key) {
    if (enabled && registered_ && window_ == window && modifiers_ == modifiers && key_ == key) return true;
    Reset();
    if (!enabled) return true;
    if (!window || !key || !modifiers || (modifiers & ~(MOD_ALT | MOD_CONTROL | MOD_SHIFT | MOD_WIN))) {
        error_ = ERROR_INVALID_PARAMETER;
        return false;
    }
    if (!RegisterHotKey(window, kId, modifiers | MOD_NOREPEAT, key)) {
        error_ = GetLastError();
        return false;
    }
    window_ = window;
    modifiers_ = modifiers;
    key_ = key;
    registered_ = true;
    return true;
}
}
