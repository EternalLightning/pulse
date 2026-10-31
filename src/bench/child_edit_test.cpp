// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "../ui/edit_host.h"
#include "../ui/ui_compositor.h"
#include "../ui/typography.h"
#include "../ui/lumatext_renderer.h"
#include <cstdio>
#include <string>
#include <commctrl.h>
#include <cstdint>
#include <utility>
#include <cmath>

namespace pulse::ui { float EditScrollForTest(HWND hwnd); }

namespace {
int failures = 0;
bool force_present_failure = false;
struct EditMessages { int selections = 0, redraws = 0; };
LRESULT CALLBACK ObserveEdit(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR data) {
    auto& seen = *reinterpret_cast<EditMessages*>(data);
    if (msg == EM_SETSEL) ++seen.selections;
    if (msg == WM_SETREDRAW) ++seen.redraws;
    return DefSubclassProc(hwnd, msg, wp, lp);
}
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
LPARAM PointAt(pulse::ui::Compositor& compositor, HWND edit, std::wstring_view text, UINT32 index) {
    RECT field{}; GetClientRect(edit, &field);
    IDWriteTextLayout* layout = nullptr;
    if (FAILED(pulse::ui::typography::CreateTextLayout(compositor.DwriteFactory(), text.data(),
        static_cast<UINT32>(text.size()), compositor.TextFormat(), 1.0e6f,
        static_cast<float>(field.bottom), &layout)) || !layout) return 0;
    float x = 0, y = 0;
    DWRITE_HIT_TEST_METRICS hit{};
    layout->HitTestTextPosition(index, FALSE, &x, &y, &hit);
    layout->Release();
    return MAKELPARAM(static_cast<int>(std::lround(x + 2.0f)), field.bottom / 2);
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
#include "edit_motion_cases.h"

int wmain(int argc, wchar_t** argv) {
    for (int i = 1; i < argc; ++i) {
        if (std::wstring_view(argv[i]) == L"--sharp")
            pulse::ui::typography::SetTextRenderMode(pulse::ui::typography::TextRenderMode::Sharp);
        else if (std::wstring_view(argv[i]) == L"--smooth")
            pulse::ui::typography::SetTextRenderMode(pulse::ui::typography::TextRenderMode::Smooth);
    }
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const HWND foreground = GetForegroundWindow();
    HWND parent = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP | WS_EX_NOACTIVATE, L"STATIC", L"",
        WS_POPUP, -30000, -30000, 700, 300, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (argc == 2 && std::wstring_view(argv[1]) == L"--motion") {
        RunEditMotionChecks(parent);
        DestroyWindow(parent);
        Check(GetForegroundWindow() == foreground, "motion tests preserve the foreground window");
        CoUninitialize();
        return failures ? 1 : 0;
    }
    if (argc == 2 && std::wstring_view(argv[1]) == L"--opacity") {
        {
            pulse::ui::Compositor compositor;
            Check(parent && compositor.Init(parent), "create isolated search editor opacity host");
            compositor.RecreateTextFormats(1.0f);
            Check(compositor.CustomEditEnabled() && compositor.TextFormat(), "initialize the production text format for opacity checks");
            HWND edit = pulse::ui::CreateChildEdit(parent, L"search");
            Check(edit != nullptr, "create native editor for animated search handoff");
            if (edit) {
                SetWindowSubclass(edit, EditProc, 1, reinterpret_cast<DWORD_PTR>(&compositor));
                SetWindowPos(edit, nullptr, 20, 30, 320, 30, SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
                ShowWindow(parent, SW_SHOWNOACTIVATE);
                for (float opacity : {0.0f, 0.25f, 0.75f, 1.0f, 0.4f, 1.0f}) {
                    compositor.SetEditOpacity(edit, opacity);
                    Check(compositor.PresentLumaEdit(edit, compositor.TextFormat(), D2D1::ColorF(1, 1, 1), D2D1::ColorF(0, 1)),
                        "composition editor repaints throughout the chrome transition");
                    BYTE alpha = 0;
                    Check(GetLayeredWindowAttributes(edit, nullptr, &alpha, nullptr) && alpha == static_cast<BYTE>(std::lround(opacity * 255)),
                        "native child and composed bitmap retain the requested opacity after repaint");
                    Check(!compositor.UsesComposedEdits(edit) || GetPropW(edit, L"Pulse.EditComposition"),
                        "animated editor retains its composition surface");
                }
                Check(!GetPropW(edit, L"Pulse.EditOpacity"), "completed transition removes its temporary opacity state");
                compositor.SetEditOpacity(edit, 0.4f);
                force_present_failure = true;
                SendMessageW(edit, WM_CHAR, L'x', 1);
                BYTE alpha = 0;
                Check(GetLayeredWindowAttributes(edit, nullptr, &alpha, nullptr) && alpha == 102,
                    "native fallback preserves transitional opacity when bitmap presentation fails");
                force_present_failure = false;
                compositor.SetEditOpacity(edit, 1.0f);
                DestroyWindow(edit);
            }
        }
        DestroyWindow(parent);
        CoUninitialize();
        return failures ? 1 : 0;
    }
    {
        SetEnvironmentVariableW(L"PULSE_TEST_LUMATEXT_INIT_FAILURE", L"1");
        pulse::ui::Compositor compositor;
        Check(parent && compositor.Init(parent), "composition survives a late text engine initialization failure");
        Check(!compositor.CustomEditEnabled(), "partially initialized text engine does not suppress native editing");
        SetEnvironmentVariableW(L"PULSE_TEST_LUMATEXT_INIT_FAILURE", nullptr);
    }
    {
        pulse::ui::Compositor compositor;
        Check(parent && compositor.Init(parent), "create composition host for native child editors");
        Check(compositor.CustomEditEnabled(), "custom editing remains enabled in the selected text mode");
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
            EditMessages seen;
            SetWindowSubclass(edit, ObserveEdit, 2, reinterpret_cast<DWORD_PTR>(&seen));
            SetWindowPos(edit, nullptr, 20, 30, static_cast<int>(320 * scale), static_cast<int>(30 * scale),
                SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
            ShowWindow(parent, SW_SHOWNOACTIVATE);
            SendMessageW(edit, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(L"show 中文"));
            Check(compositor.PresentLumaEdit(edit, compositor.TextFormat(), D2D1::ColorF(1, 1, 1),
                D2D1::ColorF(0, redirected ? 1.0f : 0.0f)), "editor presents after native redraw suppression");
            if (compositor.UsesComposedEdits(edit)) {
                Check(GetPropW(edit, L"Pulse.EditComposition") != nullptr,
                    "editor publishes a persistent composition surface instead of native fallback");
                const int redraws = seen.redraws;
                SendMessageW(edit, EM_SETSEL, 0, 0);
                SendMessageW(edit, WM_KEYDOWN, VK_RIGHT, 0);
                Check(seen.redraws == redraws && IsWindowVisible(edit) &&
                    GetPropW(edit, L"Pulse.EditComposition") != nullptr,
                    "composed keyboard updates never toggle native redraw or hide the edit");
                ShowWindow(edit, SW_HIDE);
                Check(GetPropW(edit, L"Pulse.EditComposition") == nullptr,
                    "hiding an edit removes its persistent visual");
                ShowWindow(edit, SW_SHOWNOACTIVATE);
                Check(pulse::ui::PresentChildEdit(compositor, compositor.TextFormat(),
                    D2D1::ColorF(1, 1, 1), D2D1::ColorF(0, 1.0f), edit) &&
                    GetPropW(edit, L"Pulse.EditComposition") != nullptr,
                    "showing the edit recreates its persistent visual");
            }
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
            const std::wstring long_name = L"C:\\快速拖选\\" + std::wstring(120, L'W') + L"\\filename.txt";
            SetWindowTextW(edit, long_name.c_str());
            SetFocus(edit);
            SendMessageW(edit, EM_SETSEL, 0, long_name.size() - 4);
            RECT field{}; GetClientRect(edit, &field);
            SendMessageW(edit, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(field.right / 2, field.bottom / 2));
            const float initial_scroll = pulse::ui::EditScrollForTest(edit);
            std::printf("[METRIC] drag initial_scroll=%.3f width=%ld scale=%.1f\n", initial_scroll, field.right, scale);
            bool stable = initial_scroll >= 0;
            DWORD pressed_begin = 0, pressed_end = 0;
            SendMessageW(edit, EM_GETSEL, reinterpret_cast<WPARAM>(&pressed_begin), reinterpret_cast<LPARAM>(&pressed_end));
            bool selection_deferred = true;
            seen = {};
            for (int step = 0; step < 40; ++step) {
                const int x = step % 2 ? field.right - 1 : 1;
                SendMessageW(edit, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(x, field.bottom / 2));
                pulse::ui::PresentChildEdit(compositor, compositor.TextFormat(), D2D1::ColorF(1, 1, 1),
                    D2D1::ColorF(0, 0, 0), edit);
                stable &= std::fabs(pulse::ui::EditScrollForTest(edit) - initial_scroll) < 0.01f;
                DWORD begin = 0, end = 0;
                SendMessageW(edit, EM_GETSEL, reinterpret_cast<WPARAM>(&begin), reinterpret_cast<LPARAM>(&end));
                selection_deferred &= begin == pressed_begin && end == pressed_end;
            }
            Check(stable, "fast in-bounds drag and repaint keep the horizontal text origin fixed");
            Check(selection_deferred && seen.selections == 0 && seen.redraws == 0 && GetCapture() == edit,
                "drag paints its selection without native selection writes or redraw guards");
            float previous_scroll = pulse::ui::EditScrollForTest(edit);
            bool rightward = true;
            for (int step = 0; step < 5; ++step) {
                SendMessageW(edit, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(field.right + 40, field.bottom / 2));
                const float current_scroll = pulse::ui::EditScrollForTest(edit);
                rightward &= current_scroll >= previous_scroll;
                previous_scroll = current_scroll;
            }
            Check(rightward && previous_scroll > initial_scroll, "drag beyond the right edge scrolls forward without reversing");
            SendMessageW(edit, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(-40, field.bottom / 2));
            const float left_scroll = pulse::ui::EditScrollForTest(edit);
            Check(left_scroll >= 0 && left_scroll < previous_scroll, "drag beyond the left edge scrolls backward with a nonnegative origin");
            for (int frame = 0; frame < 5; ++frame)
                pulse::ui::PresentChildEdit(compositor, compositor.TextFormat(), D2D1::ColorF(1, 1, 1), D2D1::ColorF(0, 0, 0), edit);
            Check(std::fabs(pulse::ui::EditScrollForTest(edit) - left_scroll) < 0.01f,
                "repainting an outside drag never advances scrolling without another input event");
            SendMessageW(edit, WM_LBUTTONUP, 0, MAKELPARAM(field.right - 1, field.bottom / 2));
            DWORD released_begin = 0, released_end = 0;
            SendMessageW(edit, EM_GETSEL, reinterpret_cast<WPARAM>(&released_begin), reinterpret_cast<LPARAM>(&released_end));
            Check(seen.selections == 1 && released_end > released_begin && GetCapture() != edit,
                "mouse release commits the visible selection to native EDIT exactly once");
            SendMessageW(edit, EM_SETSEL, long_name.size(), long_name.size());
            pulse::ui::PresentChildEdit(compositor, compositor.TextFormat(), D2D1::ColorF(1, 1, 1), D2D1::ColorF(0, 0, 0), edit);
            const float end_scroll = pulse::ui::EditScrollForTest(edit);
            SendMessageW(edit, EM_SETSEL, 0, 0);
            pulse::ui::PresentChildEdit(compositor, compositor.TextFormat(), D2D1::ColorF(1, 1, 1), D2D1::ColorF(0, 0, 0), edit);
            Check(GetCapture() != edit && end_scroll > 0 && pulse::ui::EditScrollForTest(edit) == 0,
                "after releasing drag the viewport follows programmatic caret movement again");
            SetWindowTextW(edit, L"short.txt");
            Check(pulse::ui::EditScrollForTest(edit) == 0, "short replacement text resets a previously scrolled viewport");
            const std::wstring mutation_text = L"abcdefghij.txt";
            SetWindowTextW(edit, mutation_text.c_str());
            const auto from = PointAt(compositor, edit, mutation_text, 2);
            const auto to = PointAt(compositor, edit, mutation_text, 8);
            SendMessageW(edit, WM_LBUTTONDOWN, MK_LBUTTON, from);
            SendMessageW(edit, WM_MOUSEMOVE, MK_LBUTTON, to);
            SendMessageW(edit, WM_CHAR, L'X', 0);
            GetWindowTextW(edit, text, ARRAYSIZE(text));
            Check(std::wstring(text) == L"abXij.txt",
                "typing during capture first commits and replaces the painted dragged selection");
            SendMessageW(edit, WM_LBUTTONUP, 0, to);
            SetWindowTextW(edit, mutation_text.c_str());
            SendMessageW(edit, WM_LBUTTONDOWN, MK_LBUTTON, from);
            SendMessageW(edit, WM_MOUSEMOVE, MK_LBUTTON, to);
            SetFocus(parent);
            SendMessageW(edit, EM_GETSEL, reinterpret_cast<WPARAM>(&released_begin), reinterpret_cast<LPARAM>(&released_end));
            Check(released_begin == 2 && released_end == 8,
                "focus loss commits the painted selection before the editor leaves focus");
            if (GetCapture() == edit) ReleaseCapture();
            SetFocus(edit);
            SendMessageW(edit, WM_LBUTTONDOWN, MK_LBUTTON, from);
            SendMessageW(edit, WM_MOUSEMOVE, MK_LBUTTON, to);
            SendMessageW(edit, EM_SETSEL, 1, 1);
            SendMessageW(edit, WM_CHAR, L'Y', 0);
            GetWindowTextW(edit, text, ARRAYSIZE(text));
            Check(std::wstring(text) == L"aYbcdefghij.txt",
                "an explicit selection replaces a deferred mouse range before later typing");
            SendMessageW(edit, WM_LBUTTONUP, 0, to);
            const auto previous_mode = pulse::ui::typography::CurrentTextRenderMode();
            SendMessageW(edit, EM_SETSEL, 1, 6);
            bool switched = true;
            for (auto mode : {pulse::ui::typography::TextRenderMode::Auto,
                              pulse::ui::typography::TextRenderMode::Sharp,
                              pulse::ui::typography::TextRenderMode::Smooth}) {
                pulse::ui::typography::SetTextRenderMode(mode);
                DWORD first = 0, last = 0;
                SendMessageW(edit, EM_GETSEL, reinterpret_cast<WPARAM>(&first), reinterpret_cast<LPARAM>(&last));
                GetWindowTextW(edit, text, ARRAYSIZE(text));
                switched &= first == 1 && last == 6 && std::wstring(text) == L"aYbcdefghij.txt" &&
                    IsWindowVisible(edit) && compositor.CustomEditEnabled() && HasRenderedText(compositor, edit);
            }
            pulse::ui::typography::SetTextRenderMode(previous_mode);
            Check(switched, "live backend changes preserve an existing editor's Unicode text, selection and rendered glyphs");
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
            if (compositor.UsesComposedEdits(edit))
                Check(GetPropW(edit, L"Pulse.EditComposition") == nullptr,
                    "failed composed presentation removes the visual before native fallback");
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
                Check(presented == !fail_initial && native_surface,
                    "initial presentation and fallback preserve an opaque redirected child surface");
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
