// SPDX-License-Identifier: Apache-2.0
#include "../app/app_internal.h"
#include "../app/app_terminal.h"
#include "../app/app_ui_schedule.h"
#include "../ui/terminal_surface.h"
#include "../ui/typography.h"
#include "terminal_menu_cases.h"
#include <windowsx.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>

namespace pulse::ui {

struct TerminalScrollbackTest {
    static bool LoadOutput(TerminalSurface& surface) {
        surface.columns_ = 30;
        surface.rows_ = 8;
        surface.terminal_ = vterm_new(surface.rows_, surface.columns_);
        if (!surface.terminal_) return false;
        surface.screen_ = vterm_obtain_screen(surface.terminal_);
        surface.status_ = ops::TerminalProcess::Status::Running;
        vterm_screen_reset(surface.screen_, 1);
        constexpr std::string_view output = "Selected text\r\n(https://sass-lang.com/d/color-functions).\r\n"
            "http://localhost:1234\r\nfile:///C:/test";
        vterm_input_write(surface.terminal_, output.data(), output.size());
        vterm_screen_flush_damage(surface.screen_);
        return true;
    }

    static POINT Point(const TerminalSurface& surface, const D2D1_RECT_F& bounds, int row, int col) {
        return {static_cast<LONG>(bounds.left + (col + 0.5f) * surface.cell_width_),
            static_cast<LONG>(bounds.top + (row + 0.5f) * surface.cell_height_)};
    }

    static bool RetainsViewportAtCapacity() {
        TerminalSurface surface;
        VTermScreenCell cell{};
        for (int i = 0; i < 1600; ++i) TerminalSurface::PushLine(1, &cell, &surface);
        const auto size = surface.history_.size();
        surface.offset_ = 2 * surface.cell_height_;
        TerminalSurface::PushLine(1, &cell, &surface);
        if (surface.history_.size() != size || surface.offset_ != 3 * surface.cell_height_) return false;
        surface.offset_ = surface.MaxScroll();
        TerminalSurface::PushLine(1, &cell, &surface);
        if (surface.offset_ != surface.MaxScroll()) return false;
        surface.offset_ = 0;
        TerminalSurface::PushLine(1, &cell, &surface);
        return surface.offset_ == 0;
    }
};

} // namespace pulse::ui

namespace {

using namespace pulse;

LRESULT CALLBACK Receive(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
    if (message == ops::kTerminalUpdated) {
        if (auto* state = GetAppState(hwnd)) {
            LRESULT result = 0;
            HandleEmbeddedTerminalMessage(*state, message, wp, lp, result);
        }
        return 0;
    }
    return DefWindowProcW(hwnd, message, wp, lp);
}

bool Check(bool passed, const char* name) {
    std::printf("[%s] %s\n", passed ? "PASS" : "FAIL", name);
    return passed;
}

bool SelectionContainsInk(ui::Compositor& compositor, POINT first, POINT last) {
    if (!compositor.CaptureFrame()) return false;
    auto* frame = compositor.FrameCopy();
    const auto properties = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        frame->GetPixelFormat());
    ui::ComPtr<ID2D1Bitmap1> pixels;
    if (FAILED(compositor.Dc()->CreateBitmap(frame->GetPixelSize(), nullptr, 0, properties, &pixels)) ||
        FAILED(pixels->CopyFromBitmap(nullptr, frame, nullptr))) return false;
    D2D1_MAPPED_RECT mapped{};
    if (FAILED(pixels->Map(D2D1_MAP_OPTIONS_READ, &mapped))) return false;
    // The selected blank cell after the text gives the actual composited fill.
    const auto* fill = mapped.bits + last.y * mapped.pitch + 4 * last.x;
    int differing = 0;
    for (int y = first.y - 5; y <= first.y + 5; ++y) {
        for (int x = first.x; x < last.x - 8; ++x) {
            const auto* pixel = mapped.bits + y * mapped.pitch + 4 * x;
            if (std::abs(pixel[0] - fill[0]) + std::abs(pixel[1] - fill[1]) + std::abs(pixel[2] - fill[2]) > 70)
                ++differing;
        }
    }
    pixels->Unmap();
    return differing > 20;
}

bool TerminalSelectionAndLinks(AppState& state, ui::TerminalSurface& surface, const D2D1_RECT_F& bounds,
                               const std::filesystem::path& output) {
    using Screen = ui::TerminalScrollbackTest;
    using Launch = ops::TerminalMenuTestProbe;
    if (!Check(Screen::LoadOutput(surface), "load isolated terminal link output")) return false;
    const auto paint = [&](const ui::Theme& theme) {
        auto* dc = state.compositor.Dc();
        dc->BeginDraw();
        dc->Clear(theme.bg);
        surface.Draw(state.compositor, theme, bounds, 1.0f, false, false, false);
        return SUCCEEDED(dc->EndDraw());
    };
    const auto dark = ui::IsHighContrast() ? ui::MakeHighContrastTheme() : ui::MakeTheme(true, ui::HexColor(0x76BAFF));
    if (!paint(dark)) return false;
    const auto point = [&](int row, int col) { return Screen::Point(surface, bounds, row, col); };
    const auto first = point(0, 0), last = point(0, 13);
    surface.Select(static_cast<float>(first.x), static_cast<float>(first.y), bounds, false);
    surface.Select(static_cast<float>(last.x), static_cast<float>(last.y), bounds, true);
    bool pass = Check(surface.SelectedText() == L"Selected text " && paint(dark) &&
        SelectionContainsInk(state.compositor, first, last) &&
        state.compositor.SaveSnapshot((output / L"selection-dark.png").c_str()), "selected glyphs remain visible on the dark fill");
    const auto light = ui::IsHighContrast() ? ui::MakeHighContrastTheme() : ui::MakeTheme(false, ui::HexColor(0x76BAFF));
    pass &= Check(paint(light) && SelectionContainsInk(state.compositor, first, last) &&
        state.compositor.SaveSnapshot((output / L"selection-light.png").c_str()), "selected glyphs remain visible on the light fill");

    const auto mouse = [&](UINT msg, WPARAM buttons, POINT at) {
        LRESULT result = 0;
        return HandleEmbeddedTerminalMessage(state, msg, buttons, MAKELPARAM(at.x, at.y), result);
    };
    const auto click = [&](POINT down, POINT up, WPARAM modifiers) {
        mouse(WM_LBUTTONDOWN, MK_LBUTTON | modifiers, down);
        mouse(WM_LBUTTONUP, modifiers, up);
    };
    Launch::Capture(state.ops);
    click(point(1, 6), point(2, 5), MK_CONTROL);
    pass &= Check(Launch::TakeLink(state.ops, L"https://sass-lang.com/d/color-functions") && surface.HasSelection(),
        "Ctrl-click queues the complete wrapped HTTPS link and keeps the selection");
    click(point(3, 16), point(3, 16), MK_CONTROL);
    pass &= Check(Launch::TakeLink(state.ops, L"http://localhost:1234"), "Ctrl-click queues localhost with its port");
    click(point(3, 4), point(4, 5), MK_CONTROL);
    click(point(4, 5), point(4, 5), MK_CONTROL);
    click(point(3, 6), point(3, 15), 0);
    pass &= Check(Launch::Empty(state.ops) && surface.HasSelection(),
        "different release target, file scheme and plain click leave link launching inactive");
    Launch::Reset(state.ops);
    return pass;
}

template<typename Predicate>
bool WaitFor(Predicate predicate, uint64_t timeout) {
    const uint64_t end = GetTickCount64() + timeout;
    do {
        if (predicate()) return true;
        MsgWaitForMultipleObjectsEx(0, nullptr, 15, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&message);
    } while (GetTickCount64() < end);
    return false;
}

} // namespace

int main() {
    using namespace pulse;
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const bool scrollback_pass = Check(ui::TerminalScrollbackTest::RetainsViewportAtCapacity(),
        "incoming output retains a scrolled viewport at capacity and follows the tail at offset zero");
    if (FAILED(OleInitialize(nullptr))) return 2;
    l10n::Initialize(GetModuleHandleW(nullptr), L"zh-CN");
    auto state = std::make_unique<AppState>();
    state->isolatedTest = true;
    state->appPrefs.persist = state->places.persist = state->ctxMenuPrefs.persist = false;
    state->appPrefs.auto_folder_size = false;
    state->window_tabs.EnsureDefault();
    auto& layout = *state->window_tabs.Active();
    state->pane = layout.FocusedPane();
    auto* tab = state->pane->ActiveTab();
    tab->current_path = L"C:\\";
    tab->loading = false;
    tab->SetSnapshot(std::make_shared<const std::vector<fs::DirEntry>>());
    WNDCLASSW wc{};
    wc.lpfnWndProc = Receive;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"PulseTerminalAppearanceFixture";
    RegisterClassW(&wc);
    state->hwnd = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_NOREDIRECTIONBITMAP,
        wc.lpszClassName, nullptr, WS_POPUP, -20000, -20000, 1200, 850, nullptr, nullptr, wc.hInstance, nullptr);
    if (!Check(state->hwnd && state->compositor.Init(state->hwnd), "initialize isolated terminal renderer")) return 2;
    ShowWindow(state->hwnd, SW_SHOWNOACTIVATE);
    SetWindowLongPtrW(state->hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state.get()));
    state->core.windows.Add(state->hwnd, state.get());
    state->renderer.SetCompositor(&state->compositor);
    state->renderer.SetScale(1.0f);
    state->compositor.RecreateTextFormats(1.0f);
    layout.terminal = std::make_shared<ui::TerminalSurface>();
    layout.terminal_open = true;
    SyncEmbeddedTerminal(*state);
    state->renderer.SnapTerminalPanel();
    state->terminalFocused = true;

    {
        auto other_window = std::make_unique<AppState>(state->core);
        const HWND other = CreateWindowExW(0, wc.lpszClassName, nullptr, WS_POPUP,
            0, 0, 1200, 850, nullptr, nullptr, wc.hInstance, nullptr);
        if (!Check(other != nullptr, "create second window for terminal splitter")) return 2;
        state->core.windows.Add(other, other_window.get());
        other_window->renderer.SetScale(1.0f);
        other_window->renderer.SetTerminalPanelVisible(true);
        other_window->renderer.SnapTerminalPanel();

        state->detailsPanelResizing = true;
        const auto x = static_cast<int>(1200.0f - state->renderer.Margin() - 630.0f);
        const LPARAM drag = MAKELPARAM(x, 240);
        HandleMouseMove(state.get(), state->hwnd, WM_MOUSEMOVE, MK_LBUTTON, drag);
        LRESULT input_result = 0;
        const bool terminal_handled_release = HandleEmbeddedTerminalMessage(*state, WM_LBUTTONUP, 0, drag, input_result);
        if (!terminal_handled_release) HandleLButtonUp(state.get(), state->hwnd, WM_LBUTTONUP, 0, drag);
        const auto resized = state->renderer.DetailsPanelRect(1200, 850);
        const auto mirrored = other_window->renderer.DetailsPanelRect(1200, 850);
        state->core.windows.Remove(other);
        DestroyWindow(other);
        if (!Check(!terminal_handled_release && !state->detailsPanelResizing && state->core.terminal_panel_width == 630.0f &&
                resized.left == mirrored.left && resized.right == mirrored.right,
                "terminal splitter releases and keeps its last width across windows")) return 1;
    }

    auto& surface = *layout.terminal;
    const auto rect = D2D1::RectF(0, 0, 1200, 850);
    const auto bounds = state->renderer.TerminalBodyRect(rect.right, rect.bottom);
    const auto panel = state->renderer.DetailsPanelRect(rect.right, rect.bottom);
    const auto theme = ui::MakeTheme(true, ui::HexColor(0x76BAFF));
    const auto output = std::filesystem::absolute(L".tmp/terminal-tab-lifecycle");
    std::filesystem::create_directories(output);
    const auto paint = [&](const ui::WindowViewModel& vm) {
        auto* dc = state->compositor.Dc();
        dc->BeginDraw();
        dc->Clear(theme.bg);
        state->renderer.Render(vm, rect, theme);
        return SUCCEEDED(dc->EndDraw());
    };
    auto vm = BuildVm(*state, false);
    vm.safe_mode = true;
    vm.dark = true;
    vm.terminal = &surface;
    vm.terminal_focused = true;
    const bool selection_links_pass = TerminalSelectionAndLinks(*state, surface, bounds, output);
    surface.Start(state->hwnd, L"C:\\", true, ops::TerminalShell::WindowsPowerShell, {});
    if (!Check(WaitFor([&] { return paint(vm) && static_cast<float>(surface.CursorPoint(bounds).x) > bounds.left + 10.0f; }, 12000),
        "PowerShell draws a prompt in the embedded terminal")) return 1;

    surface.ResetCaret();
    bool pass = Check(paint(vm), "render focused terminal");
    const auto cursor = surface.CursorPoint(bounds);
    const auto delay = GetCaretBlinkTime();
    const auto due = EmbeddedTerminalCaretDeadline(*state);
    std::printf("[PIXELS] caret=%ld,%ld border=%d,%d\n", cursor.x + 1, cursor.y + 4,
        static_cast<int>(panel.left + (panel.right - panel.left) * 0.5f), static_cast<int>(panel.top));
    pass &= Check(state->compositor.SaveSnapshot((output / L"caret-on.png").c_str()), "capture visible caret");
    if (delay == UINT32_MAX || !state->renderer.MotionEnabled() || ui::IsHighContrast()) {
        pass &= Check(!due, "system accessibility policy leaves a steady caret");
    } else {
        SyncUiScheduling(*state);
        pass &= Check(due && state->ui_schedule.deadline == due, "the main window schedules the next caret phase");
        pass &= Check(WaitFor([&] { return GetTickCount64() >= due; }, 2500) &&
            RunUiDeadlines(*state, GetTickCount64()) && !EmbeddedTerminalCaretDeadline(*state),
            "caret deadline invalidates once and is consumed");
        pass &= Check(paint(vm) && state->compositor.SaveSnapshot((output / L"caret-off.png").c_str()),
            "capture next caret phase");
    }

    vm.terminal_focused = state->terminalFocused = false;
    pass &= Check(paint(vm) && !EmbeddedTerminalCaretDeadline(*state) &&
        state->compositor.SaveSnapshot((output / L"unfocused.png").c_str()), "blur cancels caret scheduling");

    auto preferences = state->appPrefs.terminal_display;
    const auto fonts = ui::typography::MonospaceFamilies(state->compositor.DwriteFactory());
    pass &= Check(!fonts.empty(), "DirectWrite lists available monospaced fonts");
    if (!fonts.empty()) preferences.font_family = fonts.front();
    preferences.font_size = 20;
    SetEmbeddedTerminalPreferences(*state, preferences);
    ui::HitTestResult follow;
    follow.region = ui::HitTestResult::SettingsToggle;
    follow.index = 39;
    pass &= Check(HandleSettingsControl(*state, follow) && !state->appPrefs.terminal_display.follow_directory,
        "the settings switch disables directory following");
    preferences.follow_directory = false;
    app::AppPrefs reloaded;
    pass &= Check(reloaded.FromJson(state->appPrefs.ToJson()) &&
        reloaded.terminal_display.font_family == preferences.font_family &&
        reloaded.terminal_display.font_size == 20 && !reloaded.terminal_display.follow_directory,
        "terminal preferences survive JSON round trip");
    pass &= Check(reloaded.FromJson(LR"({"terminal_font_size":500})") &&
        reloaded.terminal_display.font_size == model::TerminalPreferences::kDefaultSize &&
        reloaded.terminal_display.follow_directory, "older or invalid settings use terminal defaults");

    pass &= Check(paint(vm) && surface.CursorPoint(bounds).x > cursor.x &&
        state->compositor.SaveSnapshot((output / L"large-font.png").c_str()),
        "changing fonts reflows the current session and updates its cell geometry");

    const auto terminal_text = [&] {
        surface.Select(bounds.left + 1, bounds.top + 1, bounds, false);
        surface.Select(bounds.right - 1, bounds.bottom - 1, bounds, true);
        return surface.SelectedText();
    };
    const auto contains = [&](std::wstring_view text) { return terminal_text().find(text) != std::wstring::npos; };
    const auto followed = [&] { return contains(L"PS C:\\Windows>"); };
    const auto navigate = [&](const wchar_t* path) {
        tab->current_path = path;
        TickEmbeddedTerminal(*state, GetTickCount64(), false);
    };
    navigate(L"C:\\Windows");
    pass &= Check(!WaitFor(followed, 350), "PowerShell keeps its directory with following disabled");
    pass &= Check(HandleSettingsControl(*state, follow) && state->appPrefs.terminal_display.follow_directory,
        "the settings switch enables directory following again");
    navigate(L"C:\\Windows");
    pass &= Check(WaitFor(followed, 4000), "the same PowerShell session follows the selected directory");

    auto settings = vm;
    settings.terminal = nullptr;
    settings.dark = true;
    settings.settings_open = true;
    settings.settings_page = 9;
    settings.settings_terminal_display = preferences;
    using H = ui::HitTestResult;
    const auto controls = state->renderer.SettingsControls(settings, rect);
    for (const int index : {4, 5, 39}) {
        const H::Region region = index == 39 ? H::SettingsToggle : H::SettingsDropdown;
        const auto control = std::find_if(controls.begin(), controls.end(),
            [&](const H& target) { return target.region == region && target.index == index; });
        if (control == controls.end()) { pass &= Check(false, "terminal setting is keyboard accessible"); continue; }
        const auto& r = control->control_bounds;
        const auto hit = state->renderer.HitTest(settings, rect, (r.left + r.right) / 2, (r.top + r.bottom) / 2);
        pass &= Check(hit.region == region && hit.index == index, "terminal setting shares mouse and keyboard geometry");
    }
    const bool entered_settings = paint(settings);
    const auto settings_settled = GetTickCount64() + 300;
    WaitFor([&] { return GetTickCount64() >= settings_settled; }, 500);
    pass &= Check(entered_settings && paint(settings) &&
        state->compositor.SaveSnapshot((output / L"terminal-settings.png").c_str()),
        "render terminal settings after the page transition");

    surface.Configure(model::TerminalPreferences{});
    paint(vm);
    pass &= Check(WaitFor([&] { return !surface.HasForegroundTask() && followed(); }, 2500),
        "PowerShell is idle before testing lifecycle");
    const auto original_id = surface.NotificationId();
    surface.Paste(L"sleep -Milliseconds 1800; echo ('PULSE_'+'FINISHED')");
    surface.Character(L'\r', false);
    pass &= Check(surface.HasForegroundTask(), "submitting a PowerShell command marks the session busy");
    navigate(L"C:\\Windows\\System32");
    navigate(L"C:\\Users");
    navigate(L"C:\\ProgramData");
    pass &= Check(!WaitFor([&] { return contains(L"PS C:\\ProgramData>"); }, 350),
        "directory changes wait while the terminal command is running");

    auto& other = state->window_tabs.NewTab(L"C:\\Windows\\System32");
    BindCurrentLayout(*state);
    pass &= Check(!state->showTerminalPanel && !BuildVm(*state, false).terminal &&
        !state->terminalFocused && surface.NotificationId() == original_id,
        "switching tabs hides the original terminal without ending its session");
    ToggleEmbeddedTerminal(*state);
    const auto other_id = other.terminal ? other.terminal->NotificationId() : 0;
    pass &= Check(state->showTerminalPanel && other_id && other_id != original_id,
        "opening a terminal in another tab starts an independent session");
    ToggleEmbeddedTerminal(*state);
    pass &= Check(!state->showTerminalPanel && other.terminal && other.terminal->NotificationId() == other_id,
        "toolbar toggle hides a session while keeping its process");
    ToggleEmbeddedTerminal(*state);
    pass &= Check(state->showTerminalPanel && other.terminal && other.terminal->NotificationId() == other_id,
        "toolbar reopens the same session");

    pass &= Check(WaitFor([&] { return contains(L"PULSE_FINISHED") && contains(L"PS C:\\ProgramData>"); }, 8000),
        "while another tab is active, the original task finishes and applies its final path");
    const auto transcript = terminal_text();
    const std::wstring final_prompt = L"PS C:\\ProgramData>";
    const auto final_position = transcript.find(final_prompt);
    pass &= Check(final_position != std::wstring::npos &&
        transcript.find(final_prompt, final_position + final_prompt.size()) == std::wstring::npos &&
        transcript.find(L"PS C:\\Users>") == std::wstring::npos &&
        transcript.find(L"PS C:\\Windows\\System32>") == std::wstring::npos,
        "only the final requested directory reaches the original shell");

    state->window_tabs.SwitchTab(0);
    BindCurrentLayout(*state);
    state->renderer.SnapTerminalPanel();
    pass &= Check(state->showTerminalPanel && BuildVm(*state, false).terminal == &surface &&
        surface.NotificationId() == original_id && contains(L"PULSE_FINISHED"),
        "returning to a tab restores its session and output");
    const auto close = state->renderer.TerminalCloseRect(rect.right, rect.bottom);
    const LPARAM click = MAKELPARAM(static_cast<int>((close.left + close.right) / 2),
        static_cast<int>((close.top + close.bottom) / 2));
    auto ended = layout.terminal;
    LRESULT result = 0;
    HandleEmbeddedTerminalMessage(*state, WM_LBUTTONDOWN, 0, click, result);
    HandleEmbeddedTerminalMessage(*state, WM_LBUTTONUP, 0, click, result);
    pass &= Check(!state->showTerminalPanel && !layout.terminal && !ended->NotificationId(),
        "the terminal close button destroys only its current session");
    ToggleEmbeddedTerminal(*state);
    pass &= Check(layout.terminal && layout.terminal->NotificationId() &&
        layout.terminal->NotificationId() != original_id, "reopening after close starts a new session");
    state->window_tabs.before_close = [](app::LayoutTab& closing) { StopEmbeddedTerminal(closing); };
    state->window_tabs.confirm_close = [](const app::LayoutTab&) { return false; };
    const auto other_terminal = other.terminal;
    pass &= Check(!state->window_tabs.CloseTab(1) && other.terminal == other_terminal,
        "canceling tab close preserves its terminal session");
    state->window_tabs.confirm_close = {};
    pass &= Check(state->window_tabs.CloseTab(1) && !other_terminal->NotificationId() && layout.terminal,
        "closing the owning tab ends its hidden terminal and keeps the active session");

    vm.terminal = layout.terminal.get();
    state->renderer.SnapTerminalPanel();
    const auto settle_hover = [&] { return WaitFor([&] { return paint(vm) && !state->renderer.SurfaceAnimating(); }, 750); };
    const bool animate_hover = state->renderer.MotionEnabled() && !ui::IsHighContrast();
    pass &= Check(settle_hover(), "terminal is at rest before hover");
    vm.hover_region = static_cast<int>(ui::HitTestResult::TerminalClose);
    pass &= Check(paint(vm) && state->renderer.SurfaceAnimating() == animate_hover,
        "terminal close hover follows the system animation policy");
    vm.hover_region = static_cast<int>(ui::HitTestResult::DetailsResize);
    pass &= Check(paint(vm) && state->renderer.SurfaceAnimating() == animate_hover && settle_hover(),
        "rapid hover change redirects the terminal close and splitter highlights");
    vm.hover_region = 0;
    pass &= Check(paint(vm) && state->renderer.SurfaceAnimating() == animate_hover && settle_hover(),
        "leaving the terminal splitter settles and stops requesting frames");

    StopEmbeddedTerminal(layout);
    StopUiScheduling(*state);
    state->core.windows.Remove(state->hwnd);
    DestroyWindow(state->hwnd);
    state->hwnd = nullptr;
    state.reset();
    OleUninitialize();
    return pass && scrollback_pass && selection_links_pass ? 0 : 1;
}

