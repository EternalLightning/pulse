#include "../app/explorer_takeover_policy.h"
#include "../app/explorer_window_takeover.h"
#include <windows.h>
#include <objbase.h>
#include <ocidl.h>
#include <oleauto.h>
#include <exdisp.h>
#include <shldisp.h>
#include <shlwapi.h>
#include <wrl/client.h>
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <set>
#include <string>
#include <vector>

namespace {
using Microsoft::WRL::ComPtr;
using pulse::app::ExplorerTakeoverRequest;
using pulse::app::ExplorerWindowTakeover;
namespace policy = pulse::app::explorer_takeover;
int failures = 0;
int limits = 0;
constexpr UINT kWake = WM_APP + 83;
void Check(bool value, const char* label) {
    std::printf("[%s] %s\n", value ? "PASS" : "FAIL", label);
    if (!value) ++failures;
}
void Limit(const char* label) { ++limits; std::printf("[LIMIT] %s\n", label); }
void Pump() {
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}
bool Until(const std::function<bool()>& predicate, DWORD timeout = 8000) {
    const auto end = GetTickCount64() + timeout;
    do {
        Pump();
        if (predicate()) return true;
        Sleep(20);
    } while (GetTickCount64() < end);
    return false;
}
void Pause(DWORD duration) {
    const auto end = GetTickCount64() + duration;
    while (GetTickCount64() < end) { Pump(); Sleep(10); }
}
policy::SourceObservation HealthySource() {
    policy::SourceObservation source;
    source.same_identity = source.single_tab = source.supported = true;
    source.same_folder = source.same_selection = source.input_unchanged = true;
    source.view_age_ms = 1000;
    return source;
}
void PolicyCases() {
    using policy::PendingDecision;
    policy::TabObservation tab{true, true, 1, 1};
    Check(policy::IsSingleTabSafe(tab), "one Shell view and one native tab authorize a single-tab source");
    tab.native_tab_count = 2;
    Check(!policy::IsSingleTabSafe(tab), "two native tabs reject whole-window closure even with one Shell view");
    tab.native_tab_count = 0;
    Check(!policy::IsSingleTabSafe(tab), "unknown Windows 11 tab structure fails closed");
    tab.native_tab_count = 1; tab.shell_view_count = 2;
    Check(!policy::IsSingleTabSafe(tab), "multiple enumerated views reject whole-window closure");
    tab.shell_view_count = 1; tab.enumeration_complete = false;
    Check(!policy::IsSingleTabSafe(tab), "incomplete Shell enumeration cannot authorize closure");
    tab = {false, true, 1, 0};
    Check(policy::IsSingleTabSafe(tab), "legacy single-view Explorer does not require the Windows 11 native tab class");
    auto source = HealthySource();
    Check(policy::DecidePending(source) == PendingDecision::Wait &&
        policy::CanCloseAfterAcknowledgment(source, true, false), "unchanged single-tab source may close after a successful ACK");
    Check(!policy::CanCloseAfterAcknowledgment(source, false, false), "failed ACK preserves the source");
    Check(!policy::CanCloseAfterAcknowledgment(source, true, true), "Stop prevents closure even after successful delivery");
    struct Guard { bool policy::SourceObservation::* member; const char* label; };
    for (const Guard guard : {
            Guard{&policy::SourceObservation::same_identity, "changed HWND/process creation identity retires the request"},
            Guard{&policy::SourceObservation::single_tab, "a tab added during handoff cancels the pending whole-window close"},
            Guard{&policy::SourceObservation::supported, "unsupported or unreadable source preserves Explorer"},
            Guard{&policy::SourceObservation::same_folder, "active navigation cancels the pending request"},
            Guard{&policy::SourceObservation::input_unchanged, "new user input cancels pending takeover"}}) {
        auto changed = source;
        changed.*(guard.member) = false;
        Check(policy::DecidePending(changed) == PendingDecision::Retire &&
            !policy::CanCloseAfterAcknowledgment(changed, true, false), guard.label);
    }
    auto changed = source; changed.shift = true;
    Check(policy::DecidePending(changed) == PendingDecision::Retire &&
        !policy::CanCloseAfterAcknowledgment(changed, true, false), "Shift bypass also rejects a stale successful ACK");
    changed = source; changed.same_selection = false;
    changed.submitted_empty_selection = changed.current_selection_nonempty = true;
    Check(policy::DecidePending(changed) == PendingDecision::Retire,
        "late selection in a visible source preserves Explorer rather than guessing user intent");
    changed.visible = false;
    Check(policy::DecidePending(changed) == PendingDecision::Replace &&
        !policy::CanCloseAfterAcknowledgment(changed, true, false),
        "bounded unexposed empty-to-selection update replaces delivery without authorizing the old ACK");
    changed.view_age_ms = policy::kLateSelectionGraceMs + 1;
    Check(policy::DecidePending(changed) == PendingDecision::Retire, "late selection beyond the grace period retires the request");
    changed.view_age_ms = 1000; changed.submitted_empty_selection = false;
    Check(policy::DecidePending(changed) == PendingDecision::Retire, "changing an existing selection never enters the late-selection retry");
    changed.submitted_empty_selection = true; changed.current_selection_nonempty = false;
    Check(policy::DecidePending(changed) == PendingDecision::Retire, "deselecting items is not a late initial selection");
    changed.current_selection_nonempty = true; changed.input_unchanged = false;
    Check(policy::DecidePending(changed) == PendingDecision::Retire, "input changes reject even the narrow unexposed late-selection retry");
    Check(!policy::CanSubmit(449, 1000, true) && policy::CanSubmit(450, 450, true),
        "selected source retains the full 450 ms stability boundary");
    Check(!policy::CanSubmit(450, 799, false) && policy::CanSubmit(450, 800, false),
        "initial empty selection waits for the 800 ms selection grace");
    Check(policy::NextScanDelay(false) == 1000 && policy::NextScanDelay(true) == 80,
        "empty watcher uses low-frequency scans while a candidate uses bounded short polling");
}
struct Identity {
    HWND hwnd = nullptr;
    DWORD process = 0;
    uint64_t created = 0;
    bool operator==(const Identity&) const = default;
};
bool ReadIdentity(HWND window, Identity& identity) {
    wchar_t name[64]{};
    if (!GetClassNameW(window, name, 64) || wcscmp(name, L"CabinetWClass")) return false;
    DWORD process = 0;
    GetWindowThreadProcessId(window, &process);
    HANDLE handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process);
    if (!handle) return false;
    wchar_t image[32768]{}; DWORD length = 32768;
    FILETIME created{}, exited{}, kernel{}, user{};
    const bool valid = QueryFullProcessImageNameW(handle, 0, image, &length) &&
        GetProcessTimes(handle, &created, &exited, &kernel, &user) &&
        _wcsicmp(PathFindFileNameW(image), L"explorer.exe") == 0;
    CloseHandle(handle);
    if (valid) identity = {window, process, (static_cast<uint64_t>(created.dwHighDateTime) << 32) | created.dwLowDateTime};
    return valid;
}
BOOL CALLBACK CollectCabinets(HWND window, LPARAM data) {
    wchar_t name[64]{};
    if (GetClassNameW(window, name, 64) && wcscmp(name, L"CabinetWClass") == 0)
        reinterpret_cast<std::set<HWND>*>(data)->insert(window);
    return TRUE;
}
BOOL CALLBACK CountNativeTabs(HWND window, LPARAM data) {
    wchar_t name[128]{};
    if (GetClassNameW(window, name, 128) && wcscmp(name, L"ShellTabWindowClass") == 0)
        ++*reinterpret_cast<unsigned*>(data);
    return TRUE;
}
std::wstring FolderPath(IWebBrowser2* browser) {
    ComPtr<IDispatch> document;
    ComPtr<IShellFolderViewDual> view;
    ComPtr<Folder> folder;
    ComPtr<Folder2> folder2;
    ComPtr<FolderItem> self;
    if (FAILED(browser->get_Document(&document)) || !document || FAILED(document.As(&view)) ||
        FAILED(view->get_Folder(&folder)) || !folder || FAILED(folder.As(&folder2)) ||
        FAILED(folder2->get_Self(&self)) || !self) return {};
    BSTR text = nullptr;
    if (FAILED(self->get_Path(&text)) || !text) return {};
    std::wstring path(text, SysStringLen(text));
    SysFreeString(text);
    return path;
}
bool SamePath(const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) == 0; }
class OwnedSource {
public:
    ComPtr<IWebBrowser2> browser;
    Identity identity;
    std::wstring folder;
    std::vector<std::wstring> permitted_folders;
    bool SameIdentity() const { Identity current; return ReadIdentity(identity.hwnd, current) && current == identity; }
    void Trace(const char* stage, HRESULT operation = S_OK) const {
        BSTR url = nullptr;
        VARIANT_BOOL busy = VARIANT_FALSE;
        const HRESULT location_hr = browser->get_LocationURL(&url);
        const HRESULT busy_hr = browser->get_Busy(&busy);
        const std::wstring current = FolderPath(browser.Get());
        std::printf("[DIAG] stage=%s operation=0x%08lX visible=%d busy_hr=0x%08lX busy=%d "
            "location_hr=0x%08lX URL=%ls folder=%ls\n", stage, static_cast<unsigned long>(operation),
            IsWindowVisible(identity.hwnd) != FALSE, static_cast<unsigned long>(busy_hr), busy != VARIANT_FALSE,
            static_cast<unsigned long>(location_hr), url ? url : L"", current.c_str());
        if (url) SysFreeString(url);
    }
    bool Create(const std::wstring& path) {
        std::set<HWND> existing;
        EnumWindows(CollectCabinets, reinterpret_cast<LPARAM>(&existing));
        HRESULT hr = CoCreateInstance(CLSID_ShellBrowserWindow, nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&browser));
        std::printf("[DIAG] isolated ShellBrowserWindow activation=0x%08lX\n", static_cast<unsigned long>(hr));
        if (FAILED(hr) || !browser) { Limit("new-window COM activation is unavailable; no existing Explorer is used as a fallback"); return false; }
        SHANDLE_PTR handle = 0;
        if (!Until([&] { return SUCCEEDED(browser->get_HWND(&handle)) && handle; }, 3000)) {
            Limit("new COM browser did not expose a verifiable HWND; no navigation or close was attempted"); return false;
        }
        HWND root = GetAncestor(reinterpret_cast<HWND>(handle), GA_ROOT);
        if (existing.contains(root) || !ReadIdentity(root, identity)) {
            Limit("COM browser did not prove a new Explorer identity; no navigation or close was attempted");
            identity = {}; return false;
        }
        folder = path; permitted_folders = {path};
        std::printf("[DIAG] fixture HWND=%p pid=%lu creation=%llu path=%ls\n", static_cast<void*>(root), identity.process,
            static_cast<unsigned long long>(identity.created), folder.c_str());
        Trace("new-identity-verified");
        SetWindowPos(root, nullptr, -20000, -20000, 900, 600, SWP_NOACTIVATE | SWP_NOZORDER);
        const HRESULT shown = browser->put_Visible(VARIANT_TRUE);
        Trace("show-new-source", shown);
        if (FAILED(shown)) { Limit("verified new Explorer could not initialize its visible view"); return false; }
        if (!Navigate(path)) { Limit("isolated new Explorer did not finish navigation"); return false; }
        return Until([&] { return SameIdentity() && IsWindowVisible(root); }, 3000);
    }
    bool Navigate(const std::wstring& path) {
        if (!SameIdentity()) return false;
        VARIANT url{};
        url.vt = VT_BSTR; url.bstrVal = SysAllocString(path.c_str());
        VARIANT missing{};
        missing.vt = VT_ERROR; missing.scode = DISP_E_PARAMNOTFOUND;
        const HRESULT hr = browser->Navigate2(&url, &missing, &missing, &missing, &missing);
        VariantClear(&url);
        const bool ready = SUCCEEDED(hr) && Until([&] { return SamePath(FolderPath(browser.Get()), path); });
        Trace(ready ? "navigation-ready" : "navigation-not-ready", hr);
        return ready;
    }
    bool Select(const std::wstring& path) {
        if (!SameIdentity() || !SamePath(FolderPath(browser.Get()), folder)) return false;
        ComPtr<IDispatch> document;
        ComPtr<IShellFolderViewDual> view;
        ComPtr<Folder> current;
        ComPtr<FolderItem> selected;
        if (FAILED(browser->get_Document(&document)) || !document || FAILED(document.As(&view)) ||
            FAILED(view->get_Folder(&current)) || !current) return false;
        BSTR name = SysAllocString(PathFindFileNameW(path.c_str()));
        const HRESULT parsed = current->ParseName(name, &selected);
        SysFreeString(name);
        if (FAILED(parsed) || !selected) return false;
        VARIANT item{}; item.vt = VT_DISPATCH; item.pdispVal = selected.Get(); item.pdispVal->AddRef();
        const HRESULT hr = view->SelectItem(&item, 1 | 4 | 8 | 16);
        VariantClear(&item);
        return SUCCEEDED(hr);
    }
    ~OwnedSource() {
        if (!identity.hwnd || !SameIdentity()) return;
        const std::wstring current = FolderPath(browser.Get());
        if (std::none_of(permitted_folders.begin(), permitted_folders.end(),
                [&](const auto& path) { return SamePath(current, path); })) {
            Limit("fixture source moved outside its permitted paths; cleanup leaves it open"); return;
        }
        browser->Quit();
        if (!Until([&] { return !SameIdentity(); }, 3000)) Limit("fixture-only cleanup could not close its verified source");
    }
};
bool WaitRequest(ExplorerWindowTakeover& worker, ExplorerTakeoverRequest& request) {
    return Until([&] {
        auto requests = worker.DrainRequests();
        if (requests.empty()) return false;
        request = std::move(requests.back());
        return true;
    });
}
bool VisibleUnchanged(const OwnedSource& source, LONG_PTR style, LONG_PTR extended) {
    return source.SameIdentity() && IsWindowVisible(source.identity.hwnd) &&
        GetWindowLongPtrW(source.identity.hwnd, GWL_STYLE) == style &&
        GetWindowLongPtrW(source.identity.hwnd, GWL_EXSTYLE) == extended;
}
void RealCases(const std::filesystem::path& root) {
    const HRESULT apartment = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    Check(SUCCEEDED(apartment), "initialize the isolated real-Explorer fixture apartment");
    if (FAILED(apartment)) return;
    HWND owner = CreateWindowExW(0, L"STATIC", L"Pulse scoped takeover fixture", WS_POPUP,
        0, 0, 1, 1, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    Check(owner != nullptr, "create the fixture-only worker mailbox HWND");
    if (!owner) { CoUninitialize(); return; }
    ExplorerWindowTakeover empty;
    Check(!empty.StartForTesting(owner, kWake, {}, {root.wstring()}) &&
        !empty.StartForTesting(owner, kWake, {owner}, {}), "real watcher test mode rejects either empty allowlist");
    const auto run = root / (L"run-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    std::filesystem::create_directories(run);
    bool environment_ready = true;
    for (const std::wstring scenario : {L"ack-failure", L"ack-success", L"navigate-cancel", L"selection-cancel", L"stop"}) {
        const auto folder = run / scenario;
        const auto next = run / (scenario + L"-next");
        std::filesystem::create_directories(folder);
        std::filesystem::create_directories(next);
        HANDLE file = CreateFileW((folder / L"selected.txt").c_str(), GENERIC_WRITE, 0, nullptr,
            CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
        OwnedSource source;
        if (!source.Create(folder.wstring())) { environment_ready = false; break; }
        source.permitted_folders.push_back(next.wstring());
        const LONG_PTR style = GetWindowLongPtrW(source.identity.hwnd, GWL_STYLE);
        const LONG_PTR extended = GetWindowLongPtrW(source.identity.hwnd, GWL_EXSTYLE);
        unsigned native_tabs = 0;
        EnumChildWindows(source.identity.hwnd, CountNativeTabs, reinterpret_cast<LPARAM>(&native_tabs));
        LASTINPUTINFO input_before{sizeof(input_before)};
        GetLastInputInfo(&input_before);
        ExplorerWindowTakeover worker;
        Check(worker.StartForTesting(owner, kWake, {source.identity.hwnd}, {folder.wstring(), next.wstring()}),
            "start worker with only this new window identity and isolated folders allowed");
        ExplorerTakeoverRequest request;
        const bool received = WaitRequest(worker, request);
        std::printf("[DIAG] scenario=%ls error=0x%08lX scans=%llu request=%llu\n", scenario.c_str(),
            static_cast<unsigned long>(worker.LastError()), static_cast<unsigned long long>(worker.Stats().scans),
            static_cast<unsigned long long>(request.token));
        if (!received) {
            LASTINPUTINFO input_after{sizeof(input_after)};
            GetLastInputInfo(&input_after);
            std::printf("[DIAG] native_tabs=%u last_input_before=%lu after=%lu\n", native_tabs,
                input_before.dwTime, input_after.dwTime);
            if (!native_tabs || input_before.dwTime != input_after.dwTime) {
                Check(worker.Stats().close_attempts == 0 && source.SameIdentity(),
                    "unsupported tab structure or changed input leaves the isolated source open");
                Limit("real handoff was not exercised with unknown native tabs or concurrent user input");
                worker.Stop(); environment_ready = false; break;
            }
        }
        Check(received && SamePath(request.folder, folder.wstring()), "worker submits the exact allowlisted source path");
        if (!received) { worker.Stop(); environment_ready = false; break; }
        Check(VisibleUnchanged(source, style, extended), "handoff leaves source visibility and all window style bits unchanged");
        worker.Acknowledge(request.token + 1000000, true);
        Pause(150);
        Check(worker.IsRequestActive(request.token) && source.SameIdentity(), "unknown or old ACK token cannot close the current source");
        if (scenario == L"ack-success") {
            worker.Acknowledge(request.token, true);
            Check(Until([&] { return !source.SameIdentity(); }), "successful current ACK closes the verified single-tab fixture source");
            Check(worker.Stats().close_attempts == 1, "successful handoff attempts one source close");
        } else if (scenario == L"ack-failure") {
            worker.Acknowledge(request.token, false);
            Check(Until([&] { return !worker.IsRequestActive(request.token); }), "failed ACK retires its token");
            Check(VisibleUnchanged(source, style, extended) && worker.Stats().close_attempts == 0,
                "failed delivery leaves the original source visible without a close attempt");
            worker.Acknowledge(request.token, true); Pause(150);
            Check(source.SameIdentity() && worker.Stats().close_attempts == 0,
                "successful ACK for a previously retired token cannot close the preserved source");
            const auto before = worker.Stats().scans;
            Pause(2300);
            const auto scans = worker.Stats().scans - before;
            std::printf("[DIAG] idle scans over 2300 ms=%llu\n", static_cast<unsigned long long>(scans));
            Check(scans <= 5, "retired source returns to low-frequency idle scanning");
        } else if (scenario == L"navigate-cancel") {
            Check(source.Navigate(next.wstring()), "fixture actively navigates only its own source to another allowed folder");
            Check(Until([&] { return !worker.IsRequestActive(request.token); }), "active source navigation cancels the pending request");
            worker.Acknowledge(request.token, true); Pause(200);
            Check(source.SameIdentity() && SamePath(FolderPath(source.browser.Get()), next.wstring()) &&
                worker.Stats().close_attempts == 0, "late ACK after navigation leaves the source at its new destination");
        } else if (scenario == L"selection-cancel") {
            Check(source.Select((folder / L"selected.txt").wstring()), "programmatic late selection targets only the fixture Shell view");
            Check(Until([&] { return !worker.IsRequestActive(request.token); }), "late selection in a visible source cancels the old token");
            worker.Acknowledge(request.token, true); Pause(200);
            Check(VisibleUnchanged(source, style, extended) && worker.Stats().close_attempts == 0,
                "stale ACK cannot discard a visible source after selection changed");
        } else {
            worker.Stop();
            worker.Acknowledge(request.token, true); Pause(150);
            Check(!worker.IsRequestActive(request.token) && worker.DrainRequests().empty() &&
                VisibleUnchanged(source, style, extended), "Stop rejects late delivery while preserving source visibility and style bits");
        }
        worker.Stop();
    }
    if (!environment_ready) Limit("remaining real scenarios require an isolated supported Explorer source; no global watcher fallback was enabled");
    DestroyWindow(owner);
    CoUninitialize();
}
} // namespace
int wmain(int argc, wchar_t** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    PolicyCases();
    if (argc == 3 && wcscmp(argv[1], L"--real") == 0) {
        const auto root = std::filesystem::absolute(argv[2]).lexically_normal();
        const auto expected = (std::filesystem::current_path() / L"bench_data" / L"explorer-takeover-20261019").lexically_normal();
        if (!SamePath(root.wstring(), expected.wstring())) {
            Check(false, "real fixture root must be the explicitly scoped bench_data directory");
        } else {
            RealCases(root);
        }
    } else if (argc != 1 && !(argc == 2 && wcscmp(argv[1], L"--policy") == 0)) {
        std::printf("usage: pulse_explorer_takeover_test [--policy | --real bench_data/explorer-takeover-20261019]\n");
        return 2;
    }
    std::printf("[RESULT] failures=%d limits=%d\n", failures, limits);
    return failures ? 1 : limits ? 3 : 0;
}
