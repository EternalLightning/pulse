// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "../ui/ui_renderer_internal.h"
#include <cstdio>
#include <filesystem>

namespace {
bool AccentIsCentered(pulse::ui::Compositor& compositor, const D2D1_RECT_F& task, float scale) {
    using namespace pulse::ui;
    auto* dc = compositor.Dc();
    ComPtr<ID2D1Image> image;
    ComPtr<ID2D1Bitmap1> target, readback;
    dc->GetTarget(&image);
    if (!image.get() || FAILED(image->QueryInterface(__uuidof(ID2D1Bitmap1),
        reinterpret_cast<void**>(&target.p)))) return false;
    const auto props = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        target->GetPixelFormat(), 96, 96);
    if (FAILED(dc->CreateBitmap(target->GetPixelSize(), nullptr, 0, &props, &readback)) ||
        FAILED(readback->CopyFromBitmap(nullptr, target.get(), nullptr))) return false;
    D2D1_MAPPED_RECT mapped{};
    if (FAILED(readback->Map(D2D1_MAP_OPTIONS_READ, &mapped))) return false;
    const auto size = target->GetPixelSize();
    int left = static_cast<int>(size.width), right = -1;
    for (int y = std::max(0, static_cast<int>(task.top)); y < static_cast<int>(size.height); ++y) {
        const auto* row = mapped.bits + static_cast<size_t>(y) * mapped.pitch;
        for (int x = 0; x < static_cast<int>(size.width); ++x) {
            const auto* pixel = row + x * 4;
            if (pixel[0] > pixel[2] + 40 && pixel[1] > pixel[2] + 20) {
                left = std::min(left, x);
                right = std::max(right, x);
            }
        }
    }
    readback->Unmap();
    return right >= left && left >= task.left - scale && right <= task.right + scale &&
        std::abs((left + right) * 0.5f - (task.left + task.right) * 0.5f) <= 4.0f * scale;
}
}

int RunStatusBarUiTest() {
    using namespace pulse::ui;
    int checks = 0, failures = 0;
    auto check = [&](bool ok, const char* label) {
        ++checks;
        failures += !ok;
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
    };
    const auto output = std::filesystem::absolute(L"bench_data/status-bar-centered");
    std::filesystem::create_directories(output);
    SetEnvironmentVariableW(L"PULSE_TEST_HIDDEN_SHOT", L"1");
    HWND window = CreateWindowExW(WS_EX_NOACTIVATE, L"STATIC", L"Status bar fixture",
        WS_OVERLAPPEDWINDOW, 0, 0, 2012, 640, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    {
        Compositor compositor;
        if (!window || !compositor.Init(window)) return 2;
        MainRenderer renderer;
        renderer.SetCompositor(&compositor);
        renderer.SetDetailsPanelVisible(false);
        WindowViewModel vm;
        vm.window_title = L"Pulse";
        vm.tabs.push_back({L"状态栏布局", true});
        vm.status.status_text = L"21 个项目 (2 文件夹, 19 文件)";
        vm.status.selection_text = L"已选中 1 项: exam_thesis.cls  6.4 KB";
        vm.status.hint_text = L"Enter 打开 · 空格 预览 · F2 重命名";
        for (const float scale : {1.0f, 1.5f}) for (const int width : {2012, 960, 540}) {
            renderer.SetScale(scale);
            compositor.RecreateTextFormats(scale);
            compositor.Resize(static_cast<UINT>(width * scale), static_cast<UINT>(640 * scale));
            const auto rect = D2D1::RectF(0, 0, width * scale, 640 * scale);
            for (const bool dark : {false, true}) for (const int scenario : {0, 1, 2}) {
                vm.dark = dark;
                vm.status.task_text = scenario == 0 ? L"重命名 main.pdf → 126031910037_董欣文_作业1.pdf已完成" :
                    scenario == 1 ? L"重命名已完成" : L"正在复制 19 个文件";
                vm.status.task_progress = scenario == 2 ? 42.0f : -1.0f;
                const auto metrics = MakeStatusBarMetrics(vm, rect, scale, 28 * scale,
                    compositor.DwriteFactory(), compositor.SmallFormat());
                const float task_width = metrics.task.right - metrics.task.left;
                const float desired_width = MeasureTextWidth(compositor.DwriteFactory(), compositor.SmallFormat(),
                    vm.status.task_text) + (scenario == 2 ? 108 * scale : 0);
                const float center = (rect.left + rect.right) * 0.5f;
                const float status_width = MeasureTextWidth(compositor.DwriteFactory(), compositor.SmallFormat(),
                    vm.status.status_text);
                const float selection_width = MeasureTextWidth(compositor.DwriteFactory(), compositor.SmallFormat(),
                    vm.status.selection_text);
                const float left_content_end = metrics.pad + status_width + selection_width + 32 * scale;
                const bool centered_fits = center - desired_width * 0.5f >= left_content_end &&
                    center + desired_width * 0.5f <= rect.right - metrics.right_reserved - 16 * scale;
                check(!centered_fits || std::abs((metrics.task.left + metrics.task.right) * 0.5f - center) < scale,
                    "operation text and optional progress track center on the window when they fit");
                check(task_width >= 0 && task_width <= desired_width + scale && metrics.task.left >= metrics.pad &&
                    metrics.task.right <= rect.right - metrics.right_reserved,
                    "narrow operation content stays within its available area and clear of trailing hints");
                check(StatusBarHitRegion(vm, rect, (metrics.task.left + metrics.task.right) * 0.5f,
                    metrics.bar.top + 14 * scale, scale, 28 * scale, &compositor) == HitTestResult::StatusBarTask,
                    "centered operation retains its details hit target");
                compositor.Dc()->BeginDraw();
                renderer.Render(vm, rect, MakeTheme(dark, HexColor(0x0078D4)));
                check(SUCCEEDED(compositor.Dc()->EndDraw()), "status bar production renderer completes");
                if (centered_fits && scenario != 2) check(AccentIsCentered(compositor, metrics.task, scale),
                    "actual blue text pixels are centered within the measured operation bounds");
                if (scale == 1.0f || width == 2012) {
                    const auto name = std::to_wstring(width) + L"-" + std::to_wstring(static_cast<int>(scale * 100)) +
                        (dark ? L"-dark-" : L"-light-") + std::to_wstring(scenario) + L".png";
                    check(compositor.SaveSnapshot((output / name).c_str()), "save production status bar evidence");
                }
            }
            vm.status.task_text.clear();
            vm.status.task_progress = -1;
            const auto idle = MakeStatusBarMetrics(vm, rect, scale, 28 * scale,
                compositor.DwriteFactory(), compositor.SmallFormat());
            check(idle.task.left == idle.task.right, "idle status bar leaves no operation hit area");
            vm.status.query_active = vm.status.query_cancellable = true;
            vm.status.query_text = L"正在搜索文件内容";
            const auto query = MakeStatusBarMetrics(vm, rect, scale, 28 * scale,
                compositor.DwriteFactory(), compositor.SmallFormat());
            check(std::abs((query.task.left + query.cancel_search.right) * 0.5f - rect.right * 0.5f) < scale &&
                query.task.right <= query.cancel_search.left,
                "search progress and cancel button keep their existing centered group");
            vm.status.query_active = vm.status.query_cancellable = false;
        }
        renderer.SetCompositor(nullptr);
    }
    DestroyWindow(window);
    SetEnvironmentVariableW(L"PULSE_TEST_HIDDEN_SHOT", nullptr);
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
