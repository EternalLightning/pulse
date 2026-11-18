// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <string>
#include <string_view>
#include <vector>

namespace pulse::model {
struct PathAlias {
    std::wstring name, target;
};
enum class PathAliasError { None, InvalidName, Duplicate, InvalidTarget, Unknown, InvalidSuffix, InvalidFile };

class PathAliases {
public:
    const std::vector<PathAlias>& Items() const { return items_; }
    PathAliasError Set(std::wstring name, std::wstring target, std::wstring_view original = {});
    bool Remove(std::wstring_view name);
    PathAliasError Resolve(std::wstring_view input, std::wstring& output) const;
    bool LoadJson(std::wstring_view text);
    std::wstring ToJson() const;
    static bool ValidTarget(std::wstring_view target);
    static std::wstring CleanName(std::wstring name);
private:
    std::vector<PathAlias> items_;
};
} // namespace pulse::model
