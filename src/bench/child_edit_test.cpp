#include "../ui/edit_host.h"
#include "../ui/ui_compositor.h"
#include <cstdio>
#include <string>
#include <commctrl.h>
#include <cstdint>
#include <utility>

namespace {
int failures = 0;
bool force_present_failure = false;
void Check(bool ok, const char* message) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", message);
    if (!ok) ++failures;
}
LRESULT CALLBACK EditProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR data) {
    auto& compositor = *reinterpret_cast<pulse::ui::Compositor*>(data);
    auto* format = force_present_failure ? nullptr : compositor.TextFormat();
    const auto foreground = D2D1::ColorF(1, 1, 1);
    DWORD flags = 0;
    const bool redirected = GetLayeredWindowAttributes(hwnd, nullptr, nullptr, &flags) && (flags & LWA_ALPHA);
    const auto background = D2D1::ColorF(0, redirected ? 1.0f : 0.0f);
    LRESULT result = 0;
    if (pulse::ui::HandleChildEditMessage(compositor, format, foreground,
        background, reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)), hwnd, msg, wp, lp, result)) return result;
    return pulse::ui::DefPresentedChildEditProc(compositor, format, foreground,
        background, hwnd, msg, wp, lp);
}
bool HasRenderedText(pulse::ui::Compositor& compositor, HWND edit) {
    RECT rect{};
    GetClientRect(edit, &rect);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = rect.right;
    info.bmiHeader.biHeight = -rect.bottom;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* pixels = nullptr;
    HDC dc = CreateCompatibleDC(nullptr);
    HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    if (!dc || !bitmap) {
        if (bitmap) DeleteObject(bitmap);
        if (dc) DeleteDC(dc);
        return false;
    }
    const auto old = SelectObject(dc, bitmap);
    const bool painted = compositor.PaintLumaEdit(edit, dc, compositor.TextFormat(),
        D2D1::ColorF(1, 1, 1), D2D1::ColorF(0, 0, 0));
    GdiFlush();
    int ink = 0;
    const auto* data = static_cast<const std::uint32_t*>(pixels);
    if (painted) {
        for (int i = 0; i < rect.right * rect.bottom; ++i)
            if ((data[i] & 0xff) > 64) ++ink;
    }
    SelectObject(dc, old);
    DeleteObject(bitmap);
    DeleteDC(dc);
    return painted && ink > 20;
}
}
int wmain() {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const HWND foreground = GetForegroundWindow();
    HWND parent = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP | WS_EX_NOACTIVATE, L"STATIC", L"",
        WS_POPUP, -30000, -30000, 700, 300, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    {
        SetEnvironmentVariableW(L"PULSE_TEST_LUMATEXT_INIT_FAILURE", L"1");
        pulse::ui::Compositor compositor;
        Check(parent && compositor.Init(parent), "composition survives a late text engine initialization failure");
        Check(!compositor.LumaTextEnabled(), "partially initialized text engine does not suppress native editing");
        SetEnvironmentVariableW(L"PULSE_TEST_LUMATEXT_INIT_FAILURE", nullptr);
    }
    {
        pulse::ui::Compositor compositor;
        Check(parent && compositor.Init(parent), "create composition host for native child editors");
        Check(compositor.LumaTextEnabled(), "LumaText remains enabled");
        for (const auto [redirected, scale] : {std::pair{false, 1.0f}, std::pair{false, 1.5f},
                std::pair{true, 1.0f}, std::pair{true, 1.5f}}) {
            compositor.RecreateTextFormats(scale);
            HWND edit = pulse::ui::CreateChildEdit(parent, L"show 中文");
            Check(edit && IsChild(parent, edit) && GetAncestor(edit, GA_ROOT) == parent &&
                !(GetWindowLongPtrW(edit, GWL_STYLE) & WS_POPUP), "editor is a real child, not an owned top-level popup");
            if (!edit) continue;
            if (redirected)
                Check(SetLayeredWindowAttributes(edit, 0, 255, LWA_ALPHA) != FALSE,
                    "enable redirected surface used by global search");
            SetWindowSubclass(edit, EditProc, 1, reinterpret_cast<DWORD_PTR>(&compositor));
            SetWindowPos(edit, nullptr, 20, 30, static_cast<int>(320 * scale), static_cast<int>(30 * scale),
                SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
            ShowWindow(parent, SW_SHOWNOACTIVATE);
            SendMessageW(edit, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(L"show 中文"));
            Check(compositor.PresentLumaEdit(edit, compositor.TextFormat(), D2D1::ColorF(1, 1, 1),
                D2D1::ColorF(0, redirected ? 1.0f : 0.0f)), "editor presents after native redraw suppression");
            Check(HasRenderedText(compositor, edit), "rendered editor contains visible Unicode glyph pixels");
            RECT before{}, after{};
            GetWindowRect(edit, &before);
            Check(compositor.PresentLumaEdit(edit, compositor.TextFormat(), D2D1::ColorF(1, 1, 1),
                D2D1::ColorF(0.1f, 0.1f, 0.1f)), "LumaText presents a child bitmap at 100 and 150 percent scale");
            GetWindowRect(edit, &after);
            Check(EqualRect(&before, &after), "text repaint does not move the child into screen coordinates");
            SendMessageW(edit, EM_SETSEL, 0, 4);
            SendMessageW(edit, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(L"debug"));
            wchar_t text[64]{};
            GetWindowTextW(edit, text, ARRAYSIZE(text));
            Check(std::wstring(text) == L"debug 中文", "native Unicode editing and selection remain functional");
            SendMessageW(edit, WM_UNDO, 0, 0);
            GetWindowTextW(edit, text, ARRAYSIZE(text));
            Check(std::wstring(text) == L"show 中文", "native undo remains functional");
            RECT owner{};
            GetWindowRect(parent, &owner);
            SetWindowPos(parent, nullptr, owner.left + 70, owner.top + 40, 0, 0,
                SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            GetWindowRect(edit, &after);
            Check(after.left == before.left + 70 && after.top == before.top + 40,
                "moving parent moves child without manual repositioning");
            force_present_failure = true;
            SendMessageW(edit, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(L"fallback"));
            force_present_failure = false;
            LRESULT handled = 0;
            Check(!pulse::ui::HandleChildEditMessage(compositor, compositor.TextFormat(),
                D2D1::ColorF(1, 1, 1), D2D1::ColorF(0, 0, 0), nullptr,
                edit, WM_PAINT, 0, 0, handled), "presentation failure restores native paint handling");
            SendMessageW(edit, EM_SETSEL, 0, -1);
            SendMessageW(edit, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(L"native 中文"));
            GetWindowTextW(edit, text, ARRAYSIZE(text));
            Check(std::wstring(text) == L"native 中文", "editing remains functional after presentation failure");
            ShowWindow(parent, SW_HIDE);
            Check(!IsWindowVisible(edit), "parent hide automatically hides the editor");
            DestroyWindow(edit);
        }
        for (const auto scale : {1.0f, 1.25f, 1.5f}) {
            compositor.RecreateTextFormats(scale);
            for (const bool fail_initial : {false, true}) {
                HWND field = pulse::ui::CreateChildEdit(parent, L"initial 中文");
                Check(field != nullptr, "create fresh dialog field");
                if (!field) continue;
                SetWindowSubclass(field, EditProc, 1, reinterpret_cast<DWORD_PTR>(&compositor));
                SetWindowPos(field, nullptr, 20, 30, static_cast<int>(320 * scale),
                    static_cast<int>(30 * scale), SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
                ShowWindow(parent, SW_SHOWNOACTIVATE);
                const bool presented = pulse::ui::PresentChildEdit(compositor,
                    fail_initial ? nullptr : compositor.TextFormat(), D2D1::ColorF(1, 1, 1),
                    D2D1::ColorF(0, 0, 0), field);
                BYTE alpha = 0;
                DWORD flags = 0;
                const bool native_surface = GetLayeredWindowAttributes(field, nullptr, &alpha, &flags) &&
                    (flags & LWA_ALPHA) && alpha == 255;
                Check(presented == !fail_initial && native_surface == fail_initial,
                    "initial bitmap failure selects an opaque native surface");
                LRESULT result = 0;
                Check(pulse::ui::HandleChildEditMessage(compositor, compositor.TextFormat(),
                    D2D1::ColorF(1, 1, 1), D2D1::ColorF(0, 0, 0), nullptr, field,
                    WM_PAINT, 0, 0, result) == !fail_initial,
                    "initial failure releases native paint handling");
                SetFocus(field);
                SendMessageW(field, EM_SETSEL, 0, -1);
                SendMessageW(field, WM_CHAR, L'文', 0);
                SetFocus(parent);
                SendMessageW(field, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(8, 8));
                SendMessageW(field, WM_LBUTTONUP, 0, MAKELPARAM(8, 8));
                Check(GetFocus() == field, "clicking a previously blurred field restores focus");
                SendMessageW(field, EM_SETSEL, 1, 1);
                SendMessageW(field, WM_CHAR, L'x', 0);
                wchar_t typed[32]{};
                GetWindowTextW(field, typed, ARRAYSIZE(typed));
                Check(std::wstring(typed) == L"文x", "refocused field accepts Unicode and keyboard input");
                if (fail_initial)
                    Check(!pulse::ui::PresentChildEdit(compositor, compositor.TextFormat(),
                        D2D1::ColorF(1, 1, 1), D2D1::ColorF(0, 0, 0), field),
                        "later layout preserves the native fallback");
                SetFocus(nullptr);
                DestroyWindow(field);
                ShowWindow(parent, SW_HIDE);
            }
        }
        HWND edit = pulse::ui::CreateChildEdit(parent);
        DestroyWindow(parent);
        Check(!IsWindow(edit), "destroying parent automatically destroys its editor");
    }
    Check(GetForegroundWindow() == foreground, "tests preserve the user's foreground window");
    CoUninitialize();
    return failures ? 1 : 0;
}
