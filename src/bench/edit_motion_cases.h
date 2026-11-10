// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once
#include "../ui/edit_motion.h"
#include "../ui/animation_clock.h"
#include "../ui/edit_frame_scheduler.h"
#include "snapshot_output.h"
#include <vector>

inline void RunEditMotionChecks(HWND parent) {
    using namespace pulse::ui;
    EditCaretMotion caret;
    caret.Tick({0, 0, 20}, 1000, true);
    caret.Tick({100, 0, 20}, 2000, true);
    Check(caret.pending && caret.value[0] == 0, "caret schedules frames before its first visible change");
    caret.Tick({100, 0, 20}, 2040, true);
    const auto middle = caret.value;
    caret.Tick({0, 0, 20}, 2040, true);
    Check(middle[0] > 0 && middle[0] < 100 && caret.value == middle, "rapid caret reversal preserves sampled position");
    caret.Tick({0, 0, 20}, 2120, true);
    Check(!caret.pending && caret.value[0] == 0, "caret settles exactly and releases frames");
    caret.Tick({200, 0, 20}, 2200, false);
    Check(!caret.pending && caret.value[0] == 200, "disabled motion snaps caret immediately");
    EditCaretBlink blink;
    blink.Reset(1500);
    blink.Sample(1500, false);
    Check(blink.Visible(1500, 500) && blink.Visible(1999, 500) && !blink.Visible(2000, 500),
        "activity begins a complete visible period regardless of the global clock phase");
    Check(blink.NextDelay(1999, 500) == 1 && blink.NextDelay(2000, 500) == 500,
        "blink redraw follows the activity phase rather than the focus timer phase");
    blink.Sample(2490, true);
    blink.Sample(2530, true);
    Check(blink.Visible(2530, 500), "movement stays visible across an otherwise dark blink phase");
    blink.Sample(2570, false);
    Check(blink.Visible(3069, 500) && !blink.Visible(3070, 500) && blink.Visible(3570, 500),
        "settled motion preserves a full visible period and then resumes blinking");
    EditCaretBlink other_blink;
    other_blink.Reset(3070);
    Check(other_blink.Visible(3070, 500) && !blink.Visible(3070, 500),
        "text fields keep independent blink phases");
    Check(blink.Visible(10000, UINT32_MAX) && blink.NextDelay(10000, UINT32_MAX) == 0,
        "disabled system blinking remains visible without a periodic timer");
    blink.Reset(4000);
    Check(blink.Visible(4499, 0) && !blink.Visible(4500, 0), "failed blink-time queries use a safe interval");
    EditTextMotion change;
    change.Begin(L"filename.txt", L"file新name.txt", 1000);
    Check(change.prefix == 4 && change.old_end == 4 && change.new_end == 5 && change.pending,
        "insertion preserves unchanged prefix and suffix");
    change.Sample(1050);
    Check(change.progress > 0 && change.progress < 1, "new glyph opacity has an intermediate value");
    caret.Tick({0, 0, 20}, 1000, false);
    caret.Tick({100, 0, 20}, 1000, true);
    caret.Tick({100, 0, 20}, 1050, true);
    Check(std::abs(caret.value[0] / 100 - change.progress) < 0.001f,
        "text reveal and caret travel share the same progress");
    change.Sample(1100);
    Check(change.progress == 1 && !change.pending, "text animation terminates");
    change.Begin(L"A\xD83D\xDE00Z", L"A\xD83D\xDE01Z", 1200);
    Check(change.prefix == 1 && change.old_end == 3 && change.new_end == 3, "UTF-16 replacement never splits surrogate pairs");

    pulse::ui::Compositor compositor;
    Check(compositor.Init(parent), "initialize text-motion compositor");
    for (const float scale : {1.0f, 1.5f}) for (const bool dark : {false, true}) {
        compositor.RecreateTextFormats(scale);
        HWND edit = CreateChildEdit(parent, L"File name.txt");
        SetWindowSubclass(edit, EditProc, 1, reinterpret_cast<DWORD_PTR>(&compositor));
        SetPropW(edit, L"Pulse.Test.EditMotion", reinterpret_cast<HANDLE>(1));
        SetWindowPos(edit, nullptr, 20, 30, static_cast<int>(320 * scale), static_cast<int>(32 * scale),
            SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        ShowWindow(parent, SW_SHOWNOACTIVATE);
        SetFocus(edit);
        Check(GetFocus() == edit, "isolated text field owns thread focus");
        std::vector<BYTE> frame;
        const auto draw = [&](const wchar_t* name) {
            RECT rc{}; GetClientRect(edit, &rc);
            BITMAPINFO info{};
            info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            info.bmiHeader.biWidth = rc.right; info.bmiHeader.biHeight = -rc.bottom;
            info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32; info.bmiHeader.biCompression = BI_RGB;
            void* pixels = nullptr;
            HDC dc = CreateCompatibleDC(nullptr);
            HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
            const auto old = SelectObject(dc, bitmap);
            const auto foreground = dark ? D2D1::ColorF(1, 1, 1) : D2D1::ColorF(0, 0, 0);
            const auto background = dark ? D2D1::ColorF(0.12f, 0.12f, 0.12f, 1) : D2D1::ColorF(1, 1, 1);
            const bool ok = compositor.PaintLumaEdit(edit, dc, compositor.TextFormat(), foreground, background);
            GdiFlush();
            Check(ok, "production input renderer draws motion frame");
            size_t ink = 0;
            if (ok && pixels) {
                const auto* bytes = static_cast<const BYTE*>(pixels);
                frame.assign(bytes, bytes + rc.right * rc.bottom * 4);
                // A moving one-pixel caret shares coverage across adjacent columns.
                for (int i = 0; i < rc.right * rc.bottom * 4; i += 4)
                    ink += static_cast<size_t>(dark ? std::max(0, static_cast<int>(bytes[i]) - 31) : 255 - bytes[i]);
            }
            if (name && ok) {
                auto* bytes = static_cast<BYTE*>(pixels);
                for (int i = 3; i < rc.right * rc.bottom * 4; i += 4) bytes[i] = 255;
                const auto filename = std::wstring(dark ? L"dark-" : L"light-") +
                    (scale == 1 ? L"100-" : L"150-") + name + L".png";
                Check(pulse::bench::EmitSnapshot(filename, rc.right, rc.bottom, rc.right * 4, bytes), "capture input motion frame");
            }
            SelectObject(dc, old); DeleteObject(bitmap); DeleteDC(dc);
            return ink;
        };
        draw(nullptr);
        const auto draws_before_key = compositor.GetLumaTextStats()->edit_directwrite_draws;
        PostMessageW(edit, WM_CHAR, L'X', 0);
        SendMessageW(edit, WM_KEYDOWN, L'X', 0);
        Check(compositor.GetLumaTextStats()->edit_directwrite_draws == draws_before_key,
            "ordinary keydown leaves drawing to the queued character message");
        MSG character{};
        const bool queued = PeekMessageW(&character, edit, WM_CHAR, WM_CHAR, PM_REMOVE) != FALSE;
        if (queued) DispatchMessageW(&character);
        Check(queued && compositor.GetLumaTextStats()->edit_directwrite_draws == draws_before_key + 1,
            "a text key and its queued character need only one presentation");
        SetWindowTextW(edit, L"File name.txt");
        SendMessageW(edit, EM_SETSEL, 0, 0);
        const auto draws_before_arrow = compositor.GetLumaTextStats()->edit_directwrite_draws;
        SendMessageW(edit, WM_KEYDOWN, VK_RIGHT, 0);
        Check(compositor.GetLumaTextStats()->edit_directwrite_draws > draws_before_arrow &&
            GetPropW(edit, L"Pulse.EditMotionPending"), "arrow keys still start caret motion immediately");
        SendMessageW(edit, EM_SETSEL, 5, 5); Sleep(120); draw(L"initial");
        SendMessageW(edit, WM_CHAR, L'新', 0);
        Check(GetPropW(edit, L"Pulse.EditMotionPending") != nullptr, "typing starts text/caret motion");
        wchar_t text[256]{}; GetWindowTextW(edit, text, ARRAYSIZE(text));
        Check(std::wstring(text) == L"File 新name.txt", "typing commits Unicode immediately while animation runs");
        Sleep(30); draw(L"insert-middle");
        Sleep(120); draw(L"insert-end");
        Check(!GetPropW(edit, L"Pulse.EditMotionPending"), "idle text field stops requesting motion frames");
        SendMessageW(edit, WM_CHAR, VK_BACK, 0);
        Sleep(30); draw(L"delete-middle");
        GetWindowTextW(edit, text, ARRAYSIZE(text));
        Check(std::wstring(text) == L"File name.txt", "backspace closes the text gap without delaying native text update");
        SendMessageW(edit, EM_SETSEL, 0, 0); Sleep(30); draw(L"caret-middle");
        SetPropW(edit, L"Pulse.Test.EditMotion", reinterpret_cast<HANDLE>(2));
        draw(nullptr);
        Check(!GetPropW(edit, L"Pulse.EditMotionPending"), "reduced motion settles active rendering immediately");
        SetPropW(edit, L"Pulse.Test.EditMotion", reinterpret_cast<HANDLE>(1));
        SendMessageW(edit, WM_IME_STARTCOMPOSITION, 0, 0);
        SendMessageW(edit, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(L"中文"));
        draw(nullptr);
        Check(!GetPropW(edit, L"Pulse.EditMotionPending"), "IME composition uses immediate glyph and caret positions");
        SendMessageW(edit, WM_IME_ENDCOMPOSITION, 0, 0);
        SendMessageW(edit, EM_SETSEL, 0, 4); draw(nullptr);
        Check(!GetPropW(edit, L"Pulse.EditMotionPending"), "selection presentation stays immediate");
        SendMessageW(edit, EM_SETSEL, 4, 4);
        SetPropW(edit, L"Pulse.NativeEditPolicy", reinterpret_cast<HANDLE>(1));
        SendMessageW(edit, WM_TIMER, kEditMotionTimer, 0);
        Check(!GetPropW(edit, L"Pulse.EditMotionPending"), "native backend policy cancels the motion timer");
        RemovePropW(edit, L"Pulse.NativeEditPolicy");
        SetPropW(edit, L"Pulse.EditWrap", reinterpret_cast<HANDLE>(1));
        SetWindowPos(edit, nullptr, 0, 0, static_cast<int>(130 * scale), static_cast<int>(90 * scale), SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        SetWindowTextW(edit, L"A longer file name with several words.txt");
        SendMessageW(edit, EM_SETSEL, 3, 3); Sleep(120); draw(nullptr);
        SendMessageW(edit, WM_CHAR, L'新', 0); Sleep(30); draw(L"wrapped-middle");
        Sleep(120); draw(nullptr);
        Check(!GetPropW(edit, L"Pulse.EditMotionPending"), "wrapped rename text reflow settles");
        SendMessageW(edit, WM_CHAR, L'x', 0);
        SendMessageW(edit, WM_CHAR, L'y', 0);
        SendMessageW(edit, WM_CHAR, L'z', 0);
        const auto until = GetTickCount64() + 180;
        while (GetTickCount64() < until) {
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&message); DispatchMessageW(&message);
            }
            Sleep(1);
        }
        Check(!GetPropW(edit, L"Pulse.EditMotionPending"), "motion timer advances rapid typing and stops without manual paints");
        if (scale == 1.0f && !dark) {
            int motion_ticks = 0;
            const auto deadline = AnimationTimeMs() + 240;
            bool presented = true;
            while (AnimationTimeMs() < deadline) {
                MSG message{};
                while (PeekMessageW(&message, edit, 0, 0, PM_REMOVE)) {
                    if (message.message == EditMotionFrameMessage() ||
                        (message.message == WM_TIMER && message.wParam == kEditMotionTimer)) ++motion_ticks;
                    DispatchMessageW(&message);
                }
                SendMessageW(edit, WM_CHAR, L' ', 0);
                presented &= compositor.PresentLumaEdit(edit, compositor.TextFormat(),
                    D2D1::ColorF(1, 1, 1), D2D1::ColorF(0, 1.0f));
                Sleep(1);
            }
            std::printf("[METRIC] motion ticks during continuous repaint: %d\n", motion_ticks);
            Check(presented && motion_ticks > 0,
                "continuous input and repaint do not postpone the animation timer");
            ValidateRect(edit, nullptr);
            const auto draws = compositor.GetLumaTextStats()->edit_directwrite_draws;
            SendMessageW(edit, WM_TIMER, kEditMotionTimer, 0);
            Check(compositor.GetLumaTextStats()->edit_directwrite_draws > draws,
                "a motion tick presents the editor without waiting behind unrelated paints");
        }
        double edit_time = 1500;
        SetPropW(edit, L"Pulse.Test.EditTime", reinterpret_cast<HANDLE>(&edit_time));
        SetPropW(edit, L"Pulse.Test.EditBlinkInterval", reinterpret_cast<HANDLE>(500));
        SetWindowTextW(edit, L"   ");
        SendMessageW(edit, EM_SETSEL, 0, 0);
        Check(draw(L"blink-focus") > 0, "actual caret pixels are visible at an otherwise dark clock phase");
        edit_time = 1999;
        SendMessageW(edit, EM_SETSEL, 3, 3);
        edit_time = 2039;
        Check(draw(L"blink-moving") > 0, "moving caret remains visible in production rendering");
        edit_time = 2079;
        Check(draw(L"blink-settled") > 0 && !GetPropW(edit, L"Pulse.EditMotionPending"),
            "80 ms caret motion settles without an immediate disappearance");
        edit_time = 2578;
        Check(draw(nullptr) > 0, "settled caret keeps the complete visible blink interval");
        edit_time = 2579;
        Check(draw(L"blink-hidden") == 0, "idle production caret really enters its hidden period");
        edit_time = 3079;
        Check(draw(nullptr) > 0, "idle production caret becomes visible again");
        edit_time = 3499;
        SendMessageW(edit, WM_CHAR, L' ', 0);
        edit_time = 3599;
        Check(draw(nullptr) > 0 && !GetPropW(edit, L"Pulse.EditMotionPending"),
            "typing ends its text motion with a visible caret");
        edit_time = 4098;
        Check(draw(nullptr) > 0, "typing also receives a full visible interval after settling");
        edit_time = 4099;
        Check(draw(nullptr) == 0, "typing resumes normal idle blinking");
        SetCapture(edit);
        Check(draw(nullptr) == 0, "mouse capture hides the custom caret");
        ReleaseCapture();
        Check(draw(L"blink-release") > 0, "mouse release restarts a complete visible period");
        SetPropW(edit, L"Pulse.Test.EditMotion", reinterpret_cast<HANDLE>(2));
        edit_time = 4500;
        SendMessageW(edit, EM_SETSEL, 0, 0);
        Check(draw(nullptr) > 0 && !GetPropW(edit, L"Pulse.EditMotionPending"),
            "disabled motion still resets caret blinking on movement");
        SetFocus(parent);
        Check(!GetPropW(edit, L"Pulse.EditMotionPending"), "focus loss clears motion state");
        Check(draw(nullptr) == 0, "focus loss removes actual caret pixels");
        edit_time = 4999;
        SetFocus(edit);
        Check(draw(L"blink-refocus") > 0, "refocus restores the caret independently of its old phase");
        RemovePropW(edit, L"Pulse.Test.EditTime");
        RemovePropW(edit, L"Pulse.Test.EditBlinkInterval");
        // Hold presentation time still: a committed edit's first frame must
        // preserve the pixels that its moving caret is leaving behind.
        RemovePropW(edit, L"Pulse.EditWrap");
        double transition_time = 10000;
        SetPropW(edit, L"Pulse.Test.EditTime", reinterpret_cast<HANDLE>(&transition_time));
        SetPropW(edit, L"Pulse.Test.EditMotion", reinterpret_cast<HANDLE>(2));
        SetWindowTextW(edit, L"W");
        SendMessageW(edit, EM_SETSEL, 1, 1);
        draw(L"delete-last-before");
        const auto before_delete = frame;
        SetPropW(edit, L"Pulse.Test.EditMotion", reinterpret_cast<HANDLE>(1));
        SendMessageW(edit, WM_CHAR, VK_BACK, 0);
        draw(L"delete-last-start");
        const auto same_rgb = [](const auto& a, const auto& b) {
            if (a.size() != b.size()) return false;
            for (size_t i = 0; i < a.size(); ++i)
                if (i % 4 != 3 && std::abs(static_cast<int>(a[i]) - b[i]) > 1) return false;
            return true;
        };
        Check(same_rgb(before_delete, frame), "last glyph stays visible on the first backspace frame");
        transition_time += 24;
        draw(L"delete-last-middle");
        Check(!same_rgb(before_delete, frame), "deleted glyph contracts while the caret travels");
        transition_time += 56;
        draw(L"delete-last-end");
        Check(!GetPropW(edit, L"Pulse.EditMotionPending"), "deletion and caret settle together at 80 ms");
        const auto before_insert = frame;
        SendMessageW(edit, WM_CHAR, L'W', 0);
        draw(L"append-start");
        Check(same_rgb(before_insert, frame), "append reveals from the caret without a first-frame jump");
        transition_time += 24;
        draw(L"append-middle");
        transition_time += 56;
        draw(L"append-end");
        Check(!GetPropW(edit, L"Pulse.EditMotionPending"), "appended glyph and caret settle together at 80 ms");
        SetPropW(edit, L"Pulse.Test.EditMotion", reinterpret_cast<HANDLE>(2));
        SetWindowTextW(edit, L"WWW");
        SendMessageW(edit, EM_SETSEL, 2, 2);
        draw(nullptr);
        const auto before_middle_delete = frame;
        SetPropW(edit, L"Pulse.Test.EditMotion", reinterpret_cast<HANDLE>(1));
        SendMessageW(edit, WM_CHAR, VK_BACK, 0);
        draw(L"delete-middle-start");
        Check(same_rgb(before_middle_delete, frame), "middle backspace preserves the glyph and suffix on its first frame");
        transition_time += 24;
        draw(L"delete-middle-moving");
        transition_time += 56;
        draw(nullptr);
        const auto before_forward_delete = frame;
        SendMessageW(edit, WM_KEYDOWN, VK_DELETE, 0);
        draw(L"forward-delete-start");
        Check(same_rgb(before_forward_delete, frame), "forward delete preserves its outgoing glyph on the first frame");
        transition_time += 80;
        draw(nullptr);
        Check(!GetPropW(edit, L"Pulse.EditMotionPending"), "forward deletion settles even with a stationary caret");
        SendMessageW(edit, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"Search"));
        draw(nullptr);
        const auto before_cue_delete = frame;
        SendMessageW(edit, WM_CHAR, VK_BACK, 0);
        draw(nullptr);
        Check(same_rgb(before_cue_delete, frame), "placeholder waits for the last outgoing glyph instead of replacing it immediately");
        transition_time += 80;
        draw(L"delete-cue-end");
        Check(!same_rgb(before_cue_delete, frame) && !GetPropW(edit, L"Pulse.EditMotionPending"),
            "placeholder appears when the final deletion settles");
        RemovePropW(edit, L"Pulse.EditWrap");
        SetWindowPos(edit, nullptr, 0, 0, static_cast<int>(320 * scale), static_cast<int>(32 * scale),
            SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        for (const bool erase : {false, true}) {
            SetPropW(edit, L"Pulse.Test.EditMotion", reinterpret_cast<HANDLE>(2));
            SetWindowTextW(edit, L"AZ");
            SendMessageW(edit, EM_SETSEL, 1, 1);
            draw(nullptr);
            SetPropW(edit, L"Pulse.Test.EditMotion", reinterpret_cast<HANDLE>(1));
            transition_time += 1000;
            SendMessageW(edit, WM_CHAR, L'W', 0);
            transition_time += 24;
            draw(nullptr);
            const auto interrupted = frame;
            SendMessageW(edit, WM_CHAR, erase ? VK_BACK : L'W', 0);
            draw(erase ? L"rapid-delete-start" : L"rapid-insert-start");
            Check(same_rgb(interrupted, frame), erase
                ? "backspace during text reveal preserves the actually displayed first frame"
                : "typing during text reveal preserves the actually displayed first frame");
            transition_time += 16;
            draw(nullptr);
            const auto mixed = frame;
            SendMessageW(edit, WM_CHAR, L'X', 0);
            draw(nullptr);
            Check(same_rgb(mixed, frame), "a third input retargets from the mixed frame, not from an old target");
            const auto repeated = frame;
            draw(nullptr);
            Check(same_rgb(repeated, frame), "painting an interrupted transition twice at the same time is stable");
            transition_time += 80;
            draw(nullptr);
            const auto settled = frame;
            Check(!GetPropW(edit, L"Pulse.EditMotionPending"), "interrupted typing still settles at its final target");
            SetPropW(edit, L"Pulse.Test.EditMotion", reinterpret_cast<HANDLE>(2));
            draw(nullptr);
            Check(same_rgb(settled, frame), "interrupted motion ends at the exact unanimated pixels");
        }
        SetPropW(edit, L"Pulse.EditWrap", reinterpret_cast<HANDLE>(1));
        SetWindowPos(edit, nullptr, 0, 0, static_cast<int>(130 * scale), static_cast<int>(90 * scale),
            SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        SetPropW(edit, L"Pulse.Test.EditMotion", reinterpret_cast<HANDLE>(2));
        SetWindowTextW(edit, L"ABC");
        SendMessageW(edit, EM_SETSEL, 3, 3);
        draw(nullptr);
        const auto first_ink_column = [&]() -> LONG {
            RECT bounds{}; GetClientRect(edit, &bounds);
            const int background = dark ? 31 : 255;
            for (int x = 0; x < bounds.right; ++x) for (int y = 0; y < bounds.bottom; ++y)
                if (std::abs(static_cast<int>(frame[(y * bounds.right + x) * 4]) - background) > 48) return x;
            return bounds.right;
        };
        const int prefix_before = first_ink_column();
        SetPropW(edit, L"Pulse.Test.EditMotion", reinterpret_cast<HANDLE>(1));
        transition_time += 1000;
        SendMessageW(edit, WM_CHAR, L'W', 0);
        transition_time += 0.5;
        draw(L"centered-prefix-start");
        Check(std::abs(first_ink_column() - prefix_before) <= std::ceil(scale),
            "centered unchanged prefix does not jump to its target on the first motion frame");
        transition_time += 80;
        draw(nullptr);
        Check(std::abs(first_ink_column() - prefix_before) > 2 * scale,
            "centered prefix fixture exercises a visible final displacement");
        RemovePropW(edit, L"Pulse.EditWrap");
        SetWindowPos(edit, nullptr, 0, 0, static_cast<int>(320 * scale), static_cast<int>(32 * scale),
            SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        SetPropW(edit, L"Pulse.Test.EditMotion", reinterpret_cast<HANDLE>(2));
        SetWindowTextW(edit, L"        ");
        SendMessageW(edit, EM_SETSEL, 0, 0);
        draw(nullptr);
        SetPropW(edit, L"Pulse.Test.EditMotion", reinterpret_cast<HANDLE>(1));
        transition_time += 1000;
        SendMessageW(edit, EM_SETSEL, 1, 1);
        const auto caret_energy = draw(nullptr);
        const auto caret_start = frame;
        transition_time += 0.5;
        const auto moving_energy = draw(L"subpixel-caret");
        Check(!same_rgb(caret_start, frame), "moving caret preserves fractional time and subpixel coverage");
        Check(caret_energy > 0 && std::abs(static_cast<double>(moving_energy) - static_cast<double>(caret_energy))
            <= static_cast<double>(caret_energy) * 0.01 + 2,
            "subpixel movement preserves caret contrast instead of fading it away");
        transition_time += 80;
        draw(nullptr);
        const auto caret_settled = frame;
        SetPropW(edit, L"Pulse.Test.EditMotion", reinterpret_cast<HANDLE>(2));
        draw(nullptr);
        Check(same_rgb(caret_settled, frame), "resting caret keeps the exact sharp unanimated pixels");
        RemovePropW(edit, L"Pulse.Test.EditTime");
        DestroyWindow(edit);
    }
}
