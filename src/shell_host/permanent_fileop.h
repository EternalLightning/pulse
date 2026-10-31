// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once

#include "../common/file_identity.h"
#include "../common/path_utils.h"
#include "permanent_delete_parallel.h"
#include <windows.h>
#include <functional>
#include <memory>
#include <new>
#include <string>
#include <vector>

namespace pulse::shell {

struct PermanentDeleteResult {
    HRESULT error = S_OK;
    bool cancelled = false;
    std::wstring failed_path;
    std::vector<std::wstring> deleted;
};

enum class PermanentDeleteScheduling { Automatic, Serial, ParallelForTesting };

struct PermanentDeleteOptions {
    PermanentDeleteScheduling scheduling = PermanentDeleteScheduling::Automatic;
    // Fault injection must be thread-safe when parallel scheduling is enabled.
    decltype(&SetFileInformationByHandle) set_information = &SetFileInformationByHandle;
};

class PermanentDeletionItem {
public:
    bool Capture(const std::wstring& requested, const FileIdentity& expected, DWORD& error) {
        object_.reset(); parents_.clear(); path_.clear();
        auto plain = path::StripExtendedPathPrefix(requested);
        if (!RootEnd(plain)) { error = ERROR_INVALID_NAME; return false; }
        const DWORD needed = GetFullPathNameW(plain.c_str(), 0, nullptr, nullptr);
        if (!needed) { error = GetLastError(); return false; }
        std::wstring canonical(needed, L'\0');
        const DWORD length = GetFullPathNameW(plain.c_str(), needed, canonical.data(), nullptr);
        if (!length || length >= needed) { error = ERROR_INVALID_NAME; return false; }
        canonical.resize(length);
        plain = std::move(canonical);
        const size_t root_end = RootEnd(plain);
        while (plain.size() > root_end && plain.back() == L'\\') plain.pop_back();
        if (!root_end || plain.size() <= root_end || plain.find(L'/', root_end) != std::wstring::npos) {
            error = ERROR_INVALID_NAME; return false;
        }
        path_ = Extended(plain);
        // Keep every named ancestor fixed while child paths are opened. Reparse
        // ancestors are rejected; a selected reparse leaf is deleted as a leaf.
        for (size_t slash = plain.find(L'\\', root_end); slash != std::wstring::npos;
             slash = plain.find(L'\\', slash + 1)) {
            const auto parent = Extended(plain.substr(0, slash));
            auto handle = Open(parent, 0);
            if (!handle) { error = GetLastError(); return false; }
            FILE_BASIC_INFO info{};
            if (!GetFileInformationByHandleEx(handle.get(), FileBasicInfo, &info, sizeof(info))) {
                error = GetLastError(); return false;
            }
            if (!(info.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
                (info.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
                error = ERROR_NOT_SUPPORTED; return false;
            }
            parents_.push_back(std::move(handle));
        }
        object_ = Open(path_, DELETE | FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES);
        if (!object_) { error = GetLastError(); return false; }
        FileIdentity current;
        if (!expected.Valid() || !ReadFileIdentity(object_.get(), current) || current != expected) {
            error = ERROR_FILE_INVALID; return false;
        }
        error = ERROR_SUCCESS;
        return true;
    }

    bool Delete(const std::function<bool()>& cancelled, std::wstring& failed_path, DWORD& error,
        decltype(&SetFileInformationByHandle) set_information = &SetFileInformationByHandle, bool overlap = false) {
        return Remove(object_, path_, cancelled, failed_path, error, set_information, overlap);
    }

private:
    using Handle = std::shared_ptr<void>;
    static Handle Open(const std::wstring& name, DWORD access) {
        const HANDLE value = CreateFileW(name.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (value == INVALID_HANDLE_VALUE) return {};
        return Handle(value, [](void* handle) { CloseHandle(handle); });
    }
    static size_t RootEnd(const std::wstring& name) {
        if (name.size() >= 3 && name[1] == L':' && name[2] == L'\\') return 3;
        if (!name.starts_with(L"\\\\")) return 0;
        const auto server = name.find(L'\\', 2);
        const auto share = server == std::wstring::npos ? server : name.find(L'\\', server + 1);
        return share == std::wstring::npos ? 0 : share + 1;
    }
    static std::wstring Extended(const std::wstring& plain) {
        return plain.starts_with(L"\\\\") ? L"\\\\?\\UNC\\" + plain.substr(2) : L"\\\\?\\" + plain;
    }
    static bool Remove(Handle& handle, const std::wstring& name, const std::function<bool()>& cancelled,
        std::wstring& failed_path, DWORD& error, decltype(&SetFileInformationByHandle) set_information, bool overlap) {
        failed_path = name;
        if (cancelled && cancelled()) { error = ERROR_CANCELLED; return false; }
        FILE_BASIC_INFO info{};
        if (!GetFileInformationByHandleEx(handle.get(), FileBasicInfo, &info, sizeof(info))) {
            error = GetLastError(); return false;
        }
        if ((info.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(info.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
            const auto remove_child = [&](const std::wstring& child_name, const std::function<bool()>& child_cancelled,
                bool child_overlap, std::wstring& child_failed, DWORD& child_error) {
                if (child_cancelled && child_cancelled()) {
                    child_failed = child_name; child_error = ERROR_CANCELLED; return false;
                }
                auto child = Open(child_name, DELETE | FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES);
                if (!child) {
                    child_error = GetLastError();
                    if (child_error == ERROR_FILE_NOT_FOUND || child_error == ERROR_PATH_NOT_FOUND) return true;
                    child_failed = child_name; return false;
                }
                return Remove(child, child_name, child_cancelled, child_failed, child_error, set_information, child_overlap);
            };
            std::vector<std::wstring> children;
            const auto remove_batch = [&] {
                if (children.size() < 32) {
                    for (const auto& child_name : children)
                        if (!remove_child(child_name, cancelled, overlap, failed_path, error)) return false;
                } else {
                    struct Outcome { bool attempted = false; bool success = false; DWORD error = ERROR_SUCCESS; std::wstring path; };
                    std::vector<Outcome> outcomes(children.size());
                    for (size_t i = 0; i < children.size(); ++i) outcomes[i].path = children[i];
                    detail::RunPermanentDeleteBatch(children.size(), cancelled,
                        [&](size_t i, const std::function<bool()>& worker_cancelled) {
                            auto& outcome = outcomes[i];
                            outcome.attempted = true;
                            try {
                                outcome.success = remove_child(children[i], worker_cancelled, false, outcome.path, outcome.error);
                            } catch (const std::bad_alloc&) { outcome.error = ERROR_NOT_ENOUGH_MEMORY; }
                            catch (...) { outcome.error = ERROR_UNHANDLED_EXCEPTION; }
                            return outcome.success;
                        }, [](size_t) {});
                    for (const auto& outcome : outcomes) {
                        if (outcome.attempted && !outcome.success) {
                            failed_path = outcome.path; error = outcome.error; return false;
                        }
                    }
                    if (std::any_of(outcomes.begin(), outcomes.end(), [](const auto& outcome) { return !outcome.attempted; })) {
                        failed_path = name; error = ERROR_CANCELLED; return false;
                    }
                }
                children.clear();
                return true;
            };
            WIN32_FIND_DATAW entry{};
            const HANDLE raw_find = FindFirstFileW((name + L"\\*").c_str(), &entry);
            if (raw_find != INVALID_HANDLE_VALUE) {
                const auto find = std::shared_ptr<void>(raw_find, [](void* value) { FindClose(value); });
                do {
                    if (overlap && cancelled && cancelled()) { error = ERROR_CANCELLED; return false; }
                    if (wcscmp(entry.cFileName, L".") == 0 || wcscmp(entry.cFileName, L"..") == 0) continue;
                    const auto child_name = name + L"\\" + entry.cFileName;
                    if (overlap) {
                        children.push_back(child_name);
                        // Bound names, results and each pool's work even in
                        // directories containing millions of children.
                        if (children.size() == 256 && !remove_batch()) return false;
                    }
                    else if (!remove_child(child_name, cancelled, false, failed_path, error)) return false;
                } while (FindNextFileW(raw_find, &entry));
                error = GetLastError();
                if (error != ERROR_NO_MORE_FILES) { failed_path = name; return false; }
            } else {
                error = GetLastError();
                if (error != ERROR_FILE_NOT_FOUND) return false;
            }
            if (!remove_batch()) return false;
        }
        failed_path = name;
        if (cancelled && cancelled()) { error = ERROR_CANCELLED; return false; }
        const auto original_info = info;
        if (info.FileAttributes & FILE_ATTRIBUTE_READONLY) {
            info.FileAttributes &= ~FILE_ATTRIBUTE_READONLY;
            if (!info.FileAttributes) info.FileAttributes = FILE_ATTRIBUTE_NORMAL;
            if (!set_information(handle.get(), FileBasicInfo, &info, sizeof(info))) {
                error = GetLastError(); return false;
            }
        }
        FILE_DISPOSITION_INFO disposition{TRUE};
        if (!set_information(handle.get(), FileDispositionInfo, &disposition, sizeof(disposition))) {
            error = GetLastError();
            if (original_info.FileAttributes & FILE_ATTRIBUTE_READONLY) {
                auto restore = original_info;
                set_information(handle.get(), FileBasicInfo, &restore, sizeof(restore));
            }
            return false;
        }
        handle.reset();
        error = ERROR_SUCCESS;
        return true;
    }
    std::wstring path_;
    std::vector<Handle> parents_;
    Handle object_;
};

inline PermanentDeleteResult DeletePermanentAuthorized(const std::vector<std::wstring>& paths,
    const std::vector<FileIdentity>& expected, const std::function<bool()>& cancelled,
    const std::function<void(size_t, size_t, const std::wstring&)>& progress = {},
    const PermanentDeleteOptions& options = {}) {
    PermanentDeleteResult result;
    if (paths.empty() || paths.size() != expected.size()) { result.error = E_INVALIDARG; return result; }
    std::vector<PermanentDeletionItem> captured(paths.size());
    DWORD error = ERROR_SUCCESS;
    for (size_t i = 0; i < paths.size(); ++i) {
        result.failed_path = paths[i];
        if (cancelled && cancelled()) { error = ERROR_CANCELLED; break; }
        if (!captured[i].Capture(paths[i], expected[i], error)) break;
    }
    if (error == ERROR_SUCCESS) {
        const auto overlap = [&](const std::wstring& name) {
            return options.scheduling == PermanentDeleteScheduling::ParallelForTesting ||
                (options.scheduling == PermanentDeleteScheduling::Automatic && detail::PermanentDeletesOverlapOn(name));
        };
        if (paths.size() >= 8 && std::all_of(paths.begin(), paths.end(), overlap)) {
            struct Outcome { bool attempted = false; bool success = false; DWORD error = ERROR_SUCCESS; std::wstring path; };
            std::vector<Outcome> outcomes(paths.size());
            for (size_t i = 0; i < paths.size(); ++i) outcomes[i].path = paths[i];
            detail::RunPermanentDeleteBatch(paths.size(), cancelled,
                [&](size_t i, const std::function<bool()>& worker_cancelled) {
                    auto& outcome = outcomes[i];
                    outcome.attempted = true;
                    try {
                        outcome.success = captured[i].Delete(worker_cancelled, outcome.path, outcome.error, options.set_information);
                    } catch (const std::bad_alloc&) { outcome.error = ERROR_NOT_ENOUGH_MEMORY; }
                    catch (...) { outcome.error = ERROR_UNHANDLED_EXCEPTION; }
                    return outcome.success;
                }, [&](size_t i) {
                    if (outcomes[i].success) {
                        result.deleted.push_back(paths[i]);
                        if (progress) progress(result.deleted.size(), paths.size(), paths[i]);
                    }
                });
            bool incomplete = false;
            for (size_t i = 0; i < outcomes.size(); ++i) {
                const auto& outcome = outcomes[i];
                if (outcome.attempted && !outcome.success && error == ERROR_SUCCESS) {
                    result.failed_path = outcome.path; error = outcome.error;
                }
                incomplete = incomplete || !outcome.attempted;
            }
            if (incomplete && error == ERROR_SUCCESS) error = ERROR_CANCELLED;
        } else {
            for (size_t i = 0; i < paths.size(); ++i) {
                if (!captured[i].Delete(cancelled, result.failed_path, error, options.set_information, overlap(paths[i]))) break;
                result.deleted.push_back(paths[i]);
                if (progress) progress(result.deleted.size(), paths.size(), paths[i]);
            }
        }
    }
    result.cancelled = error == ERROR_CANCELLED;
    result.error = HRESULT_FROM_WIN32(error);
    if (error == ERROR_SUCCESS) result.failed_path.clear();
    return result;
}

} // namespace pulse::shell
