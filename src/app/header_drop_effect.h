#pragma once
#include <windows.h>
#include <oleidl.h>

namespace pulse::app {

// A navigation drop never consumes the source. OLE may delete it after MOVE.
inline DWORD HeaderDropNavigationEffect(DWORD allowed) {
    if (allowed & DROPEFFECT_COPY) return DROPEFFECT_COPY;
    if (allowed & DROPEFFECT_LINK) return DROPEFFECT_LINK;
    return DROPEFFECT_NONE;
}

inline bool HeaderDropFallbackAllowed(DWORD accepted, DWORD selected) {
    // The query finishes after OLE has observed the accepted navigation effect.
    return accepted == DROPEFFECT_COPY && selected == DROPEFFECT_COPY;
}

template<typename Submit>
bool RunHeaderDropFallback(DWORD accepted, DWORD selected, Submit&& submit) {
    if (!HeaderDropFallbackAllowed(accepted, selected)) return false;
    submit();
    return true;
}

} // namespace pulse::app
