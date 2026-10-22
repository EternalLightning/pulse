#include "../ops/ops_manager.h"
#include "../ipc/shell_client.h"
#include "../common/localization.h"
#include <filesystem>
#include <fstream>
#include <cstdio>
#include <atomic>
namespace pulse::ops {
struct NetworkUndoTestProbe {
    static void RetainLatest(OpsManager& manager, const std::wstring& path) {
        std::lock_guard lock(manager.mutex_);
        auto& history = manager.undo_.back();
        history.session_only = true;
        history.produced_references = {FileObjectReference::Open(path)};
    }
    static void SimulateReusedProviderIndex(OpsManager& manager, FileObjectReference original,
        const FileIdentity& reused) {
        std::lock_guard lock(manager.mutex_);
        auto& history = manager.undo_.back();
        history.session_only = true;
        history.produced_references = {std::move(original)};
        history.produced_identities = {reused};
    }
};
}
using namespace pulse::ops;
namespace {
int failures = 0;
void Check(bool ok, const char* label) { std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label); failures += !ok; }
bool Wait(OpsManager& ops, uint64_t before) {
    const auto deadline = GetTickCount64() + 15000;
    while (GetTickCount64() < deadline) {
        if (ops.Status().completed_ops > before && !ops.Status().active && !ops.HasPendingFileOperations()) return true;
        Sleep(5);
    }
    return false;
}
bool Exact(const std::filesystem::path& path) {
    WIN32_FIND_DATAW data{};
    HANDLE find = FindFirstFileW(path.c_str(), &data);
    if (find == INVALID_HANDLE_VALUE) return false;
    FindClose(find); return path.filename().wstring() == data.cFileName;
}
int RunRenameDelete(const std::filesystem::path& parent = L"bench_data") {
    const auto root = std::filesystem::absolute(parent) /
        (L"rename-delete-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    std::filesystem::create_directories(root);
    const auto source = root / L"1.txt", product = root / L"1.txxt";
    const HWND owner = CreateWindowExW(0, L"STATIC", L"Pulse delete fixture", 0,
        0, 0, 0, 0, HWND_MESSAGE, nullptr, nullptr, nullptr);
    OpsManager ops; ops.SetUiWindow(owner); ops.Start([] {});
    OpRequest create; create.type = OpType::CreateTextFile; create.sources = {source.wstring()};
    ops.Submit(create);
    Check(Wait(ops, 0) && Exact(source) && ops.CanUndo(), "create file before rename/delete");
    // Model the in-session references retained on network drives, on isolated local files.
    if (ops.CanUndo()) NetworkUndoTestProbe::RetainLatest(ops, source.wstring());
    auto before = ops.Status().completed_ops;
    OpRequest rename; rename.type = OpType::Rename; rename.sources = {source.wstring()}; rename.new_name = L"1.txxt";
    ops.Submit(rename);
    Check(Wait(ops, before) && Exact(product) && ops.Status().phase == OpPhase::Completed,
        "rename the created file while its earlier Undo retains the object");
    if (ops.CanUndo()) NetworkUndoTestProbe::RetainLatest(ops, product.wstring());
    auto remove = [&](bool accept) {
        const auto completed = ops.Status().completed_ops;
        OpRequest request; request.type = OpType::RealDelete; request.sources = {product.wstring()};
        ops.Submit(request);
        const auto deadline = GetTickCount64() + 15000;
        bool confirmed = false;
        while (GetTickCount64() < deadline) {
            if (const auto pending = ops.PendingDeleteConfirmation()) {
                ops.ResolveDeleteConfirmation(pending->token, accept); confirmed = true;
            }
            if (ops.Status().completed_ops > completed && !ops.HasPendingFileOperations()) break;
            Sleep(5);
        }
        return confirmed && Wait(ops, completed);
    };
    Check(remove(false) && Exact(product) && ops.CanUndo(), "cancelled permanent deletion preserves file and Undo");
    Check(remove(true) && ops.Status().phase == OpPhase::Completed, "confirmed permanent deletion completes");
    Check(!Exact(product), "deleted renamed entry disappears from directory enumeration before Stop or clearing Undo");
    Check(!ops.CanUndo(), "deletion invalidates earlier Undo entries retaining the same renamed object");
    ops.Stop(); ops.UndoFromJson(L"[]"); DestroyWindow(owner);
    DeleteFileW(source.c_str()); DeleteFileW(product.c_str());
    Check(RemoveDirectoryW(root.c_str()) != FALSE, "rename/delete fixture cleaned");
    return failures ? 1 : 0;
}
int RunRetainedIdentityRejection() {
    const auto root = std::filesystem::absolute(std::filesystem::path(L"bench_data") /
        (L"retained-undo-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64())));
    std::filesystem::create_directories(root);
    const auto source = root / L"source.txt", product = root / L"product.txt", displaced = root / L"original.txt";
    { std::ofstream fixture(source); fixture << "original object"; }
    OpsManager ops; ops.Start([] {});
    OpRequest request; request.type = OpType::Rename; request.sources = {source.wstring()}; request.new_name = L"product.txt";
    ops.Submit(request);
    Check(Wait(ops, 0) && ops.CanUndo(), "record the actual object before simulating a reused provider index");
    if (ops.CanUndo()) {
        auto retained = FileObjectReference::Open(product.wstring());
        const bool moved = MoveFileW(product.c_str(), displaced.c_str()) != FALSE;
        if (moved) { std::ofstream replacement(product); replacement << "replacement object"; }
        pulse::FileIdentity replacement_id;
        pulse::ReadPathFileIdentity(product.wstring(), replacement_id);
        NetworkUndoTestProbe::SimulateReusedProviderIndex(ops, std::move(retained), replacement_id);
        const auto before = ops.Status().completed_ops; ops.Undo();
        Check(moved && Wait(ops, before) && ops.Status().phase == OpPhase::Failed &&
            ops.Status().failure_hr == HRESULT_FROM_WIN32(ERROR_FILE_INVALID) &&
            Exact(product) && Exact(displaced) && !Exact(source),
            "a matching stale provider index never bypasses a mismatched retained object");
    }
    ops.Stop(); ops.UndoFromJson(L"[]");
    DeleteFileW(source.c_str()); DeleteFileW(product.c_str()); DeleteFileW(displaced.c_str());
    Check(RemoveDirectoryW(root.c_str()) != FALSE, "retained object replacement fixture cleaned");
    return failures ? 1 : 0;
}
int RunNetworkUndo(const std::filesystem::path& parent) {
    const auto root = std::filesystem::absolute(parent / L".") / (L".pulse-undo-fixture-" + std::to_wstring(GetCurrentProcessId()) +
        L"-" + std::to_wstring(GetTickCount64()));
    std::printf("[INFO] network fixture process=%lu tick=%llu\n", GetCurrentProcessId(), GetTickCount64());
    if (!CreateDirectoryW(root.c_str(), nullptr)) return 2;
    const HWND owner = CreateWindowExW(0, L"STATIC", L"Pulse network undo fixture", 0,
        0, 0, 0, 0, HWND_MESSAGE, nullptr, nullptr, nullptr);
    for (const auto type : {OpType::Rename, OpType::BatchRename, OpType::Move, OpType::Copy}) {
        const auto dir = root / std::to_wstring(static_cast<int>(type));
        const auto destination = dir / L"destination";
        CreateDirectoryW(dir.c_str(), nullptr);
        CreateDirectoryW(destination.c_str(), nullptr);
        const auto source = dir / L"source.txt";
        { std::ofstream file(source); file << "isolated network undo fixture"; }
        OpsManager ops;
        ops.SetUiWindow(owner);
        ops.Start([] {});
        OpRequest request;
        request.type = type; request.sources = {source.wstring()};
        request.new_name = L"renamed.txt"; request.new_names = {request.new_name};
        request.dest_dir = destination.wstring();
        ops.Submit(request);
        const bool completed = Wait(ops, 0);
        const auto status = ops.Status();
        std::printf("[INFO] network operation=%d phase=%d hr=%08lx\n", static_cast<int>(type),
            static_cast<int>(status.phase), status.failure_hr);
        if (!status.last_error.empty()) std::fwprintf(stderr, L"%ls\n", status.last_error.c_str());
        Check(completed && status.phase == OpPhase::Completed && ops.CanUndo(), "network operation records usable Undo");
        OpsManager loaded;
        Check(loaded.UndoFromJson(ops.UndoToJson()) && !loaded.CanUndo(),
            "network Undo never trusts a transient ID after restart");
        if (completed && status.phase == OpPhase::Completed && ops.CanUndo()) {
            const auto product = type == OpType::Rename || type == OpType::BatchRename
                ? dir / L"renamed.txt" : destination / L"source.txt";
            if (type == OpType::Rename || type == OpType::BatchRename) {
                const HANDLE editor = CreateFileW((dir / L"renamed.txt").c_str(), GENERIC_READ | GENERIC_WRITE,
                    FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
                Check(editor != INVALID_HANDLE_VALUE, "retained Undo reference permits an editor that omits delete sharing");
                if (editor != INVALID_HANDLE_VALUE) CloseHandle(editor);
            }
            if (type == OpType::Copy) {
                const auto cancel_before = ops.Status().completed_ops;
                ops.Undo();
                const auto cancel_deadline = GetTickCount64() + 30000;
                bool cancelled_confirmation = false;
                while ((ops.Status().completed_ops == cancel_before || ops.HasPendingFileOperations()) &&
                    GetTickCount64() < cancel_deadline) {
                    if (const auto pending = ops.PendingDeleteConfirmation()) {
                        Check(pending->plan.origin == DeleteOrigin::Undo && pending->plan.targets.size() == 1 &&
                            pending->plan.targets.front().disposition == DeleteDisposition::Permanent &&
                            pending->plan.targets.front().physical_paths == std::vector<std::wstring>{product.wstring()},
                            "network copy Undo confirms permanent deletion of the exact copied result");
                        ops.ResolveDeleteConfirmation(pending->token, false);
                        cancelled_confirmation = true;
                    }
                    Sleep(5);
                }
                Check(cancelled_confirmation && ops.Status().phase == OpPhase::Cancelled &&
                    Exact(source) && Exact(product) && ops.CanUndo(),
                    "cancelling network copy Undo preserves source, copy and retryable history");
            }
            const auto before = ops.Status().completed_ops;
            ops.Undo();
            const auto deadline = GetTickCount64() + 30000;
            while ((ops.Status().completed_ops == before || ops.HasPendingFileOperations()) && GetTickCount64() < deadline) {
                if (const auto pending = ops.PendingDeleteConfirmation())
                    ops.ResolveDeleteConfirmation(pending->token, true);
                Sleep(5);
            }
            const auto undone = ops.Status();
            std::printf("[INFO] network undo=%d phase=%d hr=%08lx elevation=%d\n", static_cast<int>(type),
                static_cast<int>(undone.phase), undone.failure_hr, undone.elevation_retry_available);
            if (!undone.last_error.empty()) std::fwprintf(stderr, L"%ls\n", undone.last_error.c_str());
            if (type == OpType::Copy) {
                Check(undone.phase == OpPhase::Completed && Exact(source) && !Exact(product) &&
                    !ops.CanUndo() && !undone.elevation_retry_available,
                    "confirmed network copy Undo deletes only the copy and consumes history");
            } else Check(undone.phase == OpPhase::Completed && Exact(source) && !Exact(product),
                "network Undo restores source and removes result name");
            if (undone.phase == OpPhase::Completed) {
                const auto redo_before = ops.Status().completed_ops;
                ops.Submit(request);
                Check(Wait(ops, redo_before) && ops.Status().phase == OpPhase::Completed, "repeat the isolated mutation before replacement test");
                const auto replacement_before = ops.Status().completed_ops;
                std::optional<DeleteConfirmation> confirmation;
                if (type == OpType::Copy) {
                    ops.Undo();
                    const auto confirmation_deadline = GetTickCount64() + 30000;
                    while (!(confirmation = ops.PendingDeleteConfirmation()) &&
                        ops.Status().completed_ops == replacement_before && GetTickCount64() < confirmation_deadline) Sleep(5);
                    Check(confirmation.has_value(), "replacement test waits for the network copy Undo confirmation");
                }
                const auto displaced = dir / L"displaced.txt";
                const bool replaced = MoveFileW(product.c_str(), displaced.c_str()) != FALSE;
                const DWORD replacement_error = replaced ? ERROR_SUCCESS : GetLastError();
                if (replaced) { std::ofstream replacement(product); replacement << "same-name replacement must survive"; }
                if (type == OpType::Copy) {
                    if (confirmation) ops.ResolveDeleteConfirmation(confirmation->token, true);
                } else ops.Undo();
                const bool refused = Wait(ops, replacement_before);
                std::printf("[INFO] replacement check moved=%d error=%lu completed=%d phase=%d hr=%08lx product=%d displaced=%d source=%d\n",
                    replaced, replacement_error, refused, static_cast<int>(ops.Status().phase), ops.Status().failure_hr,
                    Exact(product), Exact(displaced), Exact(source));
                Check(replaced && refused && ops.Status().phase == OpPhase::Failed &&
                    Exact(product) && Exact(displaced) && Exact(source) == (type == OpType::Copy),
                    "network Undo refuses a same-name replacement");
            }
        }
        ops.Stop();
        ops.UndoFromJson(L"[]"); // release in-session references before fixture cleanup
        // Only names created by this fixture are cleaned; never recurse through the drive.
        DeleteFileW(source.c_str());
        DeleteFileW((dir / L"renamed.txt").c_str());
        DeleteFileW((destination / L"source.txt").c_str());
        DeleteFileW((dir / L"displaced.txt").c_str());
        Check(RemoveDirectoryW(destination.c_str()) && RemoveDirectoryW(dir.c_str()), "network operation fixture cleaned");
    }
    {
        const auto source = root / L"folder-source", destination = root / L"folder-destination";
        const auto product = destination / source.filename();
        CreateDirectoryW(source.c_str(), nullptr); CreateDirectoryW(destination.c_str(), nullptr);
        { std::ofstream child(source / L"child.txt"); child << "network directory copy fixture"; }
        OpsManager ops; ops.SetUiWindow(owner); ops.Start([] {});
        OpRequest copy; copy.type = OpType::Copy; copy.sources = {source.wstring()}; copy.dest_dir = destination.wstring();
        ops.Submit(copy);
        Check(Wait(ops, 0) && ops.Status().phase == OpPhase::Completed && ops.CanUndo() && Exact(product / L"child.txt"),
            "network directory copy records an Undo for its nonempty result");
        const auto copied = ops.Status();
        wprintf(L"[INFO] network directory copy phase=%d hr=%08lx undo=%d error=%s\n",
            static_cast<int>(copied.phase), static_cast<unsigned long>(copied.failure_hr),
            ops.CanUndo(), copied.last_error.c_str());
        const auto before = ops.Status().completed_ops; ops.Undo();
        const auto deadline = GetTickCount64() + 30000;
        bool confirmed = false;
        while ((ops.Status().completed_ops == before || ops.HasPendingFileOperations()) && GetTickCount64() < deadline) {
            if (const auto pending = ops.PendingDeleteConfirmation()) {
                ops.ResolveDeleteConfirmation(pending->token, true);
                confirmed = true;
            }
            Sleep(5);
        }
        Check(confirmed && ops.Status().phase == OpPhase::Completed && Exact(source / L"child.txt") &&
            !Exact(product) && !ops.CanUndo() && !ops.Status().elevation_retry_available,
            "confirmed directory copy Undo removes the entire copied result and preserves the source");
        ops.Stop(); ops.UndoFromJson(L"[]");
        DeleteFileW((product / L"child.txt").c_str()); RemoveDirectoryW(product.c_str());
        DeleteFileW((source / L"child.txt").c_str());
        Check(RemoveDirectoryW(source.c_str()) && RemoveDirectoryW(destination.c_str()), "network directory copy fixture cleaned");
    }
    {
        const auto source = root / L"chain-a.txt", middle = root / L"chain-b.txt", final = root / L"chain-c.txt";
        { std::ofstream fixture(source); fixture << "consecutive network renames"; }
        OpsManager ops;
        ops.Start([] {});
        OpRequest rename; rename.type = OpType::Rename; rename.sources = {source.wstring()}; rename.new_name = middle.filename().wstring();
        ops.Submit(rename);
        Check(Wait(ops, 0) && ops.Status().phase == OpPhase::Completed, "first network rename in chain completes");
        auto before = ops.Status().completed_ops;
        rename.sources = {middle.wstring()}; rename.new_name = final.filename().wstring();
        ops.Submit(rename);
        Check(Wait(ops, before) && ops.Status().phase == OpPhase::Completed, "second network rename in chain completes");
        before = ops.Status().completed_ops; ops.Undo();
        Check(Wait(ops, before) && ops.Status().phase == OpPhase::Completed && Exact(middle), "Undo restores the intermediate network name");
        before = ops.Status().completed_ops; ops.Undo();
        const bool chain_completed = Wait(ops, before);
        std::printf("[INFO] earlier chain Undo phase=%d hr=%08lx source=%d middle=%d undo=%d\n",
            static_cast<int>(ops.Status().phase), ops.Status().failure_hr, Exact(source), Exact(middle), ops.CanUndo());
        if (!ops.Status().last_error.empty()) std::fwprintf(stderr, L"%ls\n", ops.Status().last_error.c_str());
        Check(chain_completed && ops.Status().phase == OpPhase::Completed && Exact(source) && !ops.CanUndo(),
            "earlier Undo follows its retained object after later renames changed the index");
        ops.Stop(); ops.UndoFromJson(L"[]");
        DeleteFileW(source.c_str()); DeleteFileW(middle.c_str()); DeleteFileW(final.c_str());
    }
    DestroyWindow(owner);
    Check(RemoveDirectoryW(root.c_str()) != FALSE, "network Undo fixture root cleaned");
    return failures ? 1 : 0;
}
}
int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc == 2 && std::string_view(argv[1]) == "--rename-delete") return RunRenameDelete();
    if (argc == 3 && std::string_view(argv[1]) == "--rename-delete-root") return RunRenameDelete(argv[2]);
    if (argc == 2 && std::string_view(argv[1]) == "--retained-identity") return RunRetainedIdentityRejection();
    if (argc == 3 && std::string_view(argv[1]) == "--network-root")
        return RunNetworkUndo(std::filesystem::path(argv[2]));
    const auto root = std::filesystem::absolute(std::filesystem::path(L"bench_data") /
        (L"rename-ops-" + std::to_wstring(GetCurrentProcessId())));
    std::filesystem::create_directories(root / L"child");
    auto make = [&](const wchar_t* name) { std::ofstream(root / name) << "fixture"; };
    if (argc > 1 && std::string_view(argv[1]) == "--shell-roundtrip") {
        OpsManager roundtrip;
        roundtrip.Start([] {});
        OpRequest create; create.type = OpType::CreateTextFile;
        create.sources = {(root / L"shell-created.txt").wstring()};
        roundtrip.Submit(create);
        const bool received = Wait(roundtrip, 0);
        const auto create_status = roundtrip.Status();
        std::printf("[INFO] create received=%d phase=%d exists=%d error=", received,
            static_cast<int>(create_status.phase), Exact(root / L"shell-created.txt"));
        for (const auto c : create_status.last_error) std::printf("%04x ", static_cast<unsigned>(c));
        std::printf("\n");
        Check(received && create_status.phase == OpPhase::Completed &&
            Exact(root / L"shell-created.txt"), "normal shell CreateNewFile receives Done");
        Check(pulse::ipc::ShellClient::Instance().Ping(), "normal shell ping submitted on proven connection");
        const auto after_ping = roundtrip.Status().completed_ops;
        create.sources = {(root / L"shell-after-ping.txt").wstring()};
        roundtrip.Submit(create);
        Check(Wait(roundtrip, after_ping) && roundtrip.Status().phase == OpPhase::Completed &&
            Exact(root / L"shell-after-ping.txt"), "shell response stream continues after Ping");
        roundtrip.Stop();
        std::filesystem::remove_all(root);
        return failures ? 1 : 0;
    }
    if (argc > 1 && std::string_view(argv[1]) == "--startup-stop") {
        for (int i = 0; i < 20; ++i) {
            make(L"startup.txt");
            OpsManager immediate;
            HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            immediate.Start([&] { if (immediate.Status().completed_ops) SetEvent(done); });
            OpRequest request; request.type = OpType::Rename;
            request.sources = {(root / L"startup.txt").wstring()}; request.new_name = L"startup-renamed.txt";
            immediate.Submit(request);
            Check(WaitForSingleObject(done, 5000) == WAIT_OBJECT_0 && Exact(root / L"startup-renamed.txt"),
                "immediate startup rename reaches disk");
            const auto started = GetTickCount64();
            std::printf("[INFO] startup iteration=%d pid=%lu stopping\n", i, GetCurrentProcessId());
            immediate.Stop();
            std::printf("[INFO] startup stop elapsed=%llu ms\n", GetTickCount64() - started);
            Check(GetTickCount64() - started < 3000, "immediate startup stop finishes promptly");
            CloseHandle(done);
            DeleteFileW((root / L"startup-renamed.txt").c_str());
        }
        std::filesystem::remove_all(root);
        return failures ? 1 : 0;
    }
    OpsManager ops;
    std::atomic<bool> cancel_batch{false};
    ops.Start([&] {
        const auto status = ops.Status();
        if (status.type == OpType::BatchRename && status.active && status.completed_items == 1 && cancel_batch.exchange(false))
            ops.CancelCurrent();
    });
    auto rename = [&](const wchar_t* from, const wchar_t* to) {
        const auto before = ops.Status().completed_ops;
        OpRequest request; request.type = OpType::Rename; request.sources = {(root / from).wstring()}; request.new_name = to;
        ops.Submit(std::move(request)); Check(Wait(ops, before), "rename completes within deadline");
    };
    auto undo = [&] { const auto before = ops.Status().completed_ops; ops.Undo(); Check(Wait(ops, before), "undo completes within deadline"); };
    make(L"Report.txt"); rename(L"Report.txt", L"report.txt");
    Check(Exact(root / L"report.txt"), "case-only filename changes on disk");
    undo(); Check(Exact(root / L"Report.txt"), "case-only undo restores exact spelling");
    make(L"ordinary.txt"); rename(L"ordinary.txt", L"changed.txt");
    Check(Exact(root / L"changed.txt") && !std::filesystem::exists(root / L"ordinary.txt"), "ordinary file rename");
    undo(); Check(Exact(root / L"ordinary.txt"), "ordinary file undo");
    std::filesystem::create_directory(root / L"Folder"); rename(L"Folder", L"folder");
    Check(Exact(root / L"folder"), "case-only directory rename");
    undo(); Check(Exact(root / L"Folder"), "directory undo");
    rename(L"Folder", L"RenamedFolder");
    Check(Exact(root / L"RenamedFolder") && !std::filesystem::exists(root / L"Folder"), "ordinary directory rename");
    undo(); Check(Exact(root / L"Folder"), "ordinary directory undo");
    make(L"stay.txt"); rename(L"stay.txt", L"child\\moved.txt");
    Check(std::filesystem::exists(root / L"stay.txt") && !std::filesystem::exists(root / L"child" / L"moved.txt") &&
        ops.Status().phase == OpPhase::Failed, "path component rejected without moving file");
    for (const auto name : {L"child/name.txt", L"..\\escape.txt", L"C:\\escape.txt", L"bad:name", L"bad?name",
        L"CON.txt", L"LPT1", L"trailing.", L"trailing ", L".", L"..", L""}) {
        rename(L"stay.txt", name);
        Check(Exact(root / L"stay.txt") && ops.Status().phase == OpPhase::Failed, "invalid filename rejected");
    }
    make(L"first.txt"); make(L"second.txt"); make(L"third.txt");
    OpRequest batch; batch.type = OpType::BatchRename;
    batch.sources = {(root / L"first.txt").wstring(), (root / L"second.txt").wstring(), (root / L"third.txt").wstring()};
    batch.new_names = {L"first-new.txt", L"second-new.txt", L"third-new.txt"};
    cancel_batch = true;
    const auto before = ops.Status().completed_ops; ops.Submit(batch); Check(Wait(ops, before), "batch terminates");
    Check(Exact(root / L"first-new.txt") && Exact(root / L"second.txt") && Exact(root / L"third.txt") &&
        ops.Status().phase == OpPhase::Cancelled && ops.Status().completed_items == 1, "cancel after first item leaves remaining files unchanged with Cancelled terminal state");
    undo(); Check(Exact(root / L"first.txt"), "cancelled batch retains completed item undo");
    Check(Exact(root / L"second.txt") && Exact(root / L"third.txt"), "partial undo leaves unprocessed items unchanged");
    OpRequest invalid_count; invalid_count.type = OpType::Rename;
    invalid_count.sources = {(root / L"second.txt").wstring(), (root / L"third.txt").wstring()};
    invalid_count.new_name = L"multi.txt";
    const auto count_before = ops.Status().completed_ops; ops.Submit(invalid_count);
    Check(Wait(ops, count_before) && ops.Status().phase == OpPhase::Failed &&
        Exact(root / L"second.txt") && Exact(root / L"third.txt") && !std::filesystem::exists(root / L"multi.txt"),
        "single rename rejects multiple sources without changing files");
    make(L"conflict-source.txt"); make(L"conflict-target.txt");
    // A collision must never enter the Shell host's silent replacement path.
    rename(L"conflict-source.txt", L"conflict-target.txt");
    std::printf("[INFO] conflict result source=%d target=%d phase=%d\n", Exact(root / L"conflict-source.txt"),
        Exact(root / L"conflict-target.txt"), static_cast<int>(ops.Status().phase));
    Check(Exact(root / L"conflict-source.txt") && Exact(root / L"conflict-target.txt") &&
        ops.Status().phase == OpPhase::Failed && !ops.Status().last_error.empty() &&
        ops.Status().last_error != pulse::l10n::Get(pulse::l10n::StringId::OperationWorkerNotStarted),
        "existing target rejected without replacing either file");
    make(L"locked.txt");
    HANDLE locked = CreateFileW((root / L"locked.txt").c_str(), GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    Check(locked != INVALID_HANDLE_VALUE, "hold isolated file against deletion");
    rename(L"locked.txt", L"locked-renamed.txt");
    Check(Exact(root / L"locked.txt") && !std::filesystem::exists(root / L"locked-renamed.txt") &&
        ops.Status().phase == OpPhase::Failed && !ops.Status().last_error.empty() &&
        ops.Status().last_error != pulse::l10n::Get(pulse::l10n::StringId::OperationWorkerNotStarted),
        "sharing violation reports failure without changing file");
    if (locked != INVALID_HANDLE_VALUE) CloseHandle(locked);
    ops.Stop();
    // This absolute directory was created by this process under bench_data.
    std::filesystem::remove_all(root);
    return failures ? 1 : 0;
}
