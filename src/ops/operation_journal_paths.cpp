// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "operation_journal_paths.h"
#include <windows.h>
#include <atomic>
#include <algorithm>
#include <cstdint>

namespace pulse::ops {
namespace {

std::wstring Join(const std::wstring& directory, const std::wstring& name) {
    if (directory.empty()) return {};
    return directory + (directory.back() == L'\\' ? L"" : L"\\") + name;
}

std::wstring CreateWindowJournalPath(const std::wstring& data_dir) {
    static std::atomic<uint64_t> sequence{0};
    for (unsigned attempt = 0; attempt < 64; ++attempt) {
        const std::wstring name = L"operations-window-" +
            std::to_wstring(GetCurrentProcessId()) + L"-" +
            std::to_wstring(GetTickCount64()) + L"-" +
            std::to_wstring(sequence.fetch_add(1, std::memory_order_relaxed)) + L".json";
        const std::wstring path = Join(data_dir, name);
        HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
            FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            constexpr char empty_journal[] = "{\n  \"schema\":1,\n  \"entries\":[]\n}\n";
            constexpr DWORD empty_journal_bytes = static_cast<DWORD>(sizeof(empty_journal) - 1);
            DWORD written = 0;
            const bool initialized = WriteFile(file, empty_journal,
                empty_journal_bytes, &written, nullptr) &&
                written == empty_journal_bytes && FlushFileBuffers(file);
            CloseHandle(file);
            if (initialized) return path;
            DeleteFileW(path.c_str());
            return {};
        }
        const DWORD error = GetLastError();
        if (error != ERROR_FILE_EXISTS && error != ERROR_ALREADY_EXISTS)
            return {};
    }
    return {};
}

} // namespace

OperationJournalPaths AssignOperationJournalPaths(const std::wstring& data_dir,
                                                  bool recovery_owner) {
    OperationJournalPaths result;
    if (data_dir.empty()) return result;
    const std::wstring legacy = Join(data_dir, L"operations.json");
    if (!recovery_owner) {
        result.window_path = CreateWindowJournalPath(data_dir);
        if (!result.window_path.empty()) result.paths.push_back(result.window_path);
        return result;
    }

    result.window_path = legacy;
    result.paths.push_back(legacy);
    const std::wstring pattern = Join(data_dir, L"operations-window-*.json");
    WIN32_FIND_DATAW data{};
    HANDLE find = FindFirstFileW(pattern.c_str(), &data);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            const std::wstring path = Join(data_dir, data.cFileName);
            if (std::find(result.paths.begin(), result.paths.end(), path) ==
                result.paths.end()) result.paths.push_back(path);
        } while (FindNextFileW(find, &data));
        FindClose(find);
    }
    return result;
}

} // namespace pulse::ops
