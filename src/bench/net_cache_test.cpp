#include "fs/fs_net_cache.h"
#include <windows.h>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <new>

#ifndef PULSE_NET_CACHE_TESTING
#error Build this test with PULSE_NET_CACHE_TESTING to isolate the cache directory.
#endif

namespace {
bool limit_allocations = false;
bool large_allocation_attempted = false;
std::filesystem::path cache_directory;
int failures = 0;

void Check(bool value, const char* name) {
    std::cout << (value ? "[PASS] " : "[FAIL] ") << name << '\n';
    if (!value) ++failures;
}

bool WriteBytes(const std::filesystem::path& path, const std::vector<char>& bytes) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    stream.close();
    return static_cast<bool>(stream);
}

struct Fixture {
    ~Fixture() {
        std::error_code error;
        std::filesystem::remove_all(cache_directory, error);
        Check(!error && !std::filesystem::exists(cache_directory), "isolated cache cleaned up");
    }
};
}

// Bound the corrupt-count test before allocation, so a regression cannot consume
// hundreds of megabytes merely to discover that the cache is truncated.
void* operator new(std::size_t size) {
    if (limit_allocations && size >= 1024 * 1024) {
        large_allocation_attempted = true;
        throw std::bad_alloc();
    }
    if (void* value = std::malloc((std::max)(size, std::size_t{1}))) return value;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* value) noexcept { std::free(value); }
void operator delete[](void* value) noexcept { std::free(value); }
void operator delete(void* value, std::size_t) noexcept { std::free(value); }
void operator delete[](void* value, std::size_t) noexcept { std::free(value); }

namespace pulse::fs {
std::wstring NetCacheTestDirectory() { return cache_directory.wstring(); }
}

int wmain() {
    wchar_t temporary[32768]{};
    const DWORD length = GetTempPathW(static_cast<DWORD>(std::size(temporary)), temporary);
    if (!length || length >= std::size(temporary)) {
        Check(false, "temporary directory resolved");
        return 1;
    }
    cache_directory = std::filesystem::path(temporary) /
        (L"PulseNetCacheTest-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
         std::to_wstring(GetTickCount64()));
    if (!CreateDirectoryW(cache_directory.c_str(), nullptr)) {
        Check(false, "unique isolated cache created");
        return 1;
    }
    {
        Fixture fixture;
        const std::wstring unc = L"\\\\pulse-cache-test.invalid\\share\\folder";
        auto entries = std::make_shared<std::vector<pulse::fs::DirEntry>>();
        pulse::fs::DirEntry file;
        file.name = L"Unicode-\u4f60\u597d.txt";
        file.size = 0x123456789ull;
        file.attrs = FILE_ATTRIBUTE_ARCHIVE;
        file.mtime = {0x12345678, 0x87654321};
        entries->push_back(file);
        pulse::fs::DirEntry directory;
        directory.name = L"reparse-directory";
        directory.attrs = FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT;
        directory.is_dir = true;
        directory.is_reparse = true;
        entries->push_back(directory);
        const bool saved = pulse::fs::SaveNetSnapshot(unc, entries);
        Check(saved, "save isolated UNC snapshot without network access");
        uint64_t timestamp = 0;
        auto loaded = pulse::fs::LoadNetSnapshot(unc, &timestamp);
        bool same = loaded && loaded->size() == entries->size();
        if (same) {
            for (size_t i = 0; i < entries->size(); ++i) {
                const auto& actual = (*loaded)[i];
                const auto& expected = (*entries)[i];
                same = same && actual.name == expected.name && actual.size == expected.size &&
                    actual.attrs == expected.attrs && actual.is_dir == expected.is_dir &&
                    actual.is_reparse == expected.is_reparse &&
                    actual.mtime.dwLowDateTime == expected.mtime.dwLowDateTime &&
                    actual.mtime.dwHighDateTime == expected.mtime.dwHighDateTime;
            }
        }
        Check(same && timestamp > 0, "snapshot names, attributes, flags, size and timestamp roundtrip");
        std::filesystem::path cache_file;
        for (const auto& entry : std::filesystem::directory_iterator(cache_directory)) {
            if (entry.path().extension() == L".bin") cache_file = entry.path();
        }
        if (saved && !cache_file.empty()) {
            std::ifstream stream(cache_file, std::ios::binary);
            std::vector<char> valid((std::istreambuf_iterator<char>(stream)),
                                     std::istreambuf_iterator<char>());
            stream.close();
            if (valid.size() <= 20) {
                Check(false, "complete saved cache fixture read");
                return 1;
            }
            bool headers_rejected = valid.size() >= 20;
            for (size_t length_bytes = 0; length_bytes < 20; ++length_bytes) {
                auto truncated = std::vector<char>(valid.begin(), valid.begin() + length_bytes);
                timestamp = 77;
                headers_rejected = WriteBytes(cache_file, truncated) &&
                    !pulse::fs::LoadNetSnapshot(unc, &timestamp) && timestamp == 77 && headers_rejected;
            }
            Check(headers_rejected, "every truncated header is rejected without publishing timestamp");
            auto truncated_name = valid;
            truncated_name.pop_back();
            timestamp = 77;
            Check(WriteBytes(cache_file, truncated_name) &&
                  !pulse::fs::LoadNetSnapshot(unc, &timestamp) && timestamp == 77,
                  "truncated final filename is rejected without partial snapshot");
            auto corrupt_count = std::vector<char>(valid.begin(), valid.begin() + 20);
            const uint32_t count = 500000;
            std::memcpy(corrupt_count.data() + 16, &count, sizeof(count));
            const bool written = WriteBytes(cache_file, corrupt_count);
            bool rejected = false;
            limit_allocations = true;
            try {
                rejected = !pulse::fs::LoadNetSnapshot(unc);
            } catch (const std::bad_alloc&) {
            }
            limit_allocations = false;
            Check(written && rejected && !large_allocation_attempted,
                  "malicious in-range count rejected before large allocation");
            Check(WriteBytes(cache_file, valid) && pulse::fs::LoadNetSnapshot(unc) != nullptr,
                  "valid snapshot remains readable after corrupt fixtures");
        } else {
            Check(false, "isolated cache file found for corruption tests");
        }
    }
    return failures == 0 ? 0 : 1;
}
