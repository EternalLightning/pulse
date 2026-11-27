// SPDX-License-Identifier: Apache-2.0
#include "../common/windows_compat.h"
#include "../app/app_file_picker.h"
#include "../app/app_ops_ui.h"
#include "../app/app_prefs.h"
#include "../ops/clipboard.h"
#include "../ops/picker_file_operations.h"
#include "../ui/file_picker_commands.h"
#include "../ui/file_picker_dialog.h"
#include "../ui/file_picker_listing.h"
#include "../ui/file_view_menu.h"
#include "../ui/view_mode_transition.h"

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
    pulse::AppState* paste_state = nullptr;
    bool rendered = false;
    bool cancelled_before_mutation = false;

    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
        if (message == pulse::WM_CLIPBOARD_PASTE_RESULT) {
            auto& file = *reinterpret_cast<DeferredFile*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
            if (file.paste_state) pulse::HandleClipboardPasteResult(*file.paste_state, wparam);
            return 0;
        }
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

bool MainPasteDispatch(const std::filesystem::path& root) {
    using namespace pulse;
    namespace fs = std::filesystem;
    const auto source = root / L"sample.txt";
    const auto target = root / L"target";
    const auto other = root / L"other";
    fs::create_directory(target);
    fs::create_directory(other);
    std::ofstream(source) << "mailbox fixture";

    WNDCLASSW wc{};
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"PulsePasteResultFixture";
    wc.lpfnWndProc = DeferredFile::WindowProc;
    RegisterClassW(&wc);
    const auto owner = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0,
        HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
    if (!owner) return false;

    auto main = std::make_unique<AppState>();
    app::Pane pane;
    pane.view.current_path = other.wstring();
    main->hwnd = owner;
    main->pane = &pane;
    main->clipboard_paste_mailbox = std::make_shared<WindowResultMailbox<app::ClipboardPasteResult>>(
        owner, WM_CLIPBOARD_PASTE_RESULT);
    DeferredFile fixture;
    fixture.paste_state = main.get();
    SetWindowLongPtrW(owner, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&fixture));
    main->ops.Start([] {});
    const auto sequence = GetClipboardSequenceNumber();
    const auto mailbox = main->clipboard_paste_mailbox;
    const bool sent = mailbox->Publish({app::ClipboardPasteSource::Paths, {{source.wstring()}, false, sequence + 1},
                                       other.wstring(), {}}) &&
        mailbox->Publish({app::ClipboardPasteSource::Paths, {{source.wstring()}, false, sequence},
                          target.wstring(), {}});
    const bool ok = sent && WaitWithMessages([&] { return main->ops.CanUndo(); }) &&
        main->ops.DrainCompletions().size() == 1 && fs::exists(source) &&
        fs::exists(target / source.filename()) && !fs::exists(other / source.filename());
    mailbox->Close();
    main->ops.Stop();
    DestroyWindow(owner);
    return ok;
}

int PickerCopyUndoConfirmation(const std::filesystem::path& parent) {
    using namespace pulse::ops;
    namespace fs = std::filesystem;
    const auto root = parent / (L"Pulse-picker-undo-" + std::to_wstring(GetCurrentProcessId()) +
        L"-" + std::to_wstring(GetTickCount64()));
    if (!fs::create_directory(root)) return 2;
    const auto source = root / L"owned.txt", destination = root / L"destination";
    const auto product = destination / source.filename();
    fs::create_directory(destination);
    std::ofstream(source) << "picker copy undo";
    const auto owner = CreateWindowExW(0, L"STATIC", L"Picker undo confirmation fixture", 0,
        0, 0, 0, 0, HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
    const auto session = CreatePickerFileOperations();
    const auto ready = [](const auto& operation) {
        return WaitWithMessages([&] { return operation->ready.load(std::memory_order_acquire); });
    };
    OpRequest copy;
    copy.type = OpType::Copy;
    copy.dest_dir = destination.wstring();
    copy.sources = {source.wstring()};
    auto operation = std::make_shared<PickerFileOperation>();
    bool ok = owner && SubmitPickerFileOperation(session, operation, std::move(copy), owner, true) &&
        ready(operation) && operation->result == S_OK && fs::exists(source) && fs::exists(product);
    for (bool accept : {false, true}) {
        if (!ok) break;
        operation = std::make_shared<PickerFileOperation>();
        std::optional<DeleteConfirmation> pending;
        const bool queued = SubmitPickerFileOperation(session, operation, PickerUndo{}, owner, true);
        const bool prompted = queued && WaitWithMessages([&] {
            pending = session->Manager().PendingDeleteConfirmation();
            return pending.has_value() || operation->ready.load(std::memory_order_acquire);
        }) && pending && pending->plan.origin == DeleteOrigin::Undo && pending->plan.targets.size() == 1 &&
            pulse::fs::NormalizePath(pending->plan.targets.front().path) ==
                pulse::fs::NormalizePath(product.wstring()) && fs::exists(product);
        if (pending) session->Manager().ResolveDeleteConfirmation(pending->token, prompted && accept);
        else if (!operation->ready.load(std::memory_order_acquire)) operation->Cancel(*session);
        const auto waited = queued && ready(operation);
        ok = prompted && waited && operation->result == (accept ? S_OK : HRESULT_FROM_WIN32(ERROR_CANCELLED)) &&
            fs::exists(product) != accept && fs::exists(source);
        std::printf("[%s] picker copy undo %s confirmation and preserves the original\n",
            ok ? "PASS" : "FAIL", accept ? "accepts" : "honors declined");
    }
    session->Manager().Stop();
    if (owner) DestroyWindow(owner);
    std::error_code error;
    fs::remove_all(root, error);
    return ok && !error ? 0 : 1;
}

int ClipboardCases() {
    using namespace pulse::ops;
    namespace fs = std::filesystem;
    const auto previous_station = GetProcessWindowStation();
    const auto previous_desktop = GetThreadDesktop(GetCurrentThreadId());
    const auto station = CreateWindowStationW(nullptr, CWF_CREATE_ONLY, WINSTA_ALL_ACCESS, nullptr);
    if (!station || !SetProcessWindowStation(station)) {
        const auto error = GetLastError();
        if (station) CloseWindowStation(station);
        std::printf("[SKIP] no fresh private clipboard station (Win32 %lu)\n", error);
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
                "first picker paste renders delayed CF_HDROP and records undo");

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

            const auto source_folder = root / L"from" / L"merged";
            const auto target_folder = destination.parent_path() / L"merged";
            fs::create_directories(source_folder);
            fs::create_directories(target_folder);
            std::ofstream(source_folder / L"moved.txt") << "move";
            std::ofstream(target_folder / L"kept.txt") << "keep";
            operation = std::make_shared<PickerFileOperation>();
            check(WriteClipboard({source_folder.wstring()}, true, owner) && start(operation) && ready(operation) &&
                operation->result == S_OK && !fs::exists(source_folder) &&
                fs::exists(target_folder / L"moved.txt") && fs::exists(target_folder / L"kept.txt") &&
                !IsClipboardFormatAvailable(CF_HDROP), "cut merged into a same-name folder clears its removed source");

            operation = std::make_shared<PickerFileOperation>();
            check(SubmitPickerFileOperation(session, operation, PickerUndo{}, owner, true) && ready(operation) &&
                operation->result == S_OK && fs::exists(source_folder / L"moved.txt") &&
                !fs::exists(target_folder / L"moved.txt") && fs::exists(target_folder / L"kept.txt"),
                "undoing a picker merge restores the moved file and keeps the existing folder contents");

            const auto main_target = root / L"main-target";
            const auto other = root / L"other";
            fs::create_directories(main_target);
            fs::create_directories(other);
            std::ofstream(source) << "main copy";
            auto main = std::make_unique<pulse::AppState>();
            pulse::app::Pane pane;
            pane.view.current_path = main_target.wstring();
            main->pane = &pane;
            main->hwnd = owner;
            main->clipboard_paste_mailbox =
                std::make_shared<pulse::WindowResultMailbox<pulse::app::ClipboardPasteResult>>(
                    owner, pulse::WM_CLIPBOARD_PASTE_RESULT);
            file.paste_state = main.get();
            main->ops.Start([] {});
            const bool main_started = main->worker.Start({});
            const bool main_published = main_started && file.Publish(owner);
            if (main_published) pulse::PasteIntoCurrent(*main);
            const bool returned_before_render = !file.rendered;
            pane.view.current_path = other.wstring();
            check(main_published && returned_before_render && WaitWithMessages([&] {
                return fs::exists(main_target / source.filename()) && main->ops.CanUndo();
            }) && file.rendered && !fs::exists(other / source.filename()) && fs::exists(source),
                "main paste returns before clipboard rendering and keeps the original destination after navigation");
            main->clipboard_paste_mailbox->Close();
            main->worker.Stop();
            main->ops.Stop();
            file.paste_state = nullptr;

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

bool DoubleClickAfterRefresh(const std::filesystem::path& root) {
    namespace fs = std::filesystem;
    const auto original = root / L"before.txt";
    const auto replacement = root / L"after.txt";
    std::ofstream(original) << "file";
    std::thread driver;
    std::atomic_bool finish{false};
    std::atomic_int submissions{0};
    bool retained = false;
    pulse::ui::FilePickerSpec spec;
    spec.initial_path = root.wstring() + L"\\";
    spec.software_device = true;
    spec.should_close = [&] { return finish.load(); };
    spec.begin_submit = [&](const pulse::ui::FilePickerResult&) { return ++submissions == 1; };
    spec.on_ready = [&](HWND picker) {
        const int dpi = static_cast<int>(pulse::compat::WindowDpi(picker));
        const auto px = [dpi](int dip) { return MulDiv(dip, dpi, 96); };
        SetWindowPos(picker, nullptr, 0, 0, px(530), px(420), SWP_NOZORDER | SWP_NOMOVE | SWP_NOACTIVATE);
        driver = std::thread([&, picker, px] {
            const HWND first = FindWindowExW(picker, nullptr, L"Edit", nullptr);
            const HWND second = FindWindowExW(picker, first, L"Edit", nullptr);
            const bool first_address = EditText(first).starts_with(root.wstring());
            const HWND address = first_address ? first : second;
            const HWND filename = first_address ? second : first;
            const LPARAM row = MAKELPARAM(px(236), px(182));
            std::error_code error;
            if (WaitFor([&] { return EditText(address) == root.wstring(); })) {
                SendMessageW(picker, WM_LBUTTONDOWN, MK_LBUTTON, row);
                SendMessageW(picker, WM_LBUTTONUP, 0, row);
                if (EditText(filename) == original.filename().wstring()) {
                    fs::rename(original, replacement, error);
                    SendMessageW(address, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(L"refresh pending"));
                    SendMessageW(picker, WM_KEYDOWN, VK_F5, 1);
                    if (!error && WaitFor([&] { return EditText(address) == root.wstring(); })) {
                        SendMessageW(picker, WM_LBUTTONDBLCLK, MK_LBUTTON, row);
                        SendMessageW(picker, WM_LBUTTONUP, 0, row);
                        retained = submissions.load() == 0 && EditText(filename) == replacement.filename().wstring();
                        if (retained) SendMessageW(picker, WM_LBUTTONDBLCLK, MK_LBUTTON, row);
                    }
                }
            }
            finish.store(true);
        });
    };
    pulse::ui::FilePickerResult result;
    const bool submitted = pulse::ui::ShowFilePicker(nullptr, spec, false, {0.0f, 0.47f, 0.83f, 1.0f}, result);
    if (driver.joinable()) driver.join();
    return retained && submitted && submissions.load() == 1 &&
        result.paths.size() == 1 && result.paths.front() == replacement.wstring();
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
            bool restored = false;
            if (loaded) {
                const auto missing = (folder / L"unavailable").wstring();
                SendMessageW(address, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(missing.c_str()));
                PostMessageW(address, WM_KEYDOWN, VK_RETURN, 1);
                restored = WaitFor([&] { return EditText(address) == folder.wstring(); });
            }
            check(restored, "failed navigation restores the actual directory in the address field");
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
            if (loaded) {
                SendMessageW(filename, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(L"Draft.txt"));
                POINT point{px(236), px(182)};
                ClientToScreen(picker, &point);
                PostMessageW(picker, WM_CONTEXTMENU, reinterpret_cast<WPARAM>(picker), MAKELPARAM(point.x, point.y));
                const bool item_menu = WaitFor([&] { return (menu = FindOnThread(picker, L"PulseFluentMenu")) != nullptr; });
                if (item_menu) PostMessageW(menu, WM_KEYDOWN, VK_ESCAPE, 1);
                check(item_menu && WaitFor([&] { return !FindOnThread(picker, L"PulseFluentMenu"); }) &&
                    EditText(filename) == L"Draft.txt", "a context-menu selection leaves a typed save draft intact");
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
            const bool prompted = dismissed && WaitFor([&] {
                for (HWND field = FindWindowExW(picker, nullptr, L"Edit", nullptr); field;
                    field = FindWindowExW(picker, field, L"Edit", nullptr)) {
                    if (field == address || field == filename || !IsWindowVisible(field)) continue;
                    if (EditText(field) != L"Old.txt") continue;
                    edit = field;
                    return true;
                }
                return false;
            });
            check(prompted && !FindOnThread(picker, L"PulsePickerItemName"),
                "F2 after using the toolbar edits the selected name inside the Save picker");
            if (prompted) {
                SendMessageW(edit, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(L"New.txt"));
                PostMessageW(edit, WM_KEYDOWN, VK_RETURN, 1);
            }
            const bool updated = prompted && WaitFor([&] {
                return fs::exists(new_path) && !fs::exists(old_path) && EditText(filename) == L"New.txt";
            });
            check(updated, "the Save filename follows the renamed selection");
            if (updated) {
                SendMessageW(filename, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(L"New.txt"));
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
        "a relative Save filename resolves against the displayed directory after navigation failure and rename");
}

template <typename Check>
void CancelSubmitCase(const std::filesystem::path& root, bool close, Check check) {
    std::atomic_bool submitted{false}, cancelled{false}, timed_out{false};
    std::thread driver;
    pulse::ui::FilePickerSpec spec;
    spec.mode = pulse::ui::PickerMode::Open;
    spec.initial_path = root.wstring();
    spec.filename = L"pending.txt";
    spec.software_device = true;
    spec.begin_submit = [&](const auto&) { submitted = true; return true; };
    spec.begin_cancel = [&] { cancelled = true; return true; };
    spec.poll_submit = [&]() -> std::optional<pulse::ui::PickerSubmitReply> {
        if (cancelled) return pulse::ui::PickerSubmitReply{true, false, {}};
        return std::nullopt;
    };
    spec.should_close = [&] { return timed_out.load(); };
    spec.on_ready = [&](HWND picker) {
        driver = std::thread([&, picker] {
            const bool ready = WaitFor([&] {
                if (submitted) return true;
                for (HWND edit = FindWindowExW(picker, nullptr, L"Edit", nullptr); edit;
                     edit = FindWindowExW(picker, edit, L"Edit", nullptr)) {
                    if (EditText(edit) == spec.filename) PostMessageW(edit, WM_KEYDOWN, VK_RETURN, 1);
                }
                return false;
            });
            if (ready && close) PostMessageW(picker, WM_CLOSE, 0, 0);
            if (ready && !close) {
                RECT client{};
                GetClientRect(picker, &client);
                const auto dpi = static_cast<int>(pulse::compat::WindowDpi(picker));
                const auto button = MAKELPARAM(client.right - MulDiv(138, dpi, 96),
                    client.bottom - MulDiv(30, dpi, 96));
                PostMessageW(picker, WM_LBUTTONDOWN, MK_LBUTTON, button);
                PostMessageW(picker, WM_LBUTTONUP, 0, button);
            }
            if (!ready || !WaitFor([&] { return !IsWindow(picker); })) timed_out = true;
        });
    };
    pulse::ui::FilePickerResult result;
    const bool accepted = pulse::ui::ShowFilePicker(nullptr, spec, false, {0.0f, 0.47f, 0.83f, 1.0f}, result);
    if (driver.joinable()) driver.join();
    check(submitted && cancelled && !timed_out && !accepted && result.paths.empty(),
        close ? "WM_CLOSE cancels pending submission and discards late acceptance" :
            "the Cancel button cancels pending submission and discards late acceptance");
}
bool PickerProjection(const std::filesystem::path& root) {
    using namespace pulse;
    for (const auto* name : {L"alpha.txt", L"beta.txt", L"alpha.pdf", L"beta.pdf"})
        std::ofstream(root / name) << "fixture";
    const auto load = [](const ui::Listing& request) {
        if (!ui::SubmitPickerListing(request)) return false;
        if (WaitFor([&] { return request->ready.load(std::memory_order_acquire); }, 3000))
            return request->succeeded;
        request->cancelled = true;
        return false;
    };
    const auto matches = [](const ui::Listing& request, const std::vector<std::wstring_view>& names) {
        const auto& view = request->projection;
        if (view.indices.size() != names.size() || view.source_to_view.size() != request->base->size()) return false;
        for (size_t row = 0; row < names.size(); ++row) {
            const auto source = static_cast<size_t>(view.indices[row]);
            if (source >= request->base->size() || view.source_to_view[source] != static_cast<int>(row) ||
                (*request->base)[source].name != names[row]) return false;
        }
        return true;
    };
    const auto initial = std::make_shared<ui::ListingRequest>();
    initial->directory = root.wstring();
    initial->sort_direction = model::SortDirection::Desc;
    if (!load(initial) || !matches(initial, {L"beta.txt", L"beta.pdf", L"alpha.txt", L"alpha.pdf"})) return false;

    const auto filtered = std::make_shared<ui::ListingRequest>();
    filtered->directory = initial->directory;
    filtered->base = initial->base;
    filtered->project_only = true;
    filtered->query = L"beta";
    filtered->grouping = {ui::GroupMode::Type, model::SortDirection::Asc};
    return load(filtered) && filtered->base == initial->base && filtered->base->size() == 4 &&
        matches(filtered, {L"beta.pdf", L"beta.txt"}) && filtered->projection.groups.size() == 2 &&
        std::count(filtered->projection.source_to_view.begin(), filtered->projection.source_to_view.end(), -1) == 2;
}

bool PickerVisibility(const std::filesystem::path& root) {
    using namespace pulse;
    constexpr const wchar_t* names[]{L"a-ordinary.txt", L"b-system.txt", L"c-hidden.txt", L"d-protected.txt"};
    constexpr DWORD attributes[]{FILE_ATTRIBUTE_NORMAL, FILE_ATTRIBUTE_SYSTEM, FILE_ATTRIBUTE_HIDDEN,
        FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM};
    for (size_t i = 0; i < std::size(names); ++i) {
        const auto file = root / names[i];
        std::ofstream(file) << "fixture";
        if (!SetFileAttributesW(file.c_str(), attributes[i])) return false;
    }
    const auto directory = root.wstring();
    for (bool hidden : {false, true}) for (bool protected_files : {false, true}) {
        bool passed = false;
        std::atomic_bool done = false;
        std::thread driver;
        ui::FilePickerSpec spec;
        spec.initial_path = directory + L"\\";
        spec.filename = L"draft";
        spec.visibility = {hidden, protected_files};
        spec.software_device = true;
        spec.should_close = [&] { return done.load(); };
        spec.on_ready = [&](HWND picker) {
            driver = std::thread([&, picker] {
                HWND address = nullptr, filename = nullptr;
                for (HWND edit = FindWindowExW(picker, nullptr, L"Edit", nullptr); edit;
                     edit = FindWindowExW(picker, edit, L"Edit", nullptr)) {
                    if (EditText(edit) == L"draft") filename = edit;
                    if (EditText(edit).starts_with(directory)) address = edit;
                }
                const auto select = [&](WPARAM key, const wchar_t* expected) {
                    SendMessageW(picker, WM_KEYDOWN, key, 1);
                    return EditText(filename) == expected;
                };
                if (address && filename && WaitFor([&] { return EditText(address) == directory; })) {
                    const auto* last = names[hidden ? (protected_files ? 3 : 2) : 1];
                    passed = select(VK_HOME, names[0]) && select(VK_DOWN, names[1]) && select(VK_END, last);
                }
                done = true;
            });
        };
        ui::FilePickerResult result;
        ui::ShowFilePicker(nullptr, spec, false, {0.0f, 0.47f, 0.83f, 1.0f}, result);
        if (driver.joinable()) driver.join();
        if (!passed) {
            std::printf("[DIAG] picker visibility failed for hidden=%d protected=%d\n", hidden, protected_files);
            return false;
        }
    }
    return true;
}

bool PickerNarrowFooter(const std::filesystem::path& root) {
    using namespace pulse;
    const auto directory = root.wstring();
    const auto file = root / L"sample.txt";
    std::ofstream(file) << "fixture";
    std::atomic_bool custom_called = false, timed_out = false;
    std::thread driver;
    ui::FilePickerSpec spec;
    spec.initial_path = directory + L"\\";
    spec.filename = file.filename().wstring();
    spec.custom_button_label = L"Choose the documents to attach to this message using this custom command";
    spec.software_device = true;
    spec.should_close = [&] { return timed_out.load(); };
    spec.on_custom_button = [&](const ui::FilePickerSnapshot&) { custom_called = true; };
    spec.on_ready = [&](HWND picker) {
        const int dpi = static_cast<int>(compat::WindowDpi(picker));
        const auto px = [dpi](int dip) { return MulDiv(dip, dpi, 96); };
        const auto resize = [picker, px](int height) {
            SetWindowPos(picker, nullptr, 0, 0, px(530), px(height),
                SWP_NOZORDER | SWP_NOMOVE | SWP_NOACTIVATE);
        };
        resize(500);
        driver = std::thread([&, picker, px, resize] {
            const auto click = [&](int x, int y) {
                const auto point = MAKELPARAM(px(x), px(y));
                SendMessageW(picker, WM_LBUTTONDOWN, MK_LBUTTON, point);
                SendMessageW(picker, WM_LBUTTONUP, 0, point);
            };
            const auto loaded = [&] {
                for (HWND edit = FindWindowExW(picker, nullptr, L"Edit", nullptr); edit;
                     edit = FindWindowExW(picker, edit, L"Edit", nullptr))
                    if (EditText(edit) == directory) return true;
                return false;
            };
            if (WaitFor(loaded)) {
                click(250, 428);
                resize(372);
                click(470, 340);
                if (WaitFor([&] { return !IsWindow(picker); }, 2000)) return;
            }
            timed_out = true;
        });
    };
    ui::FilePickerResult result;
    const bool accepted = ui::ShowFilePicker(nullptr, spec, false, {0.0f, 0.47f, 0.83f, 1.0f}, result);
    if (driver.joinable()) driver.join();
    return custom_called && accepted && !timed_out && result.paths == std::vector{file.wstring()};
}

bool SavedPickerViews(const std::filesystem::path& root) {
    using namespace pulse;
    const auto icons = (root / L"icons").wstring();
    const auto grouped = (root / L"grouped").wstring();
    std::filesystem::create_directory(icons);
    std::filesystem::create_directory(grouped);
    std::filesystem::create_directory(root / L"icons" / L"zzz");
    for (int i = 0; i < 8; ++i)
        std::ofstream(root / L"icons" / (L"item-" + std::to_wstring(i) + L".txt")) << i;
    for (const auto* name : {L"a.pdf", L"b.pdf", L"y.txt", L"z.txt"})
        std::ofstream(root / L"grouped" / name) << "fixture";

    app::AppPrefs saved;
    saved.folder_sort_mode = static_cast<int>(model::FolderSortMode::FollowDirection);
    saved.folder_views.Set(icons, ui::ViewMode::LargeIcons);
    saved.folder_views.SetSort(icons, ui::SortColumn::Name, ui::SortDirection::Desc);
    saved.folder_views.SetSort(grouped, ui::SortColumn::Name, ui::SortDirection::Desc);
    saved.folder_views.SetGroup(grouped, ui::GroupMode::Type, ui::SortDirection::Asc);
    app::AppPrefs prefs;
    if (!prefs.FromJson(saved.ToJson())) return false;

    bool passed = false;
    std::atomic_bool finished = false;
    std::thread driver;
    ui::FilePickerSpec spec;
    spec.initial_path = icons + L"\\";
    spec.filename = L"draft";
    spec.software_device = true;
    spec.should_close = [&] { return finished.load(); };
    app::SetFilePickerFolderViews(spec, prefs);
    spec.on_ready = [&](HWND picker) {
        const int dpi = static_cast<int>(compat::WindowDpi(picker));
        const auto px = [dpi](int dip) { return MulDiv(dip, dpi, 96); };
        SetWindowPos(picker, nullptr, 0, 0, px(530), px(420), SWP_NOZORDER | SWP_NOMOVE | SWP_NOACTIVATE);
        driver = std::thread([&, picker, px] {
            HWND address = nullptr, filename = nullptr;
            const auto key = [picker](WPARAM code) { PostMessageW(picker, WM_KEYDOWN, code, 1); };
            const auto click = [&](int x, int y) {
                const auto point = MAKELPARAM(px(x), px(y));
                PostMessageW(picker, WM_LBUTTONDOWN, MK_LBUTTON, point);
                PostMessageW(picker, WM_LBUTTONUP, 0, point);
            };
            const auto expect = [&](const wchar_t* name) {
                const bool matched = WaitFor([&] { return EditText(filename) == name; }, 1500);
                if (!matched) std::printf("[DIAG] expected %ls, saw %ls\n", name, EditText(filename).c_str());
                return matched;
            };
            const auto navigate = [&](const std::wstring& path, const std::wstring& expected) {
                SendMessageW(address, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(path.c_str()));
                PostMessageW(address, WM_KEYDOWN, VK_RETURN, 1);
                return WaitFor([&] { return EditText(address) == expected; }, 3000);
            };
            const bool loaded = WaitFor([&] {
                for (HWND edit = FindWindowExW(picker, nullptr, L"Edit", nullptr); edit;
                     edit = FindWindowExW(picker, edit, L"Edit", nullptr)) {
                    if (EditText(edit) == icons) address = edit;
                    if (EditText(edit) == L"draft") filename = edit;
                }
                return address && filename;
            }, 3000);
            const auto check_icons = [&] {
                click(418, 155);
                key(VK_HOME);
                if (!expect(L"item-7.txt")) return false;
                key(VK_DOWN);
                return expect(L"item-5.txt");
            };
            if (loaded && check_icons() && navigate(grouped + L"\\.", grouped)) {
                click(280, 192);
                key(VK_HOME);
                if (expect(L"b.pdf")) {
                    key(VK_DOWN);
                    passed = expect(L"a.pdf") && navigate(icons + L"\\.", icons) && check_icons() &&
                        navigate(icons + L"\\missing", icons) && check_icons();
                }
            }
            finished = true;
        });
    };
    ui::FilePickerResult result;
    const bool accepted = ui::ShowFilePicker(nullptr, spec, false, {0.0f, 0.47f, 0.83f, 1.0f}, result);
    if (driver.joinable()) driver.join();
    return passed && !accepted;
}

int PickerViews(const std::filesystem::path& root) {
    using namespace pulse;
    std::filesystem::create_directory(root / L"views");
    const auto folder = (root / L"views").wstring();
    const auto filename = [](int i) { return L"entry-" + std::to_wstring(100 + i) + L".txt"; };
    for (int i = 0; i < 30; ++i) std::ofstream(root / L"views" / filename(i)) << i;
    int failures = 0;
    for (const auto mode : {ui::PickerMode::Open, ui::PickerMode::Save}) {
        bool exercised = false;
        std::atomic_bool failed = false;
        std::thread driver;
        ui::FilePickerSpec spec;
        spec.mode = mode;
        spec.title = mode == ui::PickerMode::Open ? L"Picker views - Open" : L"Picker views - Save";
        spec.initial_path = folder + L"\\";
        spec.filename = L"draft";
        spec.software_device = true;
        spec.should_close = [&] { return failed.load(); };
        spec.on_ready = [&](HWND picker) {
            const auto dpi = static_cast<int>(compat::WindowDpi(picker));
            const auto px = [dpi](int dip) { return MulDiv(dip, dpi, 96); };
            SetWindowPos(picker, nullptr, 0, 0, px(530), px(420), SWP_NOZORDER | SWP_NOMOVE | SWP_NOACTIVATE);
            driver = std::thread([&, picker, px] {
                const auto click = [](HWND window, int x, int y) {
                    const auto point = MAKELPARAM(x, y);
                    PostMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, point);
                    PostMessageW(window, WM_LBUTTONUP, 0, point);
                };
                const auto key = [picker](WPARAM value) { PostMessageW(picker, WM_KEYDOWN, value, 1); };
                HWND field = nullptr;
                bool ready = WaitFor([&] {
                    bool loaded = false;
                    for (HWND edit = FindWindowExW(picker, nullptr, L"Edit", nullptr); edit;
                         edit = FindWindowExW(picker, edit, L"Edit", nullptr)) {
                        const auto text = EditText(edit);
                        if (text == L"draft") field = edit;
                        if (text == folder) loaded = true;
                    }
                    return loaded && field;
                }, 3000);
                const int menu_count = static_cast<int>(ui::BuildFileViewChoices(ui::ViewMode::Details, 1).size());
                const auto change_view = [&](ui::ViewMode view) {
                    click(picker, px(492), px(116));
                    HWND menu = nullptr;
                    if (!WaitFor([&] { return (menu = FindOnThread(picker, L"PulseFluentMenu")) != nullptr; }, 1500)) return false;
                    RECT client{};
                    GetClientRect(menu, &client);
                    constexpr auto shadow = ui::FluentMenu::kShadowMargin;
                    const int row = (client.bottom - 2 * shadow) / menu_count;
                    click(menu, client.right / 2, shadow + ui::ViewModeIndex(view) * row + row / 2);
                    if (!WaitFor([&] { return !FindOnThread(picker, L"PulseFluentMenu"); }, 1500)) return false;
                    Sleep(static_cast<DWORD>(ui::ViewModeTransition::kDuration) + 50);
                    return true;
                };
                for (const auto view : {ui::ViewMode::SmallIcons, ui::ViewMode::MediumIcons, ui::ViewMode::LargeIcons}) {
                    if (!ready) break;
                    ready = change_view(view) && EditText(field) ==
                        (view == ui::ViewMode::SmallIcons ? L"draft" : filename(0));
                    if (!ready) break;
                    click(picker, px(418), px(155));
                    ready = WaitFor([&] { return EditText(field) == filename(1); }, 1500);
                    if (!ready) break;
                    key(VK_DOWN);
                    ready = WaitFor([&] { return EditText(field) == filename(3); }, 1500);
                    key(VK_HOME);
                    ready = ready && WaitFor([&] { return EditText(field) == filename(0); }, 1500);
                }
                if (ready && change_view(ui::ViewMode::List)) {
                    click(picker, px(245), px(153));
                    key(VK_RIGHT);
                    ready = WaitFor([&] { return EditText(field) == filename(5); }, 1500);
                } else ready = false;
                if (ready) {
                    POINT cursor{px(440), px(195)};
                    ClientToScreen(picker, &cursor);
                    const LPARAM wheel_at = MAKELPARAM(cursor.x, cursor.y);
                    const auto half_step = [&] {
                        PostMessageW(picker, WM_MOUSEWHEEL, MAKEWPARAM(MK_CONTROL, -WHEEL_DELTA / 2), wheel_at);
                    };
                    half_step();
                    key(VK_HOME);
                    ready = WaitFor([&] { return EditText(field) == filename(0); }, 1500);
                    key(VK_RIGHT);
                    ready = ready && WaitFor([&] { return EditText(field) == filename(5); }, 1500);
                    half_step();
                    key(VK_HOME);
                    key(VK_RIGHT);
                    key(VK_DOWN);
                    ready = ready && WaitFor([&] { return EditText(field) == filename(1); }, 1500);
                }
                if (ready) {
                    key(VK_END);
                    ready = WaitFor([&] { return EditText(field) == filename(29); }, 1500);
                }
                exercised = ready;
                if (ready) key(VK_RETURN);
                if (!ready || !WaitFor([&] { return !IsWindow(picker); }, 1800)) failed = true;
            });
        };
        ui::FilePickerResult result;
        const bool accepted = ui::ShowFilePicker(nullptr, spec, false, {0.0f, 0.47f, 0.83f, 1.0f}, result);
        if (driver.joinable()) driver.join();
        const bool ok = exercised && !failed && accepted && result.paths == std::vector{folder + L"\\" + filename(29)};
        std::printf("[%s] %s changes view by menu and high-resolution wheel, preserving selection and submission\n",
            ok ? "PASS" : "FAIL", mode == ui::PickerMode::Open ? "Open" : "Save");
        failures += !ok;
    }
    return failures ? 1 : 0;
}
}

int wmain(int argc, wchar_t** argv) {
    using namespace pulse;
    namespace fs = std::filesystem;
    using enum ui::PickerCommand;
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc == 2 && std::wstring_view(argv[1]) == L"--clipboard") return ClipboardCases();
    if (argc == 3 && std::wstring_view(argv[1]) == L"--copy-undo-confirm")
        return PickerCopyUndoConfirmation(fs::absolute(argv[2]));
    compat::EnableDpiAwareness();
    if (FAILED(OleInitialize(nullptr))) return 2;
    l10n::Initialize(GetModuleHandleW(nullptr), L"en-US");
    if (argc == 3 && std::wstring_view(argv[1]) == L"--visual-save") {
        ui::FilePickerSpec spec;
        spec.mode = ui::PickerMode::Save;
        spec.title = L"Picker views - Visual Save";
        spec.initial_path = fs::absolute(argv[2]).wstring();
        const auto deadline = GetTickCount64() + 20000;
        spec.should_close = [&] { return GetTickCount64() >= deadline; };
        ui::FilePickerResult result;
        ui::ShowFilePicker(nullptr, spec, false, {0.0f, 0.47f, 0.83f, 1.0f}, result);
        OleUninitialize();
        return 0;
    }
    const auto root = fs::absolute(L"bench_data") /
        (L"picker-commands-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    if (!fs::create_directories(root)) return 2;
    const std::wstring_view option = argc == 2 ? argv[1] : L"";
    if (option == L"--paste-dispatch") {
        const bool passed = MainPasteDispatch(root);
        std::error_code error;
        fs::remove_all(root, error);
        OleUninitialize();
        std::printf("[%s] main paste drops a stale result and submits the original folder only\n",
            passed && !error ? "PASS" : "FAIL");
        return passed && !error ? 0 : 1;
    }
    if (option == L"--projection-only") {
        const bool passed = PickerProjection(root);
        std::error_code error;
        fs::remove_all(root, error);
        OleUninitialize();
        std::printf("[%s] sorting, filtering and grouping project row indices from one directory snapshot\n",
            passed ? "PASS" : "FAIL");
        return passed && !error ? 0 : 1;
    }
    if (option == L"--saved-views-only" || option == L"--visibility-only" || option == L"--narrow-footer-only") {
        const bool visibility = option == L"--visibility-only";
        const bool footer = option == L"--narrow-footer-only";
        const bool passed = visibility ? PickerVisibility(root) : footer ? PickerNarrowFooter(root) : SavedPickerViews(root);
        std::error_code error;
        fs::remove_all(root, error);
        OleUninitialize();
        std::printf("[%s] %s\n", passed ? "PASS" : "FAIL",
            visibility ? "picker matches all four hidden/protected visibility combinations" :
            footer ? "long custom action remains clickable and narrow resize preserves the primary action" :
            "picker loads saved icon layout, sort and group per directory and keeps them on a failed load");
        return passed && !error ? 0 : 1;
    }
    if (argc == 2 && std::wstring_view(argv[1]) == L"--views-only") {
        const int result = PickerViews(root);
        OleUninitialize();
        return result;
    }
    if (argc == 2 && std::wstring_view(argv[1]) == L"--double-click-only") {
        const bool passed = DoubleClickAfterRefresh(root);
        std::printf("[%s] refresh blocks a stale double click and a new pair submits the replacement\n",
            passed ? "PASS" : "FAIL");
        std::error_code error;
        fs::remove_all(root, error);
        OleUninitialize();
        return passed && !error ? 0 : 1;
    }
    const auto first = root / L"Project";
    const auto second = root / L"Renamed Project";
    int failures = 0;
    const auto check = [&](bool ok, const char* label) {
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
        failures += !ok;
    };
    const auto background = ui::PickerContextMenu({true, true, 0, false});
    const auto selected = ui::PickerContextMenu({true, true, 1, true});
    check(background.size() == 4 && background.front().children.size() == 3 && !background[1].enabled &&
            background.back().command == static_cast<int>(Undo) && !background.back().enabled,
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
                click(260, 182);
                click(260, 182);
            }
            HWND rename = nullptr;
            const bool editing = context && WaitFor([&] {
                for (HWND field = FindWindowExW(picker, nullptr, L"Edit", nullptr); field;
                    field = FindWindowExW(picker, field, L"Edit", nullptr)) {
                    if (!IsWindowVisible(field) || EditText(field) != L"Project") continue;
                    rename = field;
                    return true;
                }
                return false;
            });
            if (editing) {
                SendMessageW(rename, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(L"Renamed Project"));
                PostMessageW(rename, WM_KEYDOWN, VK_RETURN, 1);
            }
            const bool renamed = editing && WaitFor([&] { return !fs::exists(first) && fs::is_directory(second); });
            check(renamed && !FindOnThread(picker, L"PulsePickerItemName"), "slow clicks rename the selected directory in place");
            const auto undo = [&] {
                if (!WaitFor([&] {
                        if ((menu = FindOnThread(picker, L"PulseFluentMenu"))) return true;
                        PostMessageW(picker, WM_CONTEXTMENU, reinterpret_cast<WPARAM>(picker), MAKELPARAM(row.x, row.y));
                        return false;
                    })) return false;
                PostMessageW(menu, WM_KEYDOWN, VK_UP, 1); // Wrap from the first command to Undo.
                PostMessageW(menu, WM_KEYDOWN, VK_RETURN, 1);
                return true;
            };
            const bool rename_undone = renamed && undo() &&
                WaitFor([&] { return fs::is_directory(first) && !fs::exists(second); });
            check(rename_undone, "the picker Undo command restores the original directory name");
            const bool recycled = rename_undone && WaitFor([&] {
                if (!fs::exists(first)) return true;
                click(236, 182);
                PostMessageW(picker, WM_KEYDOWN, VK_DELETE, 1);
                return false;
            });
            check(recycled && undo() && WaitFor([&] { return fs::is_directory(first); }),
                "the picker Undo command restores an item moved to the Recycle Bin");
            finish.store(true);
        });
    };
    ui::FilePickerResult result;
    const bool submitted = ui::ShowFilePicker(nullptr, spec, false, {0.0f, 0.47f, 0.83f, 1.0f}, result);
    if (driver.joinable()) driver.join();
    check(!submitted && result.paths.empty(), "file management leaves the selection dialog open until cancelled");
    SaveRenameCase(root, check);
    CancelSubmitCase(root, false, check);
    CancelSubmitCase(root, true, check);
    std::error_code cleanup;
    fs::remove_all(root, cleanup);
    OleUninitialize();
    return failures || cleanup ? 1 : 0;
}
