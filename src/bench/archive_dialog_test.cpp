#include "../app/archive_dialog.h"
#include "../common/localization.h"
#include "../common/windows_compat.h"
#include <windows.h>
#include <objbase.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <string>
#include <thread>
#include <utility>

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
    if (std::wstring(name) == L"PulseArchiveDialog" &&
        GetWindow(hwnd, GW_OWNER) == search.owner && IsWindowVisible(hwnd)) {
        search.dialog = hwnd;
        return FALSE;
    }
    return TRUE;
}

// Enumerate only this test's UI thread and require this test's owner window.
std::thread Automate(DWORD ui_thread, HWND owner, std::function<void(HWND)> action) {
    return std::thread([ui_thread, owner, action = std::move(action)] {
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

void RunProgressCases(HWND owner, DWORD thread) {
    {
        std::atomic_bool done = false, cancel = false;
        auto helper = Automate(thread, owner, [&](HWND dialog) {
            Check(IsWindow(dialog), "progress dialog appears before completion");
            done.store(true, std::memory_order_release);
        });
        const bool completed = pulse::app::ShowArchiveProgressDialog(owner, true, kAccent, done, cancel);
        helper.join();
        Check(completed && !cancel.load(), "progress: done closes without cancellation");
    }
    for (bool close : {false, true}) {
        std::atomic_bool done = false, cancel = false;
        auto helper = Automate(thread, owner, [&](HWND dialog) {
            if (close) SendMessageW(dialog, WM_CLOSE, 0, 0);
            else Cancel(dialog);
            Check(cancel.load(std::memory_order_acquire), "progress: cancellation flag published");
            std::this_thread::sleep_for(std::chrono::milliseconds(180));
            Check(IsWindow(dialog), "progress: waits for done after cancellation");
            Check(!IsWindowEnabled(GetDlgItem(dialog, IDCANCEL)), "progress: cancel button disabled while cancelling");
            done.store(true, std::memory_order_release);
        });
        const bool completed = pulse::app::ShowArchiveProgressDialog(owner, false, kAccent, done, cancel);
        helper.join();
        Check(!completed && done.load() && cancel.load(), "progress: cancelled completion returns false");
    }
}
} // namespace

int main() {
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
    RunExtractCases(owner, thread);
    RunPasswordCases(owner, thread);
    pulse::l10n::SetLanguage(L"en-US");
    RunProgressCases(owner, thread);
    DestroyWindow(owner);
    if (SUCCEEDED(com)) CoUninitialize();
    std::printf("Archive dialog tests: %d failure(s). Screenshots use actual monitor DPI.\n", failures.load());
    return failures.load() == 0 ? 0 : 1;
}
