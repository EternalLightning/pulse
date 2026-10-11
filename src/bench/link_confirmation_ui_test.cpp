#include "../ui/file_operation_dialog.h"
#include "../app/exit_confirmation.h"
#include "../common/localization.h"
#include "../common/windows_compat.h"
#include <cstdio>
#include <filesystem>
#include <thread>
#include <atomic>
#include <algorithm>

int wmain(int argc, wchar_t** argv) {
    pulse::compat::EnableDpiAwareness();
    const HRESULT com = OleInitialize(nullptr);
    bool dark = true, exit_dialog = false, english = false, cancel = false, narrow = false;
    for (int i = 1; i < argc; ++i) {
        if (wcscmp(argv[i], L"--light") == 0) dark = false;
        else if (wcscmp(argv[i], L"--exit") == 0) exit_dialog = true;
        else if (wcscmp(argv[i], L"--english") == 0) english = true;
        else if (wcscmp(argv[i], L"--cancel") == 0) cancel = true;
        else if (wcscmp(argv[i], L"--narrow") == 0) narrow = true;
    }
    pulse::l10n::Initialize(GetModuleHandleW(nullptr), english ? L"en-US" : L"zh-CN");
    const std::wstring image_name = std::wstring(exit_dialog ? L"exit_confirmation_" : L"link_confirmation_") +
        (dark ? L"dark" : L"light") + (english ? L"_en" : L"") +
        (narrow ? L"_144_narrow" : L"") + (cancel ? L"_cancel" : L"") + L".png";
    const auto output = std::filesystem::absolute(std::filesystem::path(L"bench_data") / image_name).wstring();
    std::filesystem::create_directories(std::filesystem::path(output).parent_path());
    std::atomic<bool> captured{false}, layout_ok{false};
    std::thread automate([&] {
        HWND dialog = nullptr;
        const auto deadline = GetTickCount64() + 10000;
        while (!dialog && GetTickCount64() < deadline) {
            dialog = FindWindowW(L"PulseConfirmWindow", nullptr);
            if (!dialog) Sleep(10);
        }
        if (!dialog) return;
        Sleep(250);
        if (narrow) {
            RECT rect{}; GetWindowRect(dialog, &rect);
            SendMessageW(dialog, WM_DPICHANGED, MAKEWPARAM(144, 144), reinterpret_cast<LPARAM>(&rect));
            SetWindowPos(dialog, nullptr, 0, 0, 420, 570, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
        pulse::ui::ConfirmDialogInspection inspection;
        RECT client{}; GetClientRect(dialog, &client);
        layout_ok = SendMessageW(dialog, pulse::ui::kConfirmInspectMessage, 0, reinterpret_cast<LPARAM>(&inspection)) &&
            (exit_dialog || inspection.row_count == 4) &&
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
    });
    pulse::ops::ConflictItemInfo info;
    info.link_confirmation = true;
    info.link_impact.kind = pulse::ops::DestinationLinkKind::Junction;
    info.link_impact.path = L"C:\\备份\\项目\\" + std::wstring(140, L'长') + L"链接目录";
    info.link_impact.resolved_path = L"\\\\server\\share\\重要资料\\" + std::wstring(160, L'文') + L"项目";
    bool behavior = false;
    if (exit_dialog) {
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
    const bool ok = captured && behavior && layout_ok;
    std::printf("[%s] %s %s dialog captures and keyboard behavior is safe\n",
        ok ? "PASS" : "FAIL", dark ? "dark" : "light", exit_dialog ? "exit" : "link");
    if (SUCCEEDED(com)) OleUninitialize();
    return ok ? 0 : 1;
}
