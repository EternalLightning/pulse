// SPDX-License-Identifier: Apache-2.0
#include "../app/home_catalog.h"
#include "../app/app_model.h"
#include "../app/command_catalog.h"
#include "../app/context_menu.h"
#include "../fs/shell_namespace.h"
#include "../ui/toolbar_layout.h"
#include "../ui/home_layout.h"
#include <filesystem>
#include <cstdio>

namespace {
int passed = 0, failed = 0;
void Check(bool value, const char* text) {
    std::printf("[%s] %s\n", value ? "PASS" : "FAIL", text);
    value ? ++passed : ++failed;
}
}
int main() {
    using namespace pulse;
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    OleInitialize(nullptr);
    l10n::Initialize(GetModuleHandleW(nullptr), L"zh-CN");
    const auto root = std::filesystem::absolute(L"../bench_data/home-toolbar-2026-11-17");
    std::filesystem::create_directories(root);
    for (const float scale : {1.0f, 1.75f, 2.0f}) for (const float width : {540.0f, 760.0f, 1400.0f}) {
        const float left = (width < 960 ? 64 : 224)*scale;
        for (const float expand : {0.0f, 1.0f}) {
            const auto layout = ui::MakeToolbarLayout(width*scale, scale, 44*scale, 4*scale, 80*scale, left, expand);
            Check(layout.sort.right + 8*scale == layout.group.left &&
                layout.group.right + 8*scale == layout.filter.left &&
                layout.filter.right + 8*scale == layout.overflow.left,
                "sort, group, filter and customize share consistent action spacing at every DPI");
            if (expand == 0) Check(layout.filter.right-layout.filter.left ==
                ((width*scale-left >= 700*scale) ? 88 : 32)*scale,
                "collapsed filter has compact padding while icon-only layout remains usable");
        }
    }
    app::HomeCatalog home;
    home.Refresh();
    ui::PaneViewModel home_pane;
    home_pane.is_home = true;
    const auto deadline = GetTickCount64() + 8000;
    do { home.Fill(home_pane); if (!home_pane.home_cards.empty()) break; Sleep(10); } while (GetTickCount64() < deadline);
    const auto recycle = std::find_if(home_pane.home_cards.begin(), home_pane.home_cards.end(),
        [](const auto& card) { return card.path == app::MakeRecyclePath(); });
    const auto library = std::find_if(home_pane.home_cards.begin(), home_pane.home_cards.end(),
        [](const auto& card) { return card.path == fs::kLibrariesPath; });
    Check(recycle != home_pane.home_cards.end() && library != home_pane.home_cards.end() && recycle+1 == library,
        "production home catalog places Recycle Bin immediately before Libraries");
    std::vector<fs::DirEntry> libraries;
    const auto error = fs::EnumerateShellNamespace(fs::kLibrariesPath, libraries);
    Check(SUCCEEDED(error) && !libraries.empty() && std::all_of(libraries.begin(), libraries.end(), [](const auto& row) {
        return fs::IsLibraryPath(row.full_path) && !row.shell_id.empty();
    }), "production library namespace publishes real navigable rows for this Windows profile");
    std::printf("[INFO] native library rows: %zu\n", libraries.size());
    HWND owner = CreateWindowExW(WS_EX_NOACTIVATE, L"STATIC", L"Pulse home and filter fixture", WS_POPUP,
        -30000, -30000, 1400, 800, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    ui::Compositor compositor;
    ui::MainRenderer renderer;
    Check(owner && compositor.Init(owner), "initialize isolated production home and filter renderer");
    if (compositor.Dc()) {
        renderer.SetCompositor(&compositor);
        auto render = [&](ui::WindowViewModel vm, float scale, int dip_width, const wchar_t* filename) {
            const int width = static_cast<int>(dip_width*scale), height = static_cast<int>(760*scale);
            SetWindowPos(owner, nullptr, -30000, -30000, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
            compositor.Resize(width, height); compositor.RecreateTextFormats(scale); renderer.SetScale(scale);
            renderer.SetToolbarOrder(vm.toolbar_order);
            vm.safe_mode = true; vm.pane.header_controls_opacity = 1; vm.pane.focused = true;
            for (int frame = 0; frame < 3; ++frame) {
                auto* dc = compositor.Dc(); dc->BeginDraw(); dc->Clear(D2D1::ColorF(0, 0));
                renderer.Render(vm, D2D1::RectF(0, 0, static_cast<float>(width), static_cast<float>(height)),
                    ui::MakeTheme(vm.dark, ui::HexColor(0x0078D4)));
                Check(SUCCEEDED(dc->EndDraw()), "render home, library or filter state with production controls");
                if (frame < 2) Sleep(120);
            }
            Check(compositor.SaveSnapshot((root / filename).c_str()), "save targeted home and filter screenshot");
            return vm;
        };
        ui::WindowViewModel vm;
        vm.toolbar_order.assign(ui::kDefaultToolbarOrder.begin(), ui::kDefaultToolbarOrder.end());
        app::CommandContext context; context.filesystem_folder = true;
        for (const int command : vm.toolbar_order) {
            const auto item = app::BuildCommandMenuItem(command, context);
            vm.toolbar_commands.push_back({command, item.text, item.glyph, item.enabled, item.pictogram});
        }
        vm.pane.path = root.wstring(); vm.pane.header_text = L"Projects";
        vm.pane.snapshot = std::make_shared<const std::vector<fs::DirEntry>>();
        vm.pane.view_mode = ui::ViewMode::Details;
        vm.can_group = true;
        for (const bool english : {false, true}) {
            l10n::SetLanguage(english ? L"en-US" : L"zh-CN");
            vm.dark = !english;
            const float scale = english ? 1.0f : 1.75f;
            vm.hover_region = ui::HitTestResult::FilterBox;
            vm.pane.filter_expand = 0; vm.pane.filter_text.clear();
            render(vm, scale, 1400, english ? L"filter-en-light-collapsed.png" : L"filter-zh-dark-collapsed.png");
            vm.pane.filter_expand = 1; vm.pane.filter_text = L"*.png";
            const auto rendered = render(vm, scale, 1400, english ? L"filter-en-light-expanded.png" : L"filter-zh-dark-expanded.png");
            const auto box = renderer.FilterBoxRect(D2D1::RectF(0, 0, 1400*scale, 760*scale), 1);
            const auto clear = renderer.FilterClearRect(D2D1::RectF(0, 0, 1400*scale, 760*scale), 1);
            const auto hit = renderer.HitTest(rendered, D2D1::RectF(0, 0, 1400*scale, 760*scale),
                (clear.left+clear.right)/2, (clear.top+clear.bottom)/2);
            Check(box.right-box.left > 160*scale && hit.region == ui::HitTestResult::FilterClear,
                "expanded filter retains text input room and a matching clear target");
        }
        l10n::SetLanguage(L"zh-CN"); vm.dark = true; vm.hover_region = ui::HitTestResult::None;
        vm.pane.filter_expand = 0; vm.pane.filter_text.clear();
        render(vm, 2, 540, L"filter-zh-dark-compact-200.png");
        vm.pane = home_pane; vm.pane.path = app::MakeHomePath(); vm.pane.header_text = l10n::Get(l10n::StringId::Home);
        render(vm, 1.75f, 1400, L"home-zh-dark.png");
        const auto cards = ui::MakeHomeLayout(vm.pane, renderer.ContentRect(1400*1.75f, 760*1.75f), 1.75f);
        Check(cards.cards.size() >= 8, "home cards remain laid out after reordering");
        vm.pane = {};
        vm.pane.path = fs::kLibrariesPath;
        vm.pane.header_text = l10n::Get(l10n::StringId::ShellLibraries);
        vm.pane.view_mode = ui::ViewMode::Details;
        vm.pane.details_columns = {true, {model::DetailsColumnKind::Name}, {}};
        vm.pane.snapshot = std::make_shared<const std::vector<fs::DirEntry>>(libraries);
        render(vm, 1.75f, 1400, L"libraries-zh-dark.png");
        renderer.SetCompositor(nullptr); compositor.Shutdown();
    }
    if (owner) DestroyWindow(owner);
    OleUninitialize();
    std::printf("%d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
