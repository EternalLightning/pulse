// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "../ops/copy_source_validation.h"
#include "../ops/destination_guard.h"
#include <cstdio>
#include <filesystem>
#include <fstream>

namespace {
using namespace pulse::ops;
unsigned identity_queries = 0;
bool deny_identity = false;
int failures = 0;

BOOL WINAPI CountIdentity(HANDLE handle, FILE_INFO_BY_HANDLE_CLASS kind, LPVOID data, DWORD size) {
    if (kind == FileIdInfo) {
        ++identity_queries;
        if (deny_identity) { SetLastError(ERROR_ACCESS_DENIED); return FALSE; }
    }
    return GetFileInformationByHandleEx(handle, kind, data, size);
}

void Check(bool passed, const char* label) {
    std::printf("[%s] %s\n", passed ? "PASS" : "FAIL", label);
    failures += !passed;
}
}

int main() {
    namespace fs = std::filesystem;
    const auto root = fs::absolute(L"bench_data") / (L"transfer-validation-" +
        std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    std::error_code error;
    fs::create_directories(root.parent_path(), error);
    if (error || !fs::create_directory(root, error) || !fs::create_directory(root / L"held", error)) {
        std::printf("[FAIL] create exclusive fixture: %d\n", error.value());
        return 2;
    }
    const auto source = root / L"source.txt", other = root / L"other.txt";
    { std::ofstream out(source); out << "source"; }
    { std::ofstream out(other); out << "other"; }
    {
        auto expected = FileObjectReference::Open(source.wstring(), CountIdentity);
        auto reopened = FileObjectReference::Open(source.wstring(), CountIdentity);
        auto replacement = FileObjectReference::Open(other.wstring(), CountIdentity);
        Check(expected.Get() && reopened.Get() && replacement.Get(), "open real source identity fixtures");
        CopySourceValidation validation;
        identity_queries = 0;
        Check(validation.Observe(expected, expected.Get(), 1, true, CountIdentity) && identity_queries == 2,
            "stream start checks both retained and actual source identities");
        bool valid_chunks = true;
        for (unsigned i = 0; i < 128; ++i)
            valid_chunks &= validation.Observe(expected, expected.Get(), 1, false, CountIdentity);
        Check(valid_chunks && identity_queries == 2, "stable stream chunks avoid repeated identity queries");
        Check(validation.Observe(expected, expected.Get(), 1, true, CountIdentity) && identity_queries == 4,
            "stream end revalidates source identity");
        Check(validation.Observe(expected, reopened.Get(), 1, false, CountIdentity) && identity_queries == 6,
            "a changed source handle is checked even within the same stream");
        Check(validation.Observe(expected, reopened.Get(), 2, false, CountIdentity) && identity_queries == 8,
            "a new stream is checked even when its start callback is absent");
        validation.BeginAttempt();
        Check(!validation.Checked() && validation.Observe(expected, reopened.Get(), 2, false, CountIdentity) &&
            identity_queries == 10, "resume and fallback attempts recheck a reused numeric handle");
        Check(!validation.Observe(expected, replacement.Get(), 2, false, CountIdentity) && validation.Changed(),
            "a replacement source is refused");
        validation.BeginAttempt();
        Check(!validation.Observe(expected, expected.Get(), 1, true, CountIdentity),
            "retry cannot clear a prior identity rejection");

        CopySourceValidation denied;
        Check(denied.Observe(expected, expected.Get(), 1, true, CountIdentity), "initialize final-boundary failure case");
        deny_identity = true;
        Check(!denied.Observe(expected, expected.Get(), 1, true, CountIdentity),
            "identity query failure at stream end refuses completion");
        deny_identity = false;
    }
    {
        DestinationGuard guard(CountIdentity);
        std::wstring message;
        const auto held = (root / L"held").wstring(), renamed = (root / L"renamed").wstring();
        Check(guard.Capture(held, message), "capture directory chain");
        identity_queries = 0;
        Check(guard.Check(held, message), "validate unleased directory chain");
        const auto unleased_queries = identity_queries;
        Check(guard.PinDirectories(held, message), "pin directory chain against replacement");
        identity_queries = 0;
        Check(guard.Check(held, message) && identity_queries > 0 && identity_queries < unleased_queries,
            "leased directories retain live checks while avoiding reopen identity queries");
        Check(!MoveFileW(held.c_str(), renamed.c_str()), "held directory lease prevents rename");
        deny_identity = true;
        Check(!guard.Check(held, message), "leased directory query failure still refuses operation");
        deny_identity = false;
        guard.ReleaseDirectories();
        const bool moved = MoveFileW(held.c_str(), renamed.c_str()) != FALSE;
        const DWORD move_error = moved ? ERROR_SUCCESS : GetLastError();
        const bool replaced = moved && CreateDirectoryW(held.c_str(), nullptr);
        if (!replaced) std::printf("[INFO] replacement fixture move_error=%lu create_error=%lu\n",
            move_error, moved ? GetLastError() : ERROR_SUCCESS);
        Check(replaced && !guard.Check(held, message), "released directory is reopened and replacement refused");
    }
    const auto short_parts = DestinationGuard::ComponentsForTest(L"E:\\parent\\child\\..\\leaf");
    Check(!short_parts.empty() && short_parts.back() == L"E:\\parent\\leaf", "short path normalization is preserved");
    const auto long_path = L"\\\\?\\E:\\" + std::wstring(180, L'a') + L"\\" + std::wstring(180, L'b');
    const auto long_parts = DestinationGuard::ComponentsForTest(long_path);
    Check(!long_parts.empty() && long_parts.back() == long_path, "long extended paths use the large buffer fallback");
    for (const auto* path : {L"\\\\server\\share\\folder", L"\\\\?\\UNC\\server\\share\\folder"}) {
        const auto parts = DestinationGuard::ComponentsForTest(path);
        Check(parts.size() == 2 && parts.back() == path, "UNC path keeps its share root and leaf");
    }
    const auto cleanup = root.lexically_normal();
    const bool owned = cleanup.parent_path() == fs::absolute(L"bench_data").lexically_normal() &&
        !fs::is_symlink(cleanup);
    if (owned) fs::remove_all(cleanup, error);
    Check(owned && !error && !fs::exists(cleanup), "release handles and clean only the exclusive fixture");
    return failures ? 1 : 0;
}
