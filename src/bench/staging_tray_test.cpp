#include "../app/app_model.h"
#include "../app/app_worker.h"
#include "../app/tray_metadata.h"
#include "../common/localization.h"
#include <condition_variable>
#include <cstdio>
#include <mutex>

namespace pulse::app {
std::wstring GetPulseDataDir() { return {}; }
}

int main() {
    using namespace pulse;
    using app::TrayMetadataState;
    l10n::Initialize(GetModuleHandleW(nullptr), L"en-US");
    int failures = 0;
    const auto check = [&](bool passed, const char* label) {
        std::printf("[%s] %s\n", passed ? "PASS" : "FAIL", label);
        failures += !passed;
    };
    wchar_t cwd[MAX_PATH]{};
    GetCurrentDirectoryW(MAX_PATH, cwd);
    const auto root = std::wstring(cwd) + L"\\bench_data\\staging-metadata-" +
        std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
    const auto file_path = root + L"\\five.txt";
    const auto folder_path = root + L"\\folder";
    const auto missing_path = root + L"\\absent.txt";
    const bool created = CreateDirectoryW(root.c_str(), nullptr) != FALSE;
    HANDLE file = created ? CreateFileW(file_path.c_str(), GENERIC_WRITE, 0, nullptr,
        CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr) : INVALID_HANDLE_VALUE;
    DWORD written = 0;
    const bool file_ready = file != INVALID_HANDLE_VALUE &&
        WriteFile(file, "facts", 5, &written, nullptr) && written == 5;
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    const bool folder_ready = created && CreateDirectoryW(folder_path.c_str(), nullptr);
    check(file_ready && folder_ready, "create isolated staging metadata fixture");
    if (file_ready && folder_ready) {
        app::StagingTray tray;
        app::WorkerPool worker;
        app::TrayMetadataWorkerState state;
        worker.Start([](app::WorkResult) {});
        std::mutex gate_mutex;
        std::condition_variable gate_changed;
        bool release = false;
        unsigned entered = 0;
        for (unsigned i = 0; i < 4; ++i) {
            worker.EnqueueIo([&] {
                std::unique_lock lock(gate_mutex);
                ++entered; gate_changed.notify_all();
                gate_changed.wait(lock, [&] { return release; });
            });
        }
        {
            std::unique_lock lock(gate_mutex);
            check(gate_changed.wait_for(lock, std::chrono::seconds(5), [&] { return entered >= 2; }),
                "existing I/O workers reach the metadata gate");
        }
        tray.Collect({file_path, folder_path + L"\\", missing_path, file_path}, true);
        const auto& collected = tray.batches().front();
        check(collected.items.size() == 3 && collected.move_intent && collected.total_size == 0 &&
              collected.items[0].size == 0 && !collected.items[1].is_dir &&
              collected.items[2].exists && collected.items[2].metadata_state == TrayMetadataState::Pending,
              "collect publishes deduplicated path intent immediately without filesystem facts");
        tray.Collect({folder_path, file_path}, false);
        check(tray.batches().size() == 1, "duplicate collection retains existing batch intent");
        app::PumpTrayMetadata(tray, worker, state);
        check(collected.items[0].metadata_state == TrayMetadataState::Resolving &&
              collected.total_size == 0 && state.mailbox->completed.empty(),
              "blocked workers leave metadata pending while the UI pump returns");
        {
            std::lock_guard lock(gate_mutex);
            release = true;
        }
        gate_changed.notify_all();
        const auto wait = [&] {
            const auto deadline = GetTickCount64() + 5000;
            do {
                {
                    std::lock_guard lock(state.mailbox->mutex);
                    if (!state.mailbox->completed.empty()) return true;
                }
                Sleep(5);
            } while (GetTickCount64() < deadline);
            return false;
        };
        const bool completed = wait();
        check(collected.items[0].metadata_state == TrayMetadataState::Resolving && collected.total_size == 0,
              "worker completion alone does not mutate tray or batch totals");
        check(completed && app::PumpTrayMetadata(tray, worker, state) && collected.total_size == 5 &&
              collected.items[0].size == 5 && collected.items[0].metadata_state == TrayMetadataState::Ready &&
              collected.items[1].is_dir && !collected.items[2].exists &&
              collected.items[2].metadata_state == TrayMetadataState::Missing,
              "UI applies resolved file size, directory attributes and confirmed absence");
        tray.RemoveItem(0, 0);
        check(tray.batches().front().total_size == 0, "removal updates resolved batch totals");
        worker.Stop();

        const auto ready = [](const app::TrayMetadataRequest& request, uint64_t size = 7) {
            return app::TrayMetadataResult{request, TrayMetadataState::Ready, FILE_ATTRIBUTE_NORMAL, size};
        };
        for (unsigned change = 0; change < 6; ++change) {
            tray.Clear();
            tray.Collect({file_path}, false);
            std::wstring saved;
            tray.ToJson(saved);
            const auto old = tray.BeginMetadataQueries().front();
            if (change == 0) {
                tray.Clear(); tray.Collect({file_path}, true);
            } else if (change == 1) {
                tray.RemoveItem(0, 0); tray.Collect({file_path}, false);
            } else if (change == 2) {
                tray.RemoveBatch(0); tray.Collect({file_path}, false);
            } else if (change == 3) {
                tray.FromJson(saved);
            } else if (change == 4) {
                app::StagingTray restored = tray;
                tray = restored;
            } else {
                app::StagingTray restored = tray;
                tray = std::move(restored);
            }
            const auto replacement = tray.BeginMetadataQueries().front();
            check(old.ticket.expired() && !tray.ApplyMetadataResults({ready(old, 99)}) &&
                  tray.batches().front().total_size == 0 &&
                  tray.ApplyMetadataResults({ready(replacement)}) && tray.batches().front().total_size == 7,
                  change == 0 ? "clear and recollect invalidate the old item ticket" :
                  change == 1 ? "remove item and recollect reject old facts for the same path" :
                  change == 2 ? "remove batch and recollect reject old facts for the same path" :
                  change == 3 ? "session restore replaces pending validation tickets without stat" :
                  change == 4 ? "copy restoration obtains independent validation tickets" :
                                "move restoration cannot inherit an outstanding validation request");
        }
        tray.Clear(); tray.Collect({file_path, missing_path}, false);
        const auto requests = tray.BeginMetadataQueries();
        tray.RemoveItem(0, 0);
        check(tray.ApplyMetadataResults({ready(requests[0], 99), ready(requests[1], 3)}) &&
              tray.batches().front().items.size() == 1 && tray.batches().front().total_size == 3,
              "item index changes do not redirect removed item's facts onto the surviving path");
        tray.Clear(); tray.Collect({file_path}, false);
        const auto inaccessible = tray.BeginMetadataQueries().front();
        check(tray.ApplyMetadataResults({{inaccessible}}) && tray.batches().front().items[0].exists &&
              tray.batches().front().items[0].metadata_state == TrayMetadataState::Unavailable,
              "inconclusive queries retain intent and report unavailable rather than missing");
        tray.Clear(); tray.Collect({root + L"\\invalid*name"}, false);
        std::vector<app::TrayMetadataResult> invalid_facts;
        for (const auto& request : tray.BeginMetadataQueries()) invalid_facts.push_back({request});
        app::ResolveTrayMetadata(invalid_facts);
        check(invalid_facts.front().state == TrayMetadataState::Unavailable &&
              tray.ApplyMetadataResults(invalid_facts) && tray.batches().front().items[0].exists,
              "an invalid-name query is inconclusive rather than confirmed absence");
        std::wstring saved;
        tray.ToJson(saved);
        check(!tray.FromJson(L"[{\"items\":42}]") && tray.batches().front().items[0].exists,
              "invalid restore leaves existing staging intent intact");
        check(tray.FromJson(saved) && tray.batches().front().total_size == 0 &&
              tray.batches().front().items[0].metadata_state == TrayMetadataState::Pending,
              "valid restore deserializes intent without querying filesystem metadata");
        tray.Clear(); tray.Collect({file_path}, false);
        app::PumpTrayMetadata(tray, worker, state);
        check(tray.batches().front().items[0].metadata_state == TrayMetadataState::Pending,
              "worker enqueue rejection resets pending validation for a later retry");
        worker.Start([](app::WorkResult) {});
        app::PumpTrayMetadata(tray, worker, state);
        check(wait() && app::PumpTrayMetadata(tray, worker, state) && tray.batches().front().total_size == 5,
              "pending metadata completes after the worker becomes available");
        worker.Stop();
        tray.Clear(); tray.Collect({file_path}, false);
        const auto cancelled = tray.BeginMetadataQueries().front();
        tray.RemoveDeleted({root});
        std::vector<app::TrayMetadataResult> cancelled_results{{cancelled}};
        app::ResolveTrayMetadata(cancelled_results);
        check(cancelled.ticket.expired() && tray.batches().empty() &&
              cancelled_results.front().state == TrayMetadataState::Unavailable,
              "removed ancestor cancels pending items before filesystem resolution");
    }
    check(!folder_ready || RemoveDirectoryW(folder_path.c_str()), "clean isolated staging directory");
    check(file == INVALID_HANDLE_VALUE || DeleteFileW(file_path.c_str()), "clean isolated staging file");
    check(!created || RemoveDirectoryW(root.c_str()), "clean isolated staging fixture root");
    return failures ? 1 : 0;
}
