#include "../app/app_model.h"
#include "../app/places.h"
#include "../common/utf8_file.h"
#include "../ui/FluentTokens.h"
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string_view>

namespace {

std::wstring data_dir;

struct Fixture {
    std::filesystem::path temporary;
    std::filesystem::path path;

    Fixture() {
        wchar_t buffer[MAX_PATH]{};
        if (!GetEnvironmentVariableW(L"PULSE_TEST_FIXTURE_ROOT", buffer, MAX_PATH) &&
            !GetTempPathW(MAX_PATH, buffer)) {
            std::printf("[DIAG] GetTempPathW error=%lu\n", GetLastError());
            return;
        }
        temporary = std::filesystem::path(buffer).lexically_normal();
        path = temporary / (L"pulse-quick-badges-" +
            std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
        std::error_code error;
        if (std::filesystem::create_directory(path, error)) data_dir = path.wstring();
        else std::wprintf(L"[DIAG] fixture=%ls error=%d\n", path.c_str(), error.value());
    }

    ~Fixture() {
        data_dir.clear();
        std::error_code error;
        const auto resolved = std::filesystem::weakly_canonical(path, error);
        const auto parent = std::filesystem::weakly_canonical(temporary, error);
        if (!error && resolved.parent_path() == parent &&
            resolved.filename().wstring().starts_with(L"pulse-quick-badges-"))
            std::filesystem::remove_all(resolved, error);
    }
};

const pulse::ui::SidebarItem* SidebarItemFor(const pulse::ui::WindowViewModel& model,
                                           const std::wstring& path) {
    for (const auto& group : model.sidebar)
        for (const auto& item : group.items)
            if (_wcsicmp(item.path.c_str(), path.c_str()) == 0) return &item;
    return nullptr;
}

bool BadgeIs(const pulse::app::PlacesCatalog& places, const std::wstring& path,
             const std::wstring& text, uint32_t rgb) {
    const auto* badge = places.FindQuickAccessBadge(path);
    return badge && badge->badge == text && badge->badge_rgb == rgb;
}

} // namespace

namespace pulse::app {
std::wstring GetPulseDataDir() { return data_dir; }
} // namespace pulse::app

int wmain(int argc, wchar_t** argv) {
    using namespace pulse;
    Fixture fixture;
    if (data_dir.empty()) {
        std::printf("[FAIL] create isolated badge fixture\n");
        return 1;
    }
    int failures = 0;
    const auto check = [&](bool passed, const char* message) {
        std::printf("[%s] %s\n", passed ? "PASS" : "FAIL", message);
        if (!passed) ++failures;
    };
    const std::wstring desktop = L"C:\\fixture\\desktop";
    const std::wstring pinned = L"C:\\fixture\\pinned";
    const std::wstring cloud = L"\\\\server\\share\\云盘";
    const std::wstring long_path = L"C:\\fixture\\" + std::wstring(280, L'x');
    if (argc == 2 && std::wstring_view(argv[1]) == L"--order") {
        app::Pane pane;
        pane.NewTab(app::MakeHomePath());
        pane.ActiveTab()->git_root = L"C:\\fixture\\project";
        app::SidebarModel sidebar;
        for (const auto& [path, builtin] : std::vector<std::pair<std::wstring, app::BuiltinQuickAccess>>{
                 {app::MakeRecentPath(), app::BuiltinQuickAccess::Recent},
                 {desktop, app::BuiltinQuickAccess::Desktop},
                 {L"C:\\fixture\\downloads", app::BuiltinQuickAccess::Downloads},
                 {app::MakeRecyclePath(), app::BuiltinQuickAccess::RecycleBin}}) {
            app::SidebarEntry entry;
            entry.path = path; entry.label = path; entry.builtin = static_cast<int>(builtin);
            sidebar.quick_access.push_back(std::move(entry));
        }
        const auto rows = [&](const app::PlacesCatalog& places, uint32_t hidden = 0) {
            const auto vm = app::BuildWindowViewModel(pane, sidebar, true, false, false, &places,
                0, 0, true, nullptr, hidden);
            std::vector<std::wstring> paths;
            for (const auto& group : vm.sidebar) {
                if (group.id != static_cast<int>(app::SidebarSectionId::QuickAccess)) continue;
                for (const auto& item : group.items) paths.push_back(fs::NormalizePath(item.path));
            }
            return paths;
        };
        std::vector<std::wstring> saved;
        {
            app::PlacesCatalog places;
            places.SetQuickAccessPinned({cloud, long_path, desktop}, true);
            auto visible = rows(places);
            check(visible.size() == 7 && visible.front() == app::MakeRecentPath(),
                "legacy defaults retain built-ins, project and pin order without duplicate built-ins");
            check(places.ReorderQuickAccess(visible, cloud, 0) && rows(places).front() == fs::NormalizePath(cloud),
                "a pinned UNC folder can move before all built-in entries");
            for (const auto& entry : sidebar.quick_access) {
                visible = rows(places);
                check(places.ReorderQuickAccess(visible, entry.path, visible.size()) && rows(places).back() == fs::NormalizePath(entry.path),
                    "each built-in including virtual entries can move after pinned folders");
            }
            visible = rows(places);
            check(!places.ReorderQuickAccess(visible, visible.front(), 1) && rows(places) == visible &&
                !places.ReorderQuickAccess(visible, L"C:\\missing", 0),
                "same gap and absent rows leave the full order unchanged");
            check(places.ReorderQuickAccess(visible, visible.back(), 0) && rows(places).front() == visible.back(),
                "upward moves use the indicated full-list insertion gap");
            const auto hidden = 1u << static_cast<int>(app::BuiltinQuickAccess::Downloads);
            const size_t downloads_slot = places.QuickAccessRank(L"C:\\fixture\\downloads");
            visible = rows(places, hidden);
            places.ReorderQuickAccess(visible, visible.front(), visible.size());
            check(rows(places, hidden).size() == 6 &&
                places.QuickAccessRank(L"C:\\fixture\\downloads") == downloads_slot && rows(places).size() == 7,
                "sorting visible rows preserves the hidden built-in's saved slot");
            const auto project_slot = places.QuickAccessRank(pane.ActiveTab()->git_root);
            pane.ActiveTab()->git_root.clear();
            visible = rows(places);
            places.ReorderQuickAccess(visible, visible.front(), visible.size());
            pane.ActiveTab()->git_root = L"C:\\fixture\\project";
            check(places.QuickAccessRank(pane.ActiveTab()->git_root) == project_slot && rows(places).size() == 7,
                "temporarily absent project rows keep their custom position");
            places.RemapPaths(cloud, L"\\\\server\\share\\renamed");
            check(places.QuickAccessRank(cloud) == places.quick_access_order.size() &&
                places.QuickAccessRank(L"\\\\server\\share\\renamed") < places.quick_access_order.size(),
                "renaming a pinned location also remaps its display order");
            saved = rows(places);
        }
        {
            app::PlacesCatalog restarted;
            check(restarted.Load() && rows(restarted) == saved,
                "restart restores mixed built-in, project, UNC and long-path ordering");
        }
        check(WriteUtf8FileAtomic(data_dir + L"\\places.json",
            LR"({"quick_access_paths":["C:\\fixture\\pinned"],"quick_access_order":["pulse:recent","pulse:recent","","C:\\fixture\\pinned","c:\\fixture\\PINNED"]})"),
            "write isolated duplicate-order fixture");
        {
            app::PlacesCatalog partial;
            partial.persist = false;
            check(partial.Load() && partial.quick_access_order.size() == 2 &&
                partial.QuickAccessRank(L"c:\\fixture\\PINNED") == 1,
                "loading order keeps virtual paths and rejects empty or case-duplicate paths");
        }
        return failures ? 1 : 0;
    }
    {
        app::PlacesCatalog places;
        places.persist = false;
        check(!places.SetQuickAccessBadge(L"", L"invalid", 0x123456) &&
              !places.SetQuickAccessBadge(app::MakeHomePath(), L"invalid", 0x123456),
              "reject empty and virtual badge paths");
        check(places.SetQuickAccessBadge(desktop, L"  工作  ", 0xFF123456) &&
              BadgeIs(places, L"c:\\FIXTURE\\desktop", L"工作", 0x123456) &&
              !places.SetQuickAccessBadge(desktop, L"工作", 0x123456),
              "normalize paths, trim text, mask color and detect unchanged badges");
        check(places.SetQuickAccessBadge(pinned, L"abcdefghijklmnop", 0xABCDEF) &&
              BadgeIs(places, pinned, L"abcdefghijkl", 0xABCDEF),
              "limit badge text to twelve characters");

        app::Pane pane;
        pane.view.current_path = desktop;
        places.quick_access_paths.push_back(pinned);
        places.SetQuickAccessBadge(cloud, L"云盘", 0x336699);
        for (int rebuild = 0; rebuild < 2; ++rebuild) {
            app::SidebarModel sidebar;
            app::SidebarEntry entry;
            entry.path = desktop;
            entry.badge = L"Desktop";
            sidebar.quick_access.push_back(entry);
            entry.path = cloud;
            entry.badge.clear();
            sidebar.cloud.push_back(entry);
            const auto model = app::BuildWindowViewModel(pane, sidebar, true, false,
                                                         rebuild != 0, &places);
            const auto* desktop_item = SidebarItemFor(model, desktop);
            const auto* pinned_item = SidebarItemFor(model, pinned);
            const auto* cloud_item = SidebarItemFor(model, cloud);
            check(desktop_item && desktop_item->badge == L"工作" &&
                  pinned_item && pinned_item->badge == L"abcdefghijkl" &&
                  cloud_item && cloud_item->badge == L"云盘" &&
                  cloud_item->badge_color.b == ui::HexColor(0x336699).b,
                  "fresh sidebar models restore built-in, pinned and cloud badges in both themes");
        }
        check(places.SetQuickAccessBadge(desktop, L"", app::kDefaultBadgeRgb) &&
              !places.FindQuickAccessBadge(desktop) &&
              !places.SetQuickAccessBadge(desktop, L"", app::kDefaultBadgeRgb),
              "default text and color clear stored overrides");
        app::SidebarModel restored_sidebar;
        app::SidebarEntry restored_entry;
        restored_entry.path = desktop;
        restored_entry.badge = L"Desktop";
        restored_sidebar.quick_access.push_back(restored_entry);
        const auto restored_model = app::BuildWindowViewModel(
            pane, restored_sidebar, true, false, false, &places);
        const auto* restored_item = SidebarItemFor(restored_model, desktop);
        check(restored_item && restored_item->badge == L"Desktop",
              "clearing an override restores the rebuilt entry's default badge");
    }
    {
        app::PlacesCatalog places;
        places.SetQuickAccessBadge(desktop, L"初始", 0x123456);
        for (int i = 0; i < 20; ++i)
            places.SetQuickAccessBadge(desktop, L"更新" + std::to_wstring(i), 0x123456);
        places.SetQuickAccessBadge(cloud, L"云\"盘\\", 0x345678);
        places.SetQuickAccessBadge(long_path, L"", 0x998877);
    }
    {
        app::PlacesCatalog reloaded;
        check(reloaded.Load() && BadgeIs(reloaded, desktop, L"更新19", 0x123456) &&
              BadgeIs(reloaded, cloud, L"云\"盘\\", 0x345678) &&
              BadgeIs(reloaded, long_path, L"", 0x998877),
              "writer shutdown persists latest text, escaped Unicode, UNC and long paths");
        reloaded.SetQuickAccessBadge(desktop, L"", app::kDefaultBadgeRgb);
    }
    {
        app::PlacesCatalog reloaded;
        check(reloaded.Load() && !reloaded.FindQuickAccessBadge(desktop) &&
              BadgeIs(reloaded, long_path, L"", 0x998877),
              "restart preserves clearing and color-only overrides");
    }
    check(WriteUtf8FileAtomic(data_dir + L"\\places.json",
        LR"({"quick_access_paths":["C:\\fixture\\pinned"],"starred":["C:\\fixture\\legacy"]})"),
        "write legacy places fixture");
    {
        app::PlacesCatalog legacy;
        legacy.persist = false;
        check(legacy.Load() && legacy.quick_access_badges.empty() &&
              legacy.IsQuickAccessPinned(pinned) && legacy.IsStarred(L"C:\\fixture\\legacy"),
              "older places data loads without badge records");
    }
    check(WriteUtf8FileAtomic(data_dir + L"\\places.json",
        LR"({"quick_access_badges":[{"path":"C:\\fixture\\desktop","badge":"Legacy"},{"path":"c:\\fixture\\DESKTOP","badge":"Duplicate","rgb":123},{"path":"pulse:home","badge":"Invalid","rgb":123},{"path":"","badge":"Invalid","rgb":123}]})"),
        "write partial badge fixture");
    {
        app::PlacesCatalog partial;
        partial.persist = false;
        check(partial.Load() && partial.quick_access_badges.size() == 1 &&
              BadgeIs(partial, desktop, L"Legacy", app::kDefaultBadgeRgb),
              "missing color uses default; duplicate and virtual badge paths are skipped");
    }
    {
        app::PlacesCatalog places;
        places.SetQuickAccessBadge(L"C:\\old", L"Root", 0x123456);
        places.SetQuickAccessBadge(L"C:\\old\\child", L"Child", 0x234567);
        places.SetQuickAccessBadge(L"C:\\old-other", L"Outside", 0x345678);
        places.SetQuickAccessBadge(L"C:\\new\\child", L"Collision", 0x456789);
        places.RemapPaths(L"C:\\old", L"C:\\new");
        check(!places.FindQuickAccessBadge(L"C:\\old") &&
              BadgeIs(places, L"C:\\new", L"Root", 0x123456) &&
              BadgeIs(places, L"C:\\new\\child", L"Child", 0x234567) &&
              BadgeIs(places, L"C:\\old-other", L"Outside", 0x345678) &&
              places.quick_access_badges.size() == 3,
              "rename remaps descendants, respects path boundaries and removes collisions");
    }
    {
        app::PlacesCatalog reloaded;
        check(reloaded.Load() && BadgeIs(reloaded, L"C:\\new\\child", L"Child", 0x234567),
              "renamed badge paths survive restart");
        reloaded.RemoveAssignments(L"C:\\new", false);
        check(!reloaded.FindQuickAccessBadge(L"C:\\new") &&
              reloaded.FindQuickAccessBadge(L"C:\\new\\child"),
              "exact removal retains descendant badges");
        reloaded.RemoveAssignments(L"C:\\new", true);
        check(!reloaded.FindQuickAccessBadge(L"C:\\new\\child") &&
              reloaded.FindQuickAccessBadge(L"C:\\old-other"),
              "descendant removal clears only badges within the removed subtree");
    }
    std::printf("Quick-access badge failures: %d\n", failures);
    return failures ? 1 : 0;
}
