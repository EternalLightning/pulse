// SPDX-License-Identifier: Apache-2.0
#include "../ipc/shell_query_client.h"
#include <cstdio>
#include <string_view>

namespace {
DWORD pipe_error = ERROR_ACCESS_DENIED;
unsigned pipe_opens = 0;
HANDLE WINAPI OpenTestPipe(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE) {
    ++pipe_opens;
    SetLastError(pipe_error);
    return INVALID_HANDLE_VALUE;
}
}

#define CreateFileW OpenTestPipe
#include "../ipc/shell_query_client.cpp"
#undef CreateFileW

int wmain(int argc, wchar_t** argv) {
    if (argc == 4 && std::wstring_view(argv[1]) == L"--query-worker") {
        Sleep(INFINITE);
        return 0;
    }
    using namespace pulse::ipc;
    wchar_t executable[32768]{};
    if (!GetModuleFileNameW(nullptr, executable, ARRAYSIZE(executable))) return 2;
    Pool().executable = executable;
    int failures = 0;
    const auto check = [&](bool pass, const char* name) {
        std::printf("[%s] %s\n", pass ? "PASS" : "FAIL", name);
        failures += !pass;
    };
    ShellQueryRequest request;
    request.kind = ShellQueryKind::TypeName;
    request.path = L"connection-fixture";
    for (DWORD error : {ERROR_ACCESS_DENIED, ERROR_INVALID_NAME, ERROR_FILE_NOT_FOUND, ERROR_PIPE_BUSY}) {
        pipe_error = error;
        pipe_opens = 0;
        ShellQueryResult result;
        const auto started = GetTickCount64();
        const bool ok = QueryShell(request, result, {}, 600);
        const auto elapsed = GetTickCount64() - started;
        std::printf("[CASE] error=%lu HRESULT=%08lx elapsed_ms=%llu opens=%u\n",
            error, static_cast<unsigned long>(result.error), elapsed, pipe_opens);
        const bool transient = error == ERROR_FILE_NOT_FOUND || error == ERROR_PIPE_BUSY;
        if (transient) {
            check(!ok && result.error == HRESULT_FROM_WIN32(ERROR_TIMEOUT) && pipe_opens > 2 && elapsed >= 500,
                "missing or busy startup pipe retains bounded retries and timeout");
        } else {
            check(!ok && result.error == HRESULT_FROM_WIN32(error) && pipe_opens <= 2 && elapsed < 500,
                "permanent pipe-open error is preserved without waiting out the deadline");
        }
        check(result.text.empty() && result.icons.empty(), "connection failure publishes no stale payload");
    }
    pipe_error = ERROR_FILE_NOT_FOUND;
    pipe_opens = 0;
    ShellQueryResult cancelled;
    const auto started = GetTickCount64();
    check(!QueryShell(request, cancelled, [&] { return GetTickCount64() - started >= 60; }, 600) &&
        cancelled.error == HRESULT_FROM_WIN32(ERROR_CANCELLED) && GetTickCount64() - started < 500,
        "cancellation interrupts startup retries and preserves its error");
    bool retired = true;
    for (auto& slot : Pool().slots) {
        Retire(slot);
        retired &= !slot.process && slot.pipe == INVALID_HANDLE_VALUE && !slot.leased;
    }
    check(retired, "all isolated connection-fixture helpers are reclaimed");
    return failures ? 1 : 0;
}
