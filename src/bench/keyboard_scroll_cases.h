// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstring>
#include <optional>
#include <wrl/client.h>

inline bool RunInitialSelectionChecks(pulse::AppState& s) {
    using namespace pulse;
    int failures = 0;
    const auto check = [&](bool passed, const char* label) {
        std::printf("[%s] %s\n", passed ? "PASS" : "FAIL", label);
        failures += !passed;
    };
    BYTE saved_keys[256]{}, keys[256]{};
    check(GetKeyboardState(saved_keys) != FALSE, "save fixture thread keyboard state");
    SetFocus(s.hwnd);
    check(GetFocus() == s.hwnd, "file pane has Win32 keyboard focus");
    const auto key = [&](WPARAM code) {
        SetKeyboardState(keys);
        HandleKeyDown(&s, s.hwnd, WM_KEYDOWN, code, 1);
    };
    auto& tab = *ActiveTab(s);
    auto entries = std::make_shared<std::vector<fs::DirEntry>>(120);
    for (auto& entry : *entries) entry.name = L"Background keep.txt";
    const int last = static_cast<int>(entries->size()) - 1;
    (*entries)[0].name = L"a-hidden keep.txt";
    (*entries)[0].attrs = FILE_ATTRIBUTE_HIDDEN;
    (*entries)[1].name = L"alpha keep.cfg";
    (*entries)[2].name = L"amber filtered.txt";
    entries->back().name = L"Atlas keep";
    entries->back().is_dir = true;
    entries->back().attrs = FILE_ATTRIBUTE_DIRECTORY;
    tab.SetSnapshot(entries);
    tab.view_mode = ui::ViewMode::Details;
    tab.group_mode = ui::GroupMode::None;
    tab.filter_text = L"keep";
    tab.SelectOnly(last);
    s.safeMode = true;
    key(L'A');
    check(tab.selected_index == 1 && tab.SelectedCount() == 1,
        "first letter selects the first match and skips hidden rows");
    key(L'A');
    check(tab.selected_index == last && tab.SelectedCount() == 1,
        "repeated letter selects the next match and skips filtered rows");
    if (s.renderer.MotionEnabled()) {
        check(s.keyboardScroll && s.scrollAnimating && tab.scroll_y == 0 && s.scrollTargetY > 0,
            "offscreen letter match starts keyboard reveal from the displayed position");
        s.scrollLastUpdateTime -= std::chrono::milliseconds(20);
        Render(s);
        const float intermediate = tab.scroll_y;
        check(intermediate > 0 && intermediate < s.scrollTargetY, "letter reveal advances through an intermediate position");
        key(L'A');
        check(tab.selected_index == 1 && s.keyboardScrollFrom.y == intermediate && s.scrollTargetY == 0,
            "wrapping retargets the existing scroll from its current position");
        s.scrollLastUpdateTime -= std::chrono::milliseconds(120);
        Render(s);
        check(tab.scroll_y == 0 && !s.scrollAnimating, "wrapped letter reveal settles and releases the frame chain");
    } else {
        check(tab.scroll_y > 0 && !s.scrollAnimating, "reduced motion reveals the selected match immediately");
        key(L'A');
        check(tab.selected_index == 1 && tab.scroll_y == 0, "repeated letter wraps to the first match");
    }
    key(L'B');
    check(tab.selected_index == 3, "a different letter restarts from the first match");
    key(L'B');
    key(L'Z');
    check(tab.selected_index == 4, "matching rows with duplicate names remain distinct and an unmatched letter preserves selection");
    keys[VK_CONTROL] = 0x80;
    key(L'A');
    check(tab.SelectedCount() > 1, "Ctrl+A keeps the command shortcut");
    keys[VK_CONTROL] = 0;
    key(L'A');
    key(VK_PROCESSKEY);
    keys[VK_SHIFT] = 0x80;
    key(L'A');
    keys[VK_SHIFT] = 0;
    check(tab.selected_index == 1 && tab.SelectedCount() == 1, "IME processing and shifted letters leave file selection alone");
    const HWND editor = CreateWindowExW(0, L"EDIT", nullptr, WS_CHILD, 0, 0, 100, 20,
        s.hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (editor) {
        SetFocus(editor);
        check(GetFocus() == editor, "native edit has Win32 keyboard focus");
        key(L'B');
        SendMessageW(editor, WM_CHAR, L'b', 1);
        wchar_t typed[2]{};
        GetWindowTextW(editor, typed, ARRAYSIZE(typed));
        check(tab.selected_index == 1 && typed[0] == L'b', "letter remains available to the focused native edit");
        SetFocus(s.hwnd);
        DestroyWindow(editor);
    } else check(false, "create native edit focus fixture");
    tab.group_mode = ui::GroupMode::Type;
    tab.group_direction = ui::SortDirection::Asc;
    key(L'A');
    check(tab.selected_index == last, "grouped view selects the first displayed match instead of the first source row");
    key(L'A');
    check(tab.selected_index == 1, "grouped view advances across groups in display order");
    CancelScrollAnimation(s);
    SetKeyboardState(saved_keys);
    return failures == 0;
}

inline bool RunKeyboardScrollChecks(pulse::AppState& s) {
    using namespace pulse;
    int failures = 0;
    const auto check = [&](bool passed, const char* label) {
        std::printf("[%s] %s\n", passed ? "PASS" : "FAIL", label);
        failures += !passed;
    };
    BYTE saved_keys[256]{}, keys[256]{};
    check(GetKeyboardState(saved_keys) != FALSE, "save fixture thread keyboard state");
    const auto key = [&](WPARAM code, bool shift = false) {
        keys[VK_SHIFT] = shift ? 0x80 : 0;
        SetKeyboardState(keys);
        HandleKeyDown(&s, s.hwnd, WM_KEYDOWN, code, 1);
    };
    const auto advance = [&](int milliseconds) {
        s.scrollLastUpdateTime -= std::chrono::milliseconds(milliseconds);
        Render(s);
    };
    const auto pixels = [&](D2D1_RECT_F bounds) -> std::optional<uint64_t> {
        Microsoft::WRL::ComPtr<ID2D1Image> image;
        Microsoft::WRL::ComPtr<ID2D1Bitmap1> target, copy;
        auto* dc = s.compositor.Dc();
        dc->GetTarget(&image);
        if (!image || FAILED(image.As(&target))) return {};
        const D2D1_RECT_U region{static_cast<UINT>(bounds.left), static_cast<UINT>(bounds.top),
            static_cast<UINT>(bounds.right), static_cast<UINT>(bounds.bottom)};
        const UINT width = region.right - region.left, height = region.bottom - region.top;
        const auto props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
            target->GetPixelFormat());
        if (!width || !height || FAILED(dc->CreateBitmap(D2D1::SizeU(width, height), nullptr, 0, props, &copy)) ||
            FAILED(copy->CopyFromBitmap(nullptr, target.Get(), &region))) return {};
        D2D1_MAPPED_RECT mapped{};
        if (FAILED(copy->Map(D2D1_MAP_OPTIONS_READ, &mapped))) return {};
        uint64_t hash = 14695981039346656037ull;
        for (UINT y = 0; y < height; y += 2) for (UINT x = 0; x < width * 4; x += 4) {
            hash ^= mapped.bits[static_cast<size_t>(y) * mapped.pitch + x];
            hash *= 1099511628211ull;
        }
        copy->Unmap();
        return hash;
    };
    auto& tab = *ActiveTab(s);
    auto entries = std::make_shared<std::vector<fs::DirEntry>>(240);
    for (size_t i = 0; i < entries->size(); ++i) {
        auto& entry = (*entries)[i];
        entry.name = (i % 2 ? L"Odd " : L"Even ") + std::to_wstring(i) + L".txt";
        entry.full_path = tab.current_path + L"\\" + entry.name;
        entry.attrs = FILE_ATTRIBUTE_NORMAL;
    }
    tab.snapshot = entries; tab.group_mode = ui::GroupMode::None; tab.column_layout = false;
    s.safeMode = true; s.showDetailsPanel = false;
    s.renderer.SetDetailsPanelVisible(false);
    for (const auto mode : {ui::ViewMode::Details, ui::ViewMode::Content, ui::ViewMode::List}) {
        for (float scale : {1.0f, 1.5f, 2.0f}) for (bool dark : {false, true}) {
            std::printf("[KEYBOARD] mode=%d dpi=%.0f dark=%d\n", static_cast<int>(mode), scale * 100, dark);
            s.scale = scale; s.darkMode = dark; s.shot_high_contrast = false;
            s.renderer.SetScale(scale); s.compositor.RecreateTextFormats(scale);
            SetWindowPos(s.hwnd, nullptr, 0, 0, static_cast<int>(760 * scale), static_cast<int>(540 * scale),
                SWP_NOACTIVATE | SWP_NOZORDER);
            s.compositor.Resize(static_cast<UINT>(760 * scale), static_cast<UINT>(540 * scale));
            tab.view_mode = mode; tab.scroll_x = tab.scroll_y = 0;
            CancelScrollAnimation(s); tab.SelectOnly(0); Render(s);
            const auto pane = BuildVm(s, false).pane;
            const auto viewport = s.renderer.PaneListRect(pane, FocusedPaneRect(s));
            int boundary = -1;
            for (int view = 0; view < static_cast<int>(pane.EntryCount()); ++view) {
                const auto rect = s.renderer.ItemRectInPane(pane, FocusedPaneRect(s), view);
                if (rect.top >= viewport.top && rect.bottom <= viewport.bottom &&
                    rect.left >= viewport.left && rect.right <= viewport.right) boundary = view;
                else break;
            }
            check(boundary > 0 && boundary + 1 < static_cast<int>(pane.EntryCount()), "fixture exposes the keyboard viewport boundary");
            if (boundary < 1) continue;
            const auto offset = [&] { return mode == ui::ViewMode::List ? tab.scroll_x : tab.scroll_y; };
            const auto target = [&] { return mode == ui::ViewMode::List ? s.keyboardScrollTargetX : s.scrollTargetY; };
            const auto reset = [&] {
                CancelScrollAnimation(s); s.horizontalScrollMotion.Reset();
                tab.scroll_x = tab.scroll_y = 0; tab.SelectOnly(boundary);
            };
            tab.SelectOnly(0); key(VK_DOWN);
            check(tab.selected_index == 1 && !s.scrollAnimating && offset() == 0,
                "moving focus inside the viewport does not scroll");
            reset(); key(VK_DOWN);
            check(tab.selected_index == boundary + 1 && s.scrollAnimating && s.keyboardScroll && offset() == 0 && target() > 0,
                "Down changes selection immediately and starts a scroll without jumping");
            const float endpoint = target();
            const auto initial = pixels(viewport);
            advance(20);
            const auto intermediate = pixels(viewport);
            check(offset() > 0 && offset() < endpoint && s.scrollAnimating,
                "production renderer advances through an intermediate keyboard scroll position");
            advance(100);
            const auto settled = pixels(viewport);
            check(std::abs(offset() - endpoint) < 0.1f && !s.scrollAnimating,
                "keyboard scroll reaches its exact endpoint within 100 ms and stops scheduling");
            check(initial && intermediate && settled && *initial != *intermediate && *intermediate != *settled,
                "GPU readback confirms distinct intermediate and final file-list frames without exports");
            tab.SelectOnly(1); key(VK_UP);
            const float reverse_from = offset();
            check(tab.selected_index == 0 && s.scrollAnimating && target() < reverse_from,
                "Up starts a transition towards the first item from the displayed offset");
            advance(20);
            check(offset() < reverse_from && offset() > target(), "reverse keyboard scroll responds on the next frame");
            advance(100);
            check(offset() == 0 && !s.scrollAnimating, "reverse scroll settles at the first item");
            reset();
            for (int repeat = 0; repeat < 8; ++repeat) key(VK_DOWN);
            check(tab.selected_index == boundary + 8 && offset() == 0 && s.scrollVelocityY == 0,
                "rapid key repeat retargets focus without jumps or wheel momentum");
            advance(100);
            check(!s.scrollAnimating && std::abs(offset() - target()) < 0.1f, "rapid repeat settles at the latest focus target");
            reset(); key(VK_DOWN, true); key(VK_DOWN, true);
            check(tab.selected_index == boundary + 2 && tab.SelectedCount() == 3 && s.scrollAnimating,
                "Shift Down preserves the range anchor while scrolling");
            advance(100);
            reset(); key(VK_DOWN); advance(20);
            if (mode == ui::ViewMode::List) {
                POINT point{static_cast<LONG>(viewport.left + 10), static_cast<LONG>(viewport.top + 10)};
                ClientToScreen(s.hwnd, &point);
                HandleMouseWheel(&s, s.hwnd, WM_MOUSEWHEEL, MAKEWPARAM(0, static_cast<WORD>(-WHEEL_DELTA)),
                    MAKELPARAM(point.x, point.y));
                check(!s.keyboardScroll && !s.scrollAnimating && s.horizontalScrollMotion.Pending(),
                    "horizontal list wheel takes over from keyboard scrolling");
            } else {
                StartSmoothScroll(s, 60 * scale);
                check(!s.keyboardScroll && s.scrollAnimating && s.scrollVelocityY > 0,
                    "wheel input takes over the displayed offset with its original inertia");
            }
            reset(); key(VK_DOWN); s.shot_high_contrast = true; advance(0);
            check(!s.scrollAnimating && !s.keyboardScroll && std::abs(offset() - target()) < 0.1f,
                "high contrast settles an active keyboard scroll immediately");
            reset(); key(VK_DOWN);
            check(!s.scrollAnimating && offset() > 0, "high contrast reveals keyboard focus without a transition");
            s.shot_high_contrast = false;
            Render(s);
            reset();
            int final_index = static_cast<int>(entries->size()) - 1;
            if (mode == ui::ViewMode::List) {
                const auto final_pane = BuildVm(s, false).pane;
                const float last_column = s.renderer.ItemRectInPane(final_pane, FocusedPaneRect(s), final_index).left;
                while (final_index > 1 && s.renderer.ItemRectInPane(final_pane, FocusedPaneRect(s), final_index - 1).left == last_column)
                    --final_index;
                // End on the first row of a new column so the final Down must reveal it.
                tab.snapshot = std::make_shared<std::vector<fs::DirEntry>>(entries->begin(), entries->begin() + final_index + 1);
            }
            tab.SelectOnly(final_index - 1);
            EnsureRowVisible(s, tab, tab.selected_index); key(VK_DOWN);
            check(s.scrollAnimating && s.keyboardScroll, "moving to the final item starts a bounded transition");
            advance(20);
            const float elapsed = s.keyboardScrollElapsedMs;
            key(VK_DOWN);
            check(tab.selected_index == final_index && s.keyboardScrollElapsedMs == elapsed,
                "holding Down at the final item does not restart the transition");
            advance(100);
            check(!s.scrollAnimating, "final-item repeat releases the animation frame chain");
            tab.snapshot = entries;
            if (mode != ui::ViewMode::List) {
                key(VK_HOME);
                check(tab.selected_index == 0 && tab.scroll_y == 0 && !s.scrollAnimating,
                    "Home retains immediate positioning");
            }
        }
    }
    CancelScrollAnimation(s); s.horizontalScrollMotion.Reset();
    SetKeyboardState(saved_keys);
    std::printf("[RESULT] keyboard scroll failures=%d\n", failures);
    return failures == 0;
}
