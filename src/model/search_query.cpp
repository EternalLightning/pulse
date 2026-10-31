// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "search_query.h"
#include "../common/path_utils.h"

#include <cwctype>

namespace pulse::model {
namespace {

void Trim(std::wstring& s) {
    size_t a = 0;
    while (a < s.size() && iswspace(s[a])) ++a;
    size_t b = s.size();
    while (b > a && iswspace(s[b - 1])) --b;
    s = s.substr(a, b - a);
}

std::wstring JoinWords(const std::vector<std::wstring>& words) {
    std::wstring out;
    for (const auto& w : words) {
        if (w.empty()) continue;
        if (!out.empty()) out.push_back(L' ');
        out.append(w);
    }
    return out;
}

std::vector<std::wstring> SplitWords(std::wstring_view s) {
    std::vector<std::wstring> out;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && iswspace(s[i])) ++i;
        size_t j = i;
        while (j < s.size() && !iswspace(s[j])) ++j;
        if (j > i) out.emplace_back(s.substr(i, j - i));
        i = j;
    }
    return out;
}

bool EqualsI(std::wstring_view a, std::wstring_view b) {
    return index::Fold(a) == index::Fold(b);
}

index::SearchKind KindFromExts(const std::vector<std::wstring>& exts) {
    if (exts.empty()) return index::SearchKind::Any;
    std::wstring joined;
    for (size_t i = 0; i < exts.size(); ++i) {
        if (i) joined.push_back(L';');
        joined.append(exts[i]);
    }
    const index::SearchKind kinds[] = {
        index::SearchKind::Document, index::SearchKind::Image, index::SearchKind::Video,
        index::SearchKind::Audio, index::SearchKind::Archive, index::SearchKind::Code,
    };
    for (auto kind : kinds) {
        if (EqualsI(joined, index::KindExtensions(kind))) return kind;
    }
    return index::SearchKind::Custom;
}

std::wstring QuotePathQueryValue(std::wstring_view value) {
    std::wstring path = path::StripExtendedPathPrefix(value);
    Trim(path);
    bool need = false;
    for (wchar_t c : path) {
        if (c == L' ' || c == L'\t' || c == L'|' || c == L'"') {
            need = true;
            break;
        }
    }
    if (!need) return path;
    std::wstring out = L"\"";
    for (wchar_t c : path) {
        if (c != L'"') out.push_back(c);
    }
    out.push_back(L'"');
    return out;
}

struct ConstraintToken {
    std::wstring raw;
    std::wstring value;
    bool size = false;
    bool negated = false;
};

std::vector<ConstraintToken> ReadConstraints(std::wstring_view raw,
    std::vector<ParsedSearchConstraints::Group>& groups) {
    std::vector<ConstraintToken> tokens;
    groups.emplace_back();
    bool have_term = false;
    bool pending_or = false;
    size_t i = 0;
    while (i < raw.size()) {
        if (iswspace(raw[i])) { ++i; continue; }
        if (raw[i] == L'|') {
            pending_or = have_term;
            ++i;
            continue;
        }
        const size_t start = i;
        bool negated = false;
        while (i < raw.size() && raw[i] == L'!') { negated = !negated; ++i; }
        while (i < raw.size() && iswspace(raw[i])) ++i;
        const size_t word = i;
        const bool quoted = i < raw.size() && raw[i] == L'"';
        bool in_quote = false;
        while (i < raw.size()) {
            if (raw[i] == L'"') in_quote = !in_quote;
            else if (!in_quote && (iswspace(raw[i]) || raw[i] == L'|')) break;
            ++i;
        }
        // Content and global flags do not create filename predicate groups.
        if (!index::ParseQuery(raw.substr(start, i - start)).groups.empty()) {
            if (pending_or) groups.emplace_back();
            pending_or = false;
            have_term = true;
        }
        if (quoted) continue;
        const auto text = raw.substr(word, i - word);
        const size_t colon = text.find_first_of(L":：");
        if (colon == std::wstring_view::npos) continue;
        const auto key = index::Fold(text.substr(0, colon));
        const bool size = key == L"size" || key == L"大小";
        if (!size && key != L"dm" && key != L"datemodified" && key != L"修改" && key != L"日期")
            continue;
        auto& query = size ? groups.back().size_query : groups.back().date_query;
        if (!query.empty()) query.push_back(L' ');
        query.append(raw.substr(start, i - start));
        tokens.push_back({std::wstring(raw.substr(start, i - start)),
            std::wstring(text.substr(colon + 1)), size, negated});
    }
    if (tokens.empty()) groups.clear();
    return tokens;
}

DatePreset DatePresetFor(std::wstring_view value) {
    const auto folded = index::Fold(value);
    if (folded == L"today") return DatePreset::Today;
    if (folded == L"yesterday") return DatePreset::Yesterday;
    if (folded == L"thisweek") return DatePreset::ThisWeek;
    if (folded == L"thismonth") return DatePreset::ThisMonth;
    if (folded == L"thisyear") return DatePreset::ThisYear;
    return DatePreset::Custom;
}

} // namespace

std::wstring QuoteQueryValue(std::wstring_view value) {
    bool need = false;
    for (wchar_t c : value) {
        if (c == L' ' || c == L'\t' || c == L'|' || c == L'"' || c == L':' || c == L'：') {
            need = true;
            break;
        }
    }
    if (!need) return std::wstring(value);
    std::wstring out = L"\"";
    for (wchar_t c : value) {
        if (c != L'"') out.push_back(c);
    }
    out.push_back(L'"');
    return out;
}

std::wstring CompileSearchQuery(const AdvancedSearchSpec& spec) {
    std::wstring q;
    auto append = [&](std::wstring_view token) {
        if (token.empty()) return;
        if (!q.empty()) q.push_back(L' ');
        q.append(token);
    };
    std::wstring name = spec.name;
    Trim(name);
    if (!name.empty()) {
        if (spec.name_how == NameMatchHow::Exact) {
            append(L"\"" + name + L"\"");
        } else if (spec.name_how == NameMatchHow::StartsWith) {
            if (name.find_first_of(L"*?") == std::wstring::npos) name += L"*";
            append(QuoteQueryValue(name));
        } else {
            const auto words = SplitWords(name);
            if (words.size() > 1) {
                for (const auto& word : words) append(word);
            } else {
                append(QuoteQueryValue(name));
            }
        }
    }
    std::wstring exclude = spec.exclude_name;
    Trim(exclude);
    if (!exclude.empty()) {
        append(L"!" + QuoteQueryValue(exclude));
    }
    if (spec.kind == index::SearchKind::Folder) {
        append(L"folder:");
    } else if (spec.kind == index::SearchKind::Custom) {
        const std::wstring exts = NormalizeExtensionList(spec.custom_exts);
        if (!exts.empty()) append(L"ext:" + exts);
    } else if (spec.kind != index::SearchKind::Any) {
        append(std::wstring(L"type:") + index::KindId(spec.kind));
    }
    std::wstring folder;
    if (spec.location == LocationScope::CustomFolder) folder = spec.custom_folder;
    else if (spec.location == LocationScope::CurrentFolder) folder = spec.current_folder;
    Trim(folder);
    if (!folder.empty()) {
        append(L"path:" + QuotePathQueryValue(folder));
    }
    const auto& retained = spec.parsed_constraints;
    const bool retain_date = !retained.groups.empty() && spec.date == retained.date &&
        spec.date_from == retained.date_from && spec.date_to == retained.date_to;
    const bool retain_size = !retained.groups.empty() && spec.size == retained.size &&
        spec.size_custom == retained.size_custom;
    if (!retain_date) switch (spec.date) {
    case DatePreset::Today: append(L"dm:today"); break;
    case DatePreset::Yesterday: append(L"dm:yesterday"); break;
    case DatePreset::ThisWeek: append(L"dm:thisweek"); break;
    case DatePreset::ThisMonth: append(L"dm:thismonth"); break;
    case DatePreset::ThisYear: append(L"dm:thisyear"); break;
    case DatePreset::Custom: {
        std::wstring a = spec.date_from, b = spec.date_to;
        Trim(a); Trim(b);
        if (!a.empty() && !b.empty()) append(L"dm:" + a + L".." + b);
        else if (!a.empty()) append(L"dm:" + a);
        else if (!b.empty()) append(L"dm:" + b);
        break;
    }
    default: break;
    }
    if (!retain_size) switch (spec.size) {
    case SizePreset::Empty: append(L"size:empty"); break;
    case SizePreset::Lt1MB: append(L"size:<1mb"); break;
    case SizePreset::From1To10MB: append(L"size:>=1mb") ; append(L"size:<=10mb"); break;
    case SizePreset::Gt10MB: append(L"size:>10mb"); break;
    case SizePreset::Custom: {
        std::wstring custom = spec.size_custom;
        Trim(custom);
        if (!custom.empty()) {
            std::vector<ParsedSearchConstraints::Group> custom_groups;
            const auto custom_tokens = ReadConstraints(custom, custom_groups);
            bool has_size_token = false;
            for (const auto& token : custom_tokens) has_size_token |= token.size;
            if (!has_size_token)
                append(L"size:" + custom);
            else
                append(custom);
        }
        break;
    }
    default: break;
    }
    std::wstring content = spec.content;
    Trim(content);
    if (!content.empty()) {
        if (spec.content_mode == index::ContentMatchMode::Phrase) {
            append(L"content:" + QuoteQueryValue(content));
            append(L"contentmode:phrase");
        } else if (spec.content_mode == index::ContentMatchMode::AnyWord) {
            append(L"contentmode:any");
            for (const auto& word : SplitWords(content))
                append(L"content:" + QuoteQueryValue(word));
        } else {
            for (const auto& word : SplitWords(content))
                append(L"content:" + QuoteQueryValue(word));
        }
    }
    std::wstring excluded = spec.content_exclude;
    Trim(excluded);
    if (!excluded.empty()) {
        for (const auto& word : SplitWords(excluded))
            append(L"!content:" + QuoteQueryValue(word));
    }
    if (spec.whole_word) append(L"ww:");
    if (spec.case_sensitive) append(L"case:");
    if (!spec.pinyin_enabled) append(L"nopinyin:");
    if ((!retain_date && !retain_size) || retained.groups.empty()) return q;
    std::wstring combined;
    for (const auto& group : retained.groups) {
        std::wstring branch = q;
        auto append_filter = [&](std::wstring_view filter) {
            if (filter.empty()) return;
            if (!branch.empty()) branch.push_back(L' ');
            branch.append(filter);
        };
        if (retain_date) append_filter(group.date_query);
        if (retain_size) append_filter(group.size_query);
        if (branch.empty()) return q;
        if (!combined.empty()) combined.append(L" | ");
        combined.append(branch);
    }
    return combined;
}

AdvancedSearchSpec ParseSearchQuery(std::wstring_view raw, std::wstring_view current_folder) {
    AdvancedSearchSpec spec;
    spec.current_folder = path::StripExtendedPathPrefix(current_folder);
    const auto compiled = index::ParseQuery(raw);
    spec.pinyin_enabled = compiled.pinyin_enabled;
    spec.content_mode = compiled.content.mode;
    spec.whole_word = compiled.content.whole_word;
    spec.case_sensitive = compiled.content.case_sensitive;
    spec.content = JoinWords(compiled.content.needles);
    spec.content_exclude = JoinWords(compiled.content.excluded);
    if (!compiled.path_prefix.empty()) {
        spec.custom_folder = compiled.path_prefix;
        if (!spec.current_folder.empty() &&
            EqualsI(spec.custom_folder, spec.current_folder))
            spec.location = LocationScope::CurrentFolder;
        else
            spec.location = LocationScope::CustomFolder;
    }
    bool saw_name = false;
    bool saw_ge_1mb = false;
    bool saw_le_10mb = false;
    size_t size_count = 0;
    for (const auto& group : compiled.groups) {
        for (const auto& term : group) {
            if (term.size_how != index::SizeHow::Any) ++size_count;
            if (term.folder && term.name_how == index::NameHow::Any && term.exts.empty())
                spec.kind = index::SearchKind::Folder;
            if (!term.exts.empty() && spec.kind != index::SearchKind::Folder) {
                spec.kind = KindFromExts(term.exts);
                if (spec.kind == index::SearchKind::Custom) {
                    spec.custom_exts.clear();
                    for (size_t i = 0; i < term.exts.size(); ++i) {
                        if (i) spec.custom_exts.push_back(L';');
                        spec.custom_exts.append(term.exts[i]);
                    }
                }
            }
            if (term.size_how == index::SizeHow::Ge && term.size_lo == 1024ull * 1024ull)
                saw_ge_1mb = true;
            if (term.size_how == index::SizeHow::Le && term.size_lo == 10ull * 1024ull * 1024ull)
                saw_le_10mb = true;
            if (term.size_how == index::SizeHow::Eq && term.size_lo == 0)
                spec.size = SizePreset::Empty;
            else if (term.size_how == index::SizeHow::Lt && term.size_lo == 1024ull * 1024ull)
                spec.size = SizePreset::Lt1MB;
            else if (term.size_how == index::SizeHow::Gt &&
                     term.size_lo == 10ull * 1024ull * 1024ull)
                spec.size = SizePreset::Gt10MB;
            else if (term.size_how != index::SizeHow::Any && spec.size == SizePreset::Any)
                spec.size = SizePreset::Custom;
            if (term.name_how != index::NameHow::Any && !term.name_in_path) {
                if (term.name_not) {
                    spec.exclude_name = term.name;
                } else if (!saw_name) {
                    spec.name = term.name;
                    if (term.name_how == index::NameHow::Exact) spec.name_how = NameMatchHow::Exact;
                    else if (term.name_how == index::NameHow::Wildcard &&
                             !term.name.empty() && term.name.back() == L'*' &&
                             term.name.find(L'?') == std::wstring::npos &&
                             term.name.find(L'*') == term.name.size() - 1) {
                        spec.name.pop_back();
                        spec.name_how = NameMatchHow::StartsWith;
                    } else {
                        spec.name_how = NameMatchHow::Contains;
                    }
                    saw_name = true;
                } else if (!term.name_not && spec.name_how == NameMatchHow::Contains &&
                           term.name_how == index::NameHow::Substring) {
                    spec.name.push_back(L' ');
                    spec.name.append(term.name);
                }
            }
        }
    }
    const auto tokens = ReadConstraints(raw, spec.parsed_constraints.groups);
    size_t date_count = 0;
    bool size_negated = false;
    for (const auto& token : tokens) {
        if (token.size) {
            if (!spec.size_custom.empty()) spec.size_custom.push_back(L' ');
            spec.size_custom.append(token.raw);
            size_negated |= token.negated;
        } else {
            ++date_count;
            spec.date = token.negated ? DatePreset::Custom : DatePresetFor(token.value);
            if (!token.negated && spec.date == DatePreset::Custom) {
                const size_t dots = token.value.find(L"..");
                spec.date_from = token.value.substr(0, dots);
                if (dots != std::wstring::npos) spec.date_to = token.value.substr(dots + 2);
            }
        }
    }
    const bool single_group = spec.parsed_constraints.groups.size() == 1;
    if (single_group && !size_negated && size_count == 2 && saw_ge_1mb && saw_le_10mb)
        spec.size = SizePreset::From1To10MB;
    else if (size_count && (!single_group || size_negated || size_count > 1))
        spec.size = SizePreset::Custom;
    if (size_count == 1 && !size_negated) {
        for (const auto& token : tokens) if (token.size) spec.size_custom = token.value;
    }
    if (date_count > 1 || (date_count && !single_group)) {
        spec.date = DatePreset::Custom;
        spec.date_from.clear();
        spec.date_to.clear();
    }
    auto& retained = spec.parsed_constraints;
    retained.size = spec.size;
    retained.size_custom = spec.size_custom;
    retained.date = spec.date;
    retained.date_from = spec.date_from;
    retained.date_to = spec.date_to;
    return spec;
}

SplitSearchQuery SplitSearchQueryText(std::wstring_view raw) {
    SplitSearchQuery split;
    const auto compiled = index::ParseQuery(raw);
    split.filename_needle = index::FilenameQueryText(raw);
    split.path_prefix = compiled.path_prefix;
    split.content = compiled.content;
    split.has_ext = index::QueryHasExtFilter(compiled);
    split.has_name = index::QueryHasNameFilter(compiled);
    split.has_folder = index::QueryHasFolderFilter(compiled);
    return split;
}

bool ContentSearchNeedsScope(const SplitSearchQuery& split) {
    if (!split.content.present()) return false;
    if (!split.path_prefix.empty()) return false;
    if (split.has_ext || split.has_name || split.has_folder) return false;
    return true;
}

std::wstring ApplyContentSearchGuards(std::wstring filename_needle, const SplitSearchQuery& split) {
    if (!split.content.present()) return filename_needle;
    if (split.has_ext || split.has_folder) return filename_needle;
    if (!filename_needle.empty()) filename_needle.push_back(L' ');
    filename_needle.append(index::TextExtensionQueryToken());
    return filename_needle;
}

std::wstring SearchDisplayNeedle(std::wstring_view raw) {
    const auto spec = ParseSearchQuery(raw);
    if (!spec.content.empty()) return spec.content;
    if (!spec.name.empty()) return spec.name;
    return {};
}

std::wstring NormalizeExtensionList(std::wstring_view raw) {
    std::wstring out;
    size_t i = 0;
    while (i < raw.size()) {
        while (i < raw.size() && (iswspace(raw[i]) || raw[i] == L';' || raw[i] == L',' ||
                                  raw[i] == L'*'))
            ++i;
        size_t j = i;
        while (j < raw.size() && !iswspace(raw[j]) && raw[j] != L';' && raw[j] != L',') ++j;
        std::wstring one(raw.substr(i, j - i));
        while (!one.empty() && one.front() == L'.') one.erase(one.begin());
        while (!one.empty() && one.back() == L'.') one.pop_back();
        if (!one.empty()) {
            if (!out.empty()) out.push_back(L';');
            out.append(one);
        }
        i = j;
    }
    return out;
}

} // namespace pulse::model
