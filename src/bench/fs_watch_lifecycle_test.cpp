#include "../fs/fs_watch.h"
#include <windows.h>
#include <atomic>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace {
using Watch = pulse::fs::DirWatch;

struct Block {
    HANDLE release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::atomic<uint32_t> entered{0};
    ~Block() { if (release) CloseHandle(release); }
};

struct Fixture {
    std::wstring path;
    bool valid = false;
    Fixture() {
        const DWORD needed = GetCurrentDirectoryW(0, nullptr);
        if (!needed) {
            std::printf("[ERROR] GetCurrentDirectoryW error=%lu\n", GetLastError());
            return;
        }
        std::wstring current(needed, L'\0');
        const DWORD chars = GetCurrentDirectoryW(needed, current.data());
        if (!chars || chars >= needed) return;
        current.resize(chars);
        const std::wstring root = current + L"\\bench_data";
        if (!CreateDirectoryW(root.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
            std::wprintf(L"[ERROR] fixture root path=%ls error=%lu\n", root.c_str(), GetLastError());
            return;
        }
        path = root + L"\\_watch_lifecycle_" + std::to_wstring(GetCurrentProcessId()) +
            L"_" + std::to_wstring(GetTickCount64());
        valid = CreateDirectoryW(path.c_str(), nullptr) != FALSE;
        if (!valid) std::wprintf(L"[ERROR] CreateDirectoryW path=%ls error=%lu\n", path.c_str(), GetLastError());
    }
    ~Fixture() {
        if (!valid) return;
        DeleteFileW((path + L"\\a.txt").c_str());
        DeleteFileW((path + L"\\b.txt").c_str());
        RemoveDirectoryW(path.c_str());
    }
};

template<class Predicate>
bool Await(Predicate predicate, DWORD budget = 5000) {
    const auto deadline = GetTickCount64() + budget;
    while (!predicate() && GetTickCount64() < deadline) Sleep(1);
    return predicate();
}

bool Check(bool condition, const char* name) {
    std::printf("[%s] %s\n", condition ? "PASS" : "FAIL", name);
    return condition;
}

bool BlockedPoint(Watch::TestPoint point) {
    Fixture fixture;
    if (!Check(fixture.valid, "create isolated local fixture")) return false;
    auto block = std::make_shared<Block>();
    auto callbacks = std::make_shared<std::atomic<uint32_t>>(0);
    Watch::SetIoHookForTest([block, point](Watch::TestPoint reached) {
        if (reached != point) return;
        block->entered.fetch_add(1);
        WaitForSingleObject(block->release, INFINITE);
    });
    auto watch = std::make_unique<Watch>();
    bool ok = Check(watch->Start(fixture.path, [callbacks](bool, auto) {
        callbacks->fetch_add(1);
    }), "start blocked-point worker");
    if (point == Watch::TestPoint::CancelCompletion) {
        ok &= Check(Await([&] { return watch->Armed(); }), "directory opens before IO cancellation");
        // Armed is published at handle open; allow the pending read to be issued.
        Sleep(30);
    } else {
        ok &= Check(Await([&] { return block->entered.load() != 0; }), "worker enters injected blocking filesystem point");
    }
    const auto before = std::chrono::steady_clock::now();
    watch->Stop();
    watch.reset();
    const double elapsed = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - before).count();
    ok &= Check(elapsed < 100.0, "Stop and destruction do not wait for filesystem IO");
    if (point == Watch::TestPoint::CancelCompletion)
        ok &= Check(Await([&] { return block->entered.load() != 0; }), "worker enters cancellation completion only in background");
    const auto retained = Watch::Resources();
    ok &= Check(retained.live == 1 && retained.retired == 1,
                "outstanding worker storage stays retained, not falsely reported freed");
    SetEvent(block->release);
    ok &= Check(Await([] { return Watch::Resources().live == 0; }), "late IO return reclaims actual worker and handles");
    ok &= Check(callbacks->load() == 0, "no late callbacks after Stop or destruction");
    std::printf("[INFO] stop %.2f ms at point %d\n", elapsed, static_cast<int>(point));
    Watch::SetIoHookForTest({});
    return ok;
}

bool GlobalCap(bool permanent) {
    auto block = std::make_shared<Block>();
    Watch::SetIoHookForTest([block](Watch::TestPoint point) {
        if (point != Watch::TestPoint::OpenDirectory) return;
        block->entered.fetch_add(1);
        WaitForSingleObject(block->release, INFINITE);
    });
    const auto limit = Watch::Resources().limit;
    bool ok = true;
    double max_stop = 0.0;
    for (uint32_t i = 0; i < limit; ++i) {
        Watch watch;
        ok &= watch.Start(L"C:\\pulse-never-opened-life", [](bool, auto) {});
        ok &= Await([&] { return block->entered.load() >= i + 1; });
        const auto before = std::chrono::steady_clock::now();
        watch.Stop();
        max_stop = (std::max)(max_stop, std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - before).count());
    }
    bool rejected = true;
    for (int i = 0; i < 24; ++i) {
        Watch watch;
        rejected &= !watch.Start(L"C:\\pulse-cap-denied", [](bool, auto) {});
    }
    const auto usage = Watch::Resources();
    ok &= Check(rejected && usage.live == limit && usage.retired == limit && block->entered.load() == limit,
                "global live plus retired cap cannot be bypassed with new watch instances");
    ok &= Check(max_stop < 100.0, "repeated switches keep bounded Stop latency");
    std::printf("[INFO] filesystem workers live=%u retired=%u hard cap=%u max stop %.2f ms\n",
                usage.live, usage.retired, usage.limit, max_stop);
    if (permanent) {
        std::printf("[INFO] permanently blocked %u worker states/handles remain until process exit; not zero leakage\n", usage.live);
    } else {
        SetEvent(block->release);
        ok &= Check(Await([] { return Watch::Resources().live == 0; }), "all controlled late opens release their slots");
    }
    Watch::SetIoHookForTest({});
    return ok;
}

bool Normal() {
    Fixture fixture;
    if (!Check(fixture.valid, "create isolated normal-watch fixture")) return false;
    std::mutex mutex;
    std::vector<pulse::fs::DirNotifyEvent> events;
    Watch watch;
    bool ok = Check(watch.Start(fixture.path, [&](bool, auto batch) {
        std::lock_guard lock(mutex);
        for (auto& event : batch) events.push_back(std::move(event));
    }), "start local directory watch");
    ok &= Check(Await([&] { return watch.Armed(); }), "local watch arms");
    Sleep(30);
    const HANDLE file = CreateFileW((fixture.path + L"\\a.txt").c_str(), GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, CREATE_NEW, 0, nullptr);
    ok &= Check(file != INVALID_HANDLE_VALUE, "create isolated change file");
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    ok &= Check(Await([&] {
        std::lock_guard lock(mutex);
        for (const auto& event : events) if (event.action == FILE_ACTION_ADDED && event.name == L"a.txt") return true;
        return false;
    }), "normal added-file callback arrives");
    ok &= Check(MoveFileW((fixture.path + L"\\a.txt").c_str(), (fixture.path + L"\\b.txt").c_str()) != FALSE,
                "rename isolated change file");
    ok &= Check(Await([&] {
        std::lock_guard lock(mutex);
        for (const auto& event : events) if (event.action == FILE_ACTION_RENAMED_NEW_NAME &&
            event.name == L"b.txt" && event.old_name == L"a.txt") return true;
        return false;
    }), "normal rename pairing remains functional");
    watch.Stop();
    ok &= Check(!watch.Armed() && Await([] { return Watch::Resources().live == 0; }), "normal cancellation drains and releases IO state");
    return ok;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) {
        std::printf("usage: pulse_fs_watch_lifecycle_test --open|--probe|--cancel|--cap|--permanent|--normal\n");
        return 2;
    }
    bool ok = false;
    if (wcscmp(argv[1], L"--open") == 0) ok = BlockedPoint(Watch::TestPoint::OpenDirectory);
    else if (wcscmp(argv[1], L"--probe") == 0) ok = BlockedPoint(Watch::TestPoint::ProbePath);
    else if (wcscmp(argv[1], L"--cancel") == 0) ok = BlockedPoint(Watch::TestPoint::CancelCompletion);
    else if (wcscmp(argv[1], L"--cap") == 0) ok = GlobalCap(false);
    else if (wcscmp(argv[1], L"--permanent") == 0) ok = GlobalCap(true);
    else if (wcscmp(argv[1], L"--normal") == 0) ok = Normal();
    else return 2;
    return ok ? 0 : 1;
}
