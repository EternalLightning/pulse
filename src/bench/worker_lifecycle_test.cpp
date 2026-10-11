#include "../app/app_worker.h"
#include <algorithm>
#include <cstdio>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <atomic>

namespace {
struct LocalGate {
    std::mutex mutex;
    std::condition_variable cv;
    bool release = false;
    std::atomic<unsigned> entered{0};
};
std::shared_ptr<LocalGate> g_local_gate;
}

namespace pulse::app {
std::wstring FindGitRoot(const std::wstring& path) {
    if (path == L"worker-general-error") throw std::runtime_error("synthetic worker failure");
    if (path.starts_with(L"worker-local-slot-") && g_local_gate) {
        auto gate = g_local_gate;
        ++gate->entered;
        std::unique_lock lock(gate->mutex);
        gate->cv.wait(lock, [&] { return gate->release; });
    }
    return {};
}
}

int main() {
    using pulse::app::WorkerPool;
    struct Gate {
        std::mutex mutex;
        std::condition_variable cv;
        bool release = false;
        std::atomic<unsigned> entered{0};
        std::atomic<unsigned> finished{0};
        std::atomic<unsigned> late{0};
        std::atomic<unsigned> results{0};
        std::atomic<bool> result_error{false};
        std::atomic<uint64_t> last_generation{0};
    };
    auto gate = std::make_shared<Gate>();
    int failed = 0;
    auto check = [&](bool ok, const char* name) {
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
        if (!ok) ++failed;
    };
    auto wait = [](auto predicate) {
        const auto deadline = GetTickCount64() + 2000;
        while (!predicate() && GetTickCount64() < deadline) Sleep(1);
        return predicate();
    };
    const auto root = std::filesystem::current_path() / L"bench_data" /
        (L"worker-lifecycle-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    std::filesystem::create_directories(root);
    {
        auto error_result = std::make_shared<std::atomic<DWORD>>(ERROR_SUCCESS);
        auto error_only = std::make_shared<std::atomic<bool>>(false);
        WorkerPool errors;
        check(errors.Start([error_result, error_only](pulse::app::WorkResult result) {
            *error_only = result.error && !result.cancelled && !result.snapshot;
            *error_result = result.error_code;
        }), "error publication fixture started");
        check(errors.Refresh(L"worker-general-error", pulse::ui::SortColumn::Name,
                             pulse::ui::SortDirection::Asc) &&
              wait([&] { return *error_result != ERROR_SUCCESS; }) &&
              *error_result == ERROR_GEN_FAILURE && *error_only,
              "unknown worker exception publishes generic failure code without a snapshot");
        errors.Stop();
    }
    const unsigned desired = std::min(4u, std::max(2u, std::thread::hardware_concurrency()));
    {
        g_local_gate = std::make_shared<LocalGate>();
        WorkerPool local;
        check(local.Start([](pulse::app::WorkResult) {}), "local concurrency fixture started");
        bool accepted = true;
        for (unsigned i = 0; i < desired; ++i)
            accepted &= local.Refresh(L"worker-local-slot-" + std::to_wstring(i),
                                      pulse::ui::SortColumn::Name, pulse::ui::SortDirection::Asc) != 0;
        check(accepted && wait([&] { return g_local_gate->entered == desired; }),
              "idle provider owners retain the original full local concurrency");
        {
            std::lock_guard lock(g_local_gate->mutex);
            g_local_gate->release = true;
        }
        g_local_gate->cv.notify_all();
        local.Stop();
        g_local_gate.reset();
    }
    unsigned blocked = 0;
    WorkerPool pool;
    while (8 - blocked >= 2) {
        const unsigned owners = std::min(desired, 8 - blocked);
        const bool started = pool.Start([gate](pulse::app::WorkResult result) {
            gate->result_error = result.error || result.cancelled || !result.snapshot ||
                                 result.error_code != ERROR_SUCCESS;
            gate->last_generation = result.generation;
            ++gate->results;
        });
        check(started, "pool reserves local and provider owners within physical budget");
        if (!started) break;
        bool accepted = true;
        for (unsigned i = 0; i < owners; ++i) {
            accepted &= pool.EnqueueIo([gate] {
                ++gate->entered;
                std::unique_lock lock(gate->mutex);
                gate->cv.wait(lock, [&] { return gate->release; });
                ++gate->finished;
            }, [gate] { ++gate->late; });
        }
        check(accepted, "owned background tasks accepted");
        blocked += owners - 1;
        check(wait([&] { return gate->entered == blocked; }),
              "provider owners block while one background task stays queued");
        const unsigned before = gate->results;
        const auto remote_generation = pool.LoadPaths(L"\\\\pulse-owned-blocker\\share", {},
                                                       pulse::ui::SortColumn::Name, pulse::ui::SortDirection::Asc);
        check(remote_generation != 0, "UNC enumeration stays queued behind provider owners");
        const auto generation = pool.Refresh(root.wstring(), pulse::ui::SortColumn::Name,
                                             pulse::ui::SortDirection::Asc);
        check(generation && wait([&] { return gate->results == before + 1; }) && !gate->result_error,
              "local directory completes while every provider owner is blocked");
        check(gate->entered == blocked, "local owner never picks the queued provider task");
        const auto start = GetTickCount64();
        pool.Stop();
        check(GetTickCount64() - start < 1000, "stop is bounded with uncancellable provider tasks");
        check(!pool.EnqueueIo([] {}), "stopped pool rejects background submissions");
    }
    check(blocked == 7 && gate->entered == 7, "retired blocked owners remain charged to physical limit");
    check(!pool.Start([](pulse::app::WorkResult) {}), "start rejects a lone remaining physical slot");
    const auto before_refresh = pool.ResultWatermark();
    check(!pool.Refresh(root.wstring(), pulse::ui::SortColumn::Name, pulse::ui::SortDirection::Asc) &&
          pool.ResultWatermark() == before_refresh + 1 && pool.ResultWatermark() > gate->last_generation,
          "rejected Refresh advances watermark beyond previously published results");
    const auto before_paths = pool.ResultWatermark();
    check(!pool.LoadPaths(L"pulse:rejected", {}, pulse::ui::SortColumn::Name, pulse::ui::SortDirection::Asc) &&
          pool.ResultWatermark() == before_paths + 1 && pool.ResultWatermark() > gate->last_generation,
          "rejected LoadPaths advances watermark beyond previously published results");
    check(!pool.EnqueueIo([] {}), "failed start rejects provider submissions");
    {
        std::lock_guard lock(gate->mutex);
        gate->release = true;
    }
    gate->cv.notify_all();
    check(wait([&] { return gate->finished == blocked; }) && gate->late == 0,
          "retired tasks finish without late UI completion or queued task execution");
    bool recovered = false;
    wait([&] {
        if (!recovered) recovered = pool.Start([](pulse::app::WorkResult) {});
        return recovered;
    });
    check(recovered, "new start recovers only after actual retired owners return");
    pool.Stop();
    std::error_code error;
    std::filesystem::remove_all(root, error);
    check(!error, "isolated local fixture cleaned");
    return failed ? 1 : 0;
}
