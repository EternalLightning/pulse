// SPDX-License-Identifier: Apache-2.0
#define PULSE_PREVIEW_HANDLER_TESTING 1
// Link this fixture without the separate preview_handler_host.cpp object.
#include "../ui/preview_handler_host.cpp"
#include "../app/app_internal.h"
#include "../common/user_storage.h"
#include <cstdio>
#include <filesystem>

LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);

namespace pulse::ui {
struct PreviewVisibilityTest {
    struct Provider final : IPreviewHandler {
        std::atomic<ULONG> refs{1};
        std::atomic<unsigned> unloads{0};
        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** value) override {
            *value = nullptr;
            if (iid != IID_IUnknown && iid != IID_IPreviewHandler) return E_NOINTERFACE;
            *value = static_cast<IPreviewHandler*>(this); AddRef(); return S_OK;
        }
        ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
        ULONG STDMETHODCALLTYPE Release() override {
            const auto left = --refs; if (!left) delete this; return left;
        }
        HRESULT STDMETHODCALLTYPE SetWindow(HWND, const RECT*) override { return S_OK; }
        HRESULT STDMETHODCALLTYPE SetRect(const RECT*) override { return S_OK; }
        HRESULT STDMETHODCALLTYPE DoPreview() override { return S_OK; }
        HRESULT STDMETHODCALLTYPE Unload() override { ++unloads; return S_OK; }
        HRESULT STDMETHODCALLTYPE SetFocus() override { return S_OK; }
        HRESULT STDMETHODCALLTYPE QueryFocus(HWND*) override { return E_NOTIMPL; }
        HRESULT STDMETHODCALLTYPE TranslateAccelerator(MSG*) override { return S_FALSE; }
    };

    static PreviewHandlerHost& Host(MainRenderer& renderer) { return renderer.preview_handler_; }
    static bool SeedPlayingProvider(MainRenderer& renderer, HWND owner, Provider& provider) {
        auto& host = Host(renderer);
        const uint32_t slot = ReserveApartment();
        if (slot == kApartmentLimit) return false;
        auto worker = std::make_shared<PreviewHandlerHost::WorkerState>();
        worker->open_hook = [] {};
        worker->slot = slot;
        worker->wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!worker->wake) return false;
        host.last_owner_ = owner;
        host.last_enabled_ = true;
        host.last_path_ = L"fixture.pulse-visibility-no-provider";
        host.last_identity_ = host.last_path_ + L"\n1:0:0";
        host.last_bounds_ = {0, 0, 320, 240};
        worker->identity = host.last_identity_;
        worker->command.owner = owner;
        worker->command.enabled = true;
        worker->command.path = host.last_path_;
        worker->command.identity = host.last_identity_;
        worker->command.bounds = host.last_bounds_;
        worker->command_version = 1;
        provider.AddRef();
        worker->handler = &provider;
        auto& pool = Apartments();
        std::lock_guard lock(pool.mutex);
        auto* argument = new std::shared_ptr<PreviewHandlerHost::WorkerState>(worker);
        worker->thread = CreateThread(nullptr, 0, PreviewHandlerHost::WorkerMain, argument, 0, nullptr);
        if (!worker->thread) {
            delete argument; provider.Release(); pool.slots[slot] = {}; return false;
        }
        pool.slots[slot].owner = worker;
        pool.slots[slot].thread = worker->thread;
        host.worker_ = std::move(worker);
        return true;
    }
    static void SyncSameSelection(MainRenderer& renderer, HWND owner) {
        Host(renderer).Sync(owner, D2D1::RectF(0, 0, 320, 240),
            L"fixture.pulse-visibility-no-provider", FILE_ATTRIBUTE_NORMAL, 1, 0, 0,
            true, D2D1::ColorF(0.0f, 0.0f, 0.0f), D2D1::ColorF(1.0f, 1.0f, 1.0f), true, true);
    }
    static uint64_t OpenAttempts(MainRenderer& renderer) {
        return Host(renderer).worker_->opens_started.load();
    }
};
}

namespace {
LRESULT CALLBACK FixtureProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    // Isolate lifecycle dispatch from application startup, painting and user data.
    if (GetWindowLongPtrW(hwnd, GWLP_USERDATA) &&
        (message == WM_SHOWWINDOW || message == WM_ACTIVATE || message == WM_ACTIVATEAPP))
        return WndProc(hwnd, message, wparam, lparam);
    return DefWindowProcW(hwnd, message, wparam, lparam);
}
template<class Predicate> bool Wait(Predicate predicate) {
    const auto deadline = GetTickCount64() + 2000;
    while (!predicate() && GetTickCount64() < deadline) {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&message);
        Sleep(1);
    }
    return predicate();
}
}

int main() {
    using namespace pulse;
    using Test = ui::PreviewVisibilityTest;
    using Host = ui::PreviewHandlerHost;
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (FAILED(OleInitialize(nullptr))) return 2;
    const auto root = std::filesystem::absolute(L"../bench_data/upstream-review-2026-11-17/preview-fixture");
    std::filesystem::create_directories(root);
    storage::OverrideDefaultRootForTesting((root / L"profile").wstring());
    auto state = std::make_unique<AppState>();
    auto& s = *state;
    s.isolatedTest = s.safeMode = true;
    s.ui_schedule.stopped = true;
    s.appPrefs.persist = s.places.persist = s.ctxMenuPrefs.persist = s.searchHistory.persist = false;
    WNDCLASSW window_class{};
    window_class.hInstance = GetModuleHandleW(nullptr);
    window_class.lpfnWndProc = FixtureProc;
    window_class.lpszClassName = L"PulsePreviewVisibilityFixture";
    RegisterClassW(&window_class);
    s.hwnd = CreateWindowExW(WS_EX_NOACTIVATE, window_class.lpszClassName, L"Preview lifecycle fixture",
        WS_OVERLAPPEDWINDOW, -10000, -10000, 640, 480, nullptr, nullptr, window_class.hInstance, nullptr);
    if (!s.hwnd) return 2;
    SetWindowLongPtrW(s.hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&s));
    ShowWindow(s.hwnd, SW_SHOWNOACTIVATE);
    auto* provider = new Test::Provider;
    int failures = 0;
    auto check = [&](bool result, const char* label) {
        std::printf("[%s] %s\n", result ? "PASS" : "FAIL", label);
        failures += !result;
    };
    if (!Test::SeedPlayingProvider(s.renderer, s.hwnd, *provider)) return 2;
    check(Wait([&] { return Test::Host(s.renderer).state() == Host::State::Shown; }),
        "fake media provider is active on production preview STA");
    SendMessageW(s.hwnd, WM_ACTIVATE, WA_INACTIVE, 0);
    SendMessageW(s.hwnd, WM_ACTIVATEAPP, FALSE, 0);
    Sleep(25);
    check(provider->unloads.load() == 0 && Test::Host(s.renderer).state() == Host::State::Shown,
        "switching applications preserves the selected preview provider");
    ShowWindow(s.hwnd, SW_HIDE);
    check(Wait([&] { return provider->unloads.load() == 1; }),
        "hiding the owner unloads the playing provider without another paint");
    check(Test::Host(s.renderer).state() == Host::State::Idle,
        "hidden details preview is idle");
    ShowWindow(s.hwnd, SW_SHOWNOACTIVATE);
    Test::SyncSameSelection(s.renderer, s.hwnd);
    check(Wait([&] { return Test::OpenAttempts(s.renderer) == 1; }),
        "restoring the same selection issues a fresh provider open");
    ShowWindow(s.hwnd, SW_HIDE);
    SetWindowLongPtrW(s.hwnd, GWLP_USERDATA, 0);
    DestroyWindow(s.hwnd);
    state.reset();
    check(Wait([] { return Host::Resources().live == 0; }), "preview STA and fixture release cleanly");
    provider->Release();
    OleUninitialize();
    return failures ? 1 : 0;
}
