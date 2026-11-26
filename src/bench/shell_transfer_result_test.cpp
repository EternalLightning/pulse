// SPDX-License-Identifier: Apache-2.0
#include "../shell_host/file_operations.cpp"
#include "../ops/elevated_recycle_request.h"
#include <cstdio>

int main() {
    using namespace pulse::ipc;
    using namespace pulse::shell;
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return 2;
    wchar_t executable[32768]{};
    if (!GetModuleFileNameW(nullptr, executable, static_cast<DWORD>(std::size(executable)))) return 2;
    IShellItem* raw = nullptr;
    if (FAILED(MakeItem(executable, &raw))) return 2;
    ComPtr<IShellItem> source(raw);
    raw = nullptr;
    if (FAILED(source->GetParent(&raw))) return 2;
    ComPtr<IShellItem> sibling(raw);
    int failures = 0;
    const auto check = [&](bool ok, const char* description) {
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", description);
        failures += !ok;
    };
    ShellOperationRequest request;
    FileOperationCallbacks callbacks;
    request.collision = ShellCollisionPolicy::Replace;
    for (const auto kind : {ShellOperationKind::Copy, ShellOperationKind::Move, ShellOperationKind::Rename,
        ShellOperationKind::CreateFolder, ShellOperationKind::CreateFile, ShellOperationKind::Recycle,
        ShellOperationKind::DeletePermanent}) {
        request.kind = kind;
        const auto prompt_flags = FOFX_SHOWELEVATIONPROMPT | FOFX_REQUIREELEVATION;
        check((OperationFlags(request, false) & prompt_flags) == 0 &&
            (OperationFlags(request, true) & prompt_flags) == 0,
            "ordinary operation never authorizes native elevation, with or without byte progress");
        request.elevation_authorized = true;
        check((OperationFlags(request, false) & prompt_flags) == (kind == ShellOperationKind::Recycle ? 0u : prompt_flags),
            kind == ShellOperationKind::Recycle ? "recycle elevation uses the Pulse worker, never Explorer permission UI" :
                "explicit Pulse consent authorizes UAC without Explorer's extra permission confirmation");
        request.elevation_authorized = false;
    }
    request.kind = ShellOperationKind::Copy;
    request.items = {{executable, executable, {}, {}}};
    for (bool authorized : {false, true}) {
        request.elevation_authorized = authorized;
        const auto bytes = WriteShellOperation(request);
        ShellOperationRequest decoded;
        check(ReadShellOperation(bytes.data(), bytes.size(), decoded) && decoded.elevation_authorized == authorized,
            "Shell IPC preserves the explicit elevation decision");
    }
    auto malformed = WriteShellOperation(request);
    malformed[20] = 2;
    ShellOperationRequest decoded;
    check(!ReadShellOperation(malformed.data(), malformed.size(), decoded), "Shell IPC rejects malformed elevation consent");
    auto legacy = WriteShellOperation(request);
    legacy[0] = 1;
    legacy.erase(legacy.begin() + 20, legacy.begin() + 24);
    check(ReadShellOperation(legacy.data(), legacy.size(), decoded) && !decoded.elevation_authorized,
        "legacy Shell requests never imply administrator consent");
    request.elevation_authorized = false;
    auto recycle_request = request;
    recycle_request.kind = ShellOperationKind::Recycle;
    recycle_request.items.front().destination.clear();
    pulse::ReadPathFileIdentity(executable, recycle_request.items.front().expected_identity);
    const auto recycle_payload = pulse::ops::EncodeElevatedRecycle(recycle_request);
    check(!recycle_payload.empty() && pulse::ops::DecodeElevatedRecycle(recycle_payload, decoded),
        "administrator recycle codec accepts only exact local roots with historical identities");
    recycle_request.kind = ShellOperationKind::DeletePermanent;
    check(pulse::ops::EncodeElevatedRecycle(recycle_request).empty(),
        "administrator recycle worker can never execute permanent deletion");
    recycle_request.kind = ShellOperationKind::Recycle;
    recycle_request.items.front().expected_identity = {};
    check(pulse::ops::EncodeElevatedRecycle(recycle_request).empty(),
        "administrator recycle worker refuses missing identity proof");
    pulse::shell::detail::RecycleAdviseSink recycle_sink(callbacks);
    check(recycle_sink.FileFailure(nullptr, nullptr, COPYENGINE_E_RECYCLE_FORCE_NUKE, nullptr, 0) ==
        COPYENGINE_E_RECYCLE_FORCE_NUKE && recycle_sink.failure == COPYENGINE_E_RECYCLE_FORCE_NUKE,
        "unsupported recycling returns to Pulse without approving permanent deletion or native UI");
    ShellOperationItem item{executable, executable, {}, {}};
    for (auto kind : {ShellOperationKind::Copy, ShellOperationKind::Move}) {
        request.kind = kind;
        ProgressSink retried(request, item, callbacks, 0);
        retried.Post(source.get(), COPYENGINE_E_REQUIRES_ELEVATION, nullptr);
        retried.Post(source.get(), S_OK, source.get());
        retried.FinishOperations(S_OK);
        check(retried.failure == S_OK && retried.result.hr == S_OK && !retried.result.skipped &&
            retried.result.destination == executable,
            kind == ShellOperationKind::Copy ? "copy success supersedes its elevation notification" :
                "move success supersedes its elevation notification");
    }
    ProgressSink unresolved(request, item, callbacks, 0);
    unresolved.Post(source.get(), COPYENGINE_E_REQUIRES_ELEVATION, nullptr);
    unresolved.FinishOperations(COPYENGINE_E_REQUIRES_ELEVATION);
    check(unresolved.failure == COPYENGINE_E_REQUIRES_ELEVATION,
        "unresolved elevation remains a real failure");

    ProgressSink denied(request, item, callbacks, 0);
    denied.Post(source.get(), COPYENGINE_E_REQUIRES_ELEVATION, nullptr);
    denied.Post(source.get(), COPYENGINE_E_ACCESS_DENIED_DEST, nullptr);
    denied.FinishOperations(COPYENGINE_E_ACCESS_DENIED_DEST);
    check(denied.failure == COPYENGINE_E_ACCESS_DENIED_DEST && denied.failed_path == item.destination,
        "failed elevation retry reports the final destination error");

    ProgressSink skipped(request, item, callbacks, 0);
    skipped.Post(source.get(), COPYENGINE_E_REQUIRES_ELEVATION, nullptr);
    skipped.Post(source.get(), COPYENGINE_S_USER_IGNORED, nullptr);
    skipped.FinishOperations(S_OK);
    check(SUCCEEDED(skipped.failure) && skipped.result.skipped,
        "skipping an elevation prompt does not retain an obsolete elevation error");

    const ShellOperationItem tree{L"C:\\isolated-transfer-root", L"C:\\isolated-transfer-output", {}, {}};
    ProgressSink nested(request, tree, callbacks, 0);
    nested.Post(source.get(), COPYENGINE_E_REQUIRES_ELEVATION, nullptr);
    nested.Post(source.get(), S_OK, source.get());
    nested.FinishOperations(S_OK);
    check(nested.failure == S_OK && nested.nested.size() == 1 && nested.nested.front().hr == S_OK,
        "a retried child has exactly one final outcome");

    ProgressSink partial(request, tree, callbacks, 0);
    partial.Post(source.get(), COPYENGINE_E_ACCESS_DENIED_DEST, nullptr);
    partial.Post(sibling.get(), S_OK, sibling.get());
    partial.FinishOperations(S_OK);
    check(partial.failure == COPYENGINE_E_ACCESS_DENIED_DEST && partial.nested.size() == 2 &&
        partial.nested.back().hr == S_OK,
        "a successful sibling never masks a failed child");

    callbacks.cancelled = [] { return true; };
    ProgressSink cancelled(request, item, callbacks, 0);
    cancelled.Check();
    cancelled.Post(source.get(), S_OK, source.get());
    cancelled.FinishOperations(S_OK);
    check(cancelled.failure == HRESULT_FROM_WIN32(ERROR_CANCELLED),
        "later notifications never clear caller cancellation");
    callbacks.cancelled = {};
    request.kind = ShellOperationKind::CopyBatch;
    size_t completed = 0;
    ProgressSink batch_skipped(request, item, callbacks, 0, &completed);
    check(batch_skipped.PostCopyItem(0, source.get(), nullptr, nullptr, COPYENGINE_S_USER_IGNORED, nullptr) == E_ABORT &&
        batch_skipped.result.skipped && completed == 0, "skipping one native copy stops later copies without adding a completed item");
    sibling.reset(); source.reset();
    CoUninitialize();
    return failures ? 1 : 0;
}
