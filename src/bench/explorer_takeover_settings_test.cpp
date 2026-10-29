#include "../app/app_internal.h"
#include "../app/app_input.h"
#include "../common/user_storage.h"
#include "../ui/ui_renderer_internal.h"
#include <filesystem>
#include <cstdio>

namespace {
int checks = 0, failures = 0;
void Check(bool pass, const char* label) {
    ++checks;
    if (!pass) ++failures;
    std::printf("[%s] %s\n", pass ? "PASS" : "FAIL", label);
}
class MemoryRegistry final : public pulse::app::DefaultManagerRegistryStore {
public:
    int writes = 0;
    LSTATUS Read(const std::wstring&, const std::wstring&,
        pulse::app::DefaultManagerRegistryValue& value) override { value = {}; return ERROR_SUCCESS; }
    LSTATUS Write(const std::wstring&, const std::wstring&,
        const pulse::app::DefaultManagerRegistryValue&) override { ++writes; return ERROR_SUCCESS; }
    LSTATUS Flush(const std::wstring&) override { return ERROR_SUCCESS; }
    LSTATUS RemoveTree(const std::wstring&) override { ++writes; return ERROR_SUCCESS; }
    void RemoveEmpty(const std::wstring&) override {}
};
void Settle(pulse::AppState& s) {
    const auto until = GetTickCount64() + 3000;
    while (s.systemIntegration.pending() && GetTickCount64() < until) {
        s.systemIntegration.Tick(s);
        Sleep(1);
    }
    Check(!s.systemIntegration.pending(), "isolated service task completes");
}
}

int main() {
    using namespace pulse;
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (FAILED(OleInitialize(nullptr))) return 2;
    const auto output = std::filesystem::absolute(L"bench_data/issue-fixes-main/settings");
    std::filesystem::create_directories(output);
    storage::OverrideDefaultRootForTesting((output / L"profile").wstring());
    l10n::Initialize(GetModuleHandleW(nullptr), L"zh-CN");
    auto owned = std::make_unique<AppState>();
    auto& s = *owned;
    s.isolatedTest = true;
    s.appPrefs.persist = s.places.persist = s.ctxMenuPrefs.persist = s.searchHistory.persist = false;
    s.hwnd = CreateWindowExW(WS_EX_NOACTIVATE, L"STATIC", L"isolated takeover settings",
        WS_POPUP, -30000, -30000, 1100, 900, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    Check(s.hwnd && s.compositor.Init(s.hwnd), "hidden settings owner and compositor");
    if (!s.hwnd || !s.compositor.Dc()) return 2;
    s.renderer.SetCompositor(&s.compositor);
    s.core.windows.Add(s.hwnd, &s);
    s.core.global_ui_owner = s.hwnd;
    s.window_tabs.NewTab(app::MakeSettingsPath(L"general"));
    s.pane = s.window_tabs.Active()->FocusedPane();
    ActiveTab(s)->loading = false;
    MemoryRegistry registry;
    Check(s.systemIntegration.StartForTesting(s, {nullptr, false, &registry}),
        "integration uses memory registry and disabled real Explorer services");
    Settle(s);
    app::SettingsController::UiCallbacks callbacks;
    int dispatches = 0;
    callbacks.toggle_system_integration = [&](int control) {
        ++dispatches;
        Check(control == 24, "settings controller forwards the dedicated takeover control");
        s.systemIntegration.Toggle(s, control);
    };
    s.settings.BindUi(s.appPrefs, s.ctxMenuPrefs, s.index, s.networkIndex, std::move(callbacks));
    Check(!s.appPrefs.experimental_explorer_takeover, "existing default remains opt-in and off");
    Check(!BuildVm(s, false).settings_explorer_takeover, "view model reflects disabled takeover");
    s.settings.ToggleUi(24);
    Check(s.appPrefs.experimental_explorer_takeover, "control enables the existing takeover preference");
    Settle(s);
    Check(BuildVm(s, false).settings_explorer_takeover, "view model reflects enabled takeover");
    s.settings.ToggleUi(24);
    Settle(s);
    Check(!s.appPrefs.experimental_explorer_takeover && dispatches == 2,
        "control disables the same preference through the service dispatcher");
    Check(registry.writes == 0, "independent takeover toggle does not rewrite folder associations");

    ui::fluent::Painter painter(&s.compositor);
    for (const auto* language : {L"zh-CN", L"en-US"}) {
        l10n::SetLanguage(language);
        Check(!l10n::Get(l10n::StringId::SettingsExplorerTakeover).empty() &&
            !l10n::Get(l10n::StringId::SettingsExplorerTakeoverDesc).empty(),
            "localized takeover title and description are present");
        for (float scale : {1.0f, 2.0f}) {
            s.scale = scale;
            s.compositor.RecreateTextFormats(scale);
            s.renderer.SetScale(scale);
            painter.SetScale(scale);
            for (float width : {760.0f, 1100.0f}) {
                const auto window = D2D1::RectF(0, 0, width * scale, 900 * scale);
                SetWindowPos(s.hwnd, nullptr, -30000, -30000, static_cast<int>(window.right),
                    static_cast<int>(window.bottom), SWP_NOZORDER | SWP_NOACTIVATE);
                s.compositor.Resize(static_cast<int>(window.right), static_cast<int>(window.bottom));
                auto vm = BuildVm(s, false);
                vm.settings_page = 8;
                vm.settings_scroll = 0;
                auto layout = ui::MakeSettingsLayout(vm, window, scale, s.renderer.TitleBarHeight(), 28 * scale, &painter);
                vm.settings_scroll = layout.explorer_takeover_row.top - layout.content.top - 16 * scale;
                layout = ui::MakeSettingsLayout(vm, window, scale, s.renderer.TitleBarHeight(), 28 * scale, &painter);
                const auto row = layout.explorer_takeover_row;
                const auto geometry = ui::SettingsToggleBounds(row,
                    l10n::Get(l10n::StringId::SettingsExplorerTakeoverDesc), scale, &painter);
                Check(row.top >= layout.content.top && row.bottom <= layout.content.bottom,
                    "takeover row is scrollable into the visible settings area");
                Check(geometry.title.right <= geometry.control.left && geometry.description.bottom <= row.bottom,
                    "localized title, wrapped description and switch fit without overlap");
                const auto hit = s.renderer.HitTest(vm, window,
                    (geometry.control.left + geometry.control.right) / 2,
                    (geometry.control.top + geometry.control.bottom) / 2);
                Check(hit.region == ui::HitTestResult::SettingsToggle && hit.index == 24,
                    "rendered takeover switch hits its independent action");
                for (bool dark : {false, true}) {
                    s.compositor.Dc()->BeginDraw();
                    s.renderer.Render(vm, window, ui::MakeTheme(dark, s.accentColor));
                    Check(SUCCEEDED(s.compositor.Dc()->EndDraw()), "settings renderer completes");
                    const auto name = std::wstring(language) + L"-" + std::to_wstring(static_cast<int>(width)) +
                        L"-" + std::to_wstring(static_cast<int>(scale * 100)) + (dark ? L"-dark.png" : L"-light.png");
                    Check(s.compositor.SaveSnapshot((output / name).c_str()), "capture settings design evidence");
                }
            }
        }
    }
    s.settings.ResetUi();
    s.systemIntegration.Stop();
    s.core.windows.Remove(s.hwnd);
    DestroyWindow(s.hwnd);
    s.hwnd = nullptr;
    owned.reset();
    OleUninitialize();
    std::printf("RESULT checks=%d failures=%d\n", checks, failures);
    return failures ? 1 : 0;
}
