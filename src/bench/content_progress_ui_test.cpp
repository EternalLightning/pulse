#include "../app/app_internal.h"
#include "../app/app_input.h"
#include "../ui/ui_renderer_internal.h"
#include <filesystem>
#include <iostream>
#include <cmath>

int main() {
    using namespace pulse;
    CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    l10n::Initialize(GetModuleHandleW(nullptr),L"zh-CN");
    int failures=0;
    auto check=[&](bool ok,const char* label) {
        std::cout<<(ok ? "[PASS] ":"[FAIL] ")<<label<<std::endl;
        if(!ok) ++failures;
    };
    auto state=std::make_unique<AppState>(); auto& s=*state;
    s.isolatedTest=true; s.appPrefs.persist=false; s.searchHistory.persist=false;
    WNDCLASSW wc{}; wc.hInstance=GetModuleHandleW(nullptr); wc.lpfnWndProc=DefWindowProcW;
    wc.lpszClassName=L"PulseContentProgressFixture"; RegisterClassW(&wc);
    s.hwnd=CreateWindowExW(0,wc.lpszClassName,L"",WS_OVERLAPPEDWINDOW,0,0,1100,720,nullptr,nullptr,wc.hInstance,nullptr);
    if(!s.hwnd || !s.compositor.Init(s.hwnd)) return 2;
    s.renderer.SetCompositor(&s.compositor);
    s.window_tabs.EnsureDefault(); s.pane=s.window_tabs.Active()->FocusedPane();
    auto* tab=ActiveTab(s);
    tab->current_path=app::MakeSearchPath(L"content:季度预算");
    tab->view_mode=ui::ViewMode::Details;
    auto store=std::make_shared<index::ContentResultStore>(nullptr,0);
    index::ContentHit hit{L"C:\\Fixture\\季度预算.txt",L"季度预算.txt",L"季度预算评审会议记录",120,0,1}; hit.file_id=1;
    uint64_t generation=8100;
    auto start=[&] {tab->pending_generation=++generation;tab->search_content_active=true;tab->search_content_stopped=false;tab->content_count_final=false;};
    auto update=[&](uint64_t scanned,uint64_t total,bool done=false,DWORD error=0,bool truncated=false) {
        index::ContentSearchUpdate result; result.progress.generation=generation;
        result.progress.scanned_files=scanned; result.progress.total_files=total;
        result.progress.done=done; result.progress.error=error; result.progress.truncated=truncated;
        result.results=store; ApplyContentSearchUpdate(s,std::move(result));
    };
    const auto output=std::filesystem::absolute(L"../bench_data/content-progress-ui");
    std::filesystem::create_directories(output);
    auto capture=[&](const wchar_t* label, bool hovered=false) {
        for(float scale : {1.0f,1.5f}) for(int width : {720,1100}) for(bool dark : {false,true}) {
            s.scale=scale; s.compositor.RecreateTextFormats(scale); s.renderer.SetScale(scale);
            const auto rect=D2D1::RectF(0,0,width*scale,720*scale);
            s.compositor.Resize(static_cast<UINT>(rect.right),static_cast<UINT>(rect.bottom));
            auto vm=app::BuildWindowViewModel(*s.pane,s.sidebar,true,false,dark,&s.places);
            const auto metrics=ui::MakeStatusBarMetrics(vm,rect,scale,24*scale,s.compositor.DwriteFactory(),s.compositor.SmallFormat());
            if (hovered) {
                vm.hover_region=ui::HitTestResult::StatusBarCancelSearch;
                vm.tooltip_text=l10n::Get(l10n::StringId::ContentCancelSearch);
                vm.tooltip_x=(metrics.cancel_search.left+metrics.cancel_search.right)/2;
                vm.tooltip_y=metrics.cancel_search.top;
            }
            if(vm.status.query_active) {
                check(std::abs((metrics.task.left+metrics.cancel_search.right)/2-(rect.left+rect.right)/2)<0.1f,
                    "search progress and cancel icon are centered together");
                const float available=metrics.task.right-metrics.task.left-112*scale;
                check(ui::MeasureTextWidth(s.compositor.DwriteFactory(),s.compositor.SmallFormat(),vm.status.query_text)<=available,
                    "query label and progress track fit at requested width and DPI");
                check(metrics.task.right <= metrics.cancel_search.left && metrics.task.right > metrics.task.left &&
                    metrics.cancel_search.right <= rect.right && metrics.cancel_search.left >= rect.left,
                    "progress and cancel control do not overlap at requested width and DPI");
                check(ui::StatusBarHitRegion(vm,rect,(metrics.cancel_search.left+metrics.cancel_search.right)/2,
                    (metrics.cancel_search.top+metrics.cancel_search.bottom)/2,scale,24*scale,&s.compositor)==ui::HitTestResult::StatusBarCancelSearch,
                    "cancel control has its own hit target");
                check(ui::StatusBarHitRegion(vm,rect,metrics.task.left+1,metrics.bar.top+1,scale,24*scale,&s.compositor)==ui::HitTestResult::StatusBar,
                    "query progress does not trigger unrelated operation panel");
            }
            const auto theme=ui::MakeTheme(dark,ui::HexColor(0x0078D4));
            s.compositor.Dc()->BeginDraw(); s.renderer.Render(vm,rect,theme);
            check(SUCCEEDED(s.compositor.Dc()->EndDraw()),"actual content renderer completes");
            if(scale==1.0f) {
                const auto file=output/(std::wstring(label)+L"-"+std::to_wstring(width)+(dark ? L"-dark.png":L"-light.png"));
                check(s.compositor.SaveSnapshot(file.c_str()),"content query progress screenshot saved");
            }
        }
    };
    start(); update(0,0);
    check(BuildVm(s,false).status.query_active && BuildVm(s,false).status.query_progress<0,"unknown candidate count uses indeterminate progress");
    check(tab->loading && BuildVm(s,false).status.status_text==L"已匹配 0 个文件","initial empty search exposes active progress and zero matches");
    capture(L"initial");
    check(store->Append({hit}),"isolated synthetic result spool created");
    update(1,0);
    const auto deadline=GetTickCount64()+5000;
    index::ContentResultStore::Row row;
    while(!store->Get(0,row) && GetTickCount64()<deadline) Sleep(2);
    capture(L"unknown");
    update(7200,20000);
    auto vm=BuildVm(s,false);
    check(vm.status.query_active && std::abs(vm.status.query_progress-.36f)<.001f && tab->content_total_files==20000,
        "query candidate denominator reaches view model as 36 percent");
    check(store->Get(0,row) && !tab->loading && vm.status.status_text==L"已匹配 1 个文件","progress updates retain usable list and explicit matched-file count");
    capture(L"known");
    capture(L"known-hover",true);
    l10n::SetLanguage(L"en-US");
    check(l10n::Get(l10n::StringId::ContentCancelSearch)==L"Cancel search · Esc","English cancel tooltip includes shortcut");
    capture(L"known-hover-en",true);
    l10n::SetLanguage(L"zh-CN");
    update(20000,20000,true);
    check(!BuildVm(s,false).status.query_active && tab->content_count_final,"completed query hides active progress and finalizes count");
    capture(L"completed");
    start(); update(400,20000);
    s.addressLiveDue=GetTickCount64()+1;
    const auto cancel_vm=BuildVm(s,false);
    const auto cancel_rect=D2D1::RectF(0,0,static_cast<float>(s.compositor.Width()),static_cast<float>(s.compositor.Height()));
    const auto cancel_metrics=ui::MakeStatusBarMetrics(cancel_vm,cancel_rect,s.scale,24*s.scale,
        s.compositor.DwriteFactory(),s.compositor.SmallFormat());
    HandleLButtonDown(&s,s.hwnd,WM_LBUTTONDOWN,0,MAKELPARAM(
        static_cast<int>((cancel_metrics.cancel_search.left+cancel_metrics.cancel_search.right)/2),
        static_cast<int>((cancel_metrics.cancel_search.top+cancel_metrics.cancel_search.bottom)/2)));
    check(!BuildVm(s,false).status.query_active && !BuildVm(s,false).status.query_cancellable &&
        tab->search_content_stopped && !tab->content_count_final,"cancellation stops refresh and hides control without finalizing partial count");
    capture(L"cancelled");
    TickAddressSearch(s,GetTickCount64()+2000);
    check(!s.addressLiveDue && !tab->pending_generation && tab->search_content_stopped,
        "cancel button clears pending input and timer cannot restart query");
    update(20000,20000,true);
    check(tab->search_content_stopped && !tab->content_count_final,
        "late completed response cannot revive cancelled query");
    start(); update(400,20000,true,ERROR_READ_FAULT);
    check(!BuildVm(s,false).status.query_active && !tab->content_count_final && tab->banner_title==l10n::Get(l10n::StringId::SearchIncomplete),
        "query error hides progress and retains incomplete warning");
    capture(L"error");
    start(); update(400,20000,true,0,true);
    check(!BuildVm(s,false).status.query_active && !tab->content_count_final && tab->banner_title==l10n::Get(l10n::StringId::ResultLimitTitle),
        "truncated query remains incomplete and hides progress");
    s.renderer.SetCompositor(nullptr); s.compositor.Shutdown(); DestroyWindow(s.hwnd); s.hwnd=nullptr;
    CoUninitialize(); return failures ? 1:0;
}
