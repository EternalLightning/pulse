// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "../app/app_drop_shortcuts.h"
#include "../common/path_utils.h"
#include <shlobj.h>
#include <wrl/client.h>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>

namespace {
int failures = 0;
void Check(bool condition, const char* name) {
    std::cout << (condition ? "[PASS] " : "[FAIL] ") << name << '\n';
    if (!condition) ++failures;
}

std::wstring ReadTarget(const std::filesystem::path& file) {
    Microsoft::WRL::ComPtr<IShellLinkW> link;
    Microsoft::WRL::ComPtr<IPersistFile> persist;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link))) ||
        FAILED(link.As(&persist)) || FAILED(persist->Load(file.c_str(), STGM_READ))) return {};
    wchar_t target[32768]{};
    if (FAILED(link->GetPath(target, ARRAYSIZE(target), nullptr, SLGP_RAWPATH))) return {};
    return target;
}

std::wstring ReadWorkingDirectory(const std::filesystem::path& file) {
    Microsoft::WRL::ComPtr<IShellLinkW> link;
    Microsoft::WRL::ComPtr<IPersistFile> persist;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link))) ||
        FAILED(link.As(&persist)) || FAILED(persist->Load(file.c_str(), STGM_READ))) return {};
    wchar_t value[32768]{};
    if (FAILED(link->GetWorkingDirectory(value, ARRAYSIZE(value)))) return {};
    return value;
}

std::wstring ReadStreamTarget(const std::filesystem::path& file) {
    std::ifstream input(file, std::ios::binary);
    if (!input) return {};
    const std::vector<char> bytes{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    Microsoft::WRL::ComPtr<IStream> stream;
    Microsoft::WRL::ComPtr<IShellLinkW> link;
    Microsoft::WRL::ComPtr<IPersistStream> persist;
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream)) ||
        FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link))) ||
        FAILED(link.As(&persist))) return {};
    ULONG written = 0;
    if (bytes.empty() || FAILED(stream->Write(bytes.data(), static_cast<ULONG>(bytes.size()), &written)) ||
        written != bytes.size()) return {};
    LARGE_INTEGER beginning{};
    if (FAILED(stream->Seek(beginning, STREAM_SEEK_SET, nullptr)) || FAILED(persist->Load(stream.Get()))) return {};
    wchar_t target[32768]{};
    if (FAILED(link->GetPath(target, ARRAYSIZE(target), nullptr, SLGP_RAWPATH))) return {};
    return target;
}
} // namespace

int main() {
    using namespace pulse::app;
    namespace fs = std::filesystem;
    Check(ShortcutDropRequested(kDropAltKey) && ShortcutDropRequested(MK_CONTROL | MK_SHIFT) &&
        !ShortcutDropRequested(MK_CONTROL) && !ShortcutDropRequested(MK_SHIFT),
        "Alt and Ctrl+Shift request shortcut, single modifiers retain transfer semantics");
    Check(IsShortcutFileSystemPath(L"C:\\folder") && IsShortcutFileSystemPath(L"\\\\server\\share\\folder") &&
        IsShortcutFileSystemPath(L"\\\\?\\UNC\\server\\share\\folder") &&
        !IsShortcutFileSystemPath(L"pulse:device:phone\\storage") &&
        !IsShortcutFileSystemPath(L"::pidl:0011") && !IsShortcutFileSystemPath(L"C:relative") &&
        !IsShortcutFileSystemPath(L"\\\\.\\pipe\\test"),
        "filesystem and UNC paths accepted; virtual, relative and device namespaces rejected");

    const fs::path root = fs::absolute(fs::path(L"bench_data") /
        (L"drop-shortcuts-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64())));
    struct Cleanup {
        fs::path root;
        ~Cleanup() {
            std::error_code error;
            fs::remove_all(fs::path(L"\\\\?\\" + root.wstring()), error);
        }
    } cleanup{root};
    fs::create_directories(root / L"source" / L"folder");
    fs::create_directories(root / L"destination");
    const auto source = root / L"source" / L"sample.txt";
    { std::ofstream stream(source); stream << "original source bytes"; }
    const auto folder = root / L"source" / L"folder";
    const auto dest = root / L"destination";
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(com)) return 2;
    struct ComDone { ~ComDone() { CoUninitialize(); } } done;

    auto result = CreateDropShortcuts({source.wstring(), folder.wstring()}, dest.wstring(), L"%s - Shortcut");
    Check(result.created == 2 && result.failed == 0 &&
        ReadTarget(dest / L"sample.txt - Shortcut.lnk") == source.wstring() &&
        ReadTarget(dest / L"folder - Shortcut.lnk") == folder.wstring() &&
        ReadWorkingDirectory(dest / L"sample.txt - Shortcut.lnk") == source.parent_path().wstring(),
        "file and folder shortcuts persist correct targets and file working directory");
    Check(fs::file_size(source) == 21 && fs::is_directory(folder), "LINK creation retains original file and folder");

    { std::ofstream stream(dest / L"sample.txt - Shortcut (2).lnk"); stream << "existing reservation"; }
    result = CreateDropShortcuts({source.wstring()}, dest.wstring(), L"%s - Shortcut");
    Check(result.created == 1 && result.failed == 0 &&
        ReadTarget(dest / L"sample.txt - Shortcut (3).lnk") == source.wstring() &&
        fs::file_size(dest / L"sample.txt - Shortcut (2).lnk") == 20,
        "existing shortcuts and conflicts are retained, numbering advances to free name");

    result = CreateDropShortcuts({(root / L"missing.txt").wstring(), source.wstring()}, dest.wstring(), L"%s - Partial");
    Check(result.created == 1 && result.failed == 1 && FAILED(result.error) &&
        result.failed_source == (root / L"missing.txt").wstring() &&
        fs::exists(dest / L"sample.txt - Partial.lnk") && !fs::exists(dest / L"missing.txt - Partial.lnk"),
        "mixed success reports missing source and leaves no failed shortcut");
    result = CreateDropShortcuts({source.wstring()}, source.wstring(), L"%s - Invalid");
    Check(result.created == 0 && result.failed == 1 && FAILED(result.error) && fs::file_size(source) == 21,
        "non-directory destination fails without touching source");
    result = CreateDropShortcuts({L"::pidl:0011"}, dest.wstring(), L"%s");
    const auto virtual_result = CreateDropShortcuts({source.wstring()}, L"pulse:device:phone\\storage", L"%s");
    Check(result.created == 0 && result.failed == 1 && virtual_result.created == 0 && virtual_result.failed == 1,
        "virtual sources and destinations fail without creating links");

    const auto run = [&] { return CreateDropShortcuts({source.wstring()}, dest.wstring(), L"%s - Concurrent"); };
    auto first = std::async(std::launch::async, run);
    auto second = std::async(std::launch::async, run);
    const auto first_result = first.get();
    const auto second_result = second.get();
    Check(first_result.created == 1 && second_result.created == 1 &&
        ReadTarget(dest / L"sample.txt - Concurrent.lnk") == source.wstring() &&
        ReadTarget(dest / L"sample.txt - Concurrent (2).lnk") == source.wstring(),
        "concurrent drops reserve distinct names without overwriting another shortcut");

    const auto long_dir = dest / std::wstring(100, L'a') / std::wstring(100, L'b');
    const auto native_long_dir = fs::path(L"\\\\?\\" + long_dir.wstring());
    fs::create_directories(native_long_dir);
    result = CreateDropShortcuts({source.wstring()}, native_long_dir.wstring(), L"%s - Long");
    if (result.created != 1 || result.failed != 0)
        std::cout << "Long destination HRESULT: 0x" << std::hex << static_cast<unsigned long>(result.error) << std::dec << '\n';
    Check(result.created == 1 && result.failed == 0 &&
        ReadStreamTarget(native_long_dir / L"sample.txt - Long.lnk") == source.wstring(),
        "extended destination longer than MAX_PATH persists a valid Shell shortcut stream");
    const auto long_source = native_long_dir / L"long-source.txt";
    { std::ofstream stream(long_source); stream << "long path source"; }
    result = CreateDropShortcuts({long_source.wstring()}, dest.wstring(), L"%s - LongSource");
    const auto long_source_link = dest / L"long-source.txt - LongSource.lnk";
    const bool long_source_created = result.created == 1 && result.failed == 0 &&
        ReadTarget(long_source_link) == pulse::path::StripExtendedPathPrefix(long_source.wstring());
    const bool long_source_rejected = result.created == 0 && result.failed == 1 && FAILED(result.error) &&
        result.failed_source == long_source.wstring() && !fs::exists(long_source_link);
    Check((long_source_created || long_source_rejected) && fs::file_size(long_source) == 16,
        "long source either persists correctly or reports Shell rejection without a broken link");
    if (long_source_rejected)
        std::cout << "[INFO] Shell rejects long target with HRESULT 0x" << std::hex
            << static_cast<unsigned long>(result.error) << std::dec << '\n';
    std::cout << "Failures: " << failures << '\n';
    return failures ? 1 : 0;
}
