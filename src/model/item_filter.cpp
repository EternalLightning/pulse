// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "item_filter.h"
#include <algorithm>
#include <cwctype>

namespace pulse::model {
namespace {

bool Tokenize(std::wstring_view text, std::vector<std::wstring>& tokens, std::wstring& bad) {
    size_t pos = 0;
    while (pos < text.size()) {
        while (pos < text.size() && std::iswspace(text[pos])) ++pos;
        if (pos == text.size()) break;
        const size_t begin = pos;
        bool quoted = false;
        while (pos < text.size()) {
            if (text[pos] == L'"') quoted = !quoted;
            else if (!quoted && std::iswspace(text[pos])) break;
            ++pos;
        }
        if (quoted) { bad = std::wstring(text.substr(begin)); return false; }
        tokens.emplace_back(text.substr(begin, pos - begin));
    }
    return true;
}

bool Unquote(std::wstring_view raw, std::wstring& value) {
    if (raw.size() >= 2 && raw.front() == L'"' && raw.back() == L'"')
        raw = raw.substr(1, raw.size() - 2);
    if (raw.find(L'"') != std::wstring_view::npos) return false;
    value.assign(raw);
    return true;
}

bool IsPathToken(std::wstring_view token, size_t colon) {
    if (colon == 1 && std::iswalpha(token[0])) return true;
    const size_t slash = token.find_first_of(L"\\/");
    return slash != std::wstring_view::npos && slash < colon;
}

bool IsMetadataKey(std::wstring_view key) {
    return key == L"ext" || key == L"type" || key == L"kind" || key == L"类型" ||
        key == L"种类" || key == L"size" || key == L"大小" || key == L"dm" ||
        key == L"datemodified" || key == L"修改" || key == L"日期" ||
        key == L"folder" || key == L"文件夹" || key == L"file" || key == L"文件" ||
        key == L"path" || key == L"路径";
}

bool IsExtList(std::wstring_view value) {
    if (value.empty()) return false;
    size_t begin = 0;
    while (begin < value.size()) {
        size_t end = value.find_first_of(L",;", begin);
        if (end == std::wstring_view::npos) end = value.size();
        auto part = value.substr(begin, end - begin);
        if (!part.empty() && part.front() == L'.') part.remove_prefix(1);
        if (part.empty() || part.find_first_of(L"\\/:：*?<>|\"") != std::wstring_view::npos ||
            std::any_of(part.begin(), part.end(), [](wchar_t c) { return std::iswspace(c) != 0; }))
            return false;
        if (end == value.size()) return true;
        begin = end + 1;
    }
    return false;
}

bool ResolveTagList(std::wstring_view value, const ItemFilterTagResolver& resolver,
                    CompiledItemFilter::TagClause& clause) {
    size_t begin = 0;
    bool quoted = false;
    for (size_t pos = 0; pos <= value.size(); ++pos) {
        if (pos < value.size() && value[pos] == L'"') quoted = !quoted;
        if (pos < value.size() && (quoted || value[pos] != L',')) continue;
        std::wstring one;
        if (!Unquote(value.substr(begin, pos - begin), one) || one.empty()) return false;
        if (resolver) {
            auto matches = resolver(index::Fold(one), false);
            clause.alternatives.insert(clause.alternatives.end(), matches.begin(), matches.end());
        }
        begin = pos + 1;
    }
    std::sort(clause.alternatives.begin(), clause.alternatives.end());
    clause.alternatives.erase(std::unique(clause.alternatives.begin(), clause.alternatives.end()),
                              clause.alternatives.end());
    return true;
}

} // namespace

bool ItemFilterTagNameMatches(std::wstring_view name, int tag_index,
                              std::wstring_view needle, bool fuzzy) {
    const std::wstring folded_needle = index::Fold(needle);
    const std::wstring folded_name = index::Fold(name);
    if (!fuzzy) return folded_name == folded_needle;
    if (folded_name.find(folded_needle) != std::wstring::npos) return true;
    static constexpr const wchar_t* aliases[] = { L"红", L"橙", L"绿", L"青", L"紫", L"蓝", L"灰" };
    return (tag_index >= 0 && tag_index < 7 && folded_needle == aliases[tag_index]) ||
        (tag_index == 1 && folded_needle == L"黄");
}

CompiledItemFilter CompileItemFilter(std::wstring_view text, const ItemFilterTagResolver& resolve_tag) {
    CompiledItemFilter result;
    std::vector<std::wstring> tokens;
    if (!Tokenize(text, tokens, result.error_token)) {
        result.error = ItemFilterError::UnterminatedQuote;
        return result;
    }
    CompiledItemFilter::TagClause legacy;
    bool has_legacy = false;
    auto fail = [&](ItemFilterError error, const std::wstring& token) {
        result.error = error;
        result.error_token = token;
    };
    for (const auto& raw : tokens) {
        std::wstring_view token(raw);
        bool negated = false;
        if (token.size() > 1 && (token.front() == L'-' || token.front() == L'!')) {
            const auto rest = token.substr(1);
            const auto colon = rest.find_first_of(L":：");
            if (colon != std::wstring_view::npos && !IsPathToken(rest, colon)) {
                negated = true;
                token = rest;
            }
        }
        if (token.size() > 1 && token.front() == L'#') {
            std::wstring needle;
            if (!Unquote(token.substr(1), needle) || needle.empty()) {
                fail(ItemFilterError::InvalidCondition, raw); return result;
            }
            has_legacy = true;
            if (resolve_tag) {
                auto ids = resolve_tag(index::Fold(needle), true);
                legacy.alternatives.insert(legacy.alternatives.end(), ids.begin(), ids.end());
            }
            continue;
        }
        const auto colon = token.find_first_of(L":：");
        const bool quoted_name = token.front() == L'"';
        if (colon == std::wstring_view::npos || quoted_name || IsPathToken(token, colon)) {
            std::wstring value;
            if (!Unquote(token, value)) { fail(ItemFilterError::InvalidCondition, raw); return result; }
            if (!result.name.empty()) result.name += L' ';
            result.name += value;
            continue;
        }
        const std::wstring key = index::Fold(token.substr(0, colon));
        const auto raw_value = token.substr(colon + 1);
        if (key == L"tag") {
            CompiledItemFilter::TagClause clause;
            clause.negated = negated;
            if (!ResolveTagList(raw_value, resolve_tag, clause)) {
                fail(ItemFilterError::InvalidCondition, raw); return result;
            }
            result.tags.push_back(std::move(clause));
            continue;
        }
        if (!IsMetadataKey(key)) { fail(ItemFilterError::UnknownCondition, raw); return result; }
        std::wstring value;
        if (!Unquote(raw_value, value) || value.find(L'|') != std::wstring::npos ||
            (value.empty() && key != L"folder" && key != L"文件夹" && key != L"file" && key != L"文件") ||
            ((key == L"ext" || key == L"type" || key == L"kind" || key == L"类型" || key == L"种类") &&
             !IsExtList(value))) {
            fail(ItemFilterError::InvalidCondition, raw); return result;
        }
        std::wstring query = negated ? L"!" : L"";
        query += key + L":" + std::wstring(raw_value);
        const auto compiled = index::ParseQuery(query);
        index::Term term;
        if (!compiled.path_prefix.empty()) {
            term.name = index::Fold(compiled.path_prefix);
            term.name_in_path = true;
            term.name_how = index::NameHow::Substring;
        } else if (compiled.groups.size() == 1 && compiled.groups[0].size() == 1) {
            term = compiled.groups[0][0];
        } else { fail(ItemFilterError::InvalidCondition, raw); return result; }
        if (term.size_how == index::SizeHow::Invalid || term.date_how == index::DateHow::Invalid) {
            fail(ItemFilterError::InvalidCondition, raw); return result;
        }
        term.pinyin = false;
        result.metadata.push_back(std::move(term));
    }
    if (has_legacy) {
        std::sort(legacy.alternatives.begin(), legacy.alternatives.end());
        legacy.alternatives.erase(std::unique(legacy.alternatives.begin(), legacy.alternatives.end()),
                                  legacy.alternatives.end());
        result.tags.push_back(std::move(legacy));
    }
    if (result.name.empty() && result.metadata.empty() && result.tags.empty())
        result.name.assign(text);
    result.name = index::Fold(result.name);
    result.wildcard = result.name.find_first_of(L"*?") != std::wstring::npos;
    return result;
}

bool CompiledItemFilter::Matches(const fs::DirEntry& entry, const std::wstring& full_path,
                                  const ItemFilterTagLookup& lookup) const {
    if (!Valid()) return false;
    const auto* chars = entry.name.data();
    const auto length = static_cast<uint32_t>(entry.name.size());
    if (!(wildcard ? index::WildcardFolded(chars, length, name) : index::ContainsFolded(chars, length, name)))
        return false;
    const uint64_t modified = (static_cast<uint64_t>(entry.mtime.dwHighDateTime) << 32) |
        entry.mtime.dwLowDateTime;
    for (const auto& term : metadata) {
        const bool name_matches = term.name_in_path ? index::MatchPath(full_path, term) :
            index::MatchName(chars, length, term);
        if (!index::MatchNameAndType(name_matches, entry.is_dir, term) ||
            (index::MatchExt(chars, length, term) == term.ext_not) ||
            (index::MatchSize(entry.size, term) == term.size_not) ||
            (index::MatchDate(modified, term) == term.date_not)) return false;
    }
    const auto* ids = !tags.empty() && lookup ? lookup(full_path) : nullptr;
    for (const auto& clause : tags) {
        bool found = false;
        if (ids) for (int id : *ids) {
            if (std::binary_search(clause.alternatives.begin(), clause.alternatives.end(), id)) {
                found = true;
                break;
            }
        }
        if (found == clause.negated) return false;
    }
    return true;
}

} // namespace pulse::model
