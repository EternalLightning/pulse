// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
#include "../common/windows_compat.h"
#include "../common/localization.h"
#include "../ui/file_operation_dialog.h"
#include "../ui/notification_toast.h"
#include "../ui/dialog_style.h"
#include "snapshot_output.h"
#include <windowsx.h>
#include <cmath>
#include <cstdio>
#include <cwchar>
#include <functional>

namespace pulse::ui {
struct NotificationToastTestPeer {
    static bool Overflow(const NotificationToast& toast) { return toast.overflow_; }
    static bool Focused(const NotificationToast& toast) { return toast.details_focus_; }
    static D2D1_RECT_F Details(const NotificationToast& toast) { return toast.details_; }
    static const std::wstring& Message(const NotificationToast& toast) { return toast.message_; }
    static const std::wstring& Preview(const NotificationToast& toast) { return toast.preview_; }
};
struct FileOperationFailureProbe {
    static HWND Window(const FileOperationWindow& window) { return window.hwnd_; }
    static const FileOperationProgressInspection& Inspect(FileOperationWindow& window) {
        window.Render(false);
        return window.progress_inspection_;
    }
    static bool Capture(FileOperationWindow& window, const std::filesystem::path& output) {
        window.Render(false);
        return bench::SaveSnapshot(window.compositor_, output);
    }
    static bool Pixels(FileOperationWindow& window, DialogPixelInspection& pixels) {
        window.Render(false);
        return InspectDialogPixels(window.compositor_, pixels);
    }
};
}

namespace {
using namespace pulse;
int failures = 0;
void Check(bool result, const char* label) {
    std::printf("[%s] %s\n", result ? "PASS" : "FAIL", label);
    failures += !result;
}
bool Near(float actual, float expected) { return std::abs(actual - expected) < 1.5f; }
std::function<void(HWND)> modal_action;
const wchar_t* modal_class = nullptr;
bool modal_seen = false;
UINT_PTR ArmModal(const wchar_t* name, std::function<void(HWND)> action) {
    modal_class = name;
    modal_action = std::move(action);
    modal_seen = false;
    return SetTimer(nullptr, 0, 100, [](HWND, UINT, UINT_PTR timer, DWORD) {
        EnumThreadWindows(GetCurrentThreadId(), [](HWND dialog, LPARAM timer_id) -> BOOL {
            wchar_t name[80]{};
            GetClassNameW(dialog, name, ARRAYSIZE(name));
            if (std::wcscmp(name, modal_class) || !IsWindowVisible(dialog)) return TRUE;
            KillTimer(nullptr, static_cast<UINT_PTR>(timer_id));
            modal_seen = true;
            modal_action(dialog);
            return FALSE;
        }, static_cast<LPARAM>(timer));
    });
}
void Snapshot(HWND dialog, const std::filesystem::path& output) {
    ui::DialogPixelInspection pixels;
    Check(SendMessageW(dialog, ui::kDialogInspectPixelsMessage, 0, reinterpret_cast<LPARAM>(&pixels)) &&
        pixels.varied && pixels.nonblack, "modal screenshot contains varied nonblack rendered pixels");
    const auto path = std::filesystem::absolute(output).wstring();
    Check(SendMessageW(dialog, ui::kConfirmSnapshotMessage, 0,
        reinterpret_cast<LPARAM>(path.c_str())) != 0, "capture composed modal");
}
}

int wmain(int argc, wchar_t** argv) {
    const bool title_only = argc == 2 && std::wcscmp(argv[1], L"--title-style-only") == 0;
    const bool conflict_only = argc == 2 && std::wcscmp(argv[1], L"--conflict-only") == 0;
    compat::EnableDpiAwareness();
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    l10n::Initialize(GetModuleHandleW(nullptr), L"en-US");
    WNDCLASSW wc{};
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpfnWndProc = DefWindowProcW;
    wc.lpszClassName = L"PulsePopupOperationStyleFixture";
    RegisterClassW(&wc);
    const HWND owner = CreateWindowExW(0, wc.lpszClassName, L"Popup operation fixture",
        WS_OVERLAPPEDWINDOW, 100, 100, 1000, 760, nullptr, nullptr, wc.hInstance, nullptr);
    Check(owner != nullptr, "create isolated fixture without file operations");
    if (!owner) { CoUninitialize(); return 1; }
    const std::filesystem::path output = L"bench_data/popup-operation-style";
    if (!bench::StreamSnapshots()) std::filesystem::create_directories(output);
    for (const wchar_t* language : {L"en-US", L"zh-CN"}) {
        l10n::SetLanguage(language);
        for (const bool dark : {false, true}) {
            for (const int dpi : {96, 144, 168, 192}) {
                if (!title_only && !conflict_only && dpi > 144) continue;
                const float fixture_scale = dpi / 96.0f;
                const auto theme = ui::MakeTheme(dark, ui::HexColor(0x0078D4));
                const auto suffix = std::wstring(language) + (dark ? L"-dark-" : L"-light-") +
                    std::to_wstring(dpi) + L".png";
                const auto set_dpi = [&](HWND dialog, int width, int height) {
                    RECT rect{100, 100, 100 + static_cast<LONG>(width * fixture_scale),
                        100 + static_cast<LONG>(height * fixture_scale)};
                    SendMessageW(dialog, WM_DPICHANGED, MAKEWPARAM(dpi, dpi), reinterpret_cast<LPARAM>(&rect));
                };
                auto timer = ArmModal(L"PulseFileConflictWindow", [&](HWND dialog) {
                    // Windows can suggest a rectangle based on the previous
                    // monitor's window size during an owner-monitor transition.
                    set_dpi(dialog, 284, 258);
                    RECT client{};
                    GetClientRect(dialog, &client);
                    Check(client.right == static_cast<LONG>(std::ceil(480 * fixture_scale)) &&
                        client.bottom == static_cast<LONG>(std::ceil(428 * fixture_scale)),
                        "conflict client fits the full fixed layout after a DPI change");
                    const float scale = fixture_scale;
                    Check((GetWindowLongPtrW(dialog, GWL_STYLE) & WS_THICKFRAME) == 0,
                        "conflict fixed layout has no resize frame");
                    RECT window{}; GetWindowRect(dialog, &window);
                    const auto edge = MAKELPARAM(window.left + 1, window.top + static_cast<LONG>(100 * scale));
                    Check(SendMessageW(dialog, WM_NCHITTEST, 0, edge) == HTCLIENT,
                        "conflict side edge cannot start a resize");
                    ui::ConflictDialogInspection info;
                    SendMessageW(dialog, ui::kConflictInspectMessage, 0, reinterpret_cast<LPARAM>(&info));
                    Check(Near(info.title_font_size, static_cast<float>(MulDiv(14, dpi, 96))) &&
                        info.title_weight == DWRITE_FONT_WEIGHT_BOLD,
                        "conflict title matches the native 14-DIP bold caption at this DPI");
                    Check(Near(info.close.right - info.close.left, 46 * scale) &&
                        Near(info.close.bottom, 44 * scale) &&
                        Near(info.cancel.bottom - info.cancel.top, 32 * scale) &&
                        info.cancel.right - info.cancel.left >= 79.9f * scale,
                        "conflict header close and footer use shared sizes");
                    for (int index = 0; index < 3; ++index) SendMessageW(dialog, WM_KEYDOWN, VK_TAB, 0);
                    Snapshot(dialog, output / (L"conflict-check-" + suffix));
                    SendMessageW(dialog, WM_KEYDOWN, VK_SPACE, 0);
                    SendMessageW(dialog, WM_KEYDOWN, VK_TAB, 0);
                    Snapshot(dialog, output / (L"conflict-details-focus-" + suffix));
                    SendMessageW(dialog, WM_KEYDOWN, VK_RETURN, 0);
                    SendMessageW(dialog, ui::kConflictInspectMessage, 0, reinterpret_cast<LPARAM>(&info));
                    Check(info.expanded && info.focus == 4, "keyboard details expands fixed conflict layout");
                    Snapshot(dialog, output / (L"conflict-expanded-" + suffix));
                    SendMessageW(dialog, WM_KEYDOWN, VK_TAB, 0);
                    Snapshot(dialog, output / (L"conflict-cancel-focus-" + suffix));
                    SendMessageW(dialog, WM_KEYDOWN, VK_ESCAPE, 0);
                });
                ops::ConflictItemInfo conflict;
                conflict.source = L"E:\\source\\annual report.pdf";
                conflict.destination = L"E:\\destination\\annual report.pdf";
                conflict.remaining = 3;
                const auto choice = ui::ShowFileConflictDialog(owner, conflict, dark, theme.accent);
                KillTimer(nullptr, timer);
                Check(modal_seen && choice.apply_to_all && choice.choice == ops::ConflictChoice::Cancel,
                    "keyboard checkbox preserves apply-all result on cancel");
                if (conflict_only) continue;

                timer = ArmModal(L"PulseConfirmWindow", [&](HWND dialog) {
                    set_dpi(dialog, 380, 220);
                    const float scale = fixture_scale;
                    ui::ConfirmDialogInspection info;
                    SendMessageW(dialog, ui::kConfirmInspectMessage, 0, reinterpret_cast<LPARAM>(&info));
                    Check(Near(info.title_font_size, static_cast<float>(MulDiv(14, dpi, 96))) &&
                        info.title_weight == DWRITE_FONT_WEIGHT_BOLD,
                        "confirmation title matches the native 14-DIP bold caption at this DPI");
                    Check(Near(info.close.bottom, 44 * scale) &&
                        Near(info.primary.bottom - info.primary.top, 32 * scale) &&
                        info.primary.right - info.primary.left >= 79.9f * scale &&
                        info.cancel.right - info.cancel.left >= 79.9f * scale &&
                        info.alternate.right - info.alternate.left >= 79.9f * scale &&
                        info.alternate.right < info.cancel.left && info.cancel.right < info.primary.left,
                        "confirm header and alternate-cancel-primary footer are consistent");
                    SendMessageW(dialog, WM_KEYDOWN, VK_TAB, 0);
                    Snapshot(dialog, output / (L"confirm-cancel-" + suffix));
                    SendMessageW(dialog, WM_KEYDOWN, VK_TAB, 0);
                    Snapshot(dialog, output / (L"confirm-check-" + suffix));
                    {
                        ui::HighContrastFrameScope high_contrast(true);
                        ui::DialogPixelInspection pixels;
                        pixels.bounds = D2D1::RectF(20 * scale, 12 * scale, 40 * scale, 32 * scale);
                        pixels.expected = ui::MakeHighContrastTheme().text;
                        Check(SendMessageW(dialog, ui::kDialogInspectPixelsMessage, 0,
                            reinterpret_cast<LPARAM>(&pixels)) && pixels.matching > 3,
                            "high-contrast confirmation warning glyph contains system foreground pixels");
                        Snapshot(dialog, output / (L"confirm-high-contrast-" + suffix));
                    }
                    SendMessageW(dialog, WM_KEYDOWN, VK_ESCAPE, 0);
                });
                ui::ConfirmDialogSpec confirm;
                confirm.title = std::wcscmp(language, L"zh-CN") == 0 ? L"确认永久删除" : L"Replace files?";
                confirm.message = L"The destination already contains a file with the same name.";
                confirm.confirm_text = l10n::Get(l10n::StringId::ConfirmDefault);
                confirm.alternate_text = l10n::Get(l10n::StringId::OpSkipFile);
                confirm.checkbox_text = L"Apply to all remaining files";
                ui::ShowConfirmDialog(owner, confirm, dark, theme.accent);
                KillTimer(nullptr, timer);
                Check(modal_seen, "confirmation fixture was inspected");

                ui::FileOperationWindow progress;
                Check(progress.Create(owner, {{}, [] {}, {}, {}}), "create synthetic progress fixture");
                progress.SetTheme(dark, theme.accent);
                ops::OpStatus status;
                status.active = true;
                status.type = ops::OpType::Copy;
                status.phase = ops::OpPhase::Running;
                status.source_label = L"E:\\source";
                status.destination_label = L"E:\\destination";
                status.current_item = L"annual report.pdf";
                progress.Update(status);
                const HWND progress_hwnd = ui::FileOperationFailureProbe::Window(progress);
                set_dpi(progress_hwnd, 460, 268);
                MINMAXINFO minimum{};
                SendMessageW(progress_hwnd, WM_GETMINMAXINFO, 0, reinterpret_cast<LPARAM>(&minimum));
                const auto scale = fixture_scale;
                Check(minimum.ptMinTrackSize.x >= 460 * scale && minimum.ptMinTrackSize.y >= 268 * scale,
                    "progress enforces content minimum width and height");
                SetWindowPos(progress_hwnd, nullptr, 0, 0, minimum.ptMinTrackSize.x, minimum.ptMinTrackSize.y,
                    SWP_NOMOVE | SWP_NOZORDER);
                const auto& chrome = ui::FileOperationFailureProbe::Inspect(progress);
                Check(Near(chrome.title_font_size, static_cast<float>(MulDiv(14, dpi, 96))) &&
                    chrome.title_weight == DWRITE_FONT_WEIGHT_BOLD,
                    "progress title matches the native 14-DIP bold caption at this DPI");
                Check(chrome.details.right + 8 * scale <= chrome.pause.left &&
                    chrome.pause.right + 7 * scale <= chrome.cancel.left,
                    "progress measured localized footer labels fit at minimum width");
                Check(ui::FileOperationFailureProbe::Capture(progress, output / (L"progress-" + suffix)),
                    "capture minimum-width progress");
                {
                    ui::HighContrastFrameScope high_contrast(true);
                    RECT client{}; GetClientRect(progress_hwnd, &client);
                    ui::DialogPixelInspection pixels;
                    pixels.bounds = D2D1::RectF(4 * scale, client.bottom - 10 * scale,
                        10 * scale, client.bottom - 4 * scale);
                    pixels.expected = ui::MakeHighContrastTheme().bg;
                    Check(ui::FileOperationFailureProbe::Pixels(progress, pixels) && pixels.samples > 0 &&
                        pixels.matching == pixels.samples,
                        "high-contrast progress footer is opaque system window background at actual pixels");
                    Check(ui::FileOperationFailureProbe::Capture(progress, output / (L"progress-high-contrast-" + suffix)),
                        "capture high-contrast progress footer");
                }

                if (title_only) continue;
                ui::Compositor compositor;
                Check(compositor.Init(owner), "create isolated toast compositor");
                if (!compositor.Dc()) continue;
                compositor.RecreateTextFormats(fixture_scale);
                ui::NotificationToast toast;
                toast.SetMotionEnabled(false);
                const auto draw = [&](const std::wstring& name, bool high_contrast = false) {
                    compositor.Dc()->BeginDraw();
                    compositor.Dc()->Clear(theme.bg);
                    toast.Draw(compositor, high_contrast ? ui::MakeHighContrastTheme() : theme, fixture_scale, high_contrast);
                    compositor.Dc()->EndDraw();
                    Check(bench::SaveSnapshot(compositor, output / name), "capture toast overflow action");
                };
                toast.ShowError(owner, L"Operation failed", L"Access denied.");
                draw(L"toast-short-" + suffix);
                Check(!ui::NotificationToastTestPeer::Overflow(toast), "short toast has no details action");
                std::wstring error = L"Original system error: \\\\?\\UNC\\server\\share\\file.txt\n";
                for (int index = 0; index < 90; ++index) error += L"Long diagnostic line with error code 0x80070005 and full original details.\n";
                error += L"FINAL ORIGINAL ERROR ENDPOINT";
                toast.ShowError(owner, L"Operation failed", error);
                draw(L"toast-overflow-" + suffix);
                Check(ui::NotificationToastTestPeer::Overflow(toast) &&
                    ui::NotificationToastTestPeer::Message(toast) == error,
                    "overflow toast preserves full original error and offers details");
                Check(toast.Bounds().bottom - toast.Bounds().top <= 204 * fixture_scale + 1 &&
                    ui::NotificationToastTestPeer::Preview(toast).ends_with(L"\x2026") &&
                    ui::NotificationToastTestPeer::Preview(toast).size() < error.size(),
                    "long toast uses compact ellipsized preview with full details behind action");
                const auto inspect_details = [&](HWND dialog) {
                    set_dpi(dialog, 380, 220);
                    RECT client{};
                    GetClientRect(dialog, &client);
                    MONITORINFO monitor{sizeof(monitor)};
                    const bool work_area_known = GetMonitorInfoW(
                        MonitorFromWindow(dialog, MONITOR_DEFAULTTONEAREST), &monitor) != FALSE;
                    Check(client.bottom <= std::ceil(640 * fixture_scale) && work_area_known &&
                        client.bottom <= monitor.rcWork.bottom - monitor.rcWork.top - 24,
                        "expanded error stays within 640 DIP and available work area");
                    ui::ConfirmDialogInspection info;
                    SendMessageW(dialog, ui::kConfirmInspectMessage, 0, reinterpret_cast<LPARAM>(&info));
                    Check(info.message == error && info.cancel.right == info.cancel.left && info.max_scroll > 0,
                        "expanded error has exact original text, one Close button and a scroll viewport");
                    Snapshot(dialog, output / (L"error-expanded-top-" + suffix));
                    SendMessageW(dialog, WM_KEYDOWN, VK_END, 0);
                    SendMessageW(dialog, ui::kConfirmInspectMessage, 0, reinterpret_cast<LPARAM>(&info));
                    Check(Near(info.scroll, info.max_scroll), "End reveals final original error endpoint");
                    Snapshot(dialog, output / (L"error-expanded-end-" + suffix));
                    SendMessageW(dialog, WM_KEYDOWN, VK_RETURN, 0);
                };
                Check(toast.HandleMessage(owner, WM_KEYDOWN, VK_F6, 0) &&
                    ui::NotificationToastTestPeer::Focused(toast), "F6 reaches long-toast details action");
                draw(L"toast-keyboard-" + suffix);
                timer = ArmModal(L"PulseConfirmWindow", inspect_details);
                Check(toast.HandleMessage(owner, WM_KEYDOWN, VK_RETURN, 0), "Enter opens full error details");
                KillTimer(nullptr, timer);
                Check(modal_seen && !toast.IsVisible(), "expanded error replaces toast");
                toast.ShowError(owner, L"Operation failed", error);
                draw(L"toast-high-contrast-" + suffix, true);
                timer = ArmModal(L"PulseConfirmWindow", inspect_details);
                // The fixture owner uses DefWindowProc; route clicks directly as the app does.
                const auto details = ui::NotificationToastTestPeer::Details(toast);
                const auto point = MAKELPARAM(static_cast<WORD>((details.left + details.right) / 2),
                    static_cast<WORD>((details.top + details.bottom) / 2));
                toast.HandleMessage(owner, WM_LBUTTONDOWN, MK_LBUTTON, point);
                toast.HandleMessage(owner, WM_LBUTTONUP, 0, point);
                KillTimer(nullptr, timer);
                Check(modal_seen, "mouse details action opens full original error");
            }
        }
    }
    DestroyWindow(owner);
    CoUninitialize();
    std::printf("[RESULT] failures=%d\n", failures);
    return failures ? 1 : 0;
}
