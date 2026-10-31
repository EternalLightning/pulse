// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "../index/text_task_reader.h"
#include <algorithm>
#include <chrono>
#include <clocale>
#include <cstdio>
#include <cstdlib>
#include <cwctype>
#include <memory>
#include <string>
#include <vector>

namespace {
size_t allocation_count = 0;
}
// Count C++ buffer allocations in this dedicated, single-threaded benchmark.
void* operator new(size_t size) {
    if (void* memory = std::malloc(size ? size : 1)) {
        ++allocation_count;
        return memory;
    }
    throw std::bad_alloc();
}
void* operator new[](size_t size) { return ::operator new(size); }
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, size_t) noexcept { std::free(memory); }

namespace {
using pulse::index::ReadTaskTextLiteral;
using pulse::index::TaskTextLiteralResult;
using pulse::index::TaskTextLiteralReader;
using pulse::text::Encoding;
constexpr size_t kChunk = 64 * 1024;
int failures = 0;
void Check(bool condition, const char* label) {
    std::printf("[%s] %s\n", condition ? "PASS" : "FAIL", label);
    if (!condition) ++failures;
}
struct Fixture {
    std::wstring path;
    HANDLE file = INVALID_HANDLE_VALUE;
    Fixture(const std::vector<uint8_t>& bytes, bool delete_on_close = true) {
        static unsigned sequence = 0;
        path = L"bench_data\\text_task_" + std::to_wstring(GetCurrentProcessId()) + L"_" +
               std::to_wstring(++sequence) + L".tmp";
        file = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, CREATE_NEW,
            FILE_ATTRIBUTE_TEMPORARY | (delete_on_close ? FILE_FLAG_DELETE_ON_CLOSE : 0), nullptr);
        DWORD written = 0;
        if (file == INVALID_HANDLE_VALUE || !WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) ||
            written != bytes.size()) ++failures;
        FlushFileBuffers(file);
    }
    ~Fixture() { if (file != INVALID_HANDLE_VALUE) CloseHandle(file); DeleteFileW(path.c_str()); }
};
size_t Find(std::wstring_view body, std::wstring_view needle, bool sensitive) {
    const auto at = std::search(body.begin(), body.end(), needle.begin(), needle.end(),
        [&](wchar_t a, wchar_t b) { return a == b || (!sensitive && towlower(a) == towlower(b)); });
    return at == body.end() ? std::wstring::npos : static_cast<size_t>(at - body.begin());
}
bool Compare(const std::vector<uint8_t>& bytes, std::wstring_view needle, Encoding encoding = Encoding::Auto,
             bool sensitive = false, uint64_t* read_bytes = nullptr, TaskTextLiteralReader* reader = nullptr) {
    Fixture fixture(bytes);
    TaskTextLiteralResult result;
    DWORD error = 0, old_error = 0;
    const bool ok = reader ? reader->Read(fixture.path, bytes.size(), result, &error, encoding, {}) :
        ReadTaskTextLiteral(fixture.path, bytes.size(), result, &error, encoding, {}, needle, sensitive);
    if (read_bytes) *read_bytes = result.bytes_read;
    std::wstring full;
    uint64_t old_bytes = 0;
    const bool old_ok = pulse::text::ReadFile(fixture.path, bytes.size(), full, old_bytes, &old_error, encoding);
    if (ok != old_ok) { std::printf("error=%lu old_error=%lu\n", error, old_error); return false; }
    if (!ok) return error == old_error && result.version.empty();
    BY_HANDLE_FILE_INFORMATION id{};
    FILE_BASIC_INFO basic{};
    if (!GetFileInformationByHandle(fixture.file, &id) ||
        !GetFileInformationByHandleEx(fixture.file, FileBasicInfo, &basic, sizeof(basic))) return false;
    const auto expected_version = std::to_wstring(id.dwVolumeSerialNumber) + L":" + std::to_wstring(id.nFileIndexHigh) + L":" +
        std::to_wstring(id.nFileIndexLow) + L":" + std::to_wstring(basic.ChangeTime.QuadPart);
    if (result.version.empty() || result.version != expected_version) return false;
    const size_t expected = Find(full, needle, sensitive);
    const size_t actual = Find(result.body, needle, sensitive);
    if ((expected == std::wstring::npos) != (actual == std::wstring::npos)) return false;
    if (actual == std::wstring::npos) return result.bytes_read == bytes.size() && result.body.empty();
    const auto expected_line = std::count(full.begin(), full.begin() + expected, L'\n');
    const auto actual_line = result.skipped_lines + std::count(result.body.begin(), result.body.begin() + actual, L'\n');
    return expected_line == actual_line && result.body.size() <= needle.size() + 321;
}
std::vector<uint8_t> Utf16(std::wstring_view text, bool big) {
    std::vector<uint8_t> bytes{static_cast<uint8_t>(big ? 0xfe : 0xff), static_cast<uint8_t>(big ? 0xff : 0xfe)};
    for (wchar_t c : text) {
        bytes.push_back(static_cast<uint8_t>(big ? c >> 8 : c & 0xff));
        bytes.push_back(static_cast<uint8_t>(big ? c & 0xff : c >> 8));
    }
    return bytes;
}
int BenchmarkSmall(bool reuse) {
    constexpr size_t count = 64;
    constexpr size_t rounds = 40;
    std::vector<std::unique_ptr<Fixture>> fixtures;
    for (size_t i = 0; i < count; ++i) {
        std::vector<uint8_t> bytes(4096, 'x');
        for (size_t at = 0; at < bytes.size(); at += 80) bytes[at] = '\n';
        if (i % 2 == 0) {
            const std::string marker = "3D3s";
            std::copy(marker.begin(), marker.end(), bytes.begin() + 2010);
        }
        fixtures.push_back(std::make_unique<Fixture>(bytes));
    }
    for (int sample = 0; sample < 6; ++sample) {
        const size_t allocations_before = allocation_count;
        const auto start = std::chrono::steady_clock::now();
        TaskTextLiteralReader reader(L"3d3s", false);
        size_t hits = 0;
        uint64_t bytes_read = 0;
        for (size_t round = 0; round < rounds; ++round) {
            for (const auto& fixture : fixtures) {
                TaskTextLiteralResult result;
                DWORD error = 0;
                const bool ok = reuse ? reader.Read(fixture->path, 4096, result, &error, Encoding::Auto, {}) :
                    ReadTaskTextLiteral(fixture->path, 4096, result, &error, Encoding::Auto, {}, L"3d3s", false);
                if (!ok) {
                    std::printf("benchmark read failed: %lu\n", error);
                    return 1;
                }
                hits += !result.body.empty();
                bytes_read += result.bytes_read;
            }
        }
        const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - start).count();
        std::printf("small sample=%d files=%zu file_bytes=4096 hits=%zu bytes_read=%llu elapsed_us=%lld cpp_allocations=%zu reuse=%d\n",
            sample, count * rounds, hits, static_cast<unsigned long long>(bytes_read), static_cast<long long>(elapsed),
            allocation_count - allocations_before, reuse ? 1 : 0);
        if (hits != count * rounds / 2 || bytes_read != count * rounds * 4096) return 1;
    }
    return failures ? 1 : 0;
}
}
int main(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--benchmark-small") return BenchmarkSmall(true);
    if (argc == 2 && std::string_view(argv[1]) == "--benchmark-small-fresh") return BenchmarkSmall(false);
    std::printf("ACP=%u; fixtures <= 2 MiB, deleted on close\n", GetACP());
    std::vector<uint8_t> bytes(2 * 1024 * 1024, 'x');
    const std::string marker = "needle";
    std::copy(marker.begin(), marker.end(), bytes.begin() + 20);
    uint64_t read = 0;
    const auto start = std::chrono::steady_clock::now();
    Check(Compare(bytes, L"needle", Encoding::Auto, false, &read) && read == kChunk, "Auto ASCII hit reads one 64 KiB block");
    std::printf("early-hit bytes=%llu/%zu; comparison elapsed=%lld ms\n", static_cast<unsigned long long>(read), bytes.size(),
        static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count()));
    Check(Compare(bytes, L"absent", Encoding::Auto, false, &read) && read == bytes.size(), "absent literal checks full input with bounded output");
    bytes.assign(3 * kChunk, 'x');
    for (size_t i = 0; i < bytes.size(); i += 100) bytes[i] = '\n';
    std::copy(marker.begin(), marker.end(), bytes.begin() + kChunk - 3);
    Check(Compare(bytes, L"needle"), "literal across read boundary preserves line number");
    std::copy(marker.begin(), marker.end(), bytes.begin() + kChunk + 40);
    Check(Compare(bytes, L"NEEDLE") && Compare(bytes, L"NEEDLE", Encoding::Auto, true), "case-sensitive and insensitive matching");
    bytes.assign(2 * kChunk, 'x');
    const std::string actual_case = "3D3s";
    std::copy(actual_case.begin(), actual_case.end(), bytes.begin() + kChunk - 2);
    bytes[10] = '\n'; bytes[kChunk - 30] = '\n';
    Check(Compare(bytes, L"3d3s") && Compare(bytes, L"3d3s", Encoding::Auto, true),
          "actual ASCII needle preserves mixed-case and line semantics across blocks");
    for (size_t split = 1; split < 4; ++split) {
        bytes.assign(2 * kChunk, 'x');
        const std::string utf8 = "\xf0\x9f\x98\x80needle";
        std::copy(utf8.begin(), utf8.end(), bytes.begin() + kChunk - split);
        Check(Compare(bytes, L"\xd83d\xde00needle", Encoding::Utf8), "UTF8 four-byte character across boundary");
    }
    std::wstring wide(kChunk, L'x');
    wide[10] = L'\n';
    wide.replace(kChunk / 2 - 2, 8, L"\xd83d\xde00needle");
    Check(Compare(Utf16(wide, false), L"\xd83d\xde00needle"), "UTF16 LE surrogate across block");
    Check(Compare(Utf16(wide, true), L"\xd83d\xde00needle"), "UTF16 BE surrogate across block");
    const std::wstring mixed = L"first\n\u4e2d\u6587 \u00c4\u03a3 3D3s\nlast";
    const std::string saved_locale = std::setlocale(LC_CTYPE, nullptr);
    const bool unicode_locale = std::setlocale(LC_CTYPE, ".UTF8") != nullptr;
    Check(unicode_locale && towlower(L'\u00c4') == L'\u00e4' && towlower(L'\u03a3') == L'\u03c3' &&
          Compare(Utf16(mixed, false), L"\u4e2d\u6587 \u00e4\u03c3 3d3s") &&
          Compare(Utf16(mixed, false), L"\u4e2d\u6587 \u00e4\u03c3 3d3s", Encoding::Auto, true) &&
          Compare(Utf16(mixed, true), L"\u4e2d\u6587 \u00c4\u03a3 3D3s", Encoding::Auto, true),
          "Chinese and non-ASCII case folding agrees with complete decoder matching");
    std::setlocale(LC_CTYPE, saved_locale.c_str());
    wide.replace(20, 6, L"needle");
    Check(Compare(Utf16(wide, false), L"needle", Encoding::Auto, false, &read) && read == kChunk, "UTF16 BOM early hit");
    auto odd = Utf16(wide, false); odd.push_back(1);
    Check(Compare(odd, L"needle"), "odd-length UTF16 rejected before early hit");
    bytes.assign(2 * kChunk, 'x');
    std::copy(marker.begin(), marker.end(), bytes.begin() + 20);
    bytes.back() = 0xff;
    Check(Compare(bytes, L"needle", Encoding::Auto), "Auto ASCII early hit unaffected by late ANSI fallback");
    Check(Compare(bytes, L"needle", Encoding::Utf8), "forced UTF8 validates malformed suffix after hit");
    const std::string chinese = "\xe4\xbd\xa0\xe5\xa5\xbd";
    std::copy(chinese.begin(), chinese.end(), bytes.begin() + 100);
    Check(Compare(bytes, L"\u4f60\u597d"), "late invalid UTF8 chooses ANSI despite earlier UTF8-only match");
    bytes[0] = 0xef; bytes[1] = 0xbb; bytes[2] = 0xbf;
    Check(Compare(bytes, L"needle"), "UTF8 BOM retains strict suffix validation");
    bytes.assign(kChunk + 10, 'x'); bytes[kChunk - 1] = 0;
    Check(Compare(bytes, L"xx"), "complete binary sample checked before hit");
    Check(Compare({}, L"needle"), "empty file");
    bytes.assign(2 * kChunk, 'x');
    {
        Fixture fixture(bytes);
        TaskTextLiteralResult result;
        DWORD error = 0, before = 0, after = 0;
        GetProcessHandleCount(GetCurrentProcess(), &before);
        int calls = 0;
        const bool ok = ReadTaskTextLiteral(fixture.path, bytes.size(), result, &error, Encoding::Auto,
            [&] { return ++calls >= 3; }, L"absent", false);
        GetProcessHandleCount(GetCurrentProcess(), &after);
        Check(!ok && error == ERROR_CANCELLED && result.bytes_read == kChunk && before == after && result.version.empty(),
              "mid-read cancellation closes file handle");
        Check(!ReadTaskTextLiteral(fixture.path, bytes.size() - 1, result, &error, Encoding::Auto, {}, L"x", false) &&
              error == ERROR_FILE_TOO_LARGE && !result.bytes_read && result.version.empty(), "size cap retained");
        Check(!ReadTaskTextLiteral(fixture.path, bytes.size(), result, &error, Encoding::Auto,
              [] { return true; }, L"x", false) && error == ERROR_CANCELLED && !result.bytes_read, "pre-read cancellation");
        calls = 0;
        const bool modified_ok = ReadTaskTextLiteral(fixture.path, bytes.size(), result, &error, Encoding::Auto,
            [&] {
                if (++calls == 3) {
                    FILETIME time{};
                    GetSystemTimeAsFileTime(&time);
                    time.dwHighDateTime += 1;
                    SetFileTime(fixture.file, nullptr, nullptr, &time);
                }
                return false;
            }, L"absent", false);
        Check(!modified_ok && error == ERROR_RETRY && result.body.empty() && result.version.empty(), "file modification invalidates result");
    }
    {
        Fixture fixture(bytes);
        FILE_BASIC_INFO before_basic{}, after_basic{};
        BY_HANDLE_FILE_INFORMATION before_id{}, after_id{};
        bool mutation_ok = GetFileInformationByHandleEx(fixture.file, FileBasicInfo, &before_basic, sizeof(before_basic)) &&
            GetFileInformationByHandle(fixture.file, &before_id);
        TaskTextLiteralResult result;
        result.version = L"stale-version";
        DWORD error = 0;
        int calls = 0;
        const bool ok = ReadTaskTextLiteral(fixture.path, bytes.size(), result, &error, Encoding::Auto,
            [&] {
                if (++calls == 3) {
                    LARGE_INTEGER start{};
                    const char changed = 'y';
                    DWORD written = 0;
                    auto updated = before_basic;
                    updated.ChangeTime.QuadPart += 10000000;
                    mutation_ok = mutation_ok && SetFilePointerEx(fixture.file, start, nullptr, FILE_BEGIN) &&
                        WriteFile(fixture.file, &changed, 1, &written, nullptr) && written == 1 && FlushFileBuffers(fixture.file) &&
                        SetFileInformationByHandle(fixture.file, FileBasicInfo, &updated, sizeof(updated)) &&
                        GetFileInformationByHandleEx(fixture.file, FileBasicInfo, &after_basic, sizeof(after_basic)) &&
                        GetFileInformationByHandle(fixture.file, &after_id);
                }
                return false;
            }, L"absent", false);
        Check(mutation_ok && before_id.nFileSizeHigh == after_id.nFileSizeHigh && before_id.nFileSizeLow == after_id.nFileSizeLow &&
              CompareFileTime(&before_id.ftLastWriteTime, &after_id.ftLastWriteTime) == 0 &&
              before_basic.ChangeTime.QuadPart != after_basic.ChangeTime.QuadPart && !ok && error == ERROR_RETRY &&
              result.body.empty() && result.version.empty(), "same-size same-mtime modification invalidates ChangeTime version");
    }
    // Compare ANSI and GB18030 against the existing whole-file decoder, with
    // characters and malformed sequences straddling the actual read boundary.
    bool boundary_ok = true;
    for (const auto& raw : {std::string("\xc4\xe3\xba\xc3"), std::string("\x81\x30\x81\x30"),
                            std::string("\x81\x20\x81\x81"), std::string("\x81\x81\x81")}) {
        for (size_t split = 1; split <= raw.size(); ++split) {
            bytes.assign(2 * kChunk, 'x');
            std::copy(raw.begin(), raw.end(), bytes.begin() + kChunk - split);
            std::wstring decoded;
            std::vector<uint8_t> needle_bytes(raw.begin(), raw.end());
            for (Encoding encoding : {Encoding::Auto, Encoding::System, Encoding::Gb18030}) {
                if (pulse::text::Decode(needle_bytes, decoded, encoding) && !decoded.empty())
                    boundary_ok = Compare(bytes, decoded, encoding) && boundary_ok;
                boundary_ok = Compare(bytes, L"absent", encoding) && boundary_ok;
            }
        }
    }
    Check(boundary_ok, "ANSI/GB18030 multibyte and malformed block boundaries agree with full decoder");
    {
        TaskTextLiteralReader reader(L"needle", false);
        bool reused_ok = true;
        for (Encoding encoding : {Encoding::Auto, Encoding::Utf8, Encoding::System, Encoding::Gb18030}) {
            bytes.assign(2 * kChunk, 'x');
            bytes[10] = '\n';
            std::copy(marker.begin(), marker.end(), bytes.begin() + kChunk - 3);
            reused_ok = Compare(bytes, L"needle", encoding, false, nullptr, &reader) && reused_ok;
            bytes.assign(4096, 'x');
            reused_ok = Compare(bytes, L"needle", encoding, false, nullptr, &reader) && reused_ok;
            bytes.back() = 0xff;
            reused_ok = Compare(bytes, L"needle", encoding, false, nullptr, &reader) && reused_ok;
        }
        reused_ok = Compare(Utf16(L"first\nNEEDLE\nlast", false), L"needle", Encoding::Auto, false, nullptr, &reader) && reused_ok;
        reused_ok = Compare(Utf16(L"missing", true), L"needle", Encoding::Auto, false, nullptr, &reader) && reused_ok;
        reused_ok = Compare({}, L"needle", Encoding::Auto, false, nullptr, &reader) && reused_ok;
        Check(reused_ok, "reused reader resets matches, line offsets, decoder validity, byte carries, and encodings");
        bytes.assign(kChunk, 'x');
        std::copy(marker.begin(), marker.end(), bytes.begin() + 20);
        Fixture fixture(bytes);
        TaskTextLiteralResult result;
        DWORD error = 0;
        Check(!reader.Read(fixture.path, bytes.size(), result, &error, Encoding::Auto, [] { return true; }) &&
              error == ERROR_CANCELLED && reader.Read(fixture.path, bytes.size(), result, &error, Encoding::Auto, {}) &&
              Find(result.body, L"needle", false) != std::wstring::npos,
              "cancelled reusable reader can prepare matching buffers again");
        std::vector<uint8_t> prefix(kChunk, 'x');
        prefix[prefix.size() - 3] = 'n'; prefix[prefix.size() - 2] = 'e'; prefix.back() = 'e';
        Check(Compare(prefix, L"needle", Encoding::Auto, false, nullptr, &reader) &&
              Compare({'d', 'l', 'e'}, L"needle", Encoding::Auto, false, nullptr, &reader),
              "partial literal never joins across files");
    }
    {
        bytes.assign(2 * kChunk, 'x');
        std::copy(marker.begin(), marker.end(), bytes.begin() + 20);
        Fixture fixture(bytes);
        TaskTextLiteralReader reader(L"needle", false);
        TaskTextLiteralResult result;
        DWORD error = 0, before_handles = 0, after_handles = 0;
        bool metadata_ok = false;
        GetProcessHandleCount(GetCurrentProcess(), &before_handles);
        const bool skipped = reader.Read(fixture.path, bytes.size(), result, &error, Encoding::Auto, {},
            [&](const pulse::index::TaskTextMetadata& metadata) {
                metadata_ok = metadata.size == bytes.size() && metadata.modified && !metadata.version.empty();
                return false;
            });
        GetProcessHandleCount(GetCurrentProcess(), &after_handles);
        Check(skipped && !error && metadata_ok && result.skipped && !result.bytes_read && result.body.empty() &&
              result.version.empty() && before_handles == after_handles,
              "preflight cache/filter skip reads no body and releases the opened handle");
        std::wstring opened_version;
        Check(reader.Read(fixture.path, bytes.size(), result, &error, Encoding::Auto, {},
            [&](const pulse::index::TaskTextMetadata& metadata) { opened_version.assign(metadata.version); return true; }) &&
            !result.skipped && result.version == opened_version && Find(result.body, L"needle", false) != std::wstring::npos,
            "preflight and completed read use the same version and reset a previous skip");
        bool cancelled = false;
        Check(!reader.Read(fixture.path, bytes.size(), result, &error, Encoding::Auto, [&] { return cancelled; },
            [&](const auto&) { cancelled = true; return false; }) && error == ERROR_CANCELLED && !result.bytes_read,
            "cancellation during cache preflight prevents reading or publishing a skip");
        Fixture changing(bytes);
        bool changed = false;
        const bool changed_read = reader.Read(changing.path, bytes.size(), result, &error, Encoding::Auto, {}, [&](const auto&) {
            LARGE_INTEGER start{}; DWORD written = 0;
            changed = SetFilePointerEx(changing.file, start, nullptr, FILE_END) &&
                WriteFile(changing.file, "changed", 7, &written, nullptr) && written == 7 && FlushFileBuffers(changing.file);
            return true;
        });
        Check(changed && !changed_read && error == ERROR_RETRY && result.body.empty() && result.version.empty(),
              "modification after handle preflight still invalidates the read");
        Fixture original(bytes, false), replacement(bytes, false);
        const auto moved_path = original.path + L".old";
        bool replaced = false;
        DWORD replace_error = 0;
        const bool replaced_read = reader.Read(original.path, bytes.size(), result, &error, Encoding::Auto, {}, [&](const auto&) {
            replaced = MoveFileExW(original.path.c_str(), moved_path.c_str(), 0) &&
                MoveFileExW(replacement.path.c_str(), original.path.c_str(), 0);
            if (!replaced) replace_error = GetLastError();
            return true;
        });
        if (!replaced) std::printf("path replacement setup error=%lu\n", replace_error);
        Check(replaced && !replaced_read && error == ERROR_RETRY && result.body.empty() && result.version.empty(),
              "path replacement after handle preflight rejects the old file result");
        CloseHandle(original.file); original.file = INVALID_HANDLE_VALUE;
        DeleteFileW(moved_path.c_str());
    }
    std::printf("failures=%d\n", failures);
    return failures ? 1 : 0;
}
