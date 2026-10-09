#include "../fs/fs_enum.h"
#include "../ipc/shell_client.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <cstring>
#include <type_traits>

namespace {
template<class T> concept PublicRecycleDelete = requires(T& client, const std::vector<std::wstring>& paths) { client.DeleteRecycle(paths); };
template<class T> concept PublicRealDelete = requires(T& client, const std::vector<std::wstring>& paths) { client.RealDelete(paths); };
// MSVC's requires call expression can incorrectly accept private members.
// Taking the member address in SFINAE performs ordinary access checking.
template<class T, class = void> struct PublicAuthorizedDelete : std::false_type {};
template<class T> struct PublicAuthorizedDelete<T, std::void_t<decltype(&T::DeleteAuthorized)>> : std::true_type {};
static_assert(!PublicRecycleDelete<pulse::ipc::ShellClient>);
static_assert(!PublicRealDelete<pulse::ipc::ShellClient>);
static_assert(!PublicAuthorizedDelete<pulse::ipc::ShellClient>::value);
struct PublicDeleteControl { uint32_t DeleteAuthorized(const std::vector<std::wstring>&, uint64_t, bool = false, HWND = nullptr); };
static_assert(PublicAuthorizedDelete<PublicDeleteControl>::value);

int RunCancellationCases() {
    using namespace pulse::fs;
    int failures = 0;
    auto check = [&](bool ok, const char* label) {
        std::cout << (ok ? "[PASS] " : "[FAIL] ") << label << '\n'; failures += !ok;
    };
    const auto root = std::filesystem::current_path() / L"bench_data" /
        (L"fs-cancel-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    std::filesystem::create_directories(root);
    constexpr size_t count = 1800;
    for (size_t i = 0; i < count; ++i) {
        const auto name = std::to_wstring(i) + std::wstring(120, L'x') + L".bin";
        std::ofstream file(root / name, std::ios::binary); file << "data";
    }
    std::vector<DirEntry> snapshot(1);
    check(!EnumerateDirectory((root / L"missing").wstring(), snapshot, [] { return true; }) && snapshot.empty(),
          "snapshot cancellation before open clears output without filesystem error");
    size_t observed = 0;
    const bool complete = EnumerateDirectory(root.wstring(), snapshot, [&] {
        observed = std::max(observed, snapshot.size());
        return observed >= 800;
    });
    check(!complete && observed >= 800 && snapshot.empty(),
          "multi-batch mid-enumeration cancellation discards partial snapshot");
    check(EnumerateDirectory(root.wstring(), snapshot) && snapshot.size() == count,
          "normal multi-batch enumeration returns every entry after cancellation");
    DirectoryEnumerator cursor(root.wstring());
    DirEntry item;
    unsigned calls = 0;
    check(cursor.Next(item, [&] { return ++calls == 2; }) == DirectoryRead::Cancelled,
          "stream checks cancellation immediately after native enumeration call");
    size_t streamed = 0;
    while (cursor.Next(item) == DirectoryRead::Entry) ++streamed;
    check(streamed == count, "stream resume preserves the record fetched during cancellation");
    std::error_code error;
    std::filesystem::remove_all(root, error);
    check(!error, "isolated cancellation fixture cleaned");
    return failures ? 1 : 0;
}
}

int main(int argc, char** argv) {
    using namespace pulse::fs;
    if (argc == 2 && std::strcmp(argv[1], "--cancel-only") == 0) return RunCancellationCases();
    if (argc == 2 && std::strcmp(argv[1], "--paths-only") == 0) {
        int failures = 0;
        auto check = [&](bool ok, const char* label) {
            std::cout << (ok ? "[PASS] " : "[FAIL] ") << label << '\n'; failures += !ok;
        };
        const std::wstring cwd = std::filesystem::current_path().wstring();
        const std::wstring expected = L"\\\\?\\" + cwd + L"\\child";
        check(NormalizePath(cwd + L"\\sub\\..\\child") == expected &&
              NormalizePath(L".\\sub\\..\\child") == expected,
              "ordinary absolute and relative paths resolve dot segments before extension");
        check(NormalizePath(cwd.substr(0, 2) + L".\\sub\\..\\child") == expected,
              "drive-relative path resolves against that drive's current directory");
        check(NormalizePath(L"\\\\server\\share\\sub\\..\\child") == L"\\\\?\\UNC\\server\\share\\child",
              "ordinary UNC path resolves dot segments without accessing the share");
        const std::wstring literal = L"\\\\?\\" + cwd + L"\\sub\\..\\child";
        check(NormalizePath(literal) == literal &&
              NormalizePath(L"\\\\?\\UNC\\server\\share\\sub\\..\\child") ==
                  L"\\\\?\\UNC\\server\\share\\sub\\..\\child",
              "explicit extended paths preserve literal dot segments");
        check(NormalizePath(L"pulse:search/a/../b") == L"pulse:search/a/../b" &&
              NormalizePath(L"\\\\.\\C:") == L"\\\\.\\C:",
              "virtual and explicit device paths retain their namespaces");
        check(NormalizePath(cwd.substr(0, 3)) == L"\\\\?\\" + cwd.substr(0, 3),
              "drive root retains its trailing separator");
        return failures ? 1 : 0;
    }
    const auto root = std::filesystem::current_path() / L"bench_data" /
        (L"fs-enumeration-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    std::filesystem::create_directories(root / L"empty");
    std::filesystem::create_directory(root / L"child");
    { std::ofstream file(root / L"logical.bin", std::ios::binary); file << "1234567"; }
    int failures = 0;
    auto check = [&](bool ok, const char* label) {
        std::cout << (ok ? "[PASS] " : "[FAIL] ") << label << '\n'; failures += !ok;
    };
    auto collect = [](DirectoryEnumerator& cursor) {
        std::map<std::wstring, std::pair<uint64_t, DWORD>> items;
        DirEntry item;
        while (cursor.Next(item) == DirectoryRead::Entry) items[item.name] = {item.size, item.attrs};
        return items;
    };
    DirectoryEnumerator cursor(root.wstring());
    const auto items = collect(cursor);
    check(items.size() == 3 && items.at(L"logical.bin").first == 7 &&
        (items.at(L"child").second & FILE_ATTRIBUTE_DIRECTORY), "stream preserves names, logical sizes and directory attributes, excludes dot entries");
    std::vector<DirEntry> snapshot;
    EnumerateDirectory(root.wstring(), snapshot);
    std::map<std::wstring, std::pair<uint64_t, DWORD>> snap;
    for (const auto& item : snapshot) snap[item.name] = {item.size, item.attrs};
    check(snap == items, "NT-primary snapshot and streaming fallback agree on fixture metadata");
    DirectoryEnumerator empty((root / L"empty").wstring());
    DirEntry item;
    check(empty.Next(item) == DirectoryRead::End && empty.Next(item) == DirectoryRead::End, "empty directory and stable EOF");
    DirectoryEnumerator cancelled((root / L"missing").wstring());
    check(cancelled.Next(item, [] { return true; }) == DirectoryRead::Cancelled, "cancel before open avoids filesystem error");
    DirectoryEnumerator mid(root.wstring());
    check(mid.Next(item) == DirectoryRead::Entry && mid.Next(item, [] { return true; }) == DirectoryRead::Cancelled &&
        mid.Next(item) == DirectoryRead::Entry, "cancellation between entries does not consume the next record");
    DirectoryEnumerator moved(std::move(mid));
    check(!collect(moved).empty(), "move transfers cursor handle ownership and position");
    bool stream_error = false, combined_error = false;
    try { DirectoryEnumerator missing((root / L"missing").wstring()); missing.Next(item); }
    catch (const EnumerationError& error) {
        const std::string message = error.what();
        stream_error = error.Code() == ERROR_PATH_NOT_FOUND && message.find("FindFirstFileExW") != std::string::npos && message.find("missing") != std::string::npos;
        std::cout << "[INFO] " << message << '\n';
    }
    try { EnumerateDirectory((root / L"missing").wstring(), snapshot); }
    catch (const EnumerationError& error) {
        const std::string message = error.what();
        combined_error = message.find("NT primary:") != std::string::npos && message.find("Win32 fallback:") != std::string::npos && error.Code() == ERROR_PATH_NOT_FOUND;
        std::cout << "[INFO] " << message << '\n';
    }
    check(stream_error && combined_error, "errors retain numeric Win32 code, path and both backend failures");
    bool invalid = false;
    try { DirectoryEnumerator no_path(L""); } catch (const EnumerationError& error) { invalid = error.Code() == ERROR_INVALID_NAME; }
    check(invalid, "filesystem cursor does not turn empty path into This PC");
    const auto long_dir = root / std::wstring(100, L'a') / std::wstring(100, L'b') / std::wstring(100, L'c');
    std::filesystem::create_directories(NormalizePath(long_dir.wstring()));
    { std::ofstream file(std::filesystem::path(NormalizePath((long_dir / L"long.bin").wstring())), std::ios::binary); file << "long"; }
    DirectoryEnumerator long_cursor(long_dir.wstring());
    const auto long_items = collect(long_cursor);
    EnumerateDirectory(long_dir.wstring(), snapshot);
    check(long_dir.wstring().size() > MAX_PATH && long_items.size() == 1 && long_items.at(L"long.bin").first == 4 && snapshot.size() == 1,
        "unprefixed long directory works through both public entrances");
    check(NormalizePath(L"\\\\server\\share\\dir") == L"\\\\?\\UNC\\server\\share\\dir" &&
        NormalizePath(L"\\\\?\\UNC\\server\\share\\dir") == L"\\\\?\\UNC\\server\\share\\dir", "UNC normalization retains server and share");
    const auto link = root / L"directory-link";
    if (CreateSymbolicLinkW(link.c_str(), (root / L"child").c_str(), SYMBOLIC_LINK_FLAG_DIRECTORY | SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE)) {
        DirectoryEnumerator linked(root.wstring());
        const auto linked_items = collect(linked);
        check((linked_items.at(L"directory-link").second & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) ==
            (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY), "cursor exposes directory reparse metadata without recursive traversal");
        RemoveDirectoryW(link.c_str());
    } else std::cout << "[SKIP] symbolic-link creation unavailable, error=" << GetLastError() << '\n';
    std::cout << "[SKIP] live UNC share and injected FindNext failure need host fixtures; no user/network files accessed\n";
    std::error_code cleanup;
    std::filesystem::remove_all(std::filesystem::path(NormalizePath(root.wstring())), cleanup);
    check(!cleanup, "only unique bench_data fixture cleaned");
    return failures ? 1 : 0;
}
