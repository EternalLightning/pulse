// SPDX-License-Identifier: Apache-2.0
#include "../app/app_internal.h"
#include "../app/command_catalog.h"
#include "../app/app_input.h"
#include <shlobj.h>
#include <wrl/client.h>
#include "../model/path_aliases.h"
#include "../model/path_alias_store.h"
#include "../model/details_columns.h"
#include "../fs/shell_namespace.h"
#include "../common/user_storage.h"
#include "../common/utf8_file.h"
#include "../ui/details_column_labels.h"
#include "../ui/toolbar_layout.h"
#include <filesystem>
#include <fstream>
#include <cstdio>

namespace pulse::ui {
struct ThumbnailCacheTestAccess {
    static void Seed(ThumbnailCache& cache, const std::wstring& path) {
        std::lock_guard lock(cache.mutex_);
        const auto key = cache.Key(path, 0, 0, 64) + L":properties";
        ThumbnailCache::Item item;
        if (path.ends_with(L".mp4")) item.properties = {{L"Duration", L"00:03:12"}};
        else item.properties = {{L"Dimensions", L"1920 × 1080"}, {L"Date taken", L"2026-11-17 10:00"}};
        cache.lru_.push_back(key);
        item.lru_position = std::prev(cache.lru_.end());
        cache.items_.emplace(key, std::move(item));
    }
};
}
namespace {
int passed = 0, failed = 0;
void Check(bool ok, const char* label) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
    ok ? ++passed : ++failed;
}
template<typename F> bool Wait(F ready) {
    const auto deadline = GetTickCount64() + 5000;
    while (!ready() && GetTickCount64() < deadline) Sleep(5);
    return ready();
}
}
int main() {
    using namespace pulse;
    namespace fsys = std::filesystem;
    using K = model::DetailsColumnKind;
    using E = model::PathAliasError;
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    OleInitialize(nullptr);
    const auto root = fsys::absolute(L"../bench_data/customization-2026-11-17");
    fsys::create_directories(root / L"profile");
    storage::OverrideDefaultRootForTesting((root / L"profile").wstring());
    l10n::Initialize(GetModuleHandleW(nullptr), L"en-US");
    for (const auto language : {L"zh-CN", L"en-US"}) {
        l10n::SetLanguage(language);
        const std::array labels{l10n::StringId::CustomizeCommands, l10n::StringId::PathAliases,
            l10n::StringId::PathAliasSet, l10n::StringId::ShellLibraries,
            l10n::StringId::ShellControlPanel, l10n::StringId::ShellApplications};
        Check(std::all_of(labels.begin(), labels.end(), [](const auto id) { return !l10n::Get(id).empty(); }),
            "new command and namespace titles are present in each supported language");
    }
    model::PathAliases aliases;
    std::wstring resolved;
    Check(aliases.Set(L"work", L"C:\\Projects") == E::None, "create folder alias");
    Check(aliases.Resolve(L"@WORK\\课程\\图纸", resolved) == E::None && resolved == L"C:\\Projects\\课程\\图纸", "case-insensitive alias with Unicode subfolders");
    Check(aliases.Set(L"WORK", L"D:\\Other") == E::Duplicate, "duplicate names do not overwrite");
    Check(aliases.Resolve(L"@work\\..\\outside", resolved) == E::InvalidSuffix && resolved.empty(), "parent traversal is rejected");
    Check(aliases.Resolve(L"@missing", resolved) == E::Unknown, "unknown alias is explicit");
    Check(aliases.Set(L"bad/name", L"C:\\") == E::InvalidName, "alias name validation");
    Check(aliases.Set(L"relative", L"folder") == E::InvalidTarget, "relative alias target rejected");
    model::PathAliases loaded;
    Check(loaded.LoadJson(aliases.ToJson()) && loaded.Resolve(L"@work", resolved) == E::None && resolved == L"C:\\Projects", "alias serialization round trip");
    Check(loaded.LoadJson(L"{\"aliases\":{\"D:\\\\Projects\":\"项目\"}}") && loaded.Resolve(L"@项目", resolved) == E::None, "legacy path-to-name schema migrates");
    Check(!loaded.LoadJson(L"{\"version\":2,\"aliases\":[]}") && loaded.Items().size() == 1, "unknown schema preserves current model");
    {
        model::PathAliasStore store;
        store.Start((root / L"profile" / L"aliases.json").wstring(), [] {});
        Check(Wait([&] { return store.Snapshot().state == model::PathAliasStoreState::Ready; }), "asynchronous alias store loads isolated profile");
        const auto snapshot = store.Snapshot();
        Check(store.Commit(aliases, snapshot.revision), "alias write admitted with current revision");
        Check(!store.Commit(loaded, snapshot.revision), "concurrent stale edit cannot overwrite pending save");
        Check(Wait([&] { return store.Snapshot().state == model::PathAliasStoreState::Ready && store.Snapshot().revision > snapshot.revision; }), "atomic alias save completes");
    }
    std::vector<app::CommandShortcutOverride> bindings;
    app::CommandShortcut shortcut;
    Check(app::ParseCommandShortcut(L"Ctrl+Shift+Y", shortcut), "parse configurable shortcut");
    Check(app::SetCommandShortcut(bindings, app::CmdCopy, shortcut) == app::ShortcutBindingResult::Success, "bind existing command");
    Check(app::SetCommandShortcut(bindings, app::CmdPaste, shortcut) == app::ShortcutBindingResult::Conflict, "conflicting binding rejected");
    app::ConfigureCommandShortcuts(bindings);
    Check(app::LookupCommandShortcut('Y', true, true, false) == app::CmdCopy &&
          app::LookupCommandShortcut('C', true, false, false) == app::CmdNone, "effective binding replaces old default");
    Check(app::SetCommandShortcut(bindings, app::CmdCopy, {}) == app::ShortcutBindingResult::Success, "remove default binding explicitly");
    app::ConfigureCommandShortcuts(bindings);
    Check(app::LookupCommandShortcut('C', true, false, false) == app::CmdNone, "unbound default remains disabled");
    app::ParseCommandShortcut(L"Ctrl+Shift+K", shortcut);
    Check(app::ValidateCommandShortcut(app::CmdCopy, shortcut, bindings) == app::ShortcutBindingResult::Reserved, "command palette key remains reserved");
    app::ResetCommandShortcut(bindings, app::CmdCopy);
    app::ConfigureCommandShortcuts(bindings);
    Check(app::LookupCommandShortcut('C', true, false, false) == app::CmdCopy, "restore one default binding");
    app::AppPrefs preferences;
    preferences.command_shortcuts = {{app::CmdCopy, {}}};
    preferences.toolbar_order = {app::CmdPathAliases, app::CmdCopy, app::CmdLibraries};
    auto columns = model::DefaultDetailsColumns();
    columns.order = {K::Name, K::Dimensions, K::Duration, K::DateTaken, K::Size};
    columns.widths[static_cast<size_t>(K::Dimensions)] = 142;
    Check(preferences.folder_views.SetColumnLayout(L"C:\\Projects", columns), "persist column visibility and order");
    app::AppPrefs restored;
    Check(restored.FromJson(preferences.ToJson()) && restored.toolbar_order == preferences.toolbar_order &&
          restored.command_shortcuts.size() == 1 && restored.command_shortcuts[0].shortcut.key == 0 &&
          restored.folder_views.FindColumnLayout(L"c:\\projects") == columns, "shortcut toolbar and directory columns round trip");
    auto duplicate_columns = columns; duplicate_columns.order.push_back(K::Size);
    Check(!model::ValidDetailsColumns(duplicate_columns), "duplicate column kinds rejected");
    const auto layout = ui::MakeToolbarLayout(1600, 1, 44, 12, 86, 224, 0, false, false, preferences.toolbar_order);
    Check(layout.create.right == 0 && layout.placed_commands.size() == 3 && layout.placed_commands[0].command == app::CmdPathAliases,
          "toolbar hides New and respects user order");
    const auto narrow = ui::MakeToolbarLayout(520, 1.5f, 66, 18, 100, 224, 0, false, false, preferences.toolbar_order);
    Check(narrow.placed_commands.empty(), "narrow toolbar leaves overflow access");
    fsys::create_directories(root / L"library-a"); fsys::create_directories(root / L"library-b");
    std::ofstream(root / L"library-a" / L"a.txt") << "a";
    std::ofstream(root / L"library-b" / L"b.txt") << "b";
    std::vector<fs::DirEntry> rows;
    Check(SUCCEEDED(fs::EnumerateLibraryLocations({(root / L"library-a").wstring(), (root / L"library-b").wstring(),
        (root / L"library-a").wstring()}, rows)) && rows.size() == 2, "library aggregates all member folders without duplicate physical entries");
    Check(std::all_of(rows.begin(), rows.end(), [](const auto& row) { return !row.full_path.empty() && row.shell_id.empty(); }),
        "library files retain real filesystem paths");
    Check(FAILED(fs::EnumerateLibraryLocations({(root / L"library-a").wstring()}, rows, [] { return true; })) && rows.empty(),
        "cancelled library does not publish partial rows");
    std::vector<fs::DirEntry> apps;
    for (const auto path : {fs::kLibrariesPath, fs::kControlPanelPath, fs::kApplicationsPath}) {
        const HRESULT result = fs::EnumerateShellNamespace(path, rows);
        Check(SUCCEEDED(result), "read-only enumeration of supported Windows namespace");
        if (path == fs::kApplicationsPath && SUCCEEDED(result)) apps = rows;
        if (path != fs::kLibrariesPath && SUCCEEDED(result)) Check(std::all_of(rows.begin(), rows.end(), [](const auto& row) {
            return fs::IsShellItemEntry(row) && !row.shell_id.empty();
        }), "applications and applets never become writable filesystem rows");
        if (SUCCEEDED(result)) Check(std::all_of(rows.begin(), rows.end(), [](const auto& row) {
            return !fs::IsShellItemEntry(row) || (!row.shell_id.empty() && fs::IsVirtualPath(row.full_path));
        }), "virtual rows carry Shell identities and virtual paths");
    }
    const auto app_crumbs = ui::SplitBreadcrumb(fs::kApplicationsPath);
    Check(app_crumbs.size() == 1 && !app_crumbs[0].text.empty() &&
        app_crumbs[0].text == l10n::Get(l10n::StringId::ShellApplications),
        "namespace breadcrumb shows a localized user-facing title");
    {
        using Microsoft::WRL::ComPtr;
        ComPtr<IShellLibrary> library;
        ComPtr<IShellItem> first, second, parent, saved;
        HRESULT result = CoCreateInstance(CLSID_ShellLibrary, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&library));
        if (SUCCEEDED(result)) result = SHCreateItemFromParsingName((root / L"library-a").c_str(), nullptr, IID_PPV_ARGS(&first));
        if (SUCCEEDED(result)) result = SHCreateItemFromParsingName((root / L"library-b").c_str(), nullptr, IID_PPV_ARGS(&second));
        if (SUCCEEDED(result)) result = library->AddFolder(first.Get());
        if (SUCCEEDED(result)) result = library->AddFolder(second.Get());
        if (SUCCEEDED(result)) result = SHCreateItemFromParsingName(root.c_str(), nullptr, IID_PPV_ARGS(&parent));
        if (SUCCEEDED(result)) result = library->Save(parent.Get(), L"Fixture library", LSF_OVERRIDEEXISTING, &saved);
        Check(SUCCEEDED(result), "create isolated native library description without changing user libraries");
        if (SUCCEEDED(result)) {
            Check(SUCCEEDED(fs::EnumerateShellNamespace(std::wstring(fs::kLibraryPrefix) + (root / L"Fixture library.library-ms").wstring(), rows)) &&
                rows.size() == 2, "native library provider aggregates both configured members");
        }
    }
    auto owned = std::make_unique<AppState>(); auto& s = *owned;
    s.isolatedTest = true;
    s.appPrefs.persist = s.places.persist = s.ctxMenuPrefs.persist = s.searchHistory.persist = false;
    s.hwnd = CreateWindowExW(WS_EX_NOACTIVATE, L"STATIC", L"Pulse customization fixture", WS_POPUP,
        -30000, -30000, 1200, 700, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    Check(s.hwnd && s.compositor.Init(s.hwnd), "initialize isolated production renderer");
    if (s.compositor.Dc()) {
        s.renderer.SetCompositor(&s.compositor);
        const auto sample = (root / L"library-a" / L"sample.png").wstring();
        ui::ThumbnailCacheTestAccess::Seed(*s.renderer.SharedThumbnailCache(), sample);
        const auto video = (root / L"library-a" / L"movie.mp4").wstring();
        ui::ThumbnailCacheTestAccess::Seed(*s.renderer.SharedThumbnailCache(), video);
        s.window_tabs.EnsureDefault(); s.pane = s.window_tabs.Active()->FocusedPane();
        s.pane->view.current_path = (root / L"library-a").wstring();
        s.appPrefs.auto_folder_size = false; s.detailsPreviewEnabled = false;
        app::ConfigureCommandShortcuts(std::vector<app::CommandShortcutOverride>{{app::CmdDetailsPanel, {VK_F8}}});
        HandleKeyDown(&s, s.hwnd, WM_KEYDOWN, VK_F8, 0);
        Check(s.showDetailsPanel, "production keyboard dispatch uses the custom binding");
        s.showDetailsPanel = false; s.renderer.SetDetailsPanelVisible(false);
        app::ConfigureCommandShortcuts({});
        for (const bool dark : {false, true}) for (const float scale : {1.0f, 1.5f, 2.0f}) for (const int dip_width : {760, 1500}) {
            const int width = static_cast<int>(dip_width * scale), height = static_cast<int>(660 * scale);
            SetWindowPos(s.hwnd, nullptr, -30000, -30000, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
            s.compositor.Resize(width, height); s.compositor.RecreateTextFormats(scale); s.renderer.SetScale(scale);
            ui::WindowViewModel vm;
            vm.dark = dark; vm.safe_mode = true; vm.window_title = L"Project files";
            vm.pane.path = (root / L"library-a").wstring(); vm.pane.focused = true; vm.pane.view_mode = ui::ViewMode::Details;
            vm.pane.details_columns = columns;
            auto entries = std::make_shared<std::vector<fs::DirEntry>>();
            fs::DirEntry row; row.name = L"课程设计 sample.png"; row.full_path = sample; row.size = 64; row.attrs = FILE_ATTRIBUTE_NORMAL;
            entries->push_back(row);
            row.name = L"演示 movie.mp4"; row.full_path = video; entries->push_back(row);
            vm.pane.snapshot = entries;
            vm.toolbar_order = {app::CmdCopy, app::CmdAdvancedSearch, app::CmdPathAliases, app::CmdLibraries};
            for (const int command : vm.toolbar_order) {
                const auto item = app::BuildCommandMenuItem(command, {});
                vm.toolbar_commands.push_back({command, item.text, item.glyph, true, item.pictogram});
            }
            s.renderer.SetToolbarOrder(vm.toolbar_order);
            const auto bounds = D2D1::RectF(224 * scale, 140 * scale, static_cast<float>(width), static_cast<float>(height));
            const auto measured = s.renderer.DetailsColumns(bounds, vm.pane);
            Check(measured.count >= 1 && measured.widths[0] >= 0 && measured.DividerX(measured.count - 1) <= bounds.right + 1,
                "configured columns stay inside available pane at every DPI");
            auto* dc = s.compositor.Dc(); dc->BeginDraw(); dc->Clear(D2D1::ColorF(0, 0));
            s.renderer.Render(vm, D2D1::RectF(0, 0, static_cast<float>(width), static_cast<float>(height)), ui::MakeTheme(dark, ui::HexColor(0x0078D4)));
            Check(SUCCEEDED(dc->EndDraw()), "render configured toolbar and media columns");
            const auto name = std::wstring(dark ? L"dark-" : L"light-") + std::to_wstring(static_cast<int>(scale * 100)) + L"-" + std::to_wstring(dip_width) + L".png";
            Check(s.compositor.SaveSnapshot((root / name).c_str()), "save production UI snapshot");
            const auto toolbar = ui::MakeToolbarLayout(static_cast<float>(width), scale, s.renderer.TitleBarHeight(), s.renderer.Margin(),
                s.renderer.NewButtonWidthPx(dip_width < 600), s.renderer.EffectiveSidebarWidth(static_cast<float>(width)), 0, false, false, vm.toolbar_order);
            for (const auto& placed : toolbar.placed_commands) if (placed.command == app::CmdPathAliases) {
                const auto hit = s.renderer.HitTest(vm, D2D1::RectF(0,0,static_cast<float>(width),static_cast<float>(height)),
                    (placed.bounds.left + placed.bounds.right) / 2, (placed.bounds.top + placed.bounds.bottom) / 2);
                Check(hit.region == ui::HitTestResult::CustomToolbarCommand && hit.index == app::CmdPathAliases,
                    "custom toolbar paint and hit target agree");
            }
        }
        {
            auto* tab = s.pane->ActiveTab();
            tab->current_path = fs::kApplicationsPath;
            tab->virtual_title = l10n::Get(l10n::StringId::ShellApplications);
            tab->details_columns = {true, {K::Name}, {}};
            tab->SetSnapshot(std::make_shared<const std::vector<fs::DirEntry>>(apps));
            tab->loading = false;
            if (!apps.empty()) {
                tab->SelectOnly(0);
                const auto context = BuildCommandContext(s);
                Check(context.shell_selection && !app::IsCommandEnabled(app::CmdDelete, context) &&
                      !app::IsCommandEnabled(app::CmdCopy, context) && app::IsCommandEnabled(app::CmdProperties, context),
                    "application entries expose properties while filesystem mutations stay disabled");
            }
            SetWindowPos(s.hwnd, nullptr, -30000, -30000, 1400, 760, SWP_NOZORDER | SWP_NOACTIVATE);
            s.compositor.Resize(1400,760); s.compositor.RecreateTextFormats(1); s.renderer.SetScale(1);
            auto vm = BuildVm(s, false);
            vm.dark = true; vm.safe_mode = true;
            std::vector<ui::MainRenderer::BreadcrumbPlaced> crumbs;
            s.renderer.BreadcrumbLayout(vm.pane, 1400, crumbs);
            Check(!crumbs.empty() && crumbs.back().text == l10n::Get(l10n::StringId::ShellApplications),
                "production namespace model supplies the visible breadcrumb title");
            auto* dc = s.compositor.Dc();
            for (int frame = 0; frame < 3; ++frame) {
                dc->BeginDraw(); dc->Clear(D2D1::ColorF(0,0));
                s.renderer.Render(vm, D2D1::RectF(0,0,1400,760), ui::MakeTheme(true, ui::HexColor(0x0078D4)));
                Check(SUCCEEDED(dc->EndDraw()), "render native application namespace");
                if (frame < 2) Sleep(200);
            }
            Check(s.compositor.SaveSnapshot((root / L"namespace-apps.png").c_str()), "save native namespace screenshot");
        }
        {
            const auto actual_image = root / L"light-100-760.png";
            auto cache = s.renderer.SharedThumbnailCache();
            std::vector<ui::PreviewProperty> properties;
            const bool ready = Wait([&] {
                ui::ThumbnailCache::FrameScope frame(*cache);
                return cache->Properties(actual_image.wstring(), FILE_ATTRIBUTE_NORMAL, 100, 0,
                    fsys::file_size(actual_image), properties, ui::ThumbnailCache::Consumer::Main, false);
            });
            Check(ready && std::any_of(properties.begin(), properties.end(), [](const auto& property) {
                return (property.label == L"Dimensions" || property.label == L"尺寸") && property.value.find(L"760") != std::wstring::npos;
            }), "visible-row property requests decode a real image on the background preview host");
        }
        s.renderer.SetCompositor(nullptr); s.compositor.Shutdown();
    }
    if (s.hwnd) DestroyWindow(s.hwnd); s.hwnd = nullptr; owned.reset();
    OleUninitialize();
    std::printf("%d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
