#pragma once
#include "animation_snapshot.h"
#include "../ui/address_search_layout.h"

inline bool RunAddressPathAnimationChecks(pulse::AppState& s, const std::filesystem::path& root) {
    using namespace pulse;
    int failures = 0;
    const auto check = [&](bool passed, const char* label) {
        std::printf("[%s] %s\n", passed ? "PASS" : "FAIL", label);
        if (!passed) ++failures;
    };
    ActiveTab(s)->current_path = L"pulse:home";
    ShowAddressEditor(s, false);
    BYTE alpha = 255;
    check(s.addressEditing && !s.addressSearching && s.hwndAddressEdit,
        "path editor opens immediately");
    if (!s.hwndAddressEdit) return false;
    check(GetLayeredWindowAttributes(s.hwndAddressEdit, nullptr, &alpha, nullptr) && alpha == 0,
        "path editor starts transparent");
    SendMessageW(s.hwndAddressEdit, WM_CHAR, L'z', 1);
    wchar_t typed[64]{};
    GetWindowTextW(s.hwndAddressEdit, typed, ARRAYSIZE(typed));
    check(std::wstring_view(typed) == L"z", "path editor accepts typing during entrance");
    const auto now = app::PaneAnimationTimeMs();
    s.addressAnimationTick = now;
    TickAddressSearchAnimation(s, now + 60);
    TickAddressSearchAnimation(s, now + 120);
    const float opening = s.addressPathAnimation;
    check(opening > 0 && opening < 1 && GetLayeredWindowAttributes(s.hwndAddressEdit, nullptr, &alpha, nullptr) &&
        alpha > 0 && alpha < 255, "path editor has intermediate progress and opacity");
    HideAddressEditor(s, false);
    check(s.addressPathClosingText == L"z", "closing path retains the current typed text");
    s.addressAnimationTick = now + 120;
    TickAddressSearchAnimation(s, now + 140);
    const float closing = s.addressPathAnimation;
    check(closing > 0 && closing < opening, "path exit reverses from the current pose");
    ShowAddressEditor(s, false);
    s.addressAnimationTick = now + 140;
    TickAddressSearchAnimation(s, now + 160);
    check(s.addressPathAnimation > closing && s.addressPathAnimation < 1,
        "rapid path reopening continues without snapping");
    s.shot_high_contrast = true;
    check(!TickAddressSearchAnimation(s, now + 180) && s.addressPathAnimation == 1,
        "high contrast opens path immediately");
    ShowAddressSearch(s);
    check(s.addressPathClosingText == L"pulse:home", "switch to search retains outgoing path text");
    check(!TickAddressSearchAnimation(s, now + 200) && s.addressPathAnimation == 0 && s.addressSearchAnimation == 1,
        "path to search switch settles both channels");
    SetWindowTextW(s.hwndAddressEdit, L"query");
    ShowAddressEditor(s, false);
    check(s.addressSearchClosingText == L"query" && s.addressPathAnimation == 1,
        "switch back to path retains outgoing search text");
    HideAddressEditor(s, false);
    check(!TickAddressSearchAnimation(s, now + 220) && s.addressPathAnimation == 0,
        "high contrast exit leaves no pending path animation");
    s.shot_high_contrast = false;
    for (bool dark : {false, true}) for (float scale : {1.0f, 1.5f}) for (float width : {680.0f, 1200.0f}) {
        s.renderer.SetScale(scale);
        s.compositor.RecreateTextFormats(scale);
        s.compositor.Resize(static_cast<int>(width * scale), static_cast<int>(140 * scale));
        ui::WindowViewModel frame;
        frame.dark = dark;
        frame.pane.path = L"pulse:home";
        frame.address_path_closing_text = L"pulse:home";
        frame.window_effect = ui::WindowEffect::None;
        const auto theme = ui::MakeTheme(dark, ui::HexColor(0x0078D4));
        for (float progress : {0.0f, 0.2f, 0.45f, 0.8f, 1.0f}) {
            frame.address_path_animation = progress;
            auto* dc = s.compositor.Dc();
            dc->BeginDraw();
            dc->Clear(theme.bg);
            s.renderer.Render(frame, D2D1::RectF(0, 0, width * scale, 140 * scale), theme);
            check(SUCCEEDED(dc->EndDraw()), "path transition renders without D2D errors");
            const auto name = std::wstring(L"path-") + (dark ? L"dark-" : L"light-") +
                std::to_wstring(static_cast<int>(scale * 100)) + L"-" + std::to_wstring(static_cast<int>(width)) +
                L"-" + std::to_wstring(static_cast<int>(progress * 100)) + L".png";
            check(WriteAnimationSnapshot(s.compositor, root / name), "path transition snapshot captured");
        }
    }
    return failures == 0;
}
