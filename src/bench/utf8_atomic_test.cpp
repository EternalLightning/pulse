// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "../common/utf8_file.h"
#include <filesystem>
#include <iostream>

int main() {
    const auto root = std::filesystem::current_path() / L"bench_data" /
        (L"utf8-atomic-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    std::filesystem::create_directories(root);
    int failures = 0;
    auto check = [&](bool ok, const char* name) {
        std::cout << (ok ? "[PASS] " : "[FAIL] ") << name << '\n';
        failures += !ok;
    };
    const auto path = (root / L"settings.json").wstring();
    const auto legacy = path + L".tmp";
    HANDLE held = CreateFileW(legacy.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, 0, nullptr);
    check(held != INVALID_HANDLE_VALUE, "hold another writer's legacy temporary file");
    check(pulse::WriteUtf8FileAtomic(path, L"first 汉字"), "independent save succeeds while legacy temporary file is held");
    if (held != INVALID_HANDLE_VALUE) CloseHandle(held);
    std::wstring read;
    check(pulse::ReadUtf8File(path, read) && read == L"first 汉字", "complete UTF-8 content is readable");
    check(GetFileAttributesW(legacy.c_str()) != INVALID_FILE_ATTRIBUTES, "foreign temporary file is preserved");
    check(pulse::WriteUtf8FileAtomic(path, L"replacement"), "existing destination is atomically replaced");
    check(!pulse::WriteUtf8FileAtomic(path, std::wstring(1, wchar_t(0xD800))) &&
        pulse::ReadUtf8File(path, read) && read == L"replacement", "invalid Unicode preserves previous destination");
    HANDLE destination = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    check(destination != INVALID_HANDLE_VALUE && !pulse::WriteUtf8FileAtomic(path, L"blocked"), "denied replacement reports failure");
    if (destination != INVALID_HANDLE_VALUE) CloseHandle(destination);
    check(pulse::ReadUtf8File(path, read) && read == L"replacement", "failed replacement preserves original content");
    size_t files = 0;
    for (const auto& entry : std::filesystem::directory_iterator(root)) { (void)entry; ++files; }
    check(files == 2, "owned temporary files cleaned after success and failure");
    // Remove only the two files created by this isolated fixture.
    std::filesystem::remove(path);
    std::filesystem::remove(legacy);
    std::filesystem::remove(root);
    return failures ? 1 : 0;
}
