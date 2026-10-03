#include "../app/app_internal.h"
#include "../ui/ui_renderer_internal.h"
#include "../ui/lumatext_renderer.h"
#include "../common/user_storage.h"
#include <cstdio>
#include <filesystem>

// Production BuildVm -> HitTest -> HandleLButtonDown -> ToggleUi -> Render.
// Only fresh isolated output/storage roots are used; no real SystemWorker/STA,
// preferences, registry, index service or filesystem worker starts.
namespace {
int failures = 0;
void Check(bool ok, const char* label) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
    if (!ok) ++failures;
}
bool SameRect(const D2D1_RECT_F& a, const D2D1_RECT_F& b) {
    return std::abs(a.left-b.left)<0.5f && std::abs(a.right-b.right)<0.5f &&
        std::abs(a.top-b.top)<0.5f && std::abs(a.bottom-b.bottom)<0.5f;
}
}
int main() {
    using namespace pulse;
    using I = l10n::StringId;
    using H = ui::HitTestResult;
    OleInitialize(nullptr);
    wchar_t root[32768]{};
    const DWORD specified=GetEnvironmentVariableW(L"PULSE_TEST_FIXTURE_ROOT",root,ARRAYSIZE(root));
    if(specified>=ARRAYSIZE(root) || (!specified && !GetTempPathW(ARRAYSIZE(root),root))) return 2;
    const auto output=std::filesystem::path(root)/(L"pulse-settings-toggle-"+
        std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));
    std::filesystem::create_directories(output);
    storage::OverrideDefaultRootForTesting((output/L"isolated-data").wstring());
    l10n::Initialize(GetModuleHandleW(nullptr),L"en-US");
    auto owned=std::make_unique<AppState>(); auto& s=*owned;
    s.isolatedTest=true; s.darkMode=true;
    s.appPrefs.persist=s.places.persist=s.ctxMenuPrefs.persist=s.searchHistory.persist=false;
    s.hwnd=CreateWindowExW(0,L"STATIC",L"",WS_OVERLAPPEDWINDOW,-20000,-20000,1100,900,nullptr,nullptr,nullptr,nullptr);
    if(!s.hwnd || !s.compositor.Init(s.hwnd)) return 2;
    s.renderer.SetCompositor(&s.compositor);
    s.window_tabs.EnsureDefault(); s.pane=s.window_tabs.Active()->FocusedPane();
    s.pane->NewTab(app::MakeSettingsPath(L"general"));
    ActiveTab(s)->loading=false;
    ActiveTab(s)->SetSnapshot(std::make_shared<std::vector<fs::DirEntry>>());
    s.settingsExpanded=1;
    int system_requests=0;
    app::SettingsController::UiCallbacks callbacks;
    callbacks.apply_effects=[&](app::SettingsEffect effect) { ApplySettingsEffects(s,effect); };
    callbacks.toggle_system_integration=[&](int control) {
        ++system_requests;
        // Deliberately use only isolated memory. This emulates successful
        // settings completion, not a claim about COM/registry integration.
        if(control==21) {
            const bool on=!(s.appPrefs.open_folders_in_pulse && s.appPrefs.take_over_win_e && s.appPrefs.take_over_this_pc);
            s.appPrefs.open_folders_in_pulse=s.appPrefs.take_over_win_e=s.appPrefs.take_over_this_pc=on;
        } else if(control==3) s.appPrefs.open_folders_in_pulse=!s.appPrefs.open_folders_in_pulse;
        else if(control==22) s.appPrefs.take_over_win_e=!s.appPrefs.take_over_win_e;
        else if(control==23) s.appPrefs.take_over_this_pc=!s.appPrefs.take_over_this_pc;
        else if(control==24) s.appPrefs.experimental_explorer_takeover=!s.appPrefs.experimental_explorer_takeover;
    };
    s.settings.BindUi(s.appPrefs,s.ctxMenuPrefs,s.index,s.networkIndex,std::move(callbacks));
    const auto get_row=[](const ui::SettingsLayout& l,int control) {
        switch(control) {
        case 1:return l.startup_row[0]; case 2:return l.startup_row[1]; case 31:return l.startup_tray_row;
        case 32:return l.last_tab_row; case 21:return l.startup_row[2]; case 3:return l.default_manager_rows[0];
        case 22:return l.default_manager_rows[1]; case 23:return l.default_manager_rows[2]; case 24:return l.default_manager_rows[3];
        case 20:return l.new_tab_row; case 5:return l.hidden_files_row; default:return l.performance_row;
        }
    };
    ui::fluent::Painter measuring(&s.compositor);
    for(const auto* language:{L"zh-CN",L"en-US"}) for(float scale:{1.0f,1.5f,2.0f}) {
        s.appPrefs.language=language;
        l10n::SetLanguage(language); ui::typography::InvalidateCaches();
        s.compositor.RecreateTextFormats(scale); s.renderer.InvalidateTypography();
        s.scale=scale; s.renderer.SetScale(scale); measuring.SetScale(scale);
        for(float width:{1100.0f,620.0f}) {
            s.compositor.Resize(static_cast<UINT>(width*scale),static_cast<UINT>(900*scale));
            const auto theme=ui::MakeTheme(true,ui::HexColor(0x0078D4));
            measuring.BeginFrame(theme);
            const auto rect=D2D1::RectF(0,0,width*scale,900*scale);
            s.settings.SetScroll(0,100000);
            auto vm=BuildVm(s,false);
            auto l=ui::MakeSettingsLayout(vm,rect,scale,s.renderer.TitleBarHeight(),28.0f*s.scale,&measuring);
            const I descriptions[]={I::SettingsLaunchDesc,I::SettingsKeepRunningDesc,I::StartToTrayDesc,I::CloseLastTabWindowDesc,
                I::SettingsDefaultManagerDesc,I::SettingsOpenFoldersDesc,I::SettingsTakeoverWinEDesc,I::SettingsTakeoverThisPcDesc,
                I::SettingsExplorerTakeoverDesc,I::SettingsNewTabHomeDesc};
            const int controls[]={1,2,31,32,21,3,22,23,24,20};
            for(size_t i=0;i<std::size(controls);++i) {
                const auto row=get_row(l,controls[i]);
                const auto bounds=ui::SettingsToggleBounds(row,l10n::Get(descriptions[i]),scale,&measuring);
                const float expected=ui::SettingsToggleHeight(row.right-row.left,l10n::Get(descriptions[i]),scale,&measuring);
                Check(std::abs(row.bottom-row.top-expected)<0.5f && bounds.description.bottom<=row.bottom-9*scale &&
                    std::abs((bounds.control.top+bounds.control.bottom)-(row.top+row.bottom))<0.5f,
                    "switch rows use actual caption height, consistent padding and centered controls");
                if(expected<=64*scale+0.5f)
                    Check(std::abs(row.bottom-row.top-64*scale)<0.5f,"short switch rows share the 64 DIP minimum without a blank line");
            }
            vm.settings_system_pending=true; vm.settings_system_status=l10n::Get(I::SettingsSystemApplying);
            const auto pending=ui::MakeSettingsLayout(vm,rect,scale,s.renderer.TitleBarHeight(),28.0f*s.scale,&measuring);
            Check(SameRect(l.new_tab_row,pending.new_tab_row) && SameRect(l.performance_row,pending.performance_row) &&
                l.content_h==pending.content_h && pending.system_status.bottom==pending.system_status.top,
                "pending system update never inserts a row or shifts unrelated controls");
            auto context=vm; context.settings_page=2; context.settings_system_pending=false;
            const auto c=ui::MakeSettingsLayout(context,rect,scale,s.renderer.TitleBarHeight(),28.0f*s.scale,&measuring);
            const auto cb=ui::SettingsToggleBounds(c.context_header[5],l10n::Get(app::builtin_text::GroupDescription),scale,&measuring,112);
            Check(cb.description.bottom<=c.context_header[5].bottom-9*scale &&
                std::abs(cb.control.top+cb.control.bottom-c.context_header[5].top-c.context_header[5].bottom)<0.5f,
                "context group switches use the same measured wrapping and center policy");
        }
    }

    // Native production switch events and frames at one useful screenshot scale.
    s.scale=1.5f; s.renderer.SetScale(s.scale); measuring.SetScale(s.scale);
    s.appPrefs.language=L"zh-CN"; l10n::SetLanguage(L"zh-CN"); ui::typography::InvalidateCaches();
    s.compositor.RecreateTextFormats(s.scale); s.renderer.InvalidateTypography();
    s.compositor.Resize(1650,1350);
    const auto theme=ui::MakeTheme(true,ui::HexColor(0x0078D4)); measuring.BeginFrame(theme);
    // A fixture-generated image exercises tinted/wallpaper redraw without
    // depending on a user's image or DWM backdrop capture.
    const auto wallpaper=output/L"fixture-wallpaper.png";
    s.compositor.Dc()->BeginDraw();
    s.compositor.Dc()->Clear(D2D1::ColorF(0.15f,0.25f,0.4f,1.0f));
    Check(SUCCEEDED(s.compositor.Dc()->EndDraw()) && s.compositor.SaveSnapshot(wallpaper.c_str()),
        "isolated wallpaper fixture generated");
    s.appPrefs.window_effect=L"none"; s.appPrefs.background_image=wallpaper.wstring();
    const auto rect=D2D1::RectF(0,0,1650,1350);
    const auto render=[&](ui::WindowViewModel vm,const std::wstring& name) {
        s.compositor.Dc()->BeginDraw();
        s.renderer.Render(vm,rect,theme);
        Check(SUCCEEDED(s.compositor.Dc()->EndDraw()),"production settings frame draws successfully");
        Check(s.compositor.SaveSnapshot((output/(name+L".png")).c_str()),"settings before/pending/after frame snapshot saved");
    };
    const auto state=[&](int control) {
        switch(control) {
        case 1:return s.appPrefs.launch_on_startup; case 2:return s.appPrefs.keep_running_on_close;
        case 31:return s.appPrefs.start_to_tray; case 32:return s.appPrefs.close_last_tab_window;
        case 20:return s.appPrefs.new_tab_home; case 5:return s.appPrefs.show_hidden_files;
        case 21:return s.appPrefs.open_folders_in_pulse && s.appPrefs.take_over_win_e && s.appPrefs.take_over_this_pc;
        case 3:return s.appPrefs.open_folders_in_pulse; case 22:return s.appPrefs.take_over_win_e;
        case 23:return s.appPrefs.take_over_this_pc; default:return s.appPrefs.experimental_explorer_takeover;
        }
    };
    for(int control:{2,1,31,32,20,5,21,3,22,23,24}) {
        s.settings.SetScroll(0,100000);
        auto initial=BuildVm(s,false);
        auto layout=ui::MakeSettingsLayout(initial,rect,s.scale,s.renderer.TitleBarHeight(),28.0f*s.scale,&measuring);
        const auto raw=get_row(layout,control);
        s.settings.SetScroll(raw.top-layout.content.top-20*s.scale,100000);
        auto before=BuildVm(s,false);
        layout=ui::MakeSettingsLayout(before,rect,s.scale,s.renderer.TitleBarHeight(),28.0f*s.scale,&measuring);
        const auto row=get_row(layout,control);
        const float x=row.right-35*s.scale,y=(row.top+row.bottom)*0.5f;
        const auto hit=s.renderer.HitTest(before,rect,x,y);
        Check(hit.region==H::SettingsToggle && hit.index==control,"shared settings layout matches the production switch hit target");
        const auto prefix=L"control-"+std::to_wstring(control);
        render(before,prefix+L"-cold"); render(before,prefix+L"-before");
        const auto generation=ui::typography::Generation();
        auto* const font=s.compositor.TextFormat();
        if(control==21 || control==3 || control==22 || control==23 || control==24) {
            auto busy=before; busy.settings_system_pending=true; busy.settings_system_status=l10n::Get(I::SettingsSystemApplying);
            render(busy,prefix+L"-pending");
            const auto nohit=s.renderer.HitTest(busy,rect,x,y);
            Check(nohit.region!=H::SettingsToggle || nohit.index!=control,"pending system switch refuses duplicate native hit");
        }
        const bool checked=state(control);
        HandleLButtonDown(&s,s.hwnd,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(static_cast<int>(x),static_cast<int>(y)));
        Check(state(control)!=checked,"native production input toggles the intended isolated preference");
        auto after=BuildVm(s,false);
        const auto after_layout=ui::MakeSettingsLayout(after,rect,s.scale,s.renderer.TitleBarHeight(),28.0f*s.scale,&measuring);
        Check(SameRect(get_row(layout,control),get_row(after_layout,control)) && SameRect(layout.new_tab_row,after_layout.new_tab_row),
            "native switch toggle leaves unaffected settings geometry unchanged");
        Check(ui::typography::Generation()==generation && s.compositor.TextFormat()==font,
            "ordinary switch effects never rebuild typography or formats");
        render(after,prefix+L"-after"); render(after,prefix+L"-warm");
    }
    const auto pending_generation=ui::typography::Generation();
    const auto options_before=s.appPrefs.ToJson();
    s.systemIntegration.Toggle(s,21);
    Check(!s.systemIntegration.pending() && s.appPrefs.ToJson()==options_before &&
        ui::typography::Generation()==pending_generation,"unstarted isolated SystemIntegration never accesses or changes registry/preferences");
    Check(system_requests==5,"all system switches route to isolated completion callback without real worker");
    Check(l10n::Get(I::SettingsOpenFolders).find(L"默认")==std::wstring::npos &&
        l10n::Get(I::SettingsOpenFoldersDesc).find(L"Win+E")==std::wstring::npos,
        "folder sub-option wording cannot contradict independent Win+E state");
    if(const auto* stats=s.compositor.GetLumaTextStats())
        std::printf("Luma draws=%llu hits=%llu fallbacks=%llu (diagnostic only; screenshots need visual review)\n",
            static_cast<unsigned long long>(stats->draw_calls),static_cast<unsigned long long>(stats->cache_hits),
            static_cast<unsigned long long>(stats->fallback_draws));
    std::wprintf(L"Fixture snapshots: %ls\n",output.c_str());
    s.settings.ResetUi(); s.watches.Stop();
    s.renderer.SetCompositor(nullptr);
    DestroyWindow(s.hwnd); s.hwnd=nullptr;
    owned.reset(); OleUninitialize();
    return failures ? 1 : 0;
}
