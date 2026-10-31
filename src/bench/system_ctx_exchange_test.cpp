// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "../app/system_ctx_exchange.h"

#include <cstdio>

namespace {
using namespace pulse::app::sysctx::exchange;
int failures = 0;
void Check(bool ok, const char* name) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++failures;
}

std::wstring Token(unsigned offset) {
    wchar_t token[33]{};
    swprintf_s(token, L"%016llx%016llx", static_cast<unsigned long long>(GetCurrentProcessId()),
               static_cast<unsigned long long>(GetTickCount64() + offset));
    return token;
}

// Protocol-only fixtures inherit this isolated test directory's access rules.
// The protected production ACL is exercised separately by the default mode.
struct FixtureChannel : Channel {
    HANDLE fixture_directory = INVALID_HANDLE_VALUE;
    bool Create(const std::wstring& parent, std::wstring_view id, const std::string& batch) {
        token = id;
        directory = parent + L"\\pulse_sysctx_" + token;
        if (!CreateDirectoryW(directory.c_str(), nullptr)) return false;
        fixture_directory = CreateFileW(directory.c_str(), DELETE | FILE_LIST_DIRECTORY,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (fixture_directory == INVALID_HANDLE_VALUE) return false;
        batch_path = directory + L"\\batch.txt";
        result_path = directory + L"\\result.txt";
        batch_file = CreateFileW(batch_path.c_str(), GENERIC_READ | GENERIC_WRITE | DELETE, FILE_SHARE_READ,
            nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (batch_file == INVALID_HANDLE_VALUE) return false;
        DWORD written = 0;
        if (!WriteFile(batch_file, batch.data(), static_cast<DWORD>(batch.size()), &written, nullptr) ||
            written != batch.size() || !FlushFileBuffers(batch_file)) return false;
        result_file = CreateFileW(result_path.c_str(), GENERIC_READ | GENERIC_WRITE | DELETE,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        return result_file != INVALID_HANDLE_VALUE && Write(result_file, token, Phase::Ready);
    }
    ~FixtureChannel() {
        for (HANDLE* file : {&batch_file, &result_file}) {
            if (*file == INVALID_HANDLE_VALUE) continue;
            DeleteOwned(*file);
            CloseHandle(*file);
            *file = INVALID_HANDLE_VALUE;
        }
        if (fixture_directory != INVALID_HANDLE_VALUE) {
            DeleteOwned(fixture_directory);
            CloseHandle(fixture_directory);
        }
    }
};
}

int wmain(int argc, wchar_t** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    wchar_t current[32768]{};
    const DWORD length = GetCurrentDirectoryW(ARRAYSIZE(current), current);
    if (!length || length >= ARRAYSIZE(current)) return 2;
    const std::wstring parent = std::wstring(current) + L"\\sysctx_exchange_" + Token(0);
    if (!CreateDirectoryW(parent.c_str(), nullptr)) {
        std::printf("[FAIL] fixture directory error=%lu\n", GetLastError());
        return 2;
    }
    const std::wstring partial = L"PULSE-SYSCTX-RESULT 1\nok\ndenied\n";
    const std::wstring ok = L"PULSE-SYSCTX-RESULT 1\nok\n";
    const std::string batch = "PULSE-SYSCTX 2\n";
    if (argc == 2 && std::wstring_view(argv[1]) == L"--protocol-only") {
        std::printf("[SKIP] protected production ACL creation (protocol-only mode)\n");
    } else {
        Channel secure;
        const bool created = secure.Create(parent, Token(100), batch);
        Check(created, "create the production protected private directory and channel");
        if (!created) std::printf("[DIAG] protected channel creation error=%lu\n", GetLastError());
    }
    {
        FixtureChannel channel;
        const bool created = channel.Create(parent, Token(1), batch);
        Check(created, "create an exclusive private batch and ready result before launch");
        if (!created) {
            std::printf("[DIAG] channel creation error=%lu\n", GetLastError());
            return 2;
        }
        Channel collision;
        Check(!collision.Create(parent, channel.token, batch), "preexisting batch directory is never reused");
        HANDLE overwrite = CreateFileW(channel.batch_path.c_str(), GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
        Check(overwrite == INVALID_HANDLE_VALUE, "held batch denies a second writer throughout execution");
        if (overwrite != INVALID_HANDLE_VALUE) CloseHandle(overwrite);
        HANDLE batch_read = CreateFileW(channel.batch_path.c_str(), GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
        Check(batch_read != INVALID_HANDLE_VALUE, "helper can read the held immutable batch");
        if (batch_read != INVALID_HANDLE_VALUE) CloseHandle(batch_read);
        Check(!DeleteFileW(channel.result_path.c_str()) &&
              !MoveFileW(channel.directory.c_str(), (channel.directory + L"_moved").c_str()),
              "held result and directory cannot be removed or replaced by path");
        Check(OpenForHelper(channel.result_path, Token(900)) == INVALID_HANDLE_VALUE,
              "a different batch token is rejected before mutations");
        HANDLE helper = OpenForHelper(channel.result_path, channel.token);
        Check(helper != INVALID_HANDLE_VALUE, "helper opens the precreated owned ready channel");
        if (helper == INVALID_HANDLE_VALUE) return 2;
        int invoked = 0;
        const int exit_code = Run(helper, channel.token, [&] {
            ++invoked;
            return Report{partial, false};
        }, Write);
        CloseHandle(helper);
        std::wstring body;
        Check(exit_code == 1 && invoked == 1 &&
              Confirm(channel.result_file, channel.token, exit_code, body) == Confirmation::Complete && body == partial,
              "partial success survives the owned helper channel in batch order");
        Check(MatchesExit(1, false) && MatchesExit(0, true) && !MatchesExit(0, false) &&
              !MatchesExit(1, true) && !MatchesExit(2, true),
              "success and partial-failure exit codes must agree with result contents");
        Check(Confirm(channel.result_file, channel.token, 2, body) == Confirmation::Unknown &&
              Confirm(channel.result_file, Token(901), 1, body) == Confirmation::Unknown,
              "valid results with wrong exit or token are not accepted");
        Check(OpenForHelper(channel.result_path, channel.token) == INVALID_HANDLE_VALUE,
              "an already completed result cannot authorize another apply");
    }
    {
        FixtureChannel channel;
        if (!channel.Create(parent, Token(2), batch)) return 2;
        HANDLE read_only = CreateFileW(channel.result_path.c_str(), GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (read_only == INVALID_HANDLE_VALUE) return 2;
        int invoked = 0;
        const int exit_code = Run(read_only, channel.token, [&] { ++invoked; return Report{ok, true}; }, Write);
        CloseHandle(read_only);
        std::wstring body;
        Check(exit_code == 2 && invoked == 0 &&
              Confirm(channel.result_file, channel.token, 2, body) == Confirmation::Rejected,
              "unwritable result channel aborts before the first mutation");
        HANDLE helper = OpenForHelper(channel.result_path, channel.token);
        if (helper == INVALID_HANDLE_VALUE) return 2;
        const int lost = Run(helper, channel.token, [&] { ++invoked; return Report{ok, true}; },
            [](HANDLE file, std::wstring_view token, Phase phase, std::wstring_view body) {
                if (phase == Phase::Complete) { SetLastError(ERROR_DISK_FULL); return false; }
                return Write(file, token, phase, body);
            });
        CloseHandle(helper);
        Check(lost == kUnknownExit && invoked == 1 &&
              Confirm(channel.result_file, channel.token, lost, body) == Confirmation::Unknown,
              "final write failure after mutation yields an unknown outcome, never false success");
        Check(Confirm(channel.result_file, channel.token, 2, body) == Confirmation::Unknown,
              "a running journal cannot be classified as an untouched rejected batch");
    }
    const std::wstring outsider = parent + L"\\outsider.txt";
    HANDLE old = CreateFileW(outsider.c_str(), GENERIC_READ | GENERIC_WRITE | DELETE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (old == INVALID_HANDLE_VALUE) return 2;
    const char stale[] = "PULSE-SYSCTX-RESULT 1\nok\n";
    DWORD written = 0;
    const bool seeded = WriteFile(old, stale, sizeof(stale) - 1, &written, nullptr) && written == sizeof(stale) - 1;
    Check(seeded && OpenForHelper(outsider, Token(3)) == INVALID_HANDLE_VALUE,
          "preexisting plausible legacy success results are rejected without deletion");
    LARGE_INTEGER size{};
    LARGE_INTEGER zero{};
    char retained[sizeof(stale)]{};
    DWORD read = 0;
    Check(GetFileSizeEx(old, &size) && size.QuadPart == sizeof(stale) - 1 &&
          SetFilePointerEx(old, zero, nullptr, FILE_BEGIN) &&
          ReadFile(old, retained, sizeof(stale) - 1, &read, nullptr) && read == sizeof(stale) - 1 &&
          std::string_view(retained, read) == std::string_view(stale, sizeof(stale) - 1),
          "rejecting an outsider result preserves its original bytes and length");
    DeleteOwned(old);
    CloseHandle(old);
    Check(RemoveDirectoryW(parent.c_str()) != FALSE, "all owned fixture files and directories are removed");
    std::printf("%s (%d failures)\n", failures ? "FAILED" : "ALL PASSED", failures);
    return failures ? 1 : 0;
}
