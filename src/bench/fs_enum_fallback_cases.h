// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once
#include "../fs/fs_enum.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

inline int RunEnumerationFallbackCases() {
    using namespace pulse::fs;
    int failures = 0;
    auto check = [&](bool ok, const char* label) {
        std::cout << (ok ? "[PASS] " : "[FAIL] ") << label << '\n';
        failures += !ok;
    };
    struct ResetInjection { ~ResetInjection() { EnumerationTestProbe::Reset(); } } reset;
    const auto root = std::filesystem::current_path() / L"bench_data" /
        (L"fs-fallback-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    std::filesystem::create_directories(root);
    { std::ofstream file(root / L"payload.bin", std::ios::binary); file << "data"; }
    std::vector<DirEntry> snapshot;
    struct FailureCase { ULONG status; DWORD code; const char* label; };
    const FailureCase rejected[] = {
        {0xC0000022UL, ERROR_ACCESS_DENIED, "access denied does not retry"},
        {0xC0000034UL, ERROR_FILE_NOT_FOUND, "missing object does not retry"},
        {0xC000003AUL, ERROR_PATH_NOT_FOUND, "missing path does not retry"},
        {0xC000000DUL, ERROR_INVALID_PARAMETER, "invalid parameter does not retry"},
        {0xC0000003UL, ERROR_SUCCESS, "invalid information class does not imply unsupported capability"},
        {0xC0000033UL, ERROR_INVALID_NAME, "invalid name does not retry"},
        {0x80000005UL, ERROR_MORE_DATA, "truncated query does not retry"},
        {0xC0000023UL, ERROR_INSUFFICIENT_BUFFER, "small query buffer does not retry"},
        {0xC0000010UL, ERROR_INVALID_FUNCTION, "invalid device request is not inferred as compatible"},
        {0xC0009999UL, ERROR_MR_MID_NOT_FOUND, "unknown native status does not retry"},
    };
    for (const auto& test : rejected) {
        EnumerationTestProbe::FailNative(static_cast<LONG>(test.status));
        snapshot.assign(1, {});
        bool rejected_error = false;
        try { EnumerateDirectory(root.wstring(), snapshot); }
        catch (const EnumerationError& error) {
            const std::string message = error.what();
            rejected_error = (test.code == ERROR_SUCCESS ? error.Code() != ERROR_SUCCESS : error.Code() == test.code) &&
                message.find("NtQueryDirectoryFile") != std::string::npos &&
                message.find("status=0x") != std::string::npos && message.find("fs-fallback-") != std::string::npos &&
                message.find("Win32 fallback:") == std::string::npos;
        }
        check(rejected_error && snapshot.empty() && !EnumerationTestProbe::FallbackAttempts(), test.label);
    }
    const ULONG compatible[] = {0xC0000002UL, 0xC00000BBUL};
    for (const ULONG status : compatible) {
        EnumerationTestProbe::FailNative(static_cast<LONG>(status), false);
        bool open_error = false;
        try { EnumerateDirectory(root.wstring(), snapshot); }
        catch (const EnumerationError& error) {
            open_error = std::string(error.what()).find("NtCreateFile") != std::string::npos;
        }
        check(open_error && snapshot.empty() && !EnumerationTestProbe::FallbackAttempts(),
              "unsupported open has no directory-query capability evidence and does not retry");
        EnumerationTestProbe::FailNative(static_cast<LONG>(status));
        check(EnumerateDirectory(root.wstring(), snapshot) && snapshot.size() == 1 &&
              snapshot.front().name == L"payload.bin" && snapshot.front().size == 4 &&
              EnumerationTestProbe::FallbackAttempts() == 1,
              "explicitly unsupported query retries once and returns complete Win32 snapshot");
    }
    EnumerationTestProbe::FailApi();
    check(EnumerateDirectory(root.wstring(), snapshot) && snapshot.size() == 1 &&
          EnumerationTestProbe::FallbackAttempts() == 1,
          "unavailable native API retains compatibility enumeration");
    EnumerationTestProbe::FailNative(static_cast<LONG>(0xC0000022UL), true, true);
    bool partial_error = false;
    try { EnumerateDirectory(root.wstring(), snapshot); }
    catch (const EnumerationError& error) { partial_error = error.Code() == ERROR_ACCESS_DENIED; }
    check(partial_error && snapshot.empty() && !EnumerationTestProbe::FallbackAttempts(),
          "failure after a successful native batch clears partial entries without retry");
    EnumerationTestProbe::FailNative(static_cast<LONG>(0xC00000BBUL), true, true);
    check(EnumerateDirectory(root.wstring(), snapshot) && snapshot.size() == 1 &&
          EnumerationTestProbe::FallbackAttempts() == 1,
          "capability retry after a native batch discards partial entries before restarting");
    EnumerationTestProbe::FailApi();
    bool combined_error = false;
    try { EnumerateDirectory((root / L"missing").wstring(), snapshot); }
    catch (const EnumerationError& error) {
        const std::string message = error.what();
        combined_error = error.Code() == ERROR_PATH_NOT_FOUND && message.find("NT primary:") != std::string::npos &&
            message.find("Win32 fallback:") != std::string::npos;
        std::cout << "[INFO] " << message << '\n';
    }
    check(combined_error && snapshot.empty() && EnumerationTestProbe::FallbackAttempts() == 1,
          "failed compatibility retry retains both backend diagnostics and clears output");
    EnumerationTestProbe::Reset();
    bool missing_error = false;
    try { EnumerateDirectory((root / L"missing" / L"child").wstring(), snapshot); }
    catch (const EnumerationError& error) {
        const std::string message = error.what();
        missing_error = error.Code() == ERROR_PATH_NOT_FOUND && message.find("NtCreateFile") != std::string::npos &&
            message.find("Win32 fallback:") == std::string::npos;
        std::cout << "[INFO] local provider: " << message << '\n';
    }
    check(missing_error && snapshot.empty() && !EnumerationTestProbe::FallbackAttempts(),
          "real local missing ancestor preserves native path failure without a second enumeration");
    bool invalid_error = false;
    try { EnumerateDirectory((root / L"payload.bin").wstring(), snapshot); }
    catch (const EnumerationError& error) {
        const std::string message = error.what();
        invalid_error = error.Code() == ERROR_DIRECTORY && message.find("NtCreateFile") != std::string::npos;
        std::cout << "[INFO] local provider: " << message << '\n';
    }
    check(invalid_error && snapshot.empty() && !EnumerationTestProbe::FallbackAttempts(),
          "real local file-as-directory failure does not retry");
    std::error_code cleanup;
    std::filesystem::remove_all(root, cleanup);
    check(!cleanup, "isolated fallback fixture cleaned");
    std::cout << "[SKIP] no live SMB/RaiDrive provider fixture; injected capability statuses establish policy only\n";
    return failures ? 1 : 0;
}
