// SPDX-License-Identifier: Apache-2.0
// Adapted from jimmgreen/pulse at b32436831a066e18448b2810bef4bf989024fc68.
// Modified for this Pulse distribution. See LICENSE and NOTICE.

#include "file_dialog_wrapper.h"

#include "file_dialog_client_state.h"
#include "file_dialog_events.h"
#include "file_dialog_validation.h"
#include "picker_shell_results.h"
#include "../common/file_dialog_bridge_bypass.h"
#include "../ipc/picker_client.h"

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <iterator>
#include <memory>
#include <new>
#include <shlobj.h>
#include <wrl/client.h>

namespace pulse::shell_bridge {
namespace {
namespace wire = ipc::picker;
using Microsoft::WRL::ComPtr;

struct TaskFree {
    void operator()(wchar_t* text) const noexcept { CoTaskMemFree(text); }
};

bool CopyText(PCWSTR text, std::u16string& output) {
    if (!text) return false;
    const auto size = wcsnlen_s(text, wire::kMaxStringUnits + 1);
    if (size > wire::kMaxStringUnits) return false;
    output.assign(text, text + size);
    return true;
}

bool FilesystemFolder(IShellItem* folder, std::u16string& path) {
    constexpr SFGAOF required = SFGAO_FILESYSTEM | SFGAO_FOLDER;
    SFGAOF attributes = 0;
    if (!folder || FAILED(folder->GetAttributes(required, &attributes)) || (attributes & required) != required) return false;
    PWSTR raw = nullptr;
    const auto hr = folder->GetDisplayName(SIGDN_FILESYSPATH, &raw);
    std::unique_ptr<wchar_t, TaskFree> owned(raw);
    return SUCCEEDED(hr) && CopyText(raw, path);
}

class FileDialog final : public IFileOpenDialog, public IFileSaveDialog, public IFileDialog2,
                         public IFileDialogCustomize, public IOleWindow, public IObjectWithSite {
public:
    FileDialog(DialogFactory factory, ComPtr<IFileDialog> native, const FileDialogBridgeServices& services)
        : factory_(factory), native_(std::move(native)), services_(services) {
        event_owner_->dialog = Identity();
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** output) noexcept override {
        if (!output) return E_POINTER;
        *output = nullptr;
        if (iid == IID_IUnknown || iid == IID_IModalWindow || iid == IID_IFileDialog)
            *output = Identity();
        else if (iid == IID_IFileOpenDialog && factory_ == DialogFactory::Open)
            *output = static_cast<IFileOpenDialog*>(this);
        else if (iid == IID_IFileSaveDialog && factory_ == DialogFactory::Save)
            *output = static_cast<IFileSaveDialog*>(this);
        else if (iid == IID_IFileDialog2 && NativeSupports(iid))
            *output = static_cast<IFileDialog2*>(this);
        else if (iid == IID_IFileDialogCustomize && NativeSupports(iid))
            *output = static_cast<IFileDialogCustomize*>(this);
        else if (iid == IID_IOleWindow && NativeSupports(iid))
            *output = static_cast<IOleWindow*>(this);
        else if (iid == IID_IObjectWithSite && NativeSupports(iid))
            *output = static_cast<IObjectWithSite*>(this);
        else return E_NOINTERFACE;
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() noexcept override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() noexcept override {
        ULONG count;
        {
            std::lock_guard lock(event_owner_->mutex);
            count = --references_;
            if (!count) event_owner_->dialog = nullptr;
        }
        if (!count) delete this;
        return count;
    }

    HRESULT STDMETHODCALLTYPE Show(HWND owner) noexcept override {
        if (!OnThread()) return RPC_E_WRONG_THREAD;
        if (showing_) return E_UNEXPECTED;
        AddRef();
        struct Hold {
            FileDialog* dialog;
            ~Hold() { dialog->showing_ = false; dialog->Release(); }
        } hold{this};
        showing_ = true;
        bridge_result_ = false;
        close_requested_ = false;
        ClearResult();
        try {
            if (GetKeyState(VK_SHIFT) & 0x8000) return ShowNativeWithoutTakeover(owner);
            if ((services_.enabled && !services_.enabled()) ||
                (!services_.run_bridge && services_.executable.empty())) return native_->Show(owner);
            if (native_only_) return ShowNative(owner);
            if (!services_.run_bridge && GetFileAttributesW(services_.executable.c_str()) == INVALID_FILE_ATTRIBUTES)
                return ShowNative(owner);
            if (owner) {
                DWORD process = 0;
                const DWORD thread = GetWindowThreadProcessId(owner, &process);
                if (!thread || (process != GetCurrentProcessId() && IsWindowEnabled(owner)))
                    return ShowNative(owner);
            }
            auto request = Prepare();
            if (!request) return ShowNative(owner);
            if (group_open_) return ShowNative(owner);
            wire::DialogEventHandler on_event;
            if (!subscriptions_.empty()) {
                if (services_.run_bridge) return ShowNative(owner);
                auto& body = std::get<wire::Request>(request->body);
                body.required_capabilities |= wire::DialogEvents;
                if (button_) {
                    body.required_capabilities |= wire::DialogButton;
                    body.button_label = button_->label;
                }
                on_event = [&](const wire::DialogEvent& event) { return ReportEvent(body.operation, event); };
            } else if (button_ || group_open_) {
                return ShowNative(owner);
            }

            cancel_.Reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
            if (!cancel_) return ShowNative(owner);
            bridge_result_ = true;
            const auto response = services_.run_bridge ? services_.run_bridge(*request, owner, cancel_.Get()) :
                wire::RunPickerClient(services_.executable, *request, owner, cancel_.Get(), on_event);
            cancel_.Reset();
            if (close_requested_) return close_result_;
            if (!wire::ValidateResponse(*request, response)) return E_UNEXPECTED;
            const auto& result = std::get<wire::Response>(response.body);
            if (result.status == wire::Status::Cancelled) return HRESULT_FROM_WIN32(ERROR_CANCELLED);
            if (result.status == wire::Status::UseNative) {
                bridge_result_ = false;
                return ShowNativeWithoutTakeover(owner);
            }
            if (result.status == wire::Status::Unsupported && result.error_source == wire::ErrorSource::Win32 &&
                result.error_code == ERROR_NOT_SUPPORTED) {
                bridge_result_ = false;
                return ShowNative(owner);
            }
            if (result.status == wire::Status::Failed || result.status == wire::Status::Unsupported)
                return WireError(result);
            const auto status = StoreResult(result);
            if (SUCCEEDED(status)) CommitResult();
            return status;
        } catch (const std::bad_alloc&) {
            cancel_.Reset();
            return E_OUTOFMEMORY;
        } catch (...) {
            cancel_.Reset();
            return E_FAIL;
        }
    }

    HRESULT STDMETHODCALLTYPE SetFileTypes(UINT count, const COMDLG_FILTERSPEC* filters) override {
        if (const auto hr = Mutable(); FAILED(hr)) return hr;
        const HRESULT hr = native_->SetFileTypes(count, filters);
        return Capture(Setting::FileTypes, hr, [&](SetterCall& call) {
            if (count > wire::kMaxFilters || (count && !filters)) return false;
            for (UINT i = 0; i < count; ++i) {
                wire::Filter filter;
                if (!CopyText(filters[i].pszName, filter.label) || !CopyText(filters[i].pszSpec, filter.pattern)) return false;
                call.filters.push_back(std::move(filter));
            }
            return true;
        });
    }
    HRESULT STDMETHODCALLTYPE SetFileTypeIndex(UINT index) override {
        if (const auto hr = Mutable(); FAILED(hr)) return hr;
        const HRESULT hr = native_->SetFileTypeIndex(index);
        return Capture(Setting::FileTypeIndex, hr, [&](SetterCall& call) {
            call.value = index;
            UINT observed = 0;
            if (native_->GetFileTypeIndex(&observed) != S_OK) return false;
            call.observed_filter_index = observed;
            return true;
        });
    }
    HRESULT STDMETHODCALLTYPE GetFileTypeIndex(UINT* index) override {
        if (!index) return E_POINTER;
        if (!OnThread()) return RPC_E_WRONG_THREAD;
        if (!bridge_result_) return native_->GetFileTypeIndex(index);
        if (event_) { *index = event_->filter_index + 1; return S_OK; }
        *index = result_index_;
        return item_ ? S_OK : E_UNEXPECTED;
    }
    HRESULT STDMETHODCALLTYPE Advise(IFileDialogEvents* sink, DWORD* cookie) override {
        if (!OnThread()) return RPC_E_WRONG_THREAD;
        if (showing_ && bridge_result_) return E_UNEXPECTED;
        if (!sink || !cookie) return native_->Advise(sink, cookie);
        ComPtr<IFileDialogEvents> proxy;
        const auto created = detail::CreateFileDialogEventProxy(event_owner_, sink, &proxy);
        if (FAILED(created)) return created;
        const auto hr = native_->Advise(proxy.Get(), cookie);
        if (FAILED(hr)) return hr;
        try { subscriptions_.push_back({*cookie, std::move(proxy)}); }
        catch (...) {
            native_->Unadvise(*cookie);
            *cookie = 0;
            return E_OUTOFMEMORY;
        }
        return hr;
    }
    HRESULT STDMETHODCALLTYPE Unadvise(DWORD cookie) override {
        if (!OnThread()) return RPC_E_WRONG_THREAD;
        const auto hr = native_->Unadvise(cookie);
        if (SUCCEEDED(hr)) std::erase_if(subscriptions_, [&](const Subscription& sub) { return sub.cookie == cookie; });
        return hr;
    }
    HRESULT STDMETHODCALLTYPE SetOptions(FILEOPENDIALOGOPTIONS options) override {
        if (const auto hr = Mutable(); FAILED(hr)) return hr;
        const HRESULT hr = native_->SetOptions(options);
        return Capture(Setting::Options, hr, [&](SetterCall& call) { call.value = options; return true; });
    }
    HRESULT STDMETHODCALLTYPE GetOptions(FILEOPENDIALOGOPTIONS* options) override {
        return OnThread() ? native_->GetOptions(options) : RPC_E_WRONG_THREAD;
    }
    HRESULT STDMETHODCALLTYPE SetDefaultFolder(IShellItem* folder) override {
        if (const auto hr = Mutable(); FAILED(hr)) return hr;
        return Capture(Setting::DefaultFolder, native_->SetDefaultFolder(folder), [](SetterCall&) { return true; });
    }
    HRESULT STDMETHODCALLTYPE SetFolder(IShellItem* folder) override {
        if (const auto hr = Mutable(); FAILED(hr)) return hr;
        const HRESULT hr = native_->SetFolder(folder);
        return Capture(Setting::Folder, hr, [&](SetterCall& call) {
            call.filesystem_folder = FilesystemFolder(folder, call.text);
            return call.filesystem_folder;
        });
    }
    HRESULT STDMETHODCALLTYPE GetFolder(IShellItem** folder) override {
        if (!folder) return E_POINTER;
        *folder = nullptr;
        if (!OnThread()) return RPC_E_WRONG_THREAD;
        if (!bridge_result_) return native_->GetFolder(folder);
        if (event_) return EventFolder(event_->folder, folder);
        return item_ ? item_->GetParent(folder) : E_UNEXPECTED;
    }
    HRESULT STDMETHODCALLTYPE GetCurrentSelection(IShellItem** item) override {
        if (!item) return E_POINTER;
        *item = nullptr;
        if (!OnThread()) return RPC_E_WRONG_THREAD;
        if (bridge_result_) {
            if (!event_) return item_ ? item_.CopyTo(item) : E_UNEXPECTED;
            if (event_->selections.empty()) return E_UNEXPECTED;
            return EventItem(event_->selections.front(), item);
        }
        native_only_ = true;
        return native_->GetCurrentSelection(item);
    }
    HRESULT STDMETHODCALLTYPE SetFileName(PCWSTR text) override { return Text(Setting::FileName, text, &IFileDialog::SetFileName); }
    HRESULT STDMETHODCALLTYPE SetTitle(PCWSTR text) override { return Text(Setting::Title, text, &IFileDialog::SetTitle); }
    HRESULT STDMETHODCALLTYPE SetOkButtonLabel(PCWSTR text) override { return Text(Setting::OkButtonLabel, text, &IFileDialog::SetOkButtonLabel); }
    HRESULT STDMETHODCALLTYPE SetFileNameLabel(PCWSTR text) override { return Text(Setting::FileNameLabel, text, &IFileDialog::SetFileNameLabel); }
    HRESULT STDMETHODCALLTYPE SetDefaultExtension(PCWSTR text) override { return Text(Setting::DefaultExtension, text, &IFileDialog::SetDefaultExtension); }
    HRESULT STDMETHODCALLTYPE GetFileName(PWSTR* text) override {
        if (!text) return E_POINTER;
        *text = nullptr;
        if (!OnThread()) return RPC_E_WRONG_THREAD;
        if (!bridge_result_) return native_->GetFileName(text);
        const std::wstring_view filename = event_
            ? std::wstring_view(reinterpret_cast<const wchar_t*>(event_->filename.data()), event_->filename.size())
            : std::wstring_view(filename_);
        if (!event_ && (!item_ || filename.empty())) return E_UNEXPECTED;
        const auto bytes = (filename.size() + 1) * sizeof(wchar_t);
        auto* copy = static_cast<PWSTR>(CoTaskMemAlloc(bytes));
        if (!copy) return E_OUTOFMEMORY;
        std::copy(filename.begin(), filename.end(), copy);
        copy[filename.size()] = L'\0';
        *text = copy;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetResult(IShellItem** item) override {
        if (!item) return E_POINTER;
        *item = nullptr;
        if (!OnThread()) return RPC_E_WRONG_THREAD;
        if (!bridge_result_) return native_->GetResult(item);
        return item_ && result_count_ == 1 ? item_.CopyTo(item) : E_UNEXPECTED;
    }
    HRESULT STDMETHODCALLTYPE AddPlace(IShellItem* item, FDAP location) override {
        return NativeOnly<IFileDialog>([&](auto* dialog) { return dialog->AddPlace(item, location); });
    }
    HRESULT STDMETHODCALLTYPE Close(HRESULT result) override {
        if (!OnThread()) return RPC_E_WRONG_THREAD;
        if (!showing_ || !bridge_result_ || !cancel_) return native_->Close(result);
        if (!SetEvent(cancel_.Get())) return HRESULT_FROM_WIN32(GetLastError());
        close_requested_ = true;
        close_result_ = result;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetClientGuid(REFGUID guid) override {
        if (const auto hr = Mutable(); FAILED(hr)) return hr;
        const auto hr = native_->SetClientGuid(guid);
        return Capture(Setting::ClientGuid, hr, [&](SetterCall&) {
            if (!client_state_) client_state_.emplace();
            client_guid_ = guid;
            return true;
        });
    }
    HRESULT STDMETHODCALLTYPE ClearClientData() override {
        if (const auto hr = Mutable(); FAILED(hr)) return hr;
        const auto hr = native_->ClearClientData();
        return Capture(Setting::ClearClientData, hr, [&](SetterCall&) {
            return client_state_ && client_guid_ && client_state_->Clear(*client_guid_);
        });
    }
    HRESULT STDMETHODCALLTYPE SetFilter(IShellItemFilter* filter) override {
        return NativeOnly<IFileDialog>([&](auto* dialog) { return dialog->SetFilter(filter); });
    }

    HRESULT STDMETHODCALLTYPE GetResults(IShellItemArray** items) override {
        if (!items) return E_POINTER;
        *items = nullptr;
        if (!OnThread()) return RPC_E_WRONG_THREAD;
        if (bridge_result_) return items_ ? items_.CopyTo(items) : E_UNEXPECTED;
        return NativeCall<IFileOpenDialog>([&](auto* dialog) { return dialog->GetResults(items); });
    }
    HRESULT STDMETHODCALLTYPE GetSelectedItems(IShellItemArray** items) override {
        if (!items) return E_POINTER;
        *items = nullptr;
        if (!OnThread()) return RPC_E_WRONG_THREAD;
        if (bridge_result_) return event_ ? EventSelection(items) : items_ ? items_.CopyTo(items) : E_UNEXPECTED;
        native_only_ = true;
        return NativeCall<IFileOpenDialog>([&](auto* dialog) { return dialog->GetSelectedItems(items); });
    }
    HRESULT STDMETHODCALLTYPE SetSaveAsItem(IShellItem* item) override {
        return NativeOnly<IFileSaveDialog>([&](auto* dialog) { return dialog->SetSaveAsItem(item); });
    }
    HRESULT STDMETHODCALLTYPE SetProperties(IPropertyStore* properties) override {
        return NativeOnly<IFileSaveDialog>([&](auto* dialog) { return dialog->SetProperties(properties); });
    }
    HRESULT STDMETHODCALLTYPE SetCollectedProperties(IPropertyDescriptionList* properties, BOOL append) override {
        return NativeOnly<IFileSaveDialog>([&](auto* dialog) { return dialog->SetCollectedProperties(properties, append); });
    }
    HRESULT STDMETHODCALLTYPE GetProperties(IPropertyStore** properties) override {
        if (!properties) return E_POINTER;
        *properties = nullptr;
        if (!OnThread()) return RPC_E_WRONG_THREAD;
        if (bridge_result_) return E_NOTIMPL;
        return NativeOnly<IFileSaveDialog>([&](auto* dialog) { return dialog->GetProperties(properties); });
    }
    HRESULT STDMETHODCALLTYPE ApplyProperties(IShellItem* item, IPropertyStore* properties, HWND owner,
                                             IFileOperationProgressSink* sink) override {
        if (!OnThread()) return RPC_E_WRONG_THREAD;
        if (bridge_result_) return E_NOTIMPL;
        return NativeOnly<IFileSaveDialog>([&](auto* dialog) { return dialog->ApplyProperties(item, properties, owner, sink); });
    }
    HRESULT STDMETHODCALLTYPE SetCancelButtonLabel(PCWSTR text) override {
        return NativeOnly<IFileDialog2>([&](auto* dialog) { return dialog->SetCancelButtonLabel(text); });
    }
    HRESULT STDMETHODCALLTYPE SetNavigationRoot(IShellItem* root) override {
        return NativeOnly<IFileDialog2>([&](auto* dialog) { return dialog->SetNavigationRoot(root); });
    }

    HRESULT STDMETHODCALLTYPE AddPushButton(DWORD id, PCWSTR text) override {
        if (const auto hr = Mutable(); FAILED(hr)) return hr;
        const auto hr = NativeCall<IFileDialogCustomize>([&](auto* dialog) { return dialog->AddPushButton(id, text); });
        if (hr == S_OK && !native_only_) {
            try {
                Button button{id, {}};
                if (!button_ && CopyText(text, button.label) && !button.label.empty() &&
                    button.label.size() <= 128 && button.label.find_first_of(u"\r\n\t") == button.label.npos) {
                    button_ = std::move(button);
                } else native_only_ = true;
            } catch (...) { native_only_ = true; }
        } else if (hr != S_OK) native_only_ = true;
        return hr;
    }
    HRESULT STDMETHODCALLTYPE StartVisualGroup(DWORD id, PCWSTR text) override {
        if (const auto hr = Mutable(); FAILED(hr)) return hr;
        const auto hr = NativeCall<IFileDialogCustomize>([&](auto* dialog) { return dialog->StartVisualGroup(id, text); });
        if (hr == S_OK && !group_open_ && !had_group_ && text && !*text) group_open_ = had_group_ = true;
        else native_only_ = true;
        return hr;
    }
    HRESULT STDMETHODCALLTYPE EndVisualGroup() override {
        if (const auto hr = Mutable(); FAILED(hr)) return hr;
        const auto hr = NativeCall<IFileDialogCustomize>([](auto* dialog) { return dialog->EndVisualGroup(); });
        if (hr == S_OK && group_open_ && button_) group_open_ = false;
        else native_only_ = true;
        return hr;
    }
    HRESULT STDMETHODCALLTYPE GetControlState(DWORD id, CDCONTROLSTATEF* state) override {
        if (!OnThread()) return RPC_E_WRONG_THREAD;
        return NativeCall<IFileDialogCustomize>([&](auto* dialog) { return dialog->GetControlState(id, state); });
    }

    // Remaining customization calls retain native behavior.
    #define PULSE_FORWARD_CUSTOM(method, args, call) \
        HRESULT STDMETHODCALLTYPE method args override { \
            return NativeOnly<IFileDialogCustomize>([&](auto* dialog) { return dialog->method call; }); \
        }
    PULSE_FORWARD_CUSTOM(EnableOpenDropDown, (DWORD id), (id))
    PULSE_FORWARD_CUSTOM(AddMenu, (DWORD id, PCWSTR text), (id, text))
    PULSE_FORWARD_CUSTOM(AddComboBox, (DWORD id), (id))
    PULSE_FORWARD_CUSTOM(AddRadioButtonList, (DWORD id), (id))
    PULSE_FORWARD_CUSTOM(AddCheckButton, (DWORD id, PCWSTR text, BOOL checked), (id, text, checked))
    PULSE_FORWARD_CUSTOM(AddEditBox, (DWORD id, PCWSTR text), (id, text))
    PULSE_FORWARD_CUSTOM(AddSeparator, (DWORD id), (id))
    PULSE_FORWARD_CUSTOM(AddText, (DWORD id, PCWSTR text), (id, text))
    PULSE_FORWARD_CUSTOM(SetControlLabel, (DWORD id, PCWSTR text), (id, text))
    PULSE_FORWARD_CUSTOM(SetControlState, (DWORD id, CDCONTROLSTATEF state), (id, state))
    PULSE_FORWARD_CUSTOM(GetEditBoxText, (DWORD id, PWSTR* text), (id, text))
    PULSE_FORWARD_CUSTOM(SetEditBoxText, (DWORD id, PCWSTR text), (id, text))
    PULSE_FORWARD_CUSTOM(GetCheckButtonState, (DWORD id, BOOL* checked), (id, checked))
    PULSE_FORWARD_CUSTOM(SetCheckButtonState, (DWORD id, BOOL checked), (id, checked))
    PULSE_FORWARD_CUSTOM(AddControlItem, (DWORD id, DWORD item, PCWSTR text), (id, item, text))
    PULSE_FORWARD_CUSTOM(RemoveControlItem, (DWORD id, DWORD item), (id, item))
    PULSE_FORWARD_CUSTOM(RemoveAllControlItems, (DWORD id), (id))
    PULSE_FORWARD_CUSTOM(GetControlItemState, (DWORD id, DWORD item, CDCONTROLSTATEF* state), (id, item, state))
    PULSE_FORWARD_CUSTOM(SetControlItemState, (DWORD id, DWORD item, CDCONTROLSTATEF state), (id, item, state))
    PULSE_FORWARD_CUSTOM(GetSelectedControlItem, (DWORD id, DWORD* item), (id, item))
    PULSE_FORWARD_CUSTOM(SetSelectedControlItem, (DWORD id, DWORD item), (id, item))
    PULSE_FORWARD_CUSTOM(MakeProminent, (DWORD id), (id))
    PULSE_FORWARD_CUSTOM(SetControlItemText, (DWORD id, DWORD item, PCWSTR text), (id, item, text))
    #undef PULSE_FORWARD_CUSTOM

    HRESULT STDMETHODCALLTYPE GetWindow(HWND* window) override {
        if (!window) return E_POINTER;
        *window = nullptr;
        if (!OnThread()) return RPC_E_WRONG_THREAD;
        if (bridge_result_) {
            if (!event_ || !IsWindow(EventWindow())) return E_FAIL;
            *window = EventWindow();
            return S_OK;
        }
        return NativeCall<IOleWindow>([&](auto* dialog) { return dialog->GetWindow(window); });
    }
    HRESULT STDMETHODCALLTYPE ContextSensitiveHelp(BOOL enter) override {
        return NativeOnly<IOleWindow>([&](auto* dialog) { return dialog->ContextSensitiveHelp(enter); });
    }
    HRESULT STDMETHODCALLTYPE SetSite(IUnknown* site) override {
        return NativeOnly<IObjectWithSite>([&](auto* dialog) { return dialog->SetSite(site); });
    }
    HRESULT STDMETHODCALLTYPE GetSite(REFIID iid, void** output) override {
        if (!output) return E_POINTER;
        *output = nullptr;
        if (!OnThread()) return RPC_E_WRONG_THREAD;
        return NativeCall<IObjectWithSite>([&](auto* dialog) { return dialog->GetSite(iid, output); });
    }

private:
    struct Subscription { DWORD cookie; ComPtr<IFileDialogEvents> proxy; };
    struct Button { DWORD id; std::u16string label; };

    bool OnThread() const { return GetCurrentThreadId() == thread_; }
    HRESULT Mutable() const {
        if (!OnThread()) return RPC_E_WRONG_THREAD;
        return showing_ && bridge_result_ ? E_UNEXPECTED : S_OK;
    }
    IFileDialog* Identity() {
        if (factory_ == DialogFactory::Open) return static_cast<IFileOpenDialog*>(this);
        return static_cast<IFileSaveDialog*>(this);
    }
    bool NativeSupports(REFIID iid) const {
        ComPtr<IUnknown> queried;
        return SUCCEEDED(native_->QueryInterface(iid, reinterpret_cast<void**>(queried.GetAddressOf())));
    }
    HRESULT ShowNativeWithoutTakeover(HWND owner) {
        const dialog_bridge::NativeFallbackScope bypass;
        return native_->Show(owner);
    }
    HRESULT ShowNative(HWND owner) {
        FILEOPENDIALOGOPTIONS options = 0;
        const bool open = factory_ == DialogFactory::Open && native_->GetOptions(&options) == S_OK &&
            !(options & FOS_PICKFOLDERS);
        using Mode = dialog_bridge::NativeMode;
        const dialog_bridge::NativeModeScope mode(owner ? GetAncestor(owner, GA_ROOT) : nullptr,
            open ? Mode::Open : factory_ == DialogFactory::Save ? Mode::Save : Mode::Unknown);
        return native_->Show(owner);
    }

    template<class Interface, class Call>
    HRESULT NativeCall(Call call) {
        ComPtr<Interface> target;
        const auto hr = native_.As(&target);
        return SUCCEEDED(hr) ? call(target.Get()) : hr;
    }
    template<class Interface, class Call>
    HRESULT NativeOnly(Call call) {
        if (const auto hr = Mutable(); FAILED(hr)) return hr;
        native_only_ = true;
        return NativeCall<Interface>(std::move(call));
    }
    template<class Snapshot>
    HRESULT Capture(Setting setting, HRESULT hr, Snapshot snapshot) noexcept {
        if (native_only_) return hr;
        if (calls_.size() >= 256) { native_only_ = true; return hr; }
        try {
            SetterCall call;
            call.setting = setting;
            call.native_result = hr;
            if (hr == S_OK && !snapshot(call)) native_only_ = true;
            else calls_.push_back(std::move(call));
        } catch (...) { native_only_ = true; }
        return hr;
    }
    HRESULT Text(Setting setting, PCWSTR text, HRESULT (STDMETHODCALLTYPE IFileDialog::*setter)(PCWSTR)) {
        if (const auto hr = Mutable(); FAILED(hr)) return hr;
        const HRESULT hr = (native_.Get()->*setter)(text);
        return Capture(setting, hr, [&](SetterCall& call) { return CopyText(text, call.text); });
    }

    HWND EventWindow() const {
        return reinterpret_cast<HWND>(static_cast<uintptr_t>(event_->window));
    }
    HRESULT EventItem(const std::u16string& path, IShellItem** item) const {
        const std::wstring text(path.begin(), path.end());
        HRESULT hr = CreatePickerShellItem(wire::Operation::OpenFiles, text, item);
        if (hr == HRESULT_FROM_WIN32(ERROR_DIRECTORY))
            hr = CreatePickerShellItem(wire::Operation::PickFolder, text, item);
        return hr;
    }
    HRESULT EventFolder(const std::u16string& folder, IShellItem** item) const {
        return folder.empty() ? SHGetKnownFolderItem(FOLDERID_ComputerFolder, KF_FLAG_DEFAULT, nullptr, IID_PPV_ARGS(item))
                              : CreatePickerShellItem(wire::Operation::PickFolder, {folder.begin(), folder.end()}, item);
    }
    HRESULT EventSelection(IShellItemArray** items) const try {
        if (event_->selections.empty()) return E_UNEXPECTED;
        // The view can select folders even when Show ultimately returns files.
        std::vector<PCIDLIST_ABSOLUTE> ids;
        ids.reserve(event_->selections.size());
        struct FreeIds {
            std::vector<PCIDLIST_ABSOLUTE>& ids;
            ~FreeIds() { for (auto id : ids) CoTaskMemFree(const_cast<PIDLIST_ABSOLUTE>(id)); }
        } free{ids};
        for (const auto& path : event_->selections) {
            ComPtr<IShellItem> item;
            auto hr = EventItem(path, &item);
            if (FAILED(hr)) return hr;
            PIDLIST_ABSOLUTE id = nullptr;
            hr = SHGetIDListFromObject(item.Get(), &id);
            if (FAILED(hr)) return hr;
            ids.push_back(id);
        }
        return SHCreateShellItemArrayFromIDLists(static_cast<UINT>(ids.size()), ids.data(), items);
    } catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
    catch (...) { return E_FAIL; }

    wire::DialogEventReply ReportEvent(wire::Operation operation, const wire::DialogEvent& event) {
        wire::DialogEventReply reply{event.serial};
        using enum wire::EventKind;
        if (event.kind == Ready) return reply;
        if (!OnThread() || !showing_ || !bridge_result_ || event_) {
            reply.result = static_cast<uint32_t>(E_UNEXPECTED);
            return reply;
        }
        event_ = &event;
        struct Reset {
            FileDialog& self;
            ~Reset() { self.event_ = nullptr; self.ClearResult(); }
        } reset{*this};
        ComPtr<IShellItem> target;
        HRESULT hr = S_OK;
        if (event.kind == FileOk)
            hr = StoreResult({operation, wire::Status::Accepted, wire::ErrorSource::None, 0, event.filter_index, event.candidates});
        else if (event.kind == FolderChanging) hr = EventFolder(event.target, &target);
        else if (event.kind == Overwrite) hr = EventItem(event.target, &target);
        if (FAILED(hr)) { reply.result = static_cast<uint32_t>(hr); return reply; }
        const auto subscriptions = subscriptions_;
        for (const auto& sub : subscriptions) {
            if (close_requested_) break;
            if (std::ranges::find(subscriptions_, sub.cookie, &Subscription::cookie) == subscriptions_.end()) continue;
            auto* sink = sub.proxy.Get();
            switch (event.kind) {
            case Ready: break;
            case FolderChanging: hr = sink->OnFolderChanging(Identity(), target.Get()); break;
            case FolderChanged: hr = sink->OnFolderChange(Identity()); break;
            case SelectionChanged: hr = sink->OnSelectionChange(Identity()); break;
            case TypeChanged: hr = sink->OnTypeChange(Identity()); break;
            case FileOk: hr = sink->OnFileOk(Identity()); break;
            case Overwrite: {
                FDE_OVERWRITE_RESPONSE decision = FDEOR_DEFAULT;
                hr = sink->OnOverwrite(Identity(), target.Get(), &decision);
                if (SUCCEEDED(hr) && decision == FDEOR_REFUSE) reply.overwrite = 2;
                if (SUCCEEDED(hr) && decision == FDEOR_ACCEPT && !reply.overwrite) reply.overwrite = 1;
                break;
            }
            case ControlActivating:
            case ButtonClicked: {
                ComPtr<IFileDialogControlEvents> control;
                if (!button_ || FAILED(sub.proxy.As(&control))) break;
                if (event.kind == ControlActivating) control->OnControlActivating(this, button_->id);
                else control->OnButtonClicked(this, button_->id);
                break;
            }
            }
            if ((event.kind == FileOk || event.kind == FolderChanging) && hr != S_OK && hr != E_NOTIMPL) {
                reply.result = static_cast<uint32_t>(hr);
                break;
            }
        }
        return reply;
    }

    std::optional<wire::Message> Prepare() {
        GUID guid{};
        if (FAILED(CoCreateGuid(&guid))) return std::nullopt;
        const auto* bytes = reinterpret_cast<const uint8_t*>(&guid);
        wire::RequestId id{};
        std::copy_n(bytes, id.size(), id.begin());

        FILEOPENDIALOGOPTIONS options = 0;
        if (native_->GetOptions(&options) != S_OK || ((options & FOS_PICKFOLDERS) && (options & FOS_ALLOWMULTISELECT)))
            return std::nullopt;
        ComPtr<IShellItem> folder;
        std::u16string directory;
        if (native_->GetFolder(&folder) != S_OK || !FilesystemFolder(folder.Get(), directory)) return std::nullopt;
        PreparationContext context;
        context.resolved_initial_directory = std::move(directory);
        context.caller_validation_supported = true;
        context.save_caller_validation_supported = true;
        context.native_save_defaults_supported = true;
        context.client_state_owned = client_state_.has_value() && client_guid_.has_value();
        const auto policy = factory_ == DialogFactory::Open ? AccessPolicy::LocalOpenV1 : AccessPolicy::LocalSaveV1;
        const auto decision = PrepareFileDialogContract(factory_, calls_, id, policy, context);
        if (decision.effective_options != options) return std::nullopt;
        if (!RuntimeContractCertified(decision) && !RuntimeLocalSaveContractCertified(decision)) return std::nullopt;
        auto request = BridgeRequestFor(decision);
        if (request && context.client_state_owned && std::ranges::none_of(calls_, [](const SetterCall& call) {
                return call.setting == Setting::Folder && call.native_result == S_OK;
            })) {
            if (const auto remembered = client_state_->Read(*client_guid_)) {
                const DWORD attributes = GetFileAttributesW(remembered->c_str());
                if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY))
                    std::get<wire::Request>(request->body).initial_directory.assign(remembered->begin(), remembered->end());
            }
        }
        return request;
    }

    static HRESULT WireError(const wire::Response& response) noexcept {
        if (response.error_source == wire::ErrorSource::HResult)
            return FAILED(static_cast<HRESULT>(response.error_code)) ? static_cast<HRESULT>(response.error_code) : E_FAIL;
        return HRESULT_FROM_WIN32(response.error_source == wire::ErrorSource::Win32 ? response.error_code : ERROR_INVALID_DATA);
    }

    HRESULT StoreResult(const wire::Response& result) {
        std::vector<std::wstring> paths;
        for (const auto& path : result.paths) paths.emplace_back(path.begin(), path.end());
        ComPtr<IShellItemArray> items;
        const HRESULT hr = CreatePickerShellItemArray(result.operation, paths, &items);
        if (FAILED(hr)) return hr;
        ComPtr<IShellItem> first;
        const HRESULT selected = items->GetItemAt(0, &first);
        if (FAILED(selected)) return selected;
        if (paths.size() == 1) {
            const auto separator = paths.front().find_last_of(L"\\/");
            filename_ = paths.front().substr(separator == std::wstring::npos ? 0 : separator + 1);
        }
        result_count_ = paths.size();
        result_index_ = result.filter_index + 1;
        item_ = std::move(first);
        items_ = std::move(items);
        return S_OK;
    }
    void ClearResult() {
        item_.Reset();
        items_.Reset();
        filename_.clear();
        result_count_ = 0;
        result_index_ = 0;
    }
    void CommitResult() noexcept try {
        FILEOPENDIALOGOPTIONS options = 0;
        if (native_->GetOptions(&options) == S_OK && !(options & FOS_DONTADDTORECENT)) {
            for (DWORD i = 0; i < result_count_; ++i) {
                ComPtr<IShellItem> recent;
                if (SUCCEEDED(items_->GetItemAt(i, &recent))) SHAddToRecentDocs(SHARD_SHELLITEM, recent.Get());
            }
        }
        if (client_state_ && client_guid_ && item_) {
            ComPtr<IShellItem> folder;
            std::u16string directory;
            if (SUCCEEDED(item_->GetParent(&folder)) && FilesystemFolder(folder.Get(), directory))
                client_state_->Write(*client_guid_, {directory.begin(), directory.end()});
        }
    } catch (...) {}

    const DialogFactory factory_;
    const ComPtr<IFileDialog> native_;
    const FileDialogBridgeServices services_;
    const std::shared_ptr<detail::FileDialogEventOwner> event_owner_ = std::make_shared<detail::FileDialogEventOwner>();
    const DWORD thread_ = GetCurrentThreadId();
    std::atomic<ULONG> references_{1};
    bool showing_ = false, native_only_ = false, bridge_result_ = false, close_requested_ = false;
    HRESULT close_result_ = S_OK;
    wire::PipeHandle cancel_;
    std::vector<SetterCall> calls_;
    std::vector<Subscription> subscriptions_;
    std::optional<FileDialogClientState> client_state_;
    std::optional<GUID> client_guid_;
    std::optional<Button> button_;
    bool group_open_ = false, had_group_ = false;
    const wire::DialogEvent* event_ = nullptr;
    ComPtr<IShellItem> item_;
    ComPtr<IShellItemArray> items_;
    std::wstring filename_;
    size_t result_count_ = 0;
    UINT result_index_ = 0;
};
} // namespace

HRESULT WrapNativeFileDialog(DialogFactory factory, IUnknown* native_object,
    const FileDialogBridgeServices& services, REFIID iid, void** output) noexcept {
    if (!output) return E_POINTER;
    *output = nullptr;
    if (!native_object || (factory != DialogFactory::Open && factory != DialogFactory::Save)) return E_INVALIDARG;
    try {
        ComPtr<IFileDialog> native;
        const auto hr = native_object->QueryInterface(IID_PPV_ARGS(&native));
        if (FAILED(hr)) return hr;
        auto* wrapper = new FileDialog(factory, std::move(native), services);
        const auto queried = wrapper->QueryInterface(iid, output);
        wrapper->Release();
        return queried;
    } catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
    catch (...) { return E_FAIL; }
}
} // namespace pulse::shell_bridge
