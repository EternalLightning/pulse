#pragma once

#include <string>
#include <vector>

namespace pulse::ops {

struct OperationJournalPaths {
    // The first path remains the legacy operations.json file and owns new work.
    // Owners receive legacy plus orphaned per-window files; secondaries receive
    // only their newly allocated path.
    std::vector<std::wstring> paths;
    std::wstring window_path;
};

OperationJournalPaths AssignOperationJournalPaths(const std::wstring& data_dir,
                                                  bool recovery_owner);

} // namespace pulse::ops
