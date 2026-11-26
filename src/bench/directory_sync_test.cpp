// SPDX-License-Identifier: Apache-2.0
#include "../fs/directory_sync.h"
#include "../fs/fs_enum.h"
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace {
using namespace pulse::fs;
namespace fixture = std::filesystem;
int failures = 0;

void Check(bool condition, const char* name) {
    std::printf("[%s] %s\n", condition ? "PASS" : "FAIL", name);
    if (!condition) ++failures;
}

struct Scratch {
    fixture::path root;
    Scratch() : root(fixture::absolute(fixture::path(L"bench_data") /
        (L"directory-sync-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64())))) {
        if (!fixture::create_directories(root)) throw std::runtime_error("sync fixture already exists");
    }
    ~Scratch() {
        std::error_code error;
        fixture::remove_all(root, error);
        Check(!error, "isolated fixture directory removed");
    }
};

void File(const fixture::path& path, const char* content, uint64_t ticks) {
    std::ofstream(path, std::ios::binary) << content;
    const HANDLE file = CreateFileW(path.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) throw std::runtime_error("fixture file could not be opened");
    const FILETIME modified{static_cast<DWORD>(ticks), static_cast<DWORD>(ticks >> 32)};
    const bool changed = SetFileTime(file, nullptr, nullptr, &modified) != FALSE;
    CloseHandle(file);
    if (!changed) throw std::runtime_error("fixture file time could not be set");
}

const DirectorySyncItem* Find(const DirectorySyncPlan& plan, std::wstring_view relative) {
    const auto found = std::find_if(plan.items.begin(), plan.items.end(), [&](const auto& item) {
        return item.relative == relative;
    });
    return found != plan.items.end() ? &*found : nullptr;
}

bool Is(const DirectorySyncPlan& plan, std::wstring_view relative, SyncChange change, size_t from) {
    const auto* item = Find(plan, relative);
    return item && item->change == change && item->from == from;
}
}

int main() {
    using M = DirectorySyncMode;
    try {
        Check(MapLinkedDirectory({L"\\\\?\\C:\\", L"\\\\?\\D:\\root"}, 0, L"\\\\?\\c:\\sub") ==
            L"\\\\?\\D:\\root\\sub", "navigation maps a child of a drive root");
        Check(MapLinkedDirectory({L"D:\\left", L"E:\\right"}, 1, L"e:\\RIGHT\\") ==
            L"D:\\left", "navigation returns to the other root");
        Check(MapLinkedDirectory({L"D:\\left", L"E:\\right"}, 0, L"D:\\leftover\\sub").empty(),
            "navigation does not map a same-prefix sibling");
        Check(MapLinkedDirectory({L"D:\\left", L"E:\\right"}, 0, L"D:\\left\\..\\sub").empty(),
            "navigation does not map dot segments");
        Check(MapLinkedDirectory({L"\\\\?\\UNC\\srv\\share\\", L"E:\\right"}, 0,
            L"\\\\?\\UNC\\SRV\\share\\sub") == L"E:\\right\\sub", "navigation maps a share root");

        Scratch scratch;
        const auto left = scratch.root / L"left", right = scratch.root / L"right";
        fixture::create_directories(left / L"paired");
        fixture::create_directories(left / L"new folder" / L"empty child");
        fixture::create_directories(left / L"clash");
        fixture::create_directories(right / L"paired");
        constexpr uint64_t older = 133000000000000000ull, newer = older + 100000000ull;
        File(left / L"paired" / L"left-only.txt", "left only", older);
        File(right / L"paired" / L"right-only.txt", "right only", older);
        File(left / L"paired" / L"equal.txt", "same", older);
        File(right / L"paired" / L"equal.txt", "same", older);
        File(left / L"left-newer.txt", "new left", newer);
        File(right / L"left-newer.txt", "old right", older);
        File(left / L"right-newer.txt", "old left", older);
        File(right / L"right-newer.txt", "new right", newer);
        File(left / L"ambiguous.txt", "one", newer);
        File(right / L"ambiguous.txt", "longer", newer);
        File(left / L"new folder" / L"child.txt", "copied with folder", older);
        File(right / L"clash", "occupied", older);
        File(left / L"Upper.txt", "new", newer);
        File(right / L"upper.txt", "older", older);
        File(left / L"EqualCase.txt", "same", older);
        File(right / L"equalcase.txt", "same", older);
        const std::array<std::wstring, 2> roots{left.wstring(), right.wstring()};

        const auto forward = CompareDirectories(roots, M::LeftToRight);
        Check(forward.error == ERROR_SUCCESS && !forward.overlap, "distinct roots compare successfully");
        Check(Is(forward, L"paired\\left-only.txt", SyncChange::AddFile, 0) &&
            !Find(forward, L"paired\\right-only.txt"), "one-way sync retains destination-only files");
        Check(!Find(forward, L"paired\\equal.txt"), "matching size and time do not queue a copy");
        Check(Is(forward, L"right-newer.txt", SyncChange::ReplaceFile, 0),
            "one-way sync includes a newer destination in conflict review");
        const auto* added_folder = Find(forward, L"new folder");
        Check(added_folder && added_folder->change == SyncChange::AddFolder &&
            added_folder->descendants.size() == 2 && !Find(forward, L"new folder\\child.txt"),
            "a missing directory is one copy including files and empty folders");
        Check(Is(forward, L"clash", SyncChange::TypeConflict, 0) &&
            Is(forward, L"Upper.txt", SyncChange::NameConflict, 0),
            "different kinds and case-only file names are skipped");
        Check(forward == CompareDirectories(roots, M::LeftToRight), "the same tree produces the same plan");

        const auto reverse = CompareDirectories(roots, M::RightToLeft);
        Check(Is(reverse, L"paired\\right-only.txt", SyncChange::AddFile, 1) &&
            !Find(reverse, L"paired\\left-only.txt"), "reverse sync swaps the copy side");
        const auto both = CompareDirectories(roots, M::NewerBothWays);
        Check(Is(forward, L"EqualCase.txt", SyncChange::NameConflict, 0) &&
            Is(reverse, L"EqualCase.txt", SyncChange::NameConflict, 0) &&
            Is(both, L"EqualCase.txt", SyncChange::NameConflict, 0),
            "case-only file names report a conflict even with matching metadata in both directions");
        Check(Is(both, L"left-newer.txt", SyncChange::ReplaceFile, 0) &&
            Is(both, L"right-newer.txt", SyncChange::ReplaceFile, 1),
            "bidirectional sync chooses the newer source for each file");
        Check(Is(both, L"paired\\left-only.txt", SyncChange::AddFile, 0) &&
            Is(both, L"paired\\right-only.txt", SyncChange::AddFile, 1) &&
            Is(both, L"ambiguous.txt", SyncChange::SameTimeConflict, 0),
            "bidirectional sync fills both sides and skips ambiguous timestamps");

        const auto child = left / L"new folder" / L"child.txt";
        File(child, "copied with folder and extended", older);
        const auto extended = CompareDirectories(roots, M::LeftToRight);
        Check(extended.error == ERROR_SUCCESS && forward != extended,
            "size change to an existing descendant invalidates a reviewed folder copy");
        File(child, "copied with folder and extended", newer);
        const auto retimed = CompareDirectories(roots, M::LeftToRight);
        Check(retimed.error == ERROR_SUCCESS && extended != retimed,
            "time change to an existing descendant invalidates a reviewed folder copy");

        File(right / L"left-newer.txt", "changed destination", newer + 100000000ull);
        Check(retimed != CompareDirectories(roots, M::LeftToRight), "destination changes invalidate a reviewed plan");
        Check(CompareDirectories({left.wstring(), NormalizePath(left.wstring())}, M::LeftToRight).overlap,
            "a pair resolving to the same directory is rejected");
        Check(CompareDirectories({(left / L"paired").wstring(), left.wstring()}, M::RightToLeft).overlap,
            "nested roots are rejected in either order");
        Check(CompareDirectories({left.wstring(), (right / L"absent").wstring()}, M::LeftToRight).error != ERROR_SUCCESS,
            "missing root causes comparison to fail");
        int cancellation_checks = 0;
        const auto cancelled = CompareDirectories(roots, M::LeftToRight, [&] { return ++cancellation_checks > 9; });
        Check(cancelled.error == ERROR_CANCELLED && cancelled.items.empty(), "cancellation discards partial results");

        const auto link = left / L"new folder" / L"external-link";
        if (CreateSymbolicLinkW(link.c_str(), right.c_str(),
                SYMBOLIC_LINK_FLAG_DIRECTORY | SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE)) {
            Check(Is(CompareDirectories(roots, M::LeftToRight), L"new folder", SyncChange::Link, 0),
                "a new directory containing a reparse point is skipped");
            Check(RemoveDirectoryW(link.c_str()) != FALSE, "test directory link removed");
        } else {
            std::printf("[SKIP] link fixture unavailable (%lu)\n", GetLastError());
        }
    } catch (const std::exception& error) {
        std::printf("[FAIL] %s\n", error.what());
        ++failures;
    }
    return failures == 0 ? 0 : 1;
}
