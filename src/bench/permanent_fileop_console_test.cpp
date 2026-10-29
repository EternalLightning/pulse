#include "permanent_fileop_fixture.h"
#include "permanent_fileop_parallel_fixture.h"

namespace {
int passes = 0;
int failures = 0;
void Check(bool good, const char* label) {
    std::cout << (good ? "[PASS] " : "[FAIL] ") << label << '\n';
    if (good) ++passes;
    else ++failures;
}
}

int wmain(int argc, wchar_t** argv) {
    namespace fs = std::filesystem;
    const bool readonly_only = argc == 5 && std::wstring_view(argv[3]) == L"--filter" &&
        std::wstring_view(argv[4]) == L"readonly-rollback";
    const bool parallel_only = argc == 5 && std::wstring_view(argv[3]) == L"--filter" &&
        std::wstring_view(argv[4]) == L"parallel";
    if (argc != 1 && ((argc != 3 && !readonly_only && !parallel_only) || std::wstring_view(argv[1]) != L"--root-parent")) {
        std::cout << "usage: pulse_permanent_fileop_console_test [--root-parent PATH [--filter readonly-rollback|parallel]]\n";
        return 2;
    }
    const auto parent = fs::absolute(argc >= 3 ? fs::path(argv[2]) : fs::path(L"bench_data")).lexically_normal();
    const auto root = parent / (L"permanent-fileop-console-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
        std::to_wstring(GetTickCount64()));
    bool owned = false;
    try {
        if (argc == 1) fs::create_directories(parent);
        if (!CreateDirectoryW(root.c_str(), nullptr)) {
            std::cout << "[BLOCKED] exclusive fixture creation Win32=" << GetLastError() << '\n';
            return 2;
        }
        owned = true;
        const auto encoded = root.u8string();
        std::cout << "[INFO] exclusive fixture=";
        std::cout.write(reinterpret_cast<const char*>(encoded.data()), static_cast<std::streamsize>(encoded.size()));
        std::cout << '\n';
        if (readonly_only) pulse::bench::RunPermanentReadonlyFailureFixture(root, Check);
        else if (parallel_only) pulse::bench::RunPermanentParallelFixture(root, Check);
        else pulse::bench::RunPermanentFileopFixture(root, Check);
    } catch (const std::exception& error) {
        std::cout << "[FAIL] fixture exception: " << error.what() << '\n';
        ++failures;
    }
    std::error_code cleanup_error;
    if (owned && root.parent_path() == parent && root.filename().wstring().starts_with(L"permanent-fileop-console-")) {
        for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, cleanup_error), end;
             !cleanup_error && it != end; it.increment(cleanup_error)) {
            const DWORD attributes = GetFileAttributesW(it->path().c_str());
            if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_READONLY))
                SetFileAttributesW(it->path().c_str(), attributes & ~FILE_ATTRIBUTE_READONLY);
        }
        cleanup_error.clear();
        fs::remove_all(root, cleanup_error);
    }
    Check(!cleanup_error && !fs::exists(root), "remove the isolated permanent-delete fixture");
    std::cout << passes << " PASS / " << failures << " FAIL\n";
    return failures ? 1 : 0;
}
