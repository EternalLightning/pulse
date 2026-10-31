// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once
#include <windows.h>

namespace pulse {
class GlobalSearchHotkey {
public:
    static constexpr int kId = 0x5053;
    GlobalSearchHotkey() = default;
    ~GlobalSearchHotkey() { Reset(); }
    GlobalSearchHotkey(const GlobalSearchHotkey&) = delete;
    GlobalSearchHotkey& operator=(const GlobalSearchHotkey&) = delete;
    bool Update(HWND window, bool enabled, UINT modifiers, UINT key);
    void Reset();
    bool Registered() const { return registered_; }
    DWORD Error() const { return error_; }
private:
    HWND window_ = nullptr;
    UINT modifiers_ = 0, key_ = 0;
    bool registered_ = false;
    DWORD error_ = ERROR_SUCCESS;
};
}
