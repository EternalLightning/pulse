// SPDX-License-Identifier: Apache-2.0
#include "../app/display_alias_search.h"
#include "../model/item_filter.h"
#include "../model/path_aliases.h"
#include <cstdio>
#include <filesystem>

namespace {
int failures = 0;

void Check(bool pass, const char* message) {
    std::printf("[%s] %s\n", pass ? "PASS" : "FAIL", message);
    if (!pass) ++failures;
}
}

int main() {
    namespace fs = std::filesystem;
    using namespace pulse;
    const auto root = fs::absolute(fs::path(L"bench_data") /
        (L"display-alias-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64())));
    fs::create_directories(root);
    struct Cleanup {
        fs::path folder;
        ~Cleanup() { std::error_code error; fs::remove_all(folder, error); }
    } cleanup{root};
    const auto file = root / L"invoice.txt";
    const auto folder = root / L"physical";
    fs::create_directory(folder);
    const HANDLE handle = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, 0, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return 1;
    CloseHandle(handle);

    model::PathAliases aliases;
    std::wstring resolved;
    Check(aliases.LoadJson(L"{\"aliases\":{\"C:\\\\Projects\":\"work\"}}") &&
          aliases.Resolve(L"@work", resolved) == model::PathAliasError::None &&
          resolved == L"C:\\Projects" && !aliases.DisplayNames(), "legacy path aliases load");
    Check(aliases.SetDisplayName(file.wstring(), L"  中国报告  ") &&
          aliases.SetDisplayName(folder.wstring(), L"客户文件") &&
          aliases.SetDisplayName((root / L"missing.txt").wstring(), L"中国报告"), "set file and folder aliases");

    model::PathAliases loaded;
    const auto case_variant = L"\\\\?\\" + (root / L"INVOICE.TXT").wstring();
    Check(loaded.LoadJson(aliases.ToJson()) &&
          model::FindDisplayAlias(loaded.DisplayNames(), case_variant) == L"中国报告" &&
          loaded.Resolve(L"@work", resolved) == model::PathAliasError::None && resolved == L"C:\\Projects",
          "JSON round trip preserves aliases and normalizes Windows paths");
    if (!loaded.DisplayNames()) return 1;

    index::Query query;
    query.needle = L"zgbg ext:txt";
    query.path_prefix = root.wstring();
    const auto hits = app::SearchDisplayAliases(*loaded.DisplayNames(), query);
    Check(hits.size() == 1 && hits[0].hit.path == file.wstring() && hits[0].hit.name == L"invoice.txt" &&
          hits[0].rank > 0, "pinyin alias search keeps original file identity and skips missing files");
    Check(index::MergeSearchResults(query, {}, {}, hits).total == 1,
          "alias-only hits participate in merge and result count");
    query.needle = L"nopinyin: zgbg";
    Check(app::SearchDisplayAliases(*loaded.DisplayNames(), query).empty(), "search respects the pinyin switch");
    query.needle = L"中国报告 ext:jpg";
    Check(app::SearchDisplayAliases(*loaded.DisplayNames(), query).empty(), "file extension matches the disk name");
    query.needle = L"客户";
    query.folders_only = true;
    const auto folders = app::SearchDisplayAliases(*loaded.DisplayNames(), query);
    Check(folders.size() == 1 && folders[0].hit.path == folder.wstring(),
          "folder alias matches a literal query");
    query.path_prefix = (root / L"other").wstring();
    Check(app::SearchDisplayAliases(*loaded.DisplayNames(), query).empty(), "search respects folder scope");
    query.needle = L"invoice | 中国报告";
    query.path_prefix = root.wstring();
    query.folders_only = false;
    Check(app::SearchDisplayAliases(*loaded.DisplayNames(), query).empty(),
          "a normal filename match is not counted twice");

    pulse::fs::DirEntry row;
    row.name = L"invoice.txt";
    const auto alias = model::FindDisplayAlias(loaded.DisplayNames(), file.wstring());
    const auto filter = model::CompileItemFilter(L"中国报告 ext:txt");
    Check(filter.Matches(row, file.wstring(), {}, alias) &&
          model::CompileItemFilter(L"invoice").Matches(row, file.wstring(), {}, alias),
          "directory filter matches both names and keeps original metadata");
    Check(loaded.SetDisplayName(case_variant, L"   ") &&
          model::FindDisplayAlias(loaded.DisplayNames(), file.wstring()).empty() &&
          fs::is_regular_file(file), "clearing an alias leaves the original file in place");
    return failures ? 1 : 0;
}
