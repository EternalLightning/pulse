// SPDX-License-Identifier: Apache-2.0
#include "../common/localization.h"
#include "../common/windows_compat.h"
#include "../ui/file_picker_dialog.h"
#include "../ui/ui_renderer.h"
#include "animation_snapshot.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <ole2.h>

namespace {
constexpr wchar_t kSizeKey[] = L"Software\\Pulse";
constexpr wchar_t kSizeValue[] = L"OpenFilePickerSizeDip";

struct IsolatedRegistry {
    std::filesystem::path directory;
    HKEY hive = nullptr;
    bool redirected = false;

    ~IsolatedRegistry() {
        if (redirected) RegOverridePredefKey(HKEY_CURRENT_USER, nullptr);
        if (hive) RegCloseKey(hive);
        if (!directory.empty()) {
            std::error_code error;
            std::filesystem::remove_all(directory, error);
        }
    }

    LSTATUS Open() {
        const auto path = std::filesystem::temp_directory_path() /
            (L"Pulse-picker-size-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
        if (!std::filesystem::create_directory(path)) return ERROR_ALREADY_EXISTS;
        directory = path;
        auto status = RegLoadAppKeyW((directory / L"test.hiv").c_str(), &hive, KEY_ALL_ACCESS, REG_PROCESS_APPKEY, 0);
        if (status) return status;
        HKEY marker = nullptr;
        status = RegCreateKeyExW(hive, L"PickerSizeFixture", 0, nullptr, 0, KEY_READ, nullptr, &marker, nullptr);
        if (status) return status;
        RegCloseKey(marker);
        status = RegOverridePredefKey(HKEY_CURRENT_USER, hive);
        redirected = status == ERROR_SUCCESS;
        if (!redirected) return status;
        status = RegOpenKeyExW(HKEY_CURRENT_USER, L"PickerSizeFixture", 0, KEY_READ, &marker);
        if (!status) RegCloseKey(marker);
        return status;
    }
};

bool SameSize(SIZE left, SIZE right) { return left.cx == right.cx && left.cy == right.cy; }

SIZE WindowSize(HWND window) {
    RECT bounds{};
    GetWindowRect(window, &bounds);
    return {bounds.right - bounds.left, bounds.bottom - bounds.top};
}

bool RunFileViewFeedback(const std::filesystem::path& output) {
    using namespace pulse::ui;
    std::filesystem::create_directories(output);
    SetEnvironmentVariableW(L"PULSE_TEST_HIDDEN_SHOT", L"1");
    const HWND window = CreateWindowExW(WS_EX_NOACTIVATE, L"STATIC", L"Picker feedback fixture", WS_POPUP,
        0, 0, 780, 540, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!window) return false;
    Compositor compositor;
    MainRenderer renderer;
    bool passed = compositor.Init(window, true);
    if (passed) {
        compositor.RecreateTextFormats(1.0f);
        for (const auto mode : {ViewMode::Details, ViewMode::MediumIcons}) {
            renderer.SetCompositor(&compositor);
            renderer.SetScale(1.0f);
            PaneViewModel vm;
            vm.view_mode = mode;
            vm.path = L"C:\\Picker fixture";
            vm.is_file_system = false;
            ListEntryView entry;
            entry.name = L"file.txt";
            entry.path = vm.path + L"\\" + entry.name;
            vm.entries.push_back(entry);
            const bool motion = renderer.MotionEnabled() && !IsHighContrast();
            const auto theme = MakeTheme(true, HexColor(0x0078D4));
            const auto bounds = D2D1::RectF(20, 20, 760, 410);
            const auto prefix = mode == ViewMode::Details ? L"details-" : L"icons-";
            fluent::Painter painter(&compositor);
            painter.SetScale(1.0f);
            const auto paint = [&](int row, float focus, const wchar_t* frame = nullptr) {
                vm.hover_index = row;
                auto* dc = compositor.Dc();
                dc->BeginDraw();
                dc->Clear(theme.surface_card);
                renderer.DrawEmbeddedFileView(vm, bounds, theme, true, 0);
                painter.BeginFrame(theme);
                fluent::ControlState field;
                field.focused = focus > 0.0f;
                field.focus_progress = focus;
                painter.DrawTextFieldFrame(D2D1::RectF(20, 445, 760, 477), field, true);
                const bool ok = SUCCEEDED(dc->EndDraw()) && (!frame || WriteAnimationSnapshot(compositor,
                    output / (std::wstring(prefix) + frame + L".png")));
                passed &= ok;
            };
            paint(-1, 0.0f, L"rest");
            paint(0, 0.0f);
            const bool entered = renderer.SurfaceAnimating() == motion;
            Sleep(45);
            paint(0, 0.5f, L"middle");
            const bool middle = renderer.SurfaceAnimating() == motion;
            Sleep(190);
            paint(0, 1.0f, L"end");
            const bool settled = !renderer.SurfaceAnimating();
            paint(-1, 1.0f);
            Sleep(40);
            paint(0, 0.5f);
            const bool reversed = renderer.SurfaceAnimating() == motion;
            Sleep(190);
            paint(0, 1.0f);
            const bool ok = entered && middle && settled && reversed && !renderer.SurfaceAnimating();
            std::printf("[%s] picker %s hover enters, interpolates, reverses and stops (motion %s)\n",
                ok ? "PASS" : "FAIL", mode == ViewMode::Details ? "details" : "icons", motion ? "enabled" : "disabled");
            passed &= ok;
        }
    }
    renderer.SetCompositor(nullptr);
    compositor.Shutdown();
    DestroyWindow(window);
    return passed;
}
}

int main(int argc, char** argv) {
    using namespace pulse;
    compat::EnableDpiAwareness();
    IsolatedRegistry registry;
    if (const auto status = registry.Open()) {
        std::printf("[FAIL] initialize isolated registry (%ld)\n", status);
        return 2;
    }
    if (FAILED(OleInitialize(nullptr))) return 2;
    l10n::Initialize(GetModuleHandleW(nullptr), L"en-US");
    if (argc == 3 && std::string_view(argv[1]) == "--feedback-only") {
        const bool passed = RunFileViewFeedback(argv[2]);
        OleUninitialize();
        return passed ? 0 : 1;
    }
    MONITORINFO monitor{sizeof(monitor)};
    const HMONITOR target_monitor = MonitorFromWindow(nullptr, MONITOR_DEFAULTTONEAREST);
    UINT monitor_dpi = 0, vertical_dpi = 0;
    if (!GetMonitorInfoW(target_monitor, &monitor) ||
        FAILED(GetDpiForMonitor(target_monitor, MDT_EFFECTIVE_DPI, &monitor_dpi, &vertical_dpi)) || !monitor_dpi) {
        OleUninitialize();
        return 2;
    }
    const auto dpi = static_cast<int>(monitor_dpi);
    const auto on_screen = [&](SIZE dip) -> SIZE {
        return {std::min<LONG>(MulDiv(dip.cx, dpi, 96), monitor.rcWork.right - monitor.rcWork.left),
            std::min<LONG>(MulDiv(dip.cy, dpi, 96), monitor.rcWork.bottom - monitor.rcWork.top)};
    };
    const auto open = [&](ui::PickerMode mode, SIZE new_size = {}, bool user_resize = false, UINT simulated_dpi = 0) {
        SIZE initial{};
        ui::FilePickerSpec spec;
        spec.mode = mode;
        spec.initial_path = registry.directory.wstring();
        spec.software_device = true;
        spec.on_ready = [&](HWND window) {
            initial = WindowSize(window);
            if (new_size.cx > 0) {
                RECT bounds{};
                GetWindowRect(window, &bounds);
                if (simulated_dpi)
                    SendMessageW(window, WM_DPICHANGED, MAKEWPARAM(simulated_dpi, simulated_dpi),
                        reinterpret_cast<LPARAM>(&bounds));
                SendMessageW(window, WM_ENTERSIZEMOVE, 0, 0);
                bounds.right = bounds.left + new_size.cx;
                bounds.bottom = bounds.top + new_size.cy;
                if (user_resize) SendMessageW(window, WM_SIZING, WMSZ_BOTTOMRIGHT, reinterpret_cast<LPARAM>(&bounds));
                SetWindowPos(window, nullptr, 0, 0, new_size.cx, new_size.cy,
                    SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
                SendMessageW(window, WM_EXITSIZEMOVE, 0, 0);
            }
            PostMessageW(window, WM_CLOSE, 0, 0);
        };
        ui::FilePickerResult result;
        ui::ShowFilePicker(nullptr, spec, false, {0.0f, 0.47f, 0.83f, 1.0f}, result);
        return initial;
    };
    const auto seed = [&](SIZE size) {
        return RegSetKeyValueW(registry.hive, kSizeKey, kSizeValue, REG_BINARY, &size, sizeof(size)) == ERROR_SUCCESS;
    };
    int failures = 0;
    const auto check = [&](bool ok, const char* label) {
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
        failures += !ok;
    };
    const auto resized = on_screen({880, 620});
    const auto default_open = on_screen({1120, 760});
    check(SameSize(open(ui::PickerMode::Open, resized, true), default_open),
        "first Open dialog uses the larger default");
    check(SameSize(open(ui::PickerMode::Open, on_screen({740, 510})), resized),
        "a subsequent Open dialog restores the completed manual resize");
    check(SameSize(open(ui::PickerMode::Open), resized),
        "programmatic size changes do not overwrite the remembered size");
    check(SameSize(open(ui::PickerMode::Save, on_screen({760, 520}), true), on_screen({1000, 680})) &&
        SameSize(open(ui::PickerMode::Open), resized), "Save mode has its own default and does not change Open");
    open(ui::PickerMode::Open, {1050, 720}, true, 144);
    check(SameSize(open(ui::PickerMode::Open), on_screen({700, 480})),
        "a resize at 150 percent DPI is saved in logical units");
    SIZE oversized{32000, 32000}, stored{};
    DWORD bytes = sizeof(stored);
    check(seed(oversized) && SameSize(open(ui::PickerMode::Open), on_screen(oversized)) &&
        RegGetValueW(registry.hive, kSizeKey, kSizeValue, RRF_RT_REG_BINARY,
            nullptr, &stored, &bytes) == ERROR_SUCCESS && SameSize(oversized, stored),
        "restoring on a smaller work area fits without overwriting the saved size");
    check(seed({0, 630}) && SameSize(open(ui::PickerMode::Open), default_open),
        "an invalid saved size falls back to the default");
    OleUninitialize();
    return failures ? 1 : 0;
}
