#pragma once
#include "app_prefs.h"
#include <cstdint>
#include <string>
#include <vector>

namespace pulse {

struct ResolvedOpenTarget {
    std::wstring folder;
    std::wstring file_path;
    std::wstring file_leaf;
    bool this_pc = false;
};

ResolvedOpenTarget ResolveOpenTarget(std::wstring path);

namespace app {

struct ExternalOpenResult {
    uint64_t sequence = 0;
    ExternalOpenMode mode = ExternalOpenMode::ReuseOpenedLocation;
    bool resolution_complete = false;
    ResolvedOpenTarget target;
    uint64_t takeover_token = 0;
    std::vector<std::wstring> selected_paths;
    unsigned selection_flags = 0;
};

} // namespace app
} // namespace pulse
