// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.

#include "../shell_host/file_operations.cpp"
#include <cstdio>

int main() {
    using namespace pulse::ipc;
    using namespace pulse::shell;
    int failures = 0;
    const auto check = [&](bool ok, const char* description) {
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", description);
        failures += !ok;
    };
    ShellOperationRequest request;
    request.kind = ShellOperationKind::Recycle;
    ShellOperationItem item{L"C:\\isolated-recycle-contract.txt", {}, {}, {}};
    FileOperationCallbacks callbacks;
    ProgressSink sink(request, item, callbacks, 0);
    check(sink.PreDeleteItem(0x202, nullptr) == S_OK && sink.failure == S_OK,
        "valid 0x202 transfer flags do not fabricate a recycle-unavailable failure");
    const DWORD flags = OperationFlags(request, false);
    check((flags & FOFX_RECYCLEONDELETE) && (flags & FOF_WANTNUKEWARNING) && !(flags & FOF_NOCONFIRMATION),
        "recycle forces the Recycle Bin contract and never automatically confirms permanent deletion");
    callbacks.cancelled = [] { return true; };
    ProgressSink cancelled(request, item, callbacks, 0);
    check(cancelled.PreDeleteItem(0x202, nullptr) == HRESULT_FROM_WIN32(ERROR_CANCELLED),
        "0x202 recycle callbacks still honor cancellation before mutation");
    callbacks.cancelled = {};
    sink.PostDeleteItem(0x202, nullptr, COPYENGINE_E_RECYCLE_FORCE_NUKE, nullptr);
    check(sink.failure == COPYENGINE_E_RECYCLE_FORCE_NUKE && sink.result.hr == COPYENGINE_E_RECYCLE_FORCE_NUKE,
        "actual provider recycle refusal is preserved for the existing confirmation path");
    ProgressSink unproven(request, item, callbacks, 0);
    unproven.PostDeleteItem(0x202, nullptr, S_OK, nullptr);
    check(unproven.failure == E_UNEXPECTED,
        "recycle success without a returned destination is never accepted");
    request.kind = ShellOperationKind::DeletePermanent;
    check(!(OperationFlags(request, false) & FOFX_RECYCLEONDELETE),
        "permanent-delete mode remains separate from recycle requests");
    return failures ? 1 : 0;
}
