// ops_test_main.cpp — Stage 1B-1 ops-layer self test (console).
//
// Drives the full stack: OpsManager -> ShellClient -> pipe -> Pulse.Shell.exe
// -> IFileOperation. All file operations are confined to
// bench_data/opstest (created/cleaned by this test). Prints one PASS/FAIL
// line per check; exit code 0 iff all checks pass.
#include "../ops/ops_manager.h"
#include "file_operation_matrix.h"
#include "../ops/delete_operation.h"
#include "../ops/clipboard.h"
#include "../ops/drive_label_backend.h"
#include "../ops/destination_guard.h"
#include "shell_completion_cases.h"
#include "undo_identity_cases.h"
#include "mutation_undo_cases.h"
#include "source_contract_cases.h"
#include "replace_running_exe_cases.h"
#include "delete_progress_cases.h"
#include "clipboard_isolation_cases.h"
#include "../common/path_utils.h"
#include "../ipc/shell_client.h"
#include <windows.h>
#include <chrono>
#include <cstring>
#include <winioctl.h>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

using namespace pulse;

namespace pulse::ops {
struct TransferIntegrationProbe {
    static void IdentityQuery(OpsManager& manager, decltype(&GetFileInformationByHandleEx) query) {
        manager.transfer_identity_query_ = query;
    }
    static void SourceCleanupGate(OpsManager& manager, std::function<void(const std::wstring&)> gate) {
        manager.before_source_cleanup_ = std::move(gate);
        manager.force_copy_move_ = true;
    }
    static void ExecuteDriveRequest(OpsManager& manager, OpRequest request) {
        OpsManager::QueueItem item;
        item.req = std::move(request); item.seq = 1;
        manager.ExecuteFileOperation(item);
    }
    static void RenameDriveLabel(OpsManager& manager, const OpRequest& request,
                                decltype(&SetVolumeLabelW) set_label, bool cancelled = false) {
        manager.shell_cancel_requested_.store(cancelled);
        manager.RunDriveLabelRename(request, 1, set_label);
    }
    static void RenameMappedDriveLabel(OpsManager& manager, const OpRequest& request,
                                      const DriveLabelBackend& backend, bool cancelled = false) {
        manager.shell_cancel_requested_.store(cancelled);
        manager.RunDriveLabelRename(request, 1, &SetVolumeLabelW, &backend);
    }
    static uint64_t NextTaskId(OpsManager& manager) {
        std::lock_guard<std::mutex> lock(manager.mutex_);
        return manager.next_seq_;
    }
    static void GateOpenWorker(OpsManager& manager, std::function<void()> gate) {
        std::lock_guard<std::mutex> lock(manager.open_mutex_);
        if (manager.open_mailbox_) manager.open_mailbox_->before_execute = std::move(gate);
    }
    static void ShellOperation(OpsManager& manager, const OpRequest& request, bool stopped = false) {
        manager.stopping_.store(stopped);
        manager.RunShellOp(request, 1);
    }
    static void TransferCancelled(OpsManager& manager, const OpRequest& request) {
        manager.stopping_.store(true);
        manager.RunTransfer(request, 1);
    }
    static void DeleteResult(OpsManager& manager, HRESULT failure, const std::wstring& message,
                             const std::wstring& failed_path, const std::vector<std::wstring>& deleted = {}) {
        OpsManager::QueueItem item;
        item.seq = 1; item.req.type = OpType::RealDelete;
        manager.FinishDelete(item, message, deleted, true, IsDeleteBackendLost(failure), {}, {}, false, {}, failure, failed_path);
    }
};
}

namespace {

std::wstring SandboxRoot() {
    std::vector<wchar_t> exe(32768);
    const DWORD length = GetModuleFileNameW(nullptr, exe.data(),
                                            static_cast<DWORD>(exe.size()));
    if (length == 0 || length == exe.size()) return {};
    const std::filesystem::path build_dir =
        std::filesystem::path(std::wstring(exe.data(), length)).parent_path();
    return (build_dir.parent_path() / L"bench_data" / L"opstest").wstring();
}

int g_pass = 0;
int g_fail = 0;

void Check(bool cond, const wchar_t* name) {
    if (cond) {
        ++g_pass;
        wprintf(L"[PASS] %s\n", name);
    } else {
        ++g_fail;
        wprintf(L"[FAIL] %s\n", name);
    }
}

bool Exists(const std::wstring& path) {
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool MakeFile(const std::wstring& path, const void* data, DWORD size) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD w = 0;
    BOOL ok = WriteFile(f, data, size, &w, nullptr);
    CloseHandle(f);
    return ok && w == size;
}

void MakeDir(const std::wstring& path) {
    CreateDirectoryW(path.c_str(), nullptr);
}

uint64_t FileSize(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad)) return ~0ull;
    ULARGE_INTEGER s{ fad.nFileSizeLow, fad.nFileSizeHigh };
    return s.QuadPart;
}

ops::OpsManager g_ops;
std::atomic<bool> g_pause_when_active{false};
std::mutex g_status_mutex;
std::vector<ops::OpStatus> g_status_history;

int RunMultiwindowOpenWorkers() {
    struct Gate {
        std::atomic<unsigned> entered{0};
        std::atomic<unsigned> returned{0};
        std::atomic<bool> release{false};
    };
    auto gate = std::make_shared<Gate>();
    std::vector<std::unique_ptr<ops::OpsManager>> windows;
    for (unsigned i = 0; i < 5; ++i) {
        auto window = std::make_unique<ops::OpsManager>();
        window->Start([] {});
        ops::TransferIntegrationProbe::GateOpenWorker(*window, [gate] {
            ++gate->entered;
            while (!gate->release.load()) Sleep(1);
            ++gate->returned;
        });
        window->OpenWith(L"pulse-open-worker-gate");
        windows.push_back(std::move(window));
    }
    auto deadline = GetTickCount64() + 3000;
    while (gate->entered.load() < windows.size() && GetTickCount64() < deadline) Sleep(1);
    Check(gate->entered.load() == windows.size(),
          L"all five windows have a file-open worker while the other windows remain alive");
    for (auto& window : windows) window->BeginShutdown();
    const auto started = GetTickCount64();
    for (auto& window : windows) window->Stop();
    Check(GetTickCount64() - started < 2000,
          L"closing five windows retires blocked file-open workers within the shutdown bound");
    windows.clear();
    gate->release.store(true);
    deadline = GetTickCount64() + 3000;
    while (gate->returned.load() != gate->entered.load() && GetTickCount64() < deadline) Sleep(1);
    Check(gate->returned.load() == 5,
          L"retired file-open workers finish safely after their window managers are destroyed");
    wprintf(L"== multiwindow open workers: %d passed, %d failed ==\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}

bool MakePatternFile(const std::wstring& path, uint64_t bytes) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    std::vector<unsigned char> block(1024 * 1024);
    for (size_t i = 0; i < block.size(); ++i) block[i] = static_cast<unsigned char>((i * 131u + 17u) & 0xFFu);
    bool ok = true;
    while (bytes > 0) {
        const DWORD chunk = static_cast<DWORD>((std::min<uint64_t>)(bytes, block.size()));
        DWORD written = 0;
        if (!WriteFile(file, block.data(), chunk, &written, nullptr) || written != chunk) {
            ok = false;
            break;
        }
        bytes -= chunk;
    }
    CloseHandle(file);
    return ok;
}

uint64_t FileHash(const std::wstring& path) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                              OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) return 0;
    uint64_t hash = 1469598103934665603ull;
    std::vector<unsigned char> block(1024 * 1024);
    DWORD read = 0;
    while (ReadFile(file, block.data(), static_cast<DWORD>(block.size()), &read, nullptr) && read) {
        for (DWORD i = 0; i < read; ++i) {
            hash ^= block[i];
            hash *= 1099511628211ull;
        }
    }
    CloseHandle(file);
    return hash;
}

bool WaitOpDone(uint64_t prev_completed, int timeout_ms = 90000) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (g_ops.Status().completed_ops > prev_completed) return true;
        Sleep(10);
    }
    return false;
}

// Submit and wait; returns Status snapshot after completion.
ops::OpStatus RunOp(ops::OpRequest req) {
    uint64_t prev = g_ops.Status().completed_ops;
    g_ops.Submit(std::move(req));
    bool ok = WaitOpDone(prev);
    if (!ok) fprintf(stderr, "[test] WaitOpDone TIMEOUT\n");
    return g_ops.Status();
}

ops::OpStatus RunConflictOp(ops::OpRequest req, ops::ConflictChoice choice,
                            bool apply_to_all, size_t* resolved = nullptr) {
    const uint64_t previous = g_ops.Status().completed_ops;
    g_ops.Submit(std::move(req));
    uint64_t token = 0;
    size_t count = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (std::chrono::steady_clock::now() < deadline) {
        if (g_ops.Status().completed_ops > previous) break;
        if (const auto conflict = g_ops.PendingConflict(); conflict && conflict->token != token) {
            token = conflict->token;
            ++count;
            g_ops.ResolveConflict(token, choice, apply_to_all);
        }
        Sleep(1);
    }
    if (resolved) *resolved = count;
    return g_ops.Status();
}

ops::OpRequest SimpleOp(ops::OpType type, std::initializer_list<const wchar_t*> srcs,
                        const wchar_t* dest = nullptr, const wchar_t* name = nullptr) {
    ops::OpRequest r;
    r.type = type;
    for (auto s : srcs) r.sources.push_back(s);
    if (dest) r.dest_dir = dest;
    if (name) r.new_name = name;
    return r;
}

} // namespace

namespace {
std::wstring label_root;
std::wstring label_value;
DWORD label_error = ERROR_SUCCESS;
size_t label_calls = 0;

BOOL WINAPI FakeSetVolumeLabel(LPCWSTR root, LPCWSTR label) {
    ++label_calls;
    label_root = root; label_value = label;
    SetLastError(label_error);
    return label_error == ERROR_SUCCESS;
}

UINT fake_drive_type = DRIVE_REMOTE;
HRESULT alias_result = S_OK;
size_t alias_calls = 0;
size_t alias_reads = 0;
std::wstring alias_root;
std::wstring alias_value;
std::optional<std::wstring> alias_display = L"Project share";

UINT WINAPI FakeDriveType(LPCWSTR) { return fake_drive_type; }
HRESULT FakeSetMappedAlias(const std::wstring& root, const std::wstring& label) {
    ++alias_calls; alias_root = root; alias_value = label;
    return alias_result;
}
std::optional<std::wstring> FakeReadMappedLabel(const std::wstring& root) {
    ++alias_reads; alias_root = root;
    return alias_display;
}

int RunDriveRegressions() {
    const std::vector<std::wstring> roots = {L"C:", L"C:\\", L"c:/", L"\\\\?\\C:\\", L"\\\\?\\c:"};
    for (const auto& root : roots) {
        Check(path::IsDriveRootPath(root), L"drive root spelling is recognized");
        for (const auto type : {ops::OpType::Copy, ops::OpType::Move, ops::OpType::RecycleDelete,
                               ops::OpType::RealDelete, ops::OpType::BatchRename}) {
            ops::OpsManager manager;
            auto request = SimpleOp(type, {root.c_str()}, L"C:\\unused", L"label");
            Check(manager.Submit(request) == 0, L"drive operation rejected at admission without backend work");
            ops::TransferIntegrationProbe::ExecuteDriveRequest(manager, request);
            const auto status = manager.Status();
            Check(status.phase == ops::OpPhase::Failed && status.failure_hr == E_ACCESSDENIED &&
                  status.completed_ops == 1, L"queued drive operation rejected before backend work");
        }
        ops::OpsManager manager;
        auto request = SimpleOp(ops::OpType::Rename, {root.c_str()}, nullptr, L"CON");
        Check(manager.Submit(request) != 0, L"single drive label rename is admitted");
        const auto previous_calls = label_calls;
        ops::TransferIntegrationProbe::RenameDriveLabel(manager, request, FakeSetVolumeLabel);
        Check(label_calls == previous_calls + 1 && label_value == L"CON" &&
              path::EqualInsensitive(label_root, L"C:\\") && manager.Status().phase == ops::OpPhase::Completed,
              L"drive rename writes label to canonical root with volume name rules");
        const auto completed = manager.DrainCompletions();
        Check(completed.size() == 1 && completed.front().sources == request.sources &&
              completed.front().destinations.empty() && completed.front().volume_label == request.new_name && !manager.CanUndo(),
              L"volume label completion refreshes source without filesystem rename undo");
    }
    for (const auto* path : {L"C:\\folder", L"C:relative", L"\\\\server\\share\\", L"\\\\?\\C:\\folder"})
        Check(!pulse::path::IsDriveRootPath(path), L"ordinary and UNC paths are not drive roots");
    {
        ops::OpsManager manager;
        auto request = SimpleOp(ops::OpType::Copy, {L"C:\\unused.txt", L"C:\\"}, L"C:\\unused");
        Check(manager.Submit(request) == 0, L"mixed file and drive selection is rejected as a whole");
        request.type = ops::OpType::Rename;
        Check(manager.Submit(request) == 0, L"multi-source drive rename cannot reach ordinary rename");
    }
    for (const auto type : {ops::OpType::RecycleDelete, ops::OpType::RealDelete}) {
        for (const bool physical : {false, true}) {
            ops::OpsManager manager;
            auto request = SimpleOp(type, {L"C:\\unused.txt"});
            request.delete_targets.push_back(physical
                ? ops::DeleteRequestTarget{L"C:\\unused.txt", {L"\\\\?\\C:\\"}, false}
                : ops::DeleteRequestTarget{L"C:\\", {L"C:\\unused.txt"}, false});
            Check(manager.Submit(request) == 0, L"logical and physical drive delete targets rejected at admission");
            std::wstring error;
            const auto plan = ops::BuildDeletePlan(request, 1, error);
            Check(plan.targets.empty() && !error.empty(), L"drive delete target rejected before deletion planning");
        }
    }
    {
        ops::OpsManager manager;
        const auto request = SimpleOp(ops::OpType::Rename, {L"C:\\"});
        ops::TransferIntegrationProbe::RenameDriveLabel(manager, request, FakeSetVolumeLabel);
        const auto completed = manager.DrainCompletions();
        Check(label_value.empty() && manager.Status().phase == ops::OpPhase::Completed &&
              completed.size() == 1 && completed.front().volume_label && completed.front().volume_label->empty(),
              L"empty drive label clears label");
    }
    {
        ops::OpsManager manager;
        const auto request = SimpleOp(ops::OpType::Rename, {L"C:\\"}, nullptr, L"new label");
        label_error = ERROR_ACCESS_DENIED;
        ops::TransferIntegrationProbe::RenameDriveLabel(manager, request, FakeSetVolumeLabel);
        const auto status = manager.Status();
        Check(status.phase == ops::OpPhase::Failed && status.failure_hr == HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED) &&
              !status.last_error.empty() && manager.DrainCompletions().empty() && !manager.CanUndo(),
              L"volume label failure surfaces error without completion or undo");
        label_error = ERROR_SUCCESS;
    }
    {
        ops::OpsManager manager;
        auto request = SimpleOp(ops::OpType::Rename, {L"C:\\"});
        request.new_name = std::wstring(L"a\0b", 3);
        const auto previous_calls = label_calls;
        ops::TransferIntegrationProbe::RenameDriveLabel(manager, request, FakeSetVolumeLabel);
        Check(label_calls == previous_calls && manager.Status().phase == ops::OpPhase::Failed,
              L"embedded null drive label cannot silently truncate");
        ops::TransferIntegrationProbe::RenameDriveLabel(manager, request, FakeSetVolumeLabel, true);
        Check(label_calls == previous_calls && manager.Status().phase == ops::OpPhase::Cancelled,
              L"cancelled drive label request does not call volume API");
    }
    const ops::DriveLabelBackend backend{FakeDriveType, FakeSetVolumeLabel, FakeSetMappedAlias, FakeReadMappedLabel};
    for (const auto* typed : {L"Project share", L""}) {
        ops::OpsManager manager;
        const auto request = SimpleOp(ops::OpType::Rename, {L"\\\\?\\Z:\\"}, nullptr, typed);
        const auto previous_volume_calls = label_calls;
        const auto previous_alias_calls = alias_calls;
        alias_display = typed[0] ? L"Project share" : L"share (\\\\server)";
        ops::TransferIntegrationProbe::RenameMappedDriveLabel(manager, request, backend);
        const auto completed = manager.DrainCompletions();
        Check(alias_calls == previous_alias_calls + 1 && label_calls == previous_volume_calls &&
              alias_root == L"Z:\\" && alias_value == typed && manager.Status().phase == ops::OpPhase::Completed,
              L"mapped rename uses Shell alias at canonical root without touching server volume label");
        Check(completed.size() == 1 && completed.front().volume_label == alias_display &&
              completed.front().destinations.empty() && !manager.CanUndo(),
              L"mapped completion publishes readback including the default name after clearing alias");
    }
    {
        ops::OpsManager manager;
        fake_drive_type = DRIVE_FIXED;
        const auto previous_alias_calls = alias_calls;
        const auto previous_volume_calls = label_calls;
        ops::TransferIntegrationProbe::RenameMappedDriveLabel(manager,
            SimpleOp(ops::OpType::Rename, {L"C:\\"}, nullptr, L"Local"), backend);
        Check(label_calls == previous_volume_calls + 1 && alias_calls == previous_alias_calls &&
              manager.Status().phase == ops::OpPhase::Completed,
              L"fixed drive backend retains SetVolumeLabel semantics");
        fake_drive_type = DRIVE_REMOTE;
    }
    for (const auto failure : {HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED), HRESULT_FROM_WIN32(ERROR_CANCELLED), E_ABORT}) {
        ops::OpsManager manager;
        alias_result = failure;
        const auto previous_reads = alias_reads;
        ops::TransferIntegrationProbe::RenameMappedDriveLabel(manager,
            SimpleOp(ops::OpType::Rename, {L"Z:\\"}, nullptr, L"Alias"), backend);
        const auto status = manager.Status();
        Check(status.phase == (failure == HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED)
                  ? ops::OpPhase::Failed : ops::OpPhase::Cancelled) && status.failure_hr == failure &&
              alias_reads == previous_reads && manager.DrainCompletions().empty() && !manager.CanUndo(),
              L"mapped Shell failure or cancellation preserves HRESULT without completion or undo");
        Check(!status.elevation_retry_available, L"mapped alias failure never offers volume elevation retry");
    }
    alias_result = S_OK;
    for (const auto display : {std::optional<std::wstring>{}, std::optional<std::wstring>{L""}}) {
        ops::OpsManager manager;
        alias_display = display;
        ops::TransferIntegrationProbe::RenameMappedDriveLabel(manager,
            SimpleOp(ops::OpType::Rename, {L"Z:\\"}), backend);
        Check(manager.Status().phase == ops::OpPhase::Failed && manager.DrainCompletions().empty(),
              L"failed mapped readback never publishes an invented empty display label");
    }
    {
        ops::OpsManager manager;
        const auto previous_alias_calls = alias_calls;
        ops::TransferIntegrationProbe::RenameMappedDriveLabel(manager,
            SimpleOp(ops::OpType::Rename, {L"Z:\\"}), backend, true);
        Check(alias_calls == previous_alias_calls && manager.Status().phase == ops::OpPhase::Cancelled &&
              manager.DrainCompletions().empty(), L"pre-cancelled mapped rename does not enter Shell");
    }
    Check(path::MappedDriveLabelFromDisplayName(L"\\\\?\\Z:\\", L"share (\\\\server) (Z:)") == L"share (\\\\server)" &&
          path::MappedDriveLabelFromDisplayName(L"Z:\\", L"Alias (Z:) content (z:)") == L"Alias (Z:) content" &&
          path::MappedDriveLabelFromDisplayName(L"Z:\\", L"Alias (Y:)") == L"Alias (Y:)",
          L"mapped display parsing removes only its own final drive suffix");
    wprintf(L"\n== drive ops: %d passed, %d failed ==\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

int RunOperationResourceRegressions() {
    const std::wstring root = SandboxRoot() + L"-resources-" + std::to_wstring(GetCurrentProcessId()) +
        L"-" + std::to_wstring(GetTickCount64());
    std::error_code error;
    std::filesystem::create_directories(root, error);
    Check(!error, L"isolated operation resource fixture created");
    if (error) return 1;
    const auto source = root + L"\\source.txt", target = root + L"\\target.txt";
    const char data[] = "resource regression";
    MakeFile(source, data, sizeof(data)); MakeFile(target, data, sizeof(data));
    {
        ops::OpsManager manager;
        auto request = SimpleOp(ops::OpType::BatchRename, {source.c_str()}, nullptr, L"target.txt");
        ops::TransferIntegrationProbe::ShellOperation(manager, request);
        const auto status = manager.Status();
        Check(status.phase == ops::OpPhase::Failed &&
            status.failure_hr == HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS) && status.failed_path == source &&
            Exists(source) && Exists(target), L"batch conflict preserves source and reports HRESULT plus failure path");
        request.new_name = L"invalid|name";
        ops::TransferIntegrationProbe::ShellOperation(manager, request);
        Check(manager.Status().phase == ops::OpPhase::Failed &&
            manager.Status().failure_hr == HRESULT_FROM_WIN32(ERROR_INVALID_NAME),
            L"batch invalid name reports machine error before filesystem mutation");
        request.new_name = L"renamed.txt";
        ops::TransferIntegrationProbe::ShellOperation(manager, request, true);
        Check(manager.Status().phase == ops::OpPhase::Cancelled && manager.Status().last_error.empty() &&
            manager.Status().failure_hr == S_OK && Exists(source), L"batch cancellation has explicit phase without error wording");
    }
    {
        ops::OpsManager manager;
        ops::TransferIntegrationProbe::TransferCancelled(manager,
            SimpleOp(ops::OpType::Copy, {source.c_str()}, (root + L"\\copy").c_str()));
        Check(manager.Status().phase == ops::OpPhase::Cancelled && manager.Status().last_error.empty(),
            L"transfer cancellation uses explicit terminal phase");
    }
    for (const auto* message : {L"Malformed deletion result", L"任意翻译", L"拒绝访问 | unrelated.txt"}) {
        ops::OpsManager manager;
        ops::TransferIntegrationProbe::DeleteResult(manager, ipc::kMalformedDeleteResult, message, source);
        auto outcomes = manager.DrainDeleteOutcomes();
        Check(outcomes.size() == 1 && outcomes.front().uncertain &&
            manager.Status().failure_hr == ipc::kMalformedDeleteResult && manager.Status().failed_path == source,
            L"delete trust classification and failure path are independent of error translation");
        ops::TransferIntegrationProbe::DeleteResult(manager, E_INVALIDARG, message, source);
        outcomes = manager.DrainDeleteOutcomes();
        Check(outcomes.size() == 1 && !outcomes.front().uncertain,
            L"ordinary invalid argument stays certain even with malformed-result display wording");
    }
    {
        ops::OpsManager manager;
        ops::TransferIntegrationProbe::DeleteResult(manager, E_ACCESSDENIED, L"", source, {target});
        Check(manager.Status().phase == ops::OpPhase::Failed && manager.Status().failure_hr == E_ACCESSDENIED,
            L"empty display error cannot turn failed deletion HRESULT into completion");
    }
    {
        std::wstring diagnostics;
        auto locks = ops::ProbeFileLocks(HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION), L"", {source}, L"", {}, &diagnostics);
        Check(locks.owners.empty() && diagnostics.empty(), L"missing failure object never scans candidate files");
        locks = ops::ProbeFileLocks(HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION),
            L"translated | source.txt; delete diagnostics: text", {source}, L"", {}, &diagnostics);
        Check(locks.owners.empty() && diagnostics.empty(), L"lock probe never extracts path from display text");
        locks = ops::ProbeFileLocks(HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION),
            L"missing.txt", {source}, L"", {}, &diagnostics);
        Check(locks.owners.empty() && diagnostics.find(L"RmStartSession") == std::wstring::npos,
            L"unmatched explicit name never probes unrelated files");
        locks = ops::ProbeFileLocks(HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION),
            L"source.txt", {source}, L"", {}, &diagnostics);
        Check(diagnostics.find(L"RmStartSession") != std::wstring::npos,
            L"explicit matching file name retains Restart Manager probe");
    }
    const HWND owner = CreateWindowExW(0, L"STATIC", L"Operation resource fixture", 0,
        0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr);
    Check(owner != nullptr, L"isolated delete confirmation owner created");
    g_ops.SetUiWindow(owner);
    g_ops.Start([] {
        if (const auto confirmation = g_ops.PendingDeleteConfirmation())
            g_ops.ResolveDeleteConfirmation(confirmation->token, true);
    });
    const auto missing_parent = root + L"\\missing\\child";
    auto status = RunOp(SimpleOp(ops::OpType::CreateFolder, {missing_parent.c_str()}));
    Check(status.phase == ops::OpPhase::Failed && status.failure_hr == HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND) &&
        status.failed_path == missing_parent, L"create response preserves non-permission machine failure");
    HANDLE locked = CreateFileW(source.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    Check(locked != INVALID_HANDLE_VALUE, L"delete failure fixture locked without delete sharing");
    if (owner && locked != INVALID_HANDLE_VALUE) {
        status = RunOp(SimpleOp(ops::OpType::RealDelete, {source.c_str()}));
        Check(status.phase == ops::OpPhase::Failed && FAILED(status.failure_hr) &&
            status.failed_path == source && Exists(source), L"Shell delete failure transports exact object through OpsManager");
    }
    if (locked != INVALID_HANDLE_VALUE) CloseHandle(locked);
    g_ops.Stop();
    if (owner) DestroyWindow(owner);
    std::filesystem::remove_all(root, error);
    Check(!error, L"isolated operation resource fixture cleaned");
    wprintf(L"\n== operation resources: %d passed, %d failed ==\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
} // namespace

ops::OpStatus RunLinkOp(ops::OpRequest req, bool accept, bool all,
                       size_t* links = nullptr, size_t* conflicts = nullptr,
                       const std::function<void()>& before_answer = {}) {
    const uint64_t previous = g_ops.Status().completed_ops;
    g_ops.Submit(std::move(req));
    uint64_t token = 0;
    size_t link_count = 0, conflict_count = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (std::chrono::steady_clock::now() < deadline && g_ops.Status().completed_ops == previous) {
        if (const auto pending = g_ops.PendingConflict(); pending && pending->token != token) {
            token = pending->token;
            if (pending->link_confirmation) {
                ++link_count;
                if (before_answer) before_answer();
                g_ops.ResolveConflict(token, accept ? ops::ConflictChoice::Continue
                    : ops::ConflictChoice::Cancel, all);
            } else {
                ++conflict_count;
                g_ops.ResolveConflict(token, ops::ConflictChoice::Replace, false);
            }
        }
        Sleep(1);
    }
    if (links) *links = link_count;
    if (conflicts) *conflicts = conflict_count;
    if (g_ops.Status().completed_ops == previous) {
        g_ops.CancelCurrent();
        Check(WaitOpDone(previous, 5000), L"link regression finishes before timeout");
    }
    return g_ops.Status();
}

bool MakeJunction(const std::wstring& link, const std::wstring& target) {
    MakeDir(link);
    HANDLE handle = CreateFileW(link.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false;
    struct JunctionData {
        DWORD tag;
        WORD data_length;
        WORD reserved;
        WORD substitute_offset;
        WORD substitute_length;
        WORD print_offset;
        WORD print_length;
        wchar_t names[32768];
    } data{};
    const std::wstring substitute = L"\\??\\" + target;
    const size_t substitute_bytes = substitute.size() * sizeof(wchar_t);
    const size_t print_bytes = target.size() * sizeof(wchar_t);
    data.tag = IO_REPARSE_TAG_MOUNT_POINT;
    data.substitute_length = static_cast<WORD>(substitute_bytes);
    data.print_offset = static_cast<WORD>(substitute_bytes + sizeof(wchar_t));
    data.print_length = static_cast<WORD>(print_bytes);
    data.data_length = static_cast<WORD>(8 + substitute_bytes + print_bytes + 2 * sizeof(wchar_t));
    std::memcpy(data.names, substitute.c_str(), substitute_bytes + sizeof(wchar_t));
    std::memcpy(reinterpret_cast<BYTE*>(data.names) + data.print_offset,
                target.c_str(), print_bytes + sizeof(wchar_t));
    DWORD written = 0;
    const bool ok = DeviceIoControl(handle, FSCTL_SET_REPARSE_POINT, &data,
        static_cast<DWORD>(8 + data.data_length), nullptr, 0, &written, nullptr) != FALSE;
    CloseHandle(handle);
    if (!ok) RemoveDirectoryW(link.c_str());
    return ok;
}

int RunReviewRegressions(const std::wstring& filter) {
    const std::wstring root = SandboxRoot() + L"-review-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
    std::error_code error;
    std::filesystem::create_directories(root, error);
    Check(!error, L"isolated review fixture created");
    const auto src = root + L"\\src", dst = root + L"\\dst";
    MakeDir(src); MakeDir(dst);
    g_ops.Start([] {});
    const char old_data[] = "old", new_data[] = "new replacement payload";
    if (filter == L"enumeration") {
        const auto tree = src + L"\\tree";
        MakeDir(tree); MakeDir(tree + L"\\child"); MakeDir(tree + L"\\empty");
        MakeFile(tree + L"\\a.txt", old_data, sizeof(old_data));
        MakeFile(tree + L"\\child\\b.txt", new_data, sizeof(new_data));
        auto status = RunOp(SimpleOp(ops::OpType::Copy, {tree.c_str()}, dst.c_str()));
        Check(status.phase == ops::OpPhase::Completed && status.last_error.empty() &&
            FileHash(tree + L"\\a.txt") == FileHash(dst + L"\\tree\\a.txt") &&
            FileHash(tree + L"\\child\\b.txt") == FileHash(dst + L"\\tree\\child\\b.txt") &&
            Exists(dst + L"\\tree\\empty"), L"shared scan preserves nested copy paths and empty directories");
        g_ops.UndoFromJson(L"[]");

        const auto deep_root = src + L"\\deep";
        MakeDir(deep_root);
        auto deep = deep_root;
        for (int i = 0; i < 130; ++i) { deep += L"\\d"; MakeDir(L"\\\\?\\" + deep); }
        MakeFile(L"\\\\?\\" + deep + L"\\leaf.bin", new_data, sizeof(new_data));
        status = RunOp(SimpleOp(ops::OpType::Copy, {(L"\\\\?\\" + deep_root).c_str()}, (L"\\\\?\\" + dst).c_str()));
        const auto deep_dest = L"\\\\?\\" + dst + L"\\deep" + deep.substr(deep_root.size()) + L"\\leaf.bin";
        Check(status.phase == ops::OpPhase::Completed && FileHash(deep_dest) == FileHash(L"\\\\?\\" + deep + L"\\leaf.bin"),
            L"transfer keeps unlimited depth beyond folder-size limit and extended long paths");
        g_ops.UndoFromJson(L"[]");

        const auto leaf_root = src + L"\\with-link", actual = root + L"\\actual";
        MakeDir(leaf_root); MakeDir(actual);
        MakeFile(actual + L"\\unscanned.txt", old_data, sizeof(old_data));
        const auto link = leaf_root + L"\\junction";
        Check(MakeJunction(link, actual), L"create isolated reparse child fixture");
        status = RunOp(SimpleOp(ops::OpType::Copy, {leaf_root.c_str()}, dst.c_str()));
        Check(status.phase == ops::OpPhase::Completed && status.total_items == 2 &&
            (GetFileAttributesW((dst + L"\\with-link\\junction").c_str()) & FILE_ATTRIBUTE_REPARSE_POINT),
            L"transfer treats nested junction as leaf without adding target file to plan");
        RemoveDirectoryW((dst + L"\\with-link\\junction").c_str()); RemoveDirectoryW(link.c_str());
        g_ops.UndoFromJson(L"[]");

        const auto missing = src + L"\\missing";
        status = RunOp(SimpleOp(ops::OpType::Copy, {missing.c_str()}, dst.c_str()));
        Check(status.phase == ops::OpPhase::Failed && !status.last_error.empty() && !Exists(dst + L"\\missing"),
            L"scan failure propagates instead of executing incomplete plan");

        g_ops.Stop();
        ops::OpsManager cancelled;
        ops::TransferIntegrationProbe::TransferCancelled(cancelled,
            SimpleOp(ops::OpType::Copy, {tree.c_str()}, (root + L"\\cancelled-dst").c_str()));
        Check(cancelled.Status().phase == ops::OpPhase::Cancelled && !Exists(root + L"\\cancelled-dst"),
            L"cancel before scanning performs no destination mutation");
    }
    if (filter == L"link-continue") {
        const auto first_source = src + L"\\one", second_source = src + L"\\two";
        const auto first_target = root + L"\\actual-one", second_target = root + L"\\actual-two";
        const auto replacement = root + L"\\replacement";
        for (const auto& path : {first_source, second_source, first_target, second_target, replacement}) MakeDir(path);
        MakeFile(first_source + L"\\first.txt", new_data, sizeof(new_data));
        MakeFile(second_source + L"\\second.txt", new_data, sizeof(new_data));
        const auto first_link = dst + L"\\one", second_link = dst + L"\\two";
        const bool ready = MakeJunction(first_link, first_target) && MakeJunction(second_link, second_target);
        Check(ready, L"create two independent directory link destinations");
        if (ready) {
            size_t links = 0;
            auto status = RunLinkOp(SimpleOp(ops::OpType::Copy,
                {first_source.c_str(), second_source.c_str()}, dst.c_str()), true, false, &links);
            Check(links == 1 && status.phase == ops::OpPhase::Completed &&
                FileHash(first_source + L"\\first.txt") == FileHash(first_target + L"\\first.txt") &&
                FileHash(second_source + L"\\second.txt") == FileHash(second_target + L"\\second.txt"),
                L"Continue accepts every link destination in one task without an apply-all checkbox");
            g_ops.DrainCompletions();
            MakeFile(first_source + L"\\next-task.txt", new_data, sizeof(new_data));
            status = RunLinkOp(SimpleOp(ops::OpType::Copy, {first_source.c_str()}, dst.c_str()), false, false, &links);
            Check(links == 1 && status.phase == ops::OpPhase::Cancelled && !Exists(first_target + L"\\next-task.txt"),
                L"link acceptance expires at the next task and Cancel writes nothing");
            bool retargeted = false;
            status = RunLinkOp(SimpleOp(ops::OpType::Copy, {first_source.c_str()}, dst.c_str()), true, false, &links, nullptr, [&] {
                retargeted = RemoveDirectoryW(first_link.c_str()) && MakeJunction(first_link, replacement);
            });
            Check(retargeted && status.phase == ops::OpPhase::Failed &&
                !Exists(first_target + L"\\next-task.txt") && !Exists(replacement + L"\\next-task.txt") &&
                g_ops.DrainCompletions().empty(),
                L"task-wide Continue still rejects a changed link identity before any write");
        }
        RemoveDirectoryW(first_link.c_str()); RemoveDirectoryW(second_link.c_str());
    }
    if (filter == L"links" || filter == L"review") {
        const auto outside = root + L"\\actual", redirect = root + L"\\redirect";
        MakeDir(outside); MakeDir(redirect);
        const auto junction = dst + L"\\folder";
        MakeDir(src + L"\\folder");
        MakeFile(src + L"\\folder\\nested.txt", new_data, sizeof(new_data));
        const bool junction_ok = MakeJunction(junction, outside);
        Check(junction_ok, L"create directory junction without symbolic-link privilege");
        if (junction_ok) {
            size_t links = 0;
            auto status = RunLinkOp(SimpleOp(ops::OpType::Copy,
                {(src + L"\\folder").c_str()}, dst.c_str()), false, false, &links);
            Check(links == 1 && status.phase == ops::OpPhase::Cancelled && !Exists(outside + L"\\nested.txt"),
                  L"junction cancellation writes no descendant");
            status = RunLinkOp(SimpleOp(ops::OpType::Copy,
                {(src + L"\\folder").c_str()}, dst.c_str()), true, true, &links);
            Check(links == 1 && status.last_error.empty() &&
                  FileHash(outside + L"\\nested.txt") == FileHash(src + L"\\folder\\nested.txt"),
                  L"directory junction continuation writes actual target");
            status = RunLinkOp(SimpleOp(ops::OpType::Copy,
                {(src + L"\\folder").c_str()}, dst.c_str()), true, true, &links, nullptr, [&] {
                    Check(RemoveDirectoryW(junction.c_str()) && MakeJunction(junction, redirect),
                          L"retarget junction during confirmation");
                });
            Check(links == 1 && status.phase == ops::OpPhase::Failed &&
                  !Exists(redirect + L"\\nested.txt"), L"changed confirmed junction is refused");
            RemoveDirectoryW(junction.c_str());
        }

        MakeFile(src + L"\\hard-a.txt", new_data, sizeof(new_data));
        MakeFile(src + L"\\hard-b.txt", new_data, sizeof(new_data));
        MakeFile(dst + L"\\hard-a.txt", old_data, sizeof(old_data));
        MakeFile(dst + L"\\hard-b.txt", old_data, sizeof(old_data));
        Check(CreateHardLinkW((dst + L"\\alias-a.txt").c_str(), (dst + L"\\hard-a.txt").c_str(), nullptr) &&
              CreateHardLinkW((dst + L"\\alias-b.txt").c_str(), (dst + L"\\hard-b.txt").c_str(), nullptr),
              L"create hard-link fixtures");
        const uint64_t alias_hash = FileHash(dst + L"\\alias-a.txt");
        size_t links = 0, conflicts = 0;
        auto status = RunLinkOp(SimpleOp(ops::OpType::Copy,
            {(src + L"\\hard-a.txt").c_str(), (src + L"\\hard-b.txt").c_str()}, dst.c_str()),
            true, true, &links, &conflicts);
        Check(status.last_error.empty() && links == 1 && conflicts == 2,
              L"continue-all links does not authorize replace-all collisions");
        Check(FileHash(dst + L"\\alias-a.txt") == alias_hash &&
              FileHash(dst + L"\\hard-a.txt") == FileHash(src + L"\\hard-a.txt"),
              L"hard-link replacement changes current name but preserves alias data");
        status = RunLinkOp(SimpleOp(ops::OpType::Copy,
            {(src + L"\\hard-a.txt").c_str()}, (dst + L"\\alias-target").c_str()), true, true, &links);
        Check(status.last_error.empty() && links == 0, L"ordinary destination creates without link confirmation");
        MakeFile(src + L"\\normal.lnk", new_data, sizeof(new_data));
        MakeFile(dst + L"\\normal.lnk", old_data, sizeof(old_data));
        status = RunLinkOp(SimpleOp(ops::OpType::Copy,
            {(src + L"\\normal.lnk").c_str()}, dst.c_str()), true, true, &links);
        Check(status.last_error.empty() && links == 0, L"ordinary .lnk file is not a directory link");

        const auto reparse_source = src + L"\\source-junction";
        const auto reparse_actual = root + L"\\reparse-actual";
        MakeDir(reparse_actual);
        MakeFile(reparse_actual + L"\\must-not-enumerate.txt", old_data, sizeof(old_data));
        Check(MakeJunction(reparse_source, reparse_actual), L"create source junction fixture");
        status = RunLinkOp(SimpleOp(ops::OpType::Copy, {reparse_source.c_str()}, dst.c_str()), true, false);
        Check(status.last_error.empty() && (GetFileAttributesW((dst + L"\\source-junction").c_str()) &
              FILE_ATTRIBUTE_REPARSE_POINT), L"source junction copy creates owned reparse placeholder");
        RemoveDirectoryW((dst + L"\\source-junction").c_str());
        RemoveDirectoryW(reparse_source.c_str());

        MakeFile(src + L"\\temp-owned.txt", new_data, sizeof(new_data));
        const auto collision_temp = dst + L"\\temp-owned.txt.pulse-copy-" +
            std::to_wstring(GetCurrentProcessId()) + L"-" +
            std::to_wstring(ops::TransferIntegrationProbe::NextTaskId(g_ops)) + L"-0";
        MakeFile(collision_temp, old_data, sizeof(old_data));
        const auto collision_hash = FileHash(collision_temp);
        status = RunLinkOp(SimpleOp(ops::OpType::Copy,
            {(src + L"\\temp-owned.txt").c_str()}, dst.c_str()), true, false);
        Check(status.last_error.empty() && Exists(dst + L"\\temp-owned.txt") &&
              FileHash(collision_temp) == collision_hash,
              L"temporary-name-shaped existing file is not cleaned by transfer");

        const auto metadata_source = src + L"\\metadata-dir";
        MakeDir(metadata_source);
        MakeFile(metadata_source + L"\\data.txt", new_data, sizeof(new_data));
        Check(SetFileAttributesW(metadata_source.c_str(), FILE_ATTRIBUTE_DIRECTORY |
              FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED),
              L"set source directory metadata fixture");
        status = RunLinkOp(SimpleOp(ops::OpType::Copy, {metadata_source.c_str()}, dst.c_str()), true, false);
        constexpr DWORD copied_directory_flags = FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED;
        Check(status.last_error.empty() && (GetFileAttributesW((dst + L"\\metadata-dir").c_str()) &
              copied_directory_flags) == copied_directory_flags,
              L"new directory copy preserves attributes through checked handle");
        SetFileAttributesW(metadata_source.c_str(), FILE_ATTRIBUTE_DIRECTORY);
        SetFileAttributesW((dst + L"\\metadata-dir").c_str(), FILE_ATTRIBUTE_DIRECTORY);

        MakeFile(src + L"\\fresh-task.txt", new_data, sizeof(new_data));
        MakeFile(dst + L"\\fresh-task.txt", old_data, sizeof(old_data));
        Check(CreateHardLinkW((dst + L"\\fresh-alias.txt").c_str(), (dst + L"\\fresh-task.txt").c_str(), nullptr),
              L"create next-task hardlink");
        auto replace = SimpleOp(ops::OpType::Copy, {(src + L"\\fresh-task.txt").c_str()}, dst.c_str());
        replace.collision_policy = ops::CollisionPolicy::Replace;
        status = RunLinkOp(std::move(replace), false, true, &links);
        Check(links == 1 && status.phase == ops::OpPhase::Cancelled, L"continue-all permission expires at next task");
    }
    if (filter == L"undo" || filter == L"review") {
        g_ops.UndoFromJson(L"[]");
        const auto original = src + L"\\full.original.name.txt";
        const auto old_target = dst + L"\\full.original.name.txt";
        MakeFile(original, new_data, sizeof(new_data));
        MakeFile(old_target, old_data, sizeof(old_data));
        auto move = SimpleOp(ops::OpType::Move, {original.c_str()}, dst.c_str());
        move.collision_policy = ops::CollisionPolicy::KeepBoth;
        auto status = RunOp(std::move(move));
        const auto renamed = dst + L"\\full.original.name - 副本.txt";
        Check(status.last_error.empty() && !Exists(original) && Exists(renamed), L"Move KeepBoth fixture committed");
        const auto persisted = g_ops.UndoToJson();
        Check(g_ops.UndoFromJson(persisted), L"Move KeepBoth undo JSON compatible");
        HANDLE locked = CreateFileW(renamed.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        const uint64_t failed_previous = g_ops.Status().completed_ops;
        g_ops.Undo();
        Check(WaitOpDone(failed_previous) && g_ops.CanUndo() && !Exists(original),
              L"failed move inverse retains undo record");
        if (locked != INVALID_HANDLE_VALUE) CloseHandle(locked);
        uint64_t previous = g_ops.Status().completed_ops;
        g_ops.Undo();
        Check(WaitOpDone(previous) && Exists(original) && !Exists(renamed) && !g_ops.CanUndo(),
              L"Move KeepBoth undo restores full original name after retry");

        MakeFile(src + L"\\partial-a.txt", new_data, sizeof(new_data));
        MakeFile(src + L"\\partial-b.txt", new_data, sizeof(new_data));
        status = RunOp(SimpleOp(ops::OpType::Move,
            {(src + L"\\partial-a.txt").c_str(), (src + L"\\partial-b.txt").c_str()}, dst.c_str()));
        Check(status.last_error.empty(), L"partial undo fixture moved");
        locked = CreateFileW((dst + L"\\partial-b.txt").c_str(), GENERIC_READ, FILE_SHARE_READ,
                             nullptr, OPEN_EXISTING, 0, nullptr);
        previous = g_ops.Status().completed_ops;
        g_ops.Undo();
        Check(WaitOpDone(previous) && Exists(src + L"\\partial-a.txt") && g_ops.CanUndo(),
              L"partial move inverse keeps remaining item");
        Check(g_ops.UndoToJson().find(L"partial-a.txt") == std::wstring::npos,
              L"successful inverse removed from remaining undo mapping");
        if (locked != INVALID_HANDLE_VALUE) CloseHandle(locked);
        previous = g_ops.Status().completed_ops;
        g_ops.Undo();
        Check(WaitOpDone(previous) && Exists(src + L"\\partial-b.txt") && !g_ops.CanUndo(),
              L"partial inverse retry does not repeat successful item");
        MakeDir(src + L"\\partial-dir");
        MakeFile(src + L"\\partial-dir\\a.txt", new_data, sizeof(new_data));
        MakeFile(src + L"\\partial-dir\\b.txt", new_data, sizeof(new_data));
        MakeDir(dst + L"\\partial-dir");
        MakeFile(dst + L"\\partial-dir\\old.txt", old_data, sizeof(old_data));
        auto directory_move = SimpleOp(ops::OpType::Move, {(src + L"\\partial-dir").c_str()}, dst.c_str());
        directory_move.collision_policy = ops::CollisionPolicy::KeepBoth;
        // Directory/ordinary-directory merges do not collide; force KeepBoth with a file target.
        DeleteFileW((dst + L"\\partial-dir\\old.txt").c_str());
        RemoveDirectoryW((dst + L"\\partial-dir").c_str());
        MakeFile(dst + L"\\partial-dir", old_data, sizeof(old_data));
        status = RunOp(std::move(directory_move));
        const auto moved_directory = dst + L"\\partial-dir - 副本";
        Check(status.last_error.empty() && Exists(moved_directory), L"directory KeepBoth move fixture");
        MakeDir(src + L"\\partial-dir"); // Existing original directory forces per-file merge inverse.
        locked = CreateFileW((moved_directory + L"\\b.txt").c_str(), GENERIC_READ, FILE_SHARE_READ,
                             nullptr, OPEN_EXISTING, 0, nullptr);
        previous = g_ops.Status().completed_ops;
        g_ops.Undo();
        Check(WaitOpDone(previous) && Exists(src + L"\\partial-dir\\a.txt") &&
              !Exists(moved_directory + L"\\a.txt") && g_ops.CanUndo(),
              L"partial directory inverse retains only pending content");
        if (locked != INVALID_HANDLE_VALUE) CloseHandle(locked);
        previous = g_ops.Status().completed_ops;
        g_ops.Undo();
        Check(WaitOpDone(previous) && Exists(src + L"\\partial-dir\\b.txt") &&
              !Exists(moved_directory) && !g_ops.CanUndo(),
              L"partial directory inverse retries pending leaf and cleans empty original root");
        ops::OpsManager legacy;
        Check(legacy.UndoFromJson(L"[{\"type\":1,\"sup\":true,\"dest\":\"C:\\\\tmp\",\"name\":\"\",\"src\":[\"C:\\\\original.txt\"]}]") &&
              !legacy.CanUndo(), L"legacy move history parses without authorizing objects with unknown identity");
    }
    if (filter == L"shutdown" || filter == L"review") {
        MakeFile(src + L"\\shutdown.bin", new_data, sizeof(new_data));
        g_pause_when_active.store(true);
        // Status callbacks may deliberately block the worker's publication gap.
        g_ops.Stop();
        std::atomic<bool> worker_entered{false}, release_worker{false};
        g_ops.Start([&] {
            if (g_ops.Status().active && !worker_entered.exchange(true)) {
                while (!release_worker.load()) Sleep(1);
            }
        });
        const uint64_t previous = g_ops.Status().completed_ops;
        g_ops.Submit(SimpleOp(ops::OpType::Copy, {(src + L"\\shutdown.bin").c_str()}, dst.c_str()));
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!worker_entered.load() && std::chrono::steady_clock::now() < deadline) Sleep(1);
        Check(worker_entered.load() && g_ops.HasPendingFileOperations(), L"pending includes dequeued operation before running FS");
        g_ops.Submit(SimpleOp(ops::OpType::Copy, {(src + L"\\shutdown.bin").c_str()}, (root + L"\\never-start").c_str()));
        g_ops.BeginShutdown();
        Check(g_ops.Submit(SimpleOp(ops::OpType::Copy, {(src + L"\\shutdown.bin").c_str()}, dst.c_str())) == 0,
              L"shutdown rejects new mutations");
        release_worker.store(true);
        g_ops.Stop();
        Check(!g_ops.HasPendingFileOperations() && !Exists(root + L"\\never-start"),
              L"shutdown drops queued mutations and joins in-flight cancellation");
        Check(g_ops.Status().completed_ops == previous + 1, L"shutdown completes only in-flight task");
        g_ops.Stop();

        auto open_entered = std::make_shared<std::atomic<bool>>(false);
        auto release_open = std::make_shared<std::atomic<bool>>(false);
        g_ops.Start([] {});
        ops::TransferIntegrationProbe::GateOpenWorker(g_ops, [open_entered, release_open] {
            open_entered->store(true);
            while (!release_open->load()) Sleep(1);
        });
        g_ops.OpenWith(src + L"\\shutdown.bin");
        const auto open_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!open_entered->load() && std::chrono::steady_clock::now() < open_deadline) Sleep(1);
        Check(open_entered->load(), L"isolated open worker enters blocked provider gate");
        const auto stop_started = std::chrono::steady_clock::now();
        g_ops.BeginShutdown();
        g_ops.Stop();
        Check(std::chrono::steady_clock::now() - stop_started < std::chrono::seconds(2),
              L"blocked open worker retires within bound without retaining manager");
        release_open->store(true);
    }
    g_ops.Stop();
    // Junction names were removed explicitly above; only this PID fixture is removed.
    const auto cleanup_root = filter == L"enumeration" ? std::filesystem::path(L"\\\\?\\" + root) : std::filesystem::path(root);
    std::filesystem::remove_all(cleanup_root, error);
    Check(!error, L"isolated review fixture cleaned");
    wprintf(L"\n== review ops: %d passed, %d failed ==\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

BOOL WINAPI LegacyIdentityQuery(HANDLE handle, FILE_INFO_BY_HANDLE_CLASS kind, LPVOID data, DWORD size) {
    if (kind == FileIdInfo) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    return GetFileInformationByHandleEx(handle, kind, data, size);
}

BOOL WINAPI DeniedIdentityQuery(HANDLE, FILE_INFO_BY_HANDLE_CLASS, LPVOID, DWORD) {
    SetLastError(ERROR_ACCESS_DENIED);
    return FALSE;
}

int RunNetworkIdentityRegressions() {
    const auto root = SandboxRoot() + L"-identity-" + std::to_wstring(GetCurrentProcessId()) +
        L"-" + std::to_wstring(GetTickCount64());
    std::error_code cleanup_error;
    std::filesystem::create_directories(root, cleanup_error);
    Check(!cleanup_error, L"isolated identity fixture created");
    const auto source = root + L"\\source.bin", target = root + L"\\dst\\source.bin";
    const char payload[] = "network provider copy identity regression";
    Check(MakeFile(source, payload, sizeof(payload)), L"identity source created");
    {
        ops::DestinationGuard::Handle handle(CreateFileW(source.c_str(), 0,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr));
        ops::FileIdentity first{}, second{}, denied{};
        Check(ops::ReadFileIdentity(handle.get(), first, LegacyIdentityQuery) && !first.extended &&
              ops::ReadFileIdentity(handle.get(), second, LegacyIdentityQuery) && first == second,
              L"unsupported extended query falls back to stable volume and 64-bit file index");
        Check(!ops::ReadFileIdentity(handle.get(), denied, DeniedIdentityQuery) && GetLastError() == ERROR_ACCESS_DENIED,
              L"access failure does not trigger identity fallback");
        Check(!ops::ReadFileIdentity(INVALID_HANDLE_VALUE, denied, LegacyIdentityQuery),
              L"failed legacy query still refuses identity");
        auto different_format = first;
        different_format.extended = true;
        Check(!(first == different_format), L"different identity formats never compare equal");

        std::wstring error;
        ops::DestinationGuard guard(LegacyIdentityQuery);
        Check(guard.Capture(source, error) && guard.Check(source, error),
              L"legacy destination snapshot remains valid");
        Check(MoveFileW(source.c_str(), (root + L"\\old-source.bin").c_str()) && MakeFile(source, payload, sizeof(payload)),
              L"replace identity fixture while old object remains open");
        Check(!guard.Check(source, error), L"legacy snapshot rejects replaced object at same pathname");
    }
    ops::TransferIntegrationProbe::IdentityQuery(g_ops, LegacyIdentityQuery);
    g_ops.Start([] {});
    auto status = RunOp(SimpleOp(ops::OpType::Copy, {source.c_str()}, (root + L"\\dst").c_str()));
    Check(status.phase == ops::OpPhase::Completed && status.last_error.empty() &&
          Exists(target) && FileHash(source) == FileHash(target),
          L"legacy identity supports directory creation, CopyFile2 ownership and final rename");
    auto replace = SimpleOp(ops::OpType::Copy, {source.c_str()}, (root + L"\\dst").c_str());
    replace.collision_policy = ops::CollisionPolicy::Replace;
    status = RunOp(std::move(replace));
    if (status.phase != ops::OpPhase::Completed)
        wprintf(L"[diagnostic] legacy replace phase=%d hr=0x%08lX path=%s error=%s\n",
            static_cast<int>(status.phase), static_cast<unsigned long>(status.failure_hr), status.failed_path.c_str(), status.last_error.c_str());
    Check(status.phase == ops::OpPhase::Completed && FileHash(source) == FileHash(target),
          L"legacy identity supports checked replacement and backup cleanup");
    const auto empty = root + L"\\empty.bin";
    Check(MakeFile(empty, payload, 0), L"empty copy source created");
    status = RunOp(SimpleOp(ops::OpType::Copy, {empty.c_str()}, (root + L"\\dst").c_str()));
    Check(status.phase == ops::OpPhase::Completed && Exists(root + L"\\dst\\empty.bin"),
          L"empty CopyFile2 stream retains legacy destination ownership");
    g_ops.Stop();
    std::filesystem::remove_all(root, cleanup_error);
    Check(!cleanup_error, L"isolated identity fixture cleaned");
    // Reuse the relevant retargeted-junction and hard-link checks with the same fallback.
    const int result = RunReviewRegressions(L"links");
    ops::TransferIntegrationProbe::IdentityQuery(g_ops, GetFileInformationByHandleEx);
    return result;
}

void TestTransferControl(const std::wstring& srcDir, const std::wstring& dstDir) {
    // --- 12. CopyFile2 bytes, pause/resume, cancel and atomic replacement ----
    {
        const std::wstring large_source = srcDir + L"\\large.bin";
        const std::wstring large_target = dstDir + L"\\large.bin";
        constexpr uint64_t large_bytes = 32ull * 1024ull * 1024ull;
        Check(MakePatternFile(large_source, large_bytes), L"create large transfer fixture");
        const uint64_t source_hash = FileHash(large_source);

        g_pause_when_active.store(true);
        const uint64_t previous = g_ops.Status().completed_ops;
        g_ops.Submit(SimpleOp(ops::OpType::Copy, { large_source.c_str() }, dstDir.c_str()));
        bool paused = false;
        const auto pause_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (std::chrono::steady_clock::now() < pause_deadline) {
            if (g_ops.Status().phase == ops::OpPhase::Paused) { paused = true; break; }
            if (g_ops.Status().completed_ops > previous) break;
            Sleep(1);
        }
        const uint64_t paused_bytes = g_ops.Status().transferred_bytes;
        Sleep(120);
        Check(paused, L"CopyFile2 acknowledges pause");
        Check(!paused || g_ops.Status().transferred_bytes == paused_bytes,
              L"transferred bytes remain stable while paused");
        g_ops.ResumeCurrent();
        Check(WaitOpDone(previous), L"paused copy resumes and completes");
        const auto completed = g_ops.Status();
        if (!completed.last_error.empty()) wprintf(L"resume error: %s\n", completed.last_error.c_str());
        Check(completed.total_bytes == large_bytes && completed.transferred_bytes == large_bytes &&
              completed.total_items == 1 && completed.completed_items == 1,
              L"CopyFile2 publishes real byte and item totals");
        Check(FileHash(large_target) == source_hash, L"copy source and destination hashes match");

        DeleteFileW(large_target.c_str());
        g_pause_when_active.store(true);
        const uint64_t cancel_previous = g_ops.Status().completed_ops;
        g_ops.Submit(SimpleOp(ops::OpType::Copy, { large_source.c_str() }, dstDir.c_str()));
        const auto cancel_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (std::chrono::steady_clock::now() < cancel_deadline &&
               g_ops.Status().phase != ops::OpPhase::Paused) Sleep(1);
        g_ops.CancelCurrent();
        Check(WaitOpDone(cancel_previous), L"paused copy cancellation completes");
        Check(!Exists(large_target), L"cancel removes incomplete destination file");

        const char original[] = "original target must survive";
        MakeFile(large_target, original, sizeof(original) - 1);
        const uint64_t original_hash = FileHash(large_target);
        g_pause_when_active.store(true);
        auto replace = SimpleOp(ops::OpType::Copy, { large_source.c_str() }, dstDir.c_str());
        replace.collision_policy = ops::CollisionPolicy::Replace;
        const uint64_t replace_previous = g_ops.Status().completed_ops;
        g_ops.Submit(std::move(replace));
        const auto replace_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (std::chrono::steady_clock::now() < replace_deadline &&
               g_ops.Status().phase != ops::OpPhase::Paused) Sleep(1);
        g_ops.CancelCurrent();
        Check(WaitOpDone(replace_previous), L"replacement cancellation completes");
        Check(FileHash(large_target) == original_hash,
              L"cancelled atomic replacement preserves original target");
    }

}

int RunJsonUndoParserRegressions();

int RunDirectorySkipRegressions() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    const auto root = SandboxRoot() + L"-directory-skip-" + std::to_wstring(GetCurrentProcessId()) +
        L"-" + std::to_wstring(GetTickCount64());
    std::error_code error;
    std::filesystem::create_directories(root, error);
    if (error) return 2;
    g_ops.Start([] {
        if (const auto conflict = g_ops.PendingConflict())
            g_ops.ResolveConflict(conflict->token, ops::ConflictChoice::Skip, true);
    });
    for (const auto type : {ops::OpType::Copy, ops::OpType::Move}) {
        for (const bool nested : {false, true}) {
            const auto case_root = root + (type == ops::OpType::Copy ? L"\\copy" : L"\\move") +
                (nested ? L"-nested" : L"-root");
            const auto src = case_root + L"\\src", dst = case_root + L"\\dst";
            const auto source_root = src + L"\\tree", destination_root = dst + L"\\tree";
            const auto skipped_source = nested ? source_root + L"\\skip" : source_root;
            const auto skipped_destination = nested ? destination_root + L"\\skip" : destination_root;
            error.clear();
            std::filesystem::create_directories(skipped_source + L"\\child", error);
            std::filesystem::create_directories(nested ? destination_root : dst, error);
            const char source_bytes[] = "source bytes", original_bytes[] = "keep original";
            Check(!error && MakeFile(skipped_source + L"\\child\\leaf.txt", source_bytes, sizeof(source_bytes)) &&
                MakeFile(skipped_destination, original_bytes, sizeof(original_bytes)) &&
                (!nested || MakeFile(source_root + L"\\sibling.txt", source_bytes, sizeof(source_bytes))),
                L"prepare isolated directory versus file skip fixture");
            const auto source_hash = FileHash(skipped_source + L"\\child\\leaf.txt");
            const auto original_hash = FileHash(skipped_destination);
            g_ops.UndoFromJson(L"[]");
            g_ops.DrainCompletions();
            auto status = RunOp(SimpleOp(type, {source_root.c_str()}, dst.c_str()));
            if (status.phase != ops::OpPhase::Completed)
                wprintf(L"[diagnostic] directory skip type=%d nested=%d phase=%d hr=0x%08lX %s\n",
                    static_cast<int>(type), nested, static_cast<int>(status.phase),
                    static_cast<unsigned long>(status.failure_hr), status.last_error.c_str());
            Check(status.phase == ops::OpPhase::Completed && status.last_error.empty(),
                L"skipping directory conflict completes without metadata failure");
            Check(Exists(skipped_source + L"\\child\\leaf.txt") &&
                FileHash(skipped_source + L"\\child\\leaf.txt") == source_hash &&
                FileHash(skipped_destination) == original_hash,
                L"skip preserves source subtree and original destination file");
            const auto completions = g_ops.DrainCompletions();
            const bool skipped_recorded = std::any_of(completions.begin(), completions.end(), [&](const auto& completed) {
                return std::find(completed.sources.begin(), completed.sources.end(), skipped_source) != completed.sources.end();
            });
            Check(!skipped_recorded && (nested ? Exists(destination_root + L"\\sibling.txt") &&
                FileHash(destination_root + L"\\sibling.txt") == source_hash &&
                (type == ops::OpType::Copy || !Exists(source_root + L"\\sibling.txt")) : !g_ops.CanUndo()),
                L"unskipped sibling transfers and skipped directory adds no completion or undo");
        }
    }
    g_ops.Stop();
    std::filesystem::remove_all(root, error);
    Check(!error && !Exists(root), L"isolated directory skip fixtures cleaned");
    wprintf(L"== directory skip: %d passed, %d failed ==\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}

int RunTransferUpdateRegressions() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    const auto root = SandboxRoot() + L"-transfer-updates-" + std::to_wstring(GetCurrentProcessId());
    const auto src = root + L"\\src", dst = root + L"\\dst";
    std::error_code error;
    std::filesystem::create_directories(src, error);
    std::filesystem::create_directories(dst, error);
    if (error) return 2;
    const char old_data[] = "old", new_data[] = "replacement";
    std::atomic<int> observe{0};
    std::atomic<bool> direct_seen{false}, temporary_seen{false}, old_target_seen{false};
    HANDLE rollback_lock = INVALID_HANDLE_VALUE;
    g_ops.Start([&] {
        const auto status = g_ops.Status();
        if (!status.active || status.phase != ops::OpPhase::Running) return;
        const int mode = observe.load();
        if ((mode == 1 || mode == 5) && status.current_item == L"small.bin" &&
            status.completed_items == 0 && Exists(dst + L"\\small.bin")) {
            direct_seen.store(true);
            if (mode == 5) g_ops.CancelCurrent();
        }
        if (mode == 2 && status.current_item == L"large.bin" && status.completed_items == 0) {
            for (const auto& entry : std::filesystem::directory_iterator(dst)) {
                if (entry.path().filename().wstring().find(L".pulse-copy-") != std::wstring::npos &&
                    !Exists(dst + L"\\large.bin")) temporary_seen.store(true);
            }
        }
        if (mode == 3 && status.current_item == L"small.bin" && status.completed_items == 0 &&
            FileSize(dst + L"\\small.bin") == sizeof(old_data)) old_target_seen.store(true);
        if (mode == 4 && status.completed_items == 1 && rollback_lock == INVALID_HANDLE_VALUE) {
            rollback_lock = CreateFileW((dst + L"\\rollback-a.txt").c_str(), GENERIC_READ,
                FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        }
    });

    Check(MakePatternFile(src + L"\\small.bin", 2ull << 20), L"create small direct copy fixture");
    observe.store(1);
    auto status = RunOp(SimpleOp(ops::OpType::Copy, {(src + L"\\small.bin").c_str()}, dst.c_str()));
    observe.store(0);
    Check(status.phase == ops::OpPhase::Completed && direct_seen.load() &&
        FileHash(src + L"\\small.bin") == FileHash(dst + L"\\small.bin"),
        L"small non-conflicting copy writes final name directly");
    DeleteFileW((dst + L"\\small.bin").c_str());
    direct_seen.store(false);
    observe.store(5);
    status = RunOp(SimpleOp(ops::OpType::Copy, {(src + L"\\small.bin").c_str()}, dst.c_str()));
    observe.store(0);
    Check(status.phase == ops::OpPhase::Cancelled && direct_seen.load() && !Exists(dst + L"\\small.bin"),
        L"cancel removes only the owned partial direct copy");

    Check(MakePatternFile(src + L"\\large.bin", 64ull << 20), L"create 64 MiB threshold fixture");
    observe.store(2);
    status = RunOp(SimpleOp(ops::OpType::Copy, {(src + L"\\large.bin").c_str()}, dst.c_str()));
    observe.store(0);
    Check(status.phase == ops::OpPhase::Completed && temporary_seen.load() &&
        FileHash(src + L"\\large.bin") == FileHash(dst + L"\\large.bin"),
        L"64 MiB copy retains temporary name until commit");
    MakeFile(dst + L"\\small.bin", old_data, sizeof(old_data));
    observe.store(3);
    auto replacement = SimpleOp(ops::OpType::Copy, {(src + L"\\small.bin").c_str()}, dst.c_str());
    replacement.collision_policy = ops::CollisionPolicy::Replace;
    status = RunOp(std::move(replacement));
    observe.store(0);
    Check(status.phase == ops::OpPhase::Completed && old_target_seen.load() &&
        FileHash(src + L"\\small.bin") == FileHash(dst + L"\\small.bin"),
        L"small replacement retains original until temporary copy commits");

    g_ops.UndoFromJson(L"[]");
    const auto dir_a = src + L"\\move-a", dir_b = src + L"\\move-b";
    MakeDir(dir_a); MakeDir(dir_b); MakeDir(dir_a + L"\\nested");
    MakeFile(dir_a + L"\\nested\\leaf.txt", new_data, sizeof(new_data));
    MakeFile(dir_b + L"\\leaf.txt", new_data, sizeof(new_data));
    auto identity_a = ops::FileObjectReference::Open(dir_a);
    auto identity_b = ops::FileObjectReference::Open(dir_b);
    status = RunOp(SimpleOp(ops::OpType::Move, {dir_a.c_str(), dir_b.c_str()}, dst.c_str()));
    if (status.phase != ops::OpPhase::Completed)
        wprintf(L"[diagnostic] multi move: phase=%d hr=0x%08lX %s\n", static_cast<int>(status.phase), static_cast<unsigned long>(status.failure_hr), status.last_error.c_str());
    auto moved_a = ops::FileObjectReference::Open(dst + L"\\move-a");
    auto moved_b = ops::FileObjectReference::Open(dst + L"\\move-b");
    Check(status.phase == ops::OpPhase::Completed && identity_a.Matches(moved_a.Get()) &&
        identity_b.Matches(moved_b.Get()) && !Exists(dir_a) && !Exists(dir_b),
        L"same-volume multi-directory move preserves root identities through rename");
    auto previous = g_ops.Status().completed_ops;
    const bool move_succeeded = status.phase == ops::OpPhase::Completed;
    if (move_succeeded) g_ops.Undo();
    Check(move_succeeded && WaitOpDone(previous) && g_ops.Status().phase == ops::OpPhase::Completed &&
        Exists(dir_a + L"\\nested\\leaf.txt") && Exists(dir_b + L"\\leaf.txt") && !g_ops.CanUndo(),
        L"same-volume multi-directory rename undo restores both roots");

    const auto retry_a = src + L"\\retry-a.txt", retry_b = src + L"\\retry-b.txt";
    MakeFile(retry_a, new_data, sizeof(new_data)); MakeFile(retry_b, new_data, sizeof(new_data));
    HANDLE locked = CreateFileW(retry_b.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    auto retry_copy = SimpleOp(ops::OpType::Copy, {retry_a.c_str(), retry_b.c_str()}, dst.c_str());
    retry_copy.collision_policy = ops::CollisionPolicy::KeepBoth;
    status = RunOp(std::move(retry_copy));
    wprintf(L"[diagnostic] locked copy: phase=%d hr=0x%08lX current=%d %s\n", static_cast<int>(status.phase), static_cast<unsigned long>(status.failure_hr), g_ops.IsLockedFailureCurrent(status.task_id), status.last_error.c_str());
    Check(status.phase == ops::OpPhase::Failed && Exists(dst + L"\\retry-a.txt") &&
        g_ops.IsLockedFailureCurrent(status.task_id), L"locked second copy retains resumable first success");
    if (locked != INVALID_HANDLE_VALUE) CloseHandle(locked);
    MakeFile(dst + L"\\retry-a.txt", old_data, sizeof(old_data));
    previous = g_ops.Status().completed_ops;
    Check(g_ops.RetryLockedOperation(status.task_id, false) && WaitOpDone(previous) &&
        g_ops.Status().phase == ops::OpPhase::Completed && Exists(dst + L"\\retry-b.txt") &&
        FileSize(dst + L"\\retry-a.txt") == sizeof(old_data) && !Exists(dst + L"\\retry-a - 副本.txt"),
        L"lock retry skips completed copy and does not create KeepBoth duplicate");

    const auto retry_dir_a = src + L"\\retry-dir-a", retry_dir_b = src + L"\\retry-dir-b";
    MakeDir(retry_dir_a); MakeDir(retry_dir_b);
    MakeFile(retry_dir_a + L"\\leaf.txt", new_data, sizeof(new_data));
    MakeFile(retry_dir_b + L"\\leaf.txt", new_data, sizeof(new_data));
    locked = CreateFileW((retry_dir_b + L"\\leaf.txt").c_str(), GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, 0, nullptr);
    auto retry_move = SimpleOp(ops::OpType::Move, {retry_dir_a.c_str(), retry_dir_b.c_str()}, dst.c_str());
    retry_move.collision_policy = ops::CollisionPolicy::KeepBoth;
    status = RunOp(std::move(retry_move));
    Check(status.phase == ops::OpPhase::Failed && !Exists(retry_dir_a) &&
        Exists(dst + L"\\retry-dir-a\\leaf.txt") && g_ops.IsLockedFailureCurrent(status.task_id),
        L"locked second directory move records first atomic root as completed");
    if (locked != INVALID_HANDLE_VALUE) CloseHandle(locked);
    previous = g_ops.Status().completed_ops;
    Check(g_ops.RetryLockedOperation(status.task_id, false) && WaitOpDone(previous) &&
        g_ops.Status().phase == ops::OpPhase::Completed && !Exists(retry_dir_b) &&
        Exists(dst + L"\\retry-dir-b\\leaf.txt") && !Exists(dst + L"\\retry-dir-a - 副本"),
        L"directory retry skips moved root instead of probing its absent descendants");

    MakeFile(src + L"\\rollback-a.txt", new_data, sizeof(new_data));
    MakeFile(src + L"\\rollback-b.txt", new_data, sizeof(new_data));
    MakeFile(dst + L"\\rollback-a.txt", old_data, sizeof(old_data));
    locked = CreateFileW((src + L"\\rollback-b.txt").c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    observe.store(4);
    auto rollback = SimpleOp(ops::OpType::Copy,
        {(src + L"\\rollback-a.txt").c_str(), (src + L"\\rollback-b.txt").c_str()}, dst.c_str());
    rollback.collision_policy = ops::CollisionPolicy::Replace;
    status = RunOp(std::move(rollback));
    observe.store(0);
    std::wstring backup_path;
    for (const auto& entry : std::filesystem::directory_iterator(dst)) {
        if (entry.path().filename().wstring().find(L".pulse-backup-") != std::wstring::npos)
            backup_path = entry.path().wstring();
    }
    Check(status.phase == ops::OpPhase::Failed && !backup_path.empty() &&
        status.last_error.find(backup_path) != std::wstring::npos && FileSize(backup_path) == sizeof(old_data),
        L"failed replacement rollback reports retained backup containing original data");
    if (locked != INVALID_HANDLE_VALUE) CloseHandle(locked);
    if (rollback_lock != INVALID_HANDLE_VALUE) CloseHandle(rollback_lock);
    g_ops.Stop();
    identity_a.handle.reset(); identity_b.handle.reset();
    moved_a.handle.reset(); moved_b.handle.reset();
    std::filesystem::remove_all(root, error);
    Check(!error && !Exists(root), L"isolated transfer update fixture cleaned");
    wprintf(L"== transfer updates: %d passed, %d failed ==\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}

int wmain(int argc, wchar_t** argv) {
    if (argc == 2 && std::wstring_view(argv[1]) == L"--replacement-fixture-child") return 0;
    if (argc >= 2 && std::wstring_view(argv[1]) == L"--file-operation-matrix") {
        if (argc != 3 && argc != 4) {
            fprintf(stderr, "usage: pulse_ops_test --file-operation-matrix LOCAL_PARENT [NETWORK_PARENT]\n");
            return 2;
        }
        return pulse::bench::RunFileOperationMatrix(argv[2], argc == 4 ? argv[3] : L"");
    }
    if (argc == 2 && std::wstring_view(argv[1]) == L"--json-parser-only")
        return RunJsonUndoParserRegressions();
    if (argc == 3 && std::wstring_view(argv[1]) == L"--network-fixture") {
        setvbuf(stdout, nullptr, _IONBF, 0);
        const auto drive = std::filesystem::path(argv[2]).root_path().wstring();
        if (drive.empty() || GetDriveTypeW(drive.c_str()) != DRIVE_REMOTE) {
            fprintf(stderr, "[FAIL] --network-fixture requires an existing remote volume\n");
            return 2;
        }
        const auto suffix = std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
        const auto local = SandboxRoot() + L"-network-" + suffix;
        const auto remote = std::wstring(argv[2]) + L"\\pulse-transfer-fixture-" + suffix;
        wprintf(L"[INFO] network fixture root=%s\n", remote.c_str());
        std::error_code error;
        std::filesystem::create_directories(local, error);
        if (error || !CreateDirectoryW(remote.c_str(), nullptr)) return 2;
        const char payload[] = "Pulse isolated network transfer test. No user data.\n";
        const auto source = local + L"\\payload.txt", target = remote + L"\\payload.txt";
        const auto empty = local + L"\\empty.txt", moving = local + L"\\moving.txt";
        Check(MakeFile(source, payload, sizeof(payload) - 1) && MakeFile(empty, payload, 0) &&
            MakeFile(moving, payload, sizeof(payload) - 1), L"fixed non-sensitive network fixture created");
        g_ops.Start([] {});
        auto status = RunOp(SimpleOp(ops::OpType::Copy, {source.c_str()}, remote.c_str()));
        if (!status.last_error.empty()) wprintf(L"copy error: %s\n", status.last_error.c_str());
        Check(status.phase == ops::OpPhase::Completed && Exists(target) && FileHash(source) == FileHash(target),
            L"real network copy completes final rename and byte verification");
        const auto original_hash = FileHash(target);
        const char replacement[] = "Replacement contents must not be written before conflict acceptance.\n";
        Check(MakeFile(source, replacement, sizeof(replacement) - 1), L"prepare different bytes for a real network conflict");
        size_t conflicts = 0;
        status = RunConflictOp(SimpleOp(ops::OpType::Copy, {source.c_str()}, remote.c_str()),
            ops::ConflictChoice::Cancel, false, &conflicts);
        Check(conflicts == 1 && status.phase == ops::OpPhase::Cancelled &&
            FileHash(target) == original_hash && FileHash(source) != original_hash,
            L"cancelled network paste conflict preserves both original byte streams");
        status = RunConflictOp(SimpleOp(ops::OpType::Copy, {source.c_str()}, remote.c_str()),
            ops::ConflictChoice::Skip, true, &conflicts);
        Check(conflicts == 1 && status.phase == ops::OpPhase::Completed && FileHash(target) == original_hash,
            L"skipping a network paste conflict leaves destination contents unchanged");
        auto replace = SimpleOp(ops::OpType::Copy, {source.c_str()}, remote.c_str());
        replace.collision_policy = ops::CollisionPolicy::Replace;
        status = RunOp(std::move(replace));
        if (!status.last_error.empty()) wprintf(L"replace error: %s\n", status.last_error.c_str());
        Check(status.phase == ops::OpPhase::Completed && FileHash(source) == FileHash(target),
            L"real network replacement completes identity checks and backup cleanup");
        status = RunOp(SimpleOp(ops::OpType::Copy, {empty.c_str()}, remote.c_str()));
        if (!status.last_error.empty()) wprintf(L"empty error: %s\n", status.last_error.c_str());
        Check(status.phase == ops::OpPhase::Completed && FileSize(remote + L"\\empty.txt") == 0,
            L"real network empty file copy completes");
        const auto moving_hash = FileHash(moving);
        status = RunOp(SimpleOp(ops::OpType::Move, {moving.c_str()}, remote.c_str()));
        if (!status.last_error.empty()) wprintf(L"move error: %s\n", status.last_error.c_str());
        Check(status.phase == ops::OpPhase::Completed && !Exists(moving) &&
            Exists(remote + L"\\moving.txt") && FileHash(remote + L"\\moving.txt") == moving_hash && g_ops.CanUndo(),
            L"real cross-volume network move commits before deleting isolated source");
        const auto before_undo = g_ops.Status().completed_ops;
        g_ops.Undo();
        Check(WaitOpDone(before_undo) && g_ops.Status().phase == ops::OpPhase::Completed &&
            Exists(moving) && FileHash(moving) == moving_hash && !Exists(remote + L"\\moving.txt"),
            L"cross-volume cut Undo restores the exact local bytes and removes the remote product");
        const auto return_dir = local + L"\\returned";
        MakeDir(return_dir);
        status = RunOp(SimpleOp(ops::OpType::Copy, {target.c_str()}, return_dir.c_str()));
        Check(status.phase == ops::OpPhase::Completed && Exists(return_dir + L"\\payload.txt") &&
            FileHash(return_dir + L"\\payload.txt") == FileHash(source),
            L"remote-to-local paste preserves the accepted replacement bytes");
        const auto unicode = local + L"\\中文 空格.txt";
        Check(MakeFile(unicode, payload, sizeof(payload) - 1), L"create a Unicode path fixture");
        status = RunOp(SimpleOp(ops::OpType::Copy, {unicode.c_str()}, remote.c_str()));
        Check(status.phase == ops::OpPhase::Completed && Exists(remote + L"\\中文 空格.txt") &&
            FileHash(remote + L"\\中文 空格.txt") == FileHash(unicode),
            L"network copy preserves Unicode and space-containing names and contents");
        g_ops.Stop();
        g_ops.UndoFromJson(L"[]");
        g_ops.ClearFinishedTasks();
        {
            const auto race = remote + L"\\race.txt";
            Check(MakeFile(race, payload, sizeof(payload) - 1), L"network replacement identity fixture created");
            ops::DestinationGuard guard;
            std::wstring guard_error;
            Check(guard.Capture(race, guard_error) && guard.Check(race, guard_error),
                L"real network anchored file snapshot remains valid");
            Check(MoveFileW(race.c_str(), (remote + L"\\race-old.txt").c_str()) &&
                MakeFile(race, payload, sizeof(payload) - 1), L"replace isolated network file at same pathname");
            Check(!guard.Check(race, guard_error), L"real network anchored snapshot still refuses a replaced object");
        }
        for (const auto* name : {L"payload.txt", L"empty.txt", L"moving.txt", L"中文 空格.txt", L"race.txt", L"race-old.txt"}) {
            const auto leaf = remote + L"\\" + name;
            if (Exists(leaf)) Check(DeleteFileW(leaf.c_str()) != FALSE, L"remove own network fixture leaf");
        }
        const bool removed = RemoveDirectoryW(remote.c_str()) != FALSE;
        if (!removed)
            wprintf(L"[diagnostic] network fixture RemoveDirectory path=%s error=%lu\n", remote.c_str(), GetLastError());
        Check(removed, L"remove own unique network fixture directory");
        std::filesystem::remove_all(local, error);
        Check(!error, L"isolated local network fixture cleaned");
        wprintf(L"== real network: %d passed, %d failed ==\n", g_pass, g_fail);
        return g_fail ? 1 : 0;
    }
    if (argc == 3 && std::wstring_view(argv[1]) == L"--check-destination") {
        std::wstring error;
        ops::DestinationGuard guard;
        const bool accepted = guard.Capture(argv[2], error) && guard.PinDirectories(argv[2], error) &&
            guard.Check(argv[2], error);
        wprintf(L"[%s] read-only destination identity check: %s\n%s\n", accepted ? L"PASS" : L"FAIL", argv[2], error.c_str());
        return accepted ? 0 : 1;
    }
    if (argc == 3 && std::wstring_view(argv[1]) == L"--filter") {
        const std::wstring filter = argv[2];
        if (filter == L"open-workers") return RunMultiwindowOpenWorkers();
        if (filter == L"clipboard-isolated") return ops::RunClipboardIsolationRegressions();
        if (filter == L"delete-progress") {
            setvbuf(stdout, nullptr, _IONBF, 0);
            return ops::RunDeleteProgressRegressions(SandboxRoot() + L"-delete-progress-" +
                std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
        }
        if (filter == L"directory-skip") return RunDirectorySkipRegressions();
        if (filter == L"transfer-updates") return RunTransferUpdateRegressions();
        if (filter == L"running-exe-replacement") {
            setvbuf(stdout, nullptr, _IONBF, 0);
            return bench::RunRunningExeReplacementChecks(SandboxRoot() + L"-running-exe-" +
                std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
        }
        if (filter == L"source-contract") {
            setvbuf(stdout, nullptr, _IONBF, 0);
            return ops::RunSourceContractRegressions(SandboxRoot() + L"-source-contract-" +
                std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
        }
        if (filter == L"shell-completion") return ops::RunShellCompletionRegressions();
        if (filter == L"mutation-undo") {
            setvbuf(stdout, nullptr, _IONBF, 0);
            return ops::RunMutationUndoRegressions(SandboxRoot() + L"-mutation-undo-" +
                std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
        }
        if (filter == L"undo-identity") {
            setvbuf(stdout, nullptr, _IONBF, 0);
            return ops::RunUndoIdentityRegressions(SandboxRoot() + L"-undo-identity-" +
                std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
        }
        if (filter == L"source-reparse-identity") {
            setvbuf(stdout, nullptr, _IONBF, 0);
            const auto root = SandboxRoot() + L"-source-reparse-" + std::to_wstring(GetCurrentProcessId()) +
                L"-" + std::to_wstring(GetTickCount64());
            const auto from = root + L"\\src", target = root + L"\\dst";
            const auto outside = root + L"\\outside", unrelated = root + L"\\unrelated";
            const auto holdings = root + L"\\holdings";
            std::error_code error;
            for (const auto& path : {root, from, target, outside, unrelated, holdings})
                std::filesystem::create_directories(path, error);
            if (error) return 2;
            const char payload[] = "unmodified junction target";
            Check(MakeFile(outside + L"\\payload.txt", payload, sizeof(payload)), L"create isolated junction target bytes");
            const auto link = from + L"\\link", held = holdings + L"\\held";
            Check(MakeJunction(link, outside), L"create isolated source junction");
            g_ops.Start([] {});
            ops::TransferIntegrationProbe::SourceCleanupGate(g_ops, {});
            auto status = RunOp(SimpleOp(ops::OpType::Move, {link.c_str()}, target.c_str()));
            Check(status.phase == ops::OpPhase::Completed && !Exists(link) && Exists(target + L"\\link\\payload.txt") &&
                Exists(outside + L"\\payload.txt"), L"copied reparse move deletes only original link and retains target contents");
            RemoveDirectoryW((target + L"\\link").c_str());
            Check(MakeJunction(link, outside), L"recreate isolated source junction for replacement");
            bool replaced = false;
            ops::TransferIntegrationProbe::SourceCleanupGate(g_ops, [&](const std::wstring& path) {
                replaced = MoveFileW(path.c_str(), held.c_str()) && MakeJunction(path, unrelated);
            });
            status = RunOp(SimpleOp(ops::OpType::Move, {link.c_str()}, target.c_str()));
            Check(replaced && status.phase == ops::OpPhase::Failed && Exists(link) && Exists(held) &&
                Exists(target + L"\\link\\payload.txt") && Exists(outside + L"\\payload.txt"),
                L"replacement junction survives cleanup and copied link keeps original target");
            ops::TransferIntegrationProbe::SourceCleanupGate(g_ops, {});
            g_ops.Stop();
            for (const auto& path : {link, held, target + L"\\link"}) RemoveDirectoryW(path.c_str());
            std::filesystem::remove_all(root, error);
            Check(!error && !Exists(root), L"isolated source reparse fixture cleaned");
            return g_fail ? 1 : 0;
        }
        if (filter == L"move-undo-source-identity") {
            setvbuf(stdout, nullptr, _IONBF, 0);
            const auto root = SandboxRoot() + L"-move-undo-source-identity-" +
                std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
            const auto source = root + L"\\src", target = root + L"\\dst";
            const auto original_folder = source + L"\\folder", moved_folder = target + L"\\folder";
            const auto holdings = root + L"\\holdings";
            const auto held_folder = holdings + L"\\original-held";
            std::error_code error;
            std::filesystem::create_directories(original_folder, error);
            std::filesystem::create_directories(target, error);
            std::filesystem::create_directories(holdings, error);
            if (error) return 2;
            const char payload[] = "original payload";
            Check(MakeFile(original_folder + L"\\payload.txt", payload, sizeof(payload)),
                L"create partial move inverse identity fixture");
            g_ops.Start([] {});
            ops::TransferIntegrationProbe::SourceCleanupGate(g_ops, {});
            auto status = RunOp(SimpleOp(ops::OpType::Move, {original_folder.c_str()}, target.c_str()));
            Check(status.phase == ops::OpPhase::Completed && g_ops.CanUndo(),
                L"directory move records its inverse");
            MakeDir(original_folder); // Existing original forces a per-file merge inverse.
            bool replaced = false;
            ops::TransferIntegrationProbe::SourceCleanupGate(g_ops, [&](const std::wstring& path) {
                if (path == moved_folder && !replaced)
                    replaced = MoveFileW(path.c_str(), held_folder.c_str()) &&
                        CreateDirectoryW(path.c_str(), nullptr);
            });
            const auto previous = g_ops.Status().completed_ops;
            g_ops.Undo();
            Check(WaitOpDone(previous), L"partial move inverse finishes after source replacement");
            status = g_ops.Status();
            Check(replaced && status.phase == ops::OpPhase::Failed &&
                status.failure_hr == HRESULT_FROM_WIN32(ERROR_FILE_INVALID) &&
                status.failed_path == moved_folder,
                L"partial move inverse rejects the replaced source directory identity");
            Check(Exists(moved_folder) && Exists(held_folder) &&
                FileSize(original_folder + L"\\payload.txt") == sizeof(payload),
                L"failed inverse recovery preserves the replacement empty directory and restored file");
            const auto unresolved = g_ops.UndoToJson();
            Check(!g_ops.CanUndo() && unresolved.find(L"folder") != std::wstring::npos &&
                unresolved.find(L"\"sup\":false") != std::wstring::npos,
                L"identity rejection retains the unresolved inverse without authorizing a replacement retry");
            const auto rejected_previous = g_ops.Status().completed_ops;
            g_ops.Undo();
            Check(g_ops.Status().completed_ops == rejected_previous && Exists(moved_folder),
                L"repeating rejected Undo does not admit the replacement directory as a new source");
            ops::TransferIntegrationProbe::SourceCleanupGate(g_ops, {});
            const auto ordinary_folder = source + L"\\ordinary", ordinary_moved = target + L"\\ordinary";
            MakeDir(ordinary_folder);
            Check(MakeFile(ordinary_folder + L"\\payload.txt", payload, sizeof(payload)),
                L"create authenticated directory inverse control");
            status = RunOp(SimpleOp(ops::OpType::Move, {ordinary_folder.c_str()}, target.c_str()));
            MakeDir(ordinary_folder);
            const auto ordinary_previous = g_ops.Status().completed_ops;
            g_ops.Undo();
            Check(status.phase == ops::OpPhase::Completed && WaitOpDone(ordinary_previous) &&
                g_ops.Status().phase == ops::OpPhase::Completed && !Exists(ordinary_moved) &&
                FileSize(ordinary_folder + L"\\payload.txt") == sizeof(payload),
                L"authenticated directory inverse still restores files and removes its own empty source");
            g_ops.Stop();
            std::filesystem::remove_all(root, error);
            Check(!error && !Exists(root), L"partial move inverse identity fixture cleaned");
            return g_fail ? 1 : 0;
        }
        if (filter == L"source-identity") {
            setvbuf(stdout, nullptr, _IONBF, 0);
            const auto root = SandboxRoot() + L"-source-identity-" + std::to_wstring(GetCurrentProcessId()) +
                L"-" + std::to_wstring(GetTickCount64());
            const auto source = root + L"\\src", target = root + L"\\dst";
            const auto holdings = root + L"\\holdings";
            std::error_code error;
            std::filesystem::create_directories(source, error);
            std::filesystem::create_directories(target, error);
            std::filesystem::create_directories(holdings, error);
            if (error) return 2;
            g_ops.Start([] {});
            ops::TransferIntegrationProbe::SourceCleanupGate(g_ops, {});
            const char original[] = "original object", replacement[] = "unrelated replacement object";
            for (const auto* name : {L"normal.txt", L"readonly.txt", L"empty.txt"}) {
                const auto from = source + L"\\" + name, to = target + L"\\" + name;
                const DWORD bytes = std::wstring_view(name) == L"empty.txt" ? 0 : sizeof(original);
                Check(MakeFile(from, original, bytes), L"create isolated move source");
                if (std::wstring_view(name) == L"readonly.txt")
                    Check(SetFileAttributesW(from.c_str(), FILE_ATTRIBUTE_READONLY) != FALSE, L"mark original source readonly");
                const auto status = RunOp(SimpleOp(ops::OpType::Move, {from.c_str()}, target.c_str()));
                if (!status.last_error.empty()) wprintf(L"move error: %s\n", status.last_error.c_str());
                Check(status.phase == ops::OpPhase::Completed && !Exists(from) && FileSize(to) == bytes,
                    L"copied move removes only authenticated original, including readonly and empty files");
            }
            const auto from = source + L"\\replaced.txt", held = holdings + L"\\original-held.txt";
            const auto to = target + L"\\replaced.txt";
            Check(MakeFile(from, original, sizeof(original)), L"create replacement race fixture");
            const auto original_hash = FileHash(from);
            bool replaced = false;
            ops::TransferIntegrationProbe::SourceCleanupGate(g_ops, [&](const std::wstring& path) {
                replaced = MoveFileW(path.c_str(), held.c_str()) && MakeFile(path, replacement, sizeof(replacement)) &&
                    SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_READONLY);
            });
            auto status = RunOp(SimpleOp(ops::OpType::Move, {from.c_str()}, target.c_str()));
            Check(replaced && status.phase == ops::OpPhase::Failed && status.failure_hr == HRESULT_FROM_WIN32(ERROR_FILE_INVALID) &&
                status.failed_path == from, L"replacement after destination commit fails with exact source identity error");
            Check(FileSize(from) == sizeof(replacement) && (GetFileAttributesW(from.c_str()) & FILE_ATTRIBUTE_READONLY) &&
                FileHash(to) == original_hash && FileHash(held) == original_hash,
                L"replacement content and attributes survive while committed copy retains original bytes");
            ops::TransferIntegrationProbe::SourceCleanupGate(g_ops, {});
            const auto folder = source + L"\\folder", old_folder = holdings + L"\\folder-held";
            MakeDir(folder);
            bool folder_replaced = false;
            ops::TransferIntegrationProbe::SourceCleanupGate(g_ops, [&](const std::wstring& path) {
                if (path == folder) folder_replaced = MoveFileW(path.c_str(), old_folder.c_str()) &&
                    CreateDirectoryW(path.c_str(), nullptr) && MakeFile(path + L"\\new.txt", replacement, sizeof(replacement));
            });
            status = RunOp(SimpleOp(ops::OpType::Move, {folder.c_str()}, target.c_str()));
            Check(folder_replaced && status.phase == ops::OpPhase::Failed && Exists(folder + L"\\new.txt") &&
                Exists(target + L"\\folder"), L"directory cleanup rejects replacement and keeps unrelated descendants");
            ops::TransferIntegrationProbe::SourceCleanupGate(g_ops, {});
            g_ops.Stop();
            SetFileAttributesW(from.c_str(), FILE_ATTRIBUTE_NORMAL);
            std::filesystem::remove_all(root, error);
            Check(!error && !Exists(root), L"isolated source identity fixture cleaned");
            wprintf(L"== source identity: %d passed, %d failed ==\n", g_pass, g_fail);
            return g_fail ? 1 : 0;
        }
        if (filter == L"transfer-control") {
            setvbuf(stdout, nullptr, _IONBF, 0);
            const auto root = SandboxRoot() + L"-transfer-control-" + std::to_wstring(GetCurrentProcessId());
            std::error_code error;
            const auto source = root + L"\\src", target = root + L"\\dst";
            std::filesystem::create_directories(source, error);
            std::filesystem::create_directories(target, error);
            if (error) return 2;
            g_ops.Start([] {
                const auto status = g_ops.Status();
                if (g_pause_when_active.load() && status.active && status.phase == ops::OpPhase::Scanning) {
                    g_pause_when_active.store(false);
                    g_ops.PauseCurrent();
                }
            });
            TestTransferControl(source, target);
            g_ops.Stop();
            std::filesystem::remove_all(root, error);
            Check(!error, L"isolated transfer control fixture cleaned");
            wprintf(L"== transfer control: %d passed, %d failed ==\n", g_pass, g_fail);
            return g_fail ? 1 : 0;
        }
        if (filter == L"network-identity") {
            setvbuf(stdout, nullptr, _IONBF, 0);
            return RunNetworkIdentityRegressions();
        }
        if (filter == L"operation-resources") {
            setvbuf(stdout, nullptr, _IONBF, 0);
            return RunOperationResourceRegressions();
        }
        if (filter == L"drives") {
            setvbuf(stdout, nullptr, _IONBF, 0);
            return RunDriveRegressions();
        }
        if (filter != L"review" && filter != L"links" && filter != L"link-continue" && filter != L"undo" && filter != L"shutdown" && filter != L"enumeration") return 2;
        setvbuf(stdout, nullptr, _IONBF, 0);
        return RunReviewRegressions(filter);
    }
    setvbuf(stdout, nullptr, _IONBF, 0);
    fprintf(stderr, "[test] start\n");

    Check(ops::TerminalCommandLine(L"C:\\A\\B") == L"-d \"C:\\A\\B\"",
          L"terminal command quotes a directory");
    Check(ops::TerminalCommandLine(L"C:\\") == L"-d \"C:\\\\\"",
          L"terminal command preserves a drive root");
    Check(ops::TerminalCommandLine(L"C:\\quoted\" folder\\") ==
              L"-d \"C:\\quoted\\\" folder\\\\\"",
          L"terminal command escapes quotes and trailing slashes");

    // --- Setup sandbox ------------------------------------------------------
    const std::wstring root = SandboxRoot();
    if (root.empty()) {
        fprintf(stderr, "[test] cannot locate bench_data/opstest\n");
        return 2;
    }
    std::wstring srcDir = root + L"\\src";
    std::wstring dstDir = root + L"\\dst";
    // Exact, workspace-contained sandbox path.
    std::error_code cleanup_error;
    std::filesystem::remove_all(std::filesystem::path(root), cleanup_error);
    MakeDir(root);
    MakeDir(srcDir);
    MakeDir(dstDir);

    const char payload[] = "pulse ops self-test payload";
    MakeFile(srcDir + L"\\a.txt", payload, sizeof(payload) - 1);
    MakeFile(srcDir + L"\\b.txt", payload, sizeof(payload) - 1);
    MakeFile(srcDir + L"\\d.txt", payload, sizeof(payload) - 1);

    std::atomic<bool> notified{false};
    g_ops.Start([&] {
        notified = true;
        const auto status = g_ops.Status();
        {
            std::lock_guard<std::mutex> lock(g_status_mutex);
            g_status_history.push_back(status);
        }
        if (g_pause_when_active.load() && status.active &&
            status.phase == ops::OpPhase::Scanning) {
            g_pause_when_active.store(false);
            g_ops.PauseCurrent();
        }
    });
    Sleep(800); // let the ops worker bring up ShellClient before direct IPC calls
    fprintf(stderr, "[test] ops started\n");

    // --- 1. Ping ------------------------------------------------------------
    {
        Check(ipc::ShellClient::Instance().Ping(), L"IPC ping Pulse.Shell.exe");
    }

    // --- 2. Copy ------------------------------------------------------------
    {
        auto st = RunOp(SimpleOp(ops::OpType::Copy, { (srcDir + L"\\a.txt").c_str() }, dstDir.c_str()));
        Check(Exists(dstDir + L"\\a.txt") && FileSize(dstDir + L"\\a.txt") == sizeof(payload) - 1,
              L"copy src\\a.txt -> dst");
        if (!st.last_error.empty()) wprintf(L"       error: %s\n", st.last_error.c_str());
        Check(st.last_error.empty(), L"copy reported no error");

        MakeFile(srcDir + L"\\display-path.txt", payload, sizeof(payload) - 1);
        const std::wstring prefixed_dst = L"\\\\?\\" + dstDir;
        auto prefixed_st = RunOp(SimpleOp(ops::OpType::Copy,
            { (srcDir + L"\\display-path.txt").c_str() }, prefixed_dst.c_str()));
        Check(prefixed_st.last_error.empty() &&
              prefixed_st.summary.find(L"\\\\?\\") == std::wstring::npos,
              L"copy summary hides extended-length path prefix");
    }

    {
        MakeFile(srcDir + L"\\same.txt", payload, sizeof(payload) - 1);
        auto st = RunOp(SimpleOp(ops::OpType::Copy,
            { (srcDir + L"\\same.txt").c_str() }, srcDir.c_str()));
        Check(Exists(srcDir + L"\\same.txt") && Exists(srcDir + L"\\same - 副本.txt"),
              L"copy into same folder creates 副本");
        if (!st.last_error.empty()) wprintf(L"       error: %s\n", st.last_error.c_str());
        Check(st.last_error.empty(), L"same-folder copy reported no error");

        const uint64_t before_noop = g_ops.Status().completed_ops;
        const auto noop_started = std::chrono::steady_clock::now();
        auto move_st = RunOp(SimpleOp(ops::OpType::Move,
            { (srcDir + L"\\same.txt").c_str() }, srcDir.c_str()));
        Check(move_st.completed_ops == before_noop + 1 &&
              std::chrono::steady_clock::now() - noop_started < std::chrono::seconds(2),
              L"same-folder move reports completion promptly");
        Check(Exists(srcDir + L"\\same.txt"), L"move into same folder is a no-op");
        Check(move_st.last_error.empty(), L"same-folder move reported no error");
        Check(move_st.summary.empty(), L"same-folder move stays silent in status");
    }

    // --- 3. Move ------------------------------------------------------------
    {
        const char oldPayload[] = "old";
        const char newPayload[] = "new collision payload";
        MakeFile(srcDir + L"\\collision.txt", newPayload, sizeof(newPayload) - 1);
        MakeFile(dstDir + L"\\collision.txt", oldPayload, sizeof(oldPayload) - 1);
        auto replace = SimpleOp(ops::OpType::Copy,
            { (srcDir + L"\\collision.txt").c_str() }, dstDir.c_str());
        replace.collision_policy = ops::CollisionPolicy::Replace;
        replace.is_undo = true;
        RunOp(std::move(replace));
        Check(FileSize(dstDir + L"\\collision.txt") == sizeof(newPayload) - 1,
            L"collision policy: replace target");

        auto keepBoth = SimpleOp(ops::OpType::Copy,
            { (srcDir + L"\\collision.txt").c_str() }, dstDir.c_str());
        keepBoth.collision_policy = ops::CollisionPolicy::KeepBoth;
        keepBoth.is_undo = true;
        RunOp(std::move(keepBoth));
        int collisionCopies = 0;
        WIN32_FIND_DATAW collisionData{};
        HANDLE collisionFind = FindFirstFileW((dstDir + L"\\collision*.txt").c_str(), &collisionData);
        if (collisionFind != INVALID_HANDLE_VALUE) {
            do { ++collisionCopies; } while (FindNextFileW(collisionFind, &collisionData));
            FindClose(collisionFind);
        }
        Check(collisionCopies >= 2, L"collision policy: keep both with automatic rename");
    }

    // --- 4. Move ------------------------------------------------------------
    {
        RunOp(SimpleOp(ops::OpType::Move, { (srcDir + L"\\b.txt").c_str() }, dstDir.c_str()));
        Check(!Exists(srcDir + L"\\b.txt") && Exists(dstDir + L"\\b.txt"), L"move src\\b.txt -> dst");
    }

    // --- 4. Rename ----------------------------------------------------------
    {
        auto rename_st = RunOp(SimpleOp(ops::OpType::Rename,
            { (dstDir + L"\\a.txt").c_str() }, nullptr, L"a2.txt"));
        Check(!Exists(dstDir + L"\\a.txt") && Exists(dstDir + L"\\a2.txt"), L"rename a.txt -> a2.txt");
        Check(rename_st.last_error.empty(), L"rename reported no error");

        MakeFile(srcDir + L"\\prefixed-rename.txt", payload, sizeof(payload) - 1);
        const std::wstring prefixed_rename = L"\\\\?\\" + srcDir + L"\\prefixed-rename.txt";
        auto prefixed_st = RunOp(SimpleOp(ops::OpType::Rename,
            { prefixed_rename.c_str() }, nullptr, L"prefixed-renamed.txt"));
        Check(!Exists(srcDir + L"\\prefixed-rename.txt") &&
              Exists(srcDir + L"\\prefixed-renamed.txt"),
              L"rename accepts \\\\?\\ prefixed path");
        Check(prefixed_st.last_error.empty(), L"prefixed rename reported no error");

        MakeFile(srcDir + L"\\locked-rename.txt", payload, sizeof(payload) - 1);
        HANDLE locked = CreateFileW((srcDir + L"\\locked-rename.txt").c_str(), GENERIC_READ,
                                    FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        auto failed_st = RunOp(SimpleOp(ops::OpType::Rename,
            { (srcDir + L"\\locked-rename.txt").c_str() }, nullptr, L"locked-renamed.txt"));
        if (locked != INVALID_HANDLE_VALUE) CloseHandle(locked);
        Check(Exists(srcDir + L"\\locked-rename.txt") &&
              !Exists(srcDir + L"\\locked-renamed.txt"),
              L"failed rename preserves source");
        Check(failed_st.phase == ops::OpPhase::Failed && !failed_st.last_error.empty(),
              L"failed rename reports failure");
    }

    // --- 5. Recycle delete --------------------------------------------------
    {
        RunOp(SimpleOp(ops::OpType::RecycleDelete, { (dstDir + L"\\b.txt").c_str() }));
        Check(!Exists(dstDir + L"\\b.txt"), L"recycle-delete dst\\b.txt (bin check via PowerShell)");

        MakeFile(srcDir + L"\\20260817_145219.mp4", payload, sizeof(payload) - 1);
        const std::wstring prefixed = L"\\\\?\\" + srcDir + L"\\20260817_145219.mp4";
        auto st = RunOp(SimpleOp(ops::OpType::RecycleDelete, { prefixed.c_str() }));
        Check(!Exists(srcDir + L"\\20260817_145219.mp4"),
              L"recycle-delete accepts screenshot .mp4 \\\\?\\ path");
        if (!st.last_error.empty()) wprintf(L"       error: %s\n", st.last_error.c_str());
        Check(st.last_error.empty(), L"prefixed .mp4 recycle-delete reported no error");

        const std::wstring missing = L"\\\\?\\" + srcDir + L"\\already-missing.mp4";
        auto missing_st = RunOp(SimpleOp(ops::OpType::RecycleDelete, { missing.c_str() }));
        Check(missing_st.phase == ops::OpPhase::Failed && !missing_st.last_error.empty(),
              L"missing recycle-delete remains a failure");
    }

    // --- 6. Real delete -----------------------------------------------------
    {
        MakeFile(srcDir + L"\\c.txt", payload, sizeof(payload) - 1);
        RunOp(SimpleOp(ops::OpType::RealDelete, { (srcDir + L"\\c.txt").c_str() }));
        Check(!Exists(srcDir + L"\\c.txt"), L"realdelete src\\c.txt");

        MakeFile(srcDir + L"\\prefixed-realdelete.txt", payload, sizeof(payload) - 1);
        const std::wstring prefixed = L"\\\\?\\" + srcDir + L"\\prefixed-realdelete.txt";
        auto st = RunOp(SimpleOp(ops::OpType::RealDelete, { prefixed.c_str() }));
        Check(!Exists(srcDir + L"\\prefixed-realdelete.txt"),
              L"realdelete accepts \\\\?\\ prefixed path");
        Check(st.last_error.empty(), L"prefixed realdelete reported no error");

        MakeFile(srcDir + L"\\locked-realdelete.txt", payload, sizeof(payload) - 1);
        HANDLE locked = CreateFileW((srcDir + L"\\locked-realdelete.txt").c_str(), GENERIC_READ,
                                    FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        auto locked_st = RunOp(SimpleOp(ops::OpType::RealDelete,
            { (srcDir + L"\\locked-realdelete.txt").c_str() }));
        if (locked != INVALID_HANDLE_VALUE) CloseHandle(locked);
        Check(Exists(srcDir + L"\\locked-realdelete.txt"),
              L"failed locked realdelete preserves source");
        Check(locked_st.phase == ops::OpPhase::Failed && !locked_st.last_error.empty(),
              L"failed locked realdelete reports failure");

        const std::wstring delete_a = srcDir + L"\\delete-a.tmp";
        const std::wstring delete_b = srcDir + L"\\delete-b.tmp";
        const std::wstring delete_c = srcDir + L"\\delete-c.tmp";
        MakeFile(delete_a, payload, sizeof(payload) - 1);
        MakeFile(delete_b, payload, sizeof(payload) - 1);
        MakeFile(delete_c, payload, sizeof(payload) - 1);
        {
            std::lock_guard<std::mutex> lock(g_status_mutex);
            g_status_history.clear();
        }
        const uint64_t previous = g_ops.Status().completed_ops;
        const uint64_t task_id = g_ops.Submit(SimpleOp(ops::OpType::RealDelete,
            { delete_a.c_str(), delete_b.c_str(), delete_c.c_str() }));
        Check(WaitOpDone(previous) && !Exists(delete_a) && !Exists(delete_b) && !Exists(delete_c),
              L"multi-item realdelete completes");

        uint64_t last_items = 0;
        bool monotonic_items = true;
        bool saw_shell_total = false;
        {
            std::lock_guard<std::mutex> lock(g_status_mutex);
            for (const auto& status : g_status_history) {
                if (status.task_id != task_id || !status.active) continue;
                monotonic_items = monotonic_items && status.completed_items >= last_items;
                last_items = status.completed_items;
                saw_shell_total = saw_shell_total ||
                    (status.total_items == 3 && status.completed_items == 3);
            }
        }
        Check(monotonic_items && saw_shell_total,
              L"Shell delete reports monotonic completed item counts");
    }

    // --- 7. Progress + cancel (bulk dir copy, cancel on first progress) -----
    {
        std::wstring bulk = srcDir + L"\\bulk";
        MakeDir(bulk);
        for (int i = 0; i < 400; ++i) {
            wchar_t name[64];
            swprintf_s(name, L"f%04d.bin", i);
            char buf[512] = {};
            MakeFile(bulk + L"\\" + name, buf, sizeof(buf));
        }

        uint64_t prev = g_ops.Status().completed_ops;
        g_ops.Submit(SimpleOp(ops::OpType::Copy, { bulk.c_str() }, dstDir.c_str()));

        // Wait for the op to become active, then cancel.
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (std::chrono::steady_clock::now() < deadline) {
            if (g_ops.Status().active) break;
            Sleep(5);
        }
        g_ops.CancelCurrent();
        bool finished = WaitOpDone(prev);
        auto st = g_ops.Status();
        wprintf(L"[INFO] cancel test: finished=%d last_error=%s bulk_at_dst=%d\n",
                finished ? 1 : 0, st.last_error.c_str(), Exists(dstDir + L"\\bulk") ? 1 : 0);
        Check(finished, L"cancel: op finished after cancel");
        Check(st.phase == ops::OpPhase::Cancelled || !Exists(dstDir + L"\\bulk"),
              L"cancel: op cancelled or partial copy cleaned");
    }

    // --- 8. Undo: move / rename / copy --------------------------------------
    {
        // move dst\a2.txt -> src, then undo
        RunOp(SimpleOp(ops::OpType::Move, { (dstDir + L"\\a2.txt").c_str() }, srcDir.c_str()));
        Check(Exists(srcDir + L"\\a2.txt"), L"undo-move setup: a2.txt moved to src");
        Check(g_ops.CanUndo(), L"undo available after move");
        uint64_t prev = g_ops.Status().completed_ops;
        g_ops.Undo();
        WaitOpDone(prev);
        Check(Exists(dstDir + L"\\a2.txt") && !Exists(srcDir + L"\\a2.txt"),
              L"undo move: a2.txt back in dst");

        // rename src\d.txt -> d2.txt, then undo
        RunOp(SimpleOp(ops::OpType::Rename, { (srcDir + L"\\d.txt").c_str() }, nullptr, L"d2.txt"));
        Check(Exists(srcDir + L"\\d2.txt"), L"undo-rename setup: d.txt renamed");
        prev = g_ops.Status().completed_ops;
        g_ops.Undo();
        WaitOpDone(prev);
        Check(Exists(srcDir + L"\\d.txt") && !Exists(srcDir + L"\\d2.txt"),
              L"undo rename: d2.txt back to d.txt");

        MakeFile(srcDir + L"\\batch-a.txt", payload, sizeof(payload) - 1);
        MakeFile(srcDir + L"\\batch-b.txt", payload, sizeof(payload) - 1);
        ops::OpRequest batch;
        batch.type = ops::OpType::BatchRename;
        batch.sources = { srcDir + L"\\batch-a.txt", srcDir + L"\\batch-b.txt" };
        batch.new_names = { L"batch-a2.txt", L"batch-b2.txt" };
        RunOp(std::move(batch));
        Check(Exists(srcDir + L"\\batch-a2.txt") && Exists(srcDir + L"\\batch-b2.txt") &&
              !Exists(srcDir + L"\\batch-a.txt") && !Exists(srcDir + L"\\batch-b.txt"),
              L"batch-rename: two files renamed");
        prev = g_ops.Status().completed_ops;
        g_ops.Undo();
        const auto undo_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (std::chrono::steady_clock::now() < undo_deadline) {
            if (g_ops.Status().completed_ops >= prev + 1 &&
                Exists(srcDir + L"\\batch-a.txt") && Exists(srcDir + L"\\batch-b.txt") &&
                !Exists(srcDir + L"\\batch-a2.txt") && !Exists(srcDir + L"\\batch-b2.txt"))
                break;
            Sleep(10);
        }
        Check(Exists(srcDir + L"\\batch-a.txt") && Exists(srcDir + L"\\batch-b.txt") &&
              !Exists(srcDir + L"\\batch-a2.txt") && !Exists(srcDir + L"\\batch-b2.txt"),
              L"batch-rename: undo restores original names");
        Check(g_ops.Status().completed_ops == prev + 1,
              L"batch-rename: one batch inverse finishes before the next request");

        // copy src\d.txt -> dst, then undo (deletes the copy, to recycle bin)
        RunOp(SimpleOp(ops::OpType::Copy, { (srcDir + L"\\d.txt").c_str() }, dstDir.c_str()));
        Check(Exists(dstDir + L"\\d.txt"), L"undo-copy setup: d.txt copied");
        prev = g_ops.Status().completed_ops;
        g_ops.Undo();
        WaitOpDone(prev);
        Check(!Exists(dstDir + L"\\d.txt") && Exists(srcDir + L"\\d.txt"),
              L"undo copy: dst copy removed, source intact");
    }

    // --- 10. Undo stack persistence round-trip ------------------------------
    {
        RunOp(SimpleOp(ops::OpType::Copy, { (srcDir + L"\\d.txt").c_str() }, dstDir.c_str()));
        std::wstring json = g_ops.UndoToJson();
        ops::OpsManager other;
        Check(other.UndoFromJson(json), L"undo stack JSON round-trip parses");
        Check(other.CanUndo(), L"restored undo stack is usable");
        const std::wstring legacy = L"[{\"type\":0,\"sup\":true,\"dest\":\"C:\\\\tmp\","
            L"\"name\":\"\",\"src\":[\"C:\\\\tmp\\\\legacy.txt\"]}]";
        ops::OpsManager legacy_manager;
        Check(legacy_manager.UndoFromJson(legacy) && !legacy_manager.CanUndo(),
              L"legacy Copy history parses without authorizing products with unknown identity");
    }

    // --- 11. Clipboard round-trip -------------------------------------------
    {
        bool copy_round_trips = true;
        bool cut_round_trips = true;
        for (int i = 0; i < 20; ++i) {
            ops::ClipboardData copy;
            const bool copy_write = ops::WriteClipboard({ srcDir + L"\\d.txt" }, false);
            const bool copy_read = ops::ReadClipboard(copy);
            const bool copy_match = copy.paths.size() == 1 &&
                copy.paths[0] == srcDir + L"\\d.txt" && !copy.cut;
            copy_round_trips = copy_round_trips && copy_write && copy_read && copy_match;
            ops::ClipboardData cut;
            const bool cut_write = ops::WriteClipboard({ srcDir + L"\\d.txt" }, true);
            const bool cut_read = ops::ReadClipboard(cut);
            const bool cut_match = cut.paths.size() == 1 &&
                cut.paths[0] == srcDir + L"\\d.txt" && cut.cut;
            cut_round_trips = cut_round_trips && cut_write && cut_read && cut_match;
        }
        Check(copy_round_trips, L"clipboard CF_HDROP + drop-effect round-trip (20x)");
        Check(cut_round_trips, L"clipboard cut effect round-trip (20x)");
    }

    TestTransferControl(srcDir, dstDir);

    // --- 13. Nested conflicts and apply-all ---------------------------------
    {
        const std::wstring multi_target = root + L"\\multi-dst";
        MakeDir(multi_target);
        constexpr uint64_t file_bytes = 8ull * 1024ull * 1024ull;
        const std::wstring source_a = srcDir + L"\\multi-a.bin";
        const std::wstring source_b = srcDir + L"\\multi-b.bin";
        const std::wstring source_c = srcDir + L"\\multi-c.bin";
        Check(MakePatternFile(source_a, file_bytes) &&
              MakePatternFile(source_b, file_bytes) &&
              MakePatternFile(source_c, file_bytes),
              L"create multi-file transfer fixtures");

        {
            std::lock_guard<std::mutex> lock(g_status_mutex);
            g_status_history.clear();
        }
        const uint64_t previous = g_ops.Status().completed_ops;
        const uint64_t task_id = g_ops.Submit(SimpleOp(ops::OpType::Copy,
            { source_a.c_str(), source_b.c_str(), source_c.c_str() }, multi_target.c_str()));
        Check(WaitOpDone(previous), L"multi-file copy completes");

        std::vector<ops::OpStatus> samples;
        {
            std::lock_guard<std::mutex> lock(g_status_mutex);
            for (const auto& status : g_status_history) {
                if (status.task_id == task_id && status.total_bytes > 0)
                    samples.push_back(status);
            }
        }
        bool bytes_monotonic = true;
        bool items_monotonic = true;
        bool bounded = true;
        bool saw_file_boundary = false;
        uint64_t previous_bytes = 0;
        uint64_t previous_items = 0;
        for (const auto& status : samples) {
            bytes_monotonic = bytes_monotonic && status.transferred_bytes >= previous_bytes;
            items_monotonic = items_monotonic && status.completed_items >= previous_items;
            bounded = bounded && status.transferred_bytes <= status.total_bytes;
            saw_file_boundary = saw_file_boundary ||
                (status.completed_items > 0 && status.completed_items < status.total_items);
            previous_bytes = status.transferred_bytes;
            previous_items = status.completed_items;
        }
        const auto completed = g_ops.Status();
        Check(!samples.empty() && bytes_monotonic && bounded,
              L"multi-file byte progress is monotonic and bounded");
        Check(items_monotonic && saw_file_boundary,
              L"multi-file item progress is monotonic across file boundaries");
        Check(completed.total_bytes == file_bytes * 3 &&
              completed.transferred_bytes == completed.total_bytes &&
              completed.total_items == 3 && completed.completed_items == 3,
              L"multi-file copy publishes exact final totals");
    }

    // --- 14. Nested conflicts and apply-all ---------------------------------
    {
        const std::wstring nested_source = srcDir + L"\\nested";
        const std::wstring nested_sub = nested_source + L"\\sub";
        const std::wstring nested_target = dstDir + L"\\nested";
        const std::wstring target_sub = nested_target + L"\\sub";
        MakeDir(nested_source);
        MakeDir(nested_sub);
        MakeDir(nested_target);
        MakeDir(target_sub);
        const char source_a[] = "source-a";
        const char source_b[] = "source-b";
        const char target_old[] = "target-old";
        MakeFile(nested_source + L"\\a.txt", source_a, sizeof(source_a) - 1);
        MakeFile(nested_sub + L"\\b.txt", source_b, sizeof(source_b) - 1);
        MakeFile(nested_target + L"\\a.txt", target_old, sizeof(target_old) - 1);
        MakeFile(target_sub + L"\\b.txt", target_old, sizeof(target_old) - 1);

        size_t resolved = 0;
        auto replace_status = RunConflictOp(
            SimpleOp(ops::OpType::Copy, { nested_source.c_str() }, dstDir.c_str()),
            ops::ConflictChoice::Replace, true, &resolved);
        Check(replace_status.last_error.empty() && resolved == 1,
              L"nested conflict replace apply-all resolves once");
        Check(FileHash(nested_source + L"\\a.txt") == FileHash(nested_target + L"\\a.txt") &&
              FileHash(nested_sub + L"\\b.txt") == FileHash(target_sub + L"\\b.txt"),
              L"nested conflict replace commits all source versions");

        MakeFile(nested_target + L"\\a.txt", target_old, sizeof(target_old) - 1);
        MakeFile(target_sub + L"\\b.txt", target_old, sizeof(target_old) - 1);
        resolved = 0;
        auto skip_status = RunConflictOp(
            SimpleOp(ops::OpType::Copy, { nested_source.c_str() }, dstDir.c_str()),
            ops::ConflictChoice::Skip, true, &resolved);
        Check(skip_status.last_error.empty() && resolved == 1 &&
              FileSize(nested_target + L"\\a.txt") == sizeof(target_old) - 1,
              L"nested conflict skip apply-all preserves existing files");

        resolved = 0;
        auto keep_status = RunConflictOp(
            SimpleOp(ops::OpType::Copy, { nested_source.c_str() }, dstDir.c_str()),
            ops::ConflictChoice::KeepBoth, true, &resolved);
        Check(keep_status.last_error.empty() && resolved == 1 &&
              Exists(nested_target + L"\\a - 副本.txt") &&
              Exists(target_sub + L"\\b - 副本.txt"),
              L"nested conflict keep-both apply-all creates incremented copies");
    }

    g_ops.Stop();

    wprintf(L"\n== ops self test: %d passed, %d failed ==\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
