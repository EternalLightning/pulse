#include "../ui/ui_renderer.h"
#include "../ui/ui_compositor.h"
#include "../common/localization.h"
#include "../common/windows_compat.h"
#include "../app/app_internal.h"
#include "../app/context_menu.h"
#include "../ui/ui_renderer_internal.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>

LRESULT CALLBACK WndProcImpl(HWND, UINT, WPARAM, LPARAM);
int RunTabIndicatorUiTest();

namespace {
int failures = 0;
void Check(bool ok, const char* message) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", message);
    if (!ok) ++failures;
}

bool HasBlueLine(pulse::ui::Compositor& compositor, D2D1_RECT_F cell, float scale) {
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
    const int center = static_cast<int>((cell.left + cell.right) * 0.5f);
    int blue_width = 0;
    const auto size = target->GetPixelSize();
    for (int y = std::max(0, static_cast<int>(cell.bottom - 6 * scale));
         y < std::min(static_cast<int>(size.height), static_cast<int>(cell.bottom - 3 * scale)); ++y) {
        const auto* pixels = mapped.bits + static_cast<size_t>(y) * mapped.pitch;
        int run = 0;
        for (int x = std::max(0, center - static_cast<int>(28 * scale));
             x < std::min(static_cast<int>(size.width), center + static_cast<int>(28 * scale)); ++x) {
            const auto* pixel = pixels + x * 4;
            if (pixel[0] > pixel[2] + 40 && pixel[1] > pixel[2] + 20) ++run;
            else run = 0;
            blue_width = std::max(blue_width, run);
        }
    }
    readback->Unmap();
    return blue_width >= static_cast<int>(44 * scale);
}

void InteractionChecks(const std::filesystem::path& output) {
    using namespace pulse;
    auto owned = std::make_unique<AppState>();
    auto& s = *owned;
    s.isolatedTest = true;
    s.accentColor = ui::HexColor(0x0078D4);
    s.appPrefs.persist = s.ctxMenuPrefs.persist = s.places.persist = false;
    s.shot.active = true;
    SetEnvironmentVariableW(L"PULSE_TEST_HIDDEN_SHOT", L"1");
    s.hwnd = CreateWindowExW(WS_EX_NOACTIVATE, L"STATIC", L"File interaction fixture",
        WS_POPUP, 0, 0, 960, 640, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!s.hwnd || !s.compositor.Init(s.hwnd)) {
        Check(false, "initialize production interaction window");
        return;
    }
    SetWindowLongPtrW(s.hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&s));
    s.compositor.RecreateTextFormats(s.scale);
    s.renderer.SetCompositor(&s.compositor);
    s.renderer.SetScale(s.scale);
    s.renderer.SetDetailsPanelVisible(false);
    s.settings.BindUi(s.appPrefs, s.ctxMenuPrefs, s.index, s.networkIndex, {});
    s.window_tabs.EnsureDefault();
    s.pane = s.window_tabs.Active()->FocusedPane();
    auto* tab = ActiveTab(s);
    tab->current_path = output.wstring();
    tab->view_mode = ui::ViewMode::LargeIcons;
    auto entries = std::make_shared<std::vector<fs::DirEntry>>(3);
    const wchar_t* names[] = {L"before.txt", L"after.txt", L"replacement.md"};
    for (int i = 0; i < 3; ++i) {
        auto& entry = (*entries)[static_cast<size_t>(i)];
        entry.name = names[i];
        entry.full_path = (output / names[i]).wstring();
        entry.attrs = FILE_ATTRIBUTE_NORMAL;
        std::ofstream file(std::filesystem::path(entry.full_path));
        file << "Pulse sample\nvalue = " << i << "\n";
    }
    tab->SetSnapshot(entries);
    tab->loading = false;
    auto click = [&](float x, float y) {
        const auto point = MAKELPARAM(static_cast<int>(x), static_cast<int>(y));
        WndProcImpl(s.hwnd, WM_LBUTTONDOWN, MK_LBUTTON, point);
        WndProcImpl(s.hwnd, WM_LBUTTONUP, 0, point);
    };
    auto click_file = [&](int index) {
        const auto vm = BuildVm(s, false);
        const auto& slot = vm.pane_slots.front();
        const auto list = s.renderer.PaneListRect(slot.pane, slot.rect);
        ui::ViewLayout layout(slot.pane.view_mode, list, slot.pane.EntryCount(), 0, 0, s.scale);
        const auto cell = layout.ItemRect(index);
        click((cell.left + cell.right) * 0.5f, cell.top + 24 * s.scale);
        Check(ActiveTab(s)->IsSelected(index) && ActiveTab(s)->SelectedCount() == 1,
            "production pointer message selects exactly the clicked text file");
    };
    auto has = [](const auto& items, int command) {
        return std::any_of(items.begin(), items.end(), [command](const auto& item) { return item.command == command; });
    };
    Render(s);
    click_file(0);
    auto items = BuildFinderItemMenu(s, false, L"");
    Check(has(items, app::CmdSelectTextCompare) && !has(items, app::CmdCompareSelectedText),
        "production context menu starts with only select-to-compare");
    ui::FluentMenu menu;
    if (menu.Create(s.hwnd, &s.compositor, s.scale)) {
        menu.SetTheme(s.darkMode, s.accentColor);
        Check(menu.SaveDebugSnapshot((output / L"menu-before-selection.png").c_str(), items),
            "capture actual initial file menu renderer");
    } else Check(false, "create production menu renderer");
    DispatchMenuCommand(s, app::CmdSelectTextCompare);
    click_file(2);
    DispatchMenuCommand(s, app::CmdSelectTextCompare);
    Check(s.text_compare_selection.source() == (*entries)[2].full_path,
        "production selection command replaces the first comparison file");
    click_file(1);
    items = BuildFinderItemMenu(s, false, L"");
    Check(has(items, app::CmdSelectTextCompare) && has(items, app::CmdCompareSelectedText),
        "production second-file menu offers both comparison commands");
    Check(menu.SaveDebugSnapshot((output / L"menu-after-selection.png").c_str(), items),
        "capture actual second-file menu renderer");
    DispatchMenuCommand(s, app::CmdCompareSelectedText);
    Check(s.text_diff && s.text_diff->visible(), "production compare command opens the native diff window");
    if (s.text_diff) s.text_diff->Close();

    OpenSettingsTab(s, 0);
    Check(s.text_compare_selection.source() == (*entries)[2].full_path,
        "selected comparison source survives opening another tab");
    auto vm = BuildVm(s, false);
    s.settings.SetScroll(s.renderer.SettingsDestinationOffset(vm,
        static_cast<int>(l10n::StringId::SettingsTextCompare), 960, 640),
        s.renderer.SettingsMaxScroll(vm, 960, 640));
    vm = BuildVm(s, false);
    const auto bounds = D2D1::RectF(0, 0, 960, 640);
    const auto layout = ui::MakeSettingsLayout(vm, bounds, s.scale,
        s.renderer.TitleBarHeight(), 28 * s.scale, nullptr);
    const auto row = layout.text_compare_row;
    const float tx = (row.left + row.right) * 0.5f, ty = (row.top + row.bottom) * 0.5f;
    const auto hit = s.renderer.HitTest(vm, bounds, tx, ty);
    Check(hit.region == ui::HitTestResult::SettingsToggle && hit.index == 33,
        "settings compare switch is visible and hit-testable after destination scroll");
    Render(s);
    Check(s.compositor.SaveSnapshot((output / L"settings-compare-on.png").c_str()),
        "capture comparison setting in production settings page");
    click(tx, ty);
    Check(!s.appPrefs.text_compare_enabled, "production settings pointer click disables comparison");
    Render(s);
    Check(s.compositor.SaveSnapshot((output / L"settings-compare-off.png").c_str()),
        "capture disabled comparison switch");
    s.window_tabs.active = 0;
    s.pane = s.window_tabs.Active()->FocusedPane();
    items = BuildFinderItemMenu(s, false, L"");
    Check(!has(items, app::CmdSelectTextCompare) && !has(items, app::CmdCompareSelectedText),
        "disabled setting removes both items from the actual file menu");
    s.watches.Stop();
    DestroyWindow(s.hwnd);
    s.hwnd = nullptr;
}
}

int RunSidebarStarredUiTest();
int RunWindowSizeLockUiTest();

int main(int argc, char** argv) {
    using namespace pulse;
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    OleInitialize(nullptr);
    compat::EnableDpiAwareness();
    l10n::Initialize(GetModuleHandleW(nullptr), L"zh-CN");
    if (argc == 2 && std::string_view(argv[1]) == "--tab-indicator-only") {
        const int result = RunTabIndicatorUiTest();
        OleUninitialize();
        return result;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--window-size-lock") {
        const int result = RunWindowSizeLockUiTest();
        OleUninitialize();
        return result;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--sidebar-starred") {
        const int result = RunSidebarStarredUiTest();
        OleUninitialize();
        return result;
    }
    const bool selection_only = argc == 2 && std::string_view(argv[1]) == "--selection-only";
    const auto output = std::filesystem::absolute(L"bench_data/file-selection-ui");
    std::filesystem::create_directories(output);
    const auto window = CreateWindowExW(WS_EX_NOACTIVATE, L"STATIC", L"Selection rendering fixture",
        WS_OVERLAPPEDWINDOW, 0, 0, 960, 640, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    {
        ui::Compositor compositor;
        if (!window || !compositor.Init(window)) {
            Check(false, "initialize production compositor");
            return 2;
        }
        ui::MainRenderer renderer;
        renderer.SetCompositor(&compositor);
        renderer.SetDetailsPanelVisible(false);
        ui::WindowViewModel vm;
        vm.window_title = L"Pulse";
        vm.tabs.push_back({L"MotionControl", true});
        vm.tabs.push_back({L"主页", false});
        vm.tabs.push_back({L"Documents", false});
        ui::PaneSlotView slot;
        slot.focused = true;
        slot.pane.header_controls_opacity = 0;
        for (const auto* name : {L"项目说明.txt", L"notes.md", L"source.cpp", L"数据.csv"}) {
            ui::ListEntryView entry;
            entry.name = name;
            entry.attrs = FILE_ATTRIBUTE_NORMAL;
            entry.type_text = L"文本文件";
            slot.pane.entries.push_back(std::move(entry));
        }
        vm.pane_slots.push_back(std::move(slot));
        auto draw = [&](const std::wstring& name, float scale) {
            compositor.Dc()->BeginDraw();
            renderer.Render(vm, D2D1::RectF(0, 0, 960 * scale, 640 * scale),
                ui::MakeTheme(vm.dark, ui::HexColor(0x0078D4)));
            Check(SUCCEEDED(compositor.Dc()->EndDraw()), "production renderer completes frame");
            if (!name.empty()) Check(compositor.SaveSnapshot((output / name).c_str()), "save rendered evidence");
        };
        for (const float scale : {1.0f, 1.5f}) {
            renderer.SetScale(scale);
            compositor.RecreateTextFormats(scale);
            compositor.Resize(static_cast<UINT>(960 * scale), static_cast<UINT>(640 * scale));
            vm.pane_slots[0].rect = D2D1::RectF(236 * scale, 94 * scale, 952 * scale, 606 * scale);
            for (const bool dark : {false, true}) {
                vm.dark = dark;
                for (const auto mode : {ui::ViewMode::MediumIcons, ui::ViewMode::LargeIcons,
                                        ui::ViewMode::ExtraLargeIcons}) {
                    auto& pane = vm.pane_slots[0].pane;
                    pane.view_mode = mode;
                    pane.selected_index = 0;
                    pane.selected_count = 1;
                    pane.all_selected = false;
                    pane.hover_index = 1;
                    vm.pane_slots[0].focused = true;
                    const auto name = std::to_wstring(static_cast<int>(mode)) + L"-" +
                        std::to_wstring(static_cast<int>(scale * 100)) + (dark ? L"-dark.png" : L"-light.png");
                    draw(name, scale);
                    const auto body = renderer.PaneListRect(pane, vm.pane_slots[0].rect);
                    ui::ViewLayout layout(mode, body, pane.entries.size(), 0, 0, scale);
                    Check(HasBlueLine(compositor, layout.ItemRect(0), scale),
                        "selected icon has bottom accent at each grid size, theme and DPI");
                    Check(!HasBlueLine(compositor, layout.ItemRect(1), scale),
                        "hover alone has no selection underline");
                    pane.all_selected = true;
                    pane.selected_count = static_cast<int>(pane.entries.size());
                    vm.pane_slots[0].focused = false;
                    draw(L"multi-inactive-" + name, scale);
                    Check(HasBlueLine(compositor, layout.ItemRect(0), scale) &&
                          HasBlueLine(compositor, layout.ItemRect(1), scale),
                        "multiple selection retains a subdued underline in an inactive pane");
                }
            }
        }
        if (!selection_only) {
            renderer.SetScale(1);
            compositor.RecreateTextFormats(1);
            compositor.Resize(960, 640);
            vm.pane_slots[0].rect = D2D1::RectF(236, 94, 952, 606);
            vm.pane_slots[0].pane.view_mode = ui::ViewMode::LargeIcons;
            vm.pane_slots[0].pane.all_selected = false;
            vm.pane_slots[0].pane.selected_count = 1;
            vm.pane_slots[0].pane.hover_index = -1;
            vm.pane_slots[0].focused = true;
            draw(L"tab-start.png", 1);
            vm.tabs[0].active = false;
            vm.tabs[1].active = true;
            draw(L"tab-switch.png", 1);
            BOOL animate = FALSE;
            SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animate, 0);
            if (animate) Check(renderer.TabIndicatorAnimating(), "tab switch schedules additional production frames");
            for (const int delay : {55, 55, 65, 80}) {
                Sleep(delay);
                draw(L"tab-" + std::to_wstring(delay) + L"-" +
                    std::to_wstring(GetTickCount64()) + L".png", 1);
            }
            Check(!renderer.TabIndicatorAnimating(), "tab underline settles and stops requesting frames");
        }
        renderer.SetCompositor(nullptr);
    }
    DestroyWindow(window);
    if (!selection_only) InteractionChecks(output);
    OleUninitialize();
    std::printf("selection rendering: %d failures\n", failures);
    return failures ? 1 : 0;
}
