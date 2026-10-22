#include "../model/item_filter.h"
#include <iostream>
#include <unordered_map>

using namespace pulse;

namespace {
int failures = 0;
void Check(bool condition, const wchar_t* label) {
    std::wcout << (condition ? L"[PASS] " : L"[FAIL] ") << label << L'\n';
    if (!condition) ++failures;
}

FILETIME Modified(int year, int month, int day) {
    SYSTEMTIME local{};
    local.wYear = static_cast<WORD>(year);
    local.wMonth = static_cast<WORD>(month);
    local.wDay = static_cast<WORD>(day);
    local.wHour = 12;
    FILETIME wall{}, utc{};
    SystemTimeToFileTime(&local, &wall);
    LocalFileTimeToFileTime(&wall, &utc);
    return utc;
}
} // namespace

int wmain() {
    const std::vector<std::wstring> names = {L"项目 Alpha", L"归档", L"Travel", L"Alpha, Beta"};
    int resolve_calls = 0;
    const model::ItemFilterTagResolver resolver = [&](std::wstring_view needle, bool fuzzy) {
        ++resolve_calls;
        std::vector<int> ids;
        for (size_t i = 0; i < names.size(); ++i)
            if (model::ItemFilterTagNameMatches(names[i], static_cast<int>(i), needle, fuzzy))
                ids.push_back(static_cast<int>(i));
        return ids;
    };
    const std::wstring full = L"C:\\fixtures\\report final.PDF";
    const std::unordered_map<std::wstring, std::vector<int>> assignments = {{full, {0, 2}}};
    int lookup_calls = 0;
    const model::ItemFilterTagLookup lookup = [&](const std::wstring& path) -> const std::vector<int>* {
        ++lookup_calls;
        const auto it = assignments.find(path);
        return it == assignments.end() ? nullptr : &it->second;
    };
    fs::DirEntry entry;
    entry.name = L"report final.PDF";
    entry.full_path = full;
    entry.size = 2 * 1024 * 1024;
    entry.mtime = Modified(2026, 10, 21);
    auto matches = [&](std::wstring_view text) {
        return model::CompileItemFilter(text, resolver).Matches(entry, full, lookup);
    };

    Check(matches(L""), L"empty filter matches");
    Check(matches(L"REPORT FINAL"), L"ordinary words remain one case-insensitive phrase");
    Check(!matches(L"report PDF"), L"ordinary words are not separate AND name terms");
    Check(matches(L"*.pdf"), L"legacy wildcard name");
    Check(!matches(L"report?pdf"), L"wildcard matches the entire name");
    Check(matches(L"  report   final  "), L"legacy whitespace normalization");
    Check(matches(L" "), L"legacy whitespace-only filter matches a space in the name");
    Check(!matches(L"  "), L"legacy whitespace-only filter preserves the exact whitespace needle");
    Check(matches(L"\"report final\""), L"quoted ordinary phrase");
    Check(matches(L"#Alpha"), L"legacy tag is fuzzy");
    Check(matches(L"#归档 #Travel"), L"multiple legacy tags remain OR");
    Check(!matches(L"#归档 #missing"), L"legacy OR needs an assigned matching tag");
    Check(matches(L"tag:\"项目 Alpha\""), L"quoted tag with spaces");
    Check(matches(L"TAG:travel"), L"explicit tags are case-insensitive");
    Check(!matches(L"tag:Alpha"), L"explicit tag uses the complete name");
    Check(matches(L"tag:\"项目 Alpha\" tag:Travel"), L"separate tag clauses use AND");
    Check(!matches(L"tag:归档 tag:Travel"), L"AND requires every tag");
    Check(matches(L"tag:归档,Travel"), L"comma alternatives use OR");
    Check(!matches(L"tag:归档,missing"), L"unmatched OR is empty");
    Check(matches(L"tag:\"项目 Alpha\",归档"), L"quoted and plain OR alternatives");
    Check(matches(L"-tag:归档 tag:Travel"), L"negative tag combines with positive tag");
    Check(!matches(L"-tag:Travel"), L"negative tag rejects assigned tag");
    Check(!matches(L"!tag:Travel,归档"), L"negative OR excludes either alternative");
    Check(matches(L"-tag:missing"), L"unknown negative tag excludes nothing");
    Check(!matches(L"tag:missing"), L"unknown positive tag gives empty results");
    Check(matches(L"#Alpha tag:Travel ext:pdf"), L"legacy tags combine with explicit tags and metadata");
    Check(matches(L"tag:Travel report ext:pdf size:>=1mb size:<3mb dm:2026-10 type:document"),
          L"tag name extension size date and type combine");
    Check(!matches(L"tag:Travel size:>2mb"), L"size exclusive bound");
    Check(!matches(L"tag:Travel dm:2026-10-22"), L"date excludes a different local day");
    Check(matches(L"tag:Travel dm:2026-10-21..2026-10-21"), L"date range includes its final day");
    Check(!matches(L"tag:Travel type:image"), L"type uses shared extension categories");
    Check(matches(L"tag:Travel ext:.PDF,txt"), L"extension OR and leading dot");
    Check(!matches(L"tag:Travel -ext:pdf"), L"negative metadata condition");
    Check(matches(L"文件: 类型:文档 大小:>=1mb 日期:2026-10"), L"existing Chinese metadata aliases");
    Check(!matches(L"folder:"), L"file does not match folder condition");
    entry.is_dir = true;
    Check(matches(L"type:folder"), L"folder type uses entry metadata");
    Check(!matches(L"file:"), L"folder does not match file condition");
    entry.is_dir = false;
    Check(matches(L"path:C:\\fixtures"), L"absolute path condition");
    Check(!matches(L"path:C:\\fixture"), L"absolute path prefix ends on a component boundary");
    Check(matches(L"!path:C:\\other"), L"negative absolute path condition");

    const auto without_catalog = model::CompileItemFilter(L"tag:Travel");
    Check(without_catalog.Valid() && !without_catalog.Matches(entry, full),
          L"positive tag without a catalog cannot match");
    Check(model::CompileItemFilter(L"-tag:Travel").Matches(entry, full),
          L"negative tag without a catalog matches untagged entries");
    Check(!model::CompileItemFilter(L"tag:Travel", resolver).Matches(entry, full),
          L"positive tag without assignment lookup cannot match");
    Check(model::CompileItemFilter(L"-tag:归档", resolver).Matches(entry, L"C:\\missing", lookup),
          L"negative tag accepts a path without assignments");

    const auto compiled = model::CompileItemFilter(L"tag:Travel tag:\"项目 Alpha\"", resolver);
    const int before_resolve = resolve_calls;
    const int before_lookup = lookup_calls;
    for (int i = 0; i < 20; ++i) compiled.Matches(entry, full, lookup);
    Check(resolve_calls == before_resolve, L"matching never reparses or resolves tag names");
    Check(lookup_calls == before_lookup + 20, L"assignment lookup runs once per row for all clauses");
    const auto name_reject = model::CompileItemFilter(L"absent tag:Travel", resolver);
    const int before_reject = lookup_calls;
    Check(!name_reject.Matches(entry, full, lookup) && lookup_calls == before_reject,
          L"name rejection avoids assignment lookup");

    for (const auto* text : {L"tag:", L"tag:A,", L"tag:,A", L"tag:A,,B", L"tag:\"\"", L"tag:\"A\"suffix",
                             L"size:nope", L"size:", L"dm:2026-02-30", L"dm:", L"ext:", L"ext:,",
                             L"ext:pdf,", L"type:", L"type:;;;", L"path:", L"size:>18446744073709551615gb"}) {
        const auto invalid = model::CompileItemFilter(text, resolver);
        Check(!invalid.Valid() && !invalid.error_token.empty() && !invalid.Matches(entry, full, lookup), text);
    }
    const auto unknown = model::CompileItemFilter(L"owner:someone", resolver);
    Check(unknown.error == model::ItemFilterError::UnknownCondition, L"unknown structure returns an error");
    const auto content = model::CompileItemFilter(L"content:word", resolver);
    Check(content.error == model::ItemFilterError::UnknownCondition,
          L"unsupported content conditions cannot silently match all rows");
    const auto quote = model::CompileItemFilter(L"tag:\"open", resolver);
    Check(quote.error == model::ItemFilterError::UnterminatedQuote, L"unterminated quote returns an error");
    const auto path_name = model::CompileItemFilter(L"C:\\tag:Travel", resolver);
    Check(path_name.Valid() && !path_name.HasTagConditions() && path_name.name == L"c:\\tag:travel",
          L"drive paths are not tag syntax");
    const auto relative_path = model::CompileItemFilter(L"folder/tag:Travel", resolver);
    Check(relative_path.Valid() && !relative_path.HasTagConditions(), L"path segment is not tag syntax");
    const auto quoted_literal = model::CompileItemFilter(L"\"tag:Travel\"", resolver);
    Check(quoted_literal.Valid() && !quoted_literal.HasTagConditions(), L"quoted literal is not tag syntax");
    Check(model::ItemFilterTagNameMatches(L"Renamed", 0, L"红", true), L"legacy color alias preserved");
    Check(model::ItemFilterTagNameMatches(L"Renamed", 1, L"黄", true), L"legacy yellow alias preserved");
    Check(!model::ItemFilterTagNameMatches(L"Renamed", 0, L"红", false), L"explicit tag has no color alias");
    Check(model::CompileItemFilter(L"tag:\"Alpha, Beta\"", resolver).tags[0].alternatives == std::vector<int>{3},
          L"comma in quoted tag stays part of the name");
    std::wcout << L"Failures: " << failures << L'\n';
    return failures ? 1 : 0;
}
