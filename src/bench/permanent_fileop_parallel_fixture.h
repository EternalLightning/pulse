// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once

#include "permanent_fileop_fixture.h"
#include <atomic>
#include <set>
#include <winioctl.h>

namespace pulse::bench {
namespace permanent_parallel_fixture {
inline std::atomic<unsigned> active{0}, peak{0}, dispositions{0}, successes{0};
inline FileIdentity rejected_identity;

inline BOOL WINAPI ObserveDisposition(HANDLE handle, FILE_INFO_BY_HANDLE_CLASS kind, LPVOID value, DWORD bytes) {
    if (kind != FileDispositionInfo) return SetFileInformationByHandle(handle, kind, value, bytes);
    const auto running = active.fetch_add(1) + 1;
    auto maximum = peak.load();
    while (maximum < running && !peak.compare_exchange_weak(maximum, running)) {}
    dispositions.fetch_add(1);
    Sleep(20);
    FileIdentity current;
    const bool reject = rejected_identity.Valid() && ReadFileIdentity(handle, current) &&
        current == rejected_identity;
    // Exercise a failed root together with completed peers deterministically.
    if (reject) {
        const auto deadline = GetTickCount64() + 2000;
        while (!successes.load() && GetTickCount64() < deadline) Sleep(1);
    }
    const BOOL success = reject ? FALSE : SetFileInformationByHandle(handle, kind, value, bytes);
    const DWORD error = reject ? ERROR_ACCESS_DENIED : GetLastError();
    if (success) successes.fetch_add(1);
    active.fetch_sub(1);
    SetLastError(error);
    return success;
}

inline void Reset(const std::wstring& reject = {}) {
    active.store(0); peak.store(0); dispositions.store(0); successes.store(0);
    rejected_identity = {};
    if (!reject.empty() && !ReadPathFileIdentity(reject, rejected_identity))
        throw std::runtime_error("capture rejected fixture identity");
}

inline bool CreateDirectoryLink(const std::filesystem::path& link, const std::filesystem::path& target) {
    if (CreateSymbolicLinkW(link.c_str(), target.c_str(), SYMBOLIC_LINK_FLAG_DIRECTORY | 0x2)) return true;
    if (!CreateDirectoryW(link.c_str(), nullptr)) return false;
    const HANDLE handle = CreateFileW(link.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    bool success = false;
    DWORD error = GetLastError();
    if (handle != INVALID_HANDLE_VALUE) {
        struct Junction {
            DWORD tag;
            WORD length, reserved, substitute_offset, substitute_length, print_offset, print_length;
            wchar_t path[32768];
        } buffer{};
        const auto substitute = L"\\??\\" + target.wstring();
        const auto printed = target.wstring();
        const size_t characters = substitute.size() + printed.size() + 2;
        if (characters < ARRAYSIZE(buffer.path) && characters * sizeof(wchar_t) + 8 <= MAXWORD) {
            buffer.tag = IO_REPARSE_TAG_MOUNT_POINT;
            buffer.substitute_length = static_cast<WORD>(substitute.size() * sizeof(wchar_t));
            buffer.print_offset = static_cast<WORD>((substitute.size() + 1) * sizeof(wchar_t));
            buffer.print_length = static_cast<WORD>(printed.size() * sizeof(wchar_t));
            buffer.length = static_cast<WORD>(8 + characters * sizeof(wchar_t));
            std::copy(substitute.begin(), substitute.end(), buffer.path);
            std::copy(printed.begin(), printed.end(), buffer.path + substitute.size() + 1);
            DWORD returned = 0;
            success = DeviceIoControl(handle, FSCTL_SET_REPARSE_POINT, &buffer, buffer.length + 8,
                nullptr, 0, &returned, nullptr) != FALSE;
            error = GetLastError();
        }
        CloseHandle(handle);
    }
    if (!success) { RemoveDirectoryW(link.c_str()); SetLastError(error); }
    return success;
}
} // namespace permanent_parallel_fixture

template <typename Check>
void RunPermanentParallelFixture(const std::filesystem::path& root, Check&& check) {
    namespace fs = std::filesystem;
    namespace probe = permanent_parallel_fixture;
    shell::PermanentDeleteOptions options{shell::PermanentDeleteScheduling::ParallelForTesting, &probe::ObserveDisposition};
    const DWORD owner = GetCurrentThreadId();
    bool callbacks_on_owner = true;
    bool progress_valid = true;
    std::vector<std::wstring> paths;
    std::vector<FileIdentity> identities;
    const auto make_roots = [&](const wchar_t* prefix, size_t count) {
        paths.clear(); identities.clear();
        for (size_t i = 0; i < count; ++i) {
            const auto name = root / (std::wstring(prefix) + std::to_wstring(i) + L".txt");
            std::ofstream(name) << "parallel fixture";
            FileIdentity identity;
            if (!ReadPathFileIdentity(name.wstring(), identity)) throw std::runtime_error("capture parallel fixture identity");
            paths.push_back(name.wstring()); identities.push_back(identity);
        }
    };
    const auto accounted = [&](const shell::PermanentDeleteResult& result) {
        const std::set<std::wstring> reported(result.deleted.begin(), result.deleted.end());
        if (reported.size() != result.deleted.size()) return false;
        for (const auto& name : paths)
            if (reported.contains(name) == fs::exists(name)) return false;
        return true;
    };
    size_t updates = 0;
    const auto progress = [&](size_t done, size_t total, const std::wstring& name) {
        callbacks_on_owner = callbacks_on_owner && GetCurrentThreadId() == owner;
        progress_valid = progress_valid && done == ++updates && total == paths.size() && !fs::exists(name);
    };
    const auto never_cancel = [&] {
        callbacks_on_owner = callbacks_on_owner && GetCurrentThreadId() == owner;
        return false;
    };

    make_roots(L"success-", 40);
    probe::Reset();
    auto result = shell::DeletePermanentAuthorized(paths, identities, never_cancel, progress, options);
    check(SUCCEEDED(result.error) && result.deleted.size() == paths.size() && accounted(result) &&
        probe::peak.load() >= 2 && probe::peak.load() <= 8, "parallel selected roots delete with at most eight active dispositions");
    check(callbacks_on_owner && progress_valid && updates == paths.size(),
        "parallel cancellation and exact completed-root progress callbacks stay on the caller thread");

    make_roots(L"serial-", 8);
    probe::Reset();
    options.scheduling = shell::PermanentDeleteScheduling::Serial;
    result = shell::DeletePermanentAuthorized(paths, identities, never_cancel, {}, options);
    check(SUCCEEDED(result.error) && accounted(result) && probe::peak.load() == 1,
        "local serial policy preserves one-at-a-time deletion");
    options.scheduling = shell::PermanentDeleteScheduling::ParallelForTesting;

    make_roots(L"identity-", 8);
    auto mismatched = identities;
    mismatched.back() = identities.front();
    probe::Reset();
    result = shell::DeletePermanentAuthorized(paths, mismatched, never_cancel, {}, options);
    check(result.error == HRESULT_FROM_WIN32(ERROR_FILE_INVALID) && result.deleted.empty() &&
        probe::dispositions.load() == 0 && accounted(result),
        "parallel policy captures every root identity before any mutation");

    make_roots(L"cancel-", 64);
    probe::Reset();
    size_t cancel_calls = 0;
    result = shell::DeletePermanentAuthorized(paths, identities, [&] {
        callbacks_on_owner = callbacks_on_owner && GetCurrentThreadId() == owner;
        ++cancel_calls;
        return probe::dispositions.load() != 0;
    }, {}, options);
    check(result.cancelled && !result.deleted.empty() && result.deleted.size() < paths.size() &&
        accounted(result) && probe::peak.load() <= 8 && callbacks_on_owner && cancel_calls > paths.size(),
        "mid-flight cancellation joins workers and reports every root actually deleted");

    make_roots(L"failure-", 24);
    const auto rejected = paths.front();
    check(SetFileAttributesW(rejected.c_str(), FILE_ATTRIBUTE_READONLY) != FALSE, "prepare parallel read-only failure");
    probe::Reset(rejected);
    result = shell::DeletePermanentAuthorized(paths, identities, never_cancel, {}, options);
    const DWORD restored = GetFileAttributesW(rejected.c_str());
    std::cout << "[DIAG] parallel failure hr=" << static_cast<unsigned long>(result.error)
        << " deleted=" << result.deleted.size() << " cancelled=" << result.cancelled
        << " accounted=" << accounted(result) << " attributes=" << restored
        << " named=" << result.failed_path.ends_with(fs::path(rejected).filename().wstring())
        << " bytes=" << (ReadPermanentFixture(rejected) == "parallel fixture") << '\n';
    check(result.error == HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED) && !result.cancelled && accounted(result) &&
        !result.deleted.empty() && result.failed_path.ends_with(fs::path(rejected).filename().wstring()) &&
        restored != INVALID_FILE_ATTRIBUTES && (restored & FILE_ATTRIBUTE_READONLY) &&
        ReadPermanentFixture(rejected) == "parallel fixture",
        "parallel failure reports concurrent successes and preserves the failed root with read-only restored");
    SetFileAttributesW(rejected.c_str(), FILE_ATTRIBUTE_NORMAL);

    const auto tree = root / L"nested-parallel" / L"inner";
    fs::create_directories(tree);
    for (size_t i = 0; i < 600; ++i) std::ofstream(tree / (std::to_wstring(i) + L".txt")) << "nested";
    FileIdentity tree_identity;
    const auto selected_tree = tree.parent_path();
    check(ReadPathFileIdentity(selected_tree.wstring(), tree_identity), "capture single nested tree for parallel leaves");
    probe::Reset();
    result = shell::DeletePermanentAuthorized({selected_tree.wstring()}, {tree_identity}, never_cancel, {}, options);
    check(SUCCEEDED(result.error) && result.deleted == std::vector<std::wstring>{selected_tree.wstring()} &&
        !fs::exists(selected_tree) && probe::peak.load() >= 2 && probe::peak.load() <= 8,
        "one selected nested directory drains bounded batches and removes folders after their contents");

    const auto outside = root / L"parallel-outside", linked_tree = root / L"parallel-links";
    fs::create_directories(outside); fs::create_directories(linked_tree);
    std::ofstream(outside / L"keep.txt") << "outside safe";
    for (size_t i = 0; i < 40; ++i) std::ofstream(linked_tree / (std::to_wstring(i) + L".txt")) << "leaf";
    const auto link = linked_tree / L"link";
    if (probe::CreateDirectoryLink(link, outside)) {
        check(ReadPathFileIdentity(linked_tree.wstring(), tree_identity), "capture parallel tree containing a reparse leaf");
        probe::Reset();
        result = shell::DeletePermanentAuthorized({linked_tree.wstring()}, {tree_identity}, never_cancel, {}, options);
        check(SUCCEEDED(result.error) && !fs::exists(linked_tree) && ReadPermanentFixture(outside / L"keep.txt") == "outside safe",
            "parallel directory enumeration removes a reparse leaf without entering its target");
    } else std::cout << "[SKIP] parallel directory symlink fixture unavailable, Win32=" << GetLastError() << '\n';
    probe::Reset();
}

} // namespace pulse::bench
