#pragma once

#include "../app/app_input.h"
#include "../app/folder_view_prefs.h"
#include <cmath>

inline int RunCloudColumnsCases() {
    using namespace pulse;
    using K = ui::MainRenderer::ColumnKind;
    if (FAILED(OleInitialize(nullptr))) return 2;
    l10n::Initialize(GetModuleHandleW(nullptr), L"zh-CN");
    int failures = 0, checks = 0;
    const auto check = [&](bool ok, const char* label) {
        ++checks;
        if (!ok) { ++failures; std::cout << "[FAIL] " << label << '\n'; }
    };
    const auto output = std::filesystem::absolute(L"docs/validation/cloud-columns-20261029");
    std::filesystem::create_directories(output);
    app::FolderViewPrefs prefs, restored_prefs;
    prefs.SetColumns(L"C:\\Cloud", {140, 120, 90, 88});
    std::wstring prefs_json = L"{\"version\":1";
    prefs.AppendJson(prefs_json);
    restored_prefs.ReadJson(prefs_json + L"}");
    check(restored_prefs.FindColumns(L"C:\\Cloud") == prefs.FindColumns(L"C:\\Cloud"), "status column width survives folder preference round trip");
    restored_prefs.ReadJson(L"{\"folder_columns\":[{\"path\":\"C:\\\\Cloud\",\"widths\":[140,120,90]}]}");
    check(restored_prefs.FindColumns(L"C:\\Cloud") == std::array<float, 4>{140, 120, 90, 0}, "legacy folder widths keep automatic status width");
    auto owned = std::make_unique<AppState>();
    auto& s = *owned;
    s.isolatedTest = true;
    s.appPrefs.persist = s.places.persist = s.ctxMenuPrefs.persist = s.searchHistory.persist = false;
    s.showDetailsPanel = false;
    s.hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"STATIC", L"Cloud columns fixture",
        WS_POPUP, -20000, -20000, 1500, 900, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!s.hwnd || !s.compositor.Init(s.hwnd)) return 2;
    s.renderer.SetCompositor(&s.compositor);
    s.renderer.SetDetailsPanelVisible(false);
    s.window_tabs.EnsureDefault();
    BindCurrentLayout(s);
    auto* tab = ActiveTab(s);
    tab->current_path = L"C:\\PulseFixtureCloud";
    tab->view_mode = ui::ViewMode::Details;
    tab->loading = false;
    auto rows = std::make_shared<std::vector<fs::DirEntry>>(3);
    for (size_t i = 0; i < rows->size(); ++i) {
        auto& row = (*rows)[i];
        row.name = i == 0 ? L"仅在线文档.txt" : i == 1 ? L"本地可用文档.txt" : L"始终保留的文档.txt";
        row.attrs = i == 0 ? fs::kAttrRecallOnDataAccess : i == 1 ? FILE_ATTRIBUTE_ARCHIVE : fs::kAttrPinned;
        row.size = 2048;
        row.mtime = {0, 31000000};
    }
    tab->SetSnapshot(rows);
    app::PublishCloudFilesRoots({tab->current_path});
    SetEnvironmentVariableW(L"PULSE_TEST_DATA_DIR", output.c_str());
    SetEnvironmentVariableW(L"PULSE_TEST_CLOUD_SYNC", L"仅在线文档.txt=0;本地可用文档.txt=0;始终保留的文档.txt=0");
    for (float scale : {1.0f, 1.5f, 2.0f}) for (bool search : {false, true}) {
        s.scale = scale;
        s.renderer.SetScale(scale);
        s.compositor.RecreateTextFormats(scale);
        for (float width : {280.0f, 540.0f, 900.0f, 1400.0f}) {
            ui::WindowViewModel frame;
            const auto window = D2D1::RectF(0, 0, (width + 240) * scale, 850 * scale);
            ui::PaneSlotView slot;
            slot.focused = true;
            slot.rect = D2D1::RectF(240 * scale, 200 * scale, window.right, window.bottom);
            slot.pane = BuildVm(s, false).pane;
            slot.pane.is_search = search;
            slot.pane.header_text = L"OneDrive";
            frame.pane_slots.push_back(slot);
            const auto body = s.renderer.PaneBodyBounds(slot.pane, slot.rect);
            const auto initial = s.renderer.DetailsColumns(body, slot.pane);
            auto local_pane = slot.pane;
            local_pane.cloud_status_column = false;
            const auto local = s.renderer.DetailsColumns(body, local_pane);
            check(!local.Has(K::Cloud) && local.cloud_width == 0.0f, "local folders keep their existing column order");
            const auto narrow = s.renderer.DetailsColumns(D2D1::RectF(0, 0, 190 * scale, 650 * scale), slot.pane);
            check(!narrow.Has(K::Cloud) && narrow.cloud_width == 0.0f, "status column hides when the pane is too narrow");
            check(initial.Has(K::Cloud) && initial.IndexOf(K::Cloud) > 0 &&
                (!initial.Has(K::Date) || initial.IndexOf(K::Cloud) < initial.IndexOf(K::Date)),
                "status column precedes modified date in ordinary and search layouts");
            const float header_y = s.renderer.PaneListRect(slot.pane, body).top - s.renderer.ColumnHeaderHeight() * 0.5f;
            for (int divider = 0; divider < initial.count - 1; ++divider) {
                const auto hit = s.renderer.HitTest(frame, window, initial.DividerX(divider), header_y);
                check(hit.region == ui::HitTestResult::ColumnDivider && hit.index == divider,
                    "every painted divider hits its own index, including both status edges");
            }
            const auto status = s.renderer.HitTest(frame, window,
                initial.cloud_left + initial.cloud_width * 0.5f, header_y);
            check(status.region == ui::HitTestResult::Pane, "status header does not sort another column");
            const auto outside = s.renderer.HitTest(frame, window, initial.right, header_y);
            check(outside.region != ui::HitTestResult::ColumnDivider, "no invisible resize target after the final column");
            for (const auto [kind, sort] : {std::pair{K::Date, ui::SortColumn::Mtime},
                     std::pair{K::Type, ui::SortColumn::Type}, std::pair{K::Size, ui::SortColumn::Size}}) {
                if (!initial.Has(kind)) continue;
                const auto hit = s.renderer.HitTest(frame, window,
                    initial.Left(kind) + initial.Width(kind) * 0.5f, header_y);
                check(hit.region == ui::HitTestResult::ColumnHeader && hit.column == sort,
                    "metadata headers retain their own sort action after status moves");
            }
            for (int divider = 0; divider < initial.count - 1; ++divider) for (float delta : {-8.0f, 8.0f}) {
                auto pane = slot.pane;
                for (int i = 0; i < initial.count; ++i) {
                    const auto kind = initial.kinds[i];
                    const int stored = kind == K::Date ? 0 : kind == K::Type ? 1 : kind == K::Size ? 2 : kind == K::Cloud ? 3 : -1;
                    if (stored >= 0) {
                        pane.details_column_dividers[stored] = initial.widths[i] / scale;
                        pane.search_column_dividers[stored + 1] = initial.widths[i] / scale;
                    } else if (kind == K::Name && initial.Has(K::Path))
                        pane.search_column_dividers[0] = initial.widths[i] / scale;
                }
                const auto captured = s.renderer.DetailsColumns(body, pane);
                check(captured.count == initial.count && std::abs(captured.DividerX(divider) - initial.DividerX(divider)) < 0.1f,
                    "capturing painted widths does not jump the divider");
                const float cursor = initial.DividerX(divider) + delta * scale;
                if (search) pane.search_column_dividers = s.renderer.ResizeSearchColumnDivider(body, pane.search_column_dividers, divider, cursor, true);
                else pane.details_column_dividers = s.renderer.ResizeDetailsColumnDivider(body, pane.details_column_dividers, divider, cursor, true);
                const auto after = s.renderer.DetailsColumns(body, pane);
                const float pair_left = divider ? initial.DividerX(divider - 1) : initial.left;
                const float pair_right = initial.DividerX(divider + 1);
                const bool unconstrained = cursor - pair_left >= 80 * scale && pair_right - cursor >= 48 * scale;
                if (unconstrained)
                    check(std::abs(after.DividerX(divider) - cursor) < 0.1f, "dragged divider tracks the pointer in both directions");
                check(after.count == initial.count && after.kinds == initial.kinds, "dragging preserves column visibility");
                check(std::abs(after.cloud_left - after.Left(K::Cloud)) < 0.1f &&
                    std::abs(after.cloud_width - after.Width(K::Cloud)) < 0.1f, "status icon follows the same column geometry");
                s.renderer.AutoFitColumnDivider(body, pane.details_column_dividers, search, pane.search_column_dividers, divider, true);
                check((search ? pane.search_column_dividers[4] : pane.details_column_dividers[3]) ==
                    ((initial.kinds[divider] == K::Cloud || initial.kinds[divider + 1] == K::Cloud) ? 0.0f : initial.cloud_width / scale),
                    "double click resets the correct status width");
            }
            if (!search && (width == 540 || width == 1400)) for (bool dark : {false, true}) {
                s.compositor.Resize(static_cast<UINT>(window.right), static_cast<UINT>(window.bottom));
                auto* dc = s.compositor.Dc();
                dc->BeginDraw();
                s.renderer.Render(frame, window, ui::MakeTheme(dark, ui::HexColor(0x0078D4)));
                check(SUCCEEDED(dc->EndDraw()), "cloud columns render successfully");
                check(bench::SaveSnapshot(s.compositor, output / (std::to_wstring(static_cast<int>(width)) + L"-" +
                    std::to_wstring(static_cast<int>(scale * 100)) + (dark ? L"-dark.png" : L"-light.png"))), "save production cloud column frame");
            }
        }
    }
    s.scale = 1.0f;
    s.renderer.SetScale(1.0f);
    s.compositor.RecreateTextFormats(1.0f);
    s.compositor.Resize(1500, 900);
    for (int edge : {0, 1}) {
        tab->details_column_dividers = {};
        auto frame = BuildVm(s, false);
        const auto& slot = frame.pane_slots.front();
        const auto body = s.renderer.PaneBodyBounds(slot.pane, slot.rect);
        const auto initial = s.renderer.DetailsColumns(body, slot.pane);
        const int divider = initial.IndexOf(K::Cloud) - 1 + edge;
        const int x = static_cast<int>(std::lround(initial.DividerX(divider)));
        const int y = static_cast<int>(s.renderer.PaneListRect(slot.pane, body).top - s.renderer.ColumnHeaderHeight() * 0.5f);
        BYTE saved_keys[256]{};
        check(GetKeyboardState(saved_keys) != FALSE, "read fixture thread keyboard state");
        BYTE drag_keys[256]{};
        std::copy(std::begin(saved_keys), std::end(saved_keys), std::begin(drag_keys));
        drag_keys[VK_LBUTTON] |= 0x80;
        check(SetKeyboardState(drag_keys) != FALSE, "set fixture thread mouse button state for production drag handler");
        HandleLButtonDown(&s, s.hwnd, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(x, y));
        check(s.columnResizing && s.columnResizeIndex == divider, "production mouse down captures the requested status edge");
        HandleMouseMove(&s, s.hwnd, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(x + 12, y));
        const auto moved = s.renderer.DetailsColumns(body, BuildVm(s, false).pane_slots.front().pane);
        check(std::abs(moved.DividerX(divider) - (x + 12)) < 0.1f, "production mouse move drags the requested status edge");
        SetKeyboardState(saved_keys);
        HandleLButtonUp(&s, s.hwnd, WM_LBUTTONUP, 0, MAKELPARAM(x + 12, y));
        check(!s.columnResizing, "production mouse up ends column resizing");
        HandleLButtonDblClk(&s, s.hwnd, WM_LBUTTONDBLCLK, MK_LBUTTON, MAKELPARAM(x + 12, y));
        check(tab->details_column_dividers[3] == 0.0f, "production double click restores automatic status width");
    }
    DestroyWindow(s.hwnd);
    s.hwnd = nullptr;
    std::cout << "Cloud columns: " << checks << " checks, " << failures << " failures\n";
    return failures ? 1 : 0;
}
