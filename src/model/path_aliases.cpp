// SPDX-License-Identifier: Apache-2.0
#include "path_aliases.h"
#include "../common/json_utils.h"
#include <algorithm>
#include <cwchar>
#include <cwctype>

namespace pulse::model {
namespace {
bool Equal(std::wstring_view a, std::wstring_view b) {
    return a.size() == b.size() && _wcsnicmp(a.data(), b.data(), a.size()) == 0;
}
bool ValidName(std::wstring_view name) {
    if (name.empty() || name.size() > 64 || name == L"." || name == L"..") return false;
    for (wchar_t c : name)
        if (c < 0x20 || std::iswspace(c) || std::wstring_view(L"@\\/:*?\"<>|").find(c) != std::wstring_view::npos)
            return false;
    return name.back() != L'.';
}
bool ValidParts(std::wstring_view path) {
    size_t begin = 0;
    while (begin < path.size()) {
        const auto end = path.find(L'\\', begin);
        const auto part = path.substr(begin, end == std::wstring_view::npos ? path.size() - begin : end - begin);
        if (part.empty() || part == L"." || part == L".." || part.back() == L' ' || part.back() == L'.') return false;
        for (wchar_t c : part)
            if (c < 0x20 || std::wstring_view(L":*?\"<>|").find(c) != std::wstring_view::npos) return false;
        if (end == std::wstring_view::npos) break;
        begin = end + 1;
    }
    return true;
}
std::wstring Slashes(std::wstring value) {
    std::replace(value.begin(), value.end(), L'/', L'\\');
    return value;
}
}

std::wstring PathAliases::CleanName(std::wstring name) {
    const auto first = name.find_first_not_of(L" \t\r\n");
    if (first == std::wstring::npos) return {};
    name = name.substr(first, name.find_last_not_of(L" \t\r\n") - first + 1);
    if (name.front() == L'@') name.erase(name.begin());
    return name;
}

bool PathAliases::ValidTarget(std::wstring_view input) {
    const auto normalized = Slashes(std::wstring(input));
    std::wstring_view target(normalized);
    if (target.empty() || target.size() > 32760) return false;
    if (target.starts_with(L"\\\\?\\UNC\\")) target.remove_prefix(8);
    else if (target.starts_with(L"\\\\?\\")) {
        target.remove_prefix(4);
        if (target.size() < 3 || target[1] != L':' || target[2] != L'\\') return false;
    } else if (target.starts_with(L"\\\\")) {
        target.remove_prefix(2);
        if (target.starts_with(L".\\") || target.starts_with(L"?\\")) return false;
    } else if (target.size() < 3 || target[1] != L':' || target[2] != L'\\') return false;
    if (target.size() >= 3 && target[1] == L':' && target[2] == L'\\') {
        if (!((target[0] >= L'A' && target[0] <= L'Z') || (target[0] >= L'a' && target[0] <= L'z'))) return false;
        return ValidParts(target.substr(3));
    }
    const auto share = target.find(L'\\');
    return share != std::wstring_view::npos && share > 0 && share + 1 < target.size() && ValidParts(target);
}

PathAliasError PathAliases::Set(std::wstring name, std::wstring target, std::wstring_view original) {
    name = CleanName(std::move(name));
    target = Slashes(std::move(target));
    if (!ValidName(name)) return PathAliasError::InvalidName;
    if (!ValidTarget(target)) return PathAliasError::InvalidTarget;
    const auto existing = std::find_if(items_.begin(), items_.end(), [&](const auto& item) { return Equal(item.name, original); });
    for (const auto& item : items_)
        if (Equal(item.name, name) && !Equal(item.name, original)) return PathAliasError::Duplicate;
    if (!original.empty() && existing == items_.end()) return PathAliasError::Unknown;
    if (existing != items_.end()) *existing = {std::move(name), std::move(target)};
    else items_.push_back({std::move(name), std::move(target)});
    std::sort(items_.begin(), items_.end(), [](const auto& a, const auto& b) { return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0; });
    return PathAliasError::None;
}

bool PathAliases::Remove(std::wstring_view name) {
    return std::erase_if(items_, [&](const auto& item) { return Equal(item.name, name); }) != 0;
}

PathAliasError PathAliases::Resolve(std::wstring_view input, std::wstring& output) const {
    output.clear();
    if (input.empty() || input.front() != L'@') return PathAliasError::Unknown;
    input.remove_prefix(1);
    const auto split = input.find_first_of(L"\\/");
    const auto name = input.substr(0, split);
    if (!ValidName(name)) return PathAliasError::InvalidName;
    const auto found = std::find_if(items_.begin(), items_.end(), [&](const auto& item) { return Equal(item.name, name); });
    if (found == items_.end()) return PathAliasError::Unknown;
    std::wstring suffix = split == std::wstring_view::npos ? L"" : Slashes(std::wstring(input.substr(split + 1)));
    if (!ValidParts(suffix)) return PathAliasError::InvalidSuffix;
    output = found->target;
    if (!suffix.empty()) {
        if (output.back() != L'\\') output += L'\\';
        output += suffix;
    }
    if (output.size() > 32760) { output.clear(); return PathAliasError::InvalidSuffix; }
    return PathAliasError::None;
}

bool PathAliases::ValidDisplayName(std::wstring_view alias) {
    return alias.size() <= kDisplayAliasLimit && std::none_of(alias.begin(), alias.end(), [](wchar_t ch) {
        return ch < L' ' || ch == 0x7F || ch == 0x2028 || ch == 0x2029;
    });
}

bool PathAliases::SetDisplayName(std::wstring_view path, std::wstring_view alias) {
    if (!ValidTarget(path) || path::IsDriveRootPath(path) || !ValidDisplayName(alias)) return false;
    const auto first = alias.find_first_not_of(L' ');
    alias = first == std::wstring_view::npos ? std::wstring_view{}
        : alias.substr(first, alias.find_last_not_of(L' ') - first + 1);
    if (FindDisplayAlias(display_names_, path) == alias) return true;
    auto names = display_names_ ? std::make_shared<DisplayAliases>(*display_names_)
                                : std::make_shared<DisplayAliases>();
    auto key = DisplayAliasPathKey(path);
    if (alias.empty()) names->erase(key);
    else (*names)[std::move(key)] = alias;
    display_names_ = names->empty() ? nullptr : std::move(names);
    return true;
}

bool PathAliases::LoadJson(std::wstring_view text) {
    const json::Document document(text);
    if (!document.Is(json::Kind::Object)) return false;
    const auto* aliases = document.Find(L"aliases");
    const auto* display_names = document.Find(L"display_aliases");
    const auto* version = document.Find(L"version");
    if (version && (version->kind != json::Kind::Number || version->text != L"1")) return false;
    if (!aliases || (aliases->kind != json::Kind::Array && aliases->kind != json::Kind::Object)) return false;
    // Unknown metadata is retained by refusing to rewrite an unsupported schema.
    for (const auto& member : document.Root().children)
        if (member.name != L"version" && member.name != L"aliases" &&
            member.name != L"display_aliases") return false;
    if (std::count_if(document.Root().children.begin(), document.Root().children.end(), [](const auto& member) { return member.name == L"aliases"; }) != 1) return false;
    if (display_names && (display_names->kind != json::Kind::Object ||
        std::count_if(document.Root().children.begin(), document.Root().children.end(), [](const auto& member) {
            return member.name == L"display_aliases";
        }) != 1)) return false;
    PathAliases next;
    for (const auto& item : aliases->children) {
        std::wstring name, target;
        if (aliases->kind == json::Kind::Object) {
            if (item.kind != json::Kind::String) return false;
            target = item.name;
            name = item.text;
        } else {
            const auto* n = item.Find(L"name");
            const auto* t = item.Find(L"target");
            if (item.kind != json::Kind::Object || item.children.size() != 2 || !n || !t || n->kind != json::Kind::String || t->kind != json::Kind::String) return false;
            name = n->text;
            target = t->text;
        }
        if (next.Set(name, target) != PathAliasError::None) return false;
    }
    if (display_names && !display_names->children.empty()) {
        auto names = std::make_shared<DisplayAliases>();
        for (const auto& item : display_names->children) {
            if (item.kind != json::Kind::String || !ValidTarget(item.name) || path::IsDriveRootPath(item.name) ||
                !ValidDisplayName(item.text) || item.text.find_first_not_of(L' ') == std::wstring::npos ||
                !names->emplace(DisplayAliasPathKey(item.name), item.text).second) return false;
        }
        next.display_names_ = std::move(names);
    }
    *this = std::move(next);
    return true;
}

std::wstring PathAliases::ToJson() const {
    std::wstring result = L"{\n  \"version\":1,\n  \"aliases\":[";
    for (size_t i = 0; i < items_.size(); ++i) {
        std::wstring name, target;
        json::Escape(items_[i].name, name);
        json::Escape(items_[i].target, target);
        result += (i ? L",\n    " : L"\n    ") + std::wstring(L"{\"name\":\"") + name + L"\",\"target\":\"" + target + L"\"}";
    }
    result += L"\n  ]";
    if (display_names_ && !display_names_->empty()) {
        result += L",\n  \"display_aliases\":{";
        bool first = true;
        for (const auto& [path, alias] : *display_names_) {
            std::wstring key, value;
            json::Escape(path, key);
            json::Escape(alias, value);
            result += first ? L"\n    " : L",\n    ";
            result += L"\"" + key + L"\":\"" + value + L"\"";
            first = false;
        }
        result += L"\n  }";
    }
    return result + L"\n}\n";
}
} // namespace pulse::model
