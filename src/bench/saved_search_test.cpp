// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "../app/saved_search.h"

#include <windows.h>
#include <filesystem>
#include <cstdio>

using namespace pulse::app;

namespace {
int passed = 0;
int failed = 0;
void Check(bool condition, const wchar_t* name) {
    ++(condition ? passed : failed);
    wprintf(L"[%s] %s\n", condition ? L"PASS" : L"FAIL", name);
}
}

int wmain() {
    wchar_t temp[MAX_PATH]{};
    const DWORD length = GetTempPathW(ARRAYSIZE(temp), temp);
    if (!length || length >= ARRAYSIZE(temp)) return 2;
    const auto directory = std::filesystem::path(temp) / (L"pulse-saved-search-" +
        std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    if (!CreateDirectoryW(directory.c_str(), nullptr)) return 2;
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() { std::error_code error; std::filesystem::remove_all(path, error); }
    } cleanup{directory};
    const std::wstring path = (directory / L"searches.json").wstring();

    SavedSearchStore store;
    Check(store.Add({L"源码内容", SavedSearchMode::Content, L"C:\\src", L"TODO", true}),
          L"add content saved search");
    Check(store.Add({L"重复项", SavedSearchMode::Duplicates, L"D:\\files", L"", false}),
          L"add duplicate saved search");
    Check(!store.Add({L"源码内容", SavedSearchMode::Name, L"C:\\", L"name", true}),
          L"reject duplicate name");
    Check(!store.Add({L"invalid", SavedSearchMode::Content, L"C:\\", L"", true}),
          L"reject empty content query");
    Check(store.SaveTo(path), L"atomically save schema v1");

    SavedSearchStore restored;
    Check(restored.LoadFrom(path), L"load schema v1");
    Check(restored.items().size() == 2, L"restore saved search count");
    Check(restored.items().size() == 2 && restored.items()[0].name == L"源码内容" && restored.items()[0].recursive,
          L"restore Unicode fields and options");
    Check(restored.items().size() == 2 && restored.items()[1].mode == SavedSearchMode::Duplicates,
          L"restore duplicate mode");
    Check(restored.Remove(0) && restored.items().size() == 1, L"remove saved search");

    DeleteFileW(path.c_str());
    wprintf(L"%d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
