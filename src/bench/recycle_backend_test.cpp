#include "../ops/ops_manager.h"
#include "../common/localization.h"
#include <filesystem>
#include <iostream>
#include <thread>
#include <atomic>
#include <commctrl.h>

int main(int argc, char** argv) {
    using namespace pulse;
    l10n::Initialize(GetModuleHandleW(nullptr), L"en-US");
    wchar_t module[32768]{};
    if (!GetModuleFileNameW(nullptr, module, ARRAYSIZE(module))) return 2;
    const auto root = std::filesystem::path(module).parent_path().parent_path() /
        L"bench_data/review-changes/recycle-feedback-fixture";
    const bool accept_unc = argc == 2 && std::string_view(argv[1]) == "--unc-warning-accept";
    const bool cancel_unc = argc == 2 && std::string_view(argv[1]) == "--unc-warning-cancel";
    const auto file = (root / (accept_unc ? L"unc-accepted.txt" : L"recycle-me.txt")).lexically_normal().wstring();
    if (GetFileAttributesW(file.c_str()) == INVALID_FILE_ATTRIBUTES) {
        std::cout << "[FAIL] dedicated recycle input missing; no operation submitted\n"; return 2;
    }
    const HWND owner = CreateWindowExW(0, L"STATIC", L"Pulse recycle fixture", WS_OVERLAPPEDWINDOW,
        0, 0, 600, 300, nullptr, nullptr, nullptr, nullptr);
    ops::OpsManager manager;
    manager.SetUiWindow(owner); manager.Start([] {});
    int failed = 0;
    auto check = [&](bool ok, const char* text) {
        std::cout << (ok ? "[PASS] " : "[FAIL] ") << text << std::endl;
        failed += !ok;
    };
    auto wait = [&](uint64_t previous) {
        const auto deadline = GetTickCount64() + 30000;
        while (manager.Status().completed_ops == previous && GetTickCount64() < deadline) {
            MsgWaitForMultipleObjects(0, nullptr, FALSE, 10, QS_ALLINPUT);
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
        }
        return manager.Status().completed_ops == previous + 1;
    };
    if (accept_unc || cancel_unc) {
        // Existing localhost administrative share only, dedicated test root only.
        // No new shares, credentials or bin configuration are created.
        const auto unc_file = L"\\\\localhost\\" + file.substr(0, 1) + L"$" + file.substr(2, file.size() - 2);
        if (GetFileAttributesW(unc_file.c_str()) == INVALID_FILE_ATTRIBUTES) {
            std::cout << "[FAIL] localhost UNC fixture unavailable; no deletion requested\n";
            manager.Stop(); DestroyWindow(owner); return 2;
        }
        std::atomic<bool> stop{false}, warned{false};
        std::thread warning_driver([&] {
            while (!stop.load()) {
                struct Context { const std::wstring& file; std::atomic<bool>& warned; bool accept; } context{file, warned, accept_unc};
                EnumWindows([](HWND window, LPARAM param) -> BOOL {
                    auto& value = *reinterpret_cast<Context*>(param);
                    if (!IsWindowVisible(window)) return TRUE;
                    DWORD pid = 0; GetWindowThreadProcessId(window, &pid);
                    if (pid == GetCurrentProcessId()) return TRUE;
                    std::wstring text;
                    EnumChildWindows(window, [](HWND child, LPARAM result) -> BOOL {
                        wchar_t content[2048]{}; GetWindowTextW(child, content, ARRAYSIZE(content));
                        *reinterpret_cast<std::wstring*>(result) += content;
                        return TRUE;
                    }, reinterpret_cast<LPARAM>(&text));
                    const std::wstring leaf = std::filesystem::path(value.file).filename().wstring();
                    if (text.find(leaf) != std::wstring::npos &&
                        (text.find(L"永久") != std::wstring::npos || text.find(L"permanent") != std::wstring::npos)) {
                        value.warned.store(true);
                        if (value.accept) {
                            struct Button { HWND yes = nullptr; } button;
                            EnumChildWindows(window, [](HWND child, LPARAM param) -> BOOL {
                                wchar_t caption[128]{}; GetWindowTextW(child, caption, ARRAYSIZE(caption));
                                const std::wstring label(caption);
                                if (label.find(L"是") != std::wstring::npos || label.find(L"Yes") != std::wstring::npos)
                                    reinterpret_cast<Button*>(param)->yes = child;
                                return TRUE;
                            }, reinterpret_cast<LPARAM>(&button));
                            if (button.yes) PostMessageW(button.yes, BM_CLICK, 0, 0);
                        } else {
                            PostMessageW(window, WM_KEYDOWN, VK_ESCAPE, 0);
                            PostMessageW(window, WM_CLOSE, 0, 0);
                        }
                    }
                    return TRUE;
                }, reinterpret_cast<LPARAM>(&context));
                Sleep(30);
            }
        });
        ops::OpRequest unc_request; unc_request.type = ops::OpType::RecycleDelete; unc_request.sources = {unc_file};
        const auto before = manager.Status().completed_ops;
        manager.Submit(std::move(unc_request));
        const bool finished = wait(before);
        if (!finished) manager.CancelCurrent();
        stop.store(true); warning_driver.join();
        check(warned.load(), "UNC non-recyclable item shows a Windows permanent-delete warning");
        const auto results = manager.DrainCompletions();
        if (accept_unc) {
            check(finished && GetFileAttributesW(file.c_str()) == INVALID_FILE_ATTRIBUTES &&
                manager.Status().phase == ops::OpPhase::Completed && results.size() == 1 &&
                results[0].type == ops::OpType::RealDelete && results[0].sources == std::vector<std::wstring>{unc_file} &&
                !manager.CanUndo(), "explicitly accepting the Windows warning records permanent deletion, never false recycle Undo");
        } else check(finished && GetFileAttributesW(file.c_str()) != INVALID_FILE_ATTRIBUTES && results.empty(),
            "declining the Windows permanent warning preserves the original dedicated file");
        manager.Stop(); DestroyWindow(owner); return failed ? 1 : 0;
    }
    ops::OpRequest request;
    request.type = ops::OpType::RecycleDelete; request.sources = {file};
    auto previous = manager.Status().completed_ops;
    manager.Submit(request);
    const bool finished = wait(previous);
    const auto completed = manager.DrainCompletions();
    const bool recycled = finished && manager.Status().phase == ops::OpPhase::Completed && completed.size() == 1 &&
        completed[0].type == ops::OpType::RecycleDelete && completed[0].sources == std::vector<std::wstring>{file};
    check(recycled && !manager.PendingDeleteConfirmation() && manager.CanUndo(),
        "ordinary Delete really recycles and offers Undo without Pulse permanent confirmation");
    if (recycled) {
        previous = manager.Status().completed_ops;
        manager.Undo();
        check(wait(previous) && GetFileAttributesW(file.c_str()) != INVALID_FILE_ATTRIBUTES &&
            manager.Status().phase == ops::OpPhase::Completed && !manager.CanUndo(),
            "Undo restores exactly the dedicated recycled file");
    } else {
        std::wcerr << L"[INFO] " << manager.Status().last_error << std::endl;
        manager.CancelCurrent();
    }
    if (recycled && failed == 0) {
        // Deliberately retain an older generation in the bin while recycling
        // a newer file at the same path; Undo must use its exact $R identity.
        previous = manager.Status().completed_ops;
        manager.Submit(request);
        const bool old_done = wait(previous);
        const auto old_undo = manager.UndoToJson();
        check(old_done && manager.Status().phase == ops::OpPhase::Completed, "first same-path generation is recycled");
        manager.DrainCompletions();
        const std::string newer = "new generation must be restored, never the older bin item";
        const HANDLE output = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        DWORD written = 0;
        const bool created = output != INVALID_HANDLE_VALUE && WriteFile(output, newer.data(), static_cast<DWORD>(newer.size()), &written, nullptr) && written == newer.size();
        if (output != INVALID_HANDLE_VALUE) CloseHandle(output);
        check(created, "create dedicated newer generation at the same original path");
        if (created) {
            previous = manager.Status().completed_ops; manager.Submit(request);
            const bool newer_done = wait(previous);
            check(newer_done && manager.Status().phase == ops::OpPhase::Completed, "newer same-path generation is recycled");
            manager.DrainCompletions();
            previous = manager.Status().completed_ops; manager.Undo();
            const bool restored = wait(previous);
            const HANDLE input = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            char content[128]{}; DWORD read = 0;
            const bool have_content = input != INVALID_HANDLE_VALUE && ReadFile(input, content, sizeof(content), &read, nullptr);
            if (input != INVALID_HANDLE_VALUE) CloseHandle(input);
            check(restored && have_content && std::string(content, read) == newer,
                "Undo selects the exact latest recycled identity despite older same-path bin content");
            manager.DrainCompletions();
            // Keep the newer restored fixture, remove it from this path by a
            // non-destructive rename, then restore the older generation too.
            const auto saved = (root / (L"newer-restored-" + std::to_wstring(GetCurrentProcessId()) + L".txt")).wstring();
            check(MoveFileW(file.c_str(), saved.c_str()) != FALSE, "retain newer restored fixture without deletion");
            check(manager.UndoFromJson(old_undo), "reimport preserved first generation exact Undo");
            previous = manager.Status().completed_ops; manager.Undo();
            check(wait(previous) && GetFileAttributesW(file.c_str()) != INVALID_FILE_ATTRIBUTES,
                "restore older dedicated generation as well; no user bin item is selected");
        }
    }
    manager.Stop(); DestroyWindow(owner);
    return failed ? 1 : 0;
}
