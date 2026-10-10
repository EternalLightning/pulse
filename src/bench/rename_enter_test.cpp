#include "../app/app_internal.h"
#include "../app/app_hosted_edit.h"
#include "../common/localization.h"
#include "../common/path_utils.h"
#include <filesystem>
#include <iostream>

namespace {
struct CharProbe {
    int dispatched_controls = 0;
    int native_controls = 0;
    int native_text = 0;
};

LRESULT CALLBACK NativeEditProbe(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                                UINT_PTR, DWORD_PTR data) {
    auto& probe = *reinterpret_cast<CharProbe*>(data);
    if (msg == WM_CHAR) {
        if (wp == VK_RETURN || wp == VK_ESCAPE) {
            ++probe.native_controls;
            // Count the regression without making the test itself beep.
            return 0;
        }
        ++probe.native_text;
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

LRESULT CALLBACK DispatchProbe(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                              UINT_PTR, DWORD_PTR data) {
    auto& probe = *reinterpret_cast<CharProbe*>(data);
    if (msg == WM_CHAR && (wp == VK_RETURN || wp == VK_ESCAPE)) ++probe.dispatched_controls;
    return DefSubclassProc(hwnd, msg, wp, lp);
}

bool ProbeEditor(pulse::AppState& state, CharProbe& probe, bool native_fallback) {
    const auto edit = state.hwndRenameEdit;
    if (!edit || !RemoveWindowSubclass(edit, pulse::RenameEditProc, 1)) return false;
    // The production subclass must run before the probe at the native EDIT boundary.
    if (!SetWindowSubclass(edit, NativeEditProbe, 2, reinterpret_cast<DWORD_PTR>(&probe)) ||
        !SetWindowSubclass(edit, pulse::RenameEditProc, 1, reinterpret_cast<DWORD_PTR>(&state)) ||
        !SetWindowSubclass(edit, DispatchProbe, 3, reinterpret_cast<DWORD_PTR>(&probe))) return false;
    return !native_fallback || SetPropW(edit, L"Pulse.NativeEditFallback", reinterpret_cast<HANDLE>(1));
}

void Pump() {
    MSG msg{};
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

bool PressKey(HWND edit, UINT key) {
    const LPARAM down = 1 | (static_cast<LPARAM>(MapVirtualKeyW(key, MAPVK_VK_TO_VSC)) << 16);
    if (!PostMessageW(edit, WM_KEYDOWN, key, down) ||
        !PostMessageW(edit, WM_KEYUP, key, down | (static_cast<LPARAM>(3) << 30))) return false;
    Pump();
    return true;
}

bool WaitForRename(pulse::AppState& state) {
    const auto start = GetTickCount64();
    while (state.ops.HasPendingFileOperations() && GetTickCount64() - start < 5000) {
        Pump();
        Sleep(5);
    }
    if (state.ops.HasPendingFileOperations()) return false;
    const auto status = state.ops.Status();
    if (!status.last_error.empty()) std::wcerr << L"rename error: " << status.last_error << L'\n';
    return status.last_error.empty();
}
}

int main() {
    using namespace pulse;
    OleInitialize(nullptr);
    l10n::Initialize(GetModuleHandleW(nullptr), L"en-US");
    int failures = 0;
    const auto check = [&](bool passed, const char* label) {
        std::cout << (passed ? "[PASS] " : "[FAIL] ") << label << std::endl;
        failures += !passed;
        return passed;
    };
    std::error_code error;
    const auto fixture = std::filesystem::absolute(std::filesystem::path(L"bench_data") /
        (L"rename-enter-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64())));
    if (!check(std::filesystem::create_directory(fixture, error), "create isolated rename fixture")) {
        OleUninitialize();
        return 1;
    }

    auto owned = std::make_unique<AppState>();
    auto& state = *owned;
    state.isolatedTest = true;
    state.appPrefs.persist = state.places.persist = state.searchHistory.persist = state.ctxMenuPrefs.persist = false;
    state.hwnd = CreateWindowExW(0, L"STATIC", L"Rename Enter fixture", WS_OVERLAPPEDWINDOW,
        -30000, -30000, 1000, 700, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (check(state.hwnd && state.compositor.Init(state.hwnd), "initialize isolated rename compositor")) {
        state.compositor.RecreateTextFormats(state.scale);
        state.renderer.SetCompositor(&state.compositor);
        state.window_tabs.NewTab(fixture.wstring());
        state.pane = state.window_tabs.Active()->FocusedPane();
        auto& tab = *ActiveTab(state);
        ShowWindow(state.hwnd, SW_SHOWNOACTIVATE);
        state.ops.Start([] {});
        for (bool native_fallback : {false, true}) {
            std::cout << "[INFO] " << (native_fallback ? "native fallback" : "default presentation") << std::endl;
            const std::wstring suffix = native_fallback ? L"native.txt" : L"default.txt";
            const auto source = fixture / (L"before-" + suffix);
            const auto target = fixture / (L"enter-" + suffix);
            const auto cancelled = fixture / (L"cancelled-" + suffix);
            const auto blurred = fixture / (L"blurred-" + suffix);
            HANDLE file = CreateFileW(source.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (!check(file != INVALID_HANDLE_VALUE, "create real rename source")) continue;
            CloseHandle(file);
            CharProbe probe;
            const auto open = [&](const std::filesystem::path& current) {
                fs::DirEntry entry;
                entry.name = current.filename().wstring();
                tab.SetSnapshot(std::make_shared<std::vector<fs::DirEntry>>(1, entry));
                tab.loading = false;
                tab.SelectOnly(0);
                ShowRenameOverlay(state);
                probe = {};
                return check(state.renameIndex == 0 && IsWindowVisible(state.hwndRenameEdit) &&
                    ProbeEditor(state, probe, native_fallback), "open production rename editor with native boundary probe");
            };
            const auto closed = [&] {
                return state.renameIndex == -1 && !IsWindowVisible(state.hwndRenameEdit) &&
                    !state.renameIgnoreKillFocus && GetFocus() == state.hwnd;
            };
            const auto completion = [&](const std::filesystem::path& from, const std::filesystem::path& to,
                                        uint64_t previous_count) {
                const bool finished = WaitForRename(state);
                const auto completed = state.ops.DrainCompletions();
                const bool passed = finished && state.ops.Status().completed_ops == previous_count + 1 &&
                    completed.size() == 1 && completed.front().type == ops::OpType::Rename &&
                    completed.front().sources.size() == 1 &&
                    path::StripExtendedPathPrefix(completed.front().sources.front()) == from.wstring() &&
                    std::filesystem::exists(to) && !std::filesystem::exists(from);
                if (!passed) {
                    std::cout << "[INFO] rename finished=" << finished << " completions=" << completed.size()
                        << " operations=" << previous_count << "->" << state.ops.Status().completed_ops
                        << " source_exists=" << std::filesystem::exists(from)
                        << " target_exists=" << std::filesystem::exists(to) << std::endl;
                    if (!completed.empty() && !completed.front().sources.empty())
                        std::wcerr << L"completed source: " << completed.front().sources.front()
                            << L" expected: " << from.wstring() << L'\n';
                }
                return passed;
            };
            if (open(source)) {
                SendMessageW(state.hwndRenameEdit, WM_CHAR, L'Q', 1);
                wchar_t text[128]{};
                GetWindowTextW(state.hwndRenameEdit, text, ARRAYSIZE(text));
                check(probe.native_text == 1 && wcscmp(text, L"Q.txt") == 0,
                    "ordinary text still reaches native EDIT and preserves extension");
                SetWindowTextW(state.hwndRenameEdit, target.filename().c_str());
                const auto before = state.ops.Status().completed_ops;
                check(PressKey(state.hwndRenameEdit, VK_RETURN) && closed(), "Enter closes editor and restores focus");
                std::cout << "[INFO] Enter WM_CHAR dispatched=" << probe.dispatched_controls
                    << " native=" << probe.native_controls << std::endl;
                check(probe.dispatched_controls == 1 && probe.native_controls == 0,
                    "translated Enter character is consumed before native EDIT can beep");
                check(completion(source, target, before), "Enter commits exactly one real disk rename");
            }
            if (open(target)) {
                const auto before = state.ops.Status().completed_ops;
                check(PressKey(state.hwndRenameEdit, VK_RETURN) && closed(), "unchanged Enter closes editor");
                check(probe.dispatched_controls == 1 && probe.native_controls == 0 &&
                    !state.ops.HasPendingFileOperations() && state.ops.Status().completed_ops == before,
                    "unchanged Enter consumes character without submitting an operation");
            }
            if (open(target)) {
                SetWindowTextW(state.hwndRenameEdit, cancelled.filename().c_str());
                const auto before = state.ops.Status().completed_ops;
                check(PressKey(state.hwndRenameEdit, VK_ESCAPE) && closed(), "Escape cancels editor and restores focus");
                check(probe.dispatched_controls == 1 && probe.native_controls == 0 &&
                    !state.ops.HasPendingFileOperations() && state.ops.Status().completed_ops == before &&
                    std::filesystem::exists(target) && !std::filesystem::exists(cancelled),
                    "Escape consumes character and preserves the original file");
            }
            if (open(target)) {
                SetWindowTextW(state.hwndRenameEdit, blurred.filename().c_str());
                const auto before = state.ops.Status().completed_ops;
                SetFocus(state.hwnd);
                check(closed(), "losing focus closes rename editor");
                check(completion(target, blurred, before), "losing focus still commits exactly one real disk rename");
            }
            HideRenameOverlay(state, false);
            if (state.hwndRenameEdit) DestroyWindow(state.hwndRenameEdit);
            state.hwndRenameEdit = nullptr;
            for (const auto& path : {source, target, cancelled, blurred}) std::filesystem::remove(path, error);
        }
        state.ops.Stop();
    }
    state.renderer.SetCompositor(nullptr);
    state.compositor.Shutdown();
    if (state.hwnd) DestroyWindow(state.hwnd);
    state.hwnd = state.hwndRenameEdit = nullptr;
    owned.reset();
    check(std::filesystem::remove(fixture, error), "clean isolated fixture");
    OleUninitialize();
    std::cout << "failures=" << failures << '\n';
    return failures ? 1 : 0;
}
