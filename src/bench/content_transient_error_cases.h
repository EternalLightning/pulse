// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once

int RunTransientErrorCases() {
    namespace fs = std::filesystem;
    using namespace pulse::index;
    const auto base = fs::current_path() / L"bench_data" /
        (L"transient-errors-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    const auto left = base / L"left", right = base / L"right";
    fs::create_directories(left); fs::create_directories(right);
    Check(WriteBytes((left / L"a.txt").wstring(), {'m','a','r','k'}) &&
        WriteBytes((right / L"b.txt").wstring(), {'m','a','r','k'}), L"create transient error fixtures");
    ContentSearchRequest request; request.needle = L"mark";
    request.root = left.wstring(); request.roots = {left.wstring(), right.wstring()};
    std::atomic<bool> cancel{false}; ContentSearchProgress final;
    std::vector<ContentHit> hits;
    auto run = [&](const ContentSearchEnumerationHooks* hooks = nullptr) {
        hits.clear(); final = {};
        return RunContentSearch(request, cancel, [&](const auto& progress, auto batch) {
            final = progress; hits.insert(hits.end(), batch.begin(), batch.end()); return true;
        }, hooks);
    };
    Check(run() && final.done && !final.error && hits.size() == 2, L"two transient roots override legacy root");
    request.roots.push_back((base / L"missing").wstring());
    Check(!run() && final.done && final.error == ERROR_PATH_NOT_FOUND && hits.size() == 2,
        L"partial roots retain content hits and fail completion");
    request.mode = ContentSearchMode::Duplicates;
    Check(!run() && final.done && final.error == ERROR_PATH_NOT_FOUND && hits.size() == 2,
        L"partial duplicate scan retains confirmed duplicate group");
    request.mode = ContentSearchMode::Content;
    request.roots = {(base / L"missing").wstring()};
    Check(!run() && final.error == ERROR_PATH_NOT_FOUND && hits.empty(), L"all missing roots fail completion");
    request.roots = {left.wstring()};
    ContentSearchEnumerationHooks hooks;
    hooks.next = [](HANDLE find, WIN32_FIND_DATAW& data) {
        if (!wcscmp(data.cFileName, L"a.txt")) { SetLastError(ERROR_READ_FAULT); return false; }
        return FindNextFileW(find, &data) != FALSE;
    };
    Check(!run(&hooks) && final.done && final.error == ERROR_READ_FAULT && hits.size() == 1,
        L"non EOF transient enumeration error retains confirmed hit");
    Check(run() && !final.error && hits.size() == 1, L"normal transient EOF completes");
    hooks.first = [](const auto&, auto&) { SetLastError(ERROR_NO_MORE_FILES); return INVALID_HANDLE_VALUE; };
    Check(run(&hooks) && final.done && !final.error && hits.empty(), L"empty transient directory completes");
    cancel = true;
    Check(!run() && final.error == ERROR_CANCELLED, L"transient cancellation remains distinct");
    if (!failed) fs::remove_all(base);
    wprintf(L"%d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
