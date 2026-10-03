#include "../ops/ops_manager.h"
#include "../ops/recycle_undo_validation.h"
#include "../shell_host/delete_fileop.h"
#include "../shell_host/recycle_fileop.h"
#include <shlobj.h>
#include <filesystem>
#include <fstream>
#include <cstdio>
#include <vector>
#include <algorithm>
#include <tlhelp32.h>
#include <atomic>
#include <thread>

namespace pulse::ops {
struct TransferIntegrationProbe {
    static void Initialize(OpsManager& manager) { manager.running_ = true; manager.accepting_ = true; }
    static void Rename(OpsManager& manager, const OpRequest& request, uint64_t id) {
        manager.RunShellOp(request, id);
    }
    static bool RunQueued(OpsManager& manager, bool cancel = false) {
        if (manager.queue_.empty()) return false;
        auto item = std::move(manager.queue_.front()); manager.queue_.pop_front();
        if (cancel) manager.CancelCurrent();
        manager.ExecuteFileOperation(item);
        return true;
    }
    static uint32_t BackendResult(const OpsManager& manager) { return manager.done_hr_; }
    static void RefreshUndo(OpsManager& manager) { manager.RefreshRecycleUndoValidity(); }
    static void SeedCachedRecycleUndo(OpsManager& manager, const std::wstring& original, const std::wstring& payload) {
        UndoEntry entry;
        entry.type = OpType::RecycleDelete; entry.sources = {original}; entry.destinations = {payload};
        entry.recycle_verified = true;
        manager.undo_.push_back(entry);
    }
    static void KnownMutation(OpsManager& manager, const std::wstring& path) { manager.InvalidateRecycleUndo({path}); }
};
}
namespace {
int failures = 0;
void Check(bool value, const char* name) { std::printf("[%s] %s\n", value ? "PASS" : "FAIL", name); failures += !value; }
void Write(const std::filesystem::path& file, const char* text = "fixture") { std::ofstream(file, std::ios::binary) << text; }
void Index(const std::filesystem::path& index, const std::wstring& original) {
    std::ofstream output(index, std::ios::binary | std::ios::trunc);
    const uint64_t version = 2, size = 7, deleted = 0;
    const uint32_t chars = static_cast<uint32_t>(original.size() + 1);
    output.write(reinterpret_cast<const char*>(&version), sizeof(version));
    output.write(reinterpret_cast<const char*>(&size), sizeof(size));
    output.write(reinterpret_cast<const char*>(&deleted), sizeof(deleted));
    output.write(reinterpret_cast<const char*>(&chars), sizeof(chars));
    output.write(reinterpret_cast<const char*>(original.c_str()), static_cast<std::streamsize>(chars * sizeof(wchar_t)));
}
int SafeRecycleAdapter(bool expect_refusal) {
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    Check(SUCCEEDED(initialized), "explicit production adapter fixture initializes COM");
    if (FAILED(initialized)) return 1;
    const auto parent = std::filesystem::absolute(L"bench_data");
    const auto root = parent / (L"ops-safe-recycle-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    std::filesystem::create_directories(root);
    const auto file = root / L"fixture.txt", directory = root / L"folder", child = directory / L"child.txt";
    Write(file); std::filesystem::create_directory(directory); Write(child);
    std::atomic<bool> stop{false}, native{false};
    const DWORD thread = GetCurrentThreadId();
    std::thread observer([&] {
        while (!stop.load()) {
            struct Windows { DWORD thread; std::atomic<bool>& native; } windows{thread, native};
            EnumWindows([](HWND window, LPARAM raw) -> BOOL {
                auto& value = *reinterpret_cast<Windows*>(raw);
                DWORD pid = 0;
                const DWORD tid = GetWindowThreadProcessId(window, &pid);
                if (pid == GetCurrentProcessId() && tid == value.thread && IsWindowVisible(window)) {
                    value.native.store(true); PostMessageW(window, WM_CLOSE, 0, 0);
                }
                return TRUE;
            }, reinterpret_cast<LPARAM>(&windows));
            Sleep(10);
        }
    });
    const auto cancelled = pulse::shell::RecycleAuthorized({file.wstring(), directory.wstring()}, [] { return true; });
    Check(cancelled.cancelled && std::filesystem::exists(file) && std::filesystem::exists(child) && cancelled.sources.empty(), "explicit adapter cancellation before execution preserves every selected root");
    const auto result = pulse::shell::RecycleAuthorized({file.wstring()});
    std::printf("[DIAG] production explicit recycle hr=0x%08lX source_preserved=%d actual=%zu path=%ls\n",
        static_cast<unsigned long>(result.error), std::filesystem::exists(file), result.sources.size(), result.failed_path.c_str());
    if (expect_refusal) {
        Check(FAILED(result.error) && !result.cancelled && result.sources.empty() &&
            result.destinations.empty() && std::filesystem::exists(file),
            "Low-integrity recycle refusal never falls through to permanent deletion");
        const auto folder_result = pulse::shell::RecycleAuthorized({directory.wstring(), file.wstring()});
        Check(FAILED(folder_result.error) && !folder_result.cancelled && folder_result.sources.empty() &&
            std::filesystem::exists(child) && std::filesystem::exists(file),
            "directory refusal preserves its children and never processes the later selected root");
    } else {
        Check(SUCCEEDED(result.error) && result.sources == std::vector<std::wstring>{file.wstring()} &&
            result.destinations.size() == 1 && !std::filesystem::exists(file), "explicit production adapter records a verified actual recycle");
        pulse::ops::RecycleUndoIdentity identity;
        const bool exact = result.destinations.size() == 1 &&
            pulse::ops::ValidateRecycleUndoPair(file.wstring(), result.destinations.front(), identity);
        Check(exact && MoveFileW(result.destinations.front().c_str(), file.c_str()), "explicit adapter fixture restores its exact validated payload");
    }
    stop.store(true); observer.join();
    Check(!native.load(), "explicit production recycle adapter never displays an owned native provider window");
    CoUninitialize();
    if (root.parent_path() == parent && root.filename().wstring().starts_with(L"ops-safe-recycle-")) {
        if (std::filesystem::exists(file)) DeleteFileW(file.c_str());
        if (std::filesystem::exists(child)) DeleteFileW(child.c_str());
        RemoveDirectoryW(directory.c_str());
        RemoveDirectoryW(root.c_str());
    }
    return failures ? 1 : 0;
}
int NoNativeShell(bool expect_refusal = false) {
    using namespace pulse::ops;
    const auto root = std::filesystem::absolute(std::filesystem::path(L"bench_data") /
        (L"ops-silent-shell-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64())));
    std::filesystem::create_directories(root);
    const auto recycled = root / L"recycled.txt", permanent = root / L"permanent.txt";
    Write(recycled); Write(permanent);
    const HWND owner = CreateWindowExW(0, L"STATIC", L"Pulse fixture owner", 0, 0, 0, 0, 0,
        HWND_MESSAGE, nullptr, nullptr, nullptr);
    OpsManager manager;
    manager.SetUiWindow(owner); manager.Start([] {});
    std::atomic<bool> stop{false}, saw_native{false};
    std::thread observer([&] {
        while (!stop.load()) {
            std::vector<DWORD> hosts;
            HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
            PROCESSENTRY32W entry{sizeof(entry)};
            if (snapshot != INVALID_HANDLE_VALUE && Process32FirstW(snapshot, &entry)) {
                do {
                    if (entry.th32ParentProcessID == GetCurrentProcessId() &&
                        _wcsicmp(entry.szExeFile, L"Pulse.Shell.exe") == 0) hosts.push_back(entry.th32ProcessID);
                } while (Process32NextW(snapshot, &entry));
            }
            if (snapshot != INVALID_HANDLE_VALUE) CloseHandle(snapshot);
            struct Windows { const std::vector<DWORD>& hosts; std::atomic<bool>& seen; } windows{hosts, saw_native};
            EnumWindows([](HWND window, LPARAM raw) -> BOOL {
                const auto& value = *reinterpret_cast<Windows*>(raw);
                DWORD pid = 0; GetWindowThreadProcessId(window, &pid);
                if (IsWindowVisible(window) && std::find(value.hosts.begin(), value.hosts.end(), pid) != value.hosts.end()) {
                    value.seen.store(true);
                    wchar_t title[256]{}; GetWindowTextW(window, title, ARRAYSIZE(title));
                    std::printf("[DIAG] fixture shell native window PID=%lu title=%ls\n", pid, title);
                    PostMessageW(window, WM_CLOSE, 0, 0); // only our own fixture host's window
                }
                return TRUE;
            }, reinterpret_cast<LPARAM>(&windows));
            Sleep(10);
        }
    });
    auto diagnostics = [&](const char* stage) {
        const auto status = manager.Status();
        const auto pending = manager.PendingDeleteConfirmation();
        std::printf("[DIAG] backend_result=0x%08X\n", TransferIntegrationProbe::BackendResult(manager));
        std::printf("[DIAG] %s task=%llu phase=%d active=%d hr=0x%08lX done=%llu failed=%ls error=%ls pending_token=%llu pending_targets=%zu\n",
            stage, static_cast<unsigned long long>(status.task_id), static_cast<int>(status.phase), status.active,
            static_cast<unsigned long>(status.failure_hr), static_cast<unsigned long long>(status.completed_ops),
            status.failed_path.c_str(), status.last_error.c_str(),
            static_cast<unsigned long long>(pending ? pending->token : 0), pending ? pending->plan.targets.size() : 0);
    };
    auto wait = [&](uint64_t before) {
        const auto deadline = GetTickCount64() + 20000;
        while (GetTickCount64() < deadline && (manager.Status().completed_ops <= before || manager.Status().active)) {
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
            Sleep(5);
        }
        return manager.Status().completed_ops > before && !manager.Status().active;
    };
    OpRequest request; request.type = OpType::RecycleDelete; request.sources = {recycled.wstring()};
    auto before = manager.Status().completed_ops;
    manager.Submit(request);
    const bool recycle_finished = wait(before);
    diagnostics("recycle-finished");
    if (expect_refusal) {
        Check(recycle_finished && manager.Status().phase == OpPhase::Failed && std::filesystem::exists(recycled) &&
            manager.DrainCompletions().empty(), "real recycle refusal preserves the source and reports no false mutation");
        Check(!manager.CanUndo() && manager.UndoLabel().empty(), "real recycle refusal cannot create a false delete undo record");
    } else {
        Check(recycle_finished && manager.Status().phase == OpPhase::Completed && !std::filesystem::exists(recycled),
            "real authorized recycle completes through the production Shell adapter");
        before = manager.Status().completed_ops;
        manager.Undo();
        const bool undo_finished = wait(before);
        diagnostics("undo-finished");
        Check(undo_finished && std::filesystem::exists(recycled), "real exact recycle undo restores only the fixture");
    }
    Check(!saw_native.load(), "ordinary recycle displays no owned native Explorer window");
    request.type = OpType::RealDelete; request.sources = {permanent.wstring()};
    before = manager.Status().completed_ops; manager.Submit(request);
    const auto deadline = GetTickCount64() + 10000;
    while (!manager.PendingDeleteConfirmation() && GetTickCount64() < deadline) Sleep(5);
    const auto confirmation = manager.PendingDeleteConfirmation();
    Check(confirmation.has_value() && std::filesystem::exists(permanent), "permanent deletion requires the Pulse authorization decision before execution");
    if (expect_refusal && confirmation) {
        manager.ResolveDeleteConfirmation(confirmation->token, false);
        Check(wait(before) && std::filesystem::exists(permanent) && manager.Status().phase == OpPhase::Cancelled,
            "declining the Pulse permanent-delete decision preserves the file");
        before = manager.Status().completed_ops; manager.Submit(request);
        const auto retry_deadline = GetTickCount64() + 10000;
        while (!manager.PendingDeleteConfirmation() && GetTickCount64() < retry_deadline) Sleep(5);
        const auto retry_confirmation = manager.PendingDeleteConfirmation();
        Check(retry_confirmation.has_value() && retry_confirmation->token != confirmation->token,
            "a new permanent request requires a fresh Pulse authorization token");
        if (retry_confirmation) manager.ResolveDeleteConfirmation(retry_confirmation->token, true);
    } else if (confirmation) manager.ResolveDeleteConfirmation(confirmation->token, true);
    const bool permanent_finished = wait(before);
    diagnostics("permanent-finished");
    Check(permanent_finished && manager.Status().phase == OpPhase::Completed && !std::filesystem::exists(permanent),
        "confirmed permanent deletion completes through the production Shell adapter");
    Check(!saw_native.load(), "Pulse-confirmed permanent deletion displays no second native Explorer window");
    stop.store(true); observer.join(); manager.Stop();
    if (owner) DestroyWindow(owner);
    if (std::filesystem::exists(recycled)) DeleteFileW(recycled.c_str());
    if (std::filesystem::exists(permanent)) DeleteFileW(permanent.c_str());
    RemoveDirectoryW(root.c_str());
    return failures ? 1 : 0;
}
}
int wmain(int argc, wchar_t** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc == 2 && std::wstring(argv[1]) == L"--no-native-shell") return NoNativeShell();
    if (argc == 2 && std::wstring(argv[1]) == L"--no-native-refusal") return NoNativeShell(true);
    if (argc == 2 && std::wstring(argv[1]) == L"--safe-recycle-refusal") return SafeRecycleAdapter(true);
    if (argc == 2 && std::wstring(argv[1]) == L"--safe-recycle-success") return SafeRecycleAdapter(false);
    using namespace pulse::ops;
    const auto root = std::filesystem::absolute(std::filesystem::path(L"bench_data") /
        (L"ops-feedback-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64())));
    std::filesystem::create_directories(root);
    const DWORD permanent = pulse::shell::AuthorizedDeleteFlags(false), recycle = pulse::shell::AuthorizedDeleteFlags(true);
    const DWORD silent = FOF_SILENT | FOF_NOCONFIRMATION | FOF_NOERRORUI | FOFX_EARLYFAILURE;
    Check(permanent == silent && recycle == (silent | FOFX_RECYCLEONDELETE | FOF_ALLOWUNDO), "authorized delete uses silent flags, preserves bin payload compatibility only for recycle, and omits nuke warning");
    Check((recycle & FOFX_RECYCLEONDELETE) != 0 && (permanent & FOFX_RECYCLEONDELETE) == 0,
        "production operation flags, not ambiguous PreDelete transfer flags, select recycle-only versus approved permanent mode");
    Check(pulse::shell::NeedsPermanentDeleteConfirmation(COPYENGINE_E_RECYCLE_SIZE_TOO_BIG) &&
        !pulse::shell::NeedsPermanentDeleteConfirmation(E_ACCESSDENIED) &&
        !pulse::shell::NeedsPermanentDeleteConfirmation(HRESULT_FROM_WIN32(ERROR_NETWORK_ACCESS_DENIED)),
        "only exact recycling failures request a new Pulse permanent confirmation");
    const auto source = root / L"locked.txt", target = root / L"renamed.txt";
    OpRequest rename;
    rename.type = OpType::Rename; rename.sources = {source.wstring()}; rename.new_name = target.filename().wstring();
    auto held = [&] { return CreateFileW(source.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr); };
    Write(source);
    {
        OpsManager manager; TransferIntegrationProbe::Initialize(manager);
        HANDLE lock = held(); Check(lock != INVALID_HANDLE_VALUE, "hold fixture source without delete sharing");
        TransferIntegrationProbe::Rename(manager, rename, 10);
        const auto status = manager.Status();
        Check(status.phase == OpPhase::Failed && status.lock_retry_available && status.failed_path == source.wstring() &&
            manager.IsLockedFailureCurrent(10), "single locked Rename arms Retry/Cancel even when Restart Manager owners are unavailable");
        Check(!manager.RetryLockedOperation(10, true), "single rename cannot offer process termination");
        if (lock != INVALID_HANDLE_VALUE) CloseHandle(lock);
        Check(manager.RetryLockedOperation(10, false) && TransferIntegrationProbe::RunQueued(manager) &&
            manager.Status().phase == OpPhase::Completed && !std::filesystem::exists(source) && std::filesystem::exists(target),
            "plain rename retry uses source identity and succeeds after holder closes");
        Check(!manager.RetryLockedOperation(10, false), "completed rename retry never replays the original task");
    }
    MoveFileW(target.c_str(), source.c_str());
    {
        OpsManager manager; TransferIntegrationProbe::Initialize(manager);
        HANDLE lock = held(); TransferIntegrationProbe::Rename(manager, rename, 20);
        if (lock != INVALID_HANDLE_VALUE) CloseHandle(lock);
        Check(manager.RetryLockedOperation(20, false) && TransferIntegrationProbe::RunQueued(manager, true) &&
            manager.Status().phase == OpPhase::Cancelled && std::filesystem::exists(source) && !std::filesystem::exists(target),
            "queued rename cancel follows the real generation decision path and never renames");
    }
    {
        OpsManager manager; TransferIntegrationProbe::Initialize(manager);
        HANDLE lock = held(); TransferIntegrationProbe::Rename(manager, rename, 30);
        if (lock != INVALID_HANDLE_VALUE) CloseHandle(lock);
        MoveFileW(source.c_str(), (root / L"old-source.txt").c_str()); Write(source, "new identity");
        Check(manager.RetryLockedOperation(30, false) && TransferIntegrationProbe::RunQueued(manager) &&
            manager.Status().phase == OpPhase::Failed && !manager.Status().lock_retry_available &&
            std::filesystem::exists(source) && !std::filesystem::exists(target), "rename retry refuses a replacement source identity");
    }
    {
        OpsManager manager; TransferIntegrationProbe::Initialize(manager);
        HANDLE lock = held(); TransferIntegrationProbe::Rename(manager, rename, 40);
        if (lock != INVALID_HANDLE_VALUE) CloseHandle(lock);
        Write(target, "new target");
        Check(manager.RetryLockedOperation(40, false) && TransferIntegrationProbe::RunQueued(manager) &&
            manager.Status().phase == OpPhase::Failed && !manager.Status().lock_retry_available && std::filesystem::exists(source),
            "rename retry never overwrites a target that appeared while the prompt was open");
    }
    const auto original = root / L"original.txt", payload = root / L"$Rfixture", index = root / L"$Ifixture";
    Write(payload); Index(index, original.wstring());
    RecycleUndoIdentity identity, current;
    Check(ValidateRecycleUndoPair(original.wstring(), payload.wstring(), identity), "worker recycle validator accepts intact fixture payload/index mapping");
    MoveFileW(payload.c_str(), (root / L"$Rsaved").c_str()); Write(payload, "replacement");
    Check(!ValidateRecycleUndoPair(original.wstring(), payload.wstring(), current, &identity), "worker recycle validator rejects replaced payload file identity");
    DeleteFileW(payload.c_str()); MoveFileW((root / L"$Rsaved").c_str(), payload.c_str());
    Index(index, (root / L"different.txt").wstring());
    Check(!ValidateRecycleUndoPair(original.wstring(), payload.wstring(), current), "worker recycle validator rejects metadata referring to a different original");
    Index(index, original.wstring()); Write(original);
    Check(!ValidateRecycleUndoPair(original.wstring(), payload.wstring(), current), "worker recycle validator refuses an occupied restore destination");
    DeleteFileW(original.c_str()); DeleteFileW(index.c_str());
    Check(!ValidateRecycleUndoPair(original.wstring(), payload.wstring(), current), "external removal of metadata invalidates the recycle undo pair");
    {
        OpsManager manager;
        Check(manager.UndoFromJson(L"[{\"type\":2,\"sup\":true,\"dest\":\"\",\"name\":\"\",\"src\":[\"C:\\\\missing.txt\"],\"dst\":[\"C:\\\\$Recycle.Bin\\\\missing\\\\$Rgone\"]}]"), "persisted recycle undo parses without trusting validity");
        Check(!manager.CanUndo() && manager.UndoLabel().empty(), "cached UI undo availability never exposes an unverified persisted deletion label");
        TransferIntegrationProbe::RefreshUndo(manager);
        Check(!manager.CanUndo() && manager.UndoLabel().empty(), "background verification permanently invalidates a missing recycle item");
    }
    {
        OpsManager manager;
        TransferIntegrationProbe::SeedCachedRecycleUndo(manager, original.wstring(), payload.wstring());
        Check(manager.CanUndo(), "known-mutation fixture starts from an enabled cached undo");
        TransferIntegrationProbe::KnownMutation(manager, payload.wstring());
        Check(!manager.CanUndo() && manager.UndoLabel().empty(), "known exact payload mutation immediately invalidates the stale delete undo label");
    }
    Check(root.parent_path() == std::filesystem::absolute(L"bench_data") && root.filename().wstring().starts_with(L"ops-feedback-"), "verify fixture-only cleanup target");
    if (root.parent_path() == std::filesystem::absolute(L"bench_data") && root.filename().wstring().starts_with(L"ops-feedback-"))
        std::filesystem::remove_all(root);
    return failures ? 1 : 0;
}
