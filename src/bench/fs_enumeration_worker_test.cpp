// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "../app/app_worker.h"
#include "worker_projection_cases.h"
#include "worker_net_cache_cases.h"
#include <filesystem>
#include <fstream>
#include <iostream>

namespace pulse::app {
std::wstring FindGitRoot(const std::wstring&) { return {}; }
}

namespace pulse::fs {
std::wstring NetCacheTestDirectory() {
    if (bench::worker_net_cache::cache_access) bench::worker_net_cache::cache_access();
    return bench::worker_net_cache::directory;
}
}

int main(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--net-cache") return RunWorkerNetCacheCases();
    if (argc == 2 && std::string_view(argv[1]) == "--projection") return RunWorkerProjectionCases();
    using namespace pulse::app;
    int failures = 0;
    auto check = [&](bool ok, const char* label) {
        std::cout << (ok ? "[PASS] " : "[FAIL] ") << label << '\n'; failures += !ok;
    };
    const auto root = std::filesystem::current_path() / L"bench_data" /
        (L"worker-cancel-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    std::filesystem::create_directories(root);
    constexpr size_t count = 1200;
    for (size_t i = 0; i < count; ++i) {
        std::ofstream file(root / (std::to_wstring(i) + std::wstring(100, L'x') + L".bin"), std::ios::binary);
        file << "data";
    }
    WorkItem directory{root.wstring(), L"directory", 1, pulse::ui::SortColumn::Name, pulse::ui::SortDirection::Asc};
    size_t checks = 0;
    auto stale = WorkerCancellationTestProbe::Process(directory, 700, &checks);
    check(stale.cancelled && !stale.snapshot && !stale.error && checks >= 700 && checks <= 701,
          "different-path navigation Cancel interrupts started directory worker before completion");
    auto full = WorkerCancellationTestProbe::Process(directory, 0, &checks);
    check(!full.cancelled && !full.error && full.snapshot && full.snapshot->size() == count,
          "current generation returns complete sorted directory snapshot");

    WorkItem missing{(root / L"missing" / L"child").wstring(), L"missing", 4,
                     pulse::ui::SortColumn::Name, pulse::ui::SortDirection::Asc};
    auto error_result = WorkerCancellationTestProbe::Process(missing, 0, &checks);
    check(error_result.error && !error_result.cancelled && !error_result.snapshot &&
          error_result.error_code == ERROR_PATH_NOT_FOUND,
          "directory failure preserves Win32 path-not-found code and has no snapshot");
    check(full.error_code == ERROR_SUCCESS && stale.error_code == ERROR_SUCCESS,
          "successful and cooperatively cancelled enumeration do not carry a filesystem error");

    const auto long_dir = root / std::wstring(100, L'a') / std::wstring(100, L'b');
    std::filesystem::create_directories(pulse::fs::NormalizePath(long_dir.wstring()));
    const auto long_file = pulse::fs::NormalizePath((long_dir / L"long.bin").wstring());
    { std::ofstream file(std::filesystem::path(long_file), std::ios::binary); file << "long"; }
    WorkItem paths{L"pulse:cancel-test", L"paths", 2, pulse::ui::SortColumn::Name, pulse::ui::SortDirection::Asc};
    paths.load_paths = true;
    paths.preserve_order = true;
    paths.paths.assign(1000, long_file);
    auto cancelled_paths = WorkerCancellationTestProbe::Process(paths, 17, &checks);
    check(cancelled_paths.cancelled && !cancelled_paths.snapshot && checks == 17,
          "LoadPaths checks generation within the first 128 long-path items");
    auto full_paths = WorkerCancellationTestProbe::Process(paths, 0, &checks);
    check(long_file.size() > MAX_PATH && !full_paths.cancelled && full_paths.snapshot &&
          full_paths.snapshot->size() == 1000 && full_paths.snapshot->front().size == 4,
          "normal long-path LoadPaths preserves complete results and metadata");

    WorkResult result;
    result.path = L"\\\\pulse-cancellation-fixture\\share";
    result.generation = 3;
    result.snapshot = std::make_shared<std::vector<pulse::fs::DirEntry>>(1);
    size_t callbacks = 0, writes = 0;
    WorkerCancellationTestProbe::Publish(result, true, false, &callbacks, &writes);
    check(!callbacks && !writes, "stale worker result reaches neither callback nor UNC cache");
    result.cancelled = true;
    WorkerCancellationTestProbe::Publish(result, false, false, &callbacks, &writes);
    check(!callbacks && !writes, "cancelled worker result reaches neither callback nor UNC cache");
    result.cancelled = false;
    WorkerCancellationTestProbe::Publish(result, false, true, &callbacks, &writes);
    check(callbacks == 1 && !writes, "replacement requested by callback suppresses old UNC cache write");
    callbacks = 0;
    WorkerCancellationTestProbe::Publish(result, false, false, &callbacks, &writes);
    check(callbacks == 1 && writes == 1, "current complete result publishes callback and cache once");
    check(WorkerCancellationTestProbe::CancelPreservesOtherRequests(),
          "Cancel matches literal path and generation, preserving empty-path/index-ID and newer-pane requests");
    std::error_code error;
    std::filesystem::remove_all(std::filesystem::path(pulse::fs::NormalizePath(root.wstring())), error);
    check(!error, "isolated worker fixture cleaned; synthetic UNC cache never accessed");
    return failures ? 1 : 0;
}
