// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "../fs/shell_namespace.h"
#include <shlobj.h>
#include <shobjidl.h>
#include <wrl/client.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

namespace pulse::bench {
inline void RunShellLibrariesRegressionChecks(const std::filesystem::path& root,
                                              const std::function<void(bool, const char*)>& check) {
    using Microsoft::WRL::ComPtr;
    namespace sysfs = std::filesystem;
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    struct ComDone { bool initialized; ~ComDone() { if (initialized) CoUninitialize(); } } done{SUCCEEDED(initialized)};
    check(SUCCEEDED(initialized), "library regression uses an STA for native reference enumeration");
    if (FAILED(initialized)) return;

    ComPtr<IShellItem> native_root;
    ComPtr<IEnumShellItems> native_items;
    HRESULT native_error = SHGetKnownFolderItem(FOLDERID_Libraries, KF_FLAG_DEFAULT, nullptr, IID_PPV_ARGS(&native_root));
    if (SUCCEEDED(native_error))
        native_error = native_root->BindToHandler(nullptr, BHID_EnumItems, IID_PPV_ARGS(&native_items));
    PWSTR known_directory = nullptr;
    const HRESULT directory_error = SHGetKnownFolderPath(FOLDERID_Libraries, KF_FLAG_DEFAULT, nullptr, &known_directory);
    const std::wstring directory = SUCCEEDED(directory_error) && known_directory ? known_directory : L"";
    CoTaskMemFree(known_directory);
    std::vector<std::wstring> expected;
    size_t native_without_filesystem_path = 0;
    if (SUCCEEDED(native_error) && native_items) for (;;) {
        ComPtr<IShellItem> item;
        native_error = native_items->Next(1, &item, nullptr);
        if (native_error != S_OK) break;
        if (!item) { native_error = E_UNEXPECTED; break; }
        PWSTR file = nullptr;
        const HRESULT file_error = item->GetDisplayName(SIGDN_FILESYSPATH, &file);
        if (FAILED(file_error) || !file || !*file) ++native_without_filesystem_path;
        CoTaskMemFree(file);
        PWSTR parsing = nullptr;
        const HRESULT parsing_error = item->GetDisplayName(SIGDN_DESKTOPABSOLUTEPARSING, &parsing);
        const std::wstring id = SUCCEEDED(parsing_error) && parsing ? parsing : L"";
        CoTaskMemFree(parsing);
        const auto slash = id.find_last_of(L'\\');
        if (slash == std::wstring::npos) { native_error = E_UNEXPECTED; break; }
        expected.push_back(directory + L"\\" + id.substr(slash + 1));
    }
    check(native_error == S_FALSE && !expected.empty(),
        "native Windows Libraries reference enumeration contains real library entries");
    check(SUCCEEDED(directory_error) && !directory.empty() && !expected.empty() &&
        std::all_of(expected.begin(), expected.end(), [](const auto& file) { return sysfs::is_regular_file(file); }),
        "native library entries correspond to actual user library descriptions without changing them");
    check(native_without_filesystem_path > 0,
        "native regression includes virtual libraries whose FILESYSPATH cannot be read");

    std::vector<fs::DirEntry> rows;
    const HRESULT listed = fs::EnumerateShellNamespace(fs::kLibrariesPath, rows);
    check(SUCCEEDED(listed) && !rows.empty() && rows.size() == expected.size(),
        "Libraries provider preserves every native root entry instead of returning an empty success");
    check(!expected.empty() && std::all_of(expected.begin(), expected.end(), [&](const auto& file) {
        return std::any_of(rows.begin(), rows.end(), [&](const auto& row) {
            return _wcsicmp(row.full_path.c_str(), (std::wstring(fs::kLibraryPrefix) + file).c_str()) == 0 &&
                row.is_dir && !row.name.empty() && !row.shell_id.empty() && fs::IsShellItemEntry(row);
        });
    }), "each virtual native library retains a browsable description path and Shell identity");

    const auto fixture = root / (L"shell-libraries-regression-" + std::to_wstring(GetCurrentProcessId()));
    sysfs::create_directories(fixture / L"member-a");
    sysfs::create_directories(fixture / L"member-b");
    std::ofstream(fixture / L"member-a" / L"from-a.txt") << "a";
    std::ofstream(fixture / L"member-b" / L"from-b.txt") << "b";
    ComPtr<IShellLibrary> library;
    ComPtr<IShellItem> first, second, parent, saved;
    HRESULT created = CoCreateInstance(CLSID_ShellLibrary, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&library));
    if (SUCCEEDED(created)) created = SHCreateItemFromParsingName((fixture / L"member-a").c_str(), nullptr, IID_PPV_ARGS(&first));
    if (SUCCEEDED(created)) created = SHCreateItemFromParsingName((fixture / L"member-b").c_str(), nullptr, IID_PPV_ARGS(&second));
    if (SUCCEEDED(created)) created = library->AddFolder(first.Get());
    if (SUCCEEDED(created)) created = library->AddFolder(second.Get());
    if (SUCCEEDED(created)) created = SHCreateItemFromParsingName(fixture.c_str(), nullptr, IID_PPV_ARGS(&parent));
    if (SUCCEEDED(created)) created = library->Save(parent.Get(), L"Regression library", LSF_OVERRIDEEXISTING, &saved);
    check(SUCCEEDED(created), "create an isolated two-member library description under bench_data");
    const auto library_path = std::wstring(fs::kLibraryPrefix) + (fixture / L"Regression library.library-ms").wstring();
    if (SUCCEEDED(created)) {
        const HRESULT aggregated = fs::EnumerateShellNamespace(library_path, rows);
        check(SUCCEEDED(aggregated) && rows.size() == 2 &&
            std::any_of(rows.begin(), rows.end(), [](const auto& row) { return row.name == L"from-a.txt"; }) &&
            std::any_of(rows.begin(), rows.end(), [](const auto& row) { return row.name == L"from-b.txt"; }),
            "opening a library still aggregates files from both configured locations");
        check(rows.size() == 2 && std::all_of(rows.begin(), rows.end(), [&](const auto& row) {
            return row.shell_id.empty() && !fs::IsVirtualPath(row.full_path) && sysfs::is_regular_file(row.full_path);
        }), "aggregated library files retain physical paths for ordinary filesystem operations");
    }
    size_t cancellation_checks = 0;
    const HRESULT cancelled = fs::EnumerateShellNamespace(fs::kLibrariesPath, rows,
        [&] { return ++cancellation_checks > 1; });
    check(cancelled == HRESULT_FROM_WIN32(ERROR_CANCELLED) && rows.empty(),
        "cancelling an admitted Libraries request discards its rows");
    const auto missing = std::wstring(fs::kLibraryPrefix) + (fixture / L"missing.library-ms").wstring();
    check(FAILED(fs::EnumerateShellNamespace(missing, rows)) && rows.empty(),
        "missing library description reports an error without stale root rows");
}
}
