// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "../ops/transfer_hash.h"
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <new>
#include <thread>

namespace {
thread_local bool fail_large_allocation = false;
int failures = 0;
void Check(bool passed, const char* name) {
    std::printf("[%s] %s\n", passed ? "PASS" : "FAIL", name);
    failures += !passed;
}
}

void* operator new(size_t size) {
    if (fail_large_allocation && size >= 1024 * 1024) {
        fail_large_allocation = false;
        throw std::bad_alloc();
    }
    if (void* value = std::malloc(size ? size : 1)) return value;
    throw std::bad_alloc();
}
void operator delete(void* value) noexcept { std::free(value); }
void* operator new[](size_t size) { return ::operator new(size); }
void operator delete[](void* value) noexcept { ::operator delete(value); }
void operator delete(void* value, size_t) noexcept { ::operator delete(value); }
void operator delete[](void* value, size_t) noexcept { ::operator delete(value); }

int main() {
    namespace fs = std::filesystem;
    const auto root = fs::absolute(L"bench_data") / (L"transfer-hash-" +
        std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    std::error_code fixture_error;
    fs::create_directories(root, fixture_error);
    if (fixture_error) {
        std::printf("[FAIL] create hash fixture: error=%d\n", fixture_error.value());
        return 2;
    }
    const auto file = root / L"abc.txt";
    { std::ofstream out(file, std::ios::binary); out << "abc"; }
    std::atomic<bool> cancel{false}, pause{false};
    std::array<uint8_t, 32> digest{};
    const std::array<uint8_t, 32> expected{
        0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea,
        0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23,
        0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c,
        0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad};
    std::wstring error;
    Check(pulse::ops::Sha256File(file.wstring(), cancel, pause, digest, error) && digest == expected,
        "copy verification returns the known SHA-256 digest for abc");

    DWORD handles_before = 0, handles_after = 0;
    const bool counted_before = GetProcessHandleCount(GetCurrentProcess(), &handles_before) != FALSE;
    unsigned exceptions = 0;
    for (unsigned i = 0; i < 32; ++i) {
        fail_large_allocation = true;
        try { pulse::ops::Sha256File(file.wstring(), cancel, pause, digest, error); }
        catch (const std::bad_alloc&) { ++exceptions; }
        fail_large_allocation = false;
    }
    const bool counted_after = GetProcessHandleCount(GetCurrentProcess(), &handles_after) != FALSE;
    Check(exceptions == 32 && counted_before && counted_after && handles_after <= handles_before,
        "buffer allocation exceptions release every opened verification file handle");
    Check(pulse::ops::Sha256File(file.wstring(), cancel, pause, digest, error) && digest == expected,
        "hashing remains usable after allocation exceptions");

    error.clear();
    Check(!pulse::ops::Sha256File((root / L"missing.txt").wstring(), cancel, pause, digest, error) &&
        error.find(L"0x80070002") != std::wstring::npos,
        "missing verification input reports its original file-not-found error");
    error.clear();
    pause.store(true);
    digest.fill(0xA5);
    const auto unchanged = digest;
    std::thread canceller([&] { Sleep(30); cancel.store(true); });
    const bool hashed = pulse::ops::Sha256File(file.wstring(), cancel, pause, digest, error);
    canceller.join();
    Check(!hashed && digest == unchanged && error.empty(),
        "cancelling paused verification exits without publishing a partial digest");

    Check(DeleteFileW(file.c_str()) && RemoveDirectoryW(root.c_str()),
        "verification fixture releases its handles and cleans only its own file and directory");
    return failures ? 1 : 0;
}
