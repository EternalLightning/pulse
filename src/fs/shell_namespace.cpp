// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
#include "shell_namespace.h"
#include "device_worker.h"
#include "../common/shell_id.h"
#include <shlobj.h>
#include <shobjidl.h>
#include <propkey.h>
#include <wrl/client.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cwctype>
#include <mutex>
#include <string_view>
#include <unordered_set>

using Microsoft::WRL::ComPtr;
namespace pulse::fs {
namespace {
constexpr HRESULT kCancelled = HRESULT_FROM_WIN32(ERROR_CANCELLED);

std::wstring DisplayName(IShellItem* item, SIGDN form) {
    PWSTR raw = nullptr;
    const HRESULT hr = item->GetDisplayName(form, &raw);
    std::wstring value = SUCCEEDED(hr) && raw ? raw : L"";
    CoTaskMemFree(raw);
    return value;
}

HRESULT ListItems(IShellItem* folder, const std::wstring& path,
                  std::vector<DirEntry>& rows, const std::atomic_bool& cancel) {
    std::wstring libraries_directory;
    if (path == kLibrariesPath) {
        PWSTR directory = nullptr;
        const HRESULT hr = SHGetKnownFolderPath(FOLDERID_Libraries, KF_FLAG_DEFAULT, nullptr, &directory);
        if (SUCCEEDED(hr) && directory) libraries_directory = directory;
        CoTaskMemFree(directory);
        if (FAILED(hr)) return hr;
        if (libraries_directory.empty()) return E_UNEXPECTED;
    }
    ComPtr<IEnumShellItems> items;
    HRESULT hr = folder->BindToHandler(nullptr, BHID_EnumItems, IID_PPV_ARGS(&items));
    if (FAILED(hr)) return hr;
    if (!items) return E_UNEXPECTED;
    for (;;) {
        if (cancel.load()) return kCancelled;
        ComPtr<IShellItem> item;
        hr = items->Next(1, &item, nullptr);
        if (hr == S_FALSE) return S_OK;
        if (FAILED(hr)) return hr;
        if (hr != S_OK || !item) return E_UNEXPECTED;
        DirEntry row;
        row.name = DisplayName(item.Get(), SIGDN_NORMALDISPLAY);
        row.shell_id = shell_id::FromItem(item.Get());
        if (row.name.empty() || row.shell_id.empty()) continue;
        if (path == kLibrariesPath) {
            // Libraries are .library-ms descriptions, never their default save location.
            auto file = DisplayName(item.Get(), SIGDN_FILESYSPATH);
            if (file.empty()) {
                // Native library items are virtual: FILESYSPATH returns E_INVALIDARG.
                const auto parsing = DisplayName(item.Get(), SIGDN_DESKTOPABSOLUTEPARSING);
                const auto slash = parsing.find_last_of(L'\\');
                if (slash == std::wstring::npos) return E_UNEXPECTED;
                file = libraries_directory + L"\\" + parsing.substr(slash + 1);
            }
            if (!IsLibraryPath(std::wstring(kLibraryPrefix) + file)) return E_UNEXPECTED;
            row.full_path = std::wstring(kLibraryPrefix) + file;
            row.is_dir = true;
        } else {
            // Apps and applets stay Shell objects even when backed by an EXE or shortcut.
            row.full_path = std::wstring(kShellItemPrefix) + row.shell_id;
        }
        row.attrs = row.is_dir ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
        if (!row.shell_id.empty()) row.attrs |= FILE_ATTRIBUTE_READONLY;
        ComPtr<IShellItem2> details;
        if (SUCCEEDED(item.As(&details))) {
            ULONGLONG size = 0;
            if (!row.is_dir && SUCCEEDED(details->GetUInt64(PKEY_Size, &size))) row.size = size;
            details->GetFileTime(PKEY_DateModified, &row.mtime);
        }
        rows.push_back(std::move(row));
    }
}

HRESULT EnumerateOnSta(const std::wstring& path, std::vector<DirEntry>& rows,
                       const std::atomic_bool& cancel) {
    ComPtr<IShellItem> folder;
    HRESULT hr = S_OK;
    if (IsLibraryPath(path)) {
        const auto file = path.substr(std::wstring_view(kLibraryPrefix).size());
        hr = SHCreateItemFromParsingName(file.c_str(), nullptr, IID_PPV_ARGS(&folder));
        if (FAILED(hr)) return hr;
        ComPtr<IShellLibrary> library;
        hr = SHLoadLibraryFromItem(folder.Get(), STGM_READ, IID_PPV_ARGS(&library));
        if (FAILED(hr)) return hr;
        ComPtr<IShellItemArray> members;
        hr = library->GetFolders(LFF_FORCEFILESYSTEM, IID_PPV_ARGS(&members));
        if (FAILED(hr)) return hr;
        DWORD count = 0;
        hr = members->GetCount(&count);
        if (FAILED(hr)) return hr;
        std::vector<std::wstring> locations;
        for (DWORD index = 0; index < count; ++index) {
            if (cancel.load()) return kCancelled;
            ComPtr<IShellItem> member;
            hr = members->GetItemAt(index, &member);
            if (FAILED(hr)) return hr;
            auto location = DisplayName(member.Get(), SIGDN_FILESYSPATH);
            if (location.empty()) return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
            locations.push_back(std::move(location));
        }
        return EnumerateLibraryLocations(locations, rows, [&] { return cancel.load(); });
    }
    if (path == kLibrariesPath)
        hr = SHGetKnownFolderItem(FOLDERID_Libraries, KF_FLAG_DEFAULT, nullptr, IID_PPV_ARGS(&folder));
    else if (path == kControlPanelPath)
        hr = SHGetKnownFolderItem(FOLDERID_ControlPanelFolder, KF_FLAG_DEFAULT, nullptr, IID_PPV_ARGS(&folder));
    else if (path == kApplicationsPath)
        hr = SHCreateItemFromParsingName(L"shell:AppsFolder", nullptr, IID_PPV_ARGS(&folder));
    else return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
    if (FAILED(hr)) return hr;
    return ListItems(folder.Get(), path, rows, cancel);
}
}

bool IsLibraryPath(const std::wstring& path) {
    constexpr std::wstring_view prefix(kLibraryPrefix);
    if (!path.starts_with(prefix)) return false;
    const auto file = path.substr(prefix.size());
    if (file.size() < 12 || _wcsicmp(file.c_str() + file.size() - 11, L".library-ms") != 0) return false;
    return (file.size() > 2 && iswalpha(file[0]) && file[1] == L':' && file[2] == L'\\') || IsUncPath(file);
}

bool IsShellNamespacePath(const std::wstring& path) {
    return path == kLibrariesPath || path == kControlPanelPath || path == kApplicationsPath || IsLibraryPath(path);
}

bool IsShellItemEntry(const DirEntry& entry) {
    return IsLibraryPath(entry.full_path) || entry.full_path.starts_with(kShellItemPrefix);
}

std::wstring ShellNamespaceParent(const std::wstring& path) {
    return IsLibraryPath(path) ? kLibrariesPath : L"pulse:home";
}

HRESULT EnumerateLibraryLocations(const std::vector<std::wstring>& locations,
                                 std::vector<DirEntry>& out, const std::function<bool()>& cancelled) {
    out.clear();
    std::vector<DirEntry> rows;
    std::unordered_set<std::wstring> seen;
    try {
        for (const auto& location : locations) {
            if (cancelled && cancelled()) return kCancelled;
            std::vector<DirEntry> member;
            if (!EnumerateDirectory(location, member, cancelled)) return kCancelled;
            for (auto& row : member) {
                if (cancelled && cancelled()) return kCancelled;
                row.full_path = NormalizePath(location + (location.ends_with(L"\\") ? L"" : L"\\") + row.name);
                std::wstring key = row.full_path;
                for (auto& c : key) c = static_cast<wchar_t>(towlower(c));
                if (seen.insert(std::move(key)).second) rows.push_back(std::move(row));
            }
        }
    } catch (const EnumerationError& error) {
        return HRESULT_FROM_WIN32(error.Code());
    } catch (...) { return E_FAIL; }
    out = std::move(rows);
    return S_OK;
}

HRESULT EnumerateShellNamespace(const std::wstring& path, std::vector<DirEntry>& out,
                               const std::function<bool()>& cancelled) {
    out.clear();
    if (!IsShellNamespacePath(path)) return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
    if (cancelled && cancelled()) return kCancelled;
    struct Job {
        std::mutex mutex;
        std::condition_variable cv;
        std::atomic_bool cancel{false};
        bool done = false;
        HRESULT error = E_PENDING;
        std::vector<DirEntry> rows;
    };
    auto job = std::make_shared<Job>();
    static DeviceWorker worker;
    worker.Post([job, path] {
        std::vector<DirEntry> rows;
        HRESULT error = E_FAIL;
        try { error = EnumerateOnSta(path, rows, job->cancel); }
        catch (...) { error = E_FAIL; }
        std::lock_guard lock(job->mutex);
        if (job->done) return;
        job->error = error;
        if (SUCCEEDED(error)) job->rows = std::move(rows);
        job->done = true;
        job->cv.notify_all();
    }, 15000, [job] {
        std::lock_guard lock(job->mutex);
        if (job->done) return;
        job->cancel = true;
        job->error = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
        job->done = true;
        job->cv.notify_all();
    });
    std::unique_lock lock(job->mutex);
    while (!job->done) {
        job->cv.wait_for(lock, std::chrono::milliseconds(50));
        lock.unlock();
        const bool stop = cancelled && cancelled();
        lock.lock();
        if (stop) { job->cancel = true; return kCancelled; }
    }
    if (SUCCEEDED(job->error)) out = std::move(job->rows);
    return job->error;
}
}
