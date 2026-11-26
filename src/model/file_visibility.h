// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <windows.h>

namespace pulse::model {

struct FileVisibility {
    bool show_hidden = false;
    bool show_protected = false;

    constexpr bool Includes(DWORD attributes) const noexcept {
        return !(attributes & FILE_ATTRIBUTE_HIDDEN) ||
            (show_hidden && (show_protected || !(attributes & FILE_ATTRIBUTE_SYSTEM)));
    }
};

} // namespace pulse::model
