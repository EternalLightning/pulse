#pragma once

#include "../app/cloud_root_catalog.h"

inline int RunCloudSidebarCases() {
    using namespace pulse;
    if (FAILED(OleInitialize(nullptr))) return 2;
    l10n::Initialize(GetModuleHandleW(nullptr), L"zh-CN");
    int failures = 0;
    const auto check = [&](bool ok, const char* name) {
        std::cout << (ok ? "[PASS] " : "[FAIL] ") << name << std::endl;
        if (!ok) ++failures;
    };
    const auto output = std::filesystem::absolute(L"docs/validation/cloud-sidebar-20261029");
    std::filesystem::create_directories(output);
    auto owned = std::make_unique<AppState>();
    auto& s = *owned;
    s.isolatedTest = true;
    s.shot.active = true;
    s.appPrefs.persist = s.places.persist = s.ctxMenuPrefs.persist = s.searchHistory.persist = false;
    s.showDetailsPanel = false;
    s.accentColor = ui::HexColor(0x0078D4);
    s.hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"STATIC", L"Cloud sidebar fixture",
        WS_POPUP, -20000, -20000, 1000, 700, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!s.hwnd || !s.compositor.Init(s.hwnd)) return 2;
    s.renderer.SetCompositor(&s.compositor);
    s.window_tabs.EnsureDefault();
    BindCurrentLayout(s);
    ActiveTab(s)->current_path = app::MakeHomePath();
    std::vector<app::CloudRoot> roots{
        {L"C:\\MiCloud", L"小米云盘", L"{00000000-0000-0000-0000-000000000000}"},
        {L"C:\\OneDriveWork", L"OneDrive - Nazo Game", L"OneDrive!sid!Work", app::CloudRootSource::SyncRoot},
        {L"C:\\OneDrive", L"OneDrive - Personal", {}, app::CloudRootSource::Seed, 0x42, true}};
    app::SortCloudRoots(roots);
    for (const auto& root : roots) s.sidebar.cloud.push_back(app::BuildCloudSidebarEntry(root));
    const auto frame = BuildVm(s, false);
    const int cloud_index = app::SidebarSectionIndex(frame, static_cast<int>(app::SidebarSectionId::Cloud));
    check(cloud_index >= 0, "cloud section reaches the production view model");
    if (cloud_index >= 0) {
        const auto& items = frame.sidebar[cloud_index].items;
        check(items.size() == 3 && items[0].label == L"OneDrive - Nazo Game" &&
            items[1].label == L"OneDrive - Personal" && items[2].label == L"小米云盘",
            "sidebar preserves provider grouping and account order");
        check(items.size() == 3 && !items[0].use_path_icon && !items[1].use_path_icon &&
            items[2].use_path_icon && items[2].icon_path == L"::{00000000-0000-0000-0000-000000000000}" &&
            items[2].path == L"C:\\MiCloud" && items[2].icon_glyph != L"\xE753",
            "Xiaomi uses its Shell namespace icon with a neutral fallback and keeps its navigation path");
    }
    const auto sync = app::BuildCloudSidebarEntry({L"C:\\OtherCloud", L"Other Cloud", L"Other!sid!Account",
        app::CloudRootSource::SyncRoot});
    check(sync.icon_path == sync.path && sync.glyph != L"\xE753",
        "sync providers without a namespace use their own folder icon");
    for (bool dark : {false, true}) for (float scale : {1.0f, 1.5f}) for (bool rail : {false, true}) {
        s.scale = scale;
        s.darkMode = dark;
        s.renderer.SetSidebarWidthDip(260.0f);
        s.renderer.SetScale(scale);
        s.compositor.RecreateTextFormats(scale);
        const int width = static_cast<int>((rail ? 600 : 1000) * scale), height = static_cast<int>(700 * scale);
        SetWindowPos(s.hwnd, nullptr, 0, 0, width, height, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        s.compositor.Resize(width, height);
        auto view = BuildVm(s, false);
        s.compositor.Dc()->BeginDraw();
        s.renderer.Render(view, D2D1::RectF(0, 0, static_cast<float>(width), static_cast<float>(height)),
            ui::MakeTheme(dark, s.accentColor));
        check(SUCCEEDED(s.compositor.Dc()->EndDraw()), "cloud sidebar renders in wide and rail layouts");
        const auto name = std::wstring(dark ? L"dark" : L"light") + (rail ? L"-rail-" : L"-wide-") +
            std::to_wstring(static_cast<int>(scale * 100)) + L".png";
        check(bench::SaveSnapshot(s.compositor, output / name), "cloud sidebar screenshot captured");
    }
    s.renderer.SetCompositor(nullptr);
    s.compositor.Shutdown();
    DestroyWindow(s.hwnd);
    s.hwnd = nullptr;
    owned.reset();
    OleUninitialize();
    return failures ? 1 : 0;
}
