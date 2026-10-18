#include "../app/app_internal.h"
#include "../app/app_exit.h"
#include "../app/app_input.h"
#include "../app/tab_transfer.h"
#include "../common/file_identity.h"
#include "../common/json_utils.h"
#include "../common/utf8_file.h"
#include "../common/localization.h"
#include "../common/user_storage.h"
#include "../common/windows_compat.h"
#include "../ipc/shell_client.h"
#include "../ui/file_operation_dialog.h"
#include "../ui/home_layout.h"
#include <tlhelp32.h>
#include <winternl.h>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <functional>
#include <cstdio>
#include <algorithm>

LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);

namespace pulse::ipc {
struct ShellLaunchClientTestProbe {
    static uint32_t NextRequestId() { return ShellClient::Instance().next_id_.load(); }
    static bool MatchesPendingLaunch(DWORD broker, uint32_t id, HWND owner, const std::wstring& path) {
        auto& client = ShellClient::Instance();
        std::lock_guard send_lock(client.send_mutex_);
        if (client.child_.dwProcessId != broker) return false;
        std::lock_guard pending_lock(client.pending_mutex_);
        const auto found = client.pending_.find(id);
        ShellLaunchRequest request;
        return found != client.pending_.end() && found->second.type == REQ_SHELL_LAUNCH &&
            ReadShellLaunch(found->second.payload.data(), found->second.payload.size(), request) &&
            request.owner_hwnd == reinterpret_cast<uintptr_t>(owner) && request.verb == L"open" &&
            request.executable.empty() && request.paths == std::vector<std::wstring>{path};
    }
};
}

namespace pulse::ui {
struct FluentMenuTestPeer {
    static std::vector<FluentMenuItem> Items(const FluentMenu& menu) {
        std::vector<FluentMenuItem> items;
        for (int i = 0; i < menu.model_.Count(); ++i) items.push_back(*menu.model_.At(i));
        return items;
    }
    static bool ClickCommand(FluentMenu& menu, int command) {
        for (int row = 0; row < menu.model_.Count(); ++row) {
            const auto* item = menu.model_.At(row);
            if (!item || item->command != command || !item->enabled) continue;
            const int x = FluentMenu::kShadowMargin + menu.model_.WidthPx() / 2;
            const int y = FluentMenu::kShadowMargin + static_cast<int>(menu.FilterHeaderPx() +
                menu.model_.RowTopPx(row) + menu.model_.RowHeightPx() * 0.5f - menu.scroll_y_);
            SendMessageW(menu.hwnd_, WM_LBUTTONUP, 0, MAKELPARAM(x, y));
            return true;
        }
        return false;
    }
};
}

namespace {
using namespace pulse;
int failures = 0;
std::function<void()> menu_timer_action;
VOID CALLBACK MenuTimer(HWND, UINT, UINT_PTR, DWORD) {
    if (menu_timer_action) menu_timer_action();
}
void Check(bool passed, const char* label) {
    std::printf("[%s] %s\n", passed ? "PASS" : "FAIL", label);
    failures += !passed;
}
void Pump(const std::function<bool(const MSG&)>& before_dispatch = {}) {
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        if (message.message == WM_QUIT) continue;
        if (before_dispatch && !before_dispatch(message)) continue;
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}
bool Wait(const std::function<bool()>& done) {
    const auto deadline = GetTickCount64() + 10000;
    while (!done() && GetTickCount64() < deadline) { Pump(); Sleep(10); }
    return done();
}
void ConfirmFixtureOpenWarning(AppState& state, const std::filesystem::path& executable,
                               uint32_t minimum_request, bool& confirmed) {
    if (confirmed) return;
    struct Context {
        AppState& state;
        const std::filesystem::path& executable;
        uint32_t minimum_request;
        bool& confirmed;
    } warning_context{state, executable, minimum_request, confirmed};
    EnumWindows([](HWND window, LPARAM parameter) -> BOOL {
        auto& context = *reinterpret_cast<Context*>(parameter);
        if (context.confirmed || !IsWindowVisible(window)) return TRUE;
        wchar_t title[128]{};
        GetWindowTextW(window, title, ARRAYSIZE(title));
        if (wcscmp(title, L"打开文件 - 安全警告") != 0 && wcscmp(title, L"Open File - Security Warning") != 0) return TRUE;
        DWORD worker = 0, broker = 0;
        GetWindowThreadProcessId(window, &worker);
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snapshot == INVALID_HANDLE_VALUE) return TRUE;
        PROCESSENTRY32W entry{sizeof(entry)};
        if (Process32FirstW(snapshot, &entry)) do {
            if (entry.th32ProcessID == worker) { broker = entry.th32ParentProcessID; break; }
        } while (Process32NextW(snapshot, &entry));
        CloseHandle(snapshot);
        using QueryProcess = LONG(NTAPI*)(HANDLE, ULONG, void*, ULONG, ULONG*);
        const auto query = reinterpret_cast<QueryProcess>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationProcess"));
        HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, worker);
        if (!process || !query) { if (process) CloseHandle(process); return TRUE; }
        ULONG bytes = 0;
        query(process, 60, nullptr, 0, &bytes);
        if (bytes < sizeof(UNICODE_STRING) || bytes > 65536) { CloseHandle(process); return TRUE; }
        std::vector<uint8_t> command_buffer(bytes);
        const LONG status = query(process, 60, command_buffer.data(), bytes, &bytes);
        CloseHandle(process);
        if (status < 0) return TRUE;
        const auto& command_string = *reinterpret_cast<const UNICODE_STRING*>(command_buffer.data());
        const std::wstring command(command_string.Buffer, command_string.Length / sizeof(wchar_t));
        const std::wstring worker_flag = L" --launch-worker " + std::to_wstring(broker) + L" ";
        const auto flag_at = command.find(worker_flag);
        if (flag_at == std::wstring::npos) return TRUE;
        wchar_t* end = nullptr;
        const auto id = wcstoul(command.c_str() + flag_at + worker_flag.size(), &end, 10);
        if (!id || id < context.minimum_request || !end || *end || !ipc::ShellLaunchClientTestProbe::MatchesPendingLaunch(
                broker, static_cast<uint32_t>(id), context.state.hwnd, context.executable.wstring())) return TRUE;
        struct Controls { bool target_seen = false; HWND run = nullptr; std::vector<std::wstring> text; } controls;
        EnumChildWindows(window, [](HWND child, LPARAM child_parameter) -> BOOL {
            auto& child_controls = *reinterpret_cast<Controls*>(child_parameter);
            wchar_t text[2048]{}, kind[64]{};
            GetWindowTextW(child, text, ARRAYSIZE(text));
            GetClassNameW(child, kind, ARRAYSIZE(kind));
            if (*text) child_controls.text.emplace_back(text);
            if (wcsstr(text, L"open-probe\\pulse-open-marker.exe")) child_controls.target_seen = true;
            if (wcscmp(kind, L"Button") == 0 && (wcscmp(text, L"运行(&R)") == 0 ||
                wcscmp(text, L"&Run") == 0 || wcscmp(text, L"Run") == 0)) child_controls.run = child;
            return TRUE;
        }, reinterpret_cast<LPARAM>(&controls));
        if (!controls.target_seen || !controls.run || !IsWindowEnabled(controls.run)) return TRUE;
        RECT run_bounds{};
        POINT original_cursor{};
        if (!GetWindowRect(controls.run, &run_bounds) || !GetCursorPos(&original_cursor)) return TRUE;
        const POINT run_center{(run_bounds.left + run_bounds.right) / 2, (run_bounds.top + run_bounds.bottom) / 2};
        if (WindowFromPoint(run_center) != controls.run) return TRUE;
        std::printf("[DIAG] confirm fixture-only security warning: worker=%lu broker=%lu request=%lu; displayed path is abbreviated, pending request matches the exact fixture target\n",
            worker, broker, id);
        for (const auto& text : controls.text) {
            char utf8[8192]{};
            WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, utf8, ARRAYSIZE(utf8), nullptr, nullptr);
            std::printf("[DIAG] security dialog text: %s\n", utf8);
        }
        UINT sent = 0;
        for (unsigned attempt = 0; attempt < 2; ++attempt) {
            if (attempt) Sleep(300);
            DWORD current_worker = 0;
            GetWindowThreadProcessId(window, &current_worker);
            if (!IsWindow(window) || current_worker != worker || !IsWindowEnabled(controls.run) ||
                !ipc::ShellLaunchClientTestProbe::MatchesPendingLaunch(broker, static_cast<uint32_t>(id),
                    context.state.hwnd, context.executable.wstring()) ||
                !GetWindowRect(controls.run, &run_bounds)) break;
            const POINT click_center{(run_bounds.left + run_bounds.right) / 2, (run_bounds.top + run_bounds.bottom) / 2};
            if (WindowFromPoint(click_center) != controls.run || !SetCursorPos(click_center.x, click_center.y) ||
                WindowFromPoint(click_center) != controls.run) break;
            INPUT click[2]{};
            click[0].type = click[1].type = INPUT_MOUSE;
            click[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
            click[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
            const UINT this_click = SendInput(ARRAYSIZE(click), click, sizeof(INPUT));
            sent += this_click;
            if (this_click == 1) SendInput(1, &click[1], sizeof(INPUT));
        }
        SetCursorPos(original_cursor.x, original_cursor.y);
        context.confirmed = sent != 0;
        std::printf("[DIAG] fixture Run input: sent=%u cursor_restored=true; marker remains the success condition\n", sent);
        return context.confirmed ? FALSE : TRUE;
    }, reinterpret_cast<LPARAM>(&warning_context));
}
bool OpenProbe(AppState& state, const std::filesystem::path& executable) {
    const auto started = GetTickCount64();
    const auto marker = executable.parent_path() / L"opened.marker";
    std::error_code error;
    std::filesystem::remove(marker, error);
    std::vector<std::wstring> launch_errors;
    bool deferred_notify = false;
    const auto capture_errors = [&] {
        auto pending = state.ops.DrainShellLaunchErrors();
        launch_errors.insert(launch_errors.end(), pending.begin(), pending.end());
    };
    const auto capture_before_notify = [&](const MSG& message) {
        if (message.hwnd != state.hwnd || message.message != WM_OPS_NOTIFY) return true;
        capture_errors();
        deferred_notify = true;
        return false;
    };
    bool opened = false;
    bool warning_confirmed = false;
    if (!error) {
        const auto minimum_request = ipc::ShellLaunchClientTestProbe::NextRequestId();
        state.ops.OpenWith(executable.wstring());
        const auto deadline = GetTickCount64() + 60000;
        do {
            capture_errors();
            ConfirmFixtureOpenWarning(state, executable, minimum_request, warning_confirmed);
            opened = GetFileAttributesW(marker.c_str()) != INVALID_FILE_ATTRIBUTES;
            if (opened) break;
            Pump(capture_before_notify);
            Sleep(10);
        } while (GetTickCount64() < deadline);
        capture_errors();
        opened = GetFileAttributesW(marker.c_str()) != INVALID_FILE_ATTRIBUTES;
    }
    if (deferred_notify) PostMessageW(state.hwnd, WM_OPS_NOTIFY, 0, 0);
    std::printf("[DIAG] open probe: elapsed_ms=%llu marker_created=%s\n",
        static_cast<unsigned long long>(GetTickCount64() - started), opened ? "true" : "false");
    if (!opened) {
        const auto utf8 = [](const std::wstring& text) {
            const int bytes = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                nullptr, 0, nullptr, nullptr);
            std::string result(static_cast<size_t>(bytes), '\0');
            if (bytes) WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                result.data(), bytes, nullptr, nullptr);
            return result;
        };
        WIN32_FILE_ATTRIBUTE_DATA attributes{};
        const bool attributes_read = GetFileAttributesExW(executable.c_str(), GetFileExInfoStandard, &attributes) != FALSE;
        const DWORD attributes_error = attributes_read ? ERROR_SUCCESS : GetLastError();
        FileIdentity identity;
        const bool identified = ReadPathFileIdentity(executable.wstring(), identity);
        const DWORD identity_error = identified ? ERROR_SUCCESS : GetLastError();
        std::printf("[DIAG] open probe failed: target=%s marker=%s owner=%p cleanup_error=%d shell_errors=%zu\n",
            utf8(executable.wstring()).c_str(), utf8(marker.wstring()).c_str(), static_cast<void*>(state.hwnd),
            error.value(), launch_errors.size());
        std::printf("[DIAG] open target: size=%llu write_time=%llu attributes_error=%lu identity_error=%lu volume=%llu file_id=",
            (static_cast<unsigned long long>(attributes.nFileSizeHigh) << 32) | attributes.nFileSizeLow,
            (static_cast<unsigned long long>(attributes.ftLastWriteTime.dwHighDateTime) << 32) |
                attributes.ftLastWriteTime.dwLowDateTime, attributes_error, identity_error,
            static_cast<unsigned long long>(identity.value.VolumeSerialNumber));
        for (const auto byte : identity.value.FileId.Identifier) std::printf("%02x", static_cast<unsigned>(byte));
        std::printf("\n");
        for (const auto& launch_error : launch_errors)
            std::printf("[DIAG] open probe Shell error: %s\n", utf8(launch_error).c_str());
    }
    return opened;
}
bool ShellErrorReachesRequester(AppState& requester, AppState& other,
                               const std::filesystem::path& missing) {
    requester.ops.OpenWith(missing.wstring());
    std::vector<std::wstring> requested_errors, other_errors;
    const auto deadline = GetTickCount64() + 10000;
    do {
        auto requested = requester.ops.DrainShellLaunchErrors();
        auto unrelated = other.ops.DrainShellLaunchErrors();
        requested_errors.insert(requested_errors.end(), requested.begin(), requested.end());
        other_errors.insert(other_errors.end(), unrelated.begin(), unrelated.end());
        if (!requested_errors.empty() || !other_errors.empty()) break;
        // The normal WM_OPS_NOTIFY handler drains these same mailboxes.
        Sleep(10);
    } while (GetTickCount64() < deadline);
    Pump();
    return requested_errors.size() == 1 && other_errors.empty();
}
bool Forward(HWND hwnd, const std::wstring& path) {
    COPYDATASTRUCT data{};
    data.dwData = pulse::app::SingleInstanceCoordinator::OpenPathMessageId();
    data.cbData = static_cast<DWORD>((path.size() + 1) * sizeof(wchar_t));
    data.lpData = const_cast<wchar_t*>(path.c_str());
    return SendMessageW(hwnd, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&data)) != 0;
}
POINT FindDropPoint(AppState& state, const std::function<bool(const ui::HitTestResult&)>& accepts) {
    const auto vm = BuildVm(state, false);
    const D2D1_RECT_F bounds = D2D1::RectF(0, 0,
        static_cast<float>(state.compositor.Width()),
        static_cast<float>(state.compositor.Height()));
    for (LONG y = 2; y < static_cast<LONG>(state.compositor.Height()); y += 6) {
        for (LONG x = 2; x < static_cast<LONG>(state.compositor.Width()); x += 6) {
            const auto hit = state.renderer.HitTest(vm, bounds,
                static_cast<float>(x), static_cast<float>(y));
            if (!accepts(hit)) continue;
            POINT point{x, y};
            ClientToScreen(state.hwnd, &point);
            return point;
        }
    }
    return POINT{-2147483647L, -2147483647L};
}
bool IsContentDrop(const ui::HitTestResult& hit) {
    return hit.region == ui::HitTestResult::Row || hit.region == ui::HitTestResult::Pane ||
        hit.region == ui::HitTestResult::PaneEmptyNewFolder;
}
app::LayoutTab* AddLoadedTab(AppState& state, const std::wstring& path) {
    auto& layout = state.window_tabs.NewTab(path);
    BindCurrentLayout(state);
    if (state.pane) StartLoadingPath(state, state.pane->view, path, PathLoadReason::Navigate);
    return &layout;
}

int RunHomeInteractionChecks() {
    const auto root = std::filesystem::absolute(L"bench_data/home-interaction") /
        std::to_wstring(GetCurrentProcessId());
    std::filesystem::create_directories(root / L"folder");
    storage::OverrideDefaultRootForTesting((root / L"profile").wstring());
    l10n::Initialize(GetModuleHandleW(nullptr), L"zh-CN");
    WNDCLASSW wc{};
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpfnWndProc = WndProc;
    wc.lpszClassName = app::SingleInstanceCoordinator::WindowClassName();
    RegisterClassW(&wc);
    AppState state;
    state.isolatedTest = true;
    const HWND primary = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_NOREDIRECTIONBITMAP,
        wc.lpszClassName, L"Pulse Home fixture", WS_OVERLAPPEDWINDOW & ~WS_CAPTION,
        -20000, -20000, 1080, 760, nullptr, nullptr, wc.hInstance, &state);
    Check(primary && ActiveTab(state), "Home interaction fixture initializes a production window");
    if (!primary || !ActiveTab(state)) return 1;
    const auto folder = fs::NormalizePath((root / L"folder").wstring());
    state.places.networks.push_back({L"Isolated card", folder});
    for (const float scale : {1.0f, 1.5f, 2.0f}) {
        SetWindowPos(primary, nullptr, 0, 0, static_cast<int>(1080 * scale),
            static_cast<int>(760 * scale), SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        NavigateTo(state, app::MakeHomePath());
        auto* tab = ActiveTab(state);
        tab->home_collapsed_mask = 3;
        tab->scroll_y = 0;
        state.scale = scale;
        state.renderer.SetScale(scale);
        auto vm = BuildVm(state, false);
        bool clicked = false;
        for (const auto& slot : vm.pane_slots) {
            for (const auto& card : ui::MakeHomeLayout(slot.pane, slot.rect, scale).cards) {
                if (slot.pane.home_cards[static_cast<size_t>(card.index)].path != folder) continue;
                const int x = static_cast<int>((card.rect.left + card.rect.right) * 0.5f);
                const int y = static_cast<int>((card.rect.top + card.rect.bottom) * 0.5f);
                const LPARAM position = MAKELPARAM(x, y);
                const auto hit = state.renderer.HitTest(vm,
                    D2D1::RectF(0, 0, static_cast<float>(state.compositor.Width()),
                        static_cast<float>(state.compositor.Height())), static_cast<float>(x), static_cast<float>(y));
                Check(hit.region == ui::HitTestResult::HomeCard && hit.path == folder,
                    "the click position hits the isolated Home card at the tested DPI");
                SendMessageW(primary, WM_LBUTTONDOWN, MK_LBUTTON, position);
                SendMessageW(primary, WM_LBUTTONUP, 0, position);
                Check(ActiveTab(state)->current_path == app::MakeHomePath(),
                    "a Home card single click stays on Home at the tested DPI");
                SendMessageW(primary, WM_LBUTTONDBLCLK, MK_LBUTTON, position);
                SendMessageW(primary, WM_LBUTTONUP, 0, position);
                Check(ActiveTab(state)->current_path == folder,
                    "a Home card double click opens its exact target at the tested DPI");
                clicked = true;
                break;
            }
            if (clicked) break;
        }
        Check(clicked, "isolated Home card is visible and hit-testable");
    }
    state.scale = 1.0f;
    state.renderer.SetScale(state.scale);
    NavigateTo(state, folder);
    Check(Wait([&] { return ActiveTab(state) && !ActiveTab(state)->loading; }),
        "native New fixture loads its isolated destination");
    ShowWindow(primary, SW_SHOWNOACTIVATE);
    Pump();
    wchar_t template_path[32768]{};
    DWORD template_path_bytes = sizeof(template_path);
    const auto template_value = RegGetValueW(HKEY_CLASSES_ROOT,
        L".docx\\Word.Document.12\\ShellNew", L"FileName", RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ,
        nullptr, template_path, &template_path_bytes);
    std::ifstream template_file(std::filesystem::path(template_path), std::ios::binary);
    const bool template_opened = template_value == ERROR_SUCCESS && template_file.is_open();
    const std::vector<char> template_contents{std::istreambuf_iterator<char>(template_file),
        std::istreambuf_iterator<char>()};
    const bool template_read = template_opened && !template_file.bad();
    Check(template_read, "read the installed Word ShellNew template including an empty registered template");
    std::vector<std::filesystem::path> original_documents;
    for (const auto& entry : std::filesystem::directory_iterator(root / L"folder")) {
        if (entry.path().extension() == L".docx") original_documents.push_back(entry.path());
    }
    state.context_menu.InvalidateCaches();
    bool native_seen = false, word_seen = false, word_clicked = false;
    auto deadline = GetTickCount64() + 12000;
    menu_timer_action = [&] {
        if (!state.menu || !state.menu->IsOpen()) return;
        const auto items = ui::FluentMenuTestPeer::Items(*state.menu);
        for (const auto& item : items) {
            if (item.command < app::CmdShellComBase || !item.enabled) continue;
            native_seen = true;
            word_seen |= item.text.find(L"Word") != std::wstring::npos;
            if (!word_clicked && item.text.find(L"Word") != std::wstring::npos) {
                word_clicked = ui::FluentMenuTestPeer::ClickCommand(*state.menu, item.command);
                return;
            }
        }
        if (GetTickCount64() >= deadline) state.menu->Dismiss();
    };
    UINT_PTR timer = SetTimer(nullptr, 0, 50, MenuTimer);
    Check(timer != 0, "start bounded native New observation");
    if (timer) ShowNewDropdown(state);
    KillTimer(nullptr, timer);
    Check(native_seen, "real Shell New templates appear in a cold toolbar dropdown");
    Check(word_seen, "installed Microsoft Word template appears in the real New dropdown");
    Check(word_clicked, "select the real native Word row through the toolbar menu mouse path");
    std::filesystem::path created_document;
    const bool word_created = word_clicked && Wait([&] {
        for (const auto& entry : std::filesystem::directory_iterator(root / L"folder")) {
            if (entry.is_regular_file() && entry.path().extension() == L".docx" &&
                std::find(original_documents.begin(), original_documents.end(), entry.path()) ==
                    original_documents.end()) {
                created_document = entry.path();
                return true;
            }
        }
        return false;
    });
    Check(word_created, "the native Word toolbar command creates a new DOCX in its isolated folder");
    std::vector<char> created_contents;
    const bool contents_match = template_read && word_created && Wait([&] {
        std::ifstream created_file(created_document, std::ios::binary);
        if (!created_file.is_open()) return false;
        created_contents.assign(std::istreambuf_iterator<char>(created_file), std::istreambuf_iterator<char>());
        return !created_file.bad() && created_contents == template_contents;
    });
    std::printf("[DIAG] native Word template bytes: expected=%zu actual=%zu, template_read=%d created=%d\n",
        template_contents.size(), created_contents.size(), template_read ? 1 : 0, word_created ? 1 : 0);
    Check(contents_match, "the created Word document matches the registered ShellNew template byte for byte");
    NavigateTo(state, app::MakeHomePath());
    bool home_menu = false;
    menu_timer_action = [&] {
        if (!state.menu || !state.menu->IsOpen()) return;
        const auto items = ui::FluentMenuTestPeer::Items(*state.menu);
        home_menu = items.size() == 1 && items.front().command == app::CmdRefresh;
        state.menu->Dismiss();
    };
    timer = SetTimer(nullptr, 0, 50, MenuTimer);
    if (timer) ShowBackgroundContextMenu(state, {100, 100});
    KillTimer(nullptr, timer);
    menu_timer_action = {};
    Check(home_menu, "real Home background menu excludes New and prior folder Shell items");
    RequestApplicationExit(state);
    Check(Wait([&] { return state.core.windows.Count() == 0; }), "Home fixture closes normally");
    std::printf("Home interaction: %d failures\n", failures);
    return failures ? 1 : 0;
}

int RunSessionChecks() {
    const auto root = std::filesystem::absolute(L"bench_data/window-session") /
        std::to_wstring(GetCurrentProcessId());
    std::filesystem::create_directories(root / L"folder");
    storage::OverrideDefaultRootForTesting((root / L"profile").wstring());
    l10n::Initialize(GetModuleHandleW(nullptr), L"zh-CN");
    WNDCLASSW wc{};
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpfnWndProc = WndProc;
    wc.lpszClassName = app::SingleInstanceCoordinator::WindowClassName();
    RegisterClassW(&wc);

    app::SessionSnapshot legacy;
    legacy.sidebar_hidden = 1 << static_cast<int>(app::SidebarSectionId::Tags);
    legacy.quick_access_hidden = 5;
    legacy.sidebar_collapsed = 2;
    legacy.sidebar_order = app::DefaultSidebarOrder();
    std::reverse(legacy.sidebar_order.begin(), legacy.sidebar_order.end());
    legacy.active_path = (root / L"folder").wstring();
    app::LayoutTabSnapshot old_tab;
    old_tab.panes.push_back({legacy.active_path});
    legacy.layout_tabs = {old_tab, old_tab};
    legacy.windows.assign(3, legacy);
    Check(app::SaveSession(legacy), "prepare an isolated legacy session with three windows and multiple tabs");
    for (int cycle = 0; cycle < 3; ++cycle) {
        app::WindowSessionSnapshot preferences;
        Check(app::LoadSessionPreferences(preferences) && preferences.active_path.empty() &&
            preferences.layout_tabs.empty() && preferences.tab_groups.empty() &&
            preferences.sidebar_hidden == legacy.sidebar_hidden &&
            preferences.quick_access_hidden == legacy.quick_access_hidden,
            "startup ignores saved navigation while retaining hidden sidebar sections and links");
        AppState state;
        state.isolatedTest = true;
        state.sidebarHiddenMask = static_cast<uint32_t>(preferences.sidebar_hidden);
        state.sidebarCollapsedMask = static_cast<uint32_t>(preferences.sidebar_collapsed);
        state.sidebarQuickAccessHiddenMask = static_cast<uint32_t>(preferences.quick_access_hidden);
        state.sidebarOrder = preferences.sidebar_order;
        const HWND primary = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_NOREDIRECTIONBITMAP,
            wc.lpszClassName, L"Pulse startup fixture", WS_OVERLAPPEDWINDOW & ~WS_CAPTION,
            -20000, -20000, 1080, 760, nullptr, nullptr, wc.hInstance, &state);
        Check(primary && Wait([&] { return ActiveTab(state) && !ActiveTab(state)->loading; }),
            "a default launch initializes a real window");
        if (!primary) return 1;
        Check(state.core.windows.Count() == 1 && state.window_tabs.items.size() == 1 &&
            ActiveTab(state)->current_path == app::MakeHomePath(),
            "each launch starts with exactly one Home window and one tab");
        state.appPrefs.keep_running_on_close = true;
        AddLoadedTab(state, (root / L"folder").wstring());
        SendMessageW(primary, WM_DETACH_LAYOUT_TAB, state.window_tabs.active, 0);
        auto* detached = state.core.windows.MostRecentExcept(primary);
        Check(detached && detached->sidebarHiddenMask == state.sidebarHiddenMask &&
            detached->sidebarCollapsedMask == state.sidebarCollapsedMask &&
            detached->sidebarQuickAccessHiddenMask == state.sidebarQuickAccessHiddenMask &&
            detached->sidebarOrder == state.sidebarOrder &&
            Wait([&] { return ActiveTab(*detached) && !ActiveTab(*detached)->loading; }) &&
            ActiveTab(*detached)->current_path == fs::NormalizePath((root / L"folder").wstring()),
            "detached tabs retain their folder and inherit the sidebar preferences");
        auto* external = PostOpenFolderInNewWindow(state, (root / L"folder").wstring());
        Check(external && Wait([&] { return ActiveTab(*external) && !ActiveTab(*external)->loading; }) &&
            ActiveTab(*external)->current_path == fs::NormalizePath((root / L"folder").wstring()),
            "an explicit external folder still opens at the requested location");
        state.core.windows.ForEach([](HWND, AppState& window) { window.isolatedTest = false; });
        RequestApplicationExit(state);
        Check(Wait([&] { return state.core.windows.Count() == 0; }),
            "application exit closes all three windows");
        std::wstring saved;
        Check(ReadUtf8File((root / L"profile" / L"session.json").wstring(), saved),
            "exit writes the isolated preferences file");
        const json::Document doc(saved);
        Check(doc.Is(json::Kind::Object) && !doc.Find(L"windows") && !doc.Find(L"layoutTabs") &&
            !doc.Find(L"path") && !doc.Find(L"activeTab") && !doc.Find(L"tabGroups"),
            "exit records no open windows, folders, tabs or groups");
        Check(app::LoadSessionPreferences(preferences) && preferences.sidebar_hidden == legacy.sidebar_hidden &&
            preferences.quick_access_hidden == legacy.quick_access_hidden &&
            preferences.sidebar_order == legacy.sidebar_order,
            "sidebar visibility and order survive exit and the next launch");
        Pump();
    }
    std::printf("window startup: %d failures\n", failures);
    return failures ? 1 : 0;
}

int RunActivationChecks() {
    HANDLE token = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        DWORD bytes = 0;
        GetTokenInformation(token, TokenIntegrityLevel, nullptr, 0, &bytes);
        std::vector<BYTE> label(bytes);
        if (GetTokenInformation(token, TokenIntegrityLevel, label.data(), bytes, &bytes)) {
            const auto sid = reinterpret_cast<TOKEN_MANDATORY_LABEL*>(label.data())->Label.Sid;
            std::printf("[DIAG] test integrity=%lu\n", *GetSidSubAuthority(sid, *GetSidSubAuthorityCount(sid) - 1));
        }
        CloseHandle(token);
    }
    const auto root = std::filesystem::absolute(L"bench_data/taskbar-activation") /
        std::to_wstring(GetCurrentProcessId());
    std::filesystem::create_directories(root / L"folder");
    storage::OverrideDefaultRootForTesting((root / L"profile").wstring());
    l10n::Initialize(GetModuleHandleW(nullptr), L"zh-CN");
    WNDCLASSW wc{};
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpfnWndProc = WndProc;
    wc.lpszClassName = app::SingleInstanceCoordinator::WindowClassName();
    RegisterClassW(&wc);
    AppState state;
    state.isolatedTest = true;
    state.open_path = (root / L"folder").wstring();
    const HWND primary = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_NOREDIRECTIONBITMAP,
        wc.lpszClassName, L"Pulse activation fixture", WS_OVERLAPPEDWINDOW & ~WS_CAPTION,
        -20000, -20000, 1080, 760, nullptr, nullptr, wc.hInstance, &state);
    Check(primary && Wait([&] { const auto* tab = ActiveTab(state); return tab && !tab->loading; }),
        "activation fixture initializes a production window with an isolated profile");
    if (!primary) return 1;
    state.appPrefs.keep_running_on_close = true;
    const auto tabs = state.window_tabs.items.size();
    const auto* active = ActiveTab(state);
    const auto sequence = state.external_open_issued_sequence;
    for (const auto mode : {app::ExternalOpenMode::Tab, app::ExternalOpenMode::ReuseOpenedLocation,
                            app::ExternalOpenMode::NewWindow}) {
        state.appPrefs.external_open_mode = mode;
        ShowWindow(primary, SW_SHOWNOACTIVATE);
        SendMessageW(primary, WM_CLOSE, 0, 0);
        Check(IsWindow(primary) && !IsWindowVisible(primary),
            "close-to-background hides the existing window without destroying it");
        Check(Forward(primary, L"") && IsWindowVisible(primary) && !IsIconic(primary),
            "a pathless relaunch restores the hidden window in every external-open mode");
        Check(state.core.windows.Count() == 1 && state.window_tabs.items.size() == tabs &&
            ActiveTab(state) == active && state.external_open_issued_sequence == sequence,
            "activation preserves tabs and does not enqueue navigation or create a window");
    }
    ShowWindow(primary, SW_MINIMIZE);
    Check(Forward(primary, L"") && IsWindowVisible(primary) && !IsIconic(primary),
        "a pathless relaunch restores a minimized window");
    Check(Forward(primary, L"") && IsWindowVisible(primary) && state.window_tabs.items.size() == tabs,
        "repeated activation leaves an already visible window open");

    state.appPrefs.external_open_mode = app::ExternalOpenMode::Tab;
    Check(Forward(primary, root.wstring()) && Wait([&] { return state.window_tabs.items.size() == tabs + 1; }),
        "a launch with a folder still follows the external-open policy");
    Wait([&] { return ActiveTab(state) && !ActiveTab(state)->loading; });
    auto* secondary = PostOpenFolderInNewWindow(state, state.open_path);
    Check(secondary && state.core.windows.Count() == 2, "prepare a second window for activation routing");
    if (secondary) {
        ShowWindow(primary, SW_HIDE);
        ShowWindow(secondary->hwnd, SW_HIDE);
        state.core.windows.Touch(secondary->hwnd);
        Check(Forward(primary, L"") && IsWindowVisible(secondary->hwnd) && !IsWindowVisible(primary),
            "activation delivered to another window restores the most recently used window");
        const auto* retained = ActiveTab(state);
        const auto retained_tabs = state.window_tabs.items.size();
        SendMessageW(secondary->hwnd, WM_CLOSE, 0, 0);
        ShowWindow(primary, SW_SHOWNOACTIVATE);
        SendMessageW(primary, WM_CLOSE, 0, 0);
        Check(!IsWindowVisible(primary) && !IsWindowVisible(secondary->hwnd) &&
            Forward(secondary->hwnd, L"") && IsWindowVisible(primary) &&
            !IsWindowVisible(secondary->hwnd) && ActiveTab(state) == retained &&
            state.window_tabs.items.size() == retained_tabs && state.core.windows.Count() == 2,
            "a background relaunch restores the last closed window with its original folder and tabs");
        SendMessageW(primary, WM_CLOSE, 0, 0);
        ShowWindow(secondary->hwnd, SW_SHOWNOACTIVATE);
        SendMessageW(secondary->hwnd, WM_CLOSE, 0, 0);
        SendMessageW(primary, app::TrayController::kCallbackMessage, 0, WM_LBUTTONUP);
        Check(IsWindowVisible(secondary->hwnd) && !IsWindowVisible(primary) &&
            state.core.windows.Count() == 2,
            "the tray icon also restores the last closed window rather than always its owner");
        state.core.exiting = true;
        Check(!Forward(primary, L""), "activation is rejected during application shutdown");
        state.core.exiting = false;
    }
    RequestApplicationExit(state);
    Check(Wait([&] { return state.core.windows.Count() == 0; }),
        "activation fixture shuts down all of its own windows");
    Pump();
    std::printf("taskbar activation: %d failures\n", failures);
    return failures ? 1 : 0;
}

struct CloseDialogAutomation {
    static inline CloseDialogAutomation* current = nullptr;
    bool accept = false;
    int confirmations = 0;
    UINT_PTR timer = 0;
    CloseDialogAutomation() {
        current = this;
        timer = SetTimer(nullptr, 0, 25, [](HWND, UINT, UINT_PTR, DWORD) {
            EnumThreadWindows(GetCurrentThreadId(), [](HWND window, LPARAM) -> BOOL {
                wchar_t name[64]{};
                GetClassNameW(window, name, ARRAYSIZE(name));
                if (wcscmp(name, L"PulseConfirmWindow") || !IsWindowVisible(window)) return TRUE;
                ++current->confirmations;
                if (current->accept) SendMessageW(window, WM_KEYDOWN, VK_TAB, 0);
                SendMessageW(window, WM_KEYDOWN, current->accept ? VK_RETURN : VK_ESCAPE, 0);
                return FALSE;
            }, 0);
        });
    }
    ~CloseDialogAutomation() { KillTimer(nullptr, timer); current = nullptr; }
};

std::optional<ops::ConflictItemInfo> StartBlockedCopy(AppState& state,
    const std::filesystem::path& root, const wchar_t* name) {
    const auto folder = root / name;
    std::filesystem::create_directories(folder / L"source");
    std::filesystem::create_directories(folder / L"destination");
    { std::ofstream source(folder / L"source" / L"item.txt"); source << "new contents"; }
    { std::ofstream destination(folder / L"destination" / L"item.txt"); destination << "old contents"; }
    ops::OpRequest request;
    request.type = ops::OpType::Copy;
    request.sources = {(folder / L"source" / L"item.txt").wstring()};
    request.dest_dir = (folder / L"destination").wstring();
    state.ops.Submit(std::move(request));
    // Hold the real transfer at a deterministic conflict before pumping its
    // notification, then suppress only that fixture's modal conflict prompt.
    const auto deadline = GetTickCount64() + 10000;
    std::optional<ops::ConflictItemInfo> conflict;
    while (!(conflict = state.ops.PendingConflict()) && GetTickCount64() < deadline) Sleep(10);
    if (conflict) state.conflictUiToken = conflict->token;
    return conflict;
}

int RunBackgroundChecks() {
    const auto root = std::filesystem::absolute(L"bench_data/operation-background") /
        std::to_wstring(GetCurrentProcessId());
    std::filesystem::create_directories(root / L"folder");
    storage::OverrideDefaultRootForTesting((root / L"profile").wstring());
    l10n::Initialize(GetModuleHandleW(nullptr), L"zh-CN");
    WNDCLASSW wc{};
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpfnWndProc = WndProc;
    wc.lpszClassName = app::SingleInstanceCoordinator::WindowClassName();
    RegisterClassW(&wc);
    AppState state;
    state.isolatedTest = true;
    state.open_path = (root / L"folder").wstring();
    const HWND primary = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_NOREDIRECTIONBITMAP,
        wc.lpszClassName, L"Pulse background fixture", WS_OVERLAPPEDWINDOW & ~WS_CAPTION,
        -20000, -20000, 1080, 760, nullptr, nullptr, wc.hInstance, &state);
    Check(primary && Wait([&] { return ActiveTab(state) && !ActiveTab(state)->loading; }),
        "background fixture initializes a real window with an isolated profile");
    if (!primary) return 1;
    auto* secondary = PostOpenFolderInNewWindow(state, state.open_path);
    Check(secondary && state.core.windows.Count() == 2, "background fixture registers two real windows");
    if (!secondary) { RequestApplicationExit(state); Wait([&] { return !IsWindow(primary); }); return 1; }
    CloseDialogAutomation automation;
    const auto first = StartBlockedCopy(state, root, L"first");
    const auto second = StartBlockedCopy(*secondary, root, L"second");
    Check(first && second, "both windows own real transfers waiting at independent conflicts");
    state.appPrefs.keep_running_on_close = true;
    state.appPrefs.global_search_enabled = false;
    ShowWindow(primary, SW_SHOWNOACTIVATE);
    SendMessageW(primary, WM_CLOSE, 0, 0);
    SendMessageW(secondary->hwnd, WM_NCLBUTTONDOWN, HTCLOSE, 0);
    Pump();
    std::printf("[DIAG] background close: confirmations=%d exits=%d/%d windows=%zu visible=%d/%d tray_owner=%p\n",
        automation.confirmations, state.exit_requested, secondary->exit_requested, state.core.windows.Count(),
        IsWindowVisible(primary) != FALSE, IsWindowVisible(secondary->hwnd) != FALSE,
        static_cast<void*>(state.core.global_ui_owner));
    Check(automation.confirmations == 0 && !state.exit_requested && !secondary->exit_requested &&
        state.core.windows.Count() == 2 && !IsWindowVisible(primary) && !IsWindowVisible(secondary->hwnd),
        "closing every background-enabled window hides its owner without prompting or shutdown");
    Check(state.ops.HasPendingFileOperations() && secondary->ops.HasPendingFileOperations(),
        "operations in both hidden windows retain their workers");
    if (first) state.ops.ResolveConflict(first->token, ops::ConflictChoice::Replace, false);
    if (second) secondary->ops.ResolveConflict(second->token, ops::ConflictChoice::Replace, false);
    Check(Wait([&] { return !state.ops.HasPendingFileOperations() && !secondary->ops.HasPendingFileOperations(); }) &&
        state.ops.Status().phase == ops::OpPhase::Completed && secondary->ops.Status().phase == ops::OpPhase::Completed,
        "both real transfers finish while every main window is hidden");
    for (const auto* name : {L"first", L"second"}) {
        std::ifstream destination(root / name / L"destination" / L"item.txt");
        std::string contents((std::istreambuf_iterator<char>(destination)), {});
        Check(contents == "new contents", "hidden transfer writes the expected destination bytes");
    }
    Check(!state.operationWindow->IsVisible() && !secondary->operationWindow->IsVisible(),
        "background transfer notifications keep routine progress popups hidden");

    const auto archive_done = std::make_shared<std::atomic_bool>(false);
    const auto archive_cancel = std::make_shared<std::atomic_bool>(false);
    secondary->archive_operations.push_back({archive_done, archive_cancel});
    RequestApplicationExit(state);
    Check(automation.confirmations == 1 && !archive_cancel->load() && !state.core.exiting,
        "an archive lifetime alone participates in application exit confirmation; decline preserves it");

    const auto third = StartBlockedCopy(state, root, L"third");
    Check(third.has_value(), "prepare pending work in the window being closed normally");
    state.appPrefs.keep_running_on_close = false;
    ShowWindow(primary, SW_SHOWNOACTIVATE);
    ShowWindow(secondary->hwnd, SW_SHOWNOACTIVATE);
    SendMessageW(primary, WM_CLOSE, 0, 0);
    Check(!IsWindowVisible(primary) && !state.exit_requested && automation.confirmations == 1,
        "a secondary close retains pending work without an interruption prompt when background preference is off");
    SendMessageW(secondary->hwnd, WM_CLOSE, 0, 0);
    Check(automation.confirmations == 2 && !state.core.exiting && !secondary->exit_requested &&
        state.ops.HasPendingFileOperations(),
        "closing the last visible window confirms exit across retained hidden operations; decline preserves them");
    automation.accept = true;
    RequestApplicationExit(*secondary);
    Check(automation.confirmations == 3 && state.core.exiting && archive_cancel->load(),
        "explicit application exit asks once for all hidden pending work");
    Check(Wait([&] { return secondary->exit_stop_finished.load(std::memory_order_acquire); }) &&
        IsWindow(secondary->hwnd) && !CompleteApplicationExit(*secondary),
        "confirmed exit cancels archives while retaining their owner until the extraction scope unwinds");
    archive_done->store(true);
    secondary->archive_operations.clear();
    PostMessageW(secondary->hwnd, WM_EXIT_READY, 0, 0);
    Check(Wait([&] { return state.core.windows.Count() == 0; }),
        "confirmed full exit cancels pending work and asynchronously destroys every retained window");
    Pump();
    std::printf("operation background: %d failures\n", failures);
    return failures ? 1 : 0;
}
}

int main(int argc, char** argv) {
    using namespace pulse;
    wchar_t executable_name[32768]{};
    const DWORD executable_length = GetModuleFileNameW(nullptr, executable_name, ARRAYSIZE(executable_name));
    if (!executable_length || executable_length >= ARRAYSIZE(executable_name)) return 2;
    const std::filesystem::path executable(executable_name);
    if (executable.filename() == L"pulse-open-marker.exe") {
        std::ofstream marker(executable.parent_path() / L"opened.marker", std::ios::binary | std::ios::trunc);
        marker << "opened";
        marker.close();
        return marker ? 0 : 2;
    }
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    compat::EnableDpiAwareness();
    if (FAILED(OleInitialize(nullptr))) return 2;
    if (argc == 2 && std::string(argv[1]) == "--home-only") {
        const int result = RunHomeInteractionChecks();
        OleUninitialize();
        return result;
    }
    if (argc == 2 && std::string(argv[1]) == "--session-only") {
        const int result = RunSessionChecks();
        OleUninitialize();
        return result;
    }
    if (argc == 2 && std::string(argv[1]) == "--activation-only") {
        const int result = RunActivationChecks();
        OleUninitialize();
        return result;
    }
    if (argc == 2 && std::string(argv[1]) == "--background-only") {
        const int result = RunBackgroundChecks();
        OleUninitialize();
        return result;
    }
    const auto root = std::filesystem::absolute(L"bench_data/release-1.1.3.0/multiwindow");
    std::filesystem::create_directories(root / L"first");
    std::filesystem::create_directories(root / L"second");
    std::filesystem::create_directories(root / L"open-probe");
    const auto open_probe = root / L"open-probe" / L"pulse-open-marker.exe";
    const auto batch_probe_path = root / L"open-probe" / L"pulse-open-marker.bat";
    std::error_code copy_error;
    std::filesystem::copy_file(executable, open_probe, std::filesystem::copy_options::overwrite_existing, copy_error);
    Check(!copy_error, "prepare a harmless real executable for file-open lifecycle checks");
    std::ofstream batch_probe(batch_probe_path, std::ios::binary | std::ios::trunc);
    batch_probe << "@echo off\r\n>\"%~dp0opened.marker\" echo opened\r\nexit\r\n";
    batch_probe.close();
    Check(static_cast<bool>(batch_probe), "prepare an isolated batch file for real Shell file-open lifecycle checks");
    std::ofstream(root / L"second" / L"reveal.txt") << "folder reveal fixture";
    storage::OverrideDefaultRootForTesting((root / L"profile").wstring());
    l10n::Initialize(GetModuleHandleW(nullptr), L"zh-CN");
    SetEnvironmentVariableW(L"PULSE_TEST_HIDDEN_SHOT", L"1");
    WNDCLASSW wc{};
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpfnWndProc = WndProc;
    wc.lpszClassName = app::SingleInstanceCoordinator::WindowClassName();
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
    AppState state;
    state.isolatedTest = true;
    state.open_path = (root / L"first").wstring();
    HWND primary = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_NOREDIRECTIONBITMAP,
        wc.lpszClassName, L"Pulse fixture", WS_OVERLAPPEDWINDOW & ~WS_CAPTION,
        -20000, -20000, 1080, 760, nullptr, nullptr, wc.hInstance, &state);
    Check(primary && Wait([&] { const auto* tab = ActiveTab(state); return tab && !tab->loading; }),
        "production window initializes with an isolated directory and configuration");
    if (!primary) { OleUninitialize(); return 1; }
    Check(!copy_error && OpenProbe(state, open_probe),
        "the first real window opens a file through its operations manager");

    BYTE saved_keyboard[256]{};
    BYTE drag_keyboard[256]{};
    const bool keyboard_saved = GetKeyboardState(saved_keyboard) != FALSE;
    std::copy(std::begin(saved_keyboard), std::end(saved_keyboard), std::begin(drag_keyboard));
    drag_keyboard[VK_LBUTTON] |= 0x80;
    const bool keyboard_set = keyboard_saved && SetKeyboardState(drag_keyboard);
    state.tabDragPending = true;
    state.tabDragIndex = 0;
    state.tabDragStartPt = POINT{100, 20};
    if (keyboard_set) HandleMouseMove(&state, primary, WM_MOUSEMOVE, MK_LBUTTON,
        MAKELPARAM(100, 20 + GetSystemMetrics(SM_CYDRAG) + 2));
    Check(keyboard_set && state.tabDragging && state.tabOrder.size() == 1,
        "a single tab starts a vertical drag without moving the physical mouse");
    if (keyboard_saved) SetKeyboardState(saved_keyboard);
    HandleCaptureChanged(&state, primary, WM_CAPTURECHANGED, 0, 0);
    Check(!state.tabDragging && !state.tabDragPending,
        "losing capture clears the single-tab drag state");

    state.appPrefs.external_open_mode = app::ExternalOpenMode::Tab;
    const size_t before = state.window_tabs.items.size();
    Check(Forward(primary, (root / L"second").wstring()) &&
        Wait([&] { return state.window_tabs.items.size() == before + 1; }) && state.core.windows.Count() == 1,
        "external tab mode opens another tab in the existing window");
    Wait([&] { return ActiveTab(state) && !ActiveTab(state)->loading; });
    const size_t after_first_open = state.window_tabs.items.size();
    Check(Forward(primary, (root / L"second").wstring()) &&
        Wait([&] { return state.window_tabs.items.size() == after_first_open + 1; }),
        "explicit tab mode creates a new tab even when the folder is already open");
    Wait([&] { return ActiveTab(state) && !ActiveTab(state)->loading; });
    state.appPrefs.external_open_mode = app::ExternalOpenMode::ReuseOpenedLocation;
    const size_t with_second = state.window_tabs.items.size();
    Check(Forward(primary, (root / L"first").wstring()) &&
        Wait([&] { return ActiveTab(state)->current_path == fs::NormalizePath((root / L"first").wstring()); }) &&
        state.window_tabs.items.size() == with_second,
        "reuse mode activates the existing tab without adding a duplicate");

    state.appPrefs.external_open_mode = app::ExternalOpenMode::NewWindow;
    Check(Forward(primary, (root / L"second" / L"reveal.txt").wstring()) &&
        Wait([&] { return state.core.windows.Count() == 2; }),
        "external new-window mode creates a second real Pulse window");
    AppState* secondary = state.core.windows.MostRecentExcept(primary);
    if (secondary) {
        ShowWindow(secondary->hwnd, SW_HIDE);
        Wait([&] { return ActiveTab(*secondary) && !ActiveTab(*secondary)->loading; });
        Check(!copy_error && OpenProbe(*secondary, open_probe),
            "a second live window opens a file through its own operations manager");
        Check(ShellErrorReachesRequester(*secondary, state, root / L"open-probe" / L"missing.exe"),
            "a failed file open reaches the requesting second window instead of the first window");
        Check(Wait([&] {
            const auto* tab = ActiveTab(*secondary);
            return tab && tab->selected_index >= 0 &&
                tab->EntryAt(static_cast<size_t>(tab->selected_index)).name == L"reveal.txt";
        }), "opening a file location in a new window selects the requested file");
        ui::ComPtr<ID2D1Device> first_device, second_device;
        state.compositor.Dc()->GetDevice(&first_device);
        secondary->compositor.Dc()->GetDevice(&second_device);
        Check(state.compositor.Dc() != secondary->compositor.Dc() &&
            first_device.get() == second_device.get() &&
            &state.renderer.Caches() == &secondary->renderer.Caches() &&
            state.renderer.SharedThumbnailCache() != secondary->renderer.SharedThumbnailCache(),
            "real windows share the graphics device and icons while keeping thumbnail cancellation independent");
        Check(&state.appPrefs == &secondary->appPrefs && &state.places == &secondary->places &&
            &state.ctxMenuPrefs == &secondary->ctxMenuPrefs && &state.tray == &secondary->tray &&
            &state.searchHistory == &secondary->searchHistory &&
            &state.searchHistoryWriter == &secondary->searchHistoryWriter &&
            &state.savedSearches == &secondary->savedSearches,
            "all windows edit the same preferences, places and staging tray");
        secondary->appPrefs.external_open_mode = app::ExternalOpenMode::ReuseOpenedLocation;
        const size_t count = secondary->window_tabs.items.size();
        Check(Forward(secondary->hwnd, (root / L"first").wstring()) &&
            Wait([&] { return state.core.windows.MostRecent() == &state; }) && secondary->window_tabs.items.size() == count &&
            state.window_tabs.items.size() == with_second,
            "an outside open can activate a matching tab in another window");

        std::vector<AppState*> extra_windows;
        state.appPrefs.external_open_mode = app::ExternalOpenMode::NewWindow;
        for (int i = 0; i < 3; ++i) {
            const auto expected = state.core.windows.Count() + 1;
            const bool accepted = Forward(primary, (root / L"second").wstring());
            const bool created = Wait([&] { return state.core.windows.Count() == expected; });
            AppState* extra = created ? state.core.windows.MostRecent() : nullptr;
            Check(accepted && created && extra && Wait([&] {
                const auto* tab = ActiveTab(*extra);
                return tab && !tab->loading && tab->snapshot && tab->EntryCount() != 0;
            }), "each of five simultaneous windows can finish its directory listing");
            if (extra && extra != &state && extra != secondary) {
                ShowWindow(extra->hwnd, SW_HIDE);
                extra_windows.push_back(extra);
            }
        }
        for (AppState* extra : extra_windows) RequestWindowClose(*extra);
        Check(Wait([&] { return state.core.windows.Count() == 2; }),
            "closing extra windows releases their workers while the first pair remains usable");
        Check(!copy_error && OpenProbe(state, open_probe) && OpenProbe(*secondary, open_probe),
            "both original windows can still open files after other windows close");
        ShowWindow(primary, SW_HIDE);
        const size_t source_tabs = state.window_tabs.items.size();
        const size_t destination_tabs = secondary->window_tabs.items.size();
        app::LayoutTab* moved_layout = state.window_tabs.items.back().get();
        Check(TransferLayoutTab(state, source_tabs - 1, *secondary) &&
            state.window_tabs.items.size() == source_tabs - 1 &&
            secondary->window_tabs.items.size() == destination_tabs + 1 &&
            secondary->window_tabs.Active() == moved_layout &&
            Wait([&] { return ActiveTab(*secondary) && !ActiveTab(*secondary)->loading; }),
            "a layout tab moves between real windows and resumes loading in its destination");
        ApplyLayoutPreset(state, app::LayoutPreset::TwoVertical);
        const size_t source_panes = Panes(state).size();
        const size_t destination_panes = Panes(*secondary).size();
        app::Pane* moved_pane = Panes(state).back().get();
        Check(TransferPane(state, source_panes - 1, *secondary) &&
            Panes(state).size() == source_panes - 1 &&
            Panes(*secondary).size() == destination_panes + 1 &&
            secondary->pane == moved_pane &&
            Wait([&] { return !moved_pane->view.loading; }),
            "one split pane moves independently without moving the other source pane");

        SetWindowPos(primary, nullptr, -20000, -20000, 1080, 760,
            SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        SetWindowPos(secondary->hwnd, nullptr, -17000, -20000, 1080, 760,
            SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        AddLoadedTab(*secondary, (root / L"first").wstring());
        app::LayoutTab* split_tab = AddLoadedTab(state, (root / L"first").wstring());
        ApplyLayoutPreset(state, app::LayoutPreset::TwoVertical);
        Check(Wait([&] {
            return std::all_of(split_tab->panes.begin(), split_tab->panes.end(),
                [](const auto& pane) { return pane && !pane->view.loading; });
        }), "prepare a loaded two-pane layout tab for content-area drag routing");
        std::vector<app::Pane*> split_pointers;
        for (const auto& pane : Panes(state)) split_pointers.push_back(pane.get());
        const size_t before_whole_drop = Panes(*secondary).size();
        const size_t source_tabs_before_whole_drop = state.window_tabs.items.size();
        const POINT content_drop = FindDropPoint(*secondary, IsContentDrop);
        const size_t split_index = static_cast<size_t>(std::find_if(state.window_tabs.items.begin(),
            state.window_tabs.items.end(), [&](const auto& tab) { return tab.get() == split_tab; }) -
            state.window_tabs.items.begin());
        Check(content_drop.x != -2147483647L &&
            TryTransferLayoutTabAtScreenPoint(state, split_index, content_drop) &&
            state.window_tabs.items.size() == source_tabs_before_whole_drop - 1 &&
            Panes(*secondary).size() == before_whole_drop + 2 &&
            Panes(*secondary)[before_whole_drop].get() == split_pointers[0] &&
            Panes(*secondary)[before_whole_drop + 1].get() == split_pointers[1] &&
            Wait([&] {
                return std::all_of(Panes(*secondary).begin(), Panes(*secondary).end(),
                    [](const auto& pane) { return pane && !pane->view.loading; });
            }), "dropping a multi-pane tab on content preserves every pane and joins the target split");

        app::LayoutTab* strip_tab = AddLoadedTab(state, (root / L"second").wstring());
        const size_t strip_index = static_cast<size_t>(std::find_if(state.window_tabs.items.begin(),
            state.window_tabs.items.end(), [&](const auto& tab) { return tab.get() == strip_tab; }) -
            state.window_tabs.items.begin());
        const POINT strip_drop = FindDropPoint(*secondary, [](const ui::HitTestResult& hit) {
            return hit.region == ui::HitTestResult::Tab;
        });
        const size_t destination_tabs_before_strip = secondary->window_tabs.items.size();
        Check(strip_drop.x != -2147483647L &&
            TryTransferLayoutTabAtScreenPoint(state, strip_index, strip_drop) &&
            secondary->window_tabs.items.size() == destination_tabs_before_strip + 1 &&
            secondary->window_tabs.Active()->panes.size() == 1 &&
            Wait([&] { return !ActiveTab(*secondary)->loading; }),
            "dropping a tab on the target tab strip preserves it as a separate tab");

        const size_t detached_tab_count = state.core.windows.Count() + 1;
        const size_t detach_tab_index = state.window_tabs.active;
        Check(TryTransferLayoutTabAtScreenPoint(state, detach_tab_index, POINT{30000, 30000}) &&
            Wait([&] { return state.core.windows.Count() == detached_tab_count; }),
            "dropping a tab outside all windows creates a detached top-level window");
        AppState* detached_tab_window = state.core.windows.MostRecent();
        Check(detached_tab_window && detached_tab_window != &state &&
            detached_tab_window->window_tabs.items.size() == 1 &&
            Wait([&] { return ActiveTab(*detached_tab_window) && !ActiveTab(*detached_tab_window)->loading; }),
            "the detached window contains the moved tab and finishes loading");
        if (detached_tab_window && detached_tab_window != &state && detached_tab_window != secondary) {
            const HWND detached_hwnd = detached_tab_window->hwnd;
            Check(!copy_error && OpenProbe(*detached_tab_window, open_probe),
                "a detached tab window can open a file before its tab returns");
            const size_t tabs_before_return = state.window_tabs.items.size();
            Check(TransferLayoutTab(*detached_tab_window, 0, state) &&
                state.window_tabs.items.size() == tabs_before_return + 1 &&
                Wait([&] { return !IsWindow(detached_hwnd) && state.core.windows.Count() == 2; }),
                "returning a detached window's only tab closes its empty source window");
        }
        Check(Wait([&] { return state.core.windows.Count() == 2; }),
            "a detached tab window closes without affecting the root pair");
        Check(!copy_error && OpenProbe(state, open_probe) && OpenProbe(*secondary, open_probe),
            "the receiving and other live windows still open files after a detached tab returns");

        ApplyLayoutPreset(state, app::LayoutPreset::TwoVertical);
        const size_t detached_pane_count = state.core.windows.Count() + 1;
        const size_t detached_pane_index = Panes(state).size() - 1;
        Check(TryTransferPaneAtScreenPoint(state, state.window_tabs.active, detached_pane_index,
                POINT{30000, 30000}) &&
            Wait([&] { return state.core.windows.Count() == detached_pane_count; }),
            "dropping a pane outside all windows creates a detached top-level window");
        AppState* detached_pane_window = state.core.windows.MostRecent();
        Check(detached_pane_window && detached_pane_window != &state &&
            Panes(state).size() == 1 && Panes(*detached_pane_window).size() == 1 &&
            Wait([&] { return !ActiveTab(*detached_pane_window)->loading; }),
            "pane detachment replaces the new window starter and leaves one source pane");
        if (detached_pane_window) RequestWindowClose(*detached_pane_window);
        Check(Wait([&] { return state.core.windows.Count() == 2; }),
            "a detached pane window closes without affecting the root pair");

        auto merge_target = std::make_unique<ResolvedOpenTarget>();
        merge_target->folder = (root / L"first").wstring();
        auto* merge_source = reinterpret_cast<AppState*>(SendMessageW(primary, WM_NEW_WINDOW, 0,
            reinterpret_cast<LPARAM>(merge_target.release())));
        if (merge_source) {
            ShowWindow(merge_source->hwnd, SW_HIDE);
            app::LayoutTab* merge_pinned = AddLoadedTab(*merge_source, (root / L"first").wstring());
            merge_pinned->pinned = true;
            app::LayoutTab* merge_unpinned = AddLoadedTab(*merge_source, (root / L"second").wstring());
            const auto moved_merge_tabs = std::vector<app::LayoutTab*>{
                merge_source->window_tabs.items[0].get(), merge_pinned, merge_unpinned};
            if (!merge_source->window_tabs.items.empty()) {
                auto* target_first = secondary->window_tabs.items.front().get();
                target_first->pinned = true;
                const size_t old_target_count = secondary->window_tabs.items.size();
                const size_t expected_windows = state.core.windows.Count() - 1;
                POINT title_drop{100, 8};
                ClientToScreen(secondary->hwnd, &title_drop);
                Check(TryMergeWindowAtScreenPoint(*merge_source, title_drop) &&
                    secondary->window_tabs.items.size() == old_target_count + moved_merge_tabs.size() &&
                    std::all_of(moved_merge_tabs.begin(), moved_merge_tabs.end(), [&](auto* tab) {
                        return std::any_of(secondary->window_tabs.items.begin(), secondary->window_tabs.items.end(),
                            [&](const auto& candidate) { return candidate.get() == tab; });
                    }) && std::is_partitioned(secondary->window_tabs.items.begin(),
                        secondary->window_tabs.items.end(), [](const auto& tab) { return tab->pinned; }) &&
                    Wait([&] { return state.core.windows.Count() == expected_windows; }),
                    "dropping a window title bar merges every tab and keeps pinned tabs before unpinned tabs");
            } else {
                Check(false, "title-bar merge source has tabs");
                RequestWindowClose(*merge_source);
            }
        } else {
            Check(false, "title-bar merge source window is created");
        }

        OpenSettingsTab(*secondary, 0);
        secondary->shot.active = true;
        for (const auto* language : {L"zh-CN", L"en-US"}) {
            l10n::SetLanguage(language);
            for (bool narrow : {false, true}) {
                SetThemeMode(*secondary, narrow ? 2 : 1);
                Check(state.darkMode == narrow && secondary->darkMode == narrow,
                    "theme changes update every live window");
                const int width = narrow ? 720 : 1180;
                SetWindowPos(secondary->hwnd, nullptr, 0, 0, width, 820,
                    SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
                secondary->compositor.Resize(width, 820);
                const auto vm = BuildVm(*secondary, false);
                const auto offset = secondary->renderer.SettingsDestinationOffset(vm,
                    static_cast<int>(l10n::StringId::SettingsExternalOpen),
                    static_cast<float>(width), 820);
                secondary->settings.SetScroll(offset, secondary->renderer.SettingsMaxScroll(vm,
                    static_cast<float>(width), 820));
                Render(*secondary);
                const auto image = root / (std::wstring(L"settings-") + language +
                    (narrow ? L"-narrow.png" : L"-wide.png"));
                Check(secondary->compositor.SaveSnapshot(image.c_str()),
                    "save a rendered external-open setting in the actual window");
            }
        }
        secondary->shot.active = false;
        RequestWindowClose(state);
        Check(Wait([&] { return !IsWindow(primary) && state.core.windows.Count() == 1; }),
            "closing the first window keeps the second window and process alive");
        Check(!copy_error && OpenProbe(*secondary, open_probe),
            "the surviving window still opens files after the original Shell owner closes");
        secondary->appPrefs.external_open_mode = app::ExternalOpenMode::Tab;
        const size_t surviving_tabs = secondary->window_tabs.items.size();
        Check(Forward(secondary->hwnd, (root / L"first").wstring()) &&
            Wait([&] { return secondary->window_tabs.items.size() == surviving_tabs + 1; }),
            "the surviving window still accepts outside folder opens");
        RequestWindowClose(*secondary);
        Check(Wait([&] { return state.core.windows.Count() == 0; }),
            "closing the final window completes its asynchronous shutdown");
    } else {
        Check(false, "second window is registered");
        RequestWindowClose(state);
        Wait([&] { return state.core.windows.Count() == 0; });
    }
    Pump();
    SetEnvironmentVariableW(L"PULSE_TEST_HIDDEN_SHOT", nullptr);
    std::printf("multiwindow application: %d failures\n", failures);
    OleUninitialize();
    return failures ? 1 : 0;
}
