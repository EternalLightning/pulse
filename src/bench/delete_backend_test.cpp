#include "../ops/ops_manager.h"
#include "../common/localization.h"
#include <algorithm>
#include <filesystem>
#include <iostream>

namespace pulse::ops {
struct DeleteIntegrationProbe {
    static uint32_t LastBackendResult(const OpsManager& manager) { return manager.done_hr_; }
};
}

int main() {
    using namespace pulse;
    l10n::Initialize(GetModuleHandleW(nullptr), L"en-US");
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        std::cout << "[FAIL] cannot inspect process integrity; no deletion attempted\n";
        return 2;
    }
    DWORD needed = 0;
    GetTokenInformation(token, TokenIntegrityLevel, nullptr, 0, &needed);
    std::vector<BYTE> information(needed);
    const bool have_integrity = needed != 0 && GetTokenInformation(token, TokenIntegrityLevel,
        information.data(), needed, &needed) != FALSE;
    CloseHandle(token);
    if (!have_integrity) {
        std::cout << "[FAIL] cannot read process integrity; no deletion attempted\n";
        return 2;
    }
    const auto* label = reinterpret_cast<const TOKEN_MANDATORY_LABEL*>(information.data());
    if (!IsValidSid(label->Label.Sid) || *GetSidSubAuthorityCount(label->Label.Sid) == 0) {
        std::cout << "[FAIL] invalid integrity SID; no deletion attempted\n";
        return 2;
    }
    const DWORD integrity = *GetSidSubAuthority(label->Label.Sid,
        *GetSidSubAuthorityCount(label->Label.Sid) - 1);
    std::cout << "[INFO] test process integrity RID=" << integrity << '\n';
    if (integrity < SECURITY_MANDATORY_MEDIUM_RID) {
        std::cout << "[FAIL] Low-integrity executable cannot validate deletion of the normal user fixture.\n"
                     "[INFO] Inspect integrity labels on this executable AND pulse_shell.exe.\n"
                     "[INFO] Do not lower the fixture label or bypass permissions. Use a trusted normal-integrity build environment.\n"
                     "[FAIL] backend success test NOT RUN; no deletion attempted\n";
        return 2;
    }
    wchar_t module[32768]{};
    if (!GetModuleFileNameW(nullptr, module, ARRAYSIZE(module))) return 2;
    // This executable can only address its dedicated workspace fixture directory.
    const auto root = std::filesystem::path(module).parent_path().parent_path() /
        L"bench_data/review-changes/delete-backend-fixture";
    const auto cancel_path = (root / L"cancel.txt").lexically_normal().wstring();
    const auto delete_path = (root / L"accepted.txt").lexically_normal().wstring();
    if (GetFileAttributesW(cancel_path.c_str()) == INVALID_FILE_ATTRIBUTES ||
        GetFileAttributesW(delete_path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        std::cout << "[FAIL] prepared disposable fixture missing; no deletion attempted\n"; return 2;
    }
    const HWND owner = CreateWindowExW(0, L"STATIC", L"Backend fixture", 0, 0, 0, 0, 0,
        HWND_MESSAGE, nullptr, nullptr, nullptr);
    ops::OpsManager manager;
    manager.SetUiWindow(owner);
    manager.Start([] {});
    int failures = 0;
    auto check = [&](bool passed, const char* label) {
        std::cout << (passed ? "[PASS] " : "[FAIL] ") << label << std::endl;
        failures += !passed;
    };
    auto pump_wait = [&](auto ready) {
        const auto deadline = GetTickCount64() + 15000;
        while (!ready() && GetTickCount64() < deadline) {
            MsgWaitForMultipleObjects(0, nullptr, FALSE, 10, QS_ALLINPUT);
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
        }
        return ready();
    };
    auto cancel_request = ops::OpRequest{};
    cancel_request.type = ops::OpType::RealDelete; cancel_request.sources = {cancel_path};
    uint64_t before = manager.Status().completed_ops;
    manager.Submit(cancel_request);
    check(pump_wait([&] { return manager.PendingDeleteConfirmation().has_value(); }), "backend cancelled request reaches pre-execution gate");
    if (const auto pending = manager.PendingDeleteConfirmation()) manager.ResolveDeleteConfirmation(pending->token, false);
    check(pump_wait([&] { return manager.Status().completed_ops > before; }) &&
        GetFileAttributesW(cancel_path.c_str()) != INVALID_FILE_ATTRIBUTES && manager.DrainCompletions().empty(),
        "real backend cancellation leaves the original disposable file untouched");
    auto accepted_request = ops::OpRequest{};
    accepted_request.type = ops::OpType::RealDelete; accepted_request.sources = {delete_path};
    before = manager.Status().completed_ops;
    const uint64_t seq = manager.Submit(accepted_request);
    check(pump_wait([&] { return manager.PendingDeleteConfirmation().has_value(); }), "accepted request requires whole-plan permanent confirmation");
    if (const auto pending = manager.PendingDeleteConfirmation()) manager.ResolveDeleteConfirmation(pending->token, true);
    const bool finished = pump_wait([&] { return manager.Status().completed_ops > before; });
    if (!finished) manager.CancelCurrent();
    const auto status = manager.Status();
    const auto completions = manager.DrainCompletions();
    check(finished && status.phase == ops::OpPhase::Completed &&
        GetFileAttributesW(delete_path.c_str()) == INVALID_FILE_ATTRIBUTES &&
        completions.size() == 1 && completions[0].task_id == seq &&
        completions[0].sources == std::vector<std::wstring>{delete_path} && !manager.CanUndo(),
        "confirmed permanent deletion returns actual exact root without recycle Undo");
    if (status.phase != ops::OpPhase::Completed) {
        std::wcerr << L"[INFO] backend failure: " << status.last_error << std::endl;
        std::cout << "[INFO] backend HRESULT=0x" << std::hex << ops::DeleteIntegrationProbe::LastBackendResult(manager) << std::dec << std::endl;
    }
    const std::vector<std::wstring> batch_paths = {
        (root / L"batch-one.txt").lexically_normal().wstring(),
        (root / L"batch-two.txt").lexically_normal().wstring(),
        (root / L"batch-folder").lexically_normal().wstring()};
    if (std::all_of(batch_paths.begin(), batch_paths.end(), [](const auto& path) {
        return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
    })) {
        ops::OpRequest batch;
        batch.type = ops::OpType::RealDelete; batch.sources = batch_paths;
        before = manager.Status().completed_ops;
        const auto batch_seq = manager.Submit(batch);
        check(pump_wait([&] { return manager.PendingDeleteConfirmation().has_value(); }), "multi-root batch requires confirmation");
        if (const auto pending = manager.PendingDeleteConfirmation()) manager.ResolveDeleteConfirmation(pending->token, true);
        const bool batch_finished = pump_wait([&] { return manager.Status().completed_ops > before; });
        const auto results = manager.DrainCompletions();
        check(batch_finished && manager.Status().phase == ops::OpPhase::Completed && results.size() == 1 &&
            results[0].task_id == batch_seq && results[0].sources == batch_paths &&
            std::all_of(batch_paths.begin(), batch_paths.end(), [](const auto& path) { return GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES; }),
            "multiple files and nonempty directory report only exact authorized roots");
    } else {
        check(false, "multi-root fixture must be prepared for complete backend validation");
    }
    manager.Stop();
    DestroyWindow(owner);
    std::cout << (failures ? "[FAIL] " : "[PASS] ") << "dedicated permanent-delete backend fixture only; real Recycle Bin untouched\n";
    return failures ? 1 : 0;
}
