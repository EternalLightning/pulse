// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

// search_query.h — Structured advanced-search form ↔ Everything-style query.
#pragma once
#include "../index/index_query.h"
#include "../index/search_kinds.h"
#include <string>
#include <string_view>
#include <vector>

namespace pulse::model {

enum class NameMatchHow : uint8_t { Contains, StartsWith, Exact };
enum class LocationScope : uint8_t { Indexed, CurrentFolder, CustomFolder };
enum class SizePreset : uint8_t { Any, Empty, Lt1MB, From1To10MB, Gt10MB, Custom };
enum class DatePreset : uint8_t { Any, Today, Yesterday, ThisWeek, ThisMonth, ThisYear, Custom };

struct ParsedSearchConstraints {
    struct Group {
        std::wstring size_query;
        std::wstring date_query;
    };
    std::vector<Group> groups;
    SizePreset size = SizePreset::Any;
    std::wstring size_custom;
    DatePreset date = DatePreset::Any;
    std::wstring date_from;
    std::wstring date_to;
};

struct AdvancedSearchSpec {
    std::wstring name;
    bool pinyin_enabled = true;
    NameMatchHow name_how = NameMatchHow::Contains;
    index::SearchKind kind = index::SearchKind::Any;
    std::wstring custom_exts;
    LocationScope location = LocationScope::Indexed;
    std::wstring current_folder;
    std::wstring custom_folder;
    DatePreset date = DatePreset::Any;
    std::wstring date_from;
    std::wstring date_to;
    SizePreset size = SizePreset::Any;
    std::wstring size_custom;
    std::wstring content;
    index::ContentMatchMode content_mode = index::ContentMatchMode::AllWords;
    std::wstring content_exclude;
    bool whole_word = false;
    bool case_sensitive = false;
    std::wstring exclude_name;
    // Keep bounds the form cannot express until that field is explicitly edited.
    ParsedSearchConstraints parsed_constraints;
};

struct SplitSearchQuery {
    std::wstring filename_needle;
    std::wstring path_prefix;
    index::ContentClause content;
    bool has_ext = false;
    bool has_name = false;
    bool has_folder = false;
};

std::wstring QuoteQueryValue(std::wstring_view value);
std::wstring CompileSearchQuery(const AdvancedSearchSpec& spec);
AdvancedSearchSpec ParseSearchQuery(std::wstring_view raw, std::wstring_view current_folder = {});
SplitSearchQuery SplitSearchQueryText(std::wstring_view raw);
bool ContentSearchNeedsScope(const SplitSearchQuery& split);
std::wstring ApplyContentSearchGuards(std::wstring filename_needle, const SplitSearchQuery& split);
// Short label for titles: content text, else name, else empty. Never a raw path: query.
std::wstring SearchDisplayNeedle(std::wstring_view raw);
// " .log , md " -> "log;md". Empty if nothing usable remains.
std::wstring NormalizeExtensionList(std::wstring_view raw);

} // namespace pulse::model
