// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "../ui/ui_renderer_internal.h"
#include "../app/app_internal.h"
#include "../common/windows_compat.h"
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <vector>

namespace {
using namespace pulse::ui;

bool ReadPixels(Compositor& compositor, const D2D1_RECT_F& bounds,
                std::vector<unsigned char>& pixels) {
    auto* dc = compositor.Dc();
    ComPtr<ID2D1Image> image;
    ComPtr<ID2D1Bitmap1> target, readback;
    dc->GetTarget(&image);
    if (!image.get() || FAILED(image->QueryInterface(IID_PPV_ARGS(&target)))) return false;
    const auto props = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        target->GetPixelFormat(), 96, 96);
    if (FAILED(dc->CreateBitmap(target->GetPixelSize(), nullptr, 0, props, &readback)) ||
        FAILED(readback->CopyFromBitmap(nullptr, target.get(), nullptr))) return false;
    D2D1_MAPPED_RECT mapped{};
    if (FAILED(readback->Map(D2D1_MAP_OPTIONS_READ, &mapped))) return false;
    const auto size = target->GetPixelSize();
    const int left = std::clamp(static_cast<int>(std::floor(bounds.left)), 0, static_cast<int>(size.width));
    const int right = std::clamp(static_cast<int>(std::ceil(bounds.right)), left, static_cast<int>(size.width));
    const int top = std::clamp(static_cast<int>(std::floor(bounds.top)), 0, static_cast<int>(size.height));
    const int bottom = std::clamp(static_cast<int>(std::ceil(bounds.bottom)), top, static_cast<int>(size.height));
    pixels.clear();
    pixels.reserve(static_cast<size_t>(right - left) * (bottom - top) * 4);
    for (int y = top; y < bottom; ++y) {
        const auto* row = mapped.bits + static_cast<size_t>(y) * mapped.pitch + left * 4;
        pixels.insert(pixels.end(), row, row + (right - left) * 4);
    }
    readback->Unmap();
    return !pixels.empty();
}

void CheckFailedRefreshPresentation(const std::function<void(bool, const char*)>& check,
                                    const std::filesystem::path& output) {
    using namespace pulse;
    auto owned = std::make_unique<AppState>();
    auto& s = *owned;
    s.isolatedTest = true;
    s.appPrefs.persist = s.places.persist = s.ctxMenuPrefs.persist = s.searchHistory.persist = false;
    s.shot.active = true;
    s.showDetailsPanel = false;
    s.hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"STATIC",
        L"Failed listing refresh fixture", WS_POPUP, -30000, -30000, 1000, 720,
        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    check(s.hwnd && s.compositor.Init(s.hwnd), "failed-listing fixture initializes");
    if (!s.compositor.Dc()) {
        if (s.hwnd) DestroyWindow(s.hwnd);
        s.hwnd = nullptr;
        return;
    }
    s.renderer.SetCompositor(&s.compositor);
    s.renderer.SetDetailsPanelVisible(false);
    s.renderer.SetScale(1.0f);
    s.compositor.RecreateTextFormats(1.0f);
    s.window_tabs.NewTab(L"");
    s.pane = s.window_tabs.Active()->FocusedPane();
    auto& tab = *ActiveTab(s);
    const auto missing = output / (L"missing-" + std::to_wstring(GetCurrentProcessId()) +
        L"-" + std::to_wstring(GetTickCount64()));
    const auto address = fs::NormalizePath(missing.wstring());
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<app::WorkResult> results;
    s.worker.Start([&](app::WorkResult result) {
        std::lock_guard lock(mutex);
        results.push_back(std::move(result));
        changed.notify_all();
    });
    auto apply_result = [&](uint64_t generation) {
        std::vector<app::WorkResult> ready;
        {
            std::unique_lock lock(mutex);
            if (!changed.wait_for(lock, std::chrono::seconds(5), [&] {
                return std::any_of(results.begin(), results.end(), [&](const auto& result) {
                    return result.final && result.generation == generation;
                });
            })) return false;
            ready.swap(results);
        }
        for (auto& result : ready) ApplyWorkerResult(s, result);
        return true;
    };
    auto draw = [&](std::vector<unsigned char>& pixels, const wchar_t* filename) {
        const auto vm = BuildVm(s, false);
        const auto rect = D2D1::RectF(0, 0, static_cast<float>(s.compositor.Width()),
            static_cast<float>(s.compositor.Height()));
        const auto list = s.renderer.PaneListRect(vm.pane,
            s.renderer.ContentRect(rect.right, rect.bottom));
        const auto art = MakePaneEmptyLayout(list, 1, vm.pane.can_create).art;
        auto* dc = s.compositor.Dc();
        dc->BeginDraw();
        dc->Clear(D2D1::ColorF(0, 0));
        s.renderer.Render(vm, rect, MakeTheme(false, HexColor(0x0078D4)));
        return SUCCEEDED(dc->EndDraw()) && ReadPixels(s.compositor, art, pixels) &&
            (!filename || s.compositor.SaveSnapshot((output / filename).c_str()));
    };
    StartLoadingPath(s, tab, address);
    check(tab.loading && tab.pending_generation != 0,
        "first navigation to a missing directory retains its loading state");
    check(apply_result(tab.pending_generation) && !tab.loading && !tab.pending_generation &&
        !tab.snapshot && !tab.banner_title.empty(),
        "real directory worker publishes the missing-path error");
    check(!s.watches.Armed(address), "missing directory has no armed filesystem watcher");
    std::vector<unsigned char> baseline, pending;
    check(draw(baseline, L"missing-settled.png"), "capture the settled error-page illustration");
    for (int retry = 0; retry < 3; ++retry) {
        s.last_unc_poll = 0;
        DrainDirNotifies(s);
        check(tab.pending_generation != 0 && !tab.loading && !tab.banner_title.empty(),
            "unwatched-directory polling retries while preserving the completed error presentation");
        check(draw(pending, retry == 0 ? L"missing-retrying.png" : nullptr) && pending == baseline,
            "actual error-page illustration stays identical during the pending retry");
        check(apply_result(tab.pending_generation) && !tab.loading && !tab.pending_generation &&
            !tab.snapshot && !tab.banner_title.empty(),
            "a failed retry settles back to the same error page");
    }
    RefreshActiveTab(s, RefreshReason::Explicit);
    check(tab.pending_generation != 0 && tab.loading,
        "an explicit refresh still shows loading while retrying a failed directory");
    check(apply_result(tab.pending_generation) && !tab.loading && !tab.pending_generation &&
        draw(pending, nullptr) && pending == baseline,
        "an explicit failed retry restores the completed error-page illustration");
    RefreshActiveTab(s, RefreshReason::OperationCompleted);
    check(tab.pending_generation != 0 && !tab.loading &&
        draw(pending, nullptr) && pending == baseline,
        "operation-completion refresh preserves the already completed error page");
    check(apply_result(tab.pending_generation) && !tab.loading && !tab.pending_generation,
        "operation-completion retry settles without leaving a loading state");
    check(CreateDirectoryW(missing.c_str(), nullptr) != FALSE,
        "create the previously missing directory to exercise recovery");
    const auto recovered_file = missing / L"recovered.txt";
    HANDLE file = CreateFileW(recovered_file.c_str(), GENERIC_WRITE, 0, nullptr,
        CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    check(file != INVALID_HANDLE_VALUE, "create a real entry for recovery verification");
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    s.last_unc_poll = 0;
    DrainDirNotifies(s);
    check(tab.pending_generation != 0 && apply_result(tab.pending_generation) && !tab.loading &&
        tab.snapshot && tab.EntryCount() == 1 && tab.EntryAt(0).name == L"recovered.txt" &&
        tab.banner_title.empty(),
        "automatic polling recovers a newly available directory and clears the stale error");
    s.worker.Stop();
    s.watches.Stop();
    check(DeleteFileW(recovered_file.c_str()) && RemoveDirectoryW(missing.c_str()),
        "isolated recovery files are removed");
    s.renderer.SetCompositor(nullptr);
    s.compositor.Shutdown();
    DestroyWindow(s.hwnd);
    s.hwnd = nullptr;
}
}

int main() {
    using namespace pulse::ui;
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    OleInitialize(nullptr);
    pulse::compat::EnableDpiAwareness();
    pulse::l10n::Initialize(GetModuleHandleW(nullptr), L"en-US");
    SetEnvironmentVariableW(L"PULSE_TEST_HIDDEN_SHOT", L"1");
    const auto output = std::filesystem::absolute(L"bench_data/empty-state-render");
    std::filesystem::create_directories(output);
    int checks = 0, failures = 0;
    auto check = [&](bool ok, const char* label) {
        ++checks;
        failures += !ok;
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
    };
    HWND window = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"STATIC",
        L"Empty state frame fixture", WS_POPUP, -30000, -30000, 1000, 720,
        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    {
        Compositor compositor;
        check(window && compositor.Init(window), "production compositor initializes");
        if (!compositor.Dc()) return 2;
        MainRenderer renderer;
        renderer.SetCompositor(&compositor);
        renderer.SetDetailsPanelVisible(false);
        for (const float scale : {1.0f, 1.5f}) for (const bool dark : {false, true}) {
            renderer.SetScale(scale);
            compositor.RecreateTextFormats(scale);
            compositor.Resize(static_cast<int>(1000 * scale), static_cast<int>(720 * scale));
            const auto rect = D2D1::RectF(0, 0, 1000 * scale, 720 * scale);
            for (int scenario = 0; scenario < 5; ++scenario) {
                WindowViewModel vm;
                vm.dark = dark;
                vm.window_title = L"Pulse";
                vm.tabs.push_back({L"Empty state", true});
                vm.pane.path = L"C:\\empty-state-fixture";
                vm.pane.header_text = L"Empty state";
                vm.pane.is_file_system = scenario <= 1;
                vm.pane.can_create = scenario == 0;
                vm.pane.is_recent = scenario == 2;
                vm.pane.is_starred = scenario == 3;
                if (scenario == 1) {
                    vm.pane.banner_kind = 3;
                    vm.pane.banner_title = L"Cannot open";
                    vm.pane.banner_message = L"The system cannot find the path specified.";
                }
                const auto list = renderer.PaneListRect(vm.pane,
                    renderer.ContentRect(rect.right, rect.bottom));
                const auto art = MakePaneEmptyLayout(list, scale, vm.pane.can_create,
                    scenario == 4 ? 320.0f / 240.0f : 512.0f / 360.0f).art;
                std::vector<unsigned char> baseline, pixels;
                int changed_frames = 0, draw_failures = 0;
                size_t max_changed_bytes = 0;
                for (int frame = 0; frame < 32; ++frame) {
                    auto* dc = compositor.Dc();
                    dc->BeginDraw();
                    dc->Clear(D2D1::ColorF(0, 0));
                    renderer.Render(vm, rect, MakeTheme(dark, HexColor(0x0078D4)));
                    if (FAILED(dc->EndDraw()) || !ReadPixels(compositor, art, pixels)) {
                        ++draw_failures;
                    } else if (frame == 0) {
                        baseline = pixels;
                    } else {
                        size_t changed = 0;
                        for (size_t i = 0; i < pixels.size() && i < baseline.size(); ++i)
                            changed += pixels[i] != baseline[i];
                        changed_frames += changed != 0;
                        max_changed_bytes = std::max(max_changed_bytes, changed);
                    }
                    if (frame == 0 || frame == 31) {
                        const auto name = std::to_wstring(scenario) + (dark ? L"-dark-" : L"-light-") +
                            std::to_wstring(static_cast<int>(scale * 100)) + L"-" +
                            std::to_wstring(frame) + L".png";
                        if (!compositor.SaveSnapshot((output / name).c_str())) ++draw_failures;
                    }
                    compositor.Present();
                }
                std::printf("[FRAMES] scenario=%d dark=%d scale=%.1f changed=%d max_changed_bytes=%zu\n",
                    scenario, dark, scale, changed_frames, max_changed_bytes);
                size_t visible_pixels = 0;
                for (size_t i = 4; i + 3 < baseline.size(); i += 4)
                    visible_pixels += baseline[i] != baseline[0] || baseline[i + 1] != baseline[1] ||
                                      baseline[i + 2] != baseline[2];
                check(visible_pixels > baseline.size() / 80,
                    "illustration contains visible pixels rather than a stable blank region");
                check(draw_failures == 0, "32 consecutive frames render and read back successfully");
                check(changed_frames == 0, "static empty-state pixels remain identical across presented frames");
            }
        }
        renderer.SetCompositor(nullptr);
    }
    DestroyWindow(window);
    CheckFailedRefreshPresentation(check, output);
    SetEnvironmentVariableW(L"PULSE_TEST_HIDDEN_SHOT", nullptr);
    OleUninitialize();
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
