// SPDX-License-Identifier: Apache-2.0
#pragma once

inline int pulse::ui::ItemReflowUiTest::RunHeaderIconClip(const std::filesystem::path& output) {
    std::filesystem::create_directories(output);
    SetEnvironmentVariableW(L"PULSE_TEST_HIDDEN_SHOT", L"1");
    HWND hwnd = CreateWindowExW(WS_EX_NOACTIVATE, L"STATIC", L"Header icon clipping",
        WS_POPUP, -30000, -30000, 760, 540, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    int failures = 0;
    const auto check = [&](bool ok, const char* label) {
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
        failures += !ok;
    };
    {
        Compositor compositor;
        if (!hwnd || !compositor.Init(hwnd)) {
            if (hwnd) DestroyWindow(hwnd);
            return 1;
        }
        MainRenderer renderer;
        renderer.SetCompositor(&compositor);
        renderer.SetDetailsPanelVisible(false);
        for (float scale : {1.0f, 1.5f}) for (bool dark : {false, true}) {
            compositor.Resize(static_cast<int>(760 * scale), static_cast<int>(540 * scale));
            compositor.RecreateTextFormats(scale);
            renderer.SetScale(scale);
            Reset(renderer);
            const auto theme = MakeTheme(dark, HexColor(0x0078D4));
            const auto rect = D2D1::RectF(0, 0, 760 * scale, 540 * scale);
            WindowViewModel vm;
            vm.dark = dark;
            vm.pane.path = L"C:\\pulse-header-icon-fixture";
            vm.pane.header_text = L"Projects";
            vm.pane.is_file_system = true;
            vm.pane.view_mode = ViewMode::Details;
            for (int i = 0; i < 80; ++i) {
                ListEntryView entry;
                entry.name = L"Folder " + std::to_wstring(i);
                entry.path = vm.pane.path + L"\\" + entry.name;
                entry.is_dir = true;
                entry.record_only = true;
                entry.attrs = FILE_ATTRIBUTE_DIRECTORY;
                vm.pane.entries.push_back(std::move(entry));
            }
            uint64_t now = 1000;
            const auto draw = [&] {
                auto* dc = compositor.Dc();
                dc->BeginDraw(); dc->Clear(theme.bg);
                Draw(renderer, vm, rect, theme, now += 300);
                check(SUCCEEDED(dc->EndDraw()), "header clipping frame has no Direct2D error");
            };
            draw();
            const auto pane = renderer.ContentRect(rect.right, rect.bottom);
            const auto list = renderer.PaneListRect(vm.pane, pane);
            const auto header = D2D1::RectF(list.left, pane.top + renderer.PaneHeaderHeight(),
                std::min(list.right, list.left + 180 * scale), list.top);
            const auto baseline = view_mode_title::Pixels(compositor, header);
            check(!baseline.empty(), "capture unscrolled column header pixels");
            for (float offset : {12.0f, 24.0f, 40.0f, 100.0f}) {
                vm.pane.scroll_y = offset * scale;
                draw();
                check(view_mode_title::Pixels(compositor, header) == baseline,
                    "scrolled row icons never paint over the name column header");
            }
            const auto name = std::wstring(dark ? L"dark-" : L"light-") +
                (scale == 1 ? L"100" : L"150") + L"-scrolled.png";
            check(WriteAnimationSnapshot(compositor, output / name), "save scrolled header rendering");
        }
        renderer.SetCompositor(nullptr);
    }
    DestroyWindow(hwnd);
    return failures ? 1 : 0;
}
