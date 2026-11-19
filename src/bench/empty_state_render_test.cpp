// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "../ui/ui_renderer_internal.h"
#include "../app/app_internal.h"
#include "../common/windows_compat.h"
#include "../common/resource_ids.h"
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string_view>
#include <vector>

namespace pulse::ui {
struct EmptyStateRenderTest {
    static void SetMotion(MainRenderer& renderer, bool enabled) { renderer.motion_enabled_ = enabled; }
    static float MeasureButtonWidth(MainRenderer& renderer, std::wstring_view text) {
        return renderer.painter_.MeasureButtonWidth(text);
    }
    static bool FilterArtLoaded(MainRenderer& renderer, bool invalid) {
        const auto it = renderer.fluent_svgs_.find(invalid ? IDR_INVALID_FILTER_SVG : IDR_NO_FILTER_RESULTS_SVG);
        if (it == renderer.fluent_svgs_.end() || !it->second.get()) return false;
        const auto size = it->second->GetViewportSize();
        return size.width == 320.0f && size.height == 240.0f;
    }
};
}

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

int CheckPickerIllustrations() {
    const HWND window = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"STATIC",
        L"Picker illustration fixture", WS_POPUP, -30000, -30000, 640, 480,
        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    int failures = 0;
    {
        Compositor compositor;
        if (!window || !compositor.Init(window, true)) {
            failures = 1;
        } else {
            compositor.Resize(640, 480);
            fluent::Painter painter(&compositor);
            const auto rows = D2D1::RectF(0, 0, 640, 480);
            for (const bool filtered : {false, true}) {
                const auto size = filtered ? D2D1::SizeF(320, 240) : D2D1::SizeF(512, 360);
                const auto layout = MakePaneEmptyLayout(rows, 1.0f, false, size.width / size.height, false);
                auto* dc = compositor.Dc();
                dc->BeginDraw();
                dc->Clear(D2D1::ColorF(D2D1::ColorF::White));
                const bool drawn = painter.BeginFrame(Theme{}) && painter.DrawSvgResource(
                    filtered ? IDR_NO_FILTER_RESULTS_SVG : IDR_EMPTY_FOLDER_SVG, layout.art, nullptr, size, 0.68f);
                std::vector<unsigned char> pixels;
                const bool rendered = SUCCEEDED(dc->EndDraw()) && drawn && ReadPixels(compositor, layout.art, pixels);
                size_t visible = 0;
                for (size_t i = 0; i + 3 < pixels.size(); i += 4) {
                    if (pixels[i] < 245 || pixels[i + 1] < 245 || pixels[i + 2] < 245) ++visible;
                }
                const bool passed = rendered && visible > pixels.size() / 40 &&
                    layout.title.top >= layout.art.bottom && layout.title.bottom <= rows.bottom;
                std::printf("[%s] picker %s illustration renders above its label\n", passed ? "PASS" : "FAIL",
                    filtered ? "filter" : "empty folder");
                failures += !passed;
            }
        }
    }
    if (window) DestroyWindow(window);
    return failures ? 1 : 0;
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
    EmptyStateRenderTest::SetMotion(s.renderer, false);
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
        const auto art = MakePaneEmptyLayout(list, 1, vm.pane.listing_failed || vm.pane.can_create).art;
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
        !tab.snapshot && tab.listing_failed && !tab.banner_title.empty(),
        "real directory worker publishes the missing-path error");
    const auto failed_vm = BuildVm(s, false);
    const auto failed_list = s.renderer.PaneListRect(failed_vm.pane,
        s.renderer.ContentRect(static_cast<float>(s.compositor.Width()), static_cast<float>(s.compositor.Height())));
    const auto retry = MakePaneEmptyLayout(failed_list, 1, true).action;
    const auto hit = s.renderer.HitTest(failed_vm,
        D2D1::RectF(0, 0, static_cast<float>(s.compositor.Width()), static_cast<float>(s.compositor.Height())),
        (retry.left + retry.right) * 0.5f, (retry.top + retry.bottom) * 0.5f);
    check(failed_vm.pane.listing_failed && failed_vm.pane.banner_kind == 3 && !failed_vm.pane.can_create &&
        hit.region == HitTestResult::PaneEmptyRetry && hit.pane_index == 0,
        "a real failed listing offers retry instead of creating a folder");
    check(!s.watches.Armed(address), "missing directory has no armed filesystem watcher");
    std::vector<unsigned char> baseline, pending;
    check(draw(baseline, L"missing-settled.png"), "capture the settled error-page illustration");
    for (int retry_index = 0; retry_index < 3; ++retry_index) {
        s.last_unc_poll = 0;
        DrainDirNotifies(s);
        check(tab.pending_generation != 0 && !tab.loading && !tab.banner_title.empty(),
            "unwatched-directory polling retries while preserving the completed error presentation");
        check(draw(pending, retry_index == 0 ? L"missing-retrying.png" : nullptr) && pending == baseline,
            "actual error-page illustration stays identical during the pending retry");
        check(apply_result(tab.pending_generation) && !tab.loading && !tab.pending_generation &&
            !tab.snapshot && !tab.banner_title.empty(),
            "a failed retry settles back to the same error page");
    }
    HandleLButtonDown(&s, s.hwnd, WM_LBUTTONDOWN, 0,
        MAKELPARAM(static_cast<int>((retry.left + retry.right) * 0.5f),
                   static_cast<int>((retry.top + retry.bottom) * 0.5f)));
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
        !tab.listing_failed && tab.banner_title.empty() && BuildVm(s, false).pane.can_create,
        "automatic polling recovers a newly available directory and clears the stale error");
    auto& layout = *s.window_tabs.Active();
    auto right = std::make_unique<app::Pane>();
    right->NewTab(address);
    right->view.SetSnapshot(tab.snapshot);
    right->view.loading = false;
    app::Pane* const left_pane = s.pane;
    app::Pane* const right_pane = right.get();
    layout.panes.push_back(std::move(right));
    layout.layout = app::LayoutPreset::TwoVertical;
    app::RebuildLayoutRoot(layout);
    tab.filter_text = L"untouched-left";
    for (const bool invalid : {false, true}) {
        FocusPane(s, left_pane);
        right_pane->view.filter_text = invalid ? L"size:invalid" : L"no-match";
        right_pane->view.scroll_y = 25;
        right_pane->view.scroll_x = 10;
        const auto filtered_vm = BuildVm(s, false);
        check(filtered_vm.pane_slots.size() == 2, "filtered fixture exposes both split panes");
        if (filtered_vm.pane_slots.size() != 2) break;
        const auto& slot = filtered_vm.pane_slots[1];
        const auto action = MakePaneEmptyLayout(s.renderer.PaneListRect(slot.pane, slot.rect), 1, true).action;
        const int x = static_cast<int>((action.left + action.right) * 0.5f);
        const int y = static_cast<int>((action.top + action.bottom) * 0.5f);
        const auto clear_hit = s.renderer.HitTest(filtered_vm,
            D2D1::RectF(0, 0, static_cast<float>(s.compositor.Width()), static_cast<float>(s.compositor.Height())),
            static_cast<float>(x), static_cast<float>(y));
        check(slot.pane.EntryCount() == 0 && slot.pane.filter_error.empty() != invalid &&
            clear_hit.region == HitTestResult::FilterClear && clear_hit.pane_index == 1,
            "valid and invalid empty filters expose clear in the owning split pane");
        HandleLButtonDown(&s, s.hwnd, WM_LBUTTONDOWN, 0, MAKELPARAM(x, y));
        const auto cleared = BuildVm(s, false);
        check(s.pane == right_pane && right_pane->view.filter_text.empty() &&
            right_pane->view.scroll_x == 0 && right_pane->view.scroll_y == 0 &&
            cleared.pane.EntryCount() == 1 && tab.filter_text == L"untouched-left",
            "clear-filter click focuses its pane, restores entries and leaves the other filter intact");
    }
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

int main(int argc, char* argv[]) {
    using namespace pulse::ui;
    const bool filter_only = argc > 1 && std::string_view(argv[1]) == "--filter-only";
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    OleInitialize(nullptr);
    pulse::compat::EnableDpiAwareness();
    pulse::l10n::Initialize(GetModuleHandleW(nullptr), L"en-US");
    if (argc > 1 && std::string_view(argv[1]) == "--picker-art-only") {
        const int result = CheckPickerIllustrations();
        OleUninitialize();
        return result;
    }
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
        for (const float scale : {1.0f, 1.5f}) for (const bool dark : {false, true}) {
            compositor.RecreateTextFormats(scale);
            compositor.Resize(static_cast<int>(1000 * scale), static_cast<int>(720 * scale));
            const auto rect = D2D1::RectF(0, 0, 1000 * scale, 720 * scale);
            for (const bool motion : {false, true}) for (int scenario = 0; scenario < 7; ++scenario) {
                if (filter_only && scenario < 5) continue;
                // A static scene starts without outgoing commands from a previous navigation.
                MainRenderer renderer;
                renderer.SetCompositor(&compositor);
                renderer.SetDetailsPanelVisible(false);
                renderer.SetScale(scale);
                EmptyStateRenderTest::SetMotion(renderer, motion);
                WindowViewModel vm;
                vm.dark = dark;
                vm.window_title = L"Pulse";
                vm.tabs.push_back({L"Empty state", true});
                vm.pane.path = L"C:\\empty-state-fixture";
                vm.pane.header_text = L"Empty state";
                vm.pane.is_file_system = scenario <= 1 || scenario >= 5;
                vm.pane.can_create = scenario == 0;
                vm.pane.is_recent = scenario == 2;
                vm.pane.is_starred = scenario == 3;
                if (scenario == 1) {
                    vm.pane.listing_failed = true;
                    vm.pane.banner_kind = 3;
                    vm.pane.banner_title = L"Cannot open";
                    vm.pane.banner_message = L"The system cannot find the path specified.";
                }
                if (scenario >= 5) vm.pane.filter_text = scenario == 5 ? L"no-match" : L"size:invalid";
                if (scenario == 6) vm.pane.filter_error = L"Invalid filter: size:invalid";
                const auto list = renderer.PaneListRect(vm.pane,
                    renderer.ContentRect(rect.right, rect.bottom));
                const auto art = MakePaneEmptyLayout(list, scale, vm.pane.can_create ||
                    vm.pane.listing_failed || !vm.pane.filter_text.empty(),
                    scenario == 4 ? 320.0f / 240.0f : 512.0f / 360.0f).art;
                if (scenario <= 1 || scenario >= 5) {
                    const auto action = MakePaneEmptyLayout(list, scale, true).action;
                    const auto hit = renderer.HitTest(vm, rect,
                        (action.left + action.right) * 0.5f, (action.top + action.bottom) * 0.5f);
                    check(hit.region == (scenario == 0 ? HitTestResult::PaneEmptyNewFolder :
                        scenario == 1 ? HitTestResult::PaneEmptyRetry : HitTestResult::FilterClear) &&
                        hit.pane_index == 0 && std::abs(action.bottom - action.top - 32 * scale) < 0.01f,
                        "empty-state action has the expected operation and 32 DIP height");
                }
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
                        const auto name = std::to_wstring(scenario) + (motion ? L"-motion" : L"-static") + (dark ? L"-dark-" : L"-light-") +
                            std::to_wstring(static_cast<int>(scale * 100)) + L"-" +
                            std::to_wstring(frame) + L".png";
                        if (!compositor.SaveSnapshot((output / name).c_str())) ++draw_failures;
                    }
                    compositor.Present();
                }
                std::printf("[FRAMES] scenario=%d motion=%d dark=%d scale=%.1f changed=%d max_changed_bytes=%zu\n",
                    scenario, motion, dark, scale, changed_frames, max_changed_bytes);
                size_t visible_pixels = 0;
                for (size_t i = 4; i + 3 < baseline.size(); i += 4)
                    visible_pixels += baseline[i] != baseline[0] || baseline[i + 1] != baseline[1] ||
                                      baseline[i + 2] != baseline[2];
                check(visible_pixels > baseline.size() / 80,
                    "illustration contains visible pixels rather than a stable blank region");
                check(draw_failures == 0, "32 consecutive frames render and read back successfully");
                check(changed_frames == 0, "static empty-state pixels remain identical across presented frames");
                if (scenario >= 5) {
                    check(EmptyStateRenderTest::FilterArtLoaded(renderer, scenario == 6),
                        "filter empty state draws the embedded illustration rather than a fallback glyph");
                    const auto& label = pulse::l10n::Get(pulse::l10n::StringId::ClearFilter);
                    const auto action = MakePaneEmptyLayout(list, scale, true).action;
                    check(!label.empty(), "clear-filter action has a localized label");
                    check(EmptyStateRenderTest::MeasureButtonWidth(renderer, label) <= action.right - action.left,
                        "clear-filter label and padding fit inside the action button");
                }
                renderer.SetCompositor(nullptr);
            }
        }
    }
    DestroyWindow(window);
    if (!filter_only) CheckFailedRefreshPresentation(check, output);
    SetEnvironmentVariableW(L"PULSE_TEST_HIDDEN_SHOT", nullptr);
    OleUninitialize();
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
