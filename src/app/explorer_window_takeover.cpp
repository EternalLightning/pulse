#include "explorer_window_takeover.h"
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
thread_local std::set<HWND>* creation_bypass = nullptr;
void CALLBACK ObserveWindow(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG object, LONG child,
                            DWORD, DWORD) {
    if (!creation_bypass || !hwnd || object != OBJID_WINDOW || child != CHILDID_SELF ||
        (event != EVENT_OBJECT_CREATE && event != EVENT_OBJECT_SHOW) ||
        !(GetAsyncKeyState(VK_SHIFT) & 0x8000)) return;
    wchar_t cls[64]{};
    if (GetClassNameW(hwnd, cls, 64) && wcscmp(cls, L"CabinetWClass") == 0)
        creation_bypass->insert(hwnd);
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
    Snapshot snapshot;
    uint64_t token = 0;
};
class Events final : public IDispatch {
public:
    explicit Events(std::shared_ptr<std::set<HWND>> bypass) : bypass_(std::move(bypass)) {}
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
        if (id == 200 && p && p->cArgs == 1 &&
            (GetAsyncKeyState(VK_SHIFT) & 0x8000))
            EnumWindows(CollectExisting, reinterpret_cast<LPARAM>(bypass_.get()));
        return S_OK;
    }
private:
    ~Events() = default;
    std::atomic<ULONG> refs_{1};
    std::shared_ptr<std::set<HWND>> bypass_;
};
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
                auto ignored_storage = std::make_shared<std::set<HWND>>(baseline);
                auto& ignored = *ignored_storage;
                creation_bypass = &ignored;
                const HWINEVENTHOOK window_hook = SetWinEventHook(EVENT_OBJECT_CREATE, EVENT_OBJECT_SHOW,
                    nullptr, ObserveWindow, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
                ComPtr<IConnectionPointContainer> container;
                ComPtr<IConnectionPoint> point;
                ComPtr<Events> events;
                events.Attach(new Events(ignored_storage));
                DWORD subscription = 0;
                if (SUCCEEDED(windows.As(&container)) &&
                    SUCCEEDED(container->FindConnectionPoint(DIID_DShellWindowsEvents, &point)))
                    shared->error = point->Advise(events.Get(), &subscription);
                // Without events Shift-at-registration cannot be respected: retry
                // the connection without taking any existing/new source windows.
                if (!subscription && SUCCEEDED(shared->error.load())) shared->error = E_FAIL;
                if (subscription)
                    EnumWindows(CollectExisting, reinterpret_cast<LPARAM>(&ignored));
                shared->ready = subscription != 0;
                std::map<HWND, Candidate> candidates;
                while (subscription && !shared->stop) {
                    std::map<HWND, std::vector<ComPtr<IWebBrowserApp>>> browsers;
                    long count = 0;
                    const HRESULT enumeration = windows->get_Count(&count);
                    if (FAILED(enumeration)) { shared->error = enumeration; break; }
                    if (count >= 0 && count < 4096) {
                        for (long i = 0; i < count; ++i) {
                            VARIANT index{}; index.vt = VT_I4; index.lVal = i;
                            ComPtr<IDispatch> dispatch;
                            ComPtr<IWebBrowserApp> browser;
                            SHANDLE_PTR handle = 0;
                            if (FAILED(windows->Item(index, &dispatch)) || !dispatch || FAILED(dispatch.As(&browser)) ||
                                FAILED(browser->get_HWND(&handle))) continue;
                            HWND hwnd = reinterpret_cast<HWND>(handle);
                            WindowId id;
                            if (ExplorerId(hwnd, id)) browsers[hwnd].push_back(browser);
                        }
                    }
                    const auto now = GetTickCount64();
                    std::erase_if(ignored, [](HWND hwnd) { return !IsWindow(hwnd); });
                    for (auto& [hwnd, views] : browsers) {
                        if (ignored.contains(hwnd)) continue;
                        if (views.size() != 1 || (GetAsyncKeyState(VK_SHIFT) & 0x8000)) {
                            // Retire through the candidate loop so outstanding tokens
                            // are cancelled as well, not leaked across future scans.
                            ignored.insert(hwnd); continue;
                        }
                        if (!candidates.contains(hwnd)) {
                            WindowId id;
                            if (ExplorerId(hwnd, id)) candidates.emplace(hwnd, Candidate{id, views.front(), now, 0, {}, 0});
                        }
                    }
                    for (auto it = candidates.begin(); it != candidates.end();) {
                        auto& c = it->second;
                        WindowId current;
                        const auto found = browsers.find(it->first);
                        bool retire = !ExplorerId(it->first, current) || current != c.id || found == browsers.end() ||
                            found->second.size() != 1 || ignored.contains(it->first) || now - c.discovered > 15000;
                        if (!retire && (GetAsyncKeyState(VK_SHIFT) & 0x8000)) retire = true;
                        Snapshot snapshot;
                        if (!retire && ReadBrowser(c.browser.Get(), snapshot)) {
                            if (c.token && snapshot != c.snapshot) {
                                // Navigation or selection changed while Pulse was loading.
                                // Cancel before UI delivery, rather than waiting for an ACK.
                                retire = true;
                            } else if (c.token) {
                                bool answered = false, delivered = false;
                                { std::lock_guard lock(shared->mutex);
                                    const auto answer = shared->acknowledgments.find(c.token);
                                    if (answer != shared->acknowledgments.end()) {
                                        answered = true; delivered = answer->second;
                                        shared->acknowledgments.erase(answer);
                                    }
                                }
                                if (answered) {
                                    // Revalidate the source after delivery; navigation/selection changes preserve it.
                                    if (delivered && snapshot == c.snapshot && !shared->stop)
                                        shared->error = c.browser->Quit();
                                    retire = true;
                                }
                            } else if (snapshot != c.snapshot || !c.stable_since) {
                                c.snapshot = std::move(snapshot); c.stable_since = now;
                            } else if (now - c.stable_since >= 450) {
                                std::lock_guard lock(shared->mutex);
                                if (shared->requests.size() < 128 && !shared->stop) {
                                    c.token = shared->next_token++;
                                    shared->outstanding.insert(c.token);
                                    shared->requests.push_back({c.token, snapshot.folder, snapshot.selected});
                                    if (!PostMessageW(shared->window, shared->message, 0, 0)) {
                                        shared->requests.pop_back(); shared->outstanding.erase(c.token); retire = true;
                                    }
                                } else retire = true;
                            }
                        }
                        if (retire) {
                            if (c.token) { std::lock_guard lock(shared->mutex);
                                shared->outstanding.erase(c.token); shared->acknowledgments.erase(c.token); }
                            ignored.insert(it->first); it = candidates.erase(it);
                        } else ++it;
                    }
                    MsgWaitForMultipleObjects(1, &shared->wake, FALSE, 100, QS_ALLINPUT);
                    MSG message{};
                    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                        TranslateMessage(&message); DispatchMessageW(&message);
                    }
                }
                shared->ready = false;
                if (subscription) point->Unadvise(subscription);
                if (window_hook) UnhookWinEvent(window_hook);
                creation_bypass = nullptr;
            }
            { std::lock_guard lock(shared->mutex);
                shared->requests.clear(); shared->acknowledgments.clear(); shared->outstanding.clear();
            }
            if (!shared->stop) {
                MsgWaitForMultipleObjects(1, &shared->wake, FALSE, 1000, QS_ALLINPUT);
                MSG message{};
                while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                    TranslateMessage(&message); DispatchMessageW(&message);
                }
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
    Stop();
    std::lock_guard lifecycle(lifecycle_);
    if (!IsWindow(hwnd) || message < WM_APP || message > 0xBFFF) return false;
    shared_ = std::make_shared<Shared>(); shared_->window = hwnd; shared_->message = message;
    shared_->next_token = next_token_;
    if (!shared_->wake) { shared_.reset(); return false; }
    auto context = std::make_unique<std::shared_ptr<Shared>>(shared_);
    thread_ = CreateThread(nullptr, 0, RunTakeover, context.get(), 0, nullptr);
    if (!thread_) { shared_.reset(); return false; }
    context.release(); return true;
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
            retired->stop = true; retired->ready = false;
            next_token_ = retired->next_token.load() + 1;
            SetEvent(retired->wake);
        }
    }
    // Keep the UI mailboxes independent of a slow/disconnected COM shutdown.
    // Shared is retained by the STA until it can retire; never kill the thread.
    if (thread) { WaitForSingleObject(thread, 2000); CloseHandle(thread); }
}
} // namespace pulse::app
