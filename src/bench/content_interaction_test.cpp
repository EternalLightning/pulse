#include "../app/app_internal.h"
#include "../app/search_query.h"
#include "../app/content_results_ui.h"
#include "../common/localization.h"
#include "../ui/ui_renderer_internal.h"
#include <array>
#include <cstdio>

int wmain() {
    using namespace pulse;
    OleInitialize(nullptr);
    l10n::Initialize(GetModuleHandleW(nullptr),L"zh-CN");
    auto owned=std::make_unique<AppState>(); auto& s=*owned;
    s.isolatedTest=true; s.appPrefs.persist=false;
    s.accentColor=ui::HexColor(0x0078D4);
    s.hwnd=CreateWindowExW(0,L"STATIC",L"",WS_OVERLAPPEDWINDOW,0,0,1100,640,nullptr,nullptr,nullptr,nullptr);
    s.window_tabs.EnsureDefault(); s.pane=s.window_tabs.Active()->FocusedPane();
    auto& tab=*ActiveTab(s);
    tab.SetSnapshot(std::make_shared<std::vector<fs::DirEntry>>());
    tab.current_path=app::MakeSearchPath(L"content:needle");
    index::ContentHit a{L"C:\\fixture\\a.txt",L"a.txt",L"needle",100,0,1}; a.file_id=11;
    index::ContentHit b{L"C:\\fixture\\b.txt",L"b.txt",L"needle",300,0,1}; b.file_id=12;
    index::ContentHit c{L"C:\\fixture\\c.txt",L"c.txt",L"needle",200,0,1}; c.file_id=13;
    int failures=0;
    auto check=[&](bool ok,const char* text) { printf("[%s] %s\n",ok?"PASS":"FAIL",text); fflush(stdout); if(!ok) ++failures; };
    tab.pending_generation=700; tab.search_live_generation=700; tab.search_content_active=true;
    SetSort(s,ui::SortColumn::Name,ui::SortDirection::Desc);
    check(!tab.content_results && tab.content_sort_override && tab.search_content_active &&
        tab.pending_generation==700 && tab.search_live_generation==700,
        "sorting before the first batch preserves the active query and defers ordering");
    tab.SetBannerTitle(app::BannerType::ContentIndex, l10n::Get(l10n::StringId::ContentIndexBuilding));
    tab.banner_message=L"background indexing";
    TickAddressSearch(s,GetTickCount64()+750);
    check(tab.banner_title.empty() && tab.banner_message.empty() && tab.pending_generation==700,
        "foreground query suppresses background-index banner without restarting");
    const auto store=std::make_shared<index::ContentResultStore>(nullptr,0);
    check(store->Append({a,b,c}),"seed matched files");
    index::ContentSearchUpdate first_batch;
    first_batch.progress.generation=700; first_batch.progress.scanned_files=3; first_batch.progress.total_files=12;
    first_batch.results=store; ApplyContentSearchUpdate(s,std::move(first_batch));
    const auto first_deadline=GetTickCount64()+5000;
    index::ContentResultStore::Row first_row;
    while((store->Sorting() || !store->Get(0,first_row)) && GetTickCount64()<first_deadline) Sleep(2);
    check(!store->Sorting() && store->Get(0,first_row) && first_row.file_id==13 && tab.pending_generation==700,
        "first result batch applies deferred descending name sort without rescanning");
    const auto progress_vm=app::BuildWindowViewModel(*s.pane,s.sidebar,true,false,false,&s.places);
    check(progress_vm.status.query_active && progress_vm.status.query_progress==0.25f &&
        progress_vm.status.query_text.starts_with(L"25%") && progress_vm.status.query_text.find(L"3 / 12")!=std::wstring::npos &&
        progress_vm.status.status_text==L"已匹配 3 个文件",
        "foreground progress uses actual scanned count and denominator alongside matching files");
    for (const auto [scanned,total,percent] : {std::array<uint64_t,3>{0,100,0}, std::array<uint64_t,3>{29,100,29},
        std::array<uint64_t,3>{9999,10000,99}, std::array<uint64_t,3>{100,100,100}}) {
        tab.content_scanned_files=scanned; tab.content_total_files=total;
        const auto vm=app::BuildWindowViewModel(*s.pane,s.sidebar,true,false,false,&s.places);
        check(vm.status.query_active && vm.status.query_text.starts_with(std::to_wstring(percent)+L"%"),
            "determinate progress exposes an integer percentage without rounding early to 100");
    }
    tab.content_scanned_files=3; tab.content_total_files=0;
    const auto unknown_vm=app::BuildWindowViewModel(*s.pane,s.sidebar,true,false,false,&s.places);
    check(unknown_vm.status.query_active && unknown_vm.status.query_progress<0 &&
        unknown_vm.status.query_text.find(L'%')==std::wstring::npos,
        "unknown candidate count keeps activity progress without a fabricated percentage");
    tab.search_content_active=false;
    tab.pending_generation=700; tab.search_live_generation=700; tab.content_count_final=true;
    RefreshContentResults(s);
    SetSort(s,ui::SortColumn::Size,ui::SortDirection::Desc);
    check(tab.content_results==store && tab.pending_generation==700 && tab.search_live_generation==700,
        "column sort retains query generation and result store");
    const auto deadline=GetTickCount64()+5000;
    index::ContentResultStore::Row row;
    while((store->Sorting() || !store->Get(0,row)) && GetTickCount64()<deadline) Sleep(2);
    RefreshContentResults(s);
    check(!store->Sorting() && store->Get(0,row) && row.file_id==12,"descending sort updates first row");
    check(tab.search_total==3 && tab.virtual_title.find(L"3")!=std::wstring::npos,
        "completed content search retains matching file count");
    tab.search_content_active=true; tab.content_count_final=false; tab.content_scanned_files=27; tab.content_total_files=100;
    RefreshContentResults(s);
    check(!tab.loading && tab.virtual_title.find(L"3")!=std::wstring::npos && tab.virtual_title.find(L"27")!=std::wstring::npos,
        "in-progress search keeps populated list and counts visible");
    wchar_t shot[32768]{};
    if(GetEnvironmentVariableW(L"PULSE_CONTENT_UI_SHOT",shot,ARRAYSIZE(shot))) {
        check(s.compositor.Init(s.hwnd),"initialize isolated visual fixture");
        if(s.compositor.Dc()) {
            s.renderer.SetCompositor(&s.compositor);
            tab.ClearBanner();
            for(float scale : {1.0f,1.5f,2.0f}) for(int width : {1100,720}) {
                s.compositor.RecreateTextFormats(scale);
                s.renderer.SetScale(scale); s.scale=scale;
                const auto rect=D2D1::RectF(0,0,width*scale,640*scale);
                s.compositor.Resize(static_cast<UINT>(rect.right),static_cast<UINT>(rect.bottom));
                printf("[INFO] build visual model %d\n",width); fflush(stdout);
                const auto vm=BuildVm(s,false);
                const auto metrics=ui::MakeStatusBarMetrics(vm,rect,scale,24*scale,
                    s.compositor.DwriteFactory(),s.compositor.SmallFormat());
                const float task_width=metrics.task.right-metrics.task.left;
                const float track_width=std::min(100*scale,std::max(0.0f,task_width*0.30f));
                const float text_width=std::max(0.0f,task_width-track_width-8*scale);
                check(ui::MeasureTextWidth(s.compositor.DwriteFactory(),s.compositor.SmallFormat(),vm.status.query_text)<=text_width,
                    "percentage and candidate counts fit beside the track at requested width and DPI");
                tab.content_scanned_files=9999999; tab.content_total_files=10000000;
                const auto large_vm=app::BuildWindowViewModel(*s.pane,s.sidebar,true,false,false,&s.places);
                check(ui::MeasureTextWidth(s.compositor.DwriteFactory(),s.compositor.SmallFormat(),large_vm.status.query_text)<=text_width,
                    "percentage and ten-million candidate counts fit without clipping");
                tab.content_scanned_files=27; tab.content_total_files=100;
                for(bool unknown : {false,true}) {
                    tab.content_total_files=unknown ? 0 : 100;
                    const auto shot_vm=BuildVm(s,false);
                    const float measured=ui::MeasureTextWidth(s.compositor.DwriteFactory(),s.compositor.SmallFormat(),shot_vm.status.query_text);
                    const float label_right=metrics.task.left+std::min(measured,text_width);
                    const float track_left=label_right+8*scale;
                    check(measured<=text_width && track_left+track_width<=metrics.task.right+0.01f,
                        "known and unknown query text leave an eight-DIP adjacent track inside the status area");
                    printf("[INFO] render visual model\n"); fflush(stdout);
                    auto* dc=s.compositor.Dc(); dc->BeginDraw(); dc->Clear(D2D1::ColorF(0.08f,0.08f,0.08f));
                    s.renderer.Render(shot_vm,rect,ui::MakeTheme(true,s.accentColor));
                    const auto result=dc->EndDraw();
                    const auto path=std::wstring(shot)+L"-"+std::to_wstring(width)+
                        (scale==1.0f ? L"" : L"-"+std::to_wstring(static_cast<int>(scale*100)))+
                        (unknown ? L"-unknown" : L"")+L".png";
                    check(SUCCEEDED(result) && s.compositor.SaveSnapshot(path.c_str()),"render progress track beside its query label");
                }
                tab.content_total_files=100;
            }
        }
    }
    tab.search_content_active=false;
    DestroyWindow(s.hwnd); s.hwnd=nullptr;
    printf("failures=%d\n",failures);
    return failures ? 1 : 0;
}
