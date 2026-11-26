// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "../common/path_utils.h"
#include <algorithm>
#include <map>
#include <memory>

namespace pulse::model {

inline constexpr size_t kDisplayAliasLimit = 255;

inline std::wstring DisplayAliasPathKey(std::wstring_view path) {
    auto key = path::StripExtendedPathPrefix(path);
    std::replace(key.begin(), key.end(), L'/', L'\\');
    while (key.size() > 3 && key.back() == L'\\') key.pop_back();
    return key;
}

struct DisplayAliasPathLess {
    bool operator()(const std::wstring& a, const std::wstring& b) const {
        return CompareStringOrdinal(a.data(), static_cast<int>(a.size()),
                                    b.data(), static_cast<int>(b.size()), TRUE) == CSTR_LESS_THAN;
    }
};

using DisplayAliases = std::map<std::wstring, std::wstring, DisplayAliasPathLess>;
using DisplayAliasSnapshot = std::shared_ptr<const DisplayAliases>;

inline std::wstring_view FindDisplayAlias(const DisplayAliasSnapshot& aliases, std::wstring_view path) {
    if (!aliases || aliases->empty() || path.empty()) return {};
    const auto found = aliases->find(DisplayAliasPathKey(path));
    return found == aliases->end() ? std::wstring_view{} : std::wstring_view(found->second);
}

} // namespace pulse::model
