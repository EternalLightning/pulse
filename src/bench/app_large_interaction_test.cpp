#include "../app/app_internal.h"
#include "../ui/fluent_menu.h"
#include "../common/localization.h"
#include <cstdio>
#include <functional>
#include <unordered_set>

// Link using scripts/test_integration_fixture.ps1 after building pulse.
// Uses an isolated AppState, injected listings and transient menus; no services.
namespace pulse::ui {
struct FluentMenuTestPeer {
    static bool UniqueCommands(const FluentMenu& menu) {
        std::unordered_set<int> commands;
        for (int i = 0; i < menu.model_.Count(); ++i) {
            const auto* item = menu.model_.At(i);
            if (item->command && !commands.insert(item->command).second) return false;
            for (const auto& swatch : item->quick_swatches)
                if (!commands.insert(swatch.command).second) return false;
        }
        return true;
    }
    static bool Choose(FluentMenu& menu, const std::wstring& label) {
        for (int i = 0; i < menu.model_.Count(); ++i) {
            const auto* item = menu.model_.At(i);
            if (item->text != label) continue;
            menu.result_ = item->command;
            menu.Dismiss();
            return true;
        }
        return false;
    }
    static const std::wstring& Placeholder(const FluentMenu& menu) {
        return menu.filter_placeholder_;
    }
};
}
namespace {
int failures = 0;
std::function<void()> timer_action;
void Check(bool ok, const char* label) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
    failures += !ok;
}
VOID CALLBACK Timer(HWND, UINT, UINT_PTR, DWORD) { if (timer_action) timer_action(); }
HWND OwnColorPicker() {
    HWND found = nullptr;
    EnumThreadWindows(GetCurrentThreadId(), [](HWND hwnd, LPARAM result) -> BOOL {
        wchar_t name[64]{};
        GetClassNameW(hwnd, name, ARRAYSIZE(name));
        if (wcscmp(name, L"PulseFluentColorPicker") != 0) return TRUE;
        *reinterpret_cast<HWND*>(result) = hwnd;
        return FALSE;
    }, reinterpret_cast<LPARAM>(&found));
    return found;
}
}
int main() {
    using namespace pulse;
    setvbuf(stdout, nullptr, _IONBF, 0);
    OleInitialize(nullptr);
    l10n::Initialize(GetModuleHandleW(nullptr), L"en-US");
    auto owned = std::make_unique<AppState>();
    auto& s = *owned;
    s.isolatedTest = true;
    s.appPrefs.persist = s.places.persist = s.searchHistory.persist = s.ctxMenuPrefs.persist = false;
    s.hwnd = CreateWindowExW(0, L"STATIC", L"App interaction fixture", WS_OVERLAPPEDWINDOW,
        0, 0, 1100, 800, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!s.hwnd || !s.compositor.Init(s.hwnd)) {
        Check(false, "initialize isolated compositor");
        if (s.hwnd) DestroyWindow(s.hwnd);
        s.hwnd = nullptr; owned.reset(); OleUninitialize(); return 2;
    }
    s.compositor.RecreateTextFormats(s.scale);
    s.renderer.SetCompositor(&s.compositor);
    s.renderer.SetScale(s.scale);
    s.window_tabs.EnsureDefault();
    s.pane = s.window_tabs.Active()->FocusedPane();
    auto* tab = ActiveTab(s);
    const std::wstring folder = fs::NormalizePath(L"C:\\pulse-isolated-fixture");
    const std::wstring leaf = L"zzz-\u5b9a\u4f4d.txt";
    const std::wstring full = folder + L"\\" + leaf;
    auto rows = std::make_shared<std::vector<fs::DirEntry>>();
    for (const auto& name : {std::wstring(L"aaa.txt"), leaf}) {
        fs::DirEntry entry; entry.name = name; entry.full_path = folder + L"\\" + name;
        entry.attrs = FILE_ATTRIBUTE_NORMAL; rows->push_back(std::move(entry));
    }
    tab->current_path = app::MakeSearchPath(L"fixture");
    tab->snapshot = rows; tab->snapshot_path = tab->current_path; tab->SelectOnly(1);
    DispatchMenuCommand(s, app::CmdOpenPath);
    Check(tab->current_path == folder && tab->pending_selected_name == leaf &&
        tab->pending_ensure_selection_visible, "open location retains intent after navigation clears old state");
    // No disk worker starts here; assign an admitted generation for the injected completion.
    tab->pending_generation = 900001;
    app::WorkResult loaded;
    loaded.path = folder; loaded.generation = tab->pending_generation; loaded.snapshot = rows;
    ApplyWorkerResult(s, loaded);
    Check(tab->SelectedCount() == 1 && tab->IsSelected(1), "open location listing selects requested non-first Unicode row");
    tab->SelectOnly(1);
    DispatchMenuCommand(s, app::CmdOpenPath);
    Check(tab->IsSelected(1), "open location also selects within an already-loaded folder");

    for (const auto preset : {app::LayoutPreset::TwoVertical, app::LayoutPreset::Three,
                             app::LayoutPreset::FourGrid}) {
        ApplyLayoutPreset(s, preset);
        for (const float scale : {1.0f, 1.5f, 2.0f}) {
            s.scale = scale; s.renderer.SetScale(scale);
            ui::WindowViewModel vm; FillPaneSlots(s, vm);
            for (size_t i = 0; i < vm.pane_slots.size(); ++i) {
                s.pane = PaneAtSlot(s, static_cast<int>(i));
                const auto actual = FocusedPaneRect(s);
                const auto expected = vm.pane_slots[i].rect;
                Check(actual.left == expected.left && actual.top == expected.top &&
                    actual.right == expected.right && actual.bottom == expected.bottom,
                    "focused input rectangle equals rendered split slot across layouts and DPI");
            }
        }
    }
    s.scale = 1.0f; s.renderer.SetScale(1.0f);
    for (uint32_t i = 1; i <= 4; ++i) AppendCustomTagColor(s, 0x102030u + i);
    Check(TagColorPalette(s).size() >= 11, "fixture reaches former custom-color command collision");
    int stage = 0;
    bool saw_picker = false;
    const auto started = GetTickCount64();
    timer_action = [&] {
        if (HWND picker = OwnColorPicker()) {
            saw_picker = true;
            PostMessageW(picker, WM_CLOSE, 0, 0);
            return;
        }
        if (!s.menu || !s.menu->IsOpen()) return;
        if (stage == 0) {
            Check(ui::FluentMenuTestPeer::UniqueCommands(*s.menu), "all palette and custom commands are distinct");
            Check(ui::FluentMenuTestPeer::Choose(*s.menu, l10n::Get(l10n::StringId::CustomColor)),
                "choose real custom-color menu row");
            stage = 1;
        } else {
            s.menu->Dismiss();
        }
        if (GetTickCount64() - started > 5000) s.menu->Dismiss();
    };
    const UINT_PTR timer = SetTimer(nullptr, 0, 40, Timer);
    Check(timer != 0, "start bounded modal fixture driver");
    if (timer) ShowCreateTagPicker(s, {120, 120});
    Check(saw_picker, "custom color still opens real picker with eleven palette colors");

    for (uint32_t i = 5; i <= 94; ++i) AppendCustomTagColor(s, 0x102030u + i);
    const auto id = s.places.CreateTag(L"stable tag name", 0x123456);
    stage = 0;
    timer_action = [&] {
        if (!s.menu || !s.menu->IsOpen()) return;
        if (stage++ == 0) {
            Check(ui::FluentMenuTestPeer::UniqueCommands(*s.menu),
                "delete action remains distinct with 101 palette colors");
            // The same mutation delivered by ADS discovery during TrackPopup.
            s.places.tags.reserve(s.places.tags.capacity() + 128);
            Check(ui::FluentMenuTestPeer::Choose(*s.menu, l10n::Get(l10n::StringId::TagRename)),
                "choose rename after tag-vector relocation during modal dispatch");
        } else {
            Check(ui::FluentMenuTestPeer::Placeholder(*s.menu) == L"stable tag name",
                "rename prompt retains tag name after modal tag-vector relocation");
            s.menu->Dismiss();
        }
    };
    if (timer) ShowTagSidebarMenu(s, id, {120, 120});
    Check(stage >= 2, "tag rename modal continuation was exercised");
    if (timer) KillTimer(nullptr, timer);
    timer_action = {};
    s.menu.reset();
    s.watches.Stop();
    s.renderer.SetCompositor(nullptr); s.compositor.Shutdown();
    DestroyWindow(s.hwnd); s.hwnd = nullptr;
    owned.reset(); OleUninitialize();
    std::printf("failures=%d\n", failures);
    return failures ? 1 : 0;
}
