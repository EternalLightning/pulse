#pragma once

#include "../common/user_storage.h"
#include "../app/app_navigation.h"
#include "../app/app_input.h"

int RunSettingsCategories() {
    using namespace pulse;
    using I=l10n::StringId;
    using H=ui::HitTestResult;
    std::cout<<"[INFO] starting settings category fixture"<<std::endl;
    if(FAILED(OleInitialize(nullptr))) return 2;
    const auto output=std::filesystem::absolute(L"docs/validation/settings-categories-20261028");
    std::filesystem::create_directories(output);
    storage::OverrideDefaultRootForTesting((output/L"isolated-data").wstring());
    l10n::Initialize(GetModuleHandleW(nullptr),L"zh-CN");
    int checks=0,failures=0;
    auto check=[&](bool ok,const char* label) {
        ++checks;
        if(!ok) { ++failures; std::cout<<"[FAIL] "<<label<<'\n'; }
    };
    auto owned=std::make_unique<AppState>();
    auto& state=*owned;
    state.isolatedTest=true;state.shot.active=true;
    state.appPrefs.persist=state.places.persist=state.ctxMenuPrefs.persist=state.searchHistory.persist=false;
    state.hwnd=CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE,L"STATIC",L"Settings categories fixture",
        WS_POPUP,-20000,-20000,1100,900,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    if(!state.hwnd || !state.compositor.Init(state.hwnd)) return 2;
    state.renderer.SetCompositor(&state.compositor);
    state.renderer.SetDetailsPanelVisible(false);
    state.window_tabs.EnsureDefault();BindCurrentLayout(state);
    state.pane->NewTab(app::MakeSettingsPath(L"general"));
    ActiveTab(state)->loading=false;
    ActiveTab(state)->SetSnapshot(std::make_shared<std::vector<fs::DirEntry>>());
    app::SettingsController::UiCallbacks callbacks;
    callbacks.apply_effects=[&](app::SettingsEffect effect) { ApplySettingsEffects(state,effect); };
    state.settings.BindUi(state.appPrefs,state.ctxMenuPrefs,state.index,state.networkIndex,std::move(callbacks));
    ui::fluent::Painter measuring(&state.compositor);
    const int pages[]={0,6,7,8,9};
    for(int page=0;page<10;++page) {
        state.settings.SelectPage(page);
        check(state.settings.page()==page,"controller selects every category and legacy page");
        check(app::SettingsController::PageFromName(app::SettingsController::PageName(page))==page,
            "virtual settings path round trips through session page names");
    }
    struct Control { int page; D2D1_RECT_F ui::SettingsLayout::*row; H::Region region; int index; I title; };
    const Control controls[]={
        {0,&ui::SettingsLayout::accent_picker,H::SettingsAccent,0,I::SettingsThemeColor},
        {0,&ui::SettingsLayout::language_choice,H::SettingsDropdown,1,I::SettingsLanguage},
        {0,&ui::SettingsLayout::wallpaper_choose,H::SettingsWallpaper,0,I::SettingsWallpaper},
        {0,&ui::SettingsLayout::wallpaper_look_value,H::SettingsWallpaperLook,0,I::SettingsWallpaperLook},
        {6,&ui::SettingsLayout::blank_click_row,H::SettingsToggle,7,I::SettingsBlankClickBack},
        {6,&ui::SettingsLayout::hidden_files_row,H::SettingsToggle,5,I::SettingsShowHidden},
        {6,&ui::SettingsLayout::protected_files_row,H::SettingsToggle,16,I::SettingsShowProtected},
        {6,&ui::SettingsLayout::performance_row,H::SettingsToggle,4,I::SettingsShowPerformance},
        {7,&ui::SettingsLayout::new_tab_row,H::SettingsToggle,20,I::SettingsNewTabHome},
        {7,&ui::SettingsLayout::external_open_choice,H::SettingsDropdown,2,I::SettingsExternalOpen},
        {7,&ui::SettingsLayout::pinned_names_row,H::SettingsToggle,6,I::PinnedNames},
        {7,&ui::SettingsLayout::last_tab_row,H::SettingsToggle,32,I::CloseLastTabWindow},
        {7,&ui::SettingsLayout::window_size_lock_row,H::SettingsToggle,34,I::SettingsLockWindowSize},
        {8,&ui::SettingsLayout::explorer_takeover_row,H::SettingsToggle,24,I::SettingsExplorerTakeover},
        {8,&ui::SettingsLayout::text_compare_row,H::None,0,I::SettingsTextCompare},
        {8,&ui::SettingsLayout::startup_tray_row,H::SettingsToggle,31,I::StartToTray},
        {9,&ui::SettingsLayout::text_compare_row,H::SettingsToggle,33,I::SettingsTextCompare},
        {9,&ui::SettingsLayout::change_tracking_row,H::SettingsToggle,8,I::SettingsChangeTracking},
    };
    for(const auto* language:{L"zh-CN",L"en-US"}) for(float scale:{1.0f,1.5f})
    for(float width:{620.0f,1100.0f}) for(float height:{520.0f,900.0f}) {
        l10n::SetLanguage(language); state.appPrefs.language=language;
        state.scale=scale;state.renderer.SetScale(scale);measuring.SetScale(scale);
        state.compositor.RecreateTextFormats(scale);state.renderer.InvalidateTypography();
        state.compositor.Resize(static_cast<int>(width*scale),static_cast<int>(height*scale));
        const auto rect=D2D1::RectF(0,0,width*scale,height*scale);
        for(int page:pages) {
            OpenSettingsTab(state,page);
            auto vm=BuildVm(state,false);vm.settings_scroll=0;vm.settings_expanded=0;
            auto layout=ui::MakeSettingsLayout(vm,rect,scale,state.renderer.TitleBarHeight(),28*scale,&measuring);
            check(layout.section_count>=2 && layout.section_count<=3,"only populated category sections appear");
            float previous=layout.nav.top;
            for(int nav:ui::kSettingsNavOrder) {
                const auto row=layout.nav_row[nav];
                check(row.top>=previous && row.bottom<=layout.nav.bottom,"category navigation stays ordered and visible");
                const auto hit=state.renderer.HitTest(vm,rect,(row.left+row.right)/2,(row.top+row.bottom)/2);
                check(hit.region==H::SettingsNav && hit.index==nav,"all sidebar categories hit their actual page IDs");
                previous=row.bottom;
            }
            check(layout.nav_row[3].top>previous && layout.nav_row[3].bottom<=layout.nav.bottom,
                "about remains reachable in short windows");
            for(int section=0;section<layout.section_count;++section) {
                check(!l10n::Get(layout.section_title[section]).empty(),"section heading is localized");
                check(layout.group[section].top>=layout.section[section].bottom &&
                    layout.group[section].bottom>layout.group[section].top,"section card contains existing settings");
                if(section) check(layout.section[section].top>layout.group[section-1].bottom,
                    "section headings never overlap the previous card");
            }
            for(const auto& control:controls) if(control.page==page) {
                const auto row=layout.*(control.row);
                if(control.region==H::None) {
                    check(row.bottom==row.top,"settings are absent from unrelated categories");continue;
                }
                check(row.bottom>row.top,"existing control is present without advanced disclosure");
                vm.settings_scroll=std::max(0.0f,row.top-layout.content.top-24*scale);
                const auto visible=ui::MakeSettingsLayout(vm,rect,scale,state.renderer.TitleBarHeight(),28*scale,&measuring);
                const auto target=visible.*(control.row);
                check(target.top>=visible.content.top && target.bottom<=visible.content.bottom,"moved control can scroll into view");
                const auto hit=state.renderer.HitTest(vm,rect,(target.left+target.right)/2,(target.top+target.bottom)/2);
                check(hit.region==control.region && hit.index==control.index,"moved control retains its production action");
                auto origin=vm;origin.settings_scroll=0;
                auto search_target=row;
                if(control.title==I::SettingsThemeColor) search_target=layout.accent_card;
                if(control.title==I::SettingsLanguage) search_target=layout.language_card;
                if(control.title==I::SettingsWallpaper) search_target=layout.wallpaper_card;
                if(control.title==I::SettingsWallpaperLook) search_target=layout.wallpaper_look_card;
                if(control.title==I::SettingsExternalOpen) search_target=layout.external_open_card;
                const float expected=std::max(0.0f,search_target.top-layout.content_origin-20*scale);
                check(std::abs(state.renderer.SettingsDestinationOffset(origin,static_cast<int>(control.title),rect.right,rect.bottom)-expected)<0.5f,
                    "setting search scrolls to its actual row on the new page");
            }
            struct Segment { D2D1_RECT_F bounds; H::Region region; int index; I title; };
            std::vector<Segment> segments;
            if(page==0) {
                segments={{layout.theme_tile[0],H::SettingsTheme,1,I::SettingsTheme},
                    {layout.text_render_row[2],H::SettingsTextRender,2,I::SettingsTextRender},
                    {layout.wallpaper_blur_row[1],H::SettingsWallpaperBlur,1,I::SettingsWallpaperBlur}};
                for(int i=0;i<ui::kWindowEffectCount;++i) segments.push_back({layout.effect_row[i],H::SettingsEffect,i,I::SettingsWindowEffect});
            } else if(page==6) {
                segments={{layout.density_row[2],H::SettingsDensity,2,I::SettingsRowHeight},
                    {layout.folder_sort_row[1],H::SettingsFolderSort,1,I::SettingsFolderSort}};
                const I titles[]={I::ListSmartDate,I::ListZebraRows,I::ListSizeBar};
                for(int i=0;i<3;++i) segments.push_back({layout.list_style_row[i],H::SettingsToggle,17+i,titles[i]});
            } else if(page==8) {
                segments={{layout.startup_row[0],H::SettingsToggle,1,I::SettingsLaunch},
                    {layout.startup_row[1],H::SettingsToggle,2,I::SettingsKeepRunning},
                    {layout.startup_row[2],H::SettingsToggle,21,I::SettingsDefaultManager},
                    {layout.configuration_action[0],H::SettingsConfigurationAction,0,I::ConfigurationLocation},
                    {layout.configuration_action[1],H::SettingsConfigurationAction,1,I::ConfigurationLocation}};
            } else if(page==9) {
                segments={{layout.tray_icon_row[1],H::SettingsTrayIcon,1,I::SettingsTrayIcon},
                    {layout.change_days[2],H::SettingsChangeDays,2,I::SettingsChangeDays}};
            }
            for(const auto& segment:segments) {
                vm.settings_scroll=std::max(0.0f,segment.bounds.top-layout.content.top-24*scale);
                const auto hit=state.renderer.HitTest(vm,rect,(segment.bounds.left+segment.bounds.right)/2,
                    (segment.bounds.top+segment.bounds.bottom)/2-vm.settings_scroll);
                check(hit.region==segment.region && hit.index==segment.index,"moved choices and actions retain their production hit targets");
            }
            if(height==900) for(bool dark:{false,true}) {
                vm.settings_scroll=0;vm.dark=dark;
                const auto theme=ui::MakeTheme(dark,ui::HexColor(0x0078D4));
                state.compositor.Dc()->BeginDraw();state.renderer.Render(vm,rect,theme);
                check(SUCCEEDED(state.compositor.Dc()->EndDraw()),"production category renders successfully");
                const auto filename=std::wstring(language)+L"-"+std::to_wstring(page)+L"-"+
                    std::to_wstring(static_cast<int>(width))+L"-"+std::to_wstring(static_cast<int>(scale*100))+
                    (dark?L"-dark.png":L"-light.png");
                check(bench::SaveSnapshot(state.compositor,output/filename),"capture category layout for visual review");
                if(std::wstring_view(language)==L"zh-CN" && dark) {
                    vm.settings_scroll=state.renderer.SettingsMaxScroll(vm,rect.right,rect.bottom);
                    state.compositor.Dc()->BeginDraw();state.renderer.Render(vm,rect,theme);
                    check(SUCCEEDED(state.compositor.Dc()->EndDraw()),"scrolled category renders successfully");
                    check(bench::SaveSnapshot(state.compositor,output/(L"bottom-"+filename)),"capture lower category sections");
                }
            }
        }
    }
    state.scale=1;state.renderer.SetScale(1);measuring.SetScale(1);
    state.compositor.RecreateTextFormats(1);state.compositor.Resize(1100,900);
    const auto rect=D2D1::RectF(0,0,1100,900);
    for(const auto& control:controls) if(control.region==H::SettingsToggle &&
        (control.index==5 || control.index==6 || control.index==33 || control.index==8)) {
        OpenSettingsTab(state,control.page);
        auto vm=BuildVm(state,false);
        auto layout=ui::MakeSettingsLayout(vm,rect,1,state.renderer.TitleBarHeight(),28,&measuring);
        state.settings.SetScroll(std::max(0.0f,(layout.*(control.row)).top-layout.content.top-24),10000);
        vm=BuildVm(state,false);
        layout=ui::MakeSettingsLayout(vm,rect,1,state.renderer.TitleBarHeight(),28,&measuring);
        const auto row=layout.*(control.row);
        const auto before=state.appPrefs.ToJson();
        const int x=static_cast<int>(row.right-35),y=static_cast<int>((row.top+row.bottom)/2);
        HandleLButtonDown(&state,state.hwnd,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(x,y));
        check(state.appPrefs.ToJson()!=before,"native click applies a moved preference through the production controller");
        HandleLButtonDown(&state,state.hwnd,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(x,y));
        check(state.appPrefs.ToJson()==before,"second native click restores the isolated preference");
    }
    state.settings.ResetUi();
    state.renderer.SetCompositor(nullptr);state.compositor.Shutdown();DestroyWindow(state.hwnd);state.hwnd=nullptr;
    owned.reset();OleUninitialize();
    std::cout<<"[SUMMARY] "<<checks-failures<<" PASS / "<<failures<<" FAIL\n";
    return failures?1:0;
}
