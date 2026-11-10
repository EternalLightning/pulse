// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.

#pragma once
#include "snapshot_output.h"
#include <algorithm>
#include <vector>

inline void RunEditSelectionMotionChecks(HWND parent) {
    using namespace pulse::ui;
    struct KeyboardState {
        BYTE saved[256]{};
        bool valid = GetKeyboardState(saved) != FALSE;
        bool Shift(bool down) const {
            BYTE keys[256]{};
            if (down) keys[VK_SHIFT] = 0x80;
            return valid && SetKeyboardState(keys);
        }
        ~KeyboardState() { if (valid) SetKeyboardState(saved); }
    } keys;
    Check(keys.valid, "capture thread keyboard state for selection checks");
    if (!keys.valid) return;
    Compositor compositor;
    const bool initialized = compositor.Init(parent);
    Check(initialized && compositor.CustomEditEnabled(), "initialize keyboard-selection compositor");
    if (!initialized || !compositor.CustomEditEnabled()) return;
    const std::wstring text = L"File 中文 name with several words.txt";
    for (const float scale : {1.0f, 1.5f}) for (const bool dark : {false, true}) for (const bool wraps : {false, true}) {
        compositor.RecreateTextFormats(scale);
        HWND edit = CreateChildEdit(parent, text.c_str());
        SetWindowSubclass(edit, EditProc, 1, reinterpret_cast<DWORD_PTR>(&compositor));
        if (wraps) SetPropW(edit, L"Pulse.EditWrap", reinterpret_cast<HANDLE>(1));
        const int width = static_cast<int>((wraps ? 130 : 420) * scale);
        const int height = static_cast<int>((wraps ? 110 : 32) * scale);
        SetWindowPos(edit, nullptr, 20, 30, width, height, SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        ShowWindow(parent, SW_SHOWNOACTIVATE);
        SetFocus(edit);
        double time = 10000;
        SetPropW(edit, L"Pulse.Test.EditTime", reinterpret_cast<HANDLE>(&time));
        SetPropW(edit, L"Pulse.Test.EditMotion", reinterpret_cast<HANDLE>(1));
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = width; info.bmiHeader.biHeight = -height;
        info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32; info.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        HDC dc = CreateCompatibleDC(nullptr);
        HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
        Check(dc && bitmap && bits, "allocate isolated selection pixel fixture");
        if (!dc || !bitmap || !bits) {
            if (bitmap) DeleteObject(bitmap);
            if (dc) DeleteDC(dc);
            DestroyWindow(edit);
            continue;
        }
        const auto old = SelectObject(dc, bitmap);
        const auto draw = [&](const wchar_t* shot = nullptr) {
            const auto foreground = dark ? D2D1::ColorF(1, 1, 1) : D2D1::ColorF(0, 0, 0);
            const auto background = dark ? D2D1::ColorF(0.12f, 0.12f, 0.12f, 1) : D2D1::ColorF(1, 1, 1);
            Check(compositor.PaintLumaEdit(edit, dc, compositor.TextFormat(), foreground, background),
                "render keyboard-selection frame");
            GdiFlush();
            const auto* pixels = static_cast<const uint32_t*>(bits);
            std::vector<uint32_t> result(pixels, pixels + width * height);
            if (shot) {
                const auto name = std::wstring(dark ? L"dark-" : L"light-") +
                    (scale == 1 ? L"100-" : L"150-") + (wraps ? L"wrap-" : L"line-") + shot + L".png";
                Check(pulse::bench::EmitSnapshot(name, width, height, width * 4, static_cast<BYTE*>(bits)),
                    "capture keyboard-selection frame");
            }
            return result;
        };
        const auto same_rgb = [](const auto& a, const auto& b) {
            return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(),
                [](uint32_t x, uint32_t y) { return (x & 0xffffff) == (y & 0xffffff); });
        };
        const auto blue = [](const auto& frame) {
            size_t value = 0;
            for (const auto pixel : frame) value += static_cast<size_t>(std::max(0,
                static_cast<int>(pixel & 255) - static_cast<int>((pixel >> 16) & 255)));
            return value;
        };
        const auto selection = [&] {
            DWORD lo = 0, hi = 0;
            SendMessageW(edit, EM_GETSEL, reinterpret_cast<WPARAM>(&lo), reinterpret_cast<LPARAM>(&hi));
            return std::pair{lo, hi};
        };
        Check(keys.Shift(false), "initialize isolated keyboard modifiers");
        SendMessageW(edit, EM_SETSEL, 1, 1);
        time += 80;
        const auto base = draw();
        Check(keys.Shift(true), "press Shift in the test thread");
        SendMessageW(edit, WM_KEYDOWN, VK_RIGHT, 0);
        Check(selection() == std::pair<DWORD, DWORD>{1, 2}, "Shift+Right changes the native range immediately");
        Check(GetPropW(edit, L"Pulse.EditMotionPending") != nullptr, "Shift+Right requests smooth selection frames");
        Check(same_rgb(base, draw(L"select-start")), "keyboard selection begins from the displayed frame");
        time += 24;
        const auto middle = draw(L"select-middle");
        time += 56;
        const auto end = draw(L"select-end");
        Check(blue(middle) > 0 && blue(middle) < blue(end), "selection highlight has a genuine intermediate frame");
        Check(!GetPropW(edit, L"Pulse.EditMotionPending"), "selection motion settles and stops requesting frames");
        SendMessageW(edit, WM_KEYDOWN, VK_RIGHT, 0);
        time += 24;
        const auto before_reverse = draw();
        SendMessageW(edit, WM_KEYDOWN, VK_LEFT, 0);
        Check(selection() == std::pair<DWORD, DWORD>{1, 2}, "Shift+Left shrinks the range without moving its anchor");
        Check(same_rgb(before_reverse, draw(L"reverse-start")), "selection reversal preserves the currently displayed frame");
        time += 80;
        const auto reversed = draw();
        SendMessageW(edit, EM_SETSEL, 1, 2);
        Check(same_rgb(reversed, draw()), "shrinking keeps the active caret on the correct endpoint");
        SendMessageW(edit, WM_KEYDOWN, VK_LEFT, 0);
        Check(selection() == std::pair<DWORD, DWORD>{1, 1} && same_rgb(reversed, draw()),
            "collapsing a keyboard selection changes its native range immediately without a visual jump");
        time += 24;
        const auto closing = draw();
        Check(blue(closing) > 0 && blue(closing) < blue(reversed), "selection highlight smoothly retracts to an empty range");
        time += 56;
        Check(blue(draw()) == 0 && !GetPropW(edit, L"Pulse.EditMotionPending"), "collapsed selection leaves no highlight or pending motion");
        if (wraps) {
            SendMessageW(edit, WM_KEYDOWN, VK_DOWN, 0);
            const auto across_lines = selection();
            Check(across_lines.first == 1 && across_lines.second > 2 && GetPropW(edit, L"Pulse.EditMotionPending"),
                "wrapped rename selection preserves its anchor and animates across lines");
            time += 80;
            draw(L"across-lines");
        }
        SendMessageW(edit, EM_SETSEL, 0, 0);
        time += 80;
        auto held_frame = draw();
        bool repeated_ranges = true, repeated_frames = true;
        for (DWORD count = 1; count <= 16; ++count) {
            SendMessageW(edit, WM_KEYDOWN, VK_RIGHT, 1);
            repeated_ranges &= selection() == std::pair<DWORD, DWORD>{0, count};
            repeated_frames &= same_rgb(held_frame, draw());
            time += 12;
            held_frame = draw();
        }
        Check(repeated_ranges && repeated_frames, "held Shift+Right keeps native selection current and every retarget frame continuous");
        time += 80;
        draw();
        Check(!GetPropW(edit, L"Pulse.EditMotionPending"), "held-key selection settles after the last repeat");
        SendMessageW(edit, EM_SETSEL, 1, 3);
        SendMessageW(edit, WM_KEYDOWN, VK_RIGHT, 0);
        Check(selection() == std::pair<DWORD, DWORD>{1, 4}, "replacement sees the final selection before animation settles");
        Check(keys.Shift(false), "release Shift before replacement");
        SendMessageW(edit, WM_CHAR, L'Q', 0);
        wchar_t value[128]{}; GetWindowTextW(edit, value, ARRAYSIZE(value));
        Check(value == text.substr(0, 1) + L"Q" + text.substr(4), "typing replaces the native selected text without waiting for animation");
        SendMessageW(edit, WM_UNDO, 0, 0);
        GetWindowTextW(edit, value, ARRAYSIZE(value));
        Check(value == text, "native undo remains intact after animated selection replacement");
        time += 100;
        SendMessageW(edit, EM_SETSEL, 1, 1);
        SetPropW(edit, L"Pulse.Test.EditMotion", reinterpret_cast<HANDLE>(2));
        draw();
        keys.Shift(true);
        SendMessageW(edit, WM_KEYDOWN, VK_RIGHT, 0);
        Check(!GetPropW(edit, L"Pulse.EditMotionPending") && blue(draw()) > 0,
            "reduced motion keeps keyboard selection immediate");
        keys.Shift(false);
        if (!wraps) {
            SetPropW(edit, L"Pulse.Test.EditMotion", reinterpret_cast<HANDLE>(1));
            SendMessageW(edit, WM_LBUTTONDOWN, MK_LBUTTON, PointAt(compositor, edit, text, 2));
            SendMessageW(edit, WM_MOUSEMOVE, MK_LBUTTON, PointAt(compositor, edit, text, 5));
            const auto drag = draw();
            time += 80;
            Check(blue(drag) > 0 && same_rgb(drag, draw()) && !GetPropW(edit, L"Pulse.EditMotionPending"),
                "mouse drag selection stays immediate rather than trailing the pointer");
            SendMessageW(edit, WM_LBUTTONUP, 0, PointAt(compositor, edit, text, 5));
        }
        SelectObject(dc, old); DeleteObject(bitmap); DeleteDC(dc);
        DestroyWindow(edit);
    }
}
