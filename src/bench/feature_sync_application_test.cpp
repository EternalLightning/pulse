// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "../app/app_internal.h"
#include "../app/app_network_locations.h"
#include "../app/network_locations.h"
#include "../app/cloud_roots.h"
#include "../app/cloud_commands.h"
#include "../ui/ui_renderer_internal.h"
#include "../ui/cloud_status.h"
#include "snapshot_output.h"
#include <cstdio>
#include <filesystem>
#include <iostream>

#include "settings_categories_cases.h"
#include "settings_page_cases.h"
#include "settings_input_cases.h"
#include "cloud_columns_cases.h"
#include "cloud_sidebar_cases.h"
#include "native_material_layer_cases.h"

namespace {
int CheckFileDrag(pulse::AppState& s) {
    using namespace pulse;
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    int failures = 0;
    const auto check = [&](bool ok, const char* name) {
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
        failures += !ok;
    };
    auto& tab = *ActiveTab(s);
    tab.current_path = fs::NormalizePath(LR"(C:\PulseDragFixture)");
    tab.loading = false;
    tab.view_mode = ui::ViewMode::Details;
    auto rows = std::make_shared<std::vector<fs::DirEntry>>(2);
    auto& folder = rows->front();
    folder.name = L"Folder";
    folder.is_dir = true;
    folder.attrs = FILE_ATTRIBUTE_DIRECTORY;
    rows->back().name = L"ordinary file name.txt";
    tab.SetSnapshot(rows);
    auto vm = BuildVm(s, false);
    const auto window = D2D1::RectF(0, 0, static_cast<float>(s.compositor.Width()),
        static_cast<float>(s.compositor.Height()));
    const auto pane = s.renderer.ContentRect(window.right, window.bottom);
    POINT folder_point{}, file_point{}, blank_point{};
    const auto x = static_cast<LONG>(pane.left + 40 * s.scale);
    for (LONG y = static_cast<LONG>(pane.top + s.renderer.PaneHeaderHeight()); y < pane.bottom; ++y) {
        const auto hit = s.renderer.HitTest(vm, window, static_cast<float>(x), static_cast<float>(y));
        if (hit.region == ui::HitTestResult::Row && hit.index == 0) folder_point = {x, y};
        if (hit.region == ui::HitTestResult::Row && hit.index == 1) file_point = {x, y};
        if (hit.region == ui::HitTestResult::Pane && hit.pane_index >= 0) blank_point = {x, y};
    }
    check(folder_point.y && file_point.y && blank_point.y, "hit-test finds both items and the pane background");
    if (folder_point.y && file_point.y && blank_point.y) {
        const std::wstring source_folder = LR"(c:\pulsedragfixture\folder)";
        const std::wstring source_file = LR"(C:\PulseDragFixture\ordinary file name.txt)";
        const DWORD allowed = DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK;
        const auto ignored = [&](const std::vector<std::wstring>& sources, POINT point, DWORD keys) {
            return ResolveDropTarget(s, sources, point, keys, allowed) == DROPEFFECT_NONE &&
                s.dropBadge.empty() && s.dropRow == -1 && s.springRow == -1 &&
                DropExecute(s, sources, point, keys, allowed, 0) == DROPEFFECT_NONE;
        };
        s.springRow = 0;
        s.springStart = GetTickCount64() - 1000;
        check(ignored({source_file, source_folder}, folder_point, 0) && !s.springEntered,
            "folder within the dragged selection rejects itself before spring navigation");
        check(ignored({source_file}, file_point, 0), "file rejects a drop onto its own row");
        check(ignored({source_folder, source_file}, blank_point, MK_CONTROL),
            "the source directory's blank space rejects an explicit copy");
        check(ResolveDropTarget(s, {source_file}, folder_point, 0, allowed) == DROPEFFECT_MOVE &&
            s.dropRow == 0 && !s.dropBadge.empty() && s.springRow == 0,
            "another folder still accepts the drag and displays its action");
        check(ignored({source_file}, blank_point, 0),
            "moving back to a redundant target clears the previous folder feedback");
        check(ResolveDropTarget(s, {LR"(C:\PulseOtherFixture\new.txt)"}, blank_point, MK_CONTROL, allowed)
            == DROPEFFECT_COPY, "blank space accepts a file from a different folder");
        ClearDropFeedback(s);
    }

    const auto theme = ui::MakeTheme(false, s.accentColor);
    bool drew_icon = false, drew_name = false;
    const auto bitmap_info = [&](ui::ViewMode mode) {
        vm.pane.view_mode = mode;
        const HBITMAP preview = s.renderer.CreateDragPreview(vm.pane, 1, theme);
        BITMAP bitmap{};
        if (preview) {
            GetObjectW(preview, sizeof(bitmap), &bitmap);
            if (mode == ui::ViewMode::Details && bitmap.bmBits && bitmap.bmWidth > 35 && bitmap.bmHeight == 28) {
                const auto* pixels = static_cast<const uint32_t*>(bitmap.bmBits);
                const auto background = pixels[14 * bitmap.bmWidth + 25];
                for (LONG y = 6; y < 22; ++y) {
                    const auto* line = pixels + y * bitmap.bmWidth;
                    drew_icon |= std::any_of(line + 6, line + 22, [&](auto pixel) { return pixel != background; });
                    drew_name |= std::any_of(line + 28, line + bitmap.bmWidth - 6,
                        [&](auto pixel) { return pixel != background; });
                }
            }
            DeleteObject(preview);
        }
        return bitmap;
    };
    const auto details = bitmap_info(ui::ViewMode::Details);
    const auto list = bitmap_info(ui::ViewMode::List);
    const auto icon = bitmap_info(ui::ViewMode::LargeIcons);
    check(details.bmWidth > 3 * details.bmHeight && details.bmHeight > 0 &&
        list.bmWidth == details.bmWidth && list.bmHeight == details.bmHeight,
        "Details and List produce a compact icon-and-name bitmap");
    check(drew_icon, "Details preview paints the icon with a cold Shell icon cache");
    check(drew_name, "Details preview paints the filename");
    check(icon.bmWidth == icon.bmHeight && icon.bmHeight > details.bmHeight,
        "Large Icons produces a larger square bitmap without a filename column");
    vm.pane.filter_map = std::make_shared<const std::vector<int>>(std::vector<int>{1});
    check(bitmap_info(ui::ViewMode::Details).bmWidth == details.bmWidth && details.bmWidth > 0,
        "filtered results use the selected item's source index for the preview");
    return failures ? 1 : 0;
}
}

int main(int argc, char** argv) {
    if (argc > 1 && std::string_view(argv[1]) == "--native-material-layers-only")
        return RunNativeMaterialLayerCases();
    if (argc > 1 && std::string_view(argv[1]) == "--settings-input-only")
        return RunSettingsInputCases();
    if (argc > 1 && std::string_view(argv[1]) == "--settings-transfer-only")
        return RunSettingsPageCases(true);
    if (argc > 1 && std::string_view(argv[1]) == "--settings-page-only")
        return RunSettingsPageCases();
    if (argc > 1 && std::string_view(argv[1]) == "--window-effect-preview-only")
        return RunSettingsPageCases(false, true);
    if (argc > 1 && std::string_view(argv[1]) == "--cloud-sidebar-only")
        return RunCloudSidebarCases();
    if (argc > 1 && std::string_view(argv[1]) == "--cloud-columns-only")
        return RunCloudColumnsCases();
    if(argc>1 && std::string_view(argv[1])=="--settings-categories") {
        try { return RunSettingsCategories(); }
        catch(const std::exception& error) { std::cerr<<"[FAIL] settings fixture: "<<error.what()<<std::endl;return 2; }
    }
    using namespace pulse;
    const bool file_drag_only = argc == 2 && std::string_view(argv[1]) == "--file-drag-only";
    if (FAILED(OleInitialize(nullptr))) return 2;
    l10n::Initialize(GetModuleHandleW(nullptr), L"zh-CN");
    const auto output = std::filesystem::absolute(L"docs/validation/upstream-features-20261028/screenshots");
    if (!file_drag_only) {
        std::filesystem::create_directories(output);
        SetEnvironmentVariableW(L"PULSE_TEST_DATA_DIR", output.c_str());
        SetEnvironmentVariableW(L"PULSE_TEST_CLOUD_SYNC", L"仅在线文档.txt=0;本地可用文档.txt=0;始终保留的文档.txt=0");
    }
    int failures = 0;
    const auto check = [&](bool ok, const char* name) {
        std::cout << (ok ? "[PASS] " : "[FAIL] ") << name << std::endl;
        if (!ok) ++failures;
    };
    auto owned = std::make_unique<AppState>();
    auto& state = *owned;
    state.isolatedTest = true;
    state.shot.active = true;
    state.appPrefs.persist = state.places.persist = state.ctxMenuPrefs.persist = state.searchHistory.persist = false;
    state.showDetailsPanel = false;
    state.accentColor = ui::HexColor(0x0078D4);
    state.hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"STATIC", L"Feature sync fixture",
        WS_POPUP, -20000, -20000, 1200, 900, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!state.hwnd || !state.compositor.Init(state.hwnd)) return 2;
    state.renderer.SetCompositor(&state.compositor);
    state.window_tabs.EnsureDefault();
    BindCurrentLayout(state);
    if (file_drag_only) {
        SetWindowLongPtrW(state.hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&state));
        state.compositor.RecreateTextFormats(state.scale);
        const int result = CheckFileDrag(state);
        state.renderer.SetCompositor(nullptr);
        state.compositor.Shutdown();
        DestroyWindow(state.hwnd);
        state.hwnd = nullptr;
        owned.reset();
        OleUninitialize();
        return result;
    }
    auto* tab = ActiveTab(state);
    tab->current_path = L"C:\\PulseFixtureCloud";
    tab->view_mode = ui::ViewMode::Details;
    tab->loading = false;
    auto rows = std::make_shared<std::vector<fs::DirEntry>>(3);
    (*rows)[0].name = L"仅在线文档.txt";
    (*rows)[0].attrs = FILE_ATTRIBUTE_OFFLINE | fs::kAttrRecallOnDataAccess;
    (*rows)[1].name = L"本地可用文档.txt";
    (*rows)[1].attrs = FILE_ATTRIBUTE_ARCHIVE;
    (*rows)[2].name = L"始终保留的文档.txt";
    (*rows)[2].attrs = fs::kAttrPinned;
    FILETIME now{};
    GetSystemTimeAsFileTime(&now);
    const uint64_t ticks = (static_cast<uint64_t>(now.dwHighDateTime) << 32) | now.dwLowDateTime;
    for (size_t i = 0; i < rows->size(); ++i) {
        auto& row = (*rows)[i];
        row.size = 1024 * (i + 1);
        const uint64_t time = ticks - (i + 2) * 864000000000ULL;
        row.mtime = {static_cast<DWORD>(time), static_cast<DWORD>(time >> 32)};
    }
    tab->SetSnapshot(rows);
    tab->SelectOnly(0);
    app::PublishCloudFilesRoots({tab->current_path});
    std::vector<ui::FluentMenuItem> menu;
    AppendCloudCommands(state, *tab, menu);
    const auto has = [&](int command) {
        return std::any_of(menu.begin(), menu.end(), [&](const auto& item) { return item.command == command; });
    };
    check(has(app::CmdCloudKeep) && has(app::CmdCloudFree) && has(app::CmdCloudDownload),
        "offline cloud selection exposes keep, free and explicit download commands");
    for (bool dark : {false, true}) for (float scale : {1.0f, 1.5f}) {
        const int width = scale > 1 ? 1000 : 1200;
        const int height = 900;
        state.scale = scale;
        state.darkMode = dark;
        state.renderer.SetScale(scale);
        state.compositor.RecreateTextFormats(scale);
        SetWindowPos(state.hwnd, nullptr, 0, 0, width, height, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        state.compositor.Resize(width, height);
        auto frame = BuildVm(state, false);
        const auto rect = D2D1::RectF(0, 0, static_cast<float>(width), static_cast<float>(height));
        const auto theme = ui::MakeTheme(dark, state.accentColor);
        const auto prefix = std::wstring(dark ? L"dark-" : L"light-") + std::to_wstring(static_cast<int>(scale * 100));
        const auto draw = [&](const wchar_t* name) {
            state.compositor.Dc()->BeginDraw();
            state.renderer.Render(frame, rect, theme);
            check(SUCCEEDED(state.compositor.Dc()->EndDraw()), "feature frame draws successfully");
            check(bench::SaveSnapshot(state.compositor, output / (prefix + L"-" + name + L".png")),
                "feature screenshot captured");
        };
        const auto pane = state.renderer.ContentRect(rect.right, rect.bottom);
        check(frame.pane.cloud_status_column, "cloud root enables the production status column");
        const auto columns = state.renderer.DetailsColumns(pane, frame.pane);
        check(columns.cloud_width > 0 && columns.IndexOf(ui::MainRenderer::ColumnKind::Cloud) <
            columns.IndexOf(ui::MainRenderer::ColumnKind::Date),
            "cloud status appears before the modified date column");
        draw(L"cloud");
        POINT point{static_cast<LONG>(pane.right - 50 * scale), static_cast<LONG>(pane.bottom - 80 * scale)};
        const std::vector<std::wstring> sources{L"C:\\PulseSource\\file.txt"};
        const DWORD all = DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK;
        check(ResolveDropTarget(state, sources, point, 0x20, all) == DROPEFFECT_LINK &&
            !state.dropHeader && state.dropDestDir == tab->current_path,
            "Alt hover resolves to a shortcut in the destination pane");
        check(ResolveDropTarget(state, sources, point, MK_CONTROL | MK_SHIFT, all) == DROPEFFECT_LINK,
            "Ctrl+Shift hover preserves shortcut semantics");
        check(ResolveDropTarget(state, sources, point, MK_CONTROL, all) == DROPEFFECT_COPY,
            "Ctrl hover preserves explicit copying");
        ClearDropFeedback(state);
        frame.settings_open = true;
        frame.settings_page = 8;
        frame.settings_scroll = 0;
        frame.settings_transfer_status = l10n::Get(l10n::StringId::SettingsTransferDesc);
        frame.settings_transfer_pending = true;
        ui::fluent::Painter measuring(&state.compositor);
        measuring.SetScale(scale);
        measuring.BeginFrame(theme);
        auto settings = ui::MakeSettingsLayout(frame, rect, scale, state.renderer.TitleBarHeight(), 28 * scale, &measuring);
        frame.settings_scroll = (std::max)(0.0f, settings.transfer_card.top - settings.content.top - 20 * scale);
        settings = ui::MakeSettingsLayout(frame, rect, scale, state.renderer.TitleBarHeight(), 28 * scale, &measuring);
        bool actions_fit = true, hit_actions = true;
        for (int i = 0; i < 3; ++i) {
            const auto& action = settings.transfer_action[i];
            actions_fit &= action.left >= settings.transfer_card.left && action.right <= settings.transfer_card.right &&
                action.top >= settings.transfer_card.top && action.bottom <= settings.transfer_card.bottom &&
                action.bottom <= settings.content.bottom;
            const auto hit = state.renderer.HitTest(frame, rect, (action.left + action.right) / 2, (action.top + action.bottom) / 2);
            hit_actions &= hit.region == ui::HitTestResult::SettingsDiagnosticsAction && hit.index == i + 3;
        }
        check(actions_fit && hit_actions, "export, import and restore buttons fit and remain hit-testable");
        draw(L"settings-transfer");

        tab->current_path = kNetworkLocationsPath;
        state.places.PinNetwork(L"\\\\FixtureNAS\\Public", L"Fixture NAS");
        FillNetworkLocationsView(state, *tab);
        frame = BuildVm(state, false);
        const auto network_entry = [&](const std::wstring& path) {
            for (size_t i = 0; i < tab->EntryCount(); ++i)
                if (app::SameNetworkLocation(tab->EntryAt(i).full_path, path))
                    return !tab->EntryAt(i).change_type_text.starts_with(L"\\\\?\\");
            return false;
        };
        check(network_entry(kConnectServerPath) && network_entry(L"\\\\FixtureNAS\\Public") && !tab->loading,
            "network page keeps connect and pinned locations available without provider I/O");
        draw(L"network-locations");

        tab->current_path.clear();
        tab->virtual_title.clear();
        tab->ClearBanner();
        const std::wstring phone_path = L"pulse:device:deadbeef0001";
        auto phone_rows = std::make_shared<std::vector<fs::DirEntry>>(1);
        auto& phone = phone_rows->front();
        phone.name = L"测试手机";
        phone.full_path = phone_path;
        phone.is_dir = true;
        phone.attrs = FILE_ATTRIBUTE_DIRECTORY;
        phone.pc_device = fs::kPcDevicePhone;
        tab->SetSnapshot(phone_rows);
        app::SidebarEntry phone_sidebar;
        phone_sidebar.label = phone.name;
        phone_sidebar.path = phone_path;
        phone_sidebar.glyph = L"\xE8EA";
        phone_sidebar.fallback = L"MTP";
        phone_sidebar.color = ui::HexColor(0x38BDF8);
        state.sidebar.drives = {phone_sidebar};
        frame = BuildVm(state, false);
        bool sidebar_phone = false;
        for (const auto& group : frame.sidebar) for (const auto& item : group.items)
            sidebar_phone |= item.path == phone_path;
        check(sidebar_phone && frame.pane.snapshot && frame.pane.snapshot->front().pc_device == fs::kPcDevicePhone,
            "phone rows are rendered in This PC and the sidebar");
        draw(L"phone");
        state.sidebar.drives.clear();
        tab->current_path = L"C:\\PulseFixtureCloud";
        tab->virtual_title.clear();
        tab->ClearBanner();
        tab->SetSnapshot(rows);
        tab->SelectOnly(0);
    }
    CancelNetworkLocations(state);
    CloseCloudTasks(state);
    state.renderer.SetCompositor(nullptr);
    state.compositor.Shutdown();
    DestroyWindow(state.hwnd);
    state.hwnd = nullptr;
    owned.reset();
    OleUninitialize();
    return failures ? 1 : 0;
}
