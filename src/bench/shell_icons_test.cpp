#include "../ui/shell_icons.h"
#include <shellapi.h>
#include <cstdio>
#include <chrono>
#include <thread>
#include <atomic>
#include <cstring>
#include <memory>
#include <vector>

namespace pulse::ui {
struct ShellIconCacheTestAccess {
    static bool Wait(ShellIconCache& cache) {
        if (!cache.worker_) return false;
        const auto end = GetTickCount64() + 10000;
        while (GetTickCount64() < end) {
            {
                std::lock_guard lock(cache.worker_->mutex);
                if (cache.worker_->queued.empty()) return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return false;
    }
#ifdef PULSE_SHELL_ICONS_TESTING
    struct Block {
        HANDLE release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        std::atomic<uint32_t> entered{0};
        ~Block() { if (release) CloseHandle(release); }
    };
    static bool Lifecycle(bool permanent) {
        auto block = std::make_shared<Block>();
        ShellIconCache::SetQueryHookForTest([block](const std::wstring&) {
            block->entered.fetch_add(1);
            WaitForSingleObject(block->release, INFINITE);
            return 42;
        });
        const auto limit = ShellIconCache::Resources().limit;
        bool ok = true;
        std::vector<std::shared_ptr<ShellIconCache::WorkerState>> retired_states;
        double max_reset = 0.0;
        for (uint32_t i = 0; i < limit; ++i) {
            ShellIconCache cache;
            cache.RequestExact(L"blocked-" + std::to_wstring(i));
            const auto deadline = GetTickCount64() + 3000;
            while (block->entered.load() < i + 1 && GetTickCount64() < deadline) Sleep(1);
            ok &= block->entered.load() == i + 1;
            const auto old = cache.worker_;
            if (old) retired_states.push_back(old);
            const auto before = std::chrono::steady_clock::now();
            cache.Reset();
            max_reset = (std::max)(max_reset, std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - before).count());
            if (old) {
                std::lock_guard lock(old->mutex);
                ok &= old->stop && !old->notify && old->exact_index.empty();
            }
        }
        for (int i = 0; i < 24; ++i) {
            ShellIconCache denied;
            denied.RequestExact(L"denied");
            ok &= !denied.worker_;
        }
        const auto full = ShellIconCache::Resources();
        const bool cap = full.live == limit && full.retired == limit && block->entered.load() == limit;
        std::printf("[%s] global Shell cap includes destroyed cache workers (live=%u retired=%u cap=%u)\n",
                    cap ? "PASS" : "FAIL", full.live, full.retired, full.limit);
        std::printf("[%s] Reset does not wait for Shell (max %.2f ms)\n",
                    max_reset < 100.0 ? "PASS" : "FAIL", max_reset);
        ok &= cap && max_reset < 100.0;
        if (permanent) {
            std::printf("[INFO] permanent injected calls retain %u workers until process exit; not zero leakage\n", full.live);
        } else {
            SetEvent(block->release);
            const auto deadline = GetTickCount64() + 5000;
            while (ShellIconCache::Resources().live && GetTickCount64() < deadline) Sleep(1);
            const bool reaped = ShellIconCache::Resources().live == 0;
            std::printf("[%s] late returning Shell calls release their slots\n", reaped ? "PASS" : "FAIL");
            ok &= reaped;
            bool suppressed = true;
            for (const auto& state : retired_states) {
                std::lock_guard lock(state->mutex);
                suppressed &= state->exact_index.empty() && !state->notify;
            }
            std::printf("[%s] late results cannot populate or notify retired cache state\n", suppressed ? "PASS" : "FAIL");
            ok &= suppressed;
            retired_states.clear();
            ShellIconCache::SetQueryHookForTest([](const std::wstring&) { return 7; });
            ShellIconCache fresh;
            fresh.RequestExact(L"fresh");
            const bool done = Wait(fresh);
            bool resolved = false;
            if (fresh.worker_) {
                std::lock_guard lock(fresh.worker_->mutex);
                resolved = fresh.worker_->exact_index.contains(L"fresh") &&
                    fresh.worker_->exact_index.at(L"fresh") == 7;
            }
            std::printf("[%s] fresh cache resolves after retirement completes\n", done && resolved ? "PASS" : "FAIL");
            ok &= done && resolved;
        }
        ShellIconCache::SetQueryHookForTest({});
        return ok;
    }
#endif
    static bool Run() {
        ShellIconCache cache;
        bool ok = true;
        auto check = [&](bool result, const char* name) {
            std::printf("[%s] %s\n", result ? "PASS" : "FAIL", name);
            ok &= result;
        };
        cache.GenericIndex(L"", true, FILE_ATTRIBUTE_DIRECTORY);
        cache.GenericIndex(L"test.txt", false, FILE_ATTRIBUTE_NORMAL);
        check(cache.worker_ != nullptr, "admit native icon worker");
        if (!cache.worker_) return false;
        check(Wait(cache), "native type queries complete");
        {
            std::lock_guard lock(cache.worker_->mutex);
            check(cache.worker_->generic_index.contains(L"<dir>") && cache.worker_->generic_index.contains(L".txt"),
                  "Windows folder and associated file icons resolve");
        }
        wchar_t windows[MAX_PATH]{};
        GetWindowsDirectoryW(windows, MAX_PATH);
        check(cache.NeedsExactIcon(L"Windows", true, windows), "folders use actual Shell paths");
        // Seed a full cache to exercise eviction without thousands of Shell calls.
        {
            std::lock_guard lock(cache.worker_->mutex);
            for (int i = 0; i < 4096; ++i) {
                const auto key = L"seed-" + std::to_wstring(i);
                cache.worker_->exact_index[key] = 0;
                cache.worker_->last_used[key] = ++cache.worker_->access_clock;
            }
        }
        cache.RequestExact(windows);
        check(Wait(cache), "exact folder query completes");
        SHFILEINFOW info{};
        const bool resolved = SHGetFileInfoW(windows, 0, &info, sizeof(info),
                                            SHGFI_SYSICONINDEX | SHGFI_SMALLICON) != 0;
        {
            std::lock_guard lock(cache.worker_->mutex);
            check(resolved && cache.worker_->exact_index.contains(windows) &&
                  cache.worker_->exact_index.at(windows) == info.iIcon, "index matches native Shell query");
            check(cache.worker_->exact_index.size() == 4096 && !cache.worker_->exact_index.contains(L"seed-0") &&
                  cache.worker_->exact_index.contains(L"seed-1"), "evicts only oldest entry");
        }
        const std::wstring missing = std::wstring(windows) + L"\\pulse-nonexistent-icon-test\\missing.exe";
        cache.RequestExact(missing);
        check(Wait(cache), "missing path query completes");
        {
            std::lock_guard lock(cache.worker_->mutex);
            check(!cache.worker_->exact_index.contains(missing) && cache.worker_->retry_after.contains(missing),
                  "failure is retryable rather than permanently cached");
            cache.worker_->retry_after[windows] = 0;
            cache.worker_->exact_index.erase(windows);
            cache.worker_->last_used.erase(windows);
        }
        cache.RequestExact(windows);
        check(Wait(cache), "expired failure retries");
        {
            std::lock_guard lock(cache.worker_->mutex);
            check(cache.worker_->exact_index.contains(windows) && !cache.worker_->retry_after.contains(windows),
                  "successful retry clears failure state");
        }
        cache.Reset();
        cache.GenericIndex(L"", true, FILE_ATTRIBUTE_DIRECTORY);
        check(Wait(cache), "worker restarts after reset");
        ComPtr<ID3D11Device> d3d;
        ComPtr<IDXGIDevice> dxgi;
        ComPtr<ID2D1Factory1> factory;
        ComPtr<ID2D1Device> device;
        ComPtr<ID2D1DeviceContext> context;
        const bool graphics = SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP,
            nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
            &d3d, nullptr, nullptr)) &&
            SUCCEEDED(d3d->QueryInterface(IID_PPV_ARGS(&dxgi))) &&
            SUCCEEDED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, IID_PPV_ARGS(&factory))) &&
            SUCCEEDED(factory->CreateDevice(dxgi.get(), &device)) &&
            SUCCEEDED(device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &context));
        check(graphics, "create Direct2D software device");
        if (graphics) {
            cache.SetDeviceContext(context.get());
            for (const float size : {16.0f, 24.0f, 32.0f, 48.0f, 96.0f, 256.0f}) {
                auto* bitmap = cache.BitmapFor(L"", L"", true, FILE_ATTRIBUTE_DIRECTORY, size);
                check(bitmap && bitmap->GetPixelSize().width >= 16,
                      "native folder converts to Direct2D bitmap at requested scale");
            }
            cache.SetDeviceContext(nullptr);
            cache.SetDeviceContext(context.get());
            check(cache.BitmapFor(L"", L"", true, FILE_ATTRIBUTE_DIRECTORY, 32) != nullptr,
                  "device recreation preserves native indices and rebuilds bitmap");
            cache.Reset();
        }
        return ok;
    }
};
}

int main(int argc, char** argv) {
    const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool ok = false;
#ifdef PULSE_SHELL_ICONS_TESTING
    if (argc == 2 && (std::strcmp(argv[1], "--lifecycle") == 0 ||
                      std::strcmp(argv[1], "--permanent") == 0)) {
        ok = pulse::ui::ShellIconCacheTestAccess::Lifecycle(std::strcmp(argv[1], "--permanent") == 0);
    } else
#else
    (void)argc;
    (void)argv;
#endif
    {
        ok = pulse::ui::ShellIconCacheTestAccess::Run();
    }
    if (SUCCEEDED(hr)) CoUninitialize();
    return ok ? 0 : 1;
}
