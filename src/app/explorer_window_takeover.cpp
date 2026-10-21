#include "explorer_window_takeover.h"
#include "explorer_takeover_policy.h"
#include "default_file_manager.h"
#include <shlobj.h>
#include <exdisp.h>
#include <shldisp.h>
#include <shlwapi.h>
#include <wrl/client.h>
#include <algorithm>
#include <atomic>
#include <map>
#include <mutex>
#include <set>
#include <utility>

namespace pulse::app {
using Microsoft::WRL::ComPtr;
struct ExplorerWindowTakeover::Shared {
    HWND window = nullptr;
    UINT message = 0;
    HANDLE wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    std::atomic<bool> stop{false};
    std::atomic<bool> ready{false};
    std::atomic<HRESULT> error{S_OK};
    std::mutex mutex;
    std::atomic<uint64_t> next_token{1};
    std::vector<ExplorerTakeoverRequest> requests;
    std::map<uint64_t, bool> acknowledgments;
    std::set<uint64_t> outstanding;
    bool scoped = false;
    std::map<HWND, std::pair<DWORD, uint64_t>> allowed_windows;
    std::vector<std::wstring> allowed_folders;
    std::atomic<uint64_t> scans{0}, submitted{0}, replaced{0}, cancelled{0};
    std::atomic<uint64_t> close_attempts{0}, close_failures{0};
    ~Shared() { if (wake) CloseHandle(wake); }
};
namespace {
struct WindowId {
    HWND hwnd = nullptr;
    DWORD pid = 0;
    uint64_t creation = 0;
    bool operator==(const WindowId&) const = default;
};
bool ExplorerId(HWND hwnd, WindowId& id) {
    hwnd = GetAncestor(hwnd, GA_ROOT);
    if (!hwnd) return false;
    wchar_t cls[64]{};
    if (!GetClassNameW(hwnd, cls, 64) || wcscmp(cls, L"CabinetWClass")) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return false;
    wchar_t path[32768]{}; DWORD n = 32768;
    FILETIME created{}, exited{}, kernel{}, user{};
    const bool ok = QueryFullProcessImageNameW(process, 0, path, &n) &&
        GetProcessTimes(process, &created, &exited, &kernel, &user) &&
        _wcsicmp(PathFindFileNameW(path), L"explorer.exe") == 0;
    CloseHandle(process);
    if (ok) id = {hwnd, pid, (static_cast<uint64_t>(created.dwHighDateTime) << 32) | created.dwLowDateTime};
    return ok;
}
BOOL CALLBACK CollectExisting(HWND hwnd, LPARAM context) {
    auto* existing = reinterpret_cast<std::set<HWND>*>(context);
    wchar_t cls[64]{};
    if (GetClassNameW(hwnd, cls, 64) && wcscmp(cls, L"CabinetWClass") == 0) existing->insert(hwnd);
    return TRUE;
}
struct ObservationEvents {
    explicit ObservationEvents(std::shared_ptr<ExplorerWindowTakeover::Shared> state)
        : shared(std::move(state)) {}
    std::shared_ptr<ExplorerWindowTakeover::Shared> shared;
    std::set<HWND> ignored;
    std::set<HWND> exposed;
    std::map<HWND, uint64_t> generations;
    std::map<HWND, DWORD> first_observed;
    uint64_t next_generation = 0;
    bool dirty = true;
    void Wake() { dirty = true; SetEvent(shared->wake); }
    uint64_t Generation(HWND hwnd) {
        auto [it, added] = generations.try_emplace(hwnd, 0);
        if (added) {
            it->second = ++next_generation;
            first_observed[hwnd] = GetTickCount();
        }
        return it->second;
    }
};
thread_local ObservationEvents* observation_events = nullptr;
void CALLBACK ObserveWindow(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG object, LONG child,
                            DWORD, DWORD event_time) {
    auto* observer = observation_events;
    if (!observer || !hwnd ||
        (event != EVENT_SYSTEM_FOREGROUND && (object != OBJID_WINDOW || child != CHILDID_SELF))) return;
    if (event == EVENT_OBJECT_DESTROY && observer->generations.contains(hwnd)) {
        observer->generations[hwnd] = ++observer->next_generation;
        observer->ignored.insert(hwnd);
        observer->Wake();
        return;
    }
    const HWND root = GetAncestor(hwnd, GA_ROOT);
    wchar_t cls[64]{};
    if (!root || !GetClassNameW(root, cls, 64) || wcscmp(cls, L"CabinetWClass")) return;
    if (event == EVENT_SYSTEM_FOREGROUND || (event == EVENT_OBJECT_SHOW && hwnd == root))
        observer->exposed.insert(root);
    if (event == EVENT_OBJECT_CREATE && hwnd == root) {
        const bool known = observer->generations.contains(root);
        observer->Generation(root);
        // WinEvents may arrive after the Shell registration that discovered
        // this same window. Only a later creation invalidates its generation.
        if (known && static_cast<LONG>(event_time - observer->first_observed[root]) > 0) {
            observer->generations[root] = ++observer->next_generation;
            observer->ignored.insert(root);
        }
    }
    if ((event == EVENT_OBJECT_CREATE || event == EVENT_OBJECT_SHOW || event == EVENT_SYSTEM_FOREGROUND) &&
        (GetAsyncKeyState(VK_SHIFT) & 0x8000)) observer->ignored.insert(root);
    observer->Wake();
}
struct Snapshot {
    std::wstring folder;
    std::vector<std::wstring> selected;
    bool operator==(const Snapshot&) const = default;
};
bool ReadBrowser(IWebBrowserApp* browser, Snapshot& result) {
    VARIANT_BOOL busy = VARIANT_FALSE;
    if (FAILED(browser->get_Busy(&busy)) || busy) return false;
    ComPtr<IDispatch> document;
    ComPtr<IShellFolderViewDual> view;
    ComPtr<Folder> folder;
    ComPtr<FolderItem> self;
    if (FAILED(browser->get_Document(&document)) || !document || FAILED(document.As(&view)) ||
        FAILED(view->get_Folder(&folder)) || !folder) return false;
    ComPtr<Folder2> folder2;
    if (FAILED(folder.As(&folder2)) || FAILED(folder2->get_Self(&self)) || !self) return false;
    BSTR path = nullptr;
    if (FAILED(self->get_Path(&path)) || !path) return false;
    std::wstring folder_path(path, SysStringLen(path));
    SysFreeString(path);
    VARIANT_BOOL fs = VARIANT_FALSE;
    if (FAILED(self->get_IsFileSystem(&fs))) return false;
    const bool this_pc = IsThisPcArgument(folder_path);
    if (fs == VARIANT_FALSE && !this_pc) return false;
    if (fs != VARIANT_FALSE) {
        const DWORD attrs = GetFileAttributesW(folder_path.c_str());
        if (attrs == INVALID_FILE_ATTRIBUTES || !(attrs & FILE_ATTRIBUTE_DIRECTORY)) return false;
    }
    result.folder = this_pc ? std::wstring() : std::move(folder_path);
    ComPtr<FolderItems> selected;
    if (FAILED(view->SelectedItems(&selected)) || !selected) return false;
    long count = 0;
    if (FAILED(selected->get_Count(&count)) || count < 0 || count > 4096) return false;
    for (long i = 0; i < count; ++i) {
        VARIANT index{}; index.vt = VT_I4; index.lVal = i;
        ComPtr<FolderItem> item;
        if (FAILED(selected->Item(index, &item)) || !item) return false;
        BSTR selected_path = nullptr;
        if (FAILED(item->get_Path(&selected_path)) || !selected_path) return false;
        result.selected.emplace_back(selected_path, SysStringLen(selected_path));
        SysFreeString(selected_path);
    }
    std::sort(result.selected.begin(), result.selected.end());
    return true;
}
struct Candidate {
    WindowId id;
    ComPtr<IWebBrowserApp> browser;
    ULONGLONG discovered = 0;
    ULONGLONG stable_since = 0;
    ULONGLONG view_ready_at = 0;
    uint64_t window_generation = 0;
    DWORD input_at_discovery = 0;
    bool input_known = false;
    bool exposed = false;
    Snapshot snapshot;
    uint64_t token = 0;
    uint64_t replaces_token = 0;
};
class Events final : public IDispatch {
public:
    explicit Events(std::shared_ptr<ObservationEvents> observer) : observer_(std::move(observer)) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (id != IID_IUnknown && id != IID_IDispatch && id != DIID_DShellWindowsEvents) return E_NOINTERFACE;
        *out = static_cast<IDispatch*>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override { const ULONG n = --refs_; if (!n) delete this; return n; }
    HRESULT STDMETHODCALLTYPE GetTypeInfoCount(UINT* n) override { if (!n) return E_POINTER; *n = 0; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetTypeInfo(UINT, LCID, ITypeInfo**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetIDsOfNames(REFIID, LPOLESTR*, UINT, LCID, DISPID*) override { return DISP_E_UNKNOWNNAME; }
    HRESULT STDMETHODCALLTYPE Invoke(DISPID id, REFIID, LCID, WORD, DISPPARAMS* p, VARIANT*, EXCEPINFO*, UINT*) override {
        if ((id == 200 || id == 201) && p && p->cArgs == 1) {
            if (id == 200 && (GetAsyncKeyState(VK_SHIFT) & 0x8000))
                EnumWindows(CollectExisting, reinterpret_cast<LPARAM>(&observer_->ignored));
            observer_->Wake();
        }
        return S_OK;
    }
private:
    ~Events() = default;
    std::atomic<ULONG> refs_{1};
    std::shared_ptr<ObservationEvents> observer_;
};
bool SameFolder(const std::wstring& left, const std::wstring& right) {
    return left.size() == right.size() && CompareStringOrdinal(left.c_str(),
        static_cast<int>(left.size()), right.c_str(), static_cast<int>(right.size()), TRUE) == CSTR_EQUAL;
}
bool ScopeAllows(const ExplorerWindowTakeover::Shared& shared, const WindowId& id,
                 const Snapshot* snapshot = nullptr) {
    if (!shared.scoped) return true;
    const auto found = shared.allowed_windows.find(id.hwnd);
    if (found == shared.allowed_windows.end() || found->second.first != id.pid ||
        found->second.second != id.creation) return false;
    return !snapshot || std::any_of(shared.allowed_folders.begin(), shared.allowed_folders.end(),
        [&](const auto& folder) { return SameFolder(folder, snapshot->folder); });
}
bool RequireNativeTabs() {
    using VersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
    const auto query = reinterpret_cast<VersionFn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
    OSVERSIONINFOW version{};
    version.dwOSVersionInfoSize = sizeof(version);
    // An unknown OS cannot establish that this Explorer lacks native tabs.
    return !query || query(&version) != 0 || version.dwBuildNumber >= 22000;
}
uint32_t NativeTabs(HWND root) {
    uint32_t count = 0;
    EnumChildWindows(root, [](HWND child, LPARAM context) -> BOOL {
        wchar_t cls[64]{};
        if (GetClassNameW(child, cls, ARRAYSIZE(cls)) && wcscmp(cls, L"ShellTabWindowClass") == 0)
            ++*reinterpret_cast<uint32_t*>(context);
        return TRUE;
    }, reinterpret_cast<LPARAM>(&count));
    return count;
}
bool ReadInputTime(DWORD& time) {
    LASTINPUTINFO input{sizeof(input)};
    if (!GetLastInputInfo(&input)) return false;
    time = input.dwTime;
    return true;
}
using BrowserMap = std::map<HWND, std::vector<ComPtr<IWebBrowserApp>>>;
HRESULT ReadBrowsers(IShellWindows* windows, ExplorerWindowTakeover::Shared& shared,
                     BrowserMap& browsers, bool& complete) {
    ++shared.scans;
    complete = true;
    long count = 0;
    const HRESULT result = windows->get_Count(&count);
    if (FAILED(result)) return result;
    if (count < 0 || count >= 4096) return E_UNEXPECTED;
    for (long i = 0; i < count; ++i) {
        VARIANT index{}; index.vt = VT_I4; index.lVal = i;
        ComPtr<IDispatch> dispatch;
        ComPtr<IWebBrowserApp> browser;
        SHANDLE_PTR handle = 0;
        if (FAILED(windows->Item(index, &dispatch)) || !dispatch) { complete = false; continue; }
        // Other registered clients need not expose IWebBrowserApp.
        if (FAILED(dispatch.As(&browser))) continue;
        if (FAILED(browser->get_HWND(&handle))) { complete = false; continue; }
        HWND root = GetAncestor(reinterpret_cast<HWND>(handle), GA_ROOT);
        if (!root || (shared.scoped && !shared.allowed_windows.contains(root))) continue;
        WindowId id;
        if (ExplorerId(root, id) && ScopeAllows(shared, id)) browsers[root].push_back(std::move(browser));
    }
    return S_OK;
}
bool SingleTab(const Candidate& candidate, const BrowserMap& browsers, bool complete,
                bool native_required) {
    const auto found = browsers.find(candidate.id.hwnd);
    explorer_takeover::TabObservation observation;
    observation.tab_structure_required = native_required;
    observation.enumeration_complete = complete;
    observation.shell_view_count = found == browsers.end() ? 0 : static_cast<uint32_t>(found->second.size());
    observation.native_tab_count = NativeTabs(candidate.id.hwnd);
    return explorer_takeover::IsSingleTabSafe(observation);
}
bool SameIdentity(const Candidate& candidate, ObservationEvents& observer) {
    WindowId current;
    return !observer.ignored.contains(candidate.id.hwnd) &&
        observer.Generation(candidate.id.hwnd) == candidate.window_generation &&
        ExplorerId(candidate.id.hwnd, current) && current == candidate.id &&
        ScopeAllows(*observer.shared, current);
}
explorer_takeover::SourceObservation ObserveSource(const Candidate& candidate,
    ObservationEvents& observer, const BrowserMap& browsers, bool complete, bool native_required,
    const Snapshot* snapshot, ULONGLONG now) {
    explorer_takeover::SourceObservation result;
    result.same_identity = SameIdentity(candidate, observer);
    result.single_tab = result.same_identity && SingleTab(candidate, browsers, complete, native_required);
    result.supported = snapshot && ScopeAllows(*observer.shared, candidate.id, snapshot);
    result.same_folder = snapshot && SameFolder(snapshot->folder, candidate.snapshot.folder);
    result.same_selection = snapshot && snapshot->selected == candidate.snapshot.selected;
    result.visible = candidate.exposed || observer.exposed.contains(candidate.id.hwnd) ||
        IsWindowVisible(candidate.id.hwnd) != FALSE;
    DWORD input = 0;
    result.input_unchanged = candidate.input_known && ReadInputTime(input) && input == candidate.input_at_discovery;
    result.shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    result.submitted_empty_selection = candidate.snapshot.selected.empty();
    result.current_selection_nonempty = snapshot && !snapshot->selected.empty();
    result.view_age_ms = candidate.view_ready_at ? now - candidate.view_ready_at : 0;
    return result;
}
void CancelToken(ExplorerWindowTakeover::Shared& shared, uint64_t token, bool cancelled = true) {
    if (!token) return;
    std::lock_guard lock(shared.mutex);
    if (shared.outstanding.erase(token) && cancelled) ++shared.cancelled;
    shared.acknowledgments.erase(token);
    std::erase_if(shared.requests, [&](const auto& request) { return request.token == token; });
}
bool CommitClose(ExplorerWindowTakeover::Shared& shared, uint64_t token) {
    std::lock_guard lock(shared.mutex);
    if (shared.stop || !shared.ready || !shared.outstanding.erase(token)) return false;
    // Stop uses this same gate. After the token is consumed the close is in
    // flight; never hold the mailbox mutex across a potentially slow COM call.
    ++shared.close_attempts;
    return true;
}
bool SubmitCandidate(ExplorerWindowTakeover::Shared& shared, Candidate& candidate) {
    std::lock_guard lock(shared.mutex);
    if (shared.requests.size() >= 128 || shared.outstanding.size() >= 128 || shared.stop) return false;
    candidate.token = shared.next_token++;
    shared.outstanding.insert(candidate.token);
    shared.requests.push_back({candidate.token, candidate.snapshot.folder, candidate.snapshot.selected,
        candidate.replaces_token});
    if (!PostMessageW(shared.window, shared.message, 0, 0)) {
        shared.requests.pop_back(); shared.outstanding.erase(candidate.token);
        candidate.token = 0;
        return false;
    }
    ++shared.submitted;
    if (candidate.replaces_token) ++shared.replaced;
    return true;
}
void PumpMessages() {
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&message); DispatchMessageW(&message);
    }
}
bool FreshSource(IShellWindows* windows, Candidate& candidate, ObservationEvents& observer,
                  bool native_required, Snapshot& snapshot,
                  explorer_takeover::SourceObservation& observation) {
    BrowserMap current;
    bool complete = false;
    const HRESULT result = ReadBrowsers(windows, *observer.shared, current, complete);
    if (FAILED(result)) observer.shared->error = result;
    const auto found = current.find(candidate.id.hwnd);
    bool readable = false;
    if (SUCCEEDED(result) && found != current.end() && found->second.size() == 1 &&
        SameIdentity(candidate, observer)) {
        candidate.browser = found->second.front();
        readable = ReadBrowser(candidate.browser.Get(), snapshot);
    }
    // ReadBrowser can enter COM and dispatch events. Check the identity and
    // native tab tree after it, including for the final successful ACK.
    observation = ObserveSource(candidate, observer, current, complete, native_required,
        readable ? &snapshot : nullptr, GetTickCount64());
    return readable;
}
void ReplaceCandidate(Candidate& candidate, Snapshot snapshot,
                       ExplorerWindowTakeover::Shared& shared, ULONGLONG now) {
    const uint64_t old_token = candidate.token;
    CancelToken(shared, old_token);
    candidate.token = 0;
    candidate.replaces_token = old_token;
    candidate.snapshot = std::move(snapshot);
    candidate.stable_since = now;
}
HRESULT ScanCandidates(IShellWindows* windows, ObservationEvents& observer,
                        std::map<HWND, Candidate>& candidates, bool native_required) {
    auto& shared = *observer.shared;
    BrowserMap browsers;
    bool complete = false;
    const HRESULT result = ReadBrowsers(windows, shared, browsers, complete);
    if (FAILED(result)) return result;
    const ULONGLONG now = GetTickCount64();
    std::erase_if(observer.ignored, [](HWND hwnd) { return !IsWindow(hwnd); });
    for (auto& [hwnd, views] : browsers) {
        if (observer.ignored.contains(hwnd) || candidates.contains(hwnd) || !complete) continue;
        if (views.size() != 1 || NativeTabs(hwnd) > 1 || (GetAsyncKeyState(VK_SHIFT) & 0x8000)) {
            observer.ignored.insert(hwnd); continue;
        }
        if (candidates.size() >= 128) { observer.ignored.insert(hwnd); continue; }
        Candidate candidate;
        if (!ExplorerId(hwnd, candidate.id) || !ScopeAllows(shared, candidate.id)) continue;
        candidate.browser = views.front();
        candidate.discovered = now;
        candidate.window_generation = observer.Generation(hwnd);
        candidate.input_known = ReadInputTime(candidate.input_at_discovery);
        candidate.exposed = observer.exposed.contains(hwnd) || IsWindowVisible(hwnd) != FALSE;
        candidates.emplace(hwnd, std::move(candidate));
    }
    for (auto it = candidates.begin(); it != candidates.end();) {
        auto& candidate = it->second;
        bool completed = false;
        candidate.exposed |= IsWindowVisible(candidate.id.hwnd) != FALSE;
        DWORD input = 0;
        bool retire = shared.stop || !SameIdentity(candidate, observer) ||
            now - candidate.discovered >= explorer_takeover::kCandidateTimeoutMs ||
            !candidate.input_known || !ReadInputTime(input) || input != candidate.input_at_discovery ||
            (GetAsyncKeyState(VK_SHIFT) & 0x8000);
        const auto found = browsers.find(it->first);
        const bool single = !retire && SingleTab(candidate, browsers, complete, native_required);
        if (!retire && !single) {
            // A not-yet-created Win11 tab can become readable later. It never
            // authorizes submission/close; losing an established guard retires.
            retire = candidate.token || candidate.view_ready_at || found == browsers.end() ||
                (found != browsers.end() && found->second.size() != 1) || NativeTabs(it->first) > 1;
        }
        Snapshot snapshot;
        bool readable = false;
        if (!retire && single && found != browsers.end()) {
            candidate.browser = found->second.front();
            readable = ReadBrowser(candidate.browser.Get(), snapshot);
            if (readable && !ScopeAllows(shared, candidate.id, &snapshot)) retire = true;
        }
        if (!retire && !readable && candidate.token) retire = true;
        if (!retire && readable) {
            auto observation = ObserveSource(candidate, observer, browsers, complete, native_required, &snapshot, now);
            if (!observation.same_identity || !observation.single_tab || !observation.input_unchanged ||
                observation.shift || shared.stop) retire = true;
            else if (!candidate.view_ready_at) {
                candidate.view_ready_at = now;
                candidate.stable_since = now;
                candidate.snapshot = std::move(snapshot);
            } else if (candidate.token) {
                const auto decision = explorer_takeover::DecidePending(observation);
                if (decision == explorer_takeover::PendingDecision::Retire) retire = true;
                else if (decision == explorer_takeover::PendingDecision::Replace) {
                    ReplaceCandidate(candidate, std::move(snapshot), shared, now);
                } else {
                    bool answered = false, delivered = false;
                    {
                        std::lock_guard lock(shared.mutex);
                        const auto answer = shared.acknowledgments.find(candidate.token);
                        if (answer != shared.acknowledgments.end()) {
                            answered = true; delivered = answer->second;
                            shared.acknowledgments.erase(answer);
                        }
                    }
                    if (answered) {
                        if (!delivered) retire = true;
                        else {
                            Snapshot final_snapshot;
                            explorer_takeover::SourceObservation final_observation;
                            FreshSource(windows, candidate, observer, native_required,
                                final_snapshot, final_observation);
                            const auto final_decision = explorer_takeover::DecidePending(final_observation);
                            if (final_decision == explorer_takeover::PendingDecision::Replace && !shared.stop) {
                                ReplaceCandidate(candidate, std::move(final_snapshot), shared, GetTickCount64());
                            } else {
                                if (explorer_takeover::CanCloseAfterAcknowledgment(final_observation, true, shared.stop) &&
                                    CommitClose(shared, candidate.token)) {
                                    const HRESULT closed = candidate.browser->Quit();
                                    shared.error = closed;
                                    if (FAILED(closed)) ++shared.close_failures;
                                    else completed = true;
                                }
                                retire = true;
                            }
                        }
                    }
                }
            } else if (!SameFolder(snapshot.folder, candidate.snapshot.folder)) {
                // A navigation after the first readable folder belongs to the
                // source window, even while Pulse has not received a token yet.
                retire = true;
            } else if (snapshot != candidate.snapshot) {
                candidate.snapshot = std::move(snapshot);
                candidate.stable_since = now;
            }
            const ULONGLONG decision_now = GetTickCount64();
            if (!retire && !candidate.token && candidate.view_ready_at &&
                explorer_takeover::CanSubmit(decision_now - candidate.stable_since,
                    decision_now - candidate.view_ready_at, !candidate.snapshot.selected.empty())) {
                Snapshot final_snapshot;
                explorer_takeover::SourceObservation final_observation;
                FreshSource(windows, candidate, observer, native_required, final_snapshot, final_observation);
                if (explorer_takeover::DecidePending(final_observation) == explorer_takeover::PendingDecision::Wait &&
                    final_observation.same_selection && !shared.stop) {
                    retire = !SubmitCandidate(shared, candidate);
                } else if (final_observation.same_identity && final_observation.single_tab &&
                    final_observation.supported && final_observation.same_folder &&
                    final_observation.input_unchanged && !final_observation.shift && !shared.stop) {
                    // A selection arriving during final validation has not been
                    // submitted yet. Give that complete snapshot its stable time.
                    candidate.snapshot = std::move(final_snapshot);
                    candidate.stable_since = GetTickCount64();
                } else retire = true;
            }
        }
        if (retire) {
            CancelToken(shared, candidate.token, !completed);
            observer.ignored.insert(it->first);
            it = candidates.erase(it);
        } else ++it;
    }
    std::erase_if(observer.exposed, [](HWND hwnd) { return !IsWindow(hwnd); });
    std::erase_if(observer.generations, [](const auto& item) { return !IsWindow(item.first); });
    std::erase_if(observer.first_observed, [&](const auto& item) { return !observer.generations.contains(item.first); });
    return S_OK;
}
DWORD WINAPI RunTakeover(void* context) {
    std::unique_ptr<std::shared_ptr<ExplorerWindowTakeover::Shared>> holder(
        static_cast<std::shared_ptr<ExplorerWindowTakeover::Shared>*>(context));
    auto shared = *holder;
    const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (FAILED(init)) { shared->error = init; return 1; }
    {
        while (!shared->stop) {
            // Fail closed across a disconnected Explorer: anything already open at
            // reconnect belongs to the user, not to an old takeover request.
            std::set<HWND> baseline;
            EnumWindows(CollectExisting, reinterpret_cast<LPARAM>(&baseline));
            ComPtr<IShellWindows> windows;
            shared->error = CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&windows));
            if (windows && !shared->stop) {
                auto observer = std::make_shared<ObservationEvents>(shared);
                observer->ignored = baseline;
                if (shared->scoped) {
                    for (const auto& [hwnd, identity] : shared->allowed_windows) {
                        (void)identity;
                        observer->ignored.erase(hwnd);
                    }
                }
                observation_events = observer.get();
                const HWINEVENTHOOK window_hook = SetWinEventHook(EVENT_OBJECT_CREATE, EVENT_OBJECT_SHOW,
                    nullptr, ObserveWindow, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
                const HWINEVENTHOOK foreground_hook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND,
                    nullptr, ObserveWindow, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
                ComPtr<IConnectionPointContainer> container;
                ComPtr<IConnectionPoint> point;
                ComPtr<Events> events;
                events.Attach(new Events(observer));
                DWORD subscription = 0;
                if (SUCCEEDED(windows.As(&container)) &&
                    SUCCEEDED(container->FindConnectionPoint(DIID_DShellWindowsEvents, &point)))
                    shared->error = point->Advise(events.Get(), &subscription);
                // Without events Shift-at-registration cannot be respected: retry
                // the connection without taking any existing/new source windows.
                if ((!subscription || !window_hook || !foreground_hook) && SUCCEEDED(shared->error.load()))
                    shared->error = E_FAIL;
                const bool connected = subscription && window_hook && foreground_hook;
                if (connected && !shared->scoped)
                    EnumWindows(CollectExisting, reinterpret_cast<LPARAM>(&observer->ignored));
                shared->ready = connected;
                std::map<HWND, Candidate> candidates;
                const bool native_required = RequireNativeTabs();
                ULONGLONG next_scan = 0;
                while (connected && !shared->stop) {
                    PumpMessages();
                    if (shared->stop) break;
                    const ULONGLONG now = GetTickCount64();
                    if (observer->dirty || now >= next_scan) {
                        observer->dirty = false;
                        const HRESULT scanned = ScanCandidates(windows.Get(), *observer, candidates, native_required);
                        if (FAILED(scanned)) { shared->error = scanned; break; }
                        next_scan = GetTickCount64() + explorer_takeover::NextScanDelay(!candidates.empty());
                    }
                    const ULONGLONG current = GetTickCount64();
                    const DWORD timeout = next_scan <= current ? 0 : static_cast<DWORD>(next_scan - current);
                    if (MsgWaitForMultipleObjects(1, &shared->wake, FALSE, timeout, QS_ALLINPUT) == WAIT_OBJECT_0)
                        observer->dirty = true;
                }
                shared->ready = false;
                if (subscription) point->Unadvise(subscription);
                if (window_hook) UnhookWinEvent(window_hook);
                if (foreground_hook) UnhookWinEvent(foreground_hook);
                observation_events = nullptr;
            }
            { std::lock_guard lock(shared->mutex);
                shared->requests.clear(); shared->acknowledgments.clear(); shared->outstanding.clear();
            }
            if (!shared->stop) {
                MsgWaitForMultipleObjects(1, &shared->wake, FALSE, 1000, QS_ALLINPUT);
                PumpMessages();
            }
        }
    }
    shared->ready = false;
    CoUninitialize(); return 0;
}
}
ExplorerWindowTakeover::ExplorerWindowTakeover() = default;
ExplorerWindowTakeover::~ExplorerWindowTakeover() { Stop(); }
bool ExplorerWindowTakeover::Start(HWND hwnd, UINT message) {
    return StartImpl(hwnd, message, {}, {}, false);
}
bool ExplorerWindowTakeover::StartForTesting(HWND hwnd, UINT message,
    const std::vector<HWND>& allowed_windows, const std::vector<std::wstring>& allowed_folders) {
    return StartImpl(hwnd, message, allowed_windows, allowed_folders, true);
}
bool ExplorerWindowTakeover::StartImpl(HWND hwnd, UINT message,
    const std::vector<HWND>& allowed_windows, const std::vector<std::wstring>& allowed_folders, bool scoped) {
    Stop();
    std::lock_guard lifecycle(lifecycle_);
    if (!IsWindow(hwnd) || message < WM_APP || message > 0xBFFF) return false;
    if (scoped && (allowed_windows.empty() || allowed_folders.empty())) return false;
    shared_ = std::make_shared<Shared>(); shared_->window = hwnd; shared_->message = message;
    shared_->scoped = scoped;
    for (const HWND source : allowed_windows) {
        WindowId id;
        if (!ExplorerId(source, id)) { shared_.reset(); return false; }
        shared_->allowed_windows.emplace(id.hwnd, std::pair{id.pid, id.creation});
    }
    for (const auto& folder : allowed_folders) {
        if (folder.empty()) { shared_.reset(); return false; }
        const DWORD attributes = GetFileAttributesW(folder.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY)) {
            shared_.reset(); return false;
        }
    }
    shared_->allowed_folders = allowed_folders;
    shared_->next_token = next_token_;
    if (!shared_->wake) { shared_.reset(); return false; }
    auto context = std::make_unique<std::shared_ptr<Shared>>(shared_);
    thread_ = CreateThread(nullptr, 0, RunTakeover, context.get(), 0, nullptr);
    if (!thread_) { shared_.reset(); return false; }
    context.release(); return true;
}
ExplorerTakeoverStats ExplorerWindowTakeover::Stats() const {
    std::lock_guard lifecycle(lifecycle_);
    if (!shared_) return {};
    return {shared_->scans.load(), shared_->submitted.load(), shared_->replaced.load(),
        shared_->cancelled.load(), shared_->close_attempts.load(), shared_->close_failures.load()};
}
std::vector<ExplorerTakeoverRequest> ExplorerWindowTakeover::DrainRequests() {
    std::lock_guard lifecycle(lifecycle_);
    if (!shared_) return {};
    std::lock_guard lock(shared_->mutex);
    std::vector<ExplorerTakeoverRequest> requests;
    requests.swap(shared_->requests);
    std::erase_if(requests, [&](const auto& request) {
        return !shared_->outstanding.contains(request.token);
    });
    return requests;
}
void ExplorerWindowTakeover::Acknowledge(uint64_t token, bool delivered) {
    std::lock_guard lifecycle(lifecycle_);
    if (!shared_ || shared_->stop) return;
    { std::lock_guard lock(shared_->mutex);
        if (!shared_->outstanding.contains(token)) return;
        shared_->acknowledgments.emplace(token, delivered);
    }
    SetEvent(shared_->wake);
}
HRESULT ExplorerWindowTakeover::LastError() const {
    std::lock_guard lifecycle(lifecycle_);
    return shared_ ? shared_->error.load() : S_OK;
}
bool ExplorerWindowTakeover::IsRequestActive(uint64_t token) const {
    std::lock_guard lifecycle(lifecycle_);
    if (!shared_ || !shared_->ready || shared_->stop) return false;
    std::lock_guard lock(shared_->mutex);
    return shared_->outstanding.contains(token);
}
void ExplorerWindowTakeover::Stop() {
    std::shared_ptr<Shared> retired;
    HANDLE thread = nullptr;
    {
        std::lock_guard lifecycle(lifecycle_);
        retired = std::move(shared_); thread = std::exchange(thread_, nullptr);
        if (retired) {
            std::lock_guard lock(retired->mutex);
            retired->stop = true; retired->ready = false;
            retired->cancelled += retired->outstanding.size();
            retired->requests.clear(); retired->acknowledgments.clear(); retired->outstanding.clear();
            next_token_ = retired->next_token.load() + 1;
            SetEvent(retired->wake);
        }
    }
    // Keep the UI mailboxes independent of a slow/disconnected COM shutdown.
    // Shared is retained by the STA until it can retire; never kill the thread.
    if (thread) { WaitForSingleObject(thread, 2000); CloseHandle(thread); }
}
} // namespace pulse::app
