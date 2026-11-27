// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution.

#include "../app/app_internal.h"
#include "../app/app_column_view.h"
#include "column_navigation_cases.h"
#include "column_scroll_cases.h"
#include <string_view>

static bool RunColumnProjectionChecks(pulse::AppState& s) {
    using namespace pulse;
    int failures = 0;
    const auto check = [&](bool ok, const char* label) {
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
        if (!ok) ++failures;
    };
    auto& tab = *s.pane->ActiveTab();
    tab.current_path = fs::NormalizePath(L"C:\\column-projection-fixture\\child");
    tab.column_layout = true;
    tab.column_strip.for_path = tab.current_path;
    auto& column = tab.column_strip.ancestors.emplace_back();
    column.path = fs::ParentPath(tab.current_path);
    column.requested = true;
    auto rows = std::make_shared<std::vector<fs::DirEntry>>(20'000);
    for (size_t i = 0; i < rows->size(); ++i) {
        auto& entry = (*rows)[i];
        entry.name = L"file-" + std::to_wstring(i);
        entry.size = i;
        entry.attrs = FILE_ATTRIBUTE_NORMAL;
    }
    column.snapshot = rows;
    tab.sort_column = ui::SortColumn::Size;
    if (!s.worker.Start([](app::WorkResult) {})) return false;
    const HANDLE release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!release) return false;
    const bool blocked = s.worker.EnqueueIo([release] { WaitForSingleObject(release, 3000); }, {},
        app::WorkerPool::IoLane::Local);
    SyncColumnStrips(s);
    check(blocked && !column.rows && column.pending_rows, "large column sorting leaves the UI before publishing rows");
    tab.sort_direction = ui::SortDirection::Desc;
    SyncColumnStrips(s);
    SetEvent(release);
    const auto wait = [&](auto ready) {
        const auto deadline = GetTickCount64() + 5000;
        do {
            SyncColumnStrips(s);
            if (ready()) return true;
            Sleep(5);
        } while (GetTickCount64() < deadline);
        return false;
    };
    if (!wait([&] { return column.rows && column.rows_sort_direction == ui::SortDirection::Desc; })) {
        check(false, "latest column projection settles");
    } else {
        check(column.rows->front() == static_cast<int>(rows->size() - 1) && column.rows_snapshot == rows,
            "superseded sort does not publish and the drawing uses the matching snapshot");
        column.auto_scroll = false;
        const float row_dip = s.renderer.RowHeight() / s.scale;
        column.scroll_dip = 10.0f * row_dip + 1.0f;
        tab.sort_direction = ui::SortDirection::Asc;
        const bool settled = wait([&] { return !column.pending_rows &&
            column.rows_sort_direction == ui::SortDirection::Asc; });
        const auto top = static_cast<size_t>(column.scroll_dip / row_dip);
        check(settled && top < column.rows->size() &&
            (*column.rows)[top] == static_cast<int>(rows->size() - 11),
            "changing column order preserves the first visible file anchor");
    }
    auto filtered_rows = std::make_shared<std::vector<fs::DirEntry>>(*rows);
    filtered_rows->front().attrs = FILE_ATTRIBUTE_HIDDEN;
    tab.SetShowHiddenFiles(false);
    tab.SetSnapshot(filtered_rows);
    tab.filter_text = L"file";
    tab.scroll_y = 200.0f;
    s.scrollTargetY = 220.0f;
    ClampScroll(s);
    check(tab.pending_view_projection && tab.scroll_y == 200.0f && s.scrollTargetY == 220.0f,
        "waiting for filtered rows preserves the last scroll coordinates");
    s.worker.Stop();
    CloseHandle(release);
    return failures == 0;
}

// Link production objects with scripts/test_integration_fixture.ps1.
int main(int argc, char** argv) {
    using namespace pulse;
    ConfigureAnimationTestOutput();
    OleInitialize(nullptr);
    l10n::Initialize(GetModuleHandleW(nullptr), L"en-US");
    auto owned = std::make_unique<AppState>();
    auto& s = *owned;
    s.isolatedTest = true;
    s.appPrefs.persist = s.places.persist = s.ctxMenuPrefs.persist = s.searchHistory.persist = false;
    s.shot.active = true;
    SetEnvironmentVariableW(L"PULSE_TEST_HIDDEN_SHOT", L"1");
    s.hwnd = CreateWindowExW(WS_EX_NOACTIVATE, L"STATIC", L"Column navigation fixture",
        WS_POPUP, -30000, -30000, 760, 540, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!s.hwnd || !s.compositor.Init(s.hwnd)) return 2;
    SetWindowLongPtrW(s.hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&s));
    s.compositor.RecreateTextFormats(s.scale);
    s.renderer.SetCompositor(&s.compositor);
    s.renderer.SetScale(s.scale);
    s.window_tabs.EnsureDefault();
    s.pane = s.window_tabs.Active()->FocusedPane();
    const auto output = std::filesystem::absolute(L"../bench_data/issue-review-2026-11-14/columns");
    std::filesystem::create_directories(output);
    const std::string_view mode = argc == 2 ? argv[1] : "";
    const bool passed = mode == "--projection-only" ? RunColumnProjectionChecks(s)
        : mode == "--scroll-only" ? RunColumnScrollChecks(s, output) : RunColumnNavigationChecks(s, output);
    s.watches.Stop();
    s.renderer.SetCompositor(nullptr);
    s.compositor.Shutdown();
    DestroyWindow(s.hwnd);
    s.hwnd = nullptr;
    owned.reset();
    OleUninitialize();
    return passed ? 0 : 1;
}
