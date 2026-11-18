// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace pulse::model {
inline constexpr int kHomeQuickAccessItemCount = 10;
inline constexpr uint32_t kHomeQuickAccessHiddenMask = (1u << kHomeQuickAccessItemCount) - 1;
// Keep persisted bit identities when adding folders or changing display order.
inline constexpr std::array<int, 6> kHomeQuickAccessFolderBits{0, 8, 1, 9, 2, 3};
inline constexpr std::array<int, 4> kHomeQuickAccessSystemBits{4, 5, 6, 7};
// String-only validation; reachability remains an asynchronous navigation concern.
std::wstring NormalizeHomeQuickAccessFolder(std::wstring_view path);
struct HomeQuickAccessPrefs {
    uint32_t hidden = 0;
    std::vector<std::wstring> folders;
    bool operator==(const HomeQuickAccessPrefs&) const = default;
    bool AddFolder(std::wstring path);
    bool RemoveFolder(std::wstring_view path);
    bool MoveFolder(std::wstring_view path, int direction);
};
} // namespace pulse::model
