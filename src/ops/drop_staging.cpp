// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "drop_staging.h"
#include "destination_guard.h"

#include <windows.h>
#include <atomic>
#include <cwctype>
#include <thread>

namespace pulse::ops {
namespace {

// Plain Win32 form without the \\?\ prefix or trailing separators.
std::wstring Plain(std::wstring path) {
    for (auto& c : path) if (c == L'/') c = L'\\';
    if (path.starts_with(L"\\\\?\\UNC\\")) path = L"\\\\" + path.substr(8);
    else if (path.starts_with(L"\\\\?\\")) path = path.substr(4);
    while (path.size() > 3 && path.back() == L'\\') path.pop_back();
    return path;
}

std::wstring Long(const std::wstring& path) {
    const std::wstring plain = Plain(path);
    if (plain.starts_with(L"\\\\")) return L"\\\\?\\UNC\\" + plain.substr(2);
    return L"\\\\?\\" + plain;
}

// Expands 8.3 components (C:\Users\ADMINI~1) so prefixes compare reliably.
std::wstring Expanded(const std::wstring& path) {
    std::wstring plain = Plain(path);
    if (plain.find(L'~') == std::wstring::npos) return plain;
    wchar_t buffer[32768];
    const DWORD n = GetLongPathNameW(Long(plain).c_str(), buffer, ARRAYSIZE(buffer));
    return n > 0 && n < ARRAYSIZE(buffer) ? Plain(buffer) : plain;
}

bool Within(const std::wstring& path, const std::wstring& dir) {
    const std::wstring a = Expanded(path);
    const std::wstring b = Expanded(dir);
    if (b.empty() || a.size() <= b.size()) return false;
    if (CompareStringOrdinal(a.c_str(), static_cast<int>(b.size()), b.c_str(),
                             static_cast<int>(b.size()), TRUE) != CSTR_EQUAL)
        return false;
    return b.back() == L'\\' || a[b.size()] == L'\\';
}

std::wstring Leaf(const std::wstring& path) {
    const std::wstring plain = Plain(path);
    const size_t slash = plain.find_last_of(L'\\');
    return slash == std::wstring::npos ? plain : plain.substr(slash + 1);
}

std::wstring Parent(const std::wstring& path) {
    const std::wstring plain = Plain(path);
    const size_t slash = plain.find_last_of(L'\\');
    return slash == std::wstring::npos ? std::wstring() : plain.substr(0, slash);
}

template <typename Fn>
DWORD ForEachChild(const std::wstring& dir, Fn&& fn, const DropEnumerationApi& api = {}) {
    WIN32_FIND_DATAW data{};
    HANDLE find = api.first((Long(dir) + L"\\*").c_str(), FindExInfoBasic, &data,
                                   FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (find == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND) {
            const DWORD attrs = GetFileAttributesW(Long(dir).c_str());
            if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY)) return ERROR_SUCCESS;
        }
        return error;
    }
    DWORD error = ERROR_SUCCESS;
    do {
        if (wcscmp(data.cFileName, L".") == 0 || wcscmp(data.cFileName, L"..") == 0) continue;
        if (!fn(data)) { error = ERROR_CANCELLED; break; }
    } while (api.next(find, &data));
    if (!error) {
        error = GetLastError();
        if (error == ERROR_NO_MORE_FILES) error = ERROR_SUCCESS;
    }
    FindClose(find);
    return error;
}

// Hard links share the source's data, which survives the archive manager
// deleting its own name. Read-only files are copied instead, so removing a
// stage never has to change attributes that a link would share.
DWORD LinkOrCopyTree(const std::wstring& from, const std::wstring& to, std::wstring& failed_source, const DropEnumerationApi& api) {
    failed_source = from;
    const DWORD attrs = GetFileAttributesW(Long(from).c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) return GetLastError();
    if (attrs & FILE_ATTRIBUTE_REPARSE_POINT) return ERROR_NOT_SUPPORTED;
    if (attrs & FILE_ATTRIBUTE_DIRECTORY) {
        if (!CreateDirectoryW(Long(to).c_str(), nullptr)) return GetLastError();
        DWORD child_error = ERROR_SUCCESS;
        const DWORD enumeration_error = ForEachChild(from, [&](const WIN32_FIND_DATAW& child) {
            child_error = LinkOrCopyTree(Plain(from) + L"\\" + child.cFileName,
                Plain(to) + L"\\" + child.cFileName, failed_source, api);
            return child_error == ERROR_SUCCESS;
        }, api);
        if (child_error) return child_error;
        if (enumeration_error) failed_source = from;
        return enumeration_error;
    }
    if (!(attrs & FILE_ATTRIBUTE_READONLY) &&
        CreateHardLinkW(Long(to).c_str(), Long(from).c_str(), nullptr))
        return ERROR_SUCCESS;
    return CopyFileW(Long(from).c_str(), Long(to).c_str(), TRUE) ? ERROR_SUCCESS : GetLastError();
}

bool RemoveTree(const std::wstring& path, DestinationGuard& guard, const DropEnumerationApi& api = {}) {
    std::wstring error;
    if (!guard.Capture(Long(path), error) || !guard.PinDirectories(Long(Parent(path)), error)) return false;
    const std::wstring target = Long(path);
    const DWORD attrs = GetFileAttributesW(target.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) return false;
    if (attrs & FILE_ATTRIBUTE_DIRECTORY) {
        if (!(attrs & FILE_ATTRIBUTE_REPARSE_POINT)) {
            if (!guard.PinDirectories(target, error) || !guard.Check(target, error)) return false;
            bool removed = true;
            const DWORD enumerated = ForEachChild(path, [&](const WIN32_FIND_DATAW& child) {
                const bool child_removed = RemoveTree(Plain(path) + L"\\" + child.cFileName, guard, api);
                removed = child_removed && removed;
                return child_removed;
            }, api);
            guard.ReleaseDirectory(target);
            if (enumerated || !removed) return false;
        }
    }
    auto handle = guard.PinLeaf(target, error, nullptr, FILE_WRITE_ATTRIBUTES);
    if (!handle) return false;
    // Read-only entries in a stage are copies (LinkOrCopyTree), not links.
    FILE_BASIC_INFO basic{};
    if (!GetFileInformationByHandleEx(handle.get(), FileBasicInfo, &basic, sizeof(basic))) return false;
    if (basic.FileAttributes & FILE_ATTRIBUTE_READONLY) {
        basic.FileAttributes &= ~FILE_ATTRIBUTE_READONLY;
        if (!basic.FileAttributes) basic.FileAttributes = FILE_ATTRIBUTE_NORMAL;
        if (!SetFileInformationByHandle(handle.get(), FileBasicInfo, &basic, sizeof(basic))) return false;
    }
    return guard.DeleteOwnedLeaf(target, error, std::move(handle));
}

bool PinStageRoot(const std::wstring& root, DestinationGuard& guard) {
    std::wstring error;
    if (!guard.Capture(Long(root), error)) return false;
    for (const auto& impact : guard.Impacts())
        if (impact.kind != DestinationLinkKind::HardLink) return false;
    return guard.PinDirectories(Long(root), error);
}

bool ProcessAlive(DWORD pid) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return GetLastError() == ERROR_ACCESS_DENIED;  // exists, not ours to open
    DWORD code = 0;
    const bool alive = GetExitCodeProcess(process, &code) && code == STILL_ACTIVE;
    CloseHandle(process);
    return alive;
}

} // namespace

std::wstring TempDirectory() {
    wchar_t buffer[MAX_PATH + 1]{};
    const DWORD n = GetTempPathW(ARRAYSIZE(buffer), buffer);
    if (n == 0 || n >= ARRAYSIZE(buffer)) return {};
    return Expanded(buffer);
}

std::wstring DropStageRoot() {
    const std::wstring temp = TempDirectory();
    return temp.empty() ? std::wstring() : temp + L"\\PulseDrop";
}

bool IsTemporaryDropSource(const std::wstring& path, const std::wstring& temp_dir,
                           const std::wstring& stage_root) {
    if (path.empty() || temp_dir.empty() || !Within(path, temp_dir)) return false;
    if (stage_root.empty()) return true;
    return !Within(path, stage_root) &&
           CompareStringOrdinal(Expanded(path).c_str(), -1, Expanded(stage_root).c_str(), -1, TRUE) != CSTR_EQUAL;
}

static DropStageResult StageDropSourcesImpl(const std::vector<std::wstring>& sources, const std::wstring& temp_dir,
                      const std::wstring& stage_root, std::vector<std::wstring>& staged, const DropEnumerationApi& api) {
    static std::atomic<unsigned> counter{0};
    staged = sources;
    auto candidate = sources;
    std::wstring stage;
    DestinationGuard stage_guard;
    DestinationGuard::Handle stage_handle;
    bool stage_owned = false;
    bool any = false;
    for (size_t i = 0; i < sources.size(); ++i) {
        const std::wstring& source = sources[i];
        if (!IsTemporaryDropSource(source, temp_dir, stage_root)) continue;
        auto fail = [&](DWORD error, const std::wstring& failed_source) {
            stage_handle.reset();
            const bool cleaned = stage.empty() || (stage_owned && RemoveTree(stage, stage_guard));
            return DropStageResult{false, error, failed_source, static_cast<DWORD>(cleaned ? ERROR_SUCCESS : ERROR_FILE_INVALID)};
        };
        if (stage_root.empty()) return fail(ERROR_PATH_NOT_FOUND, source);
        if (stage.empty()) {
            if (!CreateDirectoryW(Long(stage_root).c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
                return fail(GetLastError(), source);
            const DWORD root_attrs = GetFileAttributesW(Long(stage_root).c_str());
            if (root_attrs == INVALID_FILE_ATTRIBUTES) return fail(GetLastError(), source);
            if (!(root_attrs & FILE_ATTRIBUTE_DIRECTORY) || (root_attrs & FILE_ATTRIBUTE_REPARSE_POINT))
                return fail(ERROR_NOT_SUPPORTED, source);
            if (!PinStageRoot(stage_root, stage_guard)) return fail(ERROR_FILE_INVALID, source);
            stage = Plain(stage_root) + L"\\" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                    std::to_wstring(++counter) + L"-" + std::to_wstring(GetTickCount64());
            std::wstring guard_error;
            stage_handle = DestinationGuard::CreateDirectoryLeaf(Long(stage), false, guard_error);
            if (!stage_handle) {
                const DWORD error = GetLastError();
                stage.clear(); // Never clean a pre-existing directory we did not create.
                return fail(error, source);
            }
            if (!stage_guard.AdoptCreatedLeaf(Long(stage), stage_handle.get(), guard_error))
                return fail(ERROR_FILE_INVALID, source);
            stage_owned = true;
        }
        // Named like the original parent (7zE44D7D628), which dialogs show.
        std::wstring parent = Leaf(Parent(source));
        if (parent.empty()) parent = L"Temp";
        const std::wstring folder = stage + L"\\" + parent;
        if (!CreateDirectoryW(Long(folder).c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
            return fail(GetLastError(), source);
        const std::wstring target = folder + L"\\" + Leaf(source);
        if (GetFileAttributesW(Long(target).c_str()) != INVALID_FILE_ATTRIBUTES)
            return fail(ERROR_ALREADY_EXISTS, source);
        std::wstring failed_source;
        const DWORD error = LinkOrCopyTree(source, target, failed_source, api);
        if (error) return fail(error, failed_source);
        candidate[i] = target;
        any = true;
    }
    stage_handle.reset();
    if (!any && !stage.empty()) RemoveTree(stage, stage_guard);
    staged = std::move(candidate);
    return {any, ERROR_SUCCESS, {}};
}

DropStageResult StageDropSources(const std::vector<std::wstring>& sources, const std::wstring& temp_dir,
    const std::wstring& stage_root, std::vector<std::wstring>& staged) {
    return StageDropSourcesImpl(sources, temp_dir, stage_root, staged, {});
}

DropStageResult StageDropSourcesOnWorker(HWND owner, const std::vector<std::wstring>& sources,
    const std::wstring& temp_dir, const std::wstring& stage_root, std::vector<std::wstring>& staged,
    const DropEnumerationApi& api) {
    HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!done) return {false, GetLastError(), {}};
    DropStageResult result;
    std::thread worker;
    try {
        worker = std::thread([&] {
            try {
                result = StageDropSourcesImpl(sources, temp_dir, stage_root, staged, api);
            } catch (...) {
                result = {false, ERROR_NOT_ENOUGH_MEMORY, {}};
            }
            SetEvent(done);
        });
    } catch (...) {
        CloseHandle(done);
        return {false, ERROR_NOT_ENOUGH_MEMORY, {}};
    }
    const bool enable_owner = owner && IsWindowEnabled(owner);
    if (enable_owner) EnableWindow(owner, FALSE);
    for (;;) {
        const DWORD wait = MsgWaitForMultipleObjects(1, &done, FALSE, INFINITE, QS_PAINT);
        if (wait != WAIT_OBJECT_0 + 1) break;
        MSG message{};
        while (PeekMessageW(&message, nullptr, WM_PAINT, WM_PAINT, PM_REMOVE))
            DispatchMessageW(&message);
    }
    worker.join();
    CloseHandle(done);
    if (enable_owner && IsWindow(owner)) EnableWindow(owner, TRUE);
    return result;
}
bool StageDropSources(const std::vector<std::wstring>& sources, const std::wstring& temp_dir,
    const std::wstring& stage_root, std::vector<std::wstring>& staged,
    DropStageError* error, const DropEnumerationApi& api) {
    const auto result = StageDropSourcesImpl(sources, temp_dir, stage_root, staged, api);
    if (error) *error = {result.error, result.source};
    return result.any;
}

bool SweepDropStages(const std::wstring& stage_root, bool include_own, const DropEnumerationApi& api) {
    if (stage_root.empty()) return false;
    DestinationGuard guard;
    if (!PinStageRoot(stage_root, guard)) return false;
    const DWORD attrs = GetFileAttributesW(Long(stage_root).c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES || !(attrs & FILE_ATTRIBUTE_DIRECTORY) ||
        (attrs & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
    const DWORD self = GetCurrentProcessId();
    std::vector<std::wstring> doomed;
    bool captured = true;
    const DWORD enumerated = ForEachChild(stage_root, [&](const WIN32_FIND_DATAW& child) {
        if (!(child.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) return true;
        wchar_t* end = nullptr;
        const unsigned long pid = wcstoul(child.cFileName, &end, 10);
        if (end == child.cFileName || *end != L'-') return true;  // not a stage folder
        if (pid == self ? include_own : !ProcessAlive(static_cast<DWORD>(pid))) {
            const auto path = Plain(stage_root) + L"\\" + child.cFileName;
            std::wstring error;
            if (!guard.Capture(Long(path), error) ||
                (!(child.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
                 !guard.PinDirectories(Long(path), error))) captured = false;
            else doomed.push_back(path);
        }
        return true;
    }, api);
    bool removed = enumerated == ERROR_SUCCESS && captured;
    for (const auto& dir : doomed) removed = RemoveTree(dir, guard, api) && removed;
    return removed;
}

} // namespace pulse::ops
