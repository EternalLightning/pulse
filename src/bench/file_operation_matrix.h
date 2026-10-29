#pragma once

#include <filesystem>

namespace pulse::bench {
int RunFileOperationMatrix(const std::filesystem::path& local_parent,
                          const std::filesystem::path& network_parent);
}
