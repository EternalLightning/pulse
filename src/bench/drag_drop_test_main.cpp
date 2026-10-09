#include "../ui/drag_drop.h"
#include "../ops/clipboard.h"
#include <shlobj.h>
#include <cstdio>
#include <utility>

namespace {
int passed = 0, failed = 0;
void Check(bool good, const wchar_t* name) {
    wprintf(L"[%s] %s\n", good ? L"PASS" : L"FAIL", name);
    good ? ++passed : ++failed;
}

class PreferringDataObject final : public IDataObject {
public:
    explicit PreferringDataObject(DWORD preferred) : preferred_(preferred) {
        inner_ = pulse::ui::FileDataObject::Create({L"C:\\fixture\\source.txt"});
    }
    ~PreferringDataObject() { inner_->Release(); }
    IFACEMETHODIMP QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (iid != IID_IUnknown && iid != IID_IDataObject) return E_NOINTERFACE;
        *out = static_cast<IDataObject*>(this); AddRef(); return S_OK;
    }
    IFACEMETHODIMP_(ULONG) AddRef() override { return ++refs_; }
    IFACEMETHODIMP_(ULONG) Release() override {
        const ULONG remaining = --refs_;
        if (!remaining) delete this;
        return remaining;
    }
    IFACEMETHODIMP GetData(FORMATETC* fmt, STGMEDIUM* out) override {
        if (fmt && out && fmt->cfFormat == RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT)) {
            *out = {};
            out->tymed = TYMED_HGLOBAL;
            out->hGlobal = GlobalAlloc(GMEM_MOVEABLE, sizeof(DWORD));
            if (!out->hGlobal) return E_OUTOFMEMORY;
            *static_cast<DWORD*>(GlobalLock(out->hGlobal)) = preferred_;
            GlobalUnlock(out->hGlobal);
            return S_OK;
        }
        return inner_->GetData(fmt, out);
    }
    IFACEMETHODIMP GetDataHere(FORMATETC* f, STGMEDIUM* m) override { return inner_->GetDataHere(f, m); }
    IFACEMETHODIMP QueryGetData(FORMATETC* f) override { return inner_->QueryGetData(f); }
    IFACEMETHODIMP GetCanonicalFormatEtc(FORMATETC* f, FORMATETC* o) override { return inner_->GetCanonicalFormatEtc(f, o); }
    IFACEMETHODIMP SetData(FORMATETC* f, STGMEDIUM* m, BOOL r) override { return inner_->SetData(f, m, r); }
    IFACEMETHODIMP EnumFormatEtc(DWORD d, IEnumFORMATETC** o) override { return inner_->EnumFormatEtc(d, o); }
    IFACEMETHODIMP DAdvise(FORMATETC*, DWORD, IAdviseSink*, DWORD*) override { return OLE_E_ADVISENOTSUPPORTED; }
    IFACEMETHODIMP DUnadvise(DWORD) override { return OLE_E_ADVISENOTSUPPORTED; }
    IFACEMETHODIMP EnumDAdvise(IEnumSTATDATA**) override { return OLE_E_ADVISENOTSUPPORTED; }
private:
    ULONG refs_ = 1;
    DWORD preferred_;
    pulse::ui::FileDataObject* inner_ = nullptr;
};
}

int wmain() {
    using namespace pulse::ui;
    OleInitialize(nullptr);
    const DWORD both = DROPEFFECT_COPY | DROPEFFECT_MOVE;
    for (DWORD preference : {DWORD(0), DWORD(DROPEFFECT_COPY), DWORD(DROPEFFECT_MOVE), both,
                             DWORD(DROPEFFECT_COPY | DROPEFFECT_LINK)}) {
        for (DWORD keys : {DWORD(0), DWORD(MK_CONTROL), DWORD(MK_SHIFT)}) {
            const DWORD expected = keys == MK_CONTROL ? DROPEFFECT_COPY : keys == MK_SHIFT ? DROPEFFECT_MOVE :
                (preference & both) == DROPEFFECT_COPY ? DROPEFFECT_COPY : DROPEFFECT_MOVE;
            auto* data = new PreferringDataObject(preference);
            unsigned overs = 0, drops = 0;
            bool observed = true;
            DropTargetCallbacks callbacks;
            auto compute = [&](const auto& paths, POINT, DWORD state, DWORD allowed, DWORD preferred) {
                observed = observed && allowed == both && preferred == preference;
                return ComputeDropEffect(state, paths.front(), L"C:\\target", allowed, preferred);
            };
            callbacks.drag_over = [&](const auto& p, POINT pt, DWORD k, DWORD a, DWORD pref) {
                ++overs; return compute(p, pt, k, a, pref);
            };
            callbacks.drop = [&](const auto& p, POINT pt, DWORD k, DWORD a, DWORD pref) {
                ++drops; return compute(p, pt, k, a, pref);
            };
            auto* target = new WindowDropTarget(nullptr, std::move(callbacks));
            DWORD effect = both;
            target->DragEnter(data, keys, {}, &effect);
            bool correct = effect == expected;
            effect = both; target->DragOver(keys, {}, &effect); correct &= effect == expected;
            effect = both; target->Drop(data, keys, {}, &effect);
            Check(correct && effect == expected && observed && overs == 2 && drops == 1,
                  L"source preference and modifiers agree across enter, hover and drop");
            target->Release(); data->Release();
        }
    }
    {
        auto* entering = new PreferringDataObject(DROPEFFECT_MOVE);
        auto* dropped = new PreferringDataObject(DROPEFFECT_COPY);
        DWORD seen = 0;
        DropTargetCallbacks callbacks;
        callbacks.drop = [&](const auto&, POINT, DWORD, DWORD, DWORD preferred) {
            seen = preferred; return DROPEFFECT_COPY;
        };
        auto* target = new WindowDropTarget(nullptr, std::move(callbacks));
        DWORD effect = both; target->DragEnter(entering, 0, {}, &effect);
        effect = both; target->Drop(dropped, 0, {}, &effect);
        Check(seen == DROPEFFECT_COPY && effect == DROPEFFECT_COPY, L"Drop reads the final data object's preference");
        effect = 0; seen = 0; target->Drop(dropped, 0, {}, &effect);
        Check(effect == 0 && seen == 0, L"disallowed drop does not submit");
        Check(target->DragEnter(entering, 0, {}, nullptr) == E_POINTER &&
              target->DragOver(0, {}, nullptr) == E_POINTER && target->Drop(dropped, 0, {}, nullptr) == E_POINTER,
              L"drop entry points reject null effect pointers");
        target->Release(); entering->Release(); dropped->Release();
    }
    Check(ComputeDropEffect(0, L"C:\\a", L"D:\\b", both) == DROPEFFECT_COPY, L"cross-volume default copies");
    Check(ComputeDropEffect(MK_CONTROL, L"C:\\a", L"C:\\b", DROPEFFECT_MOVE) == DROPEFFECT_MOVE,
          L"source allowed mask constrains modifiers");
    for (DWORD allowed : {DWORD(DROPEFFECT_COPY), DWORD(DROPEFFECT_MOVE)}) {
        auto* data = new PreferringDataObject(DROPEFFECT_MOVE);
        bool observed = true;
        DropTargetCallbacks callbacks;
        const auto constrained = [&](const auto& paths, POINT, DWORD keys, DWORD mask, DWORD preferred) {
            observed = observed && mask == allowed;
            return ComputeDropEffect(keys, paths.front(), L"C:\\target", mask, preferred);
        };
        callbacks.drag_over = constrained;
        callbacks.drop = constrained;
        auto* target = new WindowDropTarget(nullptr, std::move(callbacks));
        DWORD effect = allowed; target->DragEnter(data, 0, {}, &effect);
        bool correct = effect == allowed;
        effect = allowed; target->DragOver(0, {}, &effect); correct &= effect == allowed;
        effect = allowed; target->Drop(data, 0, {}, &effect);
        Check(correct && effect == allowed && observed, L"source copy-only or move-only mask reaches every callback");
        target->Release(); data->Release();
    }
    Check(pulse::ops::PreferredEffectIsCut(DROPEFFECT_MOVE), L"exact MOVE is a clipboard cut");
    Check(!pulse::ops::PreferredEffectIsCut(both), L"COPY|MOVE clipboard preference never deletes originals");
    Check(!pulse::ops::PreferredEffectIsCut(DROPEFFECT_COPY), L"COPY is not a clipboard cut");
    Check(!pulse::ops::PreferredEffectIsCut(0), L"absent preference is not a clipboard cut");
    OleUninitialize();
    wprintf(L"drag/drop: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
