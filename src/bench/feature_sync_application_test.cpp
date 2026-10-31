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
#include <filesystem>
#include <iostream>

#include "settings_categories_cases.h"
#include "cloud_columns_cases.h"
#include "session_layout_cases.h"
#include "cloud_sidebar_cases.h"

int main(int argc, char** argv) {
    if (argc > 1 && std::string_view(argv[1]) == "--cloud-sidebar-only")
        return RunCloudSidebarCases();
    if (argc > 1 && std::string_view(argv[1]) == "--cloud-columns-only") {
        const int columns = RunCloudColumnsCases();
        const bool session = pulse::bench::RunSessionLayoutTests();
        return columns || !session ? 1 : 0;
    }
    if(argc>1 && std::string_view(argv[1])=="--settings-categories") {
        try { return RunSettingsCategories(); }
        catch(const std::exception& error) { std::cerr<<"[FAIL] settings fixture: "<<error.what()<<std::endl;return 2; }
    }
    using namespace pulse;
    if (FAILED(OleInitialize(nullptr))) return 2;
    l10n::Initialize(GetModuleHandleW(nullptr), L"zh-CN");
    const auto output = std::filesystem::absolute(L"docs/validation/upstream-features-20261028/screenshots");
    std::filesystem::create_directories(output);
    SetEnvironmentVariableW(L"PULSE_TEST_DATA_DIR", output.c_str());
    SetEnvironmentVariableW(L"PULSE_TEST_CLOUD_SYNC", L"仅在线文档.txt=0;本地可用文档.txt=0;始终保留的文档.txt=0");
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
        frame.settings_page = 3;
        frame.settings_scroll = 0;
        frame.settings_transfer_status = l10n::Get(l10n::StringId::SettingsTransferDesc);
        frame.settings_transfer_has_backup = true;
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
