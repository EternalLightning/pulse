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
struct BusinessPromptCase {
    HWND owner = nullptr;
    int initial_focus = 0, tabs = 0, final_focus = 0;
    UINT key = VK_RETURN;
    bool hide_cancel = false, has_alternate = false;
    bool seen = false, owner_disabled = false, layout_ok = false, focus_ok = false;
};

BusinessPromptCase* business_prompt_case = nullptr;

void CALLBACK DriveBusinessPrompt(HWND, UINT, UINT_PTR timer, DWORD) {
    HWND dialog = nullptr;
    EnumThreadWindows(GetCurrentThreadId(), [](HWND candidate, LPARAM parameter) -> BOOL {
        wchar_t name[64]{};
        GetClassNameW(candidate, name, ARRAYSIZE(name));
        if (wcscmp(name, L"PulseConfirmWindow") != 0 || !IsWindowVisible(candidate)) return TRUE;
        *reinterpret_cast<HWND*>(parameter) = candidate;
        return FALSE;
    }, reinterpret_cast<LPARAM>(&dialog));
    if (!dialog) return;
    KillTimer(nullptr, timer);
    auto& state = *business_prompt_case;
    state.seen = true;
    state.owner_disabled = !IsWindowEnabled(state.owner) && GetWindow(dialog, GW_OWNER) == state.owner;
    pulse::ui::ConfirmDialogInspection inspection;
    state.layout_ok = SendMessageW(dialog, pulse::ui::kConfirmInspectMessage, 0, reinterpret_cast<LPARAM>(&inspection)) &&
        inspection.primary.right > inspection.primary.left &&
        (inspection.cancel.right > inspection.cancel.left) == !state.hide_cancel &&
        (inspection.alternate.right > inspection.alternate.left) == state.has_alternate;
    state.focus_ok = inspection.focus == state.initial_focus;
    for (int i = 0; i < state.tabs; ++i) SendMessageW(dialog, WM_KEYDOWN, VK_TAB, 0);
    SendMessageW(dialog, pulse::ui::kConfirmInspectMessage, 0, reinterpret_cast<LPARAM>(&inspection));
    state.focus_ok &= inspection.focus == state.final_focus;
    SendMessageW(dialog, WM_KEYDOWN, state.key, 0);
}

int RunBusinessPromptsOnly(bool dark) {
    HWND owner = CreateWindowExW(WS_EX_NOACTIVATE, L"STATIC", L"Business prompt isolated fixture",
        WS_OVERLAPPEDWINDOW, 100, 100, 640, 480, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!owner) return 2;
    int failures = 0;
    auto run = [&](pulse::ui::ConfirmDialogSpec spec, int initial_focus, int tabs, int final_focus,
                   UINT key, bool expected_accepted, bool expected_alternate, const char* label) {
        bool alternate = expected_accepted ? !expected_alternate : expected_alternate;
        spec.alternate_selected = &alternate;
        const auto deadline = GetTickCount64() + 10000;
        spec.still_valid = [deadline] { return GetTickCount64() < deadline; };
        BusinessPromptCase state;
        state.owner = owner; state.initial_focus = initial_focus; state.tabs = tabs; state.final_focus = final_focus;
        state.key = key; state.hide_cancel = spec.hide_cancel; state.has_alternate = !spec.alternate_text.empty();
        business_prompt_case = &state;
        const UINT_PTR timer = SetTimer(nullptr, 0, 10, DriveBusinessPrompt);
        const bool accepted = timer && pulse::ui::ShowConfirmDialog(owner, spec, dark, D2D1::ColorF(0x0078D4));
        if (timer) KillTimer(nullptr, timer);
        business_prompt_case = nullptr;
        const bool restored = IsWindowEnabled(owner);
        const bool ok = state.seen && state.owner_disabled && state.layout_ok && state.focus_ok &&
            accepted == expected_accepted && alternate == expected_alternate && restored;
        std::printf("[%s] %s (seen=%d disabled=%d layout=%d focus=%d accepted=%d alternate=%d restored=%d)\n",
            ok ? "PASS" : "FAIL", label, state.seen, state.owner_disabled, state.layout_ok, state.focus_ok,
            accepted, alternate, restored);
        failures += !ok;
    };
    pulse::ui::ConfirmDialogSpec spec;
    spec.title = L"Business prompt fixture";
    spec.message = L"Isolated shared-dialog keyboard and owner behavior.";
    spec.confirm_text = L"OK";
    spec.hide_cancel = true;
    run(spec, 0, 1, 0, VK_RETURN, true, false, "single OK hides Cancel and Tab retains the sole action");
    spec.hide_cancel = false;
    spec.confirm_text = L"Continue";
    spec.cancel_is_default = true;
    run(spec, 1, 0, 1, VK_RETURN, false, true, "default Cancel rejects Enter and preserves the caller's alternate value");
    run(spec, 1, 1, 0, VK_RETURN, true, false, "Tab from default Cancel reaches Continue");
    spec.confirm_text = L"No";
    spec.alternate_text = L"Yes";
    spec.cancel_is_default = false;
    run(spec, 0, 0, 0, VK_RETURN, true, false, "default No accepts without selecting Yes");
    run(spec, 0, 2, 2, VK_RETURN, true, true, "Tab through Cancel reaches Yes and sets the alternate result");
    run(spec, 0, 2, 2, VK_ESCAPE, false, true, "Escape from Yes cancels and preserves the caller's alternate value");
    DestroyWindow(owner);
    std::printf("Business prompts: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}

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
    bool business_prompts_only = false;
    for (int i = 1; i < argc; ++i) {
        if (wcscmp(argv[i], L"--light") == 0) dark = false;
        else if (wcscmp(argv[i], L"--exit") == 0) exit_dialog = true;
        else if (wcscmp(argv[i], L"--recycle") == 0) { recycle_dialog = true; cancel = true; }
        else if (wcscmp(argv[i], L"--english") == 0) english = true;
        else if (wcscmp(argv[i], L"--cancel") == 0) cancel = true;
        else if (wcscmp(argv[i], L"--narrow") == 0) narrow = true;
        else if (wcscmp(argv[i], L"--business-prompts-only") == 0) business_prompts_only = true;
    }
    pulse::l10n::Initialize(GetModuleHandleW(nullptr), english ? L"en-US" : L"zh-CN");
    if (business_prompts_only) {
        const int result = RunBusinessPromptsOnly(dark);
        if (SUCCEEDED(com)) OleUninitialize();
        return result;
    }
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
        const float scale = (inspection.close.right - inspection.close.left) / 46.0f;
        layout_ok = layout_ok && scale > 0 &&
            inspection.primary.right - inspection.primary.left >= 79.9f * scale &&
            inspection.cancel.right - inspection.cancel.left >= 79.9f * scale;
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
