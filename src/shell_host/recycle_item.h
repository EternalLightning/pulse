// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "file_operations.h"
#include <shlobj.h>
#include <sherrors.h>
#include <wrl/client.h>

namespace pulse::shell {
namespace detail {
class RecycleAdviseSink final : public ITransferAdviseSink {
public:
    explicit RecycleAdviseSink(const FileOperationCallbacks& callbacks) : callbacks_(callbacks) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (iid != IID_IUnknown && iid != IID_ITransferAdviseSink) return E_NOINTERFACE;
        *out = this; AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override { return --references_; }
    HRESULT Check() const {
        return callbacks_.cancelled && callbacks_.cancelled() ? COPYENGINE_E_USER_CANCELLED : S_OK;
    }
    HRESULT STDMETHODCALLTYPE UpdateProgress(ULONGLONG, ULONGLONG, int, int, int, int) override { return Check(); }
    HRESULT STDMETHODCALLTYPE UpdateTransferState(TRANSFER_ADVISE_STATE) override { return Check(); }
    HRESULT STDMETHODCALLTYPE ConfirmOverwrite(IShellItem*, IShellItem*, LPCWSTR) override { return E_UNEXPECTED; }
    HRESULT STDMETHODCALLTYPE ConfirmEncryptionLoss(IShellItem*) override { return E_UNEXPECTED; }
    HRESULT STDMETHODCALLTYPE FileFailure(IShellItem*, LPCWSTR, HRESULT error, LPWSTR, ULONG) override { return Fail(error); }
    HRESULT STDMETHODCALLTYPE SubStreamFailure(IShellItem*, LPCWSTR, HRESULT error) override { return Fail(error); }
    HRESULT STDMETHODCALLTYPE PropertyFailure(IShellItem*, const PROPERTYKEY*, HRESULT error) override { return Fail(error); }
    HRESULT failure = S_OK;
private:
    HRESULT Fail(HRESULT error) { if (SUCCEEDED(failure)) failure = error; return error; }
    ULONG references_ = 1;
    const FileOperationCallbacks& callbacks_;
};
}

// RecycleItem has no permanent-delete fallback. Provider decisions return to Pulse.
inline HRESULT RecycleShellItem(IShellItem* source, const FileOperationCallbacks& callbacks, IShellItem** created,
    const std::function<HRESULT()>& before_mutation) {
    using Microsoft::WRL::ComPtr;
    *created = nullptr;
    detail::RecycleAdviseSink sink(callbacks);
    HRESULT hr = sink.Check();
    ComPtr<IShellItem> parent, bin;
    ComPtr<ITransferSource> transfer;
    if (SUCCEEDED(hr)) hr = source->GetParent(&parent);
    if (SUCCEEDED(hr)) hr = parent->BindToHandler(nullptr, BHID_Transfer, IID_PPV_ARGS(&transfer));
    if (SUCCEEDED(hr)) hr = SHGetKnownFolderItem(FOLDERID_RecycleBinFolder, KF_FLAG_DEFAULT, nullptr, IID_PPV_ARGS(&bin));
    DWORD cookie = 0;
    const bool advised = SUCCEEDED(hr) && SUCCEEDED(hr = transfer->Advise(&sink, &cookie));
    if (SUCCEEDED(hr)) hr = sink.Check();
    if (SUCCEEDED(hr)) hr = before_mutation();
    if (SUCCEEDED(hr)) hr = transfer->RecycleItem(source, bin.Get(), TSF_NORMAL, created);
    if (advised) transfer->Unadvise(cookie);
    if (FAILED(sink.failure)) hr = sink.failure;
    if (hr == E_NOINTERFACE || hr == E_NOTIMPL || hr == HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED))
        hr = COPYENGINE_E_RECYCLE_FORCE_NUKE;
    return hr;
}
}
