#pragma once
#include "shell_icons.h"

namespace pulse::ui {
struct RenderCaches {
    ShellIconCache icons;
    // ThumbnailCache has window-local Main/QuickPreview cancellation scopes.
    // Keep those caches with their renderer instead of aliasing their scopes.
};
}
