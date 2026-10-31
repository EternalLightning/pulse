// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "../ui/file_operation_dialog.h"
#include "../app/exit_confirmation.h"
#include "../common/localization.h"
#include "../common/windows_compat.h"
#include "snapshot_output.h"
#include <cstdio>
#include <filesystem>
#include <thread>
#include <atomic>
#include <algorithm>

namespace {
bool CaptureVisibleClient(HWND dialog, const std::filesystem::path& output, bool dark) {
    RECT client{};
    POINT origin{};
    if (!GetClientRect(dialog, &client) || !ClientToScreen(dialog, &origin)) return false;
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = client.right;
    info.bmiHeader.biHeight = -client.bottom;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    HDC screen = GetDC(nullptr), memory = CreateCompatibleDC(screen);
    void* pixels = nullptr;
    HBITMAP bitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    bool captured = false;
    if (memory && bitmap && pixels) {
        const auto previous = SelectObject(memory, bitmap);
        if (BitBlt(memory, 0, 0, client.right, client.bottom, screen, origin.x, origin.y, SRCCOPY)) {
            auto* bytes = static_cast<BYTE*>(pixels);
            for (size_t i = 0; i < static_cast<size_t>(client.right) * client.bottom; ++i) bytes[i * 4 + 3] = 255;
            unsigned title_pixels = 0;
            for (LONG y = 8; y < std::min<LONG>(28, client.bottom); ++y) {
                for (LONG x = 40; x < std::min<LONG>(180, client.right); ++x) {
                    const auto* pixel = bytes + (static_cast<size_t>(y) * client.right + x) * 4;
                    if (dark ? (pixel[0] > 200 && pixel[1] > 200 && pixel[2] > 200)
                             : (pixel[0] < 80 && pixel[1] < 80 && pixel[2] < 80)) ++title_pixels;
                }
            }
            captured = title_pixels > 20;
            if (pulse::bench::StreamSnapshots())
                captured &= pulse::bench::EmitSnapshot(output, client.right, client.bottom, client.right * 4, bytes);
        }
        SelectObject(memory, previous);
    }
    if (bitmap) DeleteObject(bitmap);
    if (memory) DeleteDC(memory);
    if (screen) ReleaseDC(nullptr, screen);
    return captured;
}
}

int wmain(int argc, wchar_t** argv) {
    pulse::compat::EnableDpiAwareness();
    const HRESULT com = OleInitialize(nullptr);
    bool dark = true, exit_dialog = false, recycle_dialog = false, english = false, cancel = false, narrow = false;
    for (int i = 1; i < argc; ++i) {
        if (wcscmp(argv[i], L"--light") == 0) dark = false;
        else if (wcscmp(argv[i], L"--exit") == 0) exit_dialog = true;
        else if (wcscmp(argv[i], L"--recycle") == 0) { recycle_dialog = true; cancel = true; }
        else if (wcscmp(argv[i], L"--english") == 0) english = true;
        else if (wcscmp(argv[i], L"--cancel") == 0) cancel = true;
        else if (wcscmp(argv[i], L"--narrow") == 0) narrow = true;
    }
    pulse::l10n::Initialize(GetModuleHandleW(nullptr), english ? L"en-US" : L"zh-CN");
    const std::wstring image_name = std::wstring(recycle_dialog ? L"recycle_confirmation_" : exit_dialog ? L"exit_confirmation_" : L"link_confirmation_") +
        (dark ? L"dark" : L"light") + (english ? L"_en" : L"") +
        (narrow ? L"_144_narrow" : L"") + (cancel ? L"_cancel" : L"") + L".png";
    const auto output = std::filesystem::absolute(std::filesystem::path(L"bench_data") / image_name).wstring();
    std::filesystem::create_directories(std::filesystem::path(output).parent_path());
    std::atomic<bool> captured{false}, layout_ok{false}, visible_ok{false};
    const DWORD ui_thread = GetCurrentThreadId();
    std::thread automate([&] {
        const HRESULT capture_com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        HWND dialog = nullptr;
        const auto deadline = GetTickCount64() + 10000;
        while (!dialog && GetTickCount64() < deadline) {
            EnumThreadWindows(ui_thread, [](HWND candidate, LPARAM parameter) -> BOOL {
                wchar_t name[64]{};
                GetClassNameW(candidate, name, ARRAYSIZE(name));
                if (wcscmp(name, L"PulseConfirmWindow") != 0 || !IsWindowVisible(candidate)) return TRUE;
                *reinterpret_cast<HWND*>(parameter) = candidate;
                return FALSE;
            }, reinterpret_cast<LPARAM>(&dialog));
            if (!dialog) Sleep(10);
        }
        if (!dialog) {
            if (SUCCEEDED(capture_com)) CoUninitialize();
            return;
        }
        Sleep(250);
        if (narrow) {
            RECT rect{}; GetWindowRect(dialog, &rect);
            SendMessageW(dialog, WM_DPICHANGED, MAKEWPARAM(144, 144), reinterpret_cast<LPARAM>(&rect));
            SetWindowPos(dialog, nullptr, 0, 0, 420, 570, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            Sleep(100);
        }
        auto visible_output = std::filesystem::path(output);
        visible_output.replace_filename(visible_output.stem().wstring() + L"_visible.png");
        visible_ok = CaptureVisibleClient(dialog, visible_output, dark);
        pulse::ui::ConfirmDialogInspection inspection;
        RECT client{}; GetClientRect(dialog, &client);
        layout_ok = SendMessageW(dialog, pulse::ui::kConfirmInspectMessage, 0, reinterpret_cast<LPARAM>(&inspection)) &&
            (exit_dialog || inspection.row_count == (recycle_dialog ? 2 : 4)) &&
            inspection.content.bottom <= std::min(inspection.primary.top, inspection.cancel.top) &&
            inspection.primary.left >= 0 && inspection.cancel.left >= 0 &&
            inspection.primary.right <= client.right && inspection.cancel.right <= client.right &&
            (inspection.primary.left >= inspection.cancel.right || inspection.cancel.left >= inspection.primary.right);
        DWORD_PTR result = 0;
        captured = SendMessageTimeoutW(dialog, pulse::ui::kConfirmSnapshotMessage, 0,
            reinterpret_cast<LPARAM>(output.c_str()), SMTO_ABORTIFHUNG, 5000, &result) && result;
        if (exit_dialog) {
            // Escape keeps the task running; the destructive exit is not the default.
            PostMessageW(dialog, WM_KEYDOWN, VK_ESCAPE, 0);
        } else {
            // Cancel is the default; a single Tab reaches Continue.
            if (!cancel) PostMessageW(dialog, WM_KEYDOWN, VK_TAB, 0);
            PostMessageW(dialog, WM_KEYDOWN, VK_RETURN, 0);
        }
        if (SUCCEEDED(capture_com)) CoUninitialize();
    });
    pulse::ops::ConflictItemInfo info;
    info.link_confirmation = true;
    info.link_impact.kind = pulse::ops::DestinationLinkKind::Junction;
    info.link_impact.path = L"C:\\备份\\项目\\" + std::wstring(140, L'长') + L"链接目录";
    info.link_impact.resolved_path = L"\\\\server\\share\\重要资料\\" + std::wstring(160, L'文') + L"项目";
    bool behavior = false;
    if (recycle_dialog) {
        HWND owner = CreateWindowExW(0, L"STATIC", L"Isolated recycle UI fixture",
            WS_OVERLAPPEDWINDOW | WS_VISIBLE, 100, 100, 1000, 700, nullptr, nullptr,
            GetModuleHandleW(nullptr), nullptr);
        pulse::ui::FileOperationWindow progress;
        progress.Create(owner, {});
        progress.SetTheme(dark, D2D1::ColorF(0x0078D4));
        pulse::ops::OpStatus status;
        status.type = pulse::ops::OpType::EmptyRecycle;
        status.task_id = 1;
        status.phase = pulse::ops::OpPhase::Scanning;
        status.active = true;
        status.percent = -1;
        progress.Update(status);
        progress.Show(false);
        const auto until = GetTickCount64() + 300;
        while (GetTickCount64() < until) {
            MSG message{};
            if (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            } else Sleep(1);
        }
        HWND progress_window = nullptr;
        EnumThreadWindows(ui_thread, [](HWND candidate, LPARAM parameter) -> BOOL {
            wchar_t name[64]{};
            GetClassNameW(candidate, name, ARRAYSIZE(name));
            if (wcscmp(name, L"PulseFileOperationWindow") != 0) return TRUE;
            *reinterpret_cast<HWND*>(parameter) = candidate;
            return FALSE;
        }, reinterpret_cast<LPARAM>(&progress_window));
        const bool progress_visible = CaptureVisibleClient(progress_window, std::filesystem::path(output).parent_path() /
            (dark ? L"recycle_progress_dark_visible.png" : L"recycle_progress_light_visible.png"), dark);
        const auto hide_started = GetTickCount64();
        progress.Hide();
        const bool hidden_promptly = GetTickCount64() - hide_started < 750 && !progress.IsVisible();
        pulse::ui::ConfirmDialogSpec spec;
        using I = pulse::l10n::StringId;
        spec.title = pulse::l10n::Get(I::DeleteConfirmTitle);
        wchar_t count[256]{};
        swprintf_s(count, pulse::l10n::Get(I::DeleteCountFormat).c_str(), 2ull, 2ull, 0ull);
        spec.message = pulse::l10n::Get(I::DeleteIrreversible) + L"\n\n" + count + L"\n" +
            pulse::l10n::Get(I::DeleteReasonRecycle) + L"\n\n" + pulse::l10n::Get(I::DeletePathHeading);
        spec.confirm_text = pulse::l10n::Get(I::PermanentDelete);
        spec.cancel_text = pulse::l10n::Get(I::Cancel);
        spec.rows = {{L"E:\\fixture\\教学方式.png", true}, {L"E:\\fixture\\archive.rar", true}};
        spec.cancel_is_default = true;
        spec.danger = true;
        spec.fit_to_work_area = true;
        spec.still_valid = [] { return true; };
        const bool accepted = pulse::ui::ShowConfirmDialog(owner, spec, dark, D2D1::ColorF(0x0078D4));
        behavior = !accepted && IsWindowEnabled(owner) && progress_visible && hidden_promptly;
        progress.Destroy();
        DestroyWindow(owner);
    } else if (exit_dialog) {
        const auto spec = pulse::app::ExitConfirmationSpec();
        behavior = spec.cancel_is_default && !pulse::ui::ShowConfirmDialog(nullptr, spec, dark, D2D1::ColorF(0x0078D4));
    } else {
        const auto result = pulse::ui::ShowFileConflictDialog(nullptr, info, dark, D2D1::ColorF(0x0078D4));
        const auto spec = pulse::ui::BuildLinkConfirmationSpec(info);
        const bool paths = spec.rows.size() == 4 &&
            spec.rows[0].text == pulse::l10n::Get(pulse::l10n::StringId::LinkPath) &&
            spec.rows[2].text == pulse::l10n::Get(pulse::l10n::StringId::LinkResolvedPath) &&
            !spec.rows[0].is_path && !spec.rows[0].boxed && !spec.rows[2].is_path && !spec.rows[2].boxed &&
            spec.rows[1].is_path && spec.rows[1].boxed && spec.rows[3].is_path && spec.rows[3].boxed &&
            spec.checkbox_text.empty() && !spec.checkbox_checked && spec.cancel_is_default && !spec.note.empty();
        behavior = paths && (cancel
            ? result.choice == pulse::ops::ConflictChoice::Cancel && !result.apply_to_all
            : result.choice == pulse::ops::ConflictChoice::Continue && result.apply_to_all);
    }
    automate.join();
    const bool ok = captured && behavior && layout_ok && visible_ok;
    std::printf("capture=%d behavior=%d layout=%d visible=%d\n",
        captured.load(), behavior, layout_ok.load(), visible_ok.load());
    std::printf("[%s] %s %s dialog captures and keyboard behavior is safe\n",
        ok ? "PASS" : "FAIL", dark ? "dark" : "light", recycle_dialog ? "recycle" : exit_dialog ? "exit" : "link");
    if (SUCCEEDED(com)) OleUninitialize();
    return ok ? 0 : 1;
}
