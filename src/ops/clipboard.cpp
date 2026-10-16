// clipboard.cpp — CF_HDROP + CFSTR_PREFERREDDROPEFFECT interop.
#include "clipboard.h"
#include "../common/path_utils.h"
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <cstring>

namespace pulse::ops {

bool PreferredEffectIsCut(uint32_t preferred_effect) {
    return preferred_effect == DROPEFFECT_MOVE;
}

namespace {

HWND ClipboardOwner() {
    if (HWND owner = GetActiveWindow()) return owner;
    return GetConsoleWindow();
}

bool OpenClipboardWithRetry(HWND owner) {
    for (int attempt = 0; attempt < 20; ++attempt) {
        if (OpenClipboard(owner)) return true;
        Sleep(5);
    }
    return false;
}

} // namespace

bool WriteClipboard(const std::vector<std::wstring>& paths, bool cut) {
    if (paths.empty()) return false;

    // DROPFILES + double-null-terminated wide path list.
    size_t chars = 1; // final terminator
    for (const auto& p : paths) chars += p.size() + 1;
    size_t bytes = sizeof(DROPFILES) + chars * sizeof(wchar_t);

    HGLOBAL hdrop = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, bytes);
    if (!hdrop) return false;
    auto* df = static_cast<DROPFILES*>(GlobalLock(hdrop));
    if (!df) {
        GlobalFree(hdrop);
        return false;
    }
    df->pFiles = sizeof(DROPFILES);
    df->fWide = TRUE;
    wchar_t* dst = reinterpret_cast<wchar_t*>(reinterpret_cast<uint8_t*>(df) + sizeof(DROPFILES));
    for (const auto& p : paths) {
        std::memcpy(dst, p.c_str(), p.size() * sizeof(wchar_t));
        dst += p.size() + 1;
    }
    GlobalUnlock(hdrop);

    // EmptyClipboard assigns ownership to the window passed to OpenClipboard;
    // using nullptr can leave no owner and make SetClipboardData fail.
    if (!OpenClipboardWithRetry(ClipboardOwner())) {
        GlobalFree(hdrop);
        return false;
    }
    if (!EmptyClipboard() || !SetClipboardData(CF_HDROP, hdrop)) {
        CloseClipboard();
        GlobalFree(hdrop);
        return false;
    }
    // Ownership of hdrop transferred to the clipboard.

    UINT fmtEffect = RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT);
    HGLOBAL heffect = fmtEffect ? GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, sizeof(DWORD)) : nullptr;
    bool effect_set = false;
    if (fmtEffect != 0 && heffect) {
        DWORD* effect = static_cast<DWORD*>(GlobalLock(heffect));
        if (effect) {
            *effect = cut ? DROPEFFECT_MOVE : DROPEFFECT_COPY;
            GlobalUnlock(heffect);
            if (SetClipboardData(fmtEffect, heffect)) effect_set = true;
            else GlobalFree(heffect);
        } else {
            GlobalFree(heffect);
        }
    }
    if (!effect_set) {
        // CF_HDROP ownership already transferred; clearing the clipboard lets
        // the system release it without a double-free.
        EmptyClipboard();
        CloseClipboard();
        return false;
    }
    CloseClipboard();
    return true;
}

bool WriteClipboardText(const std::wstring& text) {
    if (text.empty()) return false;
    size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!h) return false;
    void* memory = GlobalLock(h);
    if (!memory) {
        GlobalFree(h);
        return false;
    }
    std::memcpy(memory, text.c_str(), bytes);
    GlobalUnlock(h);
    if (!OpenClipboardWithRetry(ClipboardOwner())) {
        GlobalFree(h);
        return false;
    }
    if (!EmptyClipboard() || !SetClipboardData(CF_UNICODETEXT, h)) {
        CloseClipboard();
        GlobalFree(h);
        return false;
    }
    // Ownership of h transferred to the clipboard.
    CloseClipboard();
    return true;
}

bool ReadClipboard(ClipboardData& out) {
    out = {};
    if (!IsClipboardFormatAvailable(CF_HDROP)) return false;
    if (!OpenClipboardWithRetry(nullptr)) return false;

    bool ok = false;
    if (HANDLE h = GetClipboardData(CF_HDROP)) {
        if (HDROP hdrop = static_cast<HDROP>(GlobalLock(h))) {
            UINT count = DragQueryFileW(hdrop, 0xFFFFFFFF, nullptr, 0);
            for (UINT i = 0; i < count; ++i) {
                UINT len = DragQueryFileW(hdrop, i, nullptr, 0);
                std::wstring path(static_cast<size_t>(len) + 1, L'\0');
                const UINT copied = DragQueryFileW(hdrop, i, path.data(), len + 1);
                if (copied == 0 && len != 0) continue;
                path.resize(copied);
                out.paths.push_back(std::move(path));
            }
            GlobalUnlock(h);
        }
        ok = !out.paths.empty();
    }
    UINT fmtEffect = RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT);
    if (HANDLE h = GetClipboardData(fmtEffect)) {
        if (DWORD* effect = static_cast<DWORD*>(GlobalLock(h))) {
            out.cut = GlobalSize(h) >= sizeof(DWORD) && PreferredEffectIsCut(*effect);
            GlobalUnlock(h);
        }
    }
    if (ok) out.sequence = GetClipboardSequenceNumber();
    CloseClipboard();
    if (ok && out.sequence != GetClipboardSequenceNumber()) {
        out = {};
        return false;
    }
    return ok;
}

namespace {
bool DropShellData(IDataObject* data, const std::wstring& dir, DWORD preferred_effect) {
    if (!data) return false;
    IDropTarget* drop = nullptr;
    IShellItem* folder = nullptr;
    const std::wstring shell_path = path::StripExtendedPathPrefix(dir);
    if (SUCCEEDED(SHCreateItemFromParsingName(shell_path.c_str(), nullptr, IID_PPV_ARGS(&folder)))) {
        folder->BindToHandler(nullptr, BHID_SFUIObject, IID_PPV_ARGS(&drop));
        folder->Release();
    }

    bool ok = false;
    if (drop) {
        const DWORD wanted = PreferredEffectIsCut(preferred_effect) ? DROPEFFECT_MOVE : DROPEFFECT_COPY;
        DWORD effect = wanted;
        POINTL point{0, 0};
        if (SUCCEEDED(drop->DragEnter(data, MK_LBUTTON, point, &effect))) {
            if (effect & wanted) {
                effect = wanted;
                ok = SUCCEEDED(drop->Drop(data, MK_LBUTTON, point, &effect)) &&
                    effect != DROPEFFECT_NONE;
            } else {
                drop->DragLeave();
            }
        }
        drop->Release();
    }
    return ok;
}
} // namespace

bool PasteShellClipboard(const std::wstring& dir, uint32_t expected_sequence) {
    const HRESULT ole = OleInitialize(nullptr);
    if (FAILED(ole)) return false;
    struct OleDone { ~OleDone() { OleUninitialize(); } } ole_done;

    if (GetClipboardSequenceNumber() != expected_sequence) return false;
    IDataObject* data = nullptr;
    if (FAILED(OleGetClipboard(&data)) || !data) return false;
    if (GetClipboardSequenceNumber() != expected_sequence) {
        data->Release();
        return false;
    }

    DWORD preferred_effect = DROPEFFECT_COPY;
    const UINT preferred_format = RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT);
    FORMATETC format{ static_cast<CLIPFORMAT>(preferred_format), nullptr,
                      DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
    STGMEDIUM medium{};
    if (preferred_format && SUCCEEDED(data->GetData(&format, &medium))) {
        if (medium.tymed == TYMED_HGLOBAL && medium.hGlobal &&
            GlobalSize(medium.hGlobal) >= sizeof(DWORD)) {
            if (const auto* preferred = static_cast<const DWORD*>(GlobalLock(medium.hGlobal))) {
                preferred_effect = *preferred;
                GlobalUnlock(medium.hGlobal);
            }
        }
        ReleaseStgMedium(&medium);
    }
    const bool ok = DropShellData(data, dir, preferred_effect);
    data->Release();
    return ok;
}

#ifdef PULSE_CLIPBOARD_TESTING
bool PasteShellDataForTest(IDataObject* data, const std::wstring& dir, uint32_t preferred_effect) {
    const HRESULT ole = OleInitialize(nullptr);
    if (FAILED(ole)) return false;
    struct OleDone { ~OleDone() { OleUninitialize(); } } ole_done;
    return DropShellData(data, dir, preferred_effect);
}
#endif

bool CompleteCutClipboard(uint32_t sequence, const std::vector<std::wstring>& expected_paths) {
    if (sequence == 0 || expected_paths.empty() || GetClipboardSequenceNumber() != sequence)
        return false;
    ClipboardData current;
    if (!ReadClipboard(current) || !current.cut || current.sequence != sequence ||
        current.paths.size() != expected_paths.size()) return false;
    for (size_t i = 0; i < expected_paths.size(); ++i) {
        if (_wcsicmp(current.paths[i].c_str(), expected_paths[i].c_str()) != 0) return false;
    }
    if (!OpenClipboardWithRetry(ClipboardOwner())) return false;
    if (GetClipboardSequenceNumber() != sequence || !EmptyClipboard()) {
        CloseClipboard();
        return false;
    }
    const UINT format = RegisterClipboardFormatW(CFSTR_PERFORMEDDROPEFFECT);
    HGLOBAL value = format ? GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, sizeof(DWORD)) : nullptr;
    bool ok = false;
    if (format && value) {
        auto* effect = static_cast<DWORD*>(GlobalLock(value));
        if (effect) {
            *effect = DROPEFFECT_MOVE;
            GlobalUnlock(value);
            ok = SetClipboardData(format, value) != nullptr;
        }
        if (!ok) GlobalFree(value);
    }
    CloseClipboard();
    return ok;
}

} // namespace pulse::ops
