// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "snapshot_output.h"

inline std::vector<uint32_t> ReadScrollPixels(pulse::ui::Compositor& compositor, RECT bounds) {
    using Microsoft::WRL::ComPtr;
    auto* dc = compositor.Dc();
    ComPtr<ID2D1Image> image;
    ComPtr<ID2D1Bitmap1> target, cpu;
    dc->GetTarget(&image);
    if (!image || FAILED(image.As(&target))) return {};
    const UINT width = static_cast<UINT>(bounds.right - bounds.left);
    const UINT height = static_cast<UINT>(bounds.bottom - bounds.top);
    const auto properties = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW, target->GetPixelFormat());
    const D2D1_RECT_U source{static_cast<UINT>(bounds.left), static_cast<UINT>(bounds.top),
        static_cast<UINT>(bounds.right), static_cast<UINT>(bounds.bottom)};
    if (FAILED(dc->CreateBitmap(D2D1::SizeU(width, height), nullptr, 0, properties, &cpu)) ||
        FAILED(cpu->CopyFromBitmap(nullptr, target.Get(), &source))) return {};
    D2D1_MAPPED_RECT mapped{};
    if (FAILED(cpu->Map(D2D1_MAP_OPTIONS_READ, &mapped))) return {};
    std::vector<uint32_t> pixels(static_cast<size_t>(width) * height);
    for (UINT row = 0; row < height; ++row)
        std::memcpy(pixels.data() + static_cast<size_t>(row) * width,
            mapped.bits + static_cast<size_t>(row) * mapped.pitch, static_cast<size_t>(width) * 4);
    cpu->Unmap();
    return pixels;
}

inline int pulse::ui::ItemReflowUiTest::RunScrollGhosting() {
    using namespace pulse;
    int failures = 0;
    const auto check = [&](bool passed, const char* label) {
        std::printf("[%s] %s\n", passed ? "PASS" : "FAIL", label);
        failures += !passed;
    };
    HWND hwnd = CreateWindowExW(WS_EX_NOACTIVATE, L"STATIC", L"Scroll ghosting fixture",
        WS_POPUP, -30000, -30000, 1100, 820, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    Compositor compositor;
    if (!hwnd || !compositor.Init(hwnd)) {
        check(false, "initialize scroll ghosting compositor");
        if (hwnd) DestroyWindow(hwnd);
        return 1;
    }
    MainRenderer history, reference;
    history.SetCompositor(&compositor); reference.SetCompositor(&compositor);
    history.SetDetailsPanelVisible(false); reference.SetDetailsPanelVisible(false);
    const auto fill = [](PaneViewModel& pane, const wchar_t* prefix) {
        pane.entries.clear();
        for (int i = 0; i < 60; ++i) {
            ListEntryView entry;
            entry.name = std::wstring(prefix) + L" document " + std::to_wstring(i) + L".txt";
            entry.path = pane.path + L"\\" + entry.name;
            entry.type_text = L"Text document"; entry.size_text = L"4 KB";
            entry.date_text = L"2026-11-11 12:00";
            entry.record_only = true;
            pane.entries.push_back(std::move(entry));
        }
    };
    for (float scale : {1.0f, 1.25f, 1.5f, 2.0f}) for (bool dark : {false, true}) {
        compositor.Resize(static_cast<UINT>(1100 * scale), static_cast<UINT>(820 * scale));
        compositor.RecreateTextFormats(scale);
        history.SetScale(scale); reference.SetScale(scale);
        Reset(history); Reset(reference);
        const auto theme = MakeTheme(dark, HexColor(0x0078D4));
        const auto bounds = D2D1::RectF(0, 0, 1100 * scale, 820 * scale);
        WindowViewModel vm;
        vm.dark = dark; vm.pane.is_file_system = true;
        vm.pane.path = L"C:\\scroll-ghosting-old"; vm.pane.navigation_owner = 1;
        vm.pane.navigation_revision = 1; vm.pane.view_mode = ViewMode::Details;
        fill(vm.pane, L"OLD");
        const auto list = history.PaneListRect(vm.pane, history.ContentRect(bounds.right, bounds.bottom));
        const RECT crop{static_cast<LONG>(std::ceil(list.left)) + 3,
            static_cast<LONG>(std::ceil(list.top)) + 3, static_cast<LONG>(list.right) - 18,
            static_cast<LONG>(list.bottom) - 3};
        const auto frame = [&](MainRenderer& renderer, uint64_t now) {
            auto* dc = compositor.Dc(); dc->BeginDraw(); dc->Clear(theme.bg);
            Draw(renderer, vm, bounds, theme, now);
            check(SUCCEEDED(dc->EndDraw()), "scroll frame has no Direct2D error");
            return ReadScrollPixels(compositor, crop);
        };
        const auto save = [&](std::vector<uint32_t>& pixels, const wchar_t* label) {
            const auto name = std::wstring(L"ghost-") + (dark ? L"dark-" : L"light-") +
                std::to_wstring(static_cast<int>(scale * 100)) + L"-" + label + L".png";
            check(!pixels.empty() && bench::EmitSnapshot(name, crop.right - crop.left,
                crop.bottom - crop.top, (crop.right - crop.left) * 4, reinterpret_cast<BYTE*>(pixels.data())),
                "encode scroll pixel evidence");
        };
        frame(history, 1000); frame(history, 1300);
        uint64_t now = 1400;
        for (float offset : {0.25f, 0.75f, 8.5f, 34.0f, 84.25f, 40.5f, 3.25f, 0.0f}) {
            vm.pane.scroll_y = offset * scale;
            auto actual = frame(history, now);
            Reset(reference);
            const auto expected = frame(reference, now);
            check(!actual.empty() && actual == expected,
                "fractional scrolling and reversal leave no pixels from an older frame");
            now += 8;
        }
        vm.pane.path = L"C:\\scroll-ghosting-new";
        ++vm.pane.navigation_revision;
        vm.pane.navigation_motion = SurfaceMotion::Forward;
        fill(vm.pane, L"NEW");
        frame(history, 1600);
        check(history.pane_surfaces_[0].pending, "navigation starts the existing content transition");
        auto transition = frame(history, 1620); save(transition, L"navigation");
        vm.pane.scroll_y = 10.5f * scale;
        auto actual = frame(history, 1628); save(actual, L"scrolled-history");
        Reset(reference);
        auto expected = frame(reference, 1628); save(expected, L"scrolled-reference");
        size_t differences = 0;
        if (actual.size() == expected.size()) for (size_t i = 0; i < actual.size(); ++i)
            differences += actual[i] != expected[i];
        std::printf("[SCROLL_GHOSTING] dark=%d scale=%.2f differing_pixels=%zu navigation_pending=%d\n",
            dark, scale, differences, history.pane_surfaces_[0].pending);
        check(!actual.empty() && actual == expected && !history.pane_surfaces_[0].pending,
            "scrolling during navigation immediately removes the retained old content");
        vm.pane.path = L"C:\\scroll-ghosting-next"; ++vm.pane.navigation_revision;
        vm.pane.scroll_y = 0;
        fill(vm.pane, L"NEXT");
        frame(history, 1800);
        check(history.pane_surfaces_[0].pending,
            "later navigation still animates when its destination resets the scroll position");
        vm.pane.hover_index = 4;
        frame(history, 2100); frame(history, 2300);
        vm.pane.hover_index = 5;
        frame(history, 2310);
        check(history.SurfaceAnimating(), "moving the pointer over a stationary list retains hover fades");
        now = 2340;
        for (float offset : {36.0f, 72.0f, 108.0f}) {
            vm.pane.scroll_y = offset * scale;
            ViewLayout layout(ViewMode::Details, list, vm.pane.EntryCount(), 0, vm.pane.scroll_y,
                scale, history.ListRowHeightDip(vm.pane));
            vm.pane.hover_index = layout.HitTest(list.left + 80 * scale,
                list.top + history.ListRowHeightDip(vm.pane) * scale * 4.5f);
            actual = frame(history, now);
            size_t outgoing_hover = 0;
            const std::wstring prefix = L"0:" + vm.pane.path + L":";
            for (const auto& [key, track] : history.hover_tracks_)
                if (key.starts_with(prefix) && track.animation.value > 0 && track.animation.target == 0)
                    ++outgoing_hover;
            check(outgoing_hover == 0, "scrolling under a stationary pointer leaves no outgoing row hover trails");
            now += 16;
        }
        save(actual, L"steady-hover-history");
        Reset(reference); reference.hover_tracks_.clear();
        frame(reference, now - 220);
        expected = frame(reference, now);
        save(expected, L"steady-hover-reference");
        check(!actual.empty() && actual == expected,
            "steady scrolling paints only the hovered row now under the pointer");
    }
    history.SetCompositor(nullptr); reference.SetCompositor(nullptr); compositor.Shutdown(); DestroyWindow(hwnd);
    return failures ? 1 : 0;
}
