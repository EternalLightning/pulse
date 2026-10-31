// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "../index/document_file_version.h"
#include <cstring>
#include <cstdlib>
#include <string>

namespace {
bool Read(HANDLE file, void* data, DWORD size) {
    DWORD count = 0;
    return ReadFile(file, data, size, &count, nullptr) && count == size;
}
bool Write(HANDLE file, HANDLE ready, const void* data, DWORD size) {
    DWORD count = 0;
    if (!WriteFile(file, data, size, &count, nullptr) || count != size) return false;
    SetEvent(ready); return true;
}
void ChangeSameSizeAndTime(const std::wstring& path) {
    const auto file = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    FILETIME modified{};
    if (GetFileTime(file, nullptr, nullptr, &modified)) {
        char value = 0; DWORD bytes = 0;
        ReadFile(file, &value, 1, &bytes, nullptr); value ^= 1;
        SetFilePointer(file, 0, nullptr, FILE_BEGIN); WriteFile(file, &value, 1, &bytes, nullptr);
        SetFileTime(file, nullptr, nullptr, &modified);
    }
    CloseHandle(file);
}
}
// Disposable protocol peer for fault injection; never used by the product.
int wmain(int argc, wchar_t** argv) {
    using namespace pulse::index::document;
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    if (argc != 5) return 2;
    const auto input = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(_wcstoui64(argv[1], nullptr, 10)));
    const auto output = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(_wcstoui64(argv[2], nullptr, 10)));
    const auto request_ready = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(_wcstoui64(argv[3], nullptr, 10)));
    const auto ready = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(_wcstoui64(argv[4], nullptr, 10)));
    wchar_t mode[32]{};
    GetEnvironmentVariableW(L"PULSE_DOCUMENT_FAILURE_FIXTURE", mode, ARRAYSIZE(mode));
    auto* memory = VirtualAlloc(nullptr, 32u * 1024 * 1024, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (memory) std::memset(memory, 1, 32u * 1024 * 1024);
    for (;;) {
        if (WaitForSingleObject(request_ready, kIdleMs) != WAIT_OBJECT_0) return 0;
        Request request;
        if (!Read(input, &request, sizeof(request)) || request.magic != kMagic || request.path_chars > 32767 || request.stop_needle_chars > 4096) return 3;
        std::wstring path(request.path_chars, L'\0'), needle(request.stop_needle_chars, L'\0');
        if (!Read(input, path.data(), request.path_chars * sizeof(wchar_t)) ||
            (request.stop_needle_chars && !Read(input, needle.data(), request.stop_needle_chars * sizeof(wchar_t)))) return 3;
        Begin begin;
        begin.config_mode = PdfEngineMode();
        begin.engine = request.override_engine == 2 || begin.config_mode == 2 ? 2u : 1u;
        const auto file = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            BY_HANDLE_FILE_INFORMATION info{};
            if (GetFileInformationByHandle(file, &info) && QueryFileVersion(file, info, begin.version)) begin.flags = 1;
            CloseHandle(file);
        }
        if (!FallbackSourceMatches(request, begin)) begin.error = ERROR_FILE_INVALID;
        if (!Write(output, ready, &begin, sizeof(begin))) return 0;
        const bool fresh_case = wcsncmp(mode, L"fresh_filter", 12) == 0;
        if (fresh_case && begin.engine == 1 && !_wcsicmp(mode, L"fresh_filter_exit")) ExitProcess(kPdfiumOomExitCode);
        if (fresh_case && begin.engine == 1 && !_wcsicmp(mode, L"fresh_filter_changed")) ChangeSameSizeAndTime(path);
        if (fresh_case && begin.engine == 2 && !_wcsicmp(mode, L"fresh_filter_hang")) Sleep(60000);
        if (!_wcsicmp(mode, L"hang")) Sleep(60000);
        if (!_wcsicmp(mode, L"exit")) { Sleep(30); ExitProcess(0xe1234567); }
        {
            Response response;
            response.error = begin.error;
            if (!response.error && fresh_case && (begin.engine == 1 || !_wcsicmp(mode, L"fresh_filter_always")))
                response.error = ERROR_NOT_ENOUGH_MEMORY;
            const std::wstring body = response.error ? L"" : L"completed";
            response.text_chars = static_cast<uint32_t>(body.size());
            if (!_wcsicmp(mode, L"body") || !_wcsicmp(mode, L"header")) {
                response.text_chars = 32;
                if (!_wcsicmp(mode, L"header")) response.magic = 0;
                Write(output, ready, &response, sizeof(response)); Sleep(50); ExitProcess(0xe1234567);
            }
            if (!Write(output, ready, &response, sizeof(response)) ||
                (!body.empty() && !Write(output, ready, body.data(), static_cast<DWORD>(body.size() * sizeof(wchar_t))))) return 0;
        }
        Completion completion;
        completion.flags = 1; completion.private_bytes = 32u * 1024 * 1024;
        if (!_wcsicmp(mode, L"tail_delay")) { Sleep(250); completion.flags = 3; completion.private_bytes = kRetirePrivateBytes; }
        if (!_wcsicmp(mode, L"tail_below")) completion.private_bytes = kRetirePrivateBytes - 1;
        if (!_wcsicmp(mode, L"tail_flags")) completion.flags = 4;
        if (!_wcsicmp(mode, L"tail_size")) completion.size = 1;
        if (!_wcsicmp(mode, L"tail_magic")) completion.magic = 0;
        if (!_wcsicmp(mode, L"tail_range")) completion.private_bytes = kMemoryBytes + 1;
        if (!Write(output, ready, &completion, sizeof(completion))) return 0;
    }
}
