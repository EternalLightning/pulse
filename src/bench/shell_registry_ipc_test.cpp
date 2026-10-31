// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "../app/shell_window_registry.h"
#include "../ipc/shell_query_client.h"
#include <shlobj.h>
#include <exdisp.h>
#include <servprov.h>
#include <shlwapi.h>
#include <wrl/client.h>
#include <atomic>
#include <cstring>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <thread>

namespace {
using Microsoft::WRL::ComPtr;
constexpr UINT kSelectionMessage = WM_APP + 44;
constexpr uint64_t kPaneKey = 1001;
int failures = 0;
unsigned notifications = 0;

void Check(bool passed, const char* label) {
    std::printf("[%s] %s\n", passed ? "PASS" : "FAIL", label);
    if (!passed) ++failures;
}

LRESULT CALLBACK OwnerProcedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == kSelectionMessage) { ++notifications; return 0; }
    return DefWindowProcW(window, message, wparam, lparam);
}

void Pump() {
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&message); DispatchMessageW(&message);
    }
}

bool Until(const std::function<bool()>& ready, DWORD timeout_ms = 8000) {
    const auto deadline = GetTickCount64() + timeout_ms;
    do {
        Pump();
        if (ready()) return true;
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 20, QS_ALLINPUT);
    } while (GetTickCount64() < deadline);
    return false;
}

ComPtr<IWebBrowserApp> FindOwnBrowser(IShellWindows* windows, HWND owner) {
    long count = 0;
    if (FAILED(windows->get_Count(&count))) return {};
    for (long index = 0; index < count; ++index) {
        VARIANT position{};
        position.vt = VT_I4;
        position.lVal = index;
        ComPtr<IDispatch> dispatch;
        ComPtr<IWebBrowserApp> browser;
        SHANDLE_PTR handle = 0;
        if (SUCCEEDED(windows->Item(position, &dispatch)) && dispatch && SUCCEEDED(dispatch.As(&browser)) &&
            SUCCEEDED(browser->get_HWND(&handle)) && reinterpret_cast<HWND>(handle) == owner) return browser;
    }
    return {};
}

std::wstring LocationPath(IWebBrowserApp* browser) {
    BSTR url = nullptr;
    if (!browser || FAILED(browser->get_LocationURL(&url))) return {};
    wchar_t path[32768]{};
    DWORD length = ARRAYSIZE(path);
    const HRESULT result = url ? PathCreateFromUrlW(url, path, &length, 0) : E_FAIL;
    SysFreeString(url);
    return SUCCEEDED(result) ? std::wstring(path) : std::wstring();
}

std::wstring LocationName(IWebBrowserApp* browser) {
    BSTR name = nullptr;
    if (!browser || FAILED(browser->get_LocationName(&name))) return {};
    std::wstring result = name ? name : L"";
    SysFreeString(name);
    return result;
}

std::vector<uint8_t> ChildPidl(const std::wstring& path) {
    pulse::ipc::ShellQueryResult result;
    std::thread prepare([&] {
        pulse::ipc::ShellQueryRequest request;
        request.kind = pulse::ipc::ShellQueryKind::ParsePidl;
        request.path = path;
        pulse::ipc::QueryShell(request, result);
    });
    prepare.join();
    if (FAILED(result.error) || !pulse::ipc::ValidShellPidl(result.pidl)) return {};
    size_t offset = 0, last = 0;
    while (offset + sizeof(uint16_t) < result.pidl.size()) {
        uint16_t size = 0;
        std::memcpy(&size, result.pidl.data() + offset, sizeof(size));
        if (!size) break;
        last = offset;
        offset += size;
    }
    return {result.pidl.begin() + last, result.pidl.end()};
}

ComPtr<IShellView> SelectionView(IWebBrowserApp* browser) {
    ComPtr<IServiceProvider> provider;
    ComPtr<IShellView> view;
    if (browser && SUCCEEDED(browser->QueryInterface(IID_PPV_ARGS(&provider))))
        provider->QueryService(IID_IShellView, IID_PPV_ARGS(&view));
    return view;
}

bool SelectOwnFile(pulse::app::ShellWindowRegistry& registry, IShellView* view,
                   const std::vector<uint8_t>& child, const std::wstring& folder, const std::wstring& file) {
    constexpr SVSIF flags = SVSI_SELECT | SVSI_FOCUSED | SVSI_ENSUREVISIBLE;
    const HRESULT selected = view->SelectItem(reinterpret_cast<PCUITEMID_CHILD>(child.data()), flags);
    if (FAILED(selected)) {
        std::printf("SelectItem HRESULT=0x%08lX\n", static_cast<unsigned long>(selected));
        return false;
    }
    std::vector<pulse::app::ShellSelectRequest> requests;
    const bool delivered = Until([&] {
        requests = registry.DrainSelections();
        return !requests.empty();
    }, 2000);
    return delivered && requests.size() == 1 && requests.front().key == kPaneKey &&
        requests.front().folder == folder && requests.front().path == file && requests.front().flags == flags;
}

void RegistryFixture() {
    namespace fs = std::filesystem;
    struct Fixture {
        fs::path root;
        std::wstring class_name;
        HWND owner = nullptr;
        pulse::app::ShellWindowRegistry registry;
        ~Fixture() {
            registry.Stop();
            if (owner) DestroyWindow(owner);
            if (!class_name.empty()) UnregisterClassW(class_name.c_str(), GetModuleHandleW(nullptr));
            std::error_code error;
            if (!root.empty()) fs::remove_all(root, error);
            Check(!error, "isolated registry files are cleaned");
        }
    } fixture;
    fixture.root = fs::current_path() / L"bench_data" /
        (L"shell_registry_" + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(GetTickCount64()));
    const auto initial = fixture.root / L"initial_中文";
    const auto next = fixture.root / L"next_中文";
    fs::create_directories(initial);
    fs::create_directories(next);
    const auto first_file = initial / L"first_中文.txt";
    const auto next_file = next / L"next_中文.txt";
    for (const auto& file : {first_file, next_file}) {
        HANDLE handle = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        Check(handle != INVALID_HANDLE_VALUE, "isolated selection fixture exists");
        if (handle == INVALID_HANDLE_VALUE) return;
        CloseHandle(handle);
    }
    const auto first_child = ChildPidl(first_file.wstring());
    const auto next_child = ChildPidl(next_file.wstring());
    Check(!first_child.empty() && !next_child.empty(), "selection fixture PIDLs are prepared by the helper process");
    if (first_child.empty() || next_child.empty()) return;

    const auto class_name = L"PulseShellRegistryFixture-" + std::to_wstring(GetCurrentProcessId());
    WNDCLASSW owner_class{};
    owner_class.lpfnWndProc = OwnerProcedure;
    owner_class.hInstance = GetModuleHandleW(nullptr);
    owner_class.lpszClassName = class_name.c_str();
    if (!RegisterClassW(&owner_class)) { Check(false, "register isolated owner window class"); return; }
    fixture.class_name = class_name;
    fixture.owner = CreateWindowExW(0, class_name.c_str(), L"Pulse Shell registry fixture", WS_OVERLAPPED,
        0, 0, 100, 100, nullptr, nullptr, owner_class.hInstance, nullptr);
    const bool started = fixture.owner && fixture.registry.Start(fixture.owner, kSelectionMessage);
    Check(started, "start production registry with its own hidden HWND");
    if (!started) return;
    ComPtr<IShellWindows> windows;
    const HRESULT activation = CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&windows));
    if (FAILED(activation)) std::printf("ShellWindows activation HRESULT=0x%08lX\n", static_cast<unsigned long>(activation));
    Check(SUCCEEDED(activation) && windows, "connect to the live out-of-process ShellWindows service");
    if (!windows) return;
    fixture.registry.Publish({{kPaneKey, initial.wstring()}});
    ComPtr<IWebBrowserApp> browser;
    const bool registered = Until([&] {
        browser = FindOwnBrowser(windows.Get(), fixture.owner);
        return browser && LocationPath(browser.Get()) == initial.wstring();
    });
    if (!registered) std::printf("registry startup HRESULT=0x%08lX\n", static_cast<unsigned long>(fixture.registry.LastError()));
    Check(registered, "enumerate only the fixture HWND's published browser registration");
    if (!registered) return;
    Check(LocationName(browser.Get()) == initial.filename().wstring(), "registered COM getter returns the helper's cached Unicode location name");
    Check(LocationPath(browser.Get()) == initial.wstring(), "registered COM URL getter roundtrips to the isolated folder");
    auto initial_view = SelectionView(browser.Get());
    Check(initial_view.Get() != nullptr, "registered browser exposes its IServiceProvider Shell selection view");
    if (!initial_view) return;
    Check(SelectOwnFile(fixture.registry, initial_view.Get(), first_child, initial.wstring(), first_file.wstring()),
        "live COM SelectItem callback resolves the child PIDL through IPC and delivers the exact selection");
    Check(notifications > 0, "selection delivery posts only to the fixture owner HWND");

    fixture.registry.Publish({{kPaneKey, next.wstring()}});
    ComPtr<IWebBrowserApp> next_browser;
    const bool navigated = Until([&] {
        next_browser = FindOwnBrowser(windows.Get(), fixture.owner);
        return next_browser && LocationPath(next_browser.Get()) == next.wstring();
    });
    Check(navigated, "navigation replaces the own browser registration with the next folder identity");
    if (!navigated) return;
    Check(LocationName(next_browser.Get()) == next.filename().wstring() && LocationName(browser.Get()).empty(),
        "navigation publishes fresh cached getters and retires the old browser identity");
    Check(initial_view->SelectItem(reinterpret_cast<PCUITEMID_CHILD>(first_child.data()), SVSI_SELECT) ==
        HRESULT_FROM_WIN32(ERROR_CANCELLED) && fixture.registry.DrainSelections().empty(),
        "retired live COM selection view cannot deliver an old folder selection");
    auto next_view = SelectionView(next_browser.Get());
    Check(next_view && SelectOwnFile(fixture.registry, next_view.Get(), next_child, next.wstring(), next_file.wstring()),
        "replacement live COM view delivers selections using the new folder PIDL");
    initial_view.Reset(); next_view.Reset(); browser.Reset(); next_browser.Reset();
    fixture.registry.Stop();
    Check(Until([&] { return !FindOwnBrowser(windows.Get(), fixture.owner); }, 4000),
        "Stop revokes the fixture registration before its owner HWND is destroyed");
    Check(fixture.registry.DrainSelections().empty(), "stopped registry retains no pending fixture selections");
}
}

int wmain() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (FAILED(initialized)) return 2;
    CoEnableCallCancellation(nullptr);
    HANDLE finished = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!finished) { CoUninitialize(); return 3; }
    const DWORD owner_thread = GetCurrentThreadId();
    std::atomic_bool deadline_expired = false;
    std::thread watchdog([finished, owner_thread, &deadline_expired] {
        const HRESULT watchdog_com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (WaitForSingleObject(finished, 45000) == WAIT_TIMEOUT) {
            deadline_expired.store(true);
            std::printf("[FAIL] live registry fixture exceeded its whole-process COM deadline\n");
            CoCancelCall(owner_thread, 0);
            if (WaitForSingleObject(finished, 2000) == WAIT_TIMEOUT) TerminateProcess(GetCurrentProcess(), ERROR_TIMEOUT);
        }
        if (SUCCEEDED(watchdog_com)) CoUninitialize();
    });
    try { RegistryFixture(); }
    catch (const std::exception& error) { ++failures; std::printf("[FAIL] fixture exception: %s\n", error.what()); }
    SetEvent(finished); watchdog.join(); CloseHandle(finished);
    CoDisableCallCancellation(nullptr);
    CoUninitialize();
    std::printf("Shell registry IPC: failures=%d\n", failures);
    return failures || deadline_expired.load() ? 1 : 0;
}
