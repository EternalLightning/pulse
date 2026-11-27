// SPDX-License-Identifier: Apache-2.0
// From jimmgreen/pulse at b32436831a066e18448b2810bef4bf989024fc68.
// See LICENSE and NOTICE.

#pragma once
#include <windows.h>
#include <optional>
#include <string>

namespace pulse::shell_bridge {
// Pulse-owned directory memory; Windows private dialog history is untouched.
class FileDialogClientState {
public:
    explicit FileDialogClientState(bool save);
    std::optional<std::wstring> Read(const GUID* guid) const noexcept;
    bool Write(const GUID* guid, const std::wstring& directory) const noexcept;
    bool Clear(const GUID* guid) const noexcept;
private:
    std::wstring Key(const GUID* guid) const;
    bool save_;
    std::wstring app_key_;
};
}

