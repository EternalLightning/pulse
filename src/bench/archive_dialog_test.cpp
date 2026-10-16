#include "../app/archive_dialog.h"
#include "../app/archive_operation_ui.h"
#include "../common/localization.h"
#include "../common/windows_compat.h"
#include <windows.h>
#include <objbase.h>
#include <commctrl.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <string>
#include <thread>
#include <utility>
#include <mutex>

namespace {
constexpr int kDestination = 100, kPassword = 101, kCreateFolder = 103;
constexpr COLORREF kAccent = RGB(80, 180, 220);
std::atomic_int failures = 0;

void Check(bool condition, const char* description) {
    std::printf("[%s] %s\n", condition ? "PASS" : "FAIL", description);
    if (!condition) failures.fetch_add(1);
}

std::wstring Text(HWND control) {
    const int length = GetWindowTextLengthW(control);
    std::wstring value(static_cast<size_t>(length) + 1, L'\0');
    GetWindowTextW(control, value.data(), length + 1);
    value.resize(static_cast<size_t>(length));
    return value;
}

struct Search {
    HWND owner = nullptr;
    HWND dialog = nullptr;
};

BOOL CALLBACK FindDialog(HWND hwnd, LPARAM parameter) {
    auto& search = *reinterpret_cast<Search*>(parameter);
    wchar_t name[64]{};
    GetClassNameW(hwnd, name, ARRAYSIZE(name));
    if ((std::wstring(name) == L"PulseArchiveDialog" || std::wstring(name) == L"PulseFileOperationWindow") &&
        GetWindow(hwnd, GW_OWNER) == search.owner && IsWindowVisible(hwnd)) {
        search.dialog = hwnd;
        return FALSE;
    }
    return TRUE;
}

// Enumerate only this test's UI thread and require this test's owner window.
std::thread Automate(DWORD ui_thread, HWND owner, std::function<void(HWND)> action, std::function<void()> timed_out = {}) {
    return std::thread([ui_thread, owner, action = std::move(action), timed_out = std::move(timed_out)] {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        Search search{owner};
        while (std::chrono::steady_clock::now() < deadline) {
            EnumThreadWindows(ui_thread, FindDialog, reinterpret_cast<LPARAM>(&search));
            if (search.dialog) {
                action(search.dialog);
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        Check(false, "dialog appeared on the test UI thread within timeout");
        if (timed_out) timed_out();
        PostThreadMessageW(ui_thread, WM_QUIT, 1, 0);
    });
}

void Confirm(HWND dialog) { SendMessageW(dialog, WM_COMMAND, IDOK, 0); }
void Cancel(HWND dialog) { SendMessageW(dialog, WM_COMMAND, IDCANCEL, 0); }

bool SaveScreenshot(HWND dialog, const wchar_t* name) {
    RECT bounds{};
    GetWindowRect(dialog, &bounds);
    const int width = bounds.right - bounds.left, height = bounds.bottom - bounds.top;
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    HDC screen = GetDC(dialog), memory = CreateCompatibleDC(screen);
    void* pixels = nullptr;
    HBITMAP bitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    bool saved = false;
    if (bitmap && pixels && memory) {
        HGDIOBJ previous = SelectObject(memory, bitmap);
        RedrawWindow(dialog, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
        // These custom windows paint WM_PAINT, so capture their displayed client pixels.
        if (BitBlt(memory, 0, 0, width, height, screen, 0, 0, SRCCOPY)) {
            std::error_code error;
            const std::filesystem::path folder = L"bench_data/archive_ui";
            std::filesystem::create_directories(folder, error);
            const std::filesystem::path output = folder /
                (std::wstring(name) + L"-" + std::to_wstring(pulse::compat::WindowDpi(dialog)) + L"dpi.bmp");
            if (!error) {
                HANDLE file = CreateFileW(output.c_str(), GENERIC_WRITE, 0, nullptr,
                    CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (file != INVALID_HANDLE_VALUE) {
                    const DWORD pixel_bytes = static_cast<DWORD>(width * height * 4);
                    BITMAPFILEHEADER header{};
                    header.bfType = 0x4D42;
                    header.bfOffBits = sizeof(header) + sizeof(info.bmiHeader);
                    header.bfSize = header.bfOffBits + pixel_bytes;
                    DWORD written = 0;
                    saved = WriteFile(file, &header, sizeof(header), &written, nullptr) &&
                        WriteFile(file, &info.bmiHeader, sizeof(info.bmiHeader), &written, nullptr) &&
                        WriteFile(file, pixels, pixel_bytes, &written, nullptr);
                    CloseHandle(file);
                }
            }
        }
        SelectObject(memory, previous);
    }
    if (bitmap) DeleteObject(bitmap);
    if (memory) DeleteDC(memory);
    if (screen) ReleaseDC(dialog, screen);
    return saved;
}

void RunExtractCases(HWND owner, DWORD thread) {
    for (bool dark : {false, true}) {
        pulse::app::ArchiveExtractOptions options;
        auto helper = Automate(thread, owner, [dark](HWND dialog) {
            Check(Text(GetDlgItem(dialog, kDestination)) == L"C:\\archive-dialog-fixture",
                "all: destination defaults to archive parent");
            Check(GetDlgItem(dialog, kCreateFolder) != nullptr, "all: create-folder control exists");
            Check((GetWindowLongPtrW(GetDlgItem(dialog, kPassword), GWL_STYLE) & ES_PASSWORD) != 0,
                "all: password edit masks text");
            Check(Text(GetDlgItem(dialog, kCreateFolder)).find(L"example") != std::wstring::npos,
                "all: folder label uses archive stem");
            Check(SaveScreenshot(dialog, dark ? L"extract-dark" : L"extract-light"),
                "extraction screenshot saved at actual monitor DPI");
            Confirm(dialog);
        });
        const bool accepted = pulse::app::ShowArchiveExtractDialog(owner, dark, kAccent,
            L"C:\\archive-dialog-fixture\\example.zip", true, options);
        helper.join();
        Check(accepted && options.create_folder && options.destination == L"C:\\archive-dialog-fixture",
            "all: accepting preserves default create-folder option");
    }
    {
        pulse::app::ArchiveExtractOptions options;
        auto helper = Automate(thread, owner, [](HWND dialog) {
            SendMessageW(dialog, WM_COMMAND, MAKEWPARAM(kCreateFolder, BN_CLICKED),
                reinterpret_cast<LPARAM>(GetDlgItem(dialog, kCreateFolder)));
            SetWindowTextW(GetDlgItem(dialog, kPassword), L"fixture-password");
            Confirm(dialog);
        });
        const bool accepted = pulse::app::ShowArchiveExtractDialog(owner, true, kAccent,
            L"C:\\archive-dialog-fixture\\example.zip", true, options);
        helper.join();
        Check(accepted && !options.create_folder && options.password == L"fixture-password",
            "all: toggling checkbox and entering password updates options");
    }
    {
        pulse::app::ArchiveExtractOptions options;
        auto helper = Automate(thread, owner, [](HWND dialog) {
            Check(GetDlgItem(dialog, kCreateFolder) == nullptr, "selected: checkbox is absent");
            Confirm(dialog);
        });
        const bool accepted = pulse::app::ShowArchiveExtractDialog(owner, false, kAccent,
            L"C:\\archive-dialog-fixture\\example.zip", false, options);
        helper.join();
        Check(accepted && !options.create_folder, "selected: extracts directly to destination");
    }
    {
        pulse::app::ArchiveExtractOptions options{L"C:\\original", L"original-password", false};
        auto helper = Automate(thread, owner, [](HWND dialog) {
            SetWindowTextW(GetDlgItem(dialog, kDestination), L"C:\\changed");
            SetWindowTextW(GetDlgItem(dialog, kPassword), L"changed-password");
            SendMessageW(dialog, WM_COMMAND, MAKEWPARAM(kCreateFolder, BN_CLICKED),
                reinterpret_cast<LPARAM>(GetDlgItem(dialog, kCreateFolder)));
            Cancel(dialog);
        });
        const bool accepted = pulse::app::ShowArchiveExtractDialog(owner, false, kAccent,
            L"C:\\archive-dialog-fixture\\example.zip", true, options);
        helper.join();
        Check(!accepted && options.destination == L"C:\\original" &&
            options.password == L"original-password" && !options.create_folder,
            "cancel: options remain unchanged");
    }
}

void RunPasswordCases(HWND owner, DWORD thread) {
    std::wstring password = L"initial";
    auto helper = Automate(thread, owner, [](HWND dialog) {
        Check((GetWindowLongPtrW(GetDlgItem(dialog, kPassword), GWL_STYLE) & ES_PASSWORD) != 0,
            "password dialog: input masks text");
        Check(Text(GetDlgItem(dialog, kPassword)) == L"initial", "password dialog: initial value retained");
        SetWindowTextW(GetDlgItem(dialog, kPassword), L"entered");
        Confirm(dialog);
    });
    const bool accepted = pulse::app::ShowArchivePasswordDialog(owner, true, kAccent, password);
    helper.join();
    Check(accepted && password == L"entered", "password dialog: confirms entered value");
    helper = Automate(thread, owner, [](HWND dialog) {
        SetWindowTextW(GetDlgItem(dialog, kPassword), L"discarded");
        Cancel(dialog);
    });
    const bool cancelled = pulse::app::ShowArchivePasswordDialog(owner, false, kAccent, password);
    helper.join();
    Check(!cancelled && password == L"entered", "password dialog: cancellation retains output");
}

constexpr UINT kActivateTestWindow = WM_APP + 0x390;
LRESULT CALLBACK FocusTestProc(HWND window, UINT message, WPARAM wp, LPARAM lp,
    UINT_PTR, DWORD_PTR data) {
    if (message == kActivateTestWindow) {
        const auto target = reinterpret_cast<HWND>(wp);
        SetForegroundWindow(target);
        SetActiveWindow(target);
        SetFocus(target);
        return GetActiveWindow() == target && GetFocus() == target;
    }
    if (message == WM_ACTIVATE && LOWORD(wp) != WA_INACTIVE && data)
        ++*reinterpret_cast<int*>(data);
    return DefSubclassProc(window, message, wp, lp);
}

void RunFocusCases(HWND owner, DWORD thread) {
    using namespace pulse;
    int other_activations = 0;
    HWND other = CreateWindowExW(0, L"STATIC", L"Unrelated focus test window",
        WS_OVERLAPPEDWINDOW, 140, 140, 700, 600, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    Check(other != nullptr, "create unrelated activation target");
    if (!other) return;
    SetWindowSubclass(owner, FocusTestProc, 1, 0);
    SetWindowSubclass(other, FocusTestProc, 1, reinterpret_cast<DWORD_PTR>(&other_activations));
    ShowWindow(other, SW_SHOWNOACTIVATE);
    ShowWindow(owner, SW_SHOW);
    for (int mode = 0; mode != 6; ++mode) {
        Check(SendMessageW(owner, kActivateTestWindow, reinterpret_cast<WPARAM>(owner), 0) != 0,
            "owner active and focused before modal extraction");
        other_activations = 0;
        std::atomic_bool done = false, cancel = false;
        HWND foreground_before_close = nullptr;
        bool had_foreground = false;
        auto helper = Automate(thread, owner, [&](HWND dialog) {
            Check(SendMessageW(owner, kActivateTestWindow, reinterpret_cast<WPARAM>(dialog), 0) != 0,
                "progress window active and focused before close");
            if (mode == 4)
                Check(SendMessageW(owner, kActivateTestWindow, reinterpret_cast<WPARAM>(other), 0) != 0,
                    "user switches to unrelated window before worker completes");
            foreground_before_close = GetForegroundWindow();
            had_foreground = foreground_before_close == (mode == 4 ? other : dialog);
            if (mode < 3 || mode == 5) {
                if (mode == 0) {
                    RECT bounds{};
                    GetClientRect(dialog, &bounds);
                    const auto dpi = compat::WindowDpi(dialog);
                    const LPARAM point = MAKELPARAM(bounds.right - MulDiv(35, dpi, 96),
                        bounds.bottom - MulDiv(26, dpi, 96));
                    SendMessageW(dialog, WM_LBUTTONDOWN, MK_LBUTTON, point);
                    SendMessageW(dialog, WM_LBUTTONUP, 0, point);
                } else {
                    SendMessageW(dialog, mode == 1 ? WM_KEYDOWN : WM_CLOSE,
                        mode == 1 ? VK_ESCAPE : 0, 0);
                }
                if (mode < 3) {
                    Check(cancel.load(), "button/Escape/close requests cancellation");
                    Check(IsWindow(dialog) && !IsWindowEnabled(owner),
                        "owner remains disabled until cancelled worker terminates");
                }
            }
            done.store(true, std::memory_order_release);
        }, [&] { done.store(true); });
        const bool completed = app::ShowArchiveProgressWindow(owner, false, kAccent, done, cancel, [&] {
            ops::OpStatus status;
            status.active = mode != 5 && !done.load();
            status.phase = mode == 5 ? ops::OpPhase::Failed : ops::OpPhase::Running;
            status.summary = L"Focus regression";
            return status;
        });
        helper.join();
        std::printf("Focus mode %d: owner=%p other=%p active=%p focus=%p foreground=%p\n",
            mode, static_cast<void*>(owner), static_cast<void*>(other),
            static_cast<void*>(GetActiveWindow()), static_cast<void*>(GetFocus()),
            static_cast<void*>(GetForegroundWindow()));
        Check(IsWindowEnabled(owner), "owner re-enabled after extraction window closes");
        Check(completed == (mode >= 3), "focus restoration preserves cancellation result");
        if (mode == 4) {
            Check(GetActiveWindow() == other && GetFocus() == other,
                "background completion preserves user's active unrelated window");
        } else {
            Check(GetActiveWindow() == owner && GetFocus() == owner,
                "closing active modal progress returns activation and keyboard focus to owner");
            Check(other_activations == 0, "unrelated underlying window never activates during modal teardown");
        }
        if (had_foreground) {
            Check(GetForegroundWindow() == (mode == 4 ? other : owner),
                "foreground extraction teardown preserves the expected foreground window");
        } else {
            std::printf("[LIMIT] Windows did not grant the test foreground activation; checking external foreground preservation.\n");
            Check(GetForegroundWindow() == foreground_before_close,
                "background test does not steal foreground from an external application");
        }
    }
    RemoveWindowSubclass(owner, FocusTestProc, 1);
    DestroyWindow(other);
}

void RunProgressCases(HWND owner, DWORD thread, bool = false) {
    using namespace pulse;
    ops::ArchiveProgress progress;
    auto mapped = app::ArchiveOperationStatus(progress, L"C:\\fixtures\\example.zip", L"C:\\output", false);
    Check(mapped.phase == ops::OpPhase::Scanning && mapped.active && mapped.percent < 0 &&
        mapped.source_label == L"C:\\fixtures\\example.zip" && mapped.destination_label == L"C:\\output",
        "progress adapter preserves endpoints and unknown preparation progress");
    progress.phase = ops::ArchivePhase::Extracting;
    progress.percent = 42; progress.total_bytes = 1000; progress.processed_bytes = 420;
    progress.total_items = 10; progress.bytes_per_second = 128; progress.eta_seconds = 5;
    mapped = app::ArchiveOperationStatus(progress, L"archive.zip", L"output", false);
    Check(mapped.phase == ops::OpPhase::Running && mapped.active && mapped.percent == 42 &&
        mapped.total_bytes == 1000 && mapped.transferred_bytes == 420 && mapped.total_items == 10 &&
        mapped.completed_items == 0 && mapped.current_item.empty() && mapped.bytes_per_second == 128 && mapped.eta_seconds == 5,
        "progress adapter carries byte estimates without inventing completed items or current filenames");
    mapped = app::ArchiveOperationStatus(progress, L"archive.zip", L"output", true);
    Check(mapped.phase == ops::OpPhase::Cancelling && mapped.bytes_per_second == 0 && mapped.eta_seconds == 0,
        "cancelling adapter suppresses speed and ETA");
    for (bool dark : {false, true}) {
        std::atomic_bool done = false, cancel = false;
        ops::ArchiveProgress status;
        std::mutex status_mutex;
        auto helper = Automate(thread, owner, [&](HWND dialog) {
            wchar_t class_name[64]{};
            GetClassNameW(dialog, class_name, ARRAYSIZE(class_name));
            Check(std::wstring(class_name) == L"PulseFileOperationWindow", "extraction uses the real copy/move window class");
            Check(!IsWindowEnabled(owner), "shared extraction progress retains modal owner handling");
            {
                std::lock_guard lock(status_mutex);
                status.phase = ops::ArchivePhase::Extracting;
                status.percent = 42;
                status.total_bytes = 10ull * 1024 * 1024 * 1024;
                status.processed_bytes = status.total_bytes * 42 / 100;
                status.bytes_per_second = 123.4 * 1024 * 1024;
                status.eta_seconds = 49;
            }
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (Text(dialog) != l10n::Get(l10n::StringId::ArchiveExtracting) && std::chrono::steady_clock::now() < deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            Check(Text(dialog) == l10n::Get(l10n::StringId::ArchiveExtracting), "worker status reaches the shared window");
            {
                std::lock_guard lock(status_mutex);
                status.phase = ops::ArchivePhase::Completed;
                status.percent = 100;
                status.processed_bytes = status.total_bytes;
            }
            done.store(true, std::memory_order_release);
        }, [&] { done.store(true); });
        const bool completed = app::ShowArchiveProgressWindow(owner, dark, kAccent, done, cancel, [&] {
            std::lock_guard lock(status_mutex);
            return app::ArchiveOperationStatus(status, L"C:\\fixtures\\example.zip", L"C:\\output", cancel.load());
        });
        helper.join();
        Check(completed && !cancel.load() && IsWindowEnabled(owner), "shared window finishes and restores its owner");
    }
    for (bool close : {false, true}) {
        std::atomic_bool done = false, cancel = false;
        auto helper = Automate(thread, owner, [&](HWND dialog) {
            SendMessageW(dialog, close ? WM_CLOSE : WM_KEYDOWN, close ? 0 : VK_ESCAPE, 0);
            Check(cancel.load(std::memory_order_acquire), "shared close/Escape requests extraction cancellation");
            std::this_thread::sleep_for(std::chrono::milliseconds(180));
            Check(IsWindow(dialog) && !IsWindowEnabled(owner), "shared window waits for worker termination after cancellation");
            done.store(true, std::memory_order_release);
        }, [&] { done.store(true); });
        const bool completed = app::ShowArchiveProgressWindow(owner, false, kAccent, done, cancel,
            [&] { return app::ArchiveOperationStatus(progress, L"archive.zip", L"output", cancel.load()); });
        helper.join();
        Check(!completed && done.load() && cancel.load() && IsWindowEnabled(owner), "cancelled extraction releases the shared window safely");
    }
    {
        std::atomic_bool done = false, cancel = false;
        auto helper = Automate(thread, owner, [&](HWND dialog) {
            PostThreadMessageW(thread, WM_QUIT, 17, 0);
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (!cancel.load() && std::chrono::steady_clock::now() < deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            Check(cancel.load() && IsWindow(dialog), "thread quit cancels extraction while preserving worker lifetime");
            done.store(true, std::memory_order_release);
        }, [&] { done.store(true); });
        const bool completed = app::ShowArchiveProgressWindow(owner, false, kAccent, done, cancel,
            [&] { return app::ArchiveOperationStatus(progress, L"archive.zip", L"output", cancel.load()); });
        helper.join();
        MSG quit{};
        Check(!completed && PeekMessageW(&quit, nullptr, WM_QUIT, WM_QUIT, PM_REMOVE) && quit.wParam == 17 &&
            IsWindowEnabled(owner), "shared progress preserves WM_QUIT and restores the owner after worker termination");
    }
}
void RunFailureCases(HWND owner, DWORD thread) {
    using namespace pulse;
    ops::ArchiveOperationResult result;
    result.exit_code = 2;
    result.password_state = ops::ArchivePasswordState::Required;
    result.error = L"ERROR: Data Error in encrypted file. Wrong password? : 童欣文.pptx";
    const auto failure = app::ArchiveFailureStatus(result, L"C:\\fixtures\\example.zip", L"C:\\output");
    Check(failure.phase == ops::OpPhase::Failed && !failure.active && failure.failure_hr == S_OK &&
        failure.last_error == result.error && failure.source_label == L"C:\\fixtures\\example.zip" &&
        failure.destination_label == L"C:\\output" && failure.bytes_per_second == 0 && failure.eta_seconds == 0,
        "archive failure preserves diagnostics and endpoints without treating 7zip exit 2 as a Windows error");
    result.error.clear();
    Check(app::ArchiveFailureStatus(result, L"archive.zip", L"output").last_error ==
        l10n::Get(l10n::StringId::ArchiveExtractFailed), "empty extraction failure has a localized explanation");
    for (bool dark : {false, true}) {
        for (bool immediate : {false, true}) {
            std::atomic_bool done = immediate, cancel = false;
            auto helper = Automate(thread, owner, [&](HWND dialog) {
                wchar_t class_name[64]{};
                GetClassNameW(dialog, class_name, ARRAYSIZE(class_name));
                Check(std::wstring(class_name) == L"PulseFileOperationWindow" && !IsWindowEnabled(owner),
                    "extraction failure reuses the existing operation window and modal owner");
                done.store(true, std::memory_order_release);
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
                while (Text(dialog) != failure.summary && std::chrono::steady_clock::now() < deadline)
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                Check(Text(dialog) == failure.summary && IsWindowVisible(dialog),
                    "failed extraction stays visible after the worker completes");
                SendMessageW(dialog, immediate ? WM_KEYDOWN : WM_CLOSE, immediate ? VK_ESCAPE : 0, 0);
            }, [&] { done.store(true); });
            const bool shown = app::ShowArchiveProgressWindow(owner, dark, kAccent, done, cancel, [&] {
                return done.load(std::memory_order_acquire) ? failure
                    : app::ArchiveOperationStatus({}, L"archive.zip", L"output", false);
            });
            helper.join();
            Check(shown && !cancel.load() && IsWindowEnabled(owner),
                "closing a failure restores its owner without issuing another cancellation");
        }
    }
}
} // namespace

int main(int argc, char** argv) {
    pulse::compat::EnableDpiAwareness();
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    pulse::l10n::Initialize(GetModuleHandleW(nullptr), L"zh-CN");
    HWND owner = CreateWindowExW(0, L"STATIC", L"Pulse archive dialog test owner",
        WS_OVERLAPPEDWINDOW, 100, 100, 700, 600, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!owner) {
        Check(false, "create isolated test owner");
        if (SUCCEEDED(com)) CoUninitialize();
        return 1;
    }
    const DWORD thread = GetCurrentThreadId();
    const bool failure_only = argc == 2 && std::string_view(argv[1]) == "--failure-only";
    if (argc == 2 && std::string_view(argv[1]) == "--focus-only") {
        RunFocusCases(owner, thread);
        DestroyWindow(owner);
        if (SUCCEEDED(com)) CoUninitialize();
        std::printf("Archive focus tests: %d failure(s).\n", failures.load());
        return failures.load() == 0 ? 0 : 1;
    }
    if (failure_only) {
        RunFailureCases(owner, thread);
        pulse::l10n::SetLanguage(L"en-US");
        RunFailureCases(owner, thread);
        DestroyWindow(owner);
        if (SUCCEEDED(com)) CoUninitialize();
        std::printf("Archive failure dialog tests: %d failure(s).\n", failures.load());
        return failures.load() == 0 ? 0 : 1;
    }
    const bool progress_only = argc == 2 && std::string_view(argv[1]) == "--progress-only";
    if (!progress_only) {
        RunExtractCases(owner, thread);
        RunPasswordCases(owner, thread);
    }
    RunProgressCases(owner, thread);
    pulse::l10n::SetLanguage(L"en-US");
    RunProgressCases(owner, thread, true);
    DestroyWindow(owner);
    if (SUCCEEDED(com)) CoUninitialize();
    std::printf("Archive dialog tests: %d failure(s). Screenshots use actual monitor DPI.\n", failures.load());
    return failures.load() == 0 ? 0 : 1;
}
