#include "dialog_lifecycle.h"
#include "shortcut_help.h"
#include "../common/windows_compat.h"
#include "../common/localization.h"
#include <windowsx.h>
#include <algorithm>
#include <array>

namespace pulse::ui {
ShortcutHelpLayout LayoutShortcutHelp(float width, float height, float scale) {
    ShortcutHelpLayout l;
    const float w = std::max(0.0f, std::min(750.0f * scale, width - 32 * scale));
    const float h = std::max(0.0f, std::min(860.0f * scale, height - 32 * scale));
    l.card = D2D1::RectF((width-w)/2, (height-h)/2, (width+w)/2, (height+h)/2);
    const float pad = (w < 480 * scale ? 24.0f : 42.0f) * scale;
    l.narrow = w < 480 * scale;
    l.close = D2D1::RectF(l.card.right-56*scale, l.card.top+24*scale,
                         l.card.right-20*scale, l.card.top+60*scale);
    l.body = D2D1::RectF(l.card.left+pad, l.card.top+(l.narrow?116:136)*scale,
                        l.card.right-pad, l.card.bottom-26*scale);
    l.content_height = (l.narrow ? 846.0f : 680.0f) * scale;
    l.max_scroll = std::max(0.0f, l.content_height-(l.body.bottom-l.body.top));
    return l;
}

void DrawShortcutHelp(Compositor& compositor, bool dark, D2D1_COLOR_F accent,
                      float scale, float scroll, bool close_hover) {
    auto* dc = compositor.Dc();
    if (!dc) return;
    const auto l = LayoutShortcutHelp(static_cast<float>(compositor.Width()),
                                      static_cast<float>(compositor.Height()), scale);
    const auto theme = MakeTheme(dark, accent);
    const auto surface = dark ? HexColor(0x20242E) : HexColor(0xFFFFFF);
    const auto ink = dark ? HexColor(0xE7EAF5) : HexColor(0x303C54);
    const auto muted = dark ? HexColor(0xA7B3CA) : HexColor(0x71819D);
    const auto line = dark ? HexColor(0x363E50) : HexColor(0xE3E8F3);
    const auto tint = dark ? HexColor(0x2B324B) : HexColor(0xEEF1FD);
    ComPtr<ID2D1SolidColorBrush> brush;
    dc->CreateSolidColorBrush(ink, &brush);
    if (!brush.get()) return;
    auto fill = [&](D2D1_RECT_F r, D2D1_COLOR_F color, float radius) {
        brush->SetColor(color);
        dc->FillRoundedRectangle(D2D1::RoundedRect(r, radius*scale, radius*scale), brush.get());
    };
    auto text = [&](std::wstring_view value, D2D1_RECT_F r, float size,
                    D2D1_COLOR_F color, bool bold = false) {
        ComPtr<IDWriteTextFormat> format;
        compositor.DwriteFactory()->CreateTextFormat(L"Microsoft YaHei", nullptr,
            bold ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size*scale, L"", &format);
        if (!format.get()) return;
        brush->SetColor(color);
        dc->DrawTextW(value.data(), static_cast<UINT32>(value.size()), format.get(), r,
                      brush.get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    };
    auto rect = [&](float x, float y, float w, float h) {
        return D2D1::RectF(x, y, x+w*scale, y+h*scale);
    };
    // Soft layered shadow keeps the card distinct from the dimmed owner.
    for (int i=12; i>0; --i) {
        auto r=l.card;
        r.left-=i*scale; r.right+=i*scale; r.top-=(i-5)*scale; r.bottom+=(i+5)*scale;
        fill(r, D2D1::ColorF(0,0,0,0.012f), 22);
    }
    fill(l.card, surface, 20);
    const bool zh = l10n::effective_language() != l10n::Language::EnUS;
    text(L"A LITTLE HELP", rect(l.body.left,l.card.top+30*scale,260,20),12,
         dark ? HexColor(0xA5AEFF) : HexColor(0x6269CD),true);
    text(l10n::Get(l10n::StringId::HelpTitle),
         D2D1::RectF(l.body.left,l.card.top+62*scale,l.close.left,l.card.top+104*scale),
         l.narrow ? 24.0f : 30.0f,ink,true);
    if (close_hover) fill(l.close,tint,8);
    brush->SetColor(muted);
    const float cx=(l.close.left+l.close.right)/2, cy=(l.close.top+l.close.bottom)/2;
    dc->DrawLine({cx-6*scale,cy-6*scale},{cx+6*scale,cy+6*scale},brush.get(),1.6f*scale);
    dc->DrawLine({cx+6*scale,cy-6*scale},{cx-6*scale,cy+6*scale},brush.get(),1.6f*scale);
    dc->PushAxisAlignedClip(l.body,D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    float y=l.body.top-std::clamp(scroll,0.0f,l.max_scroll);
    text(l10n::Get(l10n::StringId::HelpDescription),
         D2D1::RectF(l.body.left,y,l.body.right,y+(l.narrow?112:76)*scale),l.narrow?15.0f:17.0f,muted);
    y+=(l.narrow?126:96)*scale;
    using Id=l10n::StringId;
    const std::array<std::pair<Id,std::wstring>,8> rows{{
        {Id::Search,L"Ctrl K"}, {Id::HelpOpen,zh?L"Enter / 双击":L"Enter / Double-click"},
        {Id::QuickPreview,L"Space"}, {Id::HelpClipboard,L"Ctrl C / X / V"},
        {Id::Rename,L"F2"}, {Id::Delete,L"Delete"}, {Id::SelectAll,L"Ctrl A"},
        {Id::SelectionHint,l10n::Get(Id::SelectionKeys)}
    }};
    for (const auto& row:rows) {
        const float row_h=(l.narrow?76.0f:58.0f)*scale;
        const float key_w=std::min(l.body.right-l.body.left,
            (static_cast<float>(row.second.size())*7.2f+18)*scale);
        const float key_y=y+(l.narrow?36:12)*scale;
        const float key_x=l.narrow?l.body.left:l.body.right-key_w;
        const auto key=D2D1::RectF(key_x,key_y,key_x+key_w,key_y+26*scale);
        text(l10n::Get(row.first),D2D1::RectF(l.body.left,y+12*scale,
             l.narrow?l.body.right:key.left-12*scale,y+38*scale),l.narrow?14.0f:16.0f,muted);
        fill(key,dark?HexColor(0x282E3B):HexColor(0xF7F8FC),5);
        brush->SetColor(line);
        dc->DrawRoundedRectangle(D2D1::RoundedRect(key,5*scale,5*scale),brush.get(),scale);
        text(row.second,D2D1::RectF(key.left+8*scale,key.top+5*scale,key.right-4*scale,key.bottom),12,muted);
        brush->SetColor(line);
        dc->DrawLine({l.body.left,y+row_h},{l.body.right,y+row_h},brush.get(),scale);
        y+=row_h;
    }
    y+=28*scale;
    const auto tip=D2D1::RectF(l.body.left,y,l.body.right,y+(l.narrow?82:76)*scale);
    fill(tip,tint,10);
    text(L"i",rect(tip.left+16*scale,y+18*scale,18,28),20,theme.accent,true);
    text(l10n::Get(Id::HelpTip),D2D1::RectF(tip.left+44*scale,y+16*scale,
         tip.right-18*scale,tip.bottom-10*scale),14,muted);
    dc->PopAxisAlignedClip();
    if (l.max_scroll>0) {
        const float track=l.body.bottom-l.body.top;
        const float thumb=std::max(24*scale,track*track/l.content_height);
        const float top=l.body.top+(track-thumb)*std::clamp(scroll/l.max_scroll,0.0f,1.0f);
        fill(D2D1::RectF(l.card.right-11*scale,top,l.card.right-8*scale,top+thumb),line,2);
    }
}

namespace {
struct HelpWindow {
    HWND hwnd{};
    Compositor compositor;
    bool dark=false, done=false, hover=false;
    D2D1_COLOR_F accent{};
    float scale=1, scroll=0;
    void Render() {
        auto* dc=compositor.Dc();
        if (!dc) return;
        dc->BeginDraw();
        dc->Clear(D2D1::ColorF(0.12f,0.16f,0.23f,0.40f));
        DrawShortcutHelp(compositor,dark,accent,scale,scroll,hover);
        if (SUCCEEDED(dc->EndDraw())) compositor.Present();
    }
    static LRESULT CALLBACK Proc(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp) {
        auto* self=reinterpret_cast<HelpWindow*>(GetWindowLongPtrW(hwnd,GWLP_USERDATA));
        if (msg==WM_NCCREATE) {
            self=static_cast<HelpWindow*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            self->hwnd=hwnd;
            SetWindowLongPtrW(hwnd,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self));
        }
        if (!self) return DefWindowProcW(hwnd,msg,wp,lp);
        const auto l=LayoutShortcutHelp(static_cast<float>(self->compositor.Width()),
            static_cast<float>(self->compositor.Height()),self->scale);
        auto inside=[&](D2D1_RECT_F r) {
            return GET_X_LPARAM(lp)>=r.left && GET_X_LPARAM(lp)<=r.right &&
                   GET_Y_LPARAM(lp)>=r.top && GET_Y_LPARAM(lp)<=r.bottom;
        };
        switch(msg) {
        case WM_CREATE: return self->compositor.Init(hwnd)?0:-1;
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: { PAINTSTRUCT ps{}; BeginPaint(hwnd,&ps); self->Render(); EndPaint(hwnd,&ps); return 0; }
        case WM_MOUSEMOVE: self->hover=inside(l.close); InvalidateRect(hwnd,nullptr,FALSE); return 0;
        case WM_LBUTTONUP: if (!inside(l.card)||inside(l.close)) self->done=true; return 0;
        case WM_MOUSEWHEEL:
            self->scroll=std::clamp(self->scroll-GET_WHEEL_DELTA_WPARAM(wp)*self->scale*0.5f,0.0f,l.max_scroll);
            InvalidateRect(hwnd,nullptr,FALSE); return 0;
        case WM_KEYDOWN:
            if (wp==VK_ESCAPE || wp==VK_RETURN) self->done=true;
            if (wp==VK_DOWN || wp==VK_NEXT) self->scroll=std::min(l.max_scroll,self->scroll+80*self->scale);
            if (wp==VK_UP || wp==VK_PRIOR) self->scroll=std::max(0.0f,self->scroll-80*self->scale);
            if (wp==VK_HOME) self->scroll=0;
            if (wp==VK_END) self->scroll=l.max_scroll;
            InvalidateRect(hwnd,nullptr,FALSE); return 0;
        case WM_CLOSE: self->done=true; return 0;
        }
        return DefWindowProcW(hwnd,msg,wp,lp);
    }
};
}
void ShowShortcutHelp(HWND owner,bool dark,D2D1_COLOR_F accent) {
    HelpWindow window;
    window.dark=dark; window.accent=accent;
    window.scale=static_cast<float>(compat::WindowDpi(owner))/96.0f;
    WNDCLASSW wc{};
    wc.hInstance=GetModuleHandleW(nullptr); wc.lpfnWndProc=HelpWindow::Proc;
    wc.lpszClassName=L"PulseShortcutHelp"; wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);
    RegisterClassW(&wc);
    RECT r{}; GetClientRect(owner,&r);
    POINT p{}; ClientToScreen(owner,&p);
    HWND previous=GetFocus();
    HWND hwnd=CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP,wc.lpszClassName,
        l10n::Get(l10n::StringId::ShortcutHints).c_str(),WS_POPUP,
        p.x,p.y,r.right,r.bottom,owner,nullptr,wc.hInstance,&window);
    if (!hwnd) return;
    EnableWindow(owner,FALSE);
    ShowDialogWithFade(hwnd); SetFocus(hwnd);
    MSG msg{};
    int status=1;
    while (!window.done && (status=GetMessageW(&msg,nullptr,0,0))>0) {
        TranslateMessage(&msg); DispatchMessageW(&msg);
    }
    EnableWindow(owner,TRUE);
    HideComposedDialog(hwnd, owner);
    DestroyWindow(hwnd);
    SetForegroundWindow(owner);
    if (IsWindow(previous)) SetFocus(previous);
    if (status==0) PostQuitMessage(static_cast<int>(msg.wParam));
}
}

