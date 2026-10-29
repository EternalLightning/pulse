#pragma once
#include "../ui/address_search_layout.h"
#include <cmath>

inline bool RunSearchInteractionAnimationChecks(pulse::AppState& s, const std::filesystem::path& root) {
    using namespace pulse;
    int failures = 0;
    const auto check = [&](bool passed, const char* label) {
        std::printf("[%s] %s\n", passed ? "PASS" : "FAIL", label);
        if (!passed) ++failures;
    };
    auto& tab = *ActiveTab(s);
    tab.search_input_path = tab.current_path;
    tab.search_input_text = L"Search caret sample";
    ShowAddressSearch(s);
    check(s.addressSearching && IsWindow(s.hwndAddressEdit), "search editor opens for repeat-click regression");
    if (!s.hwndAddressEdit) return false;
    BYTE edit_alpha = 255;
    check(GetLayeredWindowAttributes(s.hwndAddressEdit, nullptr, &edit_alpha, nullptr) && edit_alpha == 0,
          "search editor starts transparent while retaining its native input control");
    SendMessageW(s.hwndAddressEdit, WM_CHAR, L'z', 1);
    wchar_t typed[64]{};
    GetWindowTextW(s.hwndAddressEdit, typed, ARRAYSIZE(typed));
    check(std::wstring_view(typed).find(L'z') != std::wstring_view::npos,
          "typing is accepted immediately before the search transition completes");
    s.addressLiveDue = s.addressHistoryDue = 0;
    MSG message{};
    while (PeekMessageW(&message, s.hwnd, WM_SEARCH_HISTORY, WM_SEARCH_HISTORY, PM_REMOVE)) {}
    SendMessageW(s.hwndAddressEdit, EM_SETSEL, 0, -1);
    s.searchHistoryOpen = true;
    SendMessageW(s.hwndAddressEdit, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(3, 5));
    check(s.searchHistoryPointerClosing, "press on borrowed editor starts history close");
    DWORD selection_start = 0, selection_end = 0;
    SendMessageW(s.hwndAddressEdit, EM_GETSEL, reinterpret_cast<WPARAM>(&selection_start),
                 reinterpret_cast<LPARAM>(&selection_end));
    check(selection_start == selection_end && selection_start < tab.search_input_text.size(),
          "history close preserves native caret placement");
    // Reproduce the race where the close transition completes before mouse-up.
    s.searchHistoryOpen = false;
    SendMessageW(s.hwndAddressEdit, WM_LBUTTONUP, 0, MAKELPARAM(3, 5));
    check(!s.searchHistoryPointerClosing &&
          !PeekMessageW(&message, s.hwnd, WM_SEARCH_HISTORY, WM_SEARCH_HISTORY, PM_REMOVE),
          "same click release cannot reopen history after closing");
    SendMessageW(s.hwndAddressEdit, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(3, 5));
    SendMessageW(s.hwndAddressEdit, WM_LBUTTONUP, 0, MAKELPARAM(3, 5));
    check(PeekMessageW(&message, s.hwnd, WM_SEARCH_HISTORY, WM_SEARCH_HISTORY, PM_REMOVE),
          "next distinct editor click can reopen history");

    const auto now = app::PaneAnimationTimeMs();
    s.addressSearchAnimation = 0.0f;
    s.addressScopeAnimation = 0.0f;
    s.addressAnimationTick = now;
    TickAddressSearchAnimation(s, now + 60);
    TickAddressSearchAnimation(s, now + 120);
    const float opening = s.addressSearchAnimation;
    check(opening > 0.0f && opening < 1.0f, "search focus expands through an intermediate frame");
    check(GetLayeredWindowAttributes(s.hwndAddressEdit, nullptr, &edit_alpha, nullptr) && edit_alpha > 0 && edit_alpha < 255,
          "the live editor follows the search contents through intermediate opacity");
    s.addressSearching = false;
    TickAddressSearchAnimation(s, now + 140);
    const float closing = s.addressSearchAnimation;
    check(closing > 0.0f && closing < opening, "search focus collapses from its current frame");
    s.addressSearching = true;
    TickAddressSearchAnimation(s, now + 160);
    check(s.addressSearchAnimation > closing && s.addressSearchAnimation < 1.0f,
          "rapid reversal resumes smoothly without snapping");
    s.searchScopeOpen = s.searchOptionsOpen = true;
    auto vm = BuildVm(s, false);
    check(vm.address_search_scope_open && vm.address_search_options_open,
          "popup origins expose pressed feedback in the view model");
    s.searchScopeOpen = s.searchOptionsOpen = false;
    s.shot_high_contrast = true;
    s.addressSearching = false;
    check(!TickAddressSearchAnimation(s, now + 180) && s.addressSearchAnimation == 0.0f,
          "high contrast snaps search focus directly to the closed endpoint");
    s.addressSearching = true;
    check(!TickAddressSearchAnimation(s, now + 200) && s.addressSearchAnimation == 1.0f,
          "high contrast snaps search focus directly to the open endpoint");
    s.shot_high_contrast = false;
    HideAddressEditor(s, false);
    wchar_t snapshots[4]{};
    const bool save_snapshots = GetEnvironmentVariableW(L"PULSE_TEST_SNAPSHOT_STDOUT", snapshots, ARRAYSIZE(snapshots)) == 1 && snapshots[0] == L'1';
    {
        for (bool dark : {false, true}) for (float scale : {1.0f, 1.5f}) for (float width : {680.0f, 1200.0f}) {
            s.scale = scale;
            s.renderer.SetScale(scale);
            s.compositor.RecreateTextFormats(scale);
            const float window_width = width * scale;
            const auto field = s.renderer.SearchBarRect(window_width);
            const float padding = 8 * scale;
            s.compositor.Resize(static_cast<int>(std::ceil(field.right - field.left + 2 * padding)),
                                static_cast<int>(std::ceil(field.bottom - field.top + 2 * padding)));
            ui::WindowViewModel frame;
            frame.dark = dark;
            frame.window_effect = ui::WindowEffect::None;
            frame.address_search_current = true;
            frame.address_search_placeholder = L"Search in Fixture folder";
            frame.address_search_text = L"Search caret sample";
            frame.address_search_has_text = true;
            const auto theme = ui::MakeTheme(dark, ui::HexColor(0x0078D4));
            auto* dc = s.compositor.Dc();
            // Initialize the renderer's painter and brushes without painting
            // unrelated chrome or advancing the application's animation clock.
            dc->BeginDraw();
            dc->PushAxisAlignedClip(D2D1::RectF(0, 0, 0, 0), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
            s.renderer.Render(frame, D2D1::RectF(0, 0, window_width, 540 * scale), theme);
            dc->PopAxisAlignedClip();
            check(SUCCEEDED(dc->EndDraw()), "search snapshot renderer initializes without D2D errors");
            const auto prefix = std::wstring(L"search-") + (dark ? L"dark-" : L"light-") +
                std::to_wstring(static_cast<int>(scale * 100)) + (width < 1000 ? L"-narrow-" : L"-normal-");
            const auto capture = [&](const wchar_t* name) {
                dc->BeginDraw();
                dc->Clear(theme.bg);
                dc->SetTransform(D2D1::Matrix3x2F::Translation(padding - field.left, padding - field.top));
                s.renderer.DrawAddressSearchChrome(frame, window_width, theme);
                dc->SetTransform(D2D1::Matrix3x2F::Identity());
                check(SUCCEEDED(dc->EndDraw()), "isolated search snapshot draws without D2D errors");
                if (save_snapshots) check(WriteAnimationSnapshot(s.compositor, root / (prefix + name + L".png")),
                    "isolated search visual evidence captured");
                ui::ComPtr<ID2D1Image> target;
                ui::ComPtr<ID2D1Bitmap1> bitmap, pixels;
                dc->GetTarget(&target);
                uint64_t hash = 0;
                if (target.get() && SUCCEEDED(target->QueryInterface(&bitmap))) {
                    const auto size = bitmap->GetPixelSize();
                    const auto properties = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
                        bitmap->GetPixelFormat(), 96, 96);
                    D2D1_MAPPED_RECT mapped{};
                    if (SUCCEEDED(dc->CreateBitmap(size, nullptr, 0, properties, &pixels)) &&
                        SUCCEEDED(pixels->CopyFromBitmap(nullptr, bitmap.get(), nullptr)) &&
                        SUCCEEDED(pixels->Map(D2D1_MAP_OPTIONS_READ, &mapped))) {
                        hash = 1469598103934665603ull;
                        const UINT bottom = static_cast<UINT>(std::max(0.0f, field.bottom - field.top + padding - 4 * scale));
                        for (UINT y = static_cast<UINT>(padding + 4 * scale); y < bottom; ++y)
                            for (UINT x = 0; x < size.width * 4; ++x) hash = (hash ^ mapped.bits[y * mapped.pitch + x]) * 1099511628211ull;
                        pixels->Unmap();
                    }
                }
                check(hash != 0, "sample the actual search contents above the focus underline");
                return hash;
            };
            frame.address_searching = false;
            frame.address_search_animation = 0.0f;
            const auto closed_pixels = capture(L"closed");
            frame.address_searching = true;
            frame.address_search_animation = 0.2f;
            const auto early_pixels = capture(L"opening-early");
            frame.address_search_animation = 0.45f;
            const auto middle_pixels = capture(L"opening-middle");
            frame.address_searching = false;
            check(capture(L"closing-middle") == middle_pixels, "opening and closing share the same continuous contents pose");
            frame.address_searching = true;
            frame.address_search_animation = 0.8f;
            const auto late_pixels = capture(L"opening-late");
            frame.address_search_animation = 1.0f;
            const auto open_pixels = capture(L"open");
            check(closed_pixels != early_pixels && early_pixels != middle_pixels && middle_pixels != late_pixels && late_pixels != open_pixels &&
                middle_pixels != closed_pixels && middle_pixels != open_pixels,
                "icons, hint text and buttons change through distinct frames independently of the blue underline");
            const auto layout = ui::LayoutAddressSearch(field, scale);
            if (layout.scope.right > layout.scope.left) {
                frame.address_search_scope_open = true;
                capture(L"scope-pressed");
                frame.address_search_scope_open = false;
            }
            frame.address_search_options_open = true;
            capture(L"options-pressed");
        }
    }
    return failures == 0;
}
