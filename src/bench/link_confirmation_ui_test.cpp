#include "../ui/file_operation_dialog.h"
#include "../app/exit_confirmation.h"
#include "../common/localization.h"
#include "../common/windows_compat.h"
#include <cstdio>
#include <filesystem>
#include <thread>
#include <atomic>

int wmain(int argc, wchar_t** argv) {
    pulse::compat::EnableDpiAwareness();
    const HRESULT com = OleInitialize(nullptr);
    bool dark = true, exit_dialog = false, english = false;
    for (int i = 1; i < argc; ++i) {
        if (wcscmp(argv[i], L"--light") == 0) dark = false;
        else if (wcscmp(argv[i], L"--exit") == 0) exit_dialog = true;
        else if (wcscmp(argv[i], L"--english") == 0) english = true;
    }
    pulse::l10n::Initialize(GetModuleHandleW(nullptr), english ? L"en-US" : L"zh-CN");
    const std::wstring image_name = std::wstring(exit_dialog ? L"exit_confirmation_" : L"link_confirmation_") +
        (dark ? L"dark" : L"light") + (english ? L"_en" : L"") + L".png";
    const auto output = std::filesystem::absolute(std::filesystem::path(L"bench_data") / image_name).wstring();
    std::filesystem::create_directories(std::filesystem::path(output).parent_path());
    std::atomic<bool> captured{false};
    std::thread automate([&] {
        HWND dialog = nullptr;
        const auto deadline = GetTickCount64() + 10000;
        while (!dialog && GetTickCount64() < deadline) {
            dialog = FindWindowW(L"PulseConfirmWindow", nullptr);
            if (!dialog) Sleep(10);
        }
        if (!dialog) return;
        Sleep(250);
        DWORD_PTR result = 0;
        captured = SendMessageTimeoutW(dialog, pulse::ui::kConfirmSnapshotMessage, 0,
            reinterpret_cast<LPARAM>(output.c_str()), SMTO_ABORTIFHUNG, 5000, &result) && result;
        if (exit_dialog) {
            // Escape keeps the task running; the destructive exit is not the default.
            PostMessageW(dialog, WM_KEYDOWN, VK_ESCAPE, 0);
        } else {
            // Default is Cancel. Tab reaches checkbox, Space toggles it, Tab reaches Continue.
            PostMessageW(dialog, WM_KEYDOWN, VK_TAB, 0);
            PostMessageW(dialog, WM_KEYDOWN, VK_SPACE, 0);
            PostMessageW(dialog, WM_KEYDOWN, VK_TAB, 0);
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
        behavior = result.choice == pulse::ops::ConflictChoice::Continue && result.apply_to_all;
    }
    automate.join();
    const bool ok = captured && behavior;
    std::printf("[%s] %s %s dialog captures and keyboard behavior is safe\n",
        ok ? "PASS" : "FAIL", dark ? "dark" : "light", exit_dialog ? "exit" : "link");
    if (SUCCEEDED(com)) OleUninitialize();
    return ok ? 0 : 1;
}
