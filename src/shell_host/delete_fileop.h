#pragma once
#include <windows.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <sherrors.h>

namespace pulse::shell {
// Only after the deletion-service snapshot has been admitted. Pulse owns the
// confirmation; Explorer's progress and dialogs do not.
inline DWORD AuthorizedDeleteFlags(bool recycle) noexcept {
    DWORD flags = FOF_SILENT | FOF_NOCONFIRMATION | FOF_NOERRORUI | FOFX_EARLYFAILURE;
    // The filesystem implementation also needs ALLOWUNDO to produce a bin
    // payload. Without it, observed S_OK may represent permanent deletion.
    if (recycle) flags |= FOFX_RECYCLEONDELETE | FOF_ALLOWUNDO;
    return flags;
}
// PreDeleteItem transfer flags do not encode the IFileOperation recycle mode:
// the filesystem provider reports 0x202 even for a valid recycle-only request.
// FOFX_RECYCLEONDELETE is the operation contract; only its explicit recycle
// failure HRESULT can request a newly confirmed permanent-delete plan.
inline bool NeedsPermanentDeleteConfirmation(HRESULT error) noexcept {
    return error == COPYENGINE_E_RECYCLE_FORCE_NUKE || error == COPYENGINE_E_RECYCLE_SIZE_TOO_BIG ||
        error == COPYENGINE_E_RECYCLE_PATH_TOO_LONG || error == COPYENGINE_E_RECYCLE_BIN_NOT_FOUND;
}
}
