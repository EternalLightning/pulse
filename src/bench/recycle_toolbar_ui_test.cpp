// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "../app/app_internal.h"
#include "../ui/toolbar_layout.h"
#include <cstdio>
#include <filesystem>

int RunRecycleToolbarUiTest() {
    using namespace pulse;
    int checks = 0, failures = 0;
    const auto check = [&](bool ok, const char* label) {
        ++checks;
        failures += !ok;
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
    };
    const auto output = std::filesystem::absolute(L"bench_data/recycle-toolbar-ui");
    std::filesystem::create_directories(output);
    auto owned = std::make_unique<AppState>();
    auto& s = *owned;
    s.isolatedTest = true;
    s.appPrefs.persist = s.places.persist = s.ctxMenuPrefs.persist = s.searchHistory.persist = false;
    s.accentColor = ui::HexColor(0x0078D4);
    s.hwnd = CreateWindowExW(WS_EX_NOACTIVATE, L"STATIC", L"Recycle toolbar fixture",
        WS_POPUP, -30000, -30000, 1100, 720, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!s.hwnd || !s.compositor.Init(s.hwnd)) return 1;
    s.renderer.SetCompositor(&s.compositor);
    s.renderer.SetDetailsPanelVisible(false);
    ShowWindow(s.hwnd, SW_SHOWNOACTIVATE);
    SetEnvironmentVariableW(L"PULSE_TEST_HIDDEN_SHOT", L"1");
    s.window_tabs.NewTab(app::MakeRecyclePath());
    s.pane = s.window_tabs.Active()->FocusedPane();
    s.pane->filter_expand = 1.0f;
    auto* tab = ActiveTab(s);
    const auto empty_entries = std::make_shared<std::vector<fs::DirEntry>>();
    const auto populated_entries = std::make_shared<std::vector<fs::DirEntry>>(2);
    (*populated_entries)[0].name = L"example.txt";
    (*populated_entries)[0].full_path = L"C:\\RecycleToolbarFixture\\example.txt";
    (*populated_entries)[0].size = 16;
    (*populated_entries)[1].name = L"example-folder";
    (*populated_entries)[1].full_path = L"C:\\RecycleToolbarFixture\\example-folder";
    (*populated_entries)[1].attrs = FILE_ATTRIBUTE_DIRECTORY;
    // Leave OpsManager stopped: pointer dispatch can be checked without touching any real recycled files.
    for (const wchar_t* language : {L"zh-CN", L"en-US"}) {
        l10n::Initialize(GetModuleHandleW(nullptr), language);
        tab->banner_title = l10n::Get(l10n::StringId::RecycleBin);
        for (float scale : {1.0f, 1.5f}) for (bool dark : {false, true}) {
            s.scale = scale;
            s.darkMode = dark;
            s.renderer.SetScale(scale);
            s.compositor.RecreateTextFormats(scale);
            for (int width : {1100, 640, 380, 300}) {
                s.compositor.Resize(static_cast<UINT>(width * scale), static_cast<UINT>(720 * scale));
                const auto rect = D2D1::RectF(0, 0, width * scale, 720 * scale);
                const float left = s.renderer.EffectiveSidebarWidth(rect.right);
                const auto layout = ui::MakeToolbarLayout(rect.right, scale, s.renderer.TitleBarHeight(),
                    s.renderer.Margin(), s.renderer.NewButtonWidthPx(rect.right - left < 600 * scale),
                    left, 1.0f, false, true);
                const bool visible = layout.empty_recycle.right > layout.empty_recycle.left;
                check(!visible || (layout.empty_recycle.left >= layout.create.right &&
                    layout.empty_recycle.right + 8 * scale <= layout.sort.left),
                    "recycle action fits between commands and expanded filter without overlap");
                for (int state : {0, 1, 2, 3}) {
                    tab->loading = state == 2;
                    s.recycle_info.valid = state != 3;
                    s.recycle_info.items = state == 0 ? 0 : 2;
                    s.recycle_info.bytes = state == 0 ? 0 : 16;
                    tab->SetSnapshot(state == 0 ? empty_entries : populated_entries);
                    tab->banner_message = RecycleOccupancyText(s.recycle_info);
                    auto vm = BuildVm(s, false);
                    check(vm.pane.is_recycle && vm.can_empty_recycle == (state == 1 || state == 3),
                        "empty/loading disable clearing; populated/unknown occupancy enable it");
                    const float x = (layout.empty_recycle.left + layout.empty_recycle.right) * 0.5f;
                    const float y = (layout.empty_recycle.top + layout.empty_recycle.bottom) * 0.5f;
                    if (visible) {
                        const auto hit = s.renderer.HitTest(vm, rect, x, y);
                        check((hit.region == ui::HitTestResult::EmptyRecycle) == vm.can_empty_recycle,
                            "production hit test matches the rendered recycle action state");
                        if (width == 1100 && !vm.can_empty_recycle) {
                            s.hitTestVmValid = false;
                            const auto before = s.ops.TaskStatuses().size();
                            HandleLButtonDown(&s, s.hwnd, WM_LBUTTONDOWN, MK_LBUTTON,
                                MAKELPARAM(static_cast<int>(x), static_cast<int>(y)));
                            check(s.ops.TaskStatuses().size() == before,
                                "clicking an empty or loading recycle action queues no operation");
                        }
                    }
                    s.compositor.Dc()->BeginDraw();
                    s.renderer.Render(vm, rect, ui::MakeTheme(dark, s.accentColor));
                    check(SUCCEEDED(s.compositor.Dc()->EndDraw()), "recycle toolbar renders successfully");
                    if (state == 0 || state == 1) {
                        const auto name = std::wstring(language) + L"-" + std::to_wstring(width) + L"-" +
                            std::to_wstring(static_cast<int>(scale * 100)) + (dark ? L"-dark" : L"-light") +
                            (state == 0 ? L"-empty.png" : L"-populated.png");
                        check(s.compositor.SaveSnapshot((output / name).c_str()), "save production renderer evidence");
                    }
                }
                if (width == 1100) {
                    tab->loading = false;
                    s.recycle_info.bytes = 16;
                    s.recycle_info.items = 2;
                    s.recycle_info.valid = true;
                    s.hitTestVmValid = false;
                    const auto before = s.ops.TaskStatuses().size();
                    HandleLButtonDown(&s, s.hwnd, WM_LBUTTONDOWN, MK_LBUTTON,
                        MAKELPARAM(static_cast<int>((layout.empty_recycle.left + layout.empty_recycle.right) * 0.5f),
                            static_cast<int>((layout.empty_recycle.top + layout.empty_recycle.bottom) * 0.5f)));
                    const auto tasks = s.ops.TaskStatuses();
                    check(tasks.size() == before + 1 && tasks.back().type == ops::OpType::EmptyRecycle &&
                        tasks.back().phase == ops::OpPhase::Queued,
                        "production pointer dispatch queues the existing EmptyRecycle operation without executing it");
                }
            }
        }
    }
    tab->current_path = L"C:\\fixture";
    const auto folder_vm = BuildVm(s, false);
    check(!folder_vm.pane.is_recycle && !folder_vm.can_empty_recycle,
        "switching to an ordinary folder removes the recycle action");
    s.renderer.SetCompositor(nullptr);
    s.compositor.Shutdown();
    DestroyWindow(s.hwnd);
    s.hwnd = nullptr;
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
