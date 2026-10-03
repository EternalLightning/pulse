#include "shell_window_registry.h"
#include "shell_path.h"
#include <shlobj.h>
#include <exdisp.h>
#include <servprov.h>
#include <propvarutil.h>
#include <shlwapi.h>
#include <wrl/client.h>
#include <atomic>
#include <algorithm>
#include <map>
#include <mutex>
#include <utility>
#include <type_traits>

namespace pulse::app {
using Microsoft::WRL::ComPtr;
bool ShellSelectionFlagsNeedDelivery(unsigned flags) {
    return flags == SVSI_DESELECT ||
        (flags & (SVSI_SELECT | SVSI_EDIT | SVSI_FOCUSED | SVSI_ENSUREVISIBLE)) != 0;
}
struct ShellWindowRegistry::Shared {
    HWND window = nullptr;
    UINT message = 0;
    DWORD owner_thread = 0;
    HANDLE wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    std::mutex mutex;
    std::vector<ShellWindowEntry> wanted;
    std::vector<ShellSelectRequest> selections;
    std::atomic<bool> stop{false};
    std::atomic<HRESULT> error{S_OK};
    ~Shared() { if (wake) CloseHandle(wake); }
};
namespace {
using PidlItem = std::remove_pointer_t<PIDLIST_ABSOLUTE>;
struct PidlDelete { void operator()(PidlItem* p) const { CoTaskMemFree(p); } };
using Pidl = std::unique_ptr<PidlItem, PidlDelete>;
Pidl Parse(const std::wstring& path) {
    PIDLIST_ABSOLUTE p = nullptr;
    const auto shell_path = ShellPathText(path);
    const HRESULT hr = shell_path.empty() ? SHGetKnownFolderIDList(FOLDERID_ComputerFolder, 0, nullptr, &p)
        : SHParseDisplayName(shell_path.c_str(), nullptr, &p, 0, nullptr);
    return SUCCEEDED(hr) ? Pidl(p) : Pidl();
}
std::wstring Name(PCIDLIST_ABSOLUTE p, SIGDN kind) {
    PWSTR text = nullptr;
    if (!p || FAILED(SHGetNameFromIDList(p, kind, &text))) return {};
    std::wstring result = text;
    CoTaskMemFree(text);
    return result;
}
HRESULT StringResult(const std::wstring& value, BSTR* result) {
    if (!result) return E_POINTER;
    *result = SysAllocStringLen(value.data(), static_cast<UINT>(value.size()));
    return *result ? S_OK : E_OUTOFMEMORY;
}
// A single COM identity supplies the browser, document service and selection view.
class PaneBrowser final : public IWebBrowserApp, public IServiceProvider, public IShellView {
public:
    PaneBrowser(std::shared_ptr<ShellWindowRegistry::Shared> shared, ShellWindowEntry entry, Pidl folder)
        : shared_(std::move(shared)), entry_(std::move(entry)), folder_(std::move(folder)) {}
    void Retire() { folder_.reset(); }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (id == IID_IUnknown || id == IID_IDispatch || id == IID_IWebBrowser || id == IID_IWebBrowserApp)
            *out = static_cast<IWebBrowserApp*>(this);
        else if (id == IID_IServiceProvider) *out = static_cast<IServiceProvider*>(this);
        else if (id == IID_IOleWindow || id == IID_IShellView) *out = static_cast<IShellView*>(this);
        if (!*out) return E_NOINTERFACE;
        AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override { const ULONG n = --refs_; if (!n) delete this; return n; }
    HRESULT STDMETHODCALLTYPE QueryService(REFGUID service, REFIID id, void** out) override {
        if (service == IID_IFolderView || service == IID_IShellView) return QueryInterface(id, out);
        if (out) *out = nullptr;
        return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE GetTypeInfoCount(UINT* n) override { if (!n) return E_POINTER; *n = 0; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetTypeInfo(UINT, LCID, ITypeInfo** p) override { if (p) *p = nullptr; return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetIDsOfNames(REFIID, LPOLESTR*, UINT, LCID, DISPID*) override { return DISP_E_UNKNOWNNAME; }
    HRESULT STDMETHODCALLTYPE Invoke(DISPID, REFIID, LCID, WORD, DISPPARAMS*, VARIANT*, EXCEPINFO*, UINT*) override {
        return DISP_E_MEMBERNOTFOUND;
    }
    HRESULT STDMETHODCALLTYPE get_Document(IDispatch** p) override { return QueryInterface(IID_IDispatch, reinterpret_cast<void**>(p)); }
    HRESULT STDMETHODCALLTYPE get_Application(IDispatch** p) override { return get_Document(p); }
    HRESULT STDMETHODCALLTYPE get_Parent(IDispatch** p) override { return get_Document(p); }
    HRESULT STDMETHODCALLTYPE get_Container(IDispatch** p) override { return get_Document(p); }
    HRESULT STDMETHODCALLTYPE get_HWND(SHANDLE_PTR* p) override { if (!p) return E_POINTER; *p = reinterpret_cast<SHANDLE_PTR>(shared_->window); return S_OK; }
    HRESULT STDMETHODCALLTYPE get_Name(BSTR* p) override { return StringResult(L"Pulse", p); }
    HRESULT STDMETHODCALLTYPE get_LocationName(BSTR* p) override { return StringResult(Name(folder_.get(), SIGDN_NORMALDISPLAY), p); }
    HRESULT STDMETHODCALLTYPE get_LocationURL(BSTR* p) override {
        const auto path = Name(folder_.get(), SIGDN_FILESYSPATH);
        std::wstring url;
        if (!path.empty()) {
            DWORD n = static_cast<DWORD>(path.size() * 3 + 32);
            std::vector<wchar_t> buffer(n);
            if (SUCCEEDED(UrlCreateFromPathW(path.c_str(), buffer.data(), &n, 0))) url = buffer.data();
        }
        return StringResult(url, p);
    }
    HRESULT STDMETHODCALLTYPE get_Busy(VARIANT_BOOL* p) override { if (!p) return E_POINTER; *p = VARIANT_FALSE; return S_OK; }
    HRESULT STDMETHODCALLTYPE get_Visible(VARIANT_BOOL* p) override { if (!p) return E_POINTER; *p = IsWindowVisible(shared_->window) ? VARIANT_TRUE : VARIANT_FALSE; return S_OK; }
    HRESULT STDMETHODCALLTYPE get_TopLevelContainer(VARIANT_BOOL* p) override { if (!p) return E_POINTER; *p = VARIANT_TRUE; return S_OK; }
#define PULSE_COM_STUB(method, args) HRESULT STDMETHODCALLTYPE method args override { return E_NOTIMPL; }
    PULSE_COM_STUB(GoBack, ())
    PULSE_COM_STUB(GoForward, ())
    PULSE_COM_STUB(GoHome, ())
    PULSE_COM_STUB(GoSearch, ())
    PULSE_COM_STUB(Navigate, (BSTR, VARIANT*, VARIANT*, VARIANT*, VARIANT*))
    PULSE_COM_STUB(Refresh, ())
    PULSE_COM_STUB(Refresh2, (VARIANT*))
    PULSE_COM_STUB(Stop, ())
    PULSE_COM_STUB(get_Type, (BSTR*))
    PULSE_COM_STUB(get_Left, (long*))
    PULSE_COM_STUB(put_Left, (long))
    PULSE_COM_STUB(get_Top, (long*))
    PULSE_COM_STUB(put_Top, (long))
    PULSE_COM_STUB(get_Width, (long*))
    PULSE_COM_STUB(put_Width, (long))
    PULSE_COM_STUB(get_Height, (long*))
    PULSE_COM_STUB(put_Height, (long))
    PULSE_COM_STUB(Quit, ())
    PULSE_COM_STUB(ClientToWindow, (int*, int*))
    PULSE_COM_STUB(PutProperty, (BSTR, VARIANT))
    PULSE_COM_STUB(GetProperty, (BSTR, VARIANT*))
    PULSE_COM_STUB(get_FullName, (BSTR*))
    PULSE_COM_STUB(get_Path, (BSTR*))
    PULSE_COM_STUB(put_Visible, (VARIANT_BOOL))
    PULSE_COM_STUB(get_StatusBar, (VARIANT_BOOL*))
    PULSE_COM_STUB(put_StatusBar, (VARIANT_BOOL))
    PULSE_COM_STUB(get_StatusText, (BSTR*))
    PULSE_COM_STUB(put_StatusText, (BSTR))
    PULSE_COM_STUB(get_ToolBar, (int*))
    PULSE_COM_STUB(put_ToolBar, (int))
    PULSE_COM_STUB(get_MenuBar, (VARIANT_BOOL*))
    PULSE_COM_STUB(put_MenuBar, (VARIANT_BOOL))
    PULSE_COM_STUB(get_FullScreen, (VARIANT_BOOL*))
    PULSE_COM_STUB(put_FullScreen, (VARIANT_BOOL))
    HRESULT STDMETHODCALLTYPE GetWindow(HWND* p) override { if (!p) return E_POINTER; *p = shared_->window; return S_OK; }
    PULSE_COM_STUB(ContextSensitiveHelp, (BOOL))
    HRESULT STDMETHODCALLTYPE TranslateAccelerator(MSG*) override { return S_FALSE; }
    PULSE_COM_STUB(EnableModeless, (BOOL))
    PULSE_COM_STUB(UIActivate, (UINT))
    PULSE_COM_STUB(CreateViewWindow, (IShellView*, LPCFOLDERSETTINGS, IShellBrowser*, RECT*, HWND*))
    PULSE_COM_STUB(DestroyViewWindow, ())
    PULSE_COM_STUB(GetCurrentInfo, (LPFOLDERSETTINGS))
    PULSE_COM_STUB(AddPropertySheetPages, (DWORD, LPFNSVADDPROPSHEETPAGE, LPARAM))
    PULSE_COM_STUB(SaveViewState, ())
    PULSE_COM_STUB(GetItemObject, (UINT, REFIID, void**))
#undef PULSE_COM_STUB
    HRESULT STDMETHODCALLTYPE SelectItem(PCUITEMID_CHILD item, SVSIF flags) override {
        if (!item) return E_INVALIDARG;
        if (!ShellSelectionFlagsNeedDelivery(static_cast<unsigned>(flags))) return S_OK;
        if (!folder_ || shared_->stop) return HRESULT_FROM_WIN32(ERROR_CANCELLED);
        Pidl absolute(ILCombine(folder_.get(), item));
        const auto path = Name(absolute.get(), SIGDN_DESKTOPABSOLUTEPARSING);
        if (path.empty()) return E_INVALIDARG;
        std::lock_guard lock(shared_->mutex);
        if (shared_->stop) return HRESULT_FROM_WIN32(ERROR_CANCELLED);
        if (shared_->selections.size() >= 4096) return HRESULT_FROM_WIN32(ERROR_NOT_ENOUGH_QUOTA);
        shared_->selections.push_back({entry_.key, entry_.path, path, static_cast<unsigned>(flags)});
        if (!PostMessageW(shared_->window, shared_->message, 0, 0)) {
            shared_->selections.pop_back(); return HRESULT_FROM_WIN32(GetLastError());
        }
        return S_OK;
    }
private:
    ~PaneBrowser() = default;
    std::atomic<ULONG> refs_{1};
    std::shared_ptr<ShellWindowRegistry::Shared> shared_;
    ShellWindowEntry entry_;
    Pidl folder_;
};
struct Registration { ComPtr<PaneBrowser> browser; long pending = 0; long window = 0; std::wstring path; };
DWORD WINAPI RunRegistry(void* context) {
    std::unique_ptr<std::shared_ptr<ShellWindowRegistry::Shared>> holder(
        static_cast<std::shared_ptr<ShellWindowRegistry::Shared>*>(context));
    auto shared = *holder;
    const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (FAILED(init)) { shared->error = init; return 1; }
    {
        ComPtr<IShellWindows> windows;
        std::map<uint64_t, Registration> registered;
        auto revoke = [&](Registration& r) {
            r.browser->Retire();
            if (r.pending) windows->Revoke(r.pending);
            if (r.window && r.window != r.pending) windows->Revoke(r.window);
        };
        while (!shared->stop) {
            if (!windows) shared->error = CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_LOCAL_SERVER,
                IID_PPV_ARGS(&windows));
            if (windows) {
                long count = 0;
                const HRESULT probe = windows->get_Count(&count);
                if (SUCCEEDED(probe)) shared->error = S_OK;
                if (FAILED(probe)) {
                    // Explorer restarted: retire stale proxies and register again on the next wake.
                    shared->error = probe;
                    for (auto& [key, r] : registered) { (void)key; r.browser->Retire(); }
                    registered.clear(); windows.Reset();
                }
            }
            if (windows) {
                std::vector<ShellWindowEntry> wanted;
                { std::lock_guard lock(shared->mutex); wanted = shared->wanted; }
                for (auto it = registered.begin(); it != registered.end();) {
                    const auto found = std::find_if(wanted.begin(), wanted.end(), [&](const auto& e) {
                        return e.key == it->first && SameShellPath(e.path, it->second.path);
                    });
                    if (found == wanted.end()) { revoke(it->second); it = registered.erase(it); }
                    else ++it;
                }
                for (const auto& entry : wanted) {
                    if (!entry.key || registered.contains(entry.key) || shared->stop) continue;
                    Pidl pidl = Parse(entry.path);
                    if (!pidl) continue;
                    VARIANT location{}; VARIANT root{};
                    HRESULT hr = InitVariantFromBuffer(pidl.get(), ILGetSize(pidl.get()), &location);
                    if (FAILED(hr)) { shared->error = hr; continue; }
                    Registration r;
                    r.path = entry.path;
                    r.browser.Attach(new PaneBrowser(shared, entry, std::move(pidl)));
                    hr = windows->RegisterPending(static_cast<long>(shared->owner_thread), &location, &root,
                        SWC_BROWSER, &r.pending);
                    if (SUCCEEDED(hr)) hr = windows->Register(static_cast<IWebBrowserApp*>(r.browser.Get()),
                        HandleToLong(shared->window), SWC_BROWSER, &r.window);
                    if (SUCCEEDED(hr)) hr = windows->OnNavigate(r.pending, &location);
                    if (SUCCEEDED(hr) && r.window != r.pending) hr = windows->OnNavigate(r.window, &location);
                    VariantClear(&location);
                    shared->error = hr;
                    if (FAILED(hr)) revoke(r);
                    else registered.emplace(entry.key, std::move(r));
                }
            }
            MsgWaitForMultipleObjects(1, &shared->wake, FALSE, 1000, QS_ALLINPUT);
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&message); DispatchMessageW(&message);
            }
        }
        for (auto& [key, r] : registered) { (void)key; revoke(r); }
    }
    CoUninitialize();
    return 0;
}
}
ShellWindowRegistry::ShellWindowRegistry() = default;
ShellWindowRegistry::~ShellWindowRegistry() { Stop(); }
bool ShellWindowRegistry::Start(HWND hwnd, UINT message) {
    Stop();
    std::lock_guard lifecycle(lifecycle_);
    if (!IsWindow(hwnd) || message < WM_APP || message > 0xBFFF) return false;
    shared_ = std::make_shared<Shared>();
    shared_->window = hwnd; shared_->message = message;
    shared_->owner_thread = GetWindowThreadProcessId(hwnd, nullptr);
    if (!shared_->wake) { shared_.reset(); return false; }
    auto context = std::make_unique<std::shared_ptr<Shared>>(shared_);
    thread_ = CreateThread(nullptr, 0, RunRegistry, context.get(), 0, nullptr);
    if (!thread_) { shared_.reset(); return false; }
    context.release(); return true;
}
void ShellWindowRegistry::Publish(std::vector<ShellWindowEntry> entries) {
    std::lock_guard lifecycle(lifecycle_);
    if (!shared_) return;
    { std::lock_guard lock(shared_->mutex); shared_->wanted = std::move(entries); }
    SetEvent(shared_->wake);
}
std::vector<ShellSelectRequest> ShellWindowRegistry::DrainSelections() {
    std::lock_guard lifecycle(lifecycle_);
    if (!shared_) return {};
    std::lock_guard lock(shared_->mutex);
    std::vector<ShellSelectRequest> result;
    result.swap(shared_->selections); return result;
}
HRESULT ShellWindowRegistry::LastError() const {
    std::lock_guard lifecycle(lifecycle_);
    return shared_ ? shared_->error.load() : S_OK;
}
void ShellWindowRegistry::Stop() {
    std::shared_ptr<Shared> retired;
    HANDLE thread = nullptr;
    {
        std::lock_guard lifecycle(lifecycle_);
        retired = std::move(shared_); thread = std::exchange(thread_, nullptr);
        if (retired) { retired->stop = true; SetEvent(retired->wake); }
    }
    // The worker owns only Shared. A hung Explorer may outlive this service,
    // but cannot hold the UI's Publish/Drain/LastError behind a lifecycle lock.
    if (thread) { WaitForSingleObject(thread, 2000); CloseHandle(thread); }
}
} // namespace pulse::app
