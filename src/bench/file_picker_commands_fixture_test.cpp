// SPDX-License-Identifier: Apache-2.0
#include "../common/windows_compat.h"
#include "../ops/clipboard.h"
#include "../ops/picker_file_operations.h"
#include "../ui/file_picker_commands.h"
#include "../ui/file_picker_dialog.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <ole2.h>
#include <shellapi.h>
#include <shlobj.h>
#include <thread>

namespace {
template <typename Predicate>
bool WaitFor(Predicate check, DWORD timeout = 7000) {
    const auto deadline = GetTickCount64() + timeout;
    do {
        if (check()) return true;
        Sleep(20);
    } while (GetTickCount64() < deadline);
    return false;
}

HWND FindOnThread(HWND picker, const wchar_t* window_class) {
    struct Query { const wchar_t* window_class; HWND found = nullptr; } query{window_class};
    EnumThreadWindows(GetWindowThreadProcessId(picker, nullptr), [](HWND window, LPARAM value) -> BOOL {
        auto& query = *reinterpret_cast<Query*>(value);
        wchar_t name[80]{};
        if (IsWindowVisible(window) && GetClassNameW(window, name, ARRAYSIZE(name)) &&
            wcscmp(name, query.window_class) == 0) {
            query.found = window;
            return FALSE;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&query));
    return query.found;
}

std::wstring EditText(HWND edit) {
    wchar_t value[4096]{};
    return std::wstring(value, GetWindowTextW(edit, value, ARRAYSIZE(value)));
}

bool WaitWithMessages(const auto& check) {
    return WaitFor([&] {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&message);
        return check();
    });
}

struct DeferredFile {
    std::wstring path;
    pulse::ops::PickerFileOperation* cancel = nullptr;
    pulse::ops::PickerFileOperations* session = nullptr;
    bool rendered = false;
    bool cancelled_before_mutation = false;

    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
        if (message == WM_RENDERFORMAT && wparam == CF_HDROP) {
            auto& file = *reinterpret_cast<DeferredFile*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
            file.rendered = true;
            if (file.cancel) file.cancelled_before_mutation = file.cancel->Cancel(*file.session);
            const auto chars = file.path.size() + 2;
            if (const auto data = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, sizeof(DROPFILES) + chars * sizeof(wchar_t))) {
                if (auto* drop = static_cast<DROPFILES*>(GlobalLock(data))) {
                    drop->pFiles = sizeof(DROPFILES);
                    drop->fWide = TRUE;
                    std::memcpy(drop + 1, file.path.data(), file.path.size() * sizeof(wchar_t));
                    GlobalUnlock(data);
                    if (SetClipboardData(CF_HDROP, data)) return 0;
                }
                GlobalFree(data);
            }
        }
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }

    bool Publish(HWND owner) {
        rendered = false;
        if (!OpenClipboard(owner)) return false;
        const bool emptied = EmptyClipboard() != FALSE;
        if (emptied) SetClipboardData(CF_HDROP, nullptr);
        CloseClipboard();
        return emptied && IsClipboardFormatAvailable(CF_HDROP);
    }
};

int ClipboardCases() {
    using namespace pulse::ops;
    namespace fs = std::filesystem;
    const auto previous_station = GetProcessWindowStation();
    const auto previous_desktop = GetThreadDesktop(GetCurrentThreadId());
    const auto station = CreateWindowStationW(nullptr, CWF_CREATE_ONLY, WINSTA_ALL_ACCESS, nullptr);
    if (!station || !SetProcessWindowStation(station)) {
        if (station) CloseWindowStation(station);
        std::puts("[SKIP] no fresh private clipboard station");
        return 77;
    }
    const auto desktop = CreateDesktopW(L"PickerClipboardFixture", nullptr, nullptr, 0, GENERIC_ALL, nullptr);
    const bool switched = desktop && SetThreadDesktop(desktop);
    const auto root = fs::absolute(L"bench_data") / (L"picker-clipboard-" + std::to_wstring(GetCurrentProcessId()));
    std::error_code error;
    const bool created = fs::create_directories(root / L"target", error);
    const auto source = root / L"sample.txt";
    const auto destination = root / L"target" / source.filename();
    int failures = 0;
    const auto check = [&](bool ok, const char* label) {
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
        failures += !ok;
    };
    if (created && switched) {
        std::ofstream(source) << "original";
        WNDCLASSW wc{};
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"PulseDeferredClipboardFixture";
        wc.lpfnWndProc = DeferredFile::WindowProc;
        RegisterClassW(&wc);
        const auto owner = CreateWindowExW(0, wc.lpszClassName, L"", WS_POPUP,
            0, 0, 1, 1, nullptr, nullptr, wc.hInstance, nullptr);
        if (owner) {
            DeferredFile file{source.wstring()};
            SetWindowLongPtrW(owner, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&file));
            const auto session = CreatePickerFileOperations();
            const auto start = [&](const std::shared_ptr<PickerFileOperation>& operation) {
                return SubmitPickerFileOperation(session, operation,
                    PickerClipboardPaste{destination.parent_path().wstring(), GetClipboardSequenceNumber()}, owner, true);
            };
            const auto ready = [](const auto& operation) {
                return WaitWithMessages([&] { return operation->ready.load(std::memory_order_acquire); });
            };
            auto operation = std::make_shared<PickerFileOperation>();
            const bool published = file.Publish(owner);
            const auto delayed_sequence = GetClipboardSequenceNumber();
            const bool copied = published && start(operation) && ready(operation) &&
                operation->result == S_OK && fs::exists(destination);
            const auto rendered_sequence = GetClipboardSequenceNumber();
            const bool undo = session->Manager().CanUndo();
            std::printf("[INFO] delayed sequence %lu -> %lu, rendered %d, undo %d\n",
                delayed_sequence, rendered_sequence, static_cast<int>(file.rendered), static_cast<int>(undo));
            check(copied && file.rendered && fs::exists(source) && undo,
                "first paste renders delayed CF_HDROP and copies through OpsManager with undo");

            if (copied) {
                std::ofstream(destination, std::ios::trunc) << "existing";
                operation = std::make_shared<PickerFileOperation>();
                const bool waiting = start(operation) &&
                    WaitWithMessages([&] { return session->Manager().PendingConflict().has_value(); });
                const bool active = waiting && !operation->Cancel(*session);
                const bool cancelled = active && ready(operation) &&
                    operation->result == HRESULT_FROM_WIN32(ERROR_CANCELLED);
                std::string source_text, destination_text;
                std::ifstream(source) >> source_text;
                std::ifstream(destination) >> destination_text;
                check(cancelled && source_text == "original" && destination_text == "existing",
                    "cancel stops an active paste waiting for a conflict and preserves both files");
                fs::remove(destination, error);
            }

            operation = std::make_shared<PickerFileOperation>();
            file.cancel = operation.get();
            file.session = session.get();
            check(file.Publish(owner) && start(operation) && ready(operation) &&
                operation->result == HRESULT_FROM_WIN32(ERROR_CANCELLED) && file.cancelled_before_mutation &&
                !fs::exists(destination), "cancel during first clipboard rendering prevents submission");
            file.cancel = nullptr;

            SetActiveWindow(owner);
            operation = std::make_shared<PickerFileOperation>();
            check(GetActiveWindow() == owner && WriteClipboard({source.wstring()}, true) &&
                start(operation) && ready(operation) && operation->result == S_OK &&
                !fs::exists(source) && fs::exists(destination) && !IsClipboardFormatAvailable(CF_HDROP),
                "complete cut moves the file and clears only its clipboard");

            DestroyWindow(owner);
        } else check(false, "create a delayed clipboard provider");
    } else check(false, "create private desktop and paste fixtures");
    if (created) fs::remove_all(root, error);
    check(SetProcessWindowStation(previous_station) && SetThreadDesktop(previous_desktop) && !error,
        "restore the original station and clean up clipboard fixtures");
    if (desktop) CloseDesktop(desktop);
    CloseWindowStation(station);
    return failures ? 1 : 0;
}

template <typename Check>
void SaveRenameCase(const std::filesystem::path& root, Check check) {
    namespace fs = std::filesystem;
    const auto folder = root / L"save";
    fs::create_directory(folder);
    const auto old_path = folder / L"Old.txt";
    const auto new_path = folder / L"New.txt";
    std::ofstream(old_path) << "input";
    std::thread driver;
    std::atomic_bool finish{false};
    pulse::ui::FilePickerSpec spec;
    spec.mode = pulse::ui::PickerMode::Save;
    spec.initial_path = folder.wstring() + L"\\";
    spec.software_device = true;
    spec.should_close = [&] { return finish.load(); };
    spec.on_ready = [&](HWND picker) {
        const auto dpi = static_cast<int>(pulse::compat::WindowDpi(picker));
        const auto px = [dpi](int dip) { return MulDiv(dip, dpi, 96); };
        SetWindowPos(picker, nullptr, 0, 0, px(530), px(420), SWP_NOZORDER | SWP_NOMOVE | SWP_NOACTIVATE);
        driver = std::thread([&, picker, px] {
            const auto click = [&](int x, int y) {
                const auto point = MAKELPARAM(px(x), px(y));
                PostMessageW(picker, WM_LBUTTONDOWN, MK_LBUTTON, point);
                PostMessageW(picker, WM_LBUTTONUP, 0, point);
            };
            const HWND first = FindWindowExW(picker, nullptr, L"Edit", nullptr);
            const HWND second = FindWindowExW(picker, first, L"Edit", nullptr);
            const bool first_address = EditText(first).starts_with(folder.wstring());
            const HWND address = first_address ? first : second;
            const HWND filename = first_address ? second : first;
            const bool loaded = WaitFor([&] { return EditText(address) == folder.wstring(); });
            HWND menu = nullptr;
            if (loaded) click(224, 116);
            const bool text_menu = loaded && WaitFor([&] { return (menu = FindOnThread(picker, L"PulseFluentMenu")) != nullptr; });
            if (text_menu) {
                PostMessageW(menu, WM_KEYDOWN, VK_DOWN, 1);
                PostMessageW(menu, WM_KEYDOWN, VK_DOWN, 1);
                PostMessageW(menu, WM_KEYDOWN, VK_RETURN, 1);
            }
            HWND edit = nullptr;
            const auto find_prompt = [&] {
                const HWND dialog = FindOnThread(picker, L"PulsePickerItemName");
                return dialog && (edit = FindWindowExW(dialog, nullptr, L"Edit", nullptr)) != nullptr;
            };
            const bool name_prompt = text_menu && WaitFor(find_prompt);
            check(name_prompt && SendMessageW(edit, EM_GETLIMITTEXT, 0, 0) == 251,
                "new text document reserves four characters for its default extension");
            if (name_prompt) {
                PostMessageW(edit, WM_KEYDOWN, VK_ESCAPE, 1);
                WaitFor([&] { return !FindOnThread(picker, L"PulsePickerItemName"); });
            }
            if (loaded) click(236, 182);
            const bool selected = WaitFor([&] { return EditText(filename) == L"Old.txt"; });
            if (!selected) {
                check(false, "select the original file in the Save picker");
                finish.store(true);
                return;
            }
            click(224, 116);
            const bool opened = WaitFor([&] { return (menu = FindOnThread(picker, L"PulseFluentMenu")) != nullptr; });
            if (opened) PostMessageW(menu, WM_KEYDOWN, VK_ESCAPE, 1);
            const bool dismissed = opened && WaitFor([&] { return !FindOnThread(picker, L"PulseFluentMenu"); });
            if (dismissed) PostMessageW(picker, WM_KEYDOWN, VK_F2, 1);
            const bool prompted = dismissed && WaitFor(find_prompt);
            check(prompted, "F2 works directly after using the toolbar");
            if (prompted) {
                SendMessageW(edit, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(L"New.txt"));
                PostMessageW(edit, WM_KEYDOWN, VK_RETURN, 1);
            }
            const bool updated = prompted && WaitFor([&] {
                return fs::exists(new_path) && !fs::exists(old_path) && EditText(filename) == L"New.txt";
            });
            check(updated, "the Save filename follows the renamed selection");
            if (updated) {
                WaitFor([&] {
                    if (!IsWindow(picker)) return true;
                    PostMessageW(filename, WM_KEYDOWN, VK_RETURN, 1);
                    return false;
                });
            }
            finish.store(true);
        });
    };
    pulse::ui::FilePickerResult result;
    const bool submitted = pulse::ui::ShowFilePicker(nullptr, spec, false, {0.0f, 0.47f, 0.83f, 1.0f}, result);
    if (driver.joinable()) driver.join();
    check(submitted && result.paths.size() == 1 && result.paths.front() == new_path,
        "Save submits the new path and leaves the old filename absent");
}
}

int wmain(int argc, wchar_t** argv) {
    using namespace pulse;
    namespace fs = std::filesystem;
    using enum ui::PickerCommand;
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc == 2 && std::wstring_view(argv[1]) == L"--clipboard") return ClipboardCases();
    compat::EnableDpiAwareness();
    if (FAILED(OleInitialize(nullptr))) return 2;
    l10n::Initialize(GetModuleHandleW(nullptr), L"en-US");
    const auto root = fs::absolute(L"bench_data") /
        (L"picker-commands-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    if (!fs::create_directories(root)) return 2;
    const auto first = root / L"Project";
    const auto second = root / L"Renamed Project";
    int failures = 0;
    const auto check = [&](bool ok, const char* label) {
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
        failures += !ok;
    };
    const auto background = ui::PickerContextMenu({true, true, 0, false});
    const auto selected = ui::PickerContextMenu({true, true, 1, true});
    check(background.size() == 3 && background.front().children.size() == 3 && !background[1].enabled,
        "empty space offers New and Refresh, with file Paste disabled for an empty clipboard");
    check(std::any_of(selected.begin(), selected.end(), [](const auto& item) {
              return item.command == static_cast<int>(Paste) && item.enabled;
          }) && std::any_of(selected.begin(), selected.end(), [](const auto& item) {
              return item.command == static_cast<int>(Rename) && item.text.ends_with(L"…");
          }) && !ui::PickerCommandEnabled(Rename, {true, true, 2, true}) &&
          !ui::PickerCommandEnabled(Recycle, {true, false, 1, true}),
        "file context enables clipboard commands and protects multiselect rename and drive roots");
    {
        const uint32_t stale_sequence = GetClipboardSequenceNumber() == 1 ? 2 : 1;
        ops::ClipboardData captured;
        check(ops::ReadClipboardForPaste(stale_sequence, captured) ==
            HRESULT_FROM_WIN32(ERROR_CANCELLED) && captured.paths.empty(),
            "a stale clipboard is rejected before querying its data or destination");
    }

    std::thread driver;
    std::atomic_bool finish{false};
    ui::FilePickerSpec spec;
    spec.initial_path = root.wstring() + L"\\";
    spec.software_device = true;
    spec.should_close = [&] { return finish.load(); };
    spec.on_ready = [&](HWND picker) {
        const int dpi = static_cast<int>(compat::WindowDpi(picker));
        const auto px = [dpi](int dip) { return MulDiv(dip, dpi, 96); };
        SetWindowPos(picker, nullptr, 0, 0, px(530), px(420),
            SWP_NOZORDER | SWP_NOMOVE | SWP_NOACTIVATE);
        driver = std::thread([&, picker, px] {
            const auto click = [&](int x, int y) {
                const auto point = MAKELPARAM(px(x), px(y));
                PostMessageW(picker, WM_LBUTTONDOWN, MK_LBUTTON, point);
                PostMessageW(picker, WM_LBUTTONUP, 0, point);
            };
            const auto input_name = [&](const wchar_t* value) {
                HWND edit = nullptr;
                if (!WaitFor([&] {
                    const HWND dialog = FindOnThread(picker, L"PulsePickerItemName");
                    edit = dialog ? FindWindowExW(dialog, nullptr, L"Edit", nullptr) : nullptr;
                    return edit != nullptr;
                })) return false;
                SendMessageW(edit, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(value));
                return PostMessageW(edit, WM_KEYDOWN, VK_RETURN, 1) != FALSE;
            };
            HWND menu = nullptr;
            const auto path = root.wstring();
            const bool loaded = WaitFor([&] {
                for (HWND edit = FindWindowExW(picker, nullptr, L"Edit", nullptr); edit;
                     edit = FindWindowExW(picker, edit, L"Edit", nullptr)) {
                    std::wstring value(path.size() + 5, L'\0');
                    const auto length = GetWindowTextW(edit, value.data(), static_cast<int>(value.size()));
                    if (std::wstring_view(value.data(), static_cast<size_t>(length)) == path) return true;
                }
                return false;
            });
            check(loaded, "picker publishes the fixture directory after loading");
            if (loaded) click(224, 116);
            const bool opened = loaded && WaitFor([&] { return (menu = FindOnThread(picker, L"PulseFluentMenu")) != nullptr; });
            check(opened, "narrow picker toolbar opens the shared New menu");
            if (opened) PostMessageW(menu, WM_KEYDOWN, VK_RETURN, 1);
            const bool created = opened && input_name(L"Project") &&
                WaitFor([&] { return fs::is_directory(first); });
            check(created, "New Folder creates a real directory through the picker");

            POINT row{px(236), px(182)};
            ClientToScreen(picker, &row);
            const bool context = created && WaitFor([&] {
                if ((menu = FindOnThread(picker, L"PulseFluentMenu"))) return true;
                PostMessageW(picker, WM_CONTEXTMENU, reinterpret_cast<WPARAM>(picker), MAKELPARAM(row.x, row.y));
                return false;
            });
            check(context, "the reloaded file list opens its Fluent right-click menu");
            if (context) PostMessageW(menu, WM_KEYDOWN, VK_ESCAPE, 1);
            if (context && WaitFor([&] { return !FindOnThread(picker, L"PulseFluentMenu"); })) {
                click(236, 182);
                PostMessageW(picker, WM_KEYDOWN, VK_F2, 1);
            }
            const bool renamed = context && input_name(L"Renamed Project") &&
                WaitFor([&] { return !fs::exists(first) && fs::is_directory(second); });
            check(renamed, "F2 renames the selected directory on disk");
            finish.store(true);
        });
    };
    ui::FilePickerResult result;
    const bool submitted = ui::ShowFilePicker(nullptr, spec, false, {0.0f, 0.47f, 0.83f, 1.0f}, result);
    if (driver.joinable()) driver.join();
    check(!submitted && result.paths.empty(), "file management leaves the selection dialog open until cancelled");
    SaveRenameCase(root, check);
    std::error_code cleanup;
    fs::remove_all(root, cleanup);
    OleUninitialize();
    return failures || cleanup ? 1 : 0;
}
