// SPDX-License-Identifier: Apache-2.0
#pragma once

inline int pulse::ui::ItemReflowUiTest::RunBulk(const std::filesystem::path& output) {
    using namespace pulse;
    int failures = 0;
    const auto check = [&](bool passed, const char* label) {
        std::printf("[%s] %s\n", passed ? "PASS" : "FAIL", label);
        failures += !passed;
    };
    std::filesystem::create_directories(output);
    SetEnvironmentVariableW(L"PULSE_TEST_HIDDEN_SHOT", L"1");
    HWND hwnd = CreateWindowExW(WS_EX_NOACTIVATE, L"STATIC", L"Bulk item reflow",
        WS_POPUP, -30000, -30000, 1100, 820, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    Compositor compositor;
    if (!hwnd || !compositor.Init(hwnd)) {
        check(false, "initialize bulk update compositor");
        if (hwnd) DestroyWindow(hwnd);
        return 1;
    }
    MainRenderer renderer;
    renderer.SetCompositor(&compositor);
    renderer.SetDetailsPanelVisible(false);
    for (const auto mode : {ViewMode::Details, ViewMode::List, ViewMode::MediumIcons})
    for (const bool search : {false, true})
    for (const float scale : {1.0f, 1.5f}) for (const bool dark : {false, true}) {
        std::printf("[CASE] mode=%d search=%d scale=%.1f dark=%d\n",
            static_cast<int>(mode), search, scale, dark);
        Reset(renderer);
        renderer.SetScale(scale);
        compositor.Resize(static_cast<int>(1100 * scale), static_cast<int>(820 * scale));
        compositor.RecreateTextFormats(scale);
        const auto rect = D2D1::RectF(0, 0, 1100 * scale, 820 * scale);
        const auto theme = MakeTheme(dark, HexColor(0x0078D4));
        WindowViewModel vm;
        vm.dark = dark;
        vm.pane.path = L"C:\\pulse-bulk-fixture";
        vm.pane.header_text = L"Bulk item updates";
        vm.pane.is_file_system = !search;
        vm.pane.is_search = vm.pane.is_query_search = search;
        vm.pane.view_mode = mode;
        const auto fill = [&](int base, int count) {
            vm.pane.entries.clear();
            for (int i = 0; i < count; ++i) {
                ListEntryView entry;
                entry.name = L"Document " + std::to_wstring(base + i) + L".txt";
                entry.path = vm.pane.path + L"\\" + entry.name;
                entry.type_text = L"Text document";
                entry.size_text = L"4 KB";
                vm.pane.entries.push_back(std::move(entry));
            }
            ++vm.pane.view_generation;
        };
        const auto visual = [&](int row) {
            const auto& entry = vm.pane.entries[static_cast<size_t>(row)];
            return Surface(renderer).BackgroundVisual(entry.path + L"\n" + entry.name, {}, 0);
        };
        const auto frame = [&](uint64_t now, const wchar_t* label = nullptr, bool animate = true) {
            auto* dc = compositor.Dc();
            dc->BeginDraw(); dc->Clear(theme.bg);
            Draw(renderer, vm, rect, theme, now, animate);
            check(SUCCEEDED(dc->EndDraw()), "bulk pane renders without Direct2D errors");
            if (label) {
                const auto file = std::to_wstring(static_cast<int>(mode)) +
                    (search ? L"-search-" : L"-folder-") + (dark ? L"dark-" : L"light-") +
                    std::to_wstring(static_cast<int>(scale * 100)) + L"-" + label + L".png";
                check(WriteAnimationSnapshot(compositor, output / file), "save bulk update frame");
            }
        };
        fill(0, 240);
        frame(1000, L"before");
        fill(1000, 240);
        frame(1100, L"replace-0");
        const auto bounds = renderer.ContentRect(rect.right, rect.bottom);
        const auto target = renderer.ItemRectInPane(vm.pane, bounds, 0);
        const auto initial = visual(0);
        check(Surface(renderer).Pending() && initial.opacity == 0 &&
            initial.bounds.top == target.top,
            "replacement fades in its real slots without a fabricated page slide");
        float x = (initial.bounds.left + initial.bounds.right) * 0.5f;
        float y = (initial.bounds.top + initial.bounds.bottom) * 0.5f;
        check(renderer.ItemFromPointInPane(vm.pane, bounds, x, y) == -1,
            "an unrevealed replacement cannot intercept clicks");
        if (mode == ViewMode::MediumIcons) {
            check(renderer.ItemFromPointInPane(vm.pane, bounds,
                initial.bounds.right - scale, y) == -1,
                "unrevealed icon gutters remain noninteractive");
        }
        frame(1180, L"replace-80");
        check(visual(0).opacity > 0 && visual(0).bounds.top == target.top &&
            renderer.ItemFromPointInPane(vm.pane, bounds, x, y) == 0,
            "vacant replacement slots reveal immediately and pick their live source");
        frame(1220, L"replace-120");
        frame(1340, L"settled");
        check(!Surface(renderer).Pending() && Surface(renderer).CachedRows() == 0 &&
            std::fabs(visual(0).bounds.top - target.top) < 0.01f,
            "replacement settles at exact geometry and releases cached commands");

        Reset(renderer);
        fill(0, 240); frame(1500);
        const auto anchor = vm.pane.entries[5];
        const auto old_anchor = visual(5);
        fill(4000, 240);
        vm.pane.entries[1] = anchor;
        frame(1600, L"mixed-0");
        check(visual(1).bounds.top == old_anchor.bounds.top &&
            visual(1).bounds.left == old_anchor.bounds.left && visual(1).opacity == 1,
            "a visible surviving identity keeps its old position during bulk entry");
        frame(1720, L"mixed-120");
        const auto moving_anchor = visual(1);
        x = moving_anchor.bounds.left + 50 * scale;
        y = (moving_anchor.bounds.top + moving_anchor.bounds.bottom) * 0.5f;
        check(renderer.ItemFromPointInPane(vm.pane, bounds, x, y) == 1,
            "incoming rows cannot cover or intercept the moving survivor");
        frame(1840);
        check(!Surface(renderer).Pending(), "mixed bulk entry settles on its original deadline");

        // Retained neighbors below the old viewport become the entire new page.
        Reset(renderer);
        fill(0, 240); frame(2000);
        const auto range = renderer.VisibleRangeInPane(vm.pane, bounds);
        const int first = range.second + 2;
        const int page = range.second - range.first + 1;
        const auto distant_before = visual(first);
        fill(first, page); frame(2100, L"distant-0");
        check(distant_before.source >= 0 && visual(0).opacity == 1 &&
            visual(0).bounds.top == distant_before.bounds.top &&
            visual(0).bounds.left == distant_before.bounds.left,
            "distant survivors start from their actual old geometry without teleporting");
        frame(2180, L"distant-80");
        fill(2000, page); frame(2180);
        check(visual(0).opacity == 0 && visual(0).bounds.top == target.top && Surface(renderer).Pending(),
            "another bulk update starts its own real-slot reveal");
        frame(2420);
        check(!Surface(renderer).Pending(), "rapid replacement reaches a finite endpoint");
        fill(3000, page); frame(2500, nullptr, false);
        check(!Surface(renderer).Pending() && visual(0).opacity == 1,
            "disabled motion snaps the bulk update visibly");
    }
    renderer.SetCompositor(nullptr);
    DestroyWindow(hwnd);
    std::printf("bulk_reflow_failures=%d\n", failures);
    return failures ? 1 : 0;
}
