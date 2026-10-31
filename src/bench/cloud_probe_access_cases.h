// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once
#include "../app/cloud_probe_access.h"

namespace pulse::bench {

class CloudProbeStore final : public IPropertyStore {
public:
    VARTYPE type = VT_UI4;
    ULONG state = 6;
    HRESULT result = S_OK;
    int reads = 0;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** output) override {
        if (!output) return E_POINTER;
        *output = nullptr;
        if (iid != IID_IUnknown && iid != __uuidof(IPropertyStore)) return E_NOINTERFACE;
        *output = static_cast<IPropertyStore*>(this);
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 2; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE GetCount(DWORD*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetAt(DWORD, PROPERTYKEY*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetValue(REFPROPERTYKEY key, PROPVARIANT* value) override {
        ++reads;
        if (!IsEqualPropertyKey(key, app::cloud_probe_access::kStorageProviderState)) return E_INVALIDARG;
        *value = {};
        value->vt = type;
        value->ulVal = state;
        return result;
    }
    HRESULT STDMETHODCALLTYPE SetValue(REFPROPERTYKEY, REFPROPVARIANT) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Commit() override { return E_NOTIMPL; }
};

struct CloudProbeItem {
    CloudProbeStore store;
    HRESULT result = S_OK;
    int requests = 0;
    bool safe = true;
    HRESULT GetPropertyStoreForKeys(const PROPERTYKEY* keys, UINT count,
                                    GETPROPERTYSTOREFLAGS flags, REFIID iid, void** output) {
        ++requests;
        safe &= keys && count == 1 && flags == GPS_FASTPROPERTIESONLY &&
            IsEqualPropertyKey(keys[0], app::cloud_probe_access::kStorageProviderState);
        *output = nullptr;
        if (!safe) return E_ACCESSDENIED;
        return FAILED(result) ? result : store.QueryInterface(iid, output);
    }
};

template<class Check>
void CloudProbeAccessCases(Check&& check) {
    CloudProbeItem item;
    for (const ULONG state : {6ul, 7ul, 1ul}) {
        item.store.state = state;
        check(app::cloud_probe_access::ReadState(item) == state && item.safe,
            "sync probe reads transfer error and idle states exclusively from fast metadata");
    }
    item.result = E_ACCESSDENIED;
    const int requests = item.requests;
    const int reads = item.store.reads;
    check(app::cloud_probe_access::ReadState(item) == 0 && item.requests == requests + 1 &&
        item.store.reads == reads && item.safe,
        "unavailable fast store stays unknown without retrying a default property store");
    item.result = S_OK;
    item.store.type = VT_EMPTY;
    check(app::cloud_probe_access::ReadState(item) == 0 && item.safe,
        "missing fast provider state stays unknown without reading file properties");
    item.store.type = VT_UI4;
    item.store.result = E_FAIL;
    check(app::cloud_probe_access::ReadState(item) == 0 && item.safe,
        "failed fast provider state stays unknown without a content fallback");
}

} // namespace pulse::bench
