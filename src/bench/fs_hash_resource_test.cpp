// Focused equivalent-fix fixture. The source overrides select preserved baseline
// translation units without rewriting or replacing production files.
#ifndef PULSE_TEST_ENUM_SOURCE
#define PULSE_TEST_ENUM_SOURCE "../fs/fs_enum.cpp"
#endif
#ifndef PULSE_TEST_HASH_SOURCE
#define PULSE_TEST_HASH_SOURCE "../index/content_search.cpp"
#endif
#include PULSE_TEST_ENUM_SOURCE
#include PULSE_TEST_HASH_SOURCE
#include <atomic>
#include <barrier>
#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <new>
#include <thread>
#ifdef _DEBUG
#include <crtdbg.h>
#endif

namespace {
thread_local int fail_allocation_after = -1;
thread_local size_t fail_allocation_minimum = 0;
}
void* operator new(size_t bytes) {
    if (bytes >= fail_allocation_minimum) {
        if (fail_allocation_after == 0) {
            fail_allocation_after = -1;
            throw std::bad_alloc();
        }
        if (fail_allocation_after > 0) --fail_allocation_after;
    }
    if (void* value = std::malloc(bytes ? bytes : 1)) return value;
    throw std::bad_alloc();
}
void operator delete(void* value) noexcept { std::free(value); }
void* operator new[](size_t bytes) { return ::operator new(bytes); }
void operator delete[](void* value) noexcept { ::operator delete(value); }
void operator delete(void* value, size_t) noexcept { ::operator delete(value); }
void operator delete[](void* value, size_t) noexcept { ::operator delete(value); }

// This fixture calls only duplicate hashing, never document/provider reading.
namespace pulse::index {
DocumentReadSession::DocumentReadSession() {}
DocumentReadSession::~DocumentReadSession() {}
bool IsExtractedDocumentExtension(std::wstring_view) noexcept { return false; }
bool IsIndexedContentExtension(std::wstring_view) noexcept { return false; }
bool ReadSearchableDocument(const std::wstring&, uint64_t, std::wstring&, uint64_t&, DWORD*,
    text::Encoding, const std::function<bool()>&, std::wstring_view, bool, DocumentReadMetrics*) { return false; }
}

int wmain(int argc, wchar_t** argv) {
#ifdef _DEBUG
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
#endif
    if (argc < 2) return 2;
    const std::wstring root = argv[1];
    const bool measure_only = argc > 2 && std::wstring_view(argv[2]) == L"--measure-only";
    int failures = 0;
    auto check = [&](bool passed, const char* name) {
        std::cout << (passed ? "[PASS] " : "[FAIL] ") << name << std::endl;
        failures += !passed;
    };
    const auto normalized = pulse::fs::NormalizePath(root);
    std::vector<pulse::fs::DirEntry> reference;
    pulse::fs::EnumerateFindFirstFileEx(normalized, reference);
    check(reference.size() == 258, "isolated enumeration fixture has expected 258 entries");
    std::atomic<int> bad{0};
    std::barrier start(8);
    std::vector<std::thread> threads;
    for (int i = 0; i < 8; ++i) threads.emplace_back([&] {
        start.arrive_and_wait();
        for (int iteration = 0; iteration < 20; ++iteration) {
            try {
                std::vector<pulse::fs::DirEntry> actual;
                pulse::fs::EnumerateNtQuery(normalized, actual);
                if (actual.size() != reference.size()) ++bad;
            } catch (...) { ++bad; }
        }
    });
    for (auto& thread : threads) thread.join();
    check(bad == 0, "concurrent first-use NT APIs return complete listings");
    auto names = [](const auto& entries) {
        std::vector<std::wstring> result;
        for (const auto& entry : entries) result.push_back(entry.name);
        std::sort(result.begin(), result.end()); return result;
    };
    std::vector<pulse::fs::DirEntry> actual;
    pulse::fs::EnumerateDirectory(root, actual);
    check(names(actual) == names(reference), "NT and fallback names agree without changing behavior");
    pulse::index::Candidate one;
    one.path = root + L"\\hash-one.bin"; one.name = L"hash-one.bin"; one.size = 4ull * 1024 * 1024;
    auto two = one; two.path = root + L"\\hash-two.bin"; two.name = L"hash-two.bin";
    std::atomic<bool> cancelled{false};
    std::array<uint8_t, 32> digest_one{}, digest_two{};
    check(pulse::index::SampleHash(one) == pulse::index::SampleHash(two) && pulse::index::SampleHash(one) != 0,
        "64 KiB head/tail sample remains equivalent");
    check(pulse::index::FullSha256(one, cancelled, digest_one) && pulse::index::FullSha256(two, cancelled, digest_two) && digest_one == digest_two,
        "full SHA-256 remains equivalent for duplicate files");
    cancelled = true;
    check(!pulse::index::FullSha256(one, cancelled, digest_one), "cancelled full hash remains rejected");
    cancelled = false;
    auto missing = one; missing.path += L".missing";
    check(pulse::index::SampleHash(missing) == 0 && !pulse::index::FullSha256(missing, cancelled, digest_one), "missing-file hash behavior is preserved");
    std::vector<double> enum_times, hash_times;
    for (int i = 0; i < 15; ++i) {
        auto before = std::chrono::steady_clock::now();
        for (int n = 0; n < 20; ++n) { actual.clear(); pulse::fs::EnumerateDirectory(root, actual); }
        auto after = std::chrono::steady_clock::now();
        enum_times.push_back(std::chrono::duration<double, std::milli>(after - before).count() / 20);
        before = std::chrono::steady_clock::now();
        pulse::index::SampleHash(one); pulse::index::FullSha256(one, cancelled, digest_one);
        after = std::chrono::steady_clock::now();
        hash_times.push_back(std::chrono::duration<double, std::milli>(after - before).count());
    }
    std::sort(enum_times.begin(), enum_times.end()); std::sort(hash_times.begin(), hash_times.end());
    std::cout << std::fixed << std::setprecision(3) << "[MEASURE] warm enumeration 258 items median_ms=" << enum_times[7]
        << " p90_ms=" << enum_times[13] << " sample_plus_full_4MiB median_ms=" << hash_times[7] << " p90_ms=" << hash_times[13] << std::endl;
    if (!measure_only) {
        DWORD before = 0, after = 0;
        GetProcessHandleCount(GetCurrentProcess(), &before);
        // Debug string default constructors allocate a small iterator proxy in
        // noexcept code. Inject actual buffers/row growth, not those proxies.
        fail_allocation_minimum = 64;
        for (int failure = 0; failure < 100; ++failure) {
            actual.clear();
            fail_allocation_after = failure;
            try { pulse::fs::EnumerateNtQuery(normalized, actual); } catch (const std::bad_alloc&) {}
            fail_allocation_after = -1;
        }
        GetProcessHandleCount(GetCurrentProcess(), &after);
        std::cout << "[INFO] enum allocation failures handle_before=" << before << " handle_after=" << after << std::endl;
        check(before == after, "enumeration allocation failures release directory and event handles");
        GetProcessHandleCount(GetCurrentProcess(), &before);
        for (int failure = 0; failure < 100; ++failure) {
            actual.clear();
            fail_allocation_after = failure;
            try { pulse::fs::EnumerateFindFirstFileEx(normalized, actual); } catch (const std::bad_alloc&) {}
            fail_allocation_after = -1;
        }
        GetProcessHandleCount(GetCurrentProcess(), &after);
        check(before == after, "fallback enumeration allocation failures release find handles");
        GetProcessHandleCount(GetCurrentProcess(), &before);
        for (int failure = 0; failure < 8; ++failure) {
            fail_allocation_after = failure;
            try { pulse::index::SampleHash(one); } catch (const std::bad_alloc&) {}
            fail_allocation_after = -1;
            fail_allocation_after = failure;
            try { pulse::index::FullSha256(one, cancelled, digest_one); } catch (const std::bad_alloc&) {}
            fail_allocation_after = -1;
        }
        fail_allocation_minimum = 0;
        GetProcessHandleCount(GetCurrentProcess(), &after);
        std::cout << "[INFO] hash allocation failures handle_before=" << before << " handle_after=" << after << std::endl;
        check(before == after, "sample and full-hash allocation failures release resources");
    }
    std::cout << (failures ? "[FAIL] " : "[PASS] ") << "scoped resources/behavior fixture; read-only inputs, no user preferences\n";
    return failures ? 1 : 0;
}
