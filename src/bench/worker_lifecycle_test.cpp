#include "../app/app_worker.h"
#include "../app/tag_discovery_cache.h"
#include <cstdio>
#include <condition_variable>
#include <mutex>
#include <atomic>

// This target exercises cancellation mailboxes without filesystem/provider calls.
namespace pulse::app {
std::wstring FindGitRoot(const std::wstring&) { return {}; }
}

int main() {
    using pulse::app::WorkerPool;
    struct Gate {
        std::mutex mutex;
        std::condition_variable cv;
        bool release = false;
        std::atomic<unsigned> entered{0};
        std::atomic<unsigned> finished{0};
    };
    auto gate = std::make_shared<Gate>();
    std::atomic<unsigned> late{0};
    int failed = 0;
    auto check = [&](bool ok, const char* name) {
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
        if (!ok) ++failed;
    };
    {
        WorkerPool pool;
        pool.Start([](pulse::app::WorkResult) {});
        for (unsigned i = 0; i < 4; ++i) {
            pool.EnqueueIo([gate] {
                ++gate->entered;
                std::unique_lock lock(gate->mutex);
                gate->cv.wait(lock, [&] { return gate->release; });
                ++gate->finished;
            }, [&late] { ++late; });
        }
        const auto deadline = GetTickCount64() + 2000;
        while (gate->entered.load() != 4 && GetTickCount64() < deadline) Sleep(1);
        check(gate->entered == 4, "four owned background tasks entered");
        const auto start = GetTickCount64();
        pool.Stop();
        check(GetTickCount64() - start < 1000, "stop is bounded with uncancellable tasks");
        check(!pool.EnqueueIo([] {}), "stopped pool rejects submissions");
    }
    {
        std::lock_guard lock(gate->mutex);
        gate->release = true;
    }
    gate->cv.notify_all();
    const auto end = GetTickCount64() + 2000;
    while (gate->finished.load() != 4 && GetTickCount64() < end) Sleep(1);
    check(gate->finished == 4 && late == 0, "retired tasks finish without late UI completion");
    return failed ? 1 : 0;
}
