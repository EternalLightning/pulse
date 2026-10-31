// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once

#include "../index/content_task_search.h"
#include "../index/content_scope.h"
#include <set>

int RunTaskEnumerationCases() {
    namespace fs = std::filesystem;
    using namespace pulse::index;
    const auto base = fs::path(FixtureRoot()).parent_path() /
        (L"content-task-enumeration-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    const auto root = base / L"root", wanted = root / L"wanted", nested = wanted / L"nested";
    const auto other = root / L"other", missing = base / L"missing", empty = base / L"empty";
    auto key = [](const fs::path& path) { return ContentScopeKey(path.wstring()); };
    fs::create_directories(nested);
    fs::create_directories(other);
    fs::create_directories(empty);
    Check(WriteBytes((wanted / L"wanted.txt").wstring(), {'x'}) &&
        WriteBytes((nested / L"nested.txt").wstring(), {'x'}) &&
        WriteBytes((other / L"other.txt").wstring(), {'x'}), L"write isolated task enumeration fixture");
    ContentTaskCache cache;
    cache.version = [](const auto&) { return std::wstring(L"stable"); };
    cache.excluded = [](const auto&) { return false; };
    cache.fresh = [](const auto&, uint64_t, uint64_t, const auto&) { return false; };
    ContentTaskReader reader = [](const auto&, uint64_t, auto& body, auto& bytes, DWORD*,
                                    text::Encoding, const auto&) {
        body = L"enumeration_marker";
        bytes = 1;
        return true;
    };
    ContentIndexConfig config;
    config.roots = {{root.wstring()}, {missing.wstring()}};
    ContentSearchRequest request;
    request.root = wanted.wstring();
    request.needle = L"enumeration_marker";
    std::atomic<bool> cancel{false};
    ContentSearchProgress final;
    std::set<std::wstring> hits, opened;
    ContentTaskEnumerationHooks hooks;
    auto normal_first = [&](const std::wstring& path, WIN32_FIND_DATAW& data) {
        opened.insert(ContentScopeKey(path));
        return FindFirstFileExW((path + L"\\*").c_str(), FindExInfoBasic, &data,
            FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    };
    hooks.first = normal_first;
    auto run = [&] {
        hits.clear(); opened.clear(); final = {};
        return RunContentTaskSupplement(config, request, cancel, cache, {}, 0,
            [&](const auto& progress, auto batch) {
                final = progress;
                for (const auto& hit : batch) hits.insert(ContentScopeKey(hit.path));
                return true;
            }, reader, nullptr, &hooks);
    };
    const bool narrow = run();
    Check(narrow && final.done && !final.error && hits.size() == 2 && !opened.contains(key(missing)) &&
        !opened.contains(key(other)), L"narrow task skips unrelated missing roots and sibling directories");
    request.recursive = false;
    const bool flat = run();
    Check(flat && !final.error && hits == std::set<std::wstring>{key(wanted / L"wanted.txt")} &&
        !opened.contains(key(nested)), L"nonrecursive task does not enumerate descendants");
    request.root = nested.wstring();
    const bool deep = run();
    Check(deep && !final.error && hits == std::set<std::wstring>{key(nested / L"nested.txt")} &&
        opened.contains(key(wanted)), L"deep nonrecursive task retains ancestors leading to query root");
    request.recursive = true;
    request.root.clear();
    request.roots = {nested.wstring(), other.wstring()};
    const bool multiple = run();
    Check(multiple && !final.error && hits.size() == 2 &&
        hits.contains(key(nested / L"nested.txt")) && hits.contains(key(other / L"other.txt")),
        L"multiple task roots retain both disjoint scopes");
    request.root = wanted.wstring();
    const bool intersection = run();
    Check(intersection && !final.error && hits.size() == 2 && hits.contains(key(nested / L"nested.txt")) &&
        hits.contains(key(other / L"other.txt")), L"task roots override legacy single root");
    config.roots = {{empty.wstring()}};
    request.root.clear(); request.roots.clear();
    for (DWORD code : {DWORD{ERROR_FILE_NOT_FOUND}, DWORD{ERROR_NO_MORE_FILES}}) {
        hooks.first = [code](const auto&, auto&) { SetLastError(code); return INVALID_HANDLE_VALUE; };
        const bool complete = run();
        Check(complete && final.done && !final.error && final.total_files == 0 && hits.empty(),
            code == ERROR_FILE_NOT_FOUND ? L"empty FindFirst FILE_NOT_FOUND completes successfully" :
                L"empty FindFirst NO_MORE_FILES completes successfully");
    }
    hooks.first = [](const auto&, auto&) { SetLastError(ERROR_ACCESS_DENIED); return INVALID_HANDLE_VALUE; };
    const bool denied = run();
    Check(!denied && final.done && final.error == ERROR_ACCESS_DENIED,
        L"in-scope FindFirst access error stays incomplete");
    config.roots = {{wanted.wstring()}};
    request.root = wanted.wstring(); request.recursive = false;
    hooks.first = normal_first;
    hooks.next = [](HANDLE find, WIN32_FIND_DATAW& data) {
        if (!wcscmp(data.cFileName, L"wanted.txt")) {
            SetLastError(ERROR_READ_FAULT);
            return false;
        }
        return FindNextFileW(find, &data) != FALSE;
    };
    const bool partial = run();
    Check(!partial && final.done && final.error == ERROR_READ_FAULT && final.total_files == 0 &&
        hits.contains(key(wanted / L"wanted.txt")),
        L"abnormal FindNext reports incomplete scan and preserves confirmed hit");
    hooks.next = {};
    const bool eof = run();
    Check(eof && final.done && !final.error && final.total_files == 1 && hits.size() == 1,
        L"normal FindNext EOF reports complete scan");
    if (!failed) fs::remove_all(base);
    else wprintf(L"Fixture retained: %s\n", base.c_str());
    wprintf(L"%d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
