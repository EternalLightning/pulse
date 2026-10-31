// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "../ui/edit_ime_transaction.h"
#include "../ui/thumbnail_disk_cache.cpp"
#include <commctrl.h>
#include <cstdio>
#include <future>

namespace {
int failures = 0;
void Check(bool ok, const char* label) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
    if (!ok) ++failures;
}
LRESULT CALLBACK EditProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam, UINT_PTR, DWORD_PTR) {
    pulse::ui::edit_ime::BeforeDefault(hwnd, message, lparam);
    const auto result = DefSubclassProc(hwnd, message, wparam, lparam);
    pulse::ui::edit_ime::AfterDefault(hwnd, message);
    return result;
}
void Selection(HWND edit, DWORD& from, DWORD& to) {
    SendMessageW(edit, EM_GETSEL, reinterpret_cast<WPARAM>(&from), reinterpret_cast<LPARAM>(&to));
}
}

int main() {
    using namespace pulse::ui;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    InitCommonControls();
    HWND host = CreateWindowExW(0, L"STATIC", L"IME transaction fixture", WS_OVERLAPPED,
        0, 0, 400, 200, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    HWND edit = CreateWindowExW(0, L"EDIT", L"stem.txt", WS_CHILD | ES_AUTOHSCROLL,
        0, 0, 300, 24, host, nullptr, GetModuleHandleW(nullptr), nullptr);
    SetWindowSubclass(edit, EditProc, 1, 0);
    SendMessageW(edit, EM_SETSEL, 0, 4);
    SendMessageW(edit, WM_IME_STARTCOMPOSITION, 0, 0);
    Check(edit_ime::Text(edit) == L".txt", "IME start keeps the existing caret workaround");
    SendMessageW(edit, WM_IME_ENDCOMPOSITION, 0, 0);
    DWORD from = 0, to = 0; Selection(edit, from, to);
    Check(edit_ime::Text(edit) == L"stem.txt" && from == 0 && to == 4,
        "synthetic Esc-style cancellation restores original text and selection");
    SendMessageW(edit, WM_IME_STARTCOMPOSITION, 0, 0);
    SendMessageW(edit, WM_IME_COMPOSITION, 0, GCS_RESULTSTR);
    SendMessageW(edit, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(L"中文"));
    SendMessageW(edit, WM_IME_ENDCOMPOSITION, 0, 0);
    Check(edit_ime::Text(edit) == L"中文.txt", "committed result replaces only the selected stem");
    SendMessageW(edit, WM_IME_STARTCOMPOSITION, 0, 0);
    SendMessageW(edit, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(L"词"));
    SendMessageW(edit, WM_IME_COMPOSITION, 0, GCS_RESULTSTR);
    SendMessageW(edit, WM_IME_ENDCOMPOSITION, 0, 0);
    Selection(edit, from, to);
    Check(edit_ime::Text(edit) == L"中文词.txt" && from == 3 && to == 3,
        "continuous committed words stay after the preceding word");
    Check(SendMessageW(edit, EM_CANUNDO, 0, 0) != 0 && SendMessageW(edit, EM_UNDO, 0, 0) != 0,
        "committed input retains native undo");
    SetWindowTextW(edit, L"stem.txt");
    SendMessageW(edit, EM_SETSEL, 0, 4);
    SendMessageW(edit, WM_IME_STARTCOMPOSITION, 0, 0);
    SendMessageW(edit, WM_KILLFOCUS, reinterpret_cast<WPARAM>(host), 0);
    Check(edit_ime::Text(edit) == L"stem.txt", "focus-loss cancellation restores preparatory deletion");
    SendMessageW(edit, EM_SETSEL, 0, 4);
    SendMessageW(edit, WM_IME_STARTCOMPOSITION, 0, 0);
    SetWindowTextW(edit, L"replacement");
    SendMessageW(edit, WM_IME_ENDCOMPOSITION, 0, 0);
    Check(edit_ime::Text(edit) == L"replacement", "external text replacement supersedes the IME transaction");
    DestroyWindow(host);

    wchar_t temp_directory[MAX_PATH]{}, temp_file[MAX_PATH]{};
    const bool created = GetTempPathW(ARRAYSIZE(temp_directory), temp_directory) &&
        GetTempFileNameW(temp_directory, L"pui", 0, temp_file);
    Check(created, "real identity fixture allocates a unique temporary file");
    if (created) {
        HANDLE file = CreateFileW(temp_file, GENERIC_WRITE | FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, TRUNCATE_EXISTING,
            FILE_ATTRIBUTE_NORMAL, nullptr);
        DWORD written = 0;
        const char bytes[] = "identity";
        const bool wrote = file != INVALID_HANDLE_VALUE &&
            WriteFile(file, bytes, sizeof(bytes) - 1, &written, nullptr) && written == sizeof(bytes) - 1;
        BY_HANDLE_FILE_INFORMATION info{};
        if (file != INVALID_HANDLE_VALUE) {
            FlushFileBuffers(file);
            GetFileInformationByHandle(file, &info);
            CloseHandle(file);
        }
        const std::wstring path = temp_file;
        ThumbnailStoreKey key{path, info.dwFileAttributes, 96,
            (static_cast<uint64_t>(info.ftLastWriteTime.dwHighDateTime) << 32) | info.ftLastWriteTime.dwLowDateTime,
            (static_cast<uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow};
        ThumbnailFileIdentity identity;
        Check(wrote && ReadThumbnailFileIdentity(key, identity) && identity.volume == info.dwVolumeSerialNumber &&
            identity.index == ((static_cast<uint64_t>(info.nFileIndexHigh) << 32) | info.nFileIndexLow),
            "bounded production reader returns the real local file identity");
        ++key.size;
        Check(!ReadThumbnailFileIdentity(key, identity), "changed file size rejects a stale thumbnail key");
        Check(DeleteFileW(temp_file) && GetFileAttributesW(temp_file) == INVALID_FILE_ATTRIBUTES,
            "real identity fixture removes and enumerates its temporary file");
    }

    HANDLE release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE entered = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE cancel = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::atomic<unsigned> completed{0};
    auto blocked = std::async(std::launch::async, [&] {
        return BoundedIdentityRead([=, &completed] {
            SetEvent(entered);
            WaitForSingleObject(release, INFINITE);
            ++completed;
            return IdentityReadResult{true, {1, 2, 3, 4}};
        }, cancel);
    });
    Check(WaitForSingleObject(entered, 2000) == WAIT_OBJECT_0, "controlled provider probe entered blocking I/O");
    const auto start = GetTickCount64();
    SetEvent(cancel);
    const bool cancelled = !blocked.get().valid;
    Check(cancelled && GetTickCount64() - start < 250, "stop event bounds the owning worker wait while provider remains blocked");
    std::vector<std::future<IdentityReadResult>> probes;
    for (int i = 0; i < 3; ++i) probes.push_back(std::async(std::launch::async, [=, &completed] {
        return BoundedIdentityRead([=, &completed] {
            WaitForSingleObject(release, INFINITE);
            ++completed;
            return IdentityReadResult{true, {1, 2, 3, 4}};
        }, nullptr);
    }));
    for (auto& probe : probes) Check(!probe.get().valid, "unresponsive provider is a bounded cache miss");
    const auto full_start = GetTickCount64();
    Check(!BoundedIdentityRead([] { return IdentityReadResult{true, {}}; }, nullptr).valid &&
        GetTickCount64() - full_start < 100, "outstanding metadata probes are capped at four");
    SetEvent(release);
    const auto released_at = GetTickCount64();
    while (completed != 4 && GetTickCount64() - released_at < 2000) Sleep(1);
    Check(completed == 4, "all controlled providers finish before fixture handles are closed");
    Check(BoundedIdentityRead([] { return IdentityReadResult{true, {1, 2, 3, 4}}; }, nullptr).valid,
        "late completion frees slots without publishing a stale result");
    CloseHandle(release); CloseHandle(entered); CloseHandle(cancel);
    CoUninitialize();
    return failures ? 1 : 0;
}
