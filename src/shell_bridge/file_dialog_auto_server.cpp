// SPDX-License-Identifier: Apache-2.0
// Adapted from jimmgreen/pulse at b32436831a066e18448b2810bef4bf989024fc68.
// Modified for this Pulse distribution. See LICENSE and NOTICE.

#include "file_dialog_class_factory.h"
#include "../common/file_dialog_registration.h"
#include "../common/process_token_restriction.h"
#include "../ipc/picker_transport.h"

#include <atomic>
#include <bit>
#include <new>
#include <shlobj.h>
#include <wrl/client.h>

namespace {
namespace shell = pulse::shell_bridge;
using Microsoft::WRL::ComPtr;

HMODULE module = nullptr;

std::wstring OwnPath() {
    wchar_t path[32768]{};
    const DWORD length = GetModuleFileNameW(module, path, static_cast<DWORD>(std::size(path)));
    return length && length < std::size(path) ? std::wstring(path, length) : std::wstring();
}

bool EligibleToken() noexcept {
    HANDLE raw = nullptr;
    if (OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &raw)) {
        CloseHandle(raw);
        return false;
    }
    if (GetLastError() != ERROR_NO_TOKEN || !OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw)) return false;
    pulse::ipc::picker::PipeHandle token(raw);
    TOKEN_ELEVATION elevation{};
    DWORD size = 0, container = 0;
    return GetTokenInformation(token.Get(), TokenElevation, &elevation, sizeof(elevation), &size) &&
        !elevation.TokenIsElevated &&
        GetTokenInformation(token.Get(), TokenIsAppContainer, &container, sizeof(container), &size) &&
        !container && pulse::security::CheckUnrestrictedToken(token.Get()) == ERROR_SUCCESS;
}

bool Enabled(const std::wstring& path) noexcept {
    return !path.empty() && EligibleToken() && pulse::dialog_bridge::IsEnabledFor(path);
}

HRESULT WINAPI NativeFactory(REFCLSID clsid, DWORD, LPVOID, REFIID iid, void** output) {
    if (!output) return E_POINTER;
    *output = nullptr;
    using Entry = HRESULT (WINAPI*)(REFCLSID, REFIID, void**);
    static const Entry entry = []() -> Entry {
        const HMODULE library = LoadLibraryExW(L"comdlg32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        return library ? std::bit_cast<Entry>(GetProcAddress(library, "DllGetClassObject")) : nullptr;
    }();
    return entry ? entry(clsid, iid, output) : CLASS_E_CLASSNOTAVAILABLE;
}

// COM can cache the published class factory across apartments. Resolve each
// native/bridge factory on the current apartment when it creates the object.
class Factory final : public IClassFactory {
public:
    explicit Factory(REFCLSID clsid) : clsid_(clsid) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** output) noexcept override {
        if (!output) return E_POINTER;
        *output = nullptr;
        if (iid != IID_IUnknown && iid != IID_IClassFactory) return E_NOINTERFACE;
        *output = static_cast<IClassFactory*>(this);
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() noexcept override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() noexcept override {
        const auto count = --references_;
        if (!count) delete this;
        return count;
    }
    HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown* outer, REFIID iid, void** output) noexcept override {
        if (!output) return E_POINTER;
        *output = nullptr;
        shell::FileDialogClassActivation activation;
        activation.original = NativeFactory;
        try {
            const auto own = OwnPath();
            if (Enabled(own)) {
                activation.services.executable = own.substr(0, own.find_last_of(L"\\/")) + L"\\pulse.exe";
                activation.services.enabled = [own] { return Enabled(own); };
                activation.enabled = true;
            }
        } catch (...) {
            activation.enabled = false;
        }
        ComPtr<IClassFactory> factory;
        const auto hr = shell::ActivateFileDialogClass(activation, clsid_, CLSCTX_INPROC_SERVER, nullptr,
            IID_PPV_ARGS(&factory));
        return SUCCEEDED(hr) ? factory->CreateInstance(outer, iid, output) : hr;
    }
    HRESULT STDMETHODCALLTYPE LockServer(BOOL lock) noexcept override {
        ComPtr<IClassFactory> factory;
        const auto hr = NativeFactory(clsid_, CLSCTX_INPROC_SERVER, nullptr, IID_PPV_ARGS(&factory));
        return SUCCEEDED(hr) ? factory->LockServer(lock) : hr;
    }

private:
    std::atomic<ULONG> references_{1};
    CLSID clsid_;
};
} // namespace

extern "C" HRESULT WINAPI DllGetClassObject(REFCLSID clsid, REFIID iid, void** output) {
    if (!output) return E_POINTER;
    *output = nullptr;
    if (clsid != CLSID_FileOpenDialog && clsid != CLSID_FileSaveDialog) return CLASS_E_CLASSNOTAVAILABLE;
    if (iid != IID_IUnknown && iid != IID_IClassFactory)
        return NativeFactory(clsid, CLSCTX_INPROC_SERVER, nullptr, iid, output);
    auto* factory = new (std::nothrow) Factory(clsid);
    if (!factory) return E_OUTOFMEMORY;
    const auto hr = factory->QueryInterface(iid, output);
    factory->Release();
    return hr;
}

extern "C" HRESULT WINAPI DllCanUnloadNow() { return S_FALSE; }

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) module = instance;
    return TRUE;
}
