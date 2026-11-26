// SPDX-License-Identifier: Apache-2.0
#include "app_terminal.h"
#include "app_commands.h"
#include "app_navigation.h"
#include "app_state.h"
#include "archive_navigation.h"
#include "tab_shortcuts.h"
#include "../common/path_utils.h"
#include "../ui/animation_clock.h"
#include "../ui/file_operation_dialog.h"
#include "../ui/terminal_surface.h"
#include <windowsx.h>
#include <imm.h>
#include <cwchar>
#include <utility>

namespace pulse {
namespace {

std::wstring TerminalDirectory(const app::LayoutTab& layout) {
    const auto* tab = layout.ActiveFolder();
    if (!tab || (tab->loading && tab->snapshot_path != tab->current_path) ||
        tab->current_path.empty() || fs::IsVirtualPath(tab->current_path) || IsArchiveView(tab->current_path)) return {};
    return path::StripExtendedPathPrefix(tab->current_path);
}

bool PanelFits(const AppState& state) {
    return state.renderer.TerminalFits(static_cast<float>(state.compositor.Width()));
}

void FocusTerminal(AppState& state, bool focus) {
    if (focus) SetFocus(state.hwnd);
    if (state.terminalFocused == focus) return;
    state.terminalFocused = focus;
    if (auto terminal = state.presentedTerminal.lock()) terminal->ResetCaret();
    state.terminalSuppressedChar = 0;
    InvalidateRect(state.hwnd, nullptr, FALSE);
}

void EndPresentedTerminal(AppState& state) {
    auto* layout = state.window_tabs.Active();
    const auto terminal = state.presentedTerminal.lock();
    if (!layout || !terminal || layout->terminal != terminal || !ConfirmEndEmbeddedTerminal(state, *layout)) return;
    const auto found = std::find_if(state.window_tabs.items.begin(), state.window_tabs.items.end(),
        [&](const auto& item) { return item.get() == layout && item->terminal == terminal; });
    if (found != state.window_tabs.items.end()) {
        StopEmbeddedTerminal(**found);
        SyncEmbeddedTerminal(state);
    }
}

std::wstring ClipboardText(HWND owner) {
    if (!OpenClipboard(owner)) return {};
    const auto handle = GetClipboardData(CF_UNICODETEXT);
    const auto chars = static_cast<const wchar_t*>(handle ? GlobalLock(handle) : nullptr);
    std::wstring result;
    if (chars) {
        const auto capacity = (std::min)(GlobalSize(handle) / sizeof(wchar_t), size_t{65536});
        if (std::wmemchr(chars, L'\0', capacity)) result.assign(chars);
        GlobalUnlock(handle);
    }
    CloseClipboard();
    return result;
}

void TerminalMenu(AppState& state, POINT point) {
    const auto terminal = state.presentedTerminal.lock();
    if (!terminal || !EnsureMenu(state)) return;
    ui::FluentMenuItem copy;
    copy.command = app::CmdCopy;
    copy.text = l10n::Get(l10n::StringId::Copy);
    copy.glyph = L"\xE8C8";
    copy.enabled = terminal->HasSelection();
    ui::FluentMenuItem paste;
    paste.command = app::CmdPaste;
    paste.text = l10n::Get(l10n::StringId::Paste);
    paste.glyph = L"\xE77F";
    paste.enabled = IsClipboardFormatAvailable(CF_UNICODETEXT) != FALSE;
    ClientToScreen(state.hwnd, &point);
    const int command = state.menu->TrackPopup(point, {copy, paste});
    if (!state.showTerminalPanel || ActiveEmbeddedTerminal(state) != terminal.get()) return;
    FocusTerminal(state, true);
    if (command == app::CmdCopy) ops::WriteClipboardText(terminal->SelectedText(), state.hwnd);
    else if (command == app::CmdPaste) terminal->Paste(ClipboardText(state.hwnd));
}

bool Within(const D2D1_RECT_F& rect, POINT point) {
    return static_cast<float>(point.x) >= rect.left && static_cast<float>(point.x) < rect.right &&
           static_cast<float>(point.y) >= rect.top && static_cast<float>(point.y) < rect.bottom;
}

void PositionIme(HWND hwnd, POINT cursor) {
    if (HIMC context = ImmGetContext(hwnd)) {
        COMPOSITIONFORM composition{};
        composition.dwStyle = CFS_POINT;
        composition.ptCurrentPos = cursor;
        ImmSetCompositionWindow(context, &composition);
        ImmReleaseContext(hwnd, context);
    }
}

} // namespace

ui::TerminalSurface* ActiveEmbeddedTerminal(const AppState& state) {
    const auto* layout = state.window_tabs.Active();
    return layout ? layout->terminal.get() : nullptr;
}

void SyncEmbeddedTerminal(AppState& state, bool layout_changed) {
    const auto* active = state.window_tabs.Active();
    const auto previous = state.presentedTerminal.lock();
    const auto next = active && active->terminal_open && !IsSettingsTab(active->ActiveFolder())
        ? active->terminal : nullptr;
    const bool show = static_cast<bool>(next);
    if (previous == next && state.showTerminalPanel == show) return;
    if (previous) {
        previous->StopScroll();
        previous->ResetCaret();
    }
    state.presentedTerminal = next;
    state.showTerminalPanel = show;
    state.terminalFocused = false;
    state.terminalSuppressedChar = 0;
    if ((state.terminalSelecting || state.terminalClosePressed || !state.terminalPressedLink.empty()) &&
        GetCapture() == state.hwnd) ReleaseCapture();
    state.terminalSelecting = state.terminalClosePressed = false;
    state.terminalPressedLink.clear();
    if (layout_changed) state.renderer.ClearSidePanelSnapshot();
    state.renderer.SetTerminalPanelVisible(show);
    state.renderer.SetDetailsPanelVisible(state.showDetailsPanel && !show);
    state.hitTestVmValid = false;
    if (state.hwnd) InvalidateRect(state.hwnd, nullptr, FALSE);
}

bool ConfirmEndEmbeddedTerminal(AppState& state, const app::LayoutTab& layout) {
    if (!layout.terminal || !layout.terminal->HasForegroundTask()) return true;
    using I = l10n::StringId;
    ui::ConfirmDialogSpec spec;
    spec.title = l10n::Get(I::TerminalEnd);
    spec.message = l10n::Get(I::TerminalEndRunningTask);
    spec.confirm_text = spec.title;
    spec.danger = true;
    spec.cancel_is_default = true;
    return ui::ShowConfirmDialog(state.hwnd, spec, state.darkMode, state.accentColor);
}

void StopEmbeddedTerminal(app::LayoutTab& layout) {
    layout.terminal_open = false;
    if (layout.terminal) layout.terminal->Stop();
    layout.terminal.reset();
}

void ToggleEmbeddedTerminal(AppState& state) {
    using I = l10n::StringId;
    auto* layout = state.window_tabs.Active();
    if (!layout || IsSettingsTab(layout->ActiveFolder())) return;
    if (layout->terminal_open) {
        layout->terminal_open = false;
        SyncEmbeddedTerminal(state);
        return;
    }
    if (!ui::TerminalSurface::Supported() || !PanelFits(state)) {
        state.notification_toast.ShowError(state.hwnd, l10n::Get(I::TerminalPanel),
            l10n::Get(ui::TerminalSurface::Supported() ? I::TerminalNeedWidth : I::TerminalUnavailable));
        InvalidateRect(state.hwnd, nullptr, FALSE);
        return;
    }
    if (!layout->terminal || layout->terminal->Ended()) {
        if (!layout->terminal) layout->terminal = std::make_shared<ui::TerminalSurface>();
        layout->terminal->Configure(state.appPrefs.terminal_display);
        auto directory = TerminalDirectory(*layout);
        if (directory.empty()) directory = path::DefaultNavigationRoot();
        layout->terminal->Start(state.hwnd, directory, state.isolatedTest || state.shot.active,
            state.appPrefs.terminal_shell, state.appPrefs.terminal_pwsh_path);
    }
    layout->terminal_open = true;
    SyncEmbeddedTerminal(state);
    FocusTerminal(state, true);
}

void SetEmbeddedTerminalPreferences(AppState& state, const model::TerminalPreferences& preferences) {
    const auto previous = state.appPrefs.terminal_display;
    if (previous == preferences) return;
    state.appPrefs.terminal_display = preferences;
    if (!state.appPrefs.Save()) {
        state.appPrefs.terminal_display = previous;
        state.settings.SetError(l10n::Get(l10n::StringId::SettingsSystemApplySaveError));
        return;
    }
    state.settings.ClearError();
    state.core.windows.ForEach([&](HWND hwnd, AppState& window) {
        for (const auto& layout : window.window_tabs.items)
            if (layout->terminal) layout->terminal->Configure(preferences);
        InvalidateRect(hwnd, nullptr, FALSE);
    });
}

uint64_t EmbeddedTerminalCaretDeadline(const AppState& state) {
    const auto* terminal = ActiveEmbeddedTerminal(state);
    if (!terminal || !state.showTerminalPanel || !state.terminalFocused ||
        !PanelFits(state) || IsSettingsTab(state.pane ? state.pane->ActiveTab() : nullptr)) return 0;
    return terminal->CaretDeadline();
}

void ToggleDetailsPanel(AppState& state) {
    if (state.showTerminalPanel) {
        state.showDetailsPanel = true;
        if (auto* layout = state.window_tabs.Active()) layout->terminal_open = false;
        SyncEmbeddedTerminal(state);
    } else {
        state.showDetailsPanel = !state.showDetailsPanel;
        state.renderer.SetDetailsPanelVisible(state.showDetailsPanel);
    }
}

bool TickEmbeddedTerminal(AppState& state, uint64_t now, bool animate) {
    SyncEmbeddedTerminal(state);
    const auto* layout = state.window_tabs.Active();
    if (!layout || !layout->terminal) return false;
    auto& terminal = *layout->terminal;
    terminal.FollowDirectory(TerminalDirectory(*layout));
    if (!state.showTerminalPanel) return false;
    if (!PanelFits(state)) {
        FocusTerminal(state, false);
        terminal.StopScroll();
        return false;
    }
    return terminal.TickScroll(now, animate);
}

bool HandleEmbeddedTerminalMessage(AppState& state, UINT message, WPARAM wp, LPARAM lp, LRESULT& result) {
    if (message == ops::kTerminalUpdated) {
        for (const auto& layout : state.window_tabs.items) {
            if (!wp || !layout->terminal || layout->terminal->NotificationId() != wp) continue;
            layout->terminal->FollowDirectory(TerminalDirectory(*layout));
            layout->terminal->Update();
            if (layout.get() == state.window_tabs.Active() && state.showTerminalPanel && PanelFits(state) &&
                IsWindowVisible(state.hwnd) && !IsIconic(state.hwnd)) InvalidateRect(state.hwnd, nullptr, FALSE);
            break;
        }
        return true;
    }
    if (message == WM_DESTROY) {
        for (const auto& layout : state.window_tabs.items) StopEmbeddedTerminal(*layout);
        state.presentedTerminal.reset();
        return false;
    }
    auto* surface = ActiveEmbeddedTerminal(state);
    if (!surface) return false;
    const auto hwnd = state.hwnd;
    if (message == WM_KILLFOCUS) FocusTerminal(state, false);
    if (message == WM_CAPTURECHANGED || message == WM_CANCELMODE) {
        state.terminalSelecting = state.terminalClosePressed = false;
        state.terminalPressedLink.clear();
    }
    if (!state.showTerminalPanel || state.detailsPanelResizing || !PanelFits(state) || IsSettingsTab(ActiveTab(state)))
        return false;
    auto& terminal = *surface;
    const auto w = static_cast<float>(state.compositor.Width());
    const auto h = static_cast<float>(state.compositor.Height());
    const auto body = state.renderer.TerminalBodyRect(w, h);
    const bool animate = state.renderer.MotionEnabled() && !ui::IsHighContrast();
    if (message == WM_CHAR || message == WM_SYSCHAR) {
        if (!state.terminalFocused) return false;
        const auto suppressed = std::exchange(state.terminalSuppressedChar, 0);
        if (wp != suppressed) terminal.Character(static_cast<wchar_t>(wp), message == WM_SYSCHAR);
        InvalidateRect(hwnd, nullptr, FALSE);
        return true;
    }
    if (message == WM_KEYDOWN || message == WM_SYSKEYDOWN) {
        if (!state.terminalFocused) return false;
        const bool ctrl = GetKeyState(VK_CONTROL) < 0;
        const bool shift = GetKeyState(VK_SHIFT) < 0;
        const bool alt = GetKeyState(VK_MENU) < 0;
        const UINT key = static_cast<UINT>(wp);
        if (app::IsTabShortcut(key, ctrl, shift, alt)) {
            state.terminalSuppressedChar = L'\t';
            return false;
        }
        state.terminalSuppressedChar = 0;
        if (app::LookupCommandShortcut(key, ctrl, shift, alt) == app::CmdTerminalPanel) {
            ToggleEmbeddedTerminal(state);
        } else if (ctrl && !alt && key == 'C' && (shift || terminal.HasSelection())) {
            if (terminal.HasSelection()) ops::WriteClipboardText(terminal.SelectedText(), hwnd);
            state.terminalSuppressedChar = 3;
        } else if (ctrl && !alt && key == 'V') {
            terminal.Paste(ClipboardText(hwnd));
            state.terminalSuppressedChar = 22;
        } else if (!(alt && key == VK_F4) && terminal.Key(key, shift, alt, ctrl, ui::AnimationTimeMs(), animate)) {
            InvalidateRect(hwnd, nullptr, FALSE);
        } else {
            result = DefWindowProcW(hwnd, message, wp, lp);
        }
        return true;
    }
    if (message == WM_IME_STARTCOMPOSITION && state.terminalFocused) {
        PositionIme(hwnd, terminal.CursorPoint(body));
        return false; // Windows owns composition and commits it through WM_CHAR.
    }
    if (message == WM_SETCURSOR && LOWORD(lp) == HTCLIENT) {
        POINT cursor{};
        GetCursorPos(&cursor);
        ScreenToClient(hwnd, &cursor);
        if (Within(body, cursor) && static_cast<float>(cursor.x) < w - state.renderer.Margin()) {
            const bool link = GetKeyState(VK_CONTROL) < 0 &&
                !terminal.LinkAt(static_cast<float>(cursor.x), static_cast<float>(cursor.y), body).empty();
            SetCursor(LoadCursorW(nullptr, link ? IDC_HAND : IDC_IBEAM));
            result = TRUE;
            return true;
        }
    }

    if (message != WM_LBUTTONDOWN && message != WM_LBUTTONUP && message != WM_LBUTTONDBLCLK &&
        message != WM_MOUSEMOVE && message != WM_MOUSEWHEEL && message != WM_MOUSEHWHEEL &&
        message != WM_RBUTTONDOWN && message != WM_RBUTTONUP) return false;
    POINT point{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
    if (message == WM_MOUSEWHEEL || message == WM_MOUSEHWHEEL) ScreenToClient(hwnd, &point);
    auto panel = state.renderer.DetailsPanelRect(w, h);
    panel.left += 4.0f * state.scale; // the common splitter owns this edge
    panel.right = (std::min)(panel.right, w - state.renderer.Margin());
    const bool inside = Within(panel, point);
    const bool close = inside && Within(state.renderer.TerminalCloseRect(w, h), point);
    const float x = static_cast<float>(point.x), y = static_cast<float>(point.y);
    if (message == WM_LBUTTONDOWN) {
        FocusTerminal(state, inside);
        if (!inside) return false;
        if (close) {
            state.terminalClosePressed = true;
            SetCapture(hwnd);
        } else if (Within(body, point)) {
            state.terminalPressedLink = (wp & MK_CONTROL) ? terminal.LinkAt(x, y, body) : std::wstring{};
            if (state.terminalPressedLink.empty()) {
                terminal.Select(x, y, body, false);
                state.terminalSelecting = true;
            } else {
                terminal.StopScroll();
            }
            SetCapture(hwnd);
        }
    } else if (!state.terminalPressedLink.empty() && (message == WM_MOUSEMOVE || message == WM_LBUTTONUP)) {
        if (message == WM_MOUSEMOVE) return true;
        const auto pressed = std::exchange(state.terminalPressedLink, {});
        if (GetCapture() == hwnd) ReleaseCapture();
        if ((wp & MK_CONTROL) && Within(body, point) && terminal.LinkAt(x, y, body) == pressed)
            state.ops.OpenWith(pressed);
        return true;
    } else if (state.terminalSelecting && (message == WM_MOUSEMOVE || message == WM_LBUTTONUP)) {
        terminal.Select(x, y, body, true);
    } else if (message == WM_MOUSEMOVE) {
        return false;
    } else if (message == WM_LBUTTONUP && state.terminalClosePressed && close) {
        state.terminalClosePressed = false;
        if (GetCapture() == hwnd) ReleaseCapture();
        EndPresentedTerminal(state);
        return true;
    } else if (message == WM_MOUSEWHEEL && inside) {
        terminal.Scroll(GET_WHEEL_DELTA_WPARAM(wp), ui::AnimationTimeMs(), animate);
    } else if (message == WM_RBUTTONUP && inside) {
        TerminalMenu(state, point);
    } else if (!inside && !state.terminalClosePressed) {
        return false;
    }
    if (message == WM_LBUTTONUP && (state.terminalSelecting || state.terminalClosePressed)) {
        state.terminalSelecting = state.terminalClosePressed = false;
        if (GetCapture() == hwnd) ReleaseCapture();
    }
    InvalidateRect(hwnd, nullptr, FALSE);
    return true;
}

} // namespace pulse
