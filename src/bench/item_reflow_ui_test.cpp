// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "../ui/ui_renderer.h"
#include "../common/localization.h"
#include "animation_snapshot.h"
#include <cmath>
#include <cstdio>
#include <chrono>
#include <thread>
#include <limits>

namespace pulse::ui {
struct ThumbnailCacheTestAccess {
    static size_t VisibleCount(ThumbnailCache& cache) {
        std::lock_guard lock(cache.mutex_);
        return cache.consumers_[static_cast<size_t>(ThumbnailCache::Consumer::Main)].visible_keys.size();
    }
};
struct ItemReflowUiTest {
    static int RunSearchFilter(const std::filesystem::path& output);
    static int RunBulk(const std::filesystem::path& output);
    static int RunSearchContinuity(const std::filesystem::path& output);
    static int RunViewModes(const std::filesystem::path& output);
    static int RunViewModeFeedback(const std::filesystem::path& output);
    static int RunWheelFeedback(const std::filesystem::path& output);
    static int RunFrameProfile(bool presented);
    static const ViewModeTransition& ViewTransition(const MainRenderer& renderer) {
        return renderer.view_mode_transitions_[0];
    }
    static void DrawSearchFilter(MainRenderer& renderer, const WindowViewModel& vm,
                                 D2D1_RECT_F rect, const Theme& theme, uint64_t now) {
        renderer.motion_enabled_ = true;
        renderer.surface_animating_ = false;
        renderer.settings_switch_now_ = now;
        ++renderer.settings_switch_frame_;
        renderer.UpdateBrushes(theme);
        renderer.painter_.BeginFrame(theme, false);
        ThumbnailCache::FrameScope thumbnails(renderer.thumbnail_cache_);
        renderer.DrawPane(vm, renderer.ContentRect(rect.right, rect.bottom), theme);
    }
    static void Draw(MainRenderer& renderer, const WindowViewModel& vm, D2D1_RECT_F rect,
                     const Theme& theme, uint64_t now, bool animate = true) {
        renderer.motion_enabled_ = animate;
        renderer.surface_animating_ = false;
        renderer.settings_switch_now_ = now;
        ++renderer.settings_switch_frame_;
        renderer.UpdateBrushes(theme);
        renderer.painter_.BeginFrame(theme, false);
        ThumbnailCache::FrameScope thumbnails(renderer.thumbnail_cache_);
        renderer.DrawSinglePane(vm, vm.pane, renderer.ContentRect(rect.right, rect.bottom),
            0, true, false, theme);
    }
    static const ItemReflowSurface& Surface(const MainRenderer& renderer) {
        return renderer.item_reflow_surfaces_[0];
    }
    static size_t VisibleThumbnails(MainRenderer& renderer) {
        return ThumbnailCacheTestAccess::VisibleCount(renderer.thumbnail_cache_);
    }
    static void Reset(MainRenderer& renderer) {
        for (auto& surface : renderer.item_reflow_surfaces_) surface.Reset();
        for (auto& surface : renderer.view_mode_transitions_) surface.Reset();
        for (auto& surface : renderer.pane_surfaces_) surface.Reset();
    }
};
}

#include "search_filter_update_cases.h"
#include "item_reflow_bulk_cases.h"
#include "search_continuity_cases.h"
#include "view_mode_transition_cases.h"
#include "view_mode_transition_feedback_cases.h"
#include "view_mode_wheel_feedback_cases.h"
#include "animation_frame_profile_cases.h"

namespace {
int failures = 0;
void Check(bool condition, const char* message) {
    std::printf("[%s] %s\n", condition ? "PASS" : "FAIL", message);
    if (!condition) ++failures;
}
pulse::ui::ListEntryView Entry(wchar_t name) {
    pulse::ui::ListEntryView entry;
    entry.name = std::wstring(1, name) + L" - Example document.txt";
    entry.path = L"C:\\pulse-reflow-fixture\\" + entry.name;
    entry.size_text = L"4 KB";
    entry.type_text = L"Text document";
    entry.date_text = L"2026-10-30 12:00";
    return entry;
}
void Items(pulse::ui::PaneViewModel& pane, const wchar_t* names) {
    pane.entries.clear();
    for (const auto* name = names; *name; ++name) pane.entries.push_back(Entry(*name));
}

float RowTone(pulse::ui::Compositor& compositor, const pulse::ui::ItemReflowVisual& visual, float scale) {
    using Microsoft::WRL::ComPtr;
    ComPtr<ID2D1Image> image;
    ComPtr<ID2D1Bitmap1> target, sample;
    auto* dc = compositor.Dc();
    dc->GetTarget(image.GetAddressOf());
    const float invalid = std::numeric_limits<float>::quiet_NaN();
    if (!image || FAILED(image.As(&target))) return invalid;
    const UINT x = static_cast<UINT>(std::lround(visual.bounds.left + 350 * scale));
    const UINT y = static_cast<UINT>(std::lround((visual.bounds.top + visual.bounds.bottom) * 0.5f));
    const auto size = target->GetPixelSize();
    if (x >= size.width || y >= size.height) return invalid;
    const auto properties = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW, target->GetPixelFormat());
    const D2D1_RECT_U region{x, y, x + 1, y + 1};
    if (FAILED(dc->CreateBitmap(D2D1::SizeU(1, 1), nullptr, 0, properties, &sample)) ||
        FAILED(sample->CopyFromBitmap(nullptr, target.Get(), &region))) return invalid;
    D2D1_MAPPED_RECT mapped{};
    if (FAILED(sample->Map(D2D1_MAP_OPTIONS_READ, &mapped))) return invalid;
    const float tone = (mapped.bits[0] + mapped.bits[1] + mapped.bits[2]) / 3.0f;
    sample->Unmap();
    return tone;
}

void Profile(pulse::ui::Compositor& compositor, pulse::ui::MainRenderer& renderer, bool zebra) {
    using namespace pulse;
    using Clock = std::chrono::steady_clock;
    renderer.SetScale(1);
    renderer.SetListStyle(true, zebra, false);
    compositor.Resize(1100, 820);
    compositor.RecreateTextFormats(1);
    const auto rect = D2D1::RectF(0, 0, 1100, 820);
    const auto theme = ui::MakeTheme(false, ui::HexColor(0x0078D4));
    for (const auto mode : {ui::ViewMode::Details, ui::ViewMode::List, ui::ViewMode::MediumIcons}) {
        ui::ItemReflowUiTest::Reset(renderer);
        ui::WindowViewModel vm;
        vm.pane.path = L"C:\\pulse-reflow-profile";
        vm.pane.is_file_system = true;
        vm.pane.view_mode = mode;
        for (int i = 0; i < 240; ++i) {
            auto entry = Entry(L'A');
            entry.name = std::to_wstring(i) + L" - Example document.txt";
            entry.path = vm.pane.path + L"\\" + entry.name;
            vm.pane.entries.push_back(std::move(entry));
        }
        std::vector<double> times;
        std::vector<double> moving_times, idle_times;
        for (int frame = -12; frame < 80; ++frame) {
            if (frame >= 0 && frame % 20 == 0) {
                if (frame % 40 == 0) {
                    vm.pane.entries.erase(vm.pane.entries.begin() + 12);
                    vm.pane.entries.erase(vm.pane.entries.begin() + 5);
                } else {
                    auto entry = Entry(static_cast<wchar_t>(L'X' + frame / 20));
                    vm.pane.entries.insert(vm.pane.entries.begin() + 2, entry);
                    entry.name += L" second"; entry.path += L" second";
                    vm.pane.entries.insert(vm.pane.entries.begin() + 9, std::move(entry));
                }
            }
            const auto start = Clock::now();
            const bool was_moving = ui::ItemReflowUiTest::Surface(renderer).Pending();
            auto* dc = compositor.Dc();
            dc->BeginDraw(); dc->Clear(theme.bg);
            renderer.Render(vm, rect, theme);
            const HRESULT result = dc->EndDraw();
            const auto end = Clock::now();
            if (FAILED(result)) { Check(false, "profile frame renders"); break; }
            if (frame >= 0) {
                const double elapsed = std::chrono::duration<double, std::milli>(end - start).count();
                times.push_back(elapsed);
                (was_moving || ui::ItemReflowUiTest::Surface(renderer).Pending() ? moving_times : idle_times).push_back(elapsed);
            }
            std::this_thread::sleep_until(start + std::chrono::milliseconds(16));
        }
        std::sort(times.begin(), times.end());
        if (!times.empty()) std::printf("[PROFILE] mode=%d frames=%zu draw_p50_ms=%.3f draw_p95_ms=%.3f draw_max_ms=%.3f\n",
            static_cast<int>(mode), times.size(), times[times.size() / 2],
            times[(times.size() * 95) / 100], times.back());
        for (auto* samples : {&moving_times, &idle_times}) {
            std::sort(samples->begin(), samples->end());
            if (!samples->empty()) std::printf("[PROFILE_PHASE] mode=%d zebra=%d moving=%d frames=%zu p50_ms=%.3f p95_ms=%.3f max_ms=%.3f\n",
                static_cast<int>(mode), zebra, samples == &moving_times, samples->size(),
                (*samples)[samples->size() / 2], (*samples)[samples->size() * 95 / 100], samples->back());
        }
    }
}
}

int main(int argc, char** argv) {
    using namespace pulse;
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc > 2 && std::string_view(argv[2]) == "--search-filter-only")
        std::printf("[TEST] starting search/filter fixture\n");
    OleInitialize(nullptr);
    l10n::Initialize(GetModuleHandleW(nullptr), L"en-US");
    if (argc > 2 && (std::string_view(argv[2]) == "--frame-profile-hidden" ||
                    std::string_view(argv[2]) == "--frame-profile-presented")) {
        const int result = ui::ItemReflowUiTest::RunFrameProfile(std::string_view(argv[2]) == "--frame-profile-presented");
        OleUninitialize();
        return result;
    }
    if (argc > 2 && std::string_view(argv[2]) == "--view-mode-feedback-only") {
        int result = 1;
        try {
            result = ui::ItemReflowUiTest::RunViewModeFeedback(std::filesystem::path(argv[1]));
            result |= ui::ItemReflowUiTest::RunWheelFeedback(std::filesystem::path(argv[1])) ? 1 : 0;
        } catch (const std::exception& error) {
            std::printf("[FAIL] view-mode feedback exception: %s\n", error.what());
        }
        OleUninitialize();
        return result;
    }
    if (argc > 2 && std::string_view(argv[2]) == "--view-mode-only") {
        std::printf("[TEST] starting view-mode fixture\n");
        int result = 1;
        try {
            result = ui::ItemReflowUiTest::RunViewModes(std::filesystem::path(argv[1]));
        } catch (const std::exception& error) {
            std::printf("[FAIL] view-mode fixture exception: %s\n", error.what());
        }
        OleUninitialize();
        return result;
    }
    if (argc > 2 && std::string_view(argv[2]) == "--search-continuity-only") {
        const int result = ui::ItemReflowUiTest::RunSearchContinuity(std::filesystem::path(argv[1]));
        OleUninitialize();
        return result;
    }
    if (argc > 2 && std::string_view(argv[2]) == "--bulk-only") {
        const int result = ui::ItemReflowUiTest::RunBulk(std::filesystem::path(argv[1]));
        OleUninitialize();
        return result;
    }
    if (argc > 2 && std::string_view(argv[2]) == "--search-filter-only") {
        int result = 1;
        try {
            result = ui::ItemReflowUiTest::RunSearchFilter(std::filesystem::path(argv[1]));
        } catch (const std::exception& error) {
            std::printf("[FAIL] search/filter fixture exception: %s\n", error.what());
        }
        OleUninitialize();
        return result;
    }
    const auto output = argc > 1 ? std::filesystem::path(argv[1]) :
        std::filesystem::path(L"bench_data/item-reflow");
    wchar_t transport[4]{};
    if (GetEnvironmentVariableW(L"PULSE_TEST_SNAPSHOT_STDOUT", transport, ARRAYSIZE(transport)) != 1 ||
        transport[0] != L'1') {
        std::error_code error;
        std::filesystem::create_directories(output, error);
        if (error) { Check(false, "create artifact directory"); OleUninitialize(); return 1; }
    }
    SetEnvironmentVariableW(L"PULSE_TEST_HIDDEN_SHOT", L"1");
    HWND hwnd = CreateWindowExW(WS_EX_NOACTIVATE, L"STATIC", L"Item reflow fixture",
        WS_POPUP, -30000, -30000, 1100, 820, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    {
        ui::Compositor compositor;
        if (!hwnd || !compositor.Init(hwnd)) {
            Check(false, "initialize actual pane compositor");
            if (hwnd) DestroyWindow(hwnd);
            OleUninitialize();
            return 1;
        }
        ui::MainRenderer renderer;
        renderer.SetCompositor(&compositor);
        renderer.SetDetailsPanelVisible(false);
        if (argc > 2 && (std::string(argv[2]) == "--profile" || std::string(argv[2]) == "--profile-no-zebra")) {
            Profile(compositor, renderer, std::string(argv[2]) == "--profile");
            renderer.SetCompositor(nullptr);
            DestroyWindow(hwnd);
            OleUninitialize();
            return failures ? 1 : 0;
        }
        for (const auto mode : {ui::ViewMode::Details, ui::ViewMode::List, ui::ViewMode::MediumIcons})
        for (const float scale : {1.0f, 1.5f}) for (const bool dark : {false, true}) {
            ui::ItemReflowUiTest::Reset(renderer);
            renderer.SetScale(scale);
            compositor.Resize(static_cast<int>(1100 * scale), static_cast<int>(820 * scale));
            compositor.RecreateTextFormats(scale);
            const auto rect = D2D1::RectF(0, 0, 1100 * scale, 820 * scale);
            ui::WindowViewModel vm;
            vm.dark = dark;
            vm.pane.path = L"C:\\pulse-reflow-fixture";
            vm.pane.header_text = L"Item transitions";
            vm.pane.is_file_system = true;
            vm.pane.view_mode = mode;
            const auto theme = ui::MakeTheme(dark, ui::HexColor(0x0078D4));
            auto frame = [&](uint64_t now, const wchar_t* label, bool animate = true) {
                auto* dc = compositor.Dc();
                dc->BeginDraw(); dc->Clear(theme.bg);
                ui::ItemReflowUiTest::Draw(renderer, vm, rect, theme, now, animate);
                Check(SUCCEEDED(dc->EndDraw()), "actual pane draws without Direct2D errors");
                if (label) {
                    const auto file = std::to_wstring(static_cast<int>(mode)) + L"-" +
                        (dark ? L"dark-" : L"light-") + (scale == 1 ? L"100-" : L"150-") + label + L".png";
                    Check(WriteAnimationSnapshot(compositor, output / file), "save actual pane animation frame");
                }
            };
            const auto visual_for = [&](wchar_t name) {
                const auto entry = Entry(name);
                return ui::ItemReflowUiTest::Surface(renderer).BackgroundVisual(
                    entry.path + L"\n" + entry.name, {}, 0);
            };
            Items(vm.pane, L"ABCDEFGH");
            frame(1000, L"before");
            const float zebra_before = mode == ui::ViewMode::Details
                ? RowTone(compositor, visual_for(L'C'), scale) : 0;
            const float removed_before = mode == ui::ViewMode::Details
                ? RowTone(compositor, visual_for(L'B'), scale) : 0;
            float zebra_middle = 0;
            Check(!renderer.SurfaceAnimating(), "initial listing has no entry animation");
            Items(vm.pane, L"ACEFGH");
            frame(1100, L"delete-0");
            if (mode == ui::ViewMode::Details) Check(
                std::fabs(RowTone(compositor, visual_for(L'C'), scale) - zebra_before) <= 1,
                "actual zebra pixels retain the old shade on the first deletion frame");
            Check(renderer.SurfaceAnimating(), "nonadjacent removal requests its successor frame");
            frame(1140, L"delete-40");
            if (mode == ui::ViewMode::Details) {
                const auto removed = visual_for(L'B');
                Check(std::fabs(RowTone(compositor, removed, scale) -
                    (removed_before + zebra_before) * 0.5f) <= 1.5f,
                    "deleted zebra background fades in the shrinking vacant space");
                const auto bounds = renderer.ContentRect(rect.right, rect.bottom);
                Check(renderer.ItemFromPointInPane(vm.pane, bounds,
                    removed.bounds.left + 350 * scale, (removed.bounds.top + removed.bounds.bottom) * 0.5f) == -1,
                    "fading removed backgrounds cannot be clicked as live rows");
            }
            const auto pane_rect = renderer.ContentRect(rect.right, rect.bottom);
            const auto final_c = renderer.ItemRectInPane(vm.pane, pane_rect, 1);
            const auto old_c = renderer.ItemRectInPane(vm.pane, pane_rect, 2);
            const float phase = 40.0f / 240.0f;
            const float progress = phase * phase * (3 - 2 * phase);
            float x = (final_c.left + old_c.left + final_c.right + old_c.right) * 0.25f;
            float y = old_c.top + (final_c.top - old_c.top) * progress +
                (final_c.bottom - final_c.top) * 0.5f;
            if (mode == ui::ViewMode::MediumIcons) {
                x = (old_c.left + old_c.right) * 0.5f + (final_c.left - old_c.left) * progress;
            } else x = final_c.left + 120 * scale;
            Check(renderer.ItemFromPointInPane(vm.pane, pane_rect, x, y) == 1,
                "moving C retains its current source index for mouse picking");
            const auto hit = renderer.HitTest(vm, rect, x, y);
            Check(hit.region == ui::HitTestResult::Row && hit.index == 1,
                "production hit testing picks the moving row rather than the old source");
            Check(ui::ItemReflowUiTest::Surface(renderer).CachedRows() > 0,
                "moving rows replay retained drawing commands");
            if (ui::UsesThumbnails(mode)) Check(ui::ItemReflowUiTest::VisibleThumbnails(renderer) > 0,
                "replayed icon rows retain their visible thumbnail working set");
            if (mode == ui::ViewMode::Details) {
                frame(1220, L"delete-midpoint");
                zebra_middle = RowTone(compositor, visual_for(L'C'), scale);
                Check(std::fabs(visual_for(L'C').stripe - 0.5f) < 0.001f,
                    "row shade reaches its midpoint while cached text continues moving");
            }
            frame(1340, L"delete-settled");
            if (mode == ui::ViewMode::Details) {
                const float zebra_final = RowTone(compositor, visual_for(L'C'), scale);
                Check(std::isfinite(zebra_before) && std::isfinite(zebra_middle) &&
                    std::fabs(zebra_final - zebra_before) >= 2 &&
                    std::fabs(zebra_middle - (zebra_before + zebra_final) * 0.5f) <= 1.5f,
                    "actual zebra pixels crossfade independently of cached foreground commands");
            }
            Check(!renderer.SurfaceAnimating(), "nonadjacent removal releases frame scheduling");
            Check(ui::ItemReflowUiTest::Surface(renderer).CachedRows() == 0,
                "settled layout releases row drawing commands");
            Items(vm.pane, L"XACEYFGH");
            frame(1400, L"insert-0");
            frame(1460, L"insert-60");
            Check(renderer.SurfaceAnimating(), "separated insertions animate siblings together");
            Items(vm.pane, L"XAEFGHZ");
            frame(1460, L"mixed-0");
            frame(1500, L"mixed-40");
            frame(1580, L"mixed-120");
            frame(1660, L"mixed-200");
            frame(1700, L"mixed-settled");
            Check(!renderer.SurfaceAnimating(), "interrupted mixed updates settle within the new deadline");
            Items(vm.pane, L"XAEF"); frame(1760, nullptr);
            vm.pane.path = L"C:\\pulse-reflow-other"; frame(1770, nullptr);
            Check(!renderer.SurfaceAnimating(), "navigation discards previous directory item tracks");
            Items(vm.pane, L"XAEFG"); frame(1840, nullptr);
            vm.pane.scroll_y = 10 * scale; frame(1850, nullptr);
            Check(!renderer.SurfaceAnimating(), "scrolling resets item movement");
            Items(vm.pane, L"XA"); frame(1940, nullptr, false);
            Check(!renderer.SurfaceAnimating(), "disabled motion removes outgoing items immediately");
            Items(vm.pane, L""); frame(2040, nullptr);
            frame(2280, nullptr);
            Check(!renderer.SurfaceAnimating(), "deletion to empty completes without a retained row");
            if (mode == ui::ViewMode::Details) {
                ui::ItemReflowUiTest::Reset(renderer);
                vm.pane.scroll_y = 0;
                Items(vm.pane, L"ABCDEFGH");
                vm.pane.date_groups = std::make_shared<const std::vector<ui::DateGroup>>(
                    std::vector<ui::DateGroup>{{0, 3, 0, L"First group", ui::GroupMode::Type},
                        {3, 5, 1, L"Second group", ui::GroupMode::Type}});
                frame(2400, L"groups-before");
                Check(visual_for(L'D').stripe == 0 && visual_for(L'E').stripe == 1,
                    "second group restarts zebra alternation after an odd-sized first group");
                Items(vm.pane, L"DEFGH");
                vm.pane.date_groups = std::make_shared<const std::vector<ui::DateGroup>>(
                    std::vector<ui::DateGroup>{{0, 5, 1, L"Second group", ui::GroupMode::Type}});
                frame(2500, L"groups-delete-0");
                frame(2540, L"groups-delete-40");
                Check(visual_for(L'D').stripe == 0 && visual_for(L'E').stripe == 1,
                    "removing the first group does not invert the next group's shades");
                Check(renderer.SurfaceAnimating(), "group headers move with surviving rows when a group disappears");
                frame(2740, L"groups-settled");
                Check(!renderer.SurfaceAnimating(), "grouped reflow releases scheduling at its endpoint");
                ui::ItemReflowUiTest::Reset(renderer);
                vm.pane.date_groups.reset();
                Items(vm.pane, L"AB");
                frame(2800, L"empty-before");
                Items(vm.pane, L"");
                frame(2900, L"empty-background-0");
                Check(renderer.SurfaceAnimating(), "last deleted stripe keeps its short background fade scheduled");
                const auto removed = visual_for(L'B');
                Check(renderer.ItemFromPointInPane(vm.pane, pane_rect, removed.bounds.left + 350 * scale,
                    (removed.bounds.top + removed.bounds.bottom) * 0.5f) == -1,
                    "an empty list never picks a fading zebra background");
                frame(2940, L"empty-background-40");
                frame(2980, L"empty-background-settled");
                Check(!renderer.SurfaceAnimating(), "last stripe fade settles before displaying the empty state");
            }
        }
        renderer.SetCompositor(nullptr);
    }
    DestroyWindow(hwnd);
    OleUninitialize();
    std::printf("item reflow UI: %d failures\n", failures);
    return failures ? 1 : 0;
}
