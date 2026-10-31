// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "../app/app_internal.h"
#include "../common/localization.h"
#include "../ui/typography.h"
#include <iostream>

namespace {
struct Messages { int margins=0,positions=0,redraw=0,hidden=0,text=0,size=0,selection=0; bool trace=false; };
LRESULT CALLBACK Observe(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp,UINT_PTR,DWORD_PTR data) {
    auto& counts=*reinterpret_cast<Messages*>(data);
    if(msg==EM_SETMARGINS) ++counts.margins;
    if(msg==WM_WINDOWPOSCHANGED) ++counts.positions;
    if(msg==WM_SETREDRAW) ++counts.redraw;
    if(msg==WM_SETTEXT) ++counts.text;
    if(msg==EM_SETSEL) {
        ++counts.selection;
        if(counts.trace) std::cout<<"[TRACE] EM_SETSEL "<<wp<<","<<lp<<std::endl;
    }
    if(msg==WM_SIZE) ++counts.size;
    const auto result=DefSubclassProc(hwnd,msg,wp,lp);
    if(counts.trace && msg==WM_CHAR) {
        DWORD a=0,b=0;SendMessageW(hwnd,EM_GETSEL,reinterpret_cast<WPARAM>(&a),reinterpret_cast<LPARAM>(&b));
        std::cout<<"[TRACE] WM_CHAR "<<wp<<" selection="<<a<<","<<b<<" length="<<GetWindowTextLengthW(hwnd)<<std::endl;
    }
    if(msg==WM_SETREDRAW && !wp && !(GetWindowLongPtrW(hwnd,GWL_STYLE)&WS_VISIBLE)) ++counts.hidden;
    return result;
}
}
int main(int argc, char** argv) {
    using namespace pulse;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--sharp") ui::typography::SetTextRenderMode(ui::typography::TextRenderMode::Sharp);
        else if (std::string(argv[i]) == "--smooth") ui::typography::SetTextRenderMode(ui::typography::TextRenderMode::Smooth);
    }
    OleInitialize(nullptr); l10n::Initialize(GetModuleHandleW(nullptr),L"zh-CN");
    auto owned=std::make_unique<AppState>(); auto& s=*owned;
    s.isolatedTest=true; s.appPrefs.persist=false; s.searchHistory.persist=false;
    s.hwnd=CreateWindowExW(0,L"STATIC",L"",WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN,-20000,-20000,1100,720,nullptr,nullptr,nullptr,nullptr);
    int failures=0;
    auto check=[&](bool ok,const char* label){std::cout<<(ok ? "[PASS] ":"[FAIL] ")<<label<<std::endl;if(!ok)++failures;};
    if(!s.hwnd || !s.compositor.Init(s.hwnd)) return 2;
    s.renderer.SetCompositor(&s.compositor); s.compositor.RecreateTextFormats(1);s.renderer.SetScale(1);
    check(s.compositor.CustomEditEnabled(),"fixture uses the production compositor with custom editing enabled");
    s.window_tabs.EnsureDefault();s.pane=s.window_tabs.Active()->FocusedPane();
    auto* tab=ActiveTab(s);tab->current_path=app::MakeSearchPath(L"content:3d3s");
    tab->pending_generation=990;tab->search_content_active=true;
    auto store=std::make_shared<index::ContentResultStore>(nullptr,0);
    index::ContentHit hit{L"C:\\fixture\\3d3s.txt",L"3d3s.txt",L"3d3s indexed result",5,0,1};hit.file_id=1;
    store->Append({hit});
    s.hwndAddressEdit=CreateHostedEdit(s,AddressEditProc);
    if(!s.hwndAddressEdit) return 2;
    s.addressEditing=true;s.addressSearching=true;s.addressSearchContent=true;s.addressSearchAnimation=1;
    SetWindowTextW(s.hwndAddressEdit,L"3d3s"); LayoutAddressEditor(s);
    ShowWindow(s.hwnd,SW_SHOWNOACTIVATE);ShowWindow(s.hwndAddressEdit,SW_SHOW);SetFocus(s.hwndAddressEdit);
    SendMessageW(s.hwndAddressEdit,EM_SETSEL,1,3);
    Render(s);UpdateWindow(s.hwndAddressEdit);
    Messages messages;
    SetWindowSubclass(s.hwndAddressEdit,Observe,98,reinterpret_cast<DWORD_PTR>(&messages));
    bool stable=true;
    for(int i=0;i<60;++i) {
        index::ContentSearchUpdate update;update.progress.generation=990;update.progress.scanned_files=i;
        update.progress.total_files=1000;update.results=store;ApplyContentSearchUpdate(s,std::move(update));
        Render(s);UpdateWindow(s.hwndAddressEdit);
        DWORD first=0,last=0;SendMessageW(s.hwndAddressEdit,EM_GETSEL,reinterpret_cast<WPARAM>(&first),reinterpret_cast<LPARAM>(&last));
        wchar_t text[32]{};GetWindowTextW(s.hwndAddressEdit,text,32);
        stable &= first==1 && last==3 && wcscmp(text,L"3d3s")==0 && GetFocus()==s.hwndAddressEdit && IsWindowVisible(s.hwndAddressEdit);
    }
    std::cout<<"frames=60 margins="<<messages.margins<<" positions="<<messages.positions<<" size="<<messages.size
             <<" redraw="<<messages.redraw<<" hidden="<<messages.hidden<<" text="<<messages.text<<std::endl;
    check(stable,"continuous search notifications preserve query text selection and focus");
    check(messages.margins==0 && messages.positions==0 && messages.size==0,"unchanged search editor geometry does not reapply native layout");
    check(messages.redraw==0 && messages.hidden==0 && messages.text==0,"search frames never hide or reinitialize the hosted edit surface");
    messages={};
    SetWindowPos(s.hwnd,nullptr,-20000,-20000,720,720,SWP_NOACTIVATE|SWP_NOZORDER);Render(s);
    check(messages.positions==1,"actual window resize updates hosted editor once");
    messages={};Render(s);
    check(messages.positions==0 && messages.redraw==0,"resized layout becomes stable on next frame");
    SendMessageW(s.hwndAddressEdit,EM_SETSEL,4,4);SendMessageW(s.hwndAddressEdit,WM_CHAR,L'x',0);
    wchar_t typed[32]{};GetWindowTextW(s.hwndAddressEdit,typed,32);
    check(wcscmp(typed,L"3d3sx")==0,"typing still updates the native edit after layout stabilizes");
    messages.trace=true;
    auto read_query=[&] {wchar_t value[256]{};GetWindowTextW(s.hwndAddressEdit,value,ARRAYSIZE(value));return std::wstring(value);};
    auto type=[&](const wchar_t* value,bool live) {
        for(const wchar_t* p=value;*p;++p) {
            SendMessageW(s.hwndAddressEdit,WM_CHAR,*p,0);
            if(live) {QueueAddressSearch(s);SubmitAddressSearch(s,true);}
            Render(s);UpdateWindow(s.hwndAddressEdit);
        }
    };
    SetWindowTextW(s.hwndAddressEdit,L"3d3s");SendMessageW(s.hwndAddressEdit,EM_SETSEL,4,4);
    type(L"abcd",false);
    check(read_query()==L"3d3sabcd","continuous typing appends characters in order across parent renders");
    SetWindowTextW(s.hwndAddressEdit,L"3d3s");SendMessageW(s.hwndAddressEdit,EM_SETSEL,4,4);
    type(L"abcd",true);
    check(read_query()==L"3d3sabcd","live query submission does not move subsequent typed characters to the front");
    SetWindowTextW(s.hwndAddressEdit,L"1234");SendMessageW(s.hwndAddressEdit,EM_SETSEL,2,2);
    type(L"abc",true);
    check(read_query()==L"12abc34","middle insertion preserves cursor across live query submission");
    SetWindowTextW(s.hwndAddressEdit,L"3d3s");SendMessageW(s.hwndAddressEdit,EM_SETSEL,0,0);
    RECT edit_rect{};GetClientRect(s.hwndAddressEdit,&edit_rect);
    const auto click=MAKELPARAM(edit_rect.right-4,edit_rect.bottom/2);
    SendMessageW(s.hwndAddressEdit,WM_LBUTTONDOWN,MK_LBUTTON,click);
    SendMessageW(s.hwndAddressEdit,WM_LBUTTONUP,0,click);
    type(L"abc",true);
    check(read_query()==L"3d3sabc","mouse placement at text end preserves following typing order");
    RemoveWindowSubclass(s.hwndAddressEdit,Observe,98);
    s.addressIgnoreKillFocus=true;DestroyWindow(s.hwndAddressEdit);s.hwndAddressEdit=nullptr;
    s.renderer.SetCompositor(nullptr);s.compositor.Shutdown();DestroyWindow(s.hwnd);s.hwnd=nullptr;
    std::cout<<"failures="<<failures<<std::endl;OleUninitialize();return failures ? 1:0;
}
