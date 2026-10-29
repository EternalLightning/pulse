#include "../app/folder_sizes.h"
#include "../fs/fs_enum.h"
#include "../common/json_utils.h"
#include "../common/utf8_file.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <condition_variable>
#include <mutex>
#include <thread>

int main() {
    using namespace pulse::app;
    namespace fs = std::filesystem;
    const auto root = fs::current_path() / L"bench_data" /
        (L"folder-size-scan-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    fs::create_directories(root / L"nested" / L"child"); fs::create_directory(root / L"empty");
    auto write = [](const fs::path& path, size_t bytes) {
        std::ofstream file(fs::path(pulse::fs::NormalizePath(path.wstring())), std::ios::binary | std::ios::trunc);
        file << std::string(bytes, 'x'); return file.good();
    };
    int failures = 0;
    auto check = [&](bool ok, const char* label) {
        std::cout << (ok ? "[PASS] " : "[FAIL] ") << label << std::endl; failures += !ok;
    };
    auto wait = [](auto ready) {
        const auto deadline = GetTickCount64() + 10000;
        while (!ready() && GetTickCount64() < deadline) Sleep(10);
        return ready();
    };
    const auto nested = (root / L"nested").wstring(), empty = (root / L"empty").wstring(), missing = (root / L"missing").wstring();
    check(write(root / L"nested" / L"a.bin", 1234) && write(root / L"nested" / L"child" / L"b.bin", 5678), "isolated size fixture writes");
    {
        FolderSizes sizes; sizes.SetIndexEnabled(false); sizes.Sync({{nested}, {empty}, {missing}}, {});
        check(wait([&] { return sizes.Get(nested).state == FolderSizeState::Ready; }) && sizes.Get(nested).bytes == 6912, "recursive scan sums logical bytes");
        check(sizes.Get(nested).counts_valid && sizes.Get(nested).files == 2 && sizes.Get(nested).dirs == 1,
              "the shared scan returns exact file and directory counts with bytes");
        check(wait([&] { return sizes.Get(empty).state == FolderSizeState::Ready; }) && sizes.Get(empty).has_value && sizes.Get(empty).bytes == 0, "empty root remains a known zero");
        check(wait([&] { return sizes.Get(missing).state == FolderSizeState::Unavailable; }) && !sizes.Get(missing).has_value, "missing root is unavailable, never false zero");
#ifdef PULSE_FOLDER_SIZES_TESTING
        const auto sync_revision = sizes.SyncRevisionForTest();
        for (int i = 0; i < 100; ++i) sizes.Sync({{nested}, {empty}, {missing}}, {});
        check(sizes.SyncRevisionForTest() == sync_revision,
            "identical consumer interests do not rebuild the service request maps");
#endif
        write(root / L"nested" / L"child" / L"b.bin", 1000); sizes.Invalidate(nested);
        const auto stale = sizes.Get(nested);
        check(stale.state == FolderSizeState::Updating && stale.has_value && stale.bytes == 6912, "invalidate retains previous total until refresh");
        check(wait([&] { return sizes.Get(nested).state == FolderSizeState::Ready && sizes.Get(nested).bytes == 2234; }), "refreshed total replaces stale value");
        for (int i = 0; i < 50; ++i) sizes.Sync({{i % 2 ? nested : empty}}, {});
        sizes.Sync({{empty}}, {});
        check(wait([&] { return sizes.Get(empty).state == FolderSizeState::Ready; }) && sizes.Get(empty).bytes == 0, "rapid scope switches cannot publish previous directory into new scope");
        sizes.Stop();
    }
    const auto offline = root / L"nested" / L"offline";
    fs::create_directory(offline); write(offline / L"skipped.bin", 9000);
    if (SetFileAttributesW(offline.c_str(), FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_OFFLINE)) {
        FolderSizes sizes; sizes.SetIndexEnabled(false); sizes.Sync({{nested}, {offline.wstring()}}, {});
        check(wait([&] { return sizes.Get(nested).state == FolderSizeState::Partial; }) && sizes.Get(nested).bytes == 2234 && sizes.Get(nested).has_value,
            "offline subtree is skipped, readable total remains partial");
        check(sizes.Get(nested).counts_valid && sizes.Get(nested).files == 2 && sizes.Get(nested).dirs == 2,
              "partial counts include the skipped directory without claiming its contents");
        check(wait([&] { return sizes.Get(offline.wstring()).state == FolderSizeState::Unavailable; }) && !sizes.Get(offline.wstring()).has_value, "offline root does not hydrate or report zero");
        sizes.Stop(); SetFileAttributesW(offline.c_str(), FILE_ATTRIBUTE_DIRECTORY);
    } else std::cout << "[SKIP] offline attribute unavailable, error=" << GetLastError() << '\n';
    const auto link = root / L"nested" / L"linked";
    if (CreateSymbolicLinkW(link.c_str(), offline.c_str(), SYMBOLIC_LINK_FLAG_DIRECTORY | SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE)) {
        FolderSizes sizes; sizes.SetIndexEnabled(false); sizes.Sync({{nested}, {link.wstring()}}, {});
        check(wait([&] { return sizes.Get(nested).state == FolderSizeState::Partial; }) && sizes.Get(nested).bytes == 11234, "reparse child remains a leaf and marks parent partial");
        check(wait([&] { return sizes.Get(link.wstring()).state == FolderSizeState::Unavailable; }), "reparse root remains unavailable");
        sizes.Stop(); RemoveDirectoryW(link.c_str());
    } else std::cout << "[SKIP] symbolic-link creation unavailable, error=" << GetLastError() << '\n';
    const auto deep_root = root / L"deep";
    fs::create_directory(deep_root);
    fs::path deep = deep_root;
    // Frame 128 is readable; its child is not traversed.
    for (int i = 1; i <= 128; ++i) {
        deep /= L"d"; fs::create_directory(pulse::fs::NormalizePath(deep.wstring()));
        if (i == 127) write(deep / L"included.bin", 17);
        if (i == 128) write(deep / L"excluded.bin", 99);
    }
    {
        FolderSizes sizes; sizes.SetIndexEnabled(false); sizes.Sync({{deep_root.wstring()}}, {});
        check(wait([&] { return sizes.Get(deep_root.wstring()).state == FolderSizeState::Partial; }) && sizes.Get(deep_root.wstring()).bytes == 17,
            "128-frame depth limit is preserved on long paths, excluded subtree makes partial");
        sizes.Stop();
    }
    const auto wide = root / L"wide"; fs::create_directory(wide);
    for (int i = 0; i < 1024; ++i) write(wide / (std::to_wstring(i) + L".bin"), 1);
    {
        FolderSizes sizes; sizes.SetIndexEnabled(false); sizes.Sync({{wide.wstring()}}, {});
        const auto started = GetTickCount64(); sizes.Stop();
        check(GetTickCount64() - started < 2000, "stop cancels pending sliced local scan within bound");
    }
#ifdef PULSE_FOLDER_SIZES_TESTING
    {
        std::mutex mutex;
        std::condition_variable changed;
        unsigned entered = 0;
        bool release = false;
        std::vector<std::unique_ptr<FolderSizes>> blocked;
        for (unsigned i = 0; i < 4; ++i) {
            auto sizes = std::make_unique<FolderSizes>();
            sizes->SetIndexEnabled(false);
            sizes->SetScanHookForTest([&](const std::wstring&) {
                std::unique_lock lock(mutex);
                ++entered; changed.notify_all();
                changed.wait(lock, [&] { return release; });
            });
            sizes->Sync({{empty, true, true}}, {});
            blocked.push_back(std::move(sizes));
        }
        bool all_entered = false;
        {
            std::unique_lock lock(mutex);
            all_entered = changed.wait_for(lock, std::chrono::seconds(5), [&] { return entered == 4; });
        }
        check(all_entered, "four real scan workers enter a blocked provider gate");
        const auto started = GetTickCount64();
        blocked.clear();
        check(GetTickCount64() - started < 1500 && FolderSizes::LiveWorkersForTest() == 4,
              "destroying consumers is bounded and retains four physically blocked workers");
        FolderSizes refused; refused.SetIndexEnabled(false); refused.Sync({{empty, true, true}}, {});
        check(refused.Get(empty).state == FolderSizeState::Unavailable && FolderSizes::LiveWorkersForTest() == 4,
              "a fifth scan is refused while retired provider calls occupy all slots");
        refused.Stop();
        { std::lock_guard lock(mutex); release = true; }
        changed.notify_all();
        check(wait([&] { return FolderSizes::LiveWorkersForTest() == 0; }),
              "physical completion releases retired scan slots without touching destroyed consumers");
        FolderSizes recovered; recovered.SetIndexEnabled(false); recovered.Sync({{empty, true, true}}, {});
        check(wait([&] { return recovered.Get(empty).state == FolderSizeState::Ready; }) && recovered.Get(empty).counts_valid,
              "new scan admission recovers after provider workers physically finish");
        recovered.Stop();
    }
#endif
    {
        const auto cache_path = (root / L"cache.json").wstring();
        std::wstring cache = L"{\"folders\":[\"";
        pulse::json::Escape(L"18446744073709551616\t0\t" + empty, cache);
        cache += L"\",\"";
        pulse::json::Escape(L"9\t1\t" + nested, cache);
        cache += L"\"]}";
        check(pulse::WriteUtf8FileAtomic(cache_path, cache), "write isolated legacy folder-size cache");
        FolderSizes sizes; sizes.SetIndexEnabled(false);
        sizes.SetCachePath([cache_path] { return cache_path; });
        sizes.Sync({{empty, false}, {nested, false}}, {});
        check(wait([&] { return sizes.Get(nested).has_value; }) && sizes.Get(nested).bytes == 9 &&
              sizes.Get(nested).incomplete && !sizes.Get(nested).counts_valid,
              "legacy partial byte cache retains its incomplete origin without inventing counts");
        check(!sizes.Get(empty).has_value,
              "overflowed serialized byte count is rejected without wrapping to a cache value");
        sizes.Stop();
    }
    std::error_code cleanup; fs::remove_all(fs::path(pulse::fs::NormalizePath(root.wstring())), cleanup);
    check(!cleanup, "only unique bench_data fixture cleaned");
    std::cout << "[SKIP] live UNC IO and access-denied descendants require host fixtures; index and watch tests intentionally excluded\n";
    return failures ? 1 : 0;
}
