#include "file_operation_matrix.h"
#include "../ops/ops_manager.h"
#include "../common/localization.h"
#include <algorithm>
#include <array>
#include <bcrypt.h>
#include <cstdio>
#include <fcntl.h>
#include <io.h>
#include <set>

namespace pulse::bench {
namespace {
namespace fs = std::filesystem;
using namespace pulse::ops;

struct Results {
    const wchar_t* group;
    int passed = 0;
    int failed = 0;
    int blocked = 0;
    void Check(bool ok, const wchar_t* label) {
        wprintf(L"[%ls] [%ls] %ls\n", ok ? L"PASS" : L"FAIL", group, label);
        if (ok) ++passed; else ++failed;
    }
    void Block(const wchar_t* label, DWORD error) {
        ++blocked;
        wprintf(L"[BLOCKED] [%ls] %ls Win32=%lu\n", group, label, error);
    }
    void Print() const {
        wprintf(L"== %ls: %d passed, %d failed, %d blocked ==\n", group, passed, failed, blocked);
    }
};

bool Missing(const fs::path& path) {
    if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) return false;
    const auto error = GetLastError();
    return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
}

bool Directory(const fs::path& path) {
    const auto attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) &&
        !(attributes & FILE_ATTRIBUTE_REPARSE_POINT);
}

unsigned char Pattern(uint64_t offset, unsigned seed) {
    return static_cast<unsigned char>((offset * 131u) ^ (offset >> 11u) ^ seed);
}

// Independent expected bytes also catch short reads and a missing zero-byte file.
bool PatternFile(const fs::path& path, uint64_t bytes, unsigned seed, bool write = false) {
    const HANDLE file = CreateFileW(path.c_str(), write ? GENERIC_WRITE : GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        write ? CREATE_NEW : OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        wprintf(L"[DIAG] %ls %ls Win32=%lu\n", write ? L"write" : L"read", path.c_str(), GetLastError());
        return false;
    }
    const auto close = std::shared_ptr<void>(file, [](void* value) { CloseHandle(value); });
    std::array<unsigned char, 65536> buffer{};
    uint64_t offset = 0;
    while (offset < bytes) {
        const auto count = static_cast<DWORD>((std::min<uint64_t>)(buffer.size(), bytes - offset));
        if (write)
            for (DWORD i = 0; i < count; ++i) buffer[i] = Pattern(offset + i, seed);
        DWORD actual = 0;
        const bool ok = write ? WriteFile(file, buffer.data(), count, &actual, nullptr) != FALSE :
                                ReadFile(file, buffer.data(), count, &actual, nullptr) != FALSE;
        if (!ok || actual != count) return false;
        if (!write)
            for (DWORD i = 0; i < actual; ++i)
                if (buffer[i] != Pattern(offset + i, seed)) return false;
        offset += actual;
    }
    if (write) return FlushFileBuffers(file) != FALSE;
    DWORD extra = 0;
    return ReadFile(file, buffer.data(), 1, &extra, nullptr) && extra == 0;
}

struct OwnedFixture {
    fs::path root;
    fs::path parent;
    bool owned = false;
    bool Create(const fs::path& requested_parent, UINT drive_type, Results& result) {
        std::error_code error;
        parent = fs::absolute(requested_parent, error).lexically_normal();
        if (error || !Directory(parent) || GetDriveTypeW(parent.root_path().c_str()) != drive_type) {
            result.Block(L"fixture parent unavailable or wrong drive type", error ? error.value() : ERROR_BAD_PATHNAME);
            return false;
        }
        std::array<unsigned char, 16> nonce{};
        if (BCryptGenRandom(nullptr, nonce.data(), static_cast<ULONG>(nonce.size()),
                            BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
            result.Block(L"cannot generate exclusive fixture name", ERROR_GEN_FAILURE);
            return false;
        }
        std::wstring name = L"pulse-fileops-";
        for (const auto value : nonce) {
            name += L"0123456789abcdef"[value >> 4];
            name += L"0123456789abcdef"[value & 15];
        }
        root = parent / name;
        if (!CreateDirectoryW(root.c_str(), nullptr)) {
            result.Block(L"cannot create fresh fixture directory", GetLastError());
            return false;
        }
        owned = true;
        wprintf(L"[INFO] [%ls] exclusive fixture=%ls\n", result.group, root.c_str());
        if (!PatternFile(root / L".pulse-test-owner", 64, 197, true)) {
            result.Block(L"fixture write access unavailable", GetLastError());
            DeleteFileW((root / L".pulse-test-owner").c_str());
            owned = RemoveDirectoryW(root.c_str()) == FALSE;
            if (owned) result.Block(L"partially initialized fixture could not be removed", GetLastError());
            return false;
        }
        return true;
    }
    bool Contains(const std::wstring& name) const {
        const auto path = fs::path(name).lexically_normal();
        const auto relative = path.lexically_relative(root);
        return owned && !relative.empty() && !relative.is_absolute() && *relative.begin() != L".." && path != root;
    }
    void Cleanup(Results& result) {
        if (!owned) return;
        // Validate the absolute, directly owned root before recursive removal.
        if (root.parent_path() != parent || !root.filename().wstring().starts_with(L"pulse-fileops-") ||
            !Directory(root) || !PatternFile(root / L".pulse-test-owner", 64, 197)) {
            result.Check(false, L"cleanup ownership check; fixture preserved for inspection");
            return;
        }
        std::error_code error;
        fs::remove_all(root, error);
        if (error) wprintf(L"[DIAG] cleanup %ls error=%d\n", root.c_str(), error.value());
        result.Check(!error && Missing(root), L"remove only the exclusive fixture directory");
        owned = false;
    }
};

OpRequest Request(OpType type, std::initializer_list<fs::path> sources,
                  const fs::path& destination = {}, const wchar_t* name = L"") {
    OpRequest request;
    request.type = type;
    for (const auto& source : sources) request.sources.push_back(source.wstring());
    request.dest_dir = destination.wstring();
    request.new_name = name;
    return request;
}

struct Session {
    OpsManager manager;
    Results& result;
    const OwnedFixture& first;
    const OwnedFixture& second;
    bool usable = true;
    unsigned confirmations = 0;
    unsigned conflicts = 0;
    bool permanent = false;
    Session(HWND owner, Results& checks, const OwnedFixture& a, const OwnedFixture& b)
        : result(checks), first(a), second(b) {
        manager.SetUiWindow(owner);
        manager.Start([] {});
    }
    ~Session() {
        manager.Stop();
        manager.UndoFromJson(L"[]");
        manager.ClearFinishedTasks();
    }
    bool Wait(uint64_t before, uint64_t id, bool accept_delete, ConflictChoice choice) {
        const auto deadline = GetTickCount64() + 45000;
        while (GetTickCount64() < deadline) {
            if (const auto pending = manager.PendingDeleteConfirmation()) {
                bool owned = !pending->plan.targets.empty();
                for (const auto& target : pending->plan.targets) {
                    owned = owned && !target.physical_paths.empty();
                    for (const auto& path : target.physical_paths)
                        owned = owned && (first.Contains(path) || second.Contains(path));
                    permanent = permanent || target.disposition == DeleteDisposition::Permanent;
                }
                ++confirmations;
                manager.ResolveDeleteConfirmation(pending->token, accept_delete && owned);
                if (!owned) result.Check(false, L"delete confirmation escaped the owned fixture");
            }
            if (const auto pending = manager.PendingConflict()) {
                ++conflicts;
                manager.ResolveConflict(pending->token, choice, true);
            }
            const auto status = manager.Status();
            if (status.completed_ops > before && !status.active && !manager.HasPendingFileOperations()) {
                if (status.task_id != id) result.Check(false, L"completion belongs to the submitted task");
                if (!status.last_error.empty() || FAILED(status.failure_hr))
                    wprintf(L"[DIAG] [%ls] task=%llu phase=%d hr=%08lx path=%ls error=%ls\n",
                        result.group, id, static_cast<int>(status.phase), status.failure_hr,
                        status.failed_path.c_str(), status.last_error.c_str());
                return status.task_id == id;
            }
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            Sleep(5);
        }
        result.Check(false, L"operation reached the 45 second deadline");
        manager.CancelCurrent();
        usable = false;
        return false;
    }
    bool Run(OpRequest request, const wchar_t* label, bool accept_delete = true,
             ConflictChoice choice = ConflictChoice::Cancel, OpPhase expected = OpPhase::Completed) {
        if (!usable) { result.Block(label, ERROR_CANCELLED); return false; }
        confirmations = conflicts = 0; permanent = false;
        const auto before = manager.Status().completed_ops;
        const auto id = manager.Submit(std::move(request));
        const bool finished = id && Wait(before, id, accept_delete, choice);
        const auto status = manager.Status();
        const bool ok = finished && status.phase == expected &&
            (expected != OpPhase::Completed || (SUCCEEDED(status.failure_hr) && status.last_error.empty()));
        result.Check(ok, label);
        return ok;
    }
    bool Undo(const wchar_t* label) {
        if (!usable || !manager.CanUndo()) { result.Check(false, label); return false; }
        confirmations = conflicts = 0; permanent = false;
        const auto before = manager.Status().completed_ops;
        manager.Undo();
        // Undo assigns its own task ID; require the exact next completion count instead.
        const auto deadline = GetTickCount64() + 45000;
        while (GetTickCount64() < deadline &&
               (manager.Status().completed_ops == before || manager.HasPendingFileOperations())) Sleep(5);
        const auto status = manager.Status();
        const bool ok = status.completed_ops == before + 1 && !status.active &&
            !manager.HasPendingFileOperations() && status.phase == OpPhase::Completed &&
            SUCCEEDED(status.failure_hr) && status.last_error.empty();
        if (!ok) wprintf(L"[DIAG] Undo phase=%d hr=%08lx error=%ls\n",
            static_cast<int>(status.phase), status.failure_hr, status.last_error.c_str());
        result.Check(ok, label);
        if (manager.HasPendingFileOperations()) { manager.CancelCurrent(); usable = false; }
        return ok;
    }
};

bool MakeTree(const fs::path& root) {
    return CreateDirectoryW(root.c_str(), nullptr) && CreateDirectoryW((root / L"sub").c_str(), nullptr) &&
        CreateDirectoryW((root / L"empty").c_str(), nullptr) &&
        PatternFile(root / L"说明 空格.txt", 97, 31, true) &&
        PatternFile(root / L"sub" / L"binary.bin", 131085, 71, true) &&
        PatternFile(root / L"zero.txt", 0, 0, true);
}

bool TreeMatches(const fs::path& root) {
    if (!Directory(root)) return false;
    const std::set<fs::path> expected{L"sub", L"empty", L"说明 空格.txt", L"sub/binary.bin", L"zero.txt"};
    std::set<fs::path> actual;
    std::error_code error;
    fs::recursive_directory_iterator it(root, error), end;
    while (!error && it != end) {
        if (it->is_symlink(error)) return false;
        actual.insert(it->path().lexically_relative(root).generic_wstring());
        it.increment(error);
    }
    return !error && actual == expected && Directory(root / L"empty") && Directory(root / L"sub") &&
        PatternFile(root / L"说明 空格.txt", 97, 31) &&
        PatternFile(root / L"sub" / L"binary.bin", 131085, 71) && PatternFile(root / L"zero.txt", 0, 0);
}

void Transfers(Session& session, const fs::path& source, const fs::path& destination) {
    auto& result = session.result;
    constexpr uint64_t bytes = 2 * 1024 * 1024 + 137;
    if (!CreateDirectoryW(source.c_str(), nullptr) || !CreateDirectoryW(destination.c_str(), nullptr) ||
        !PatternFile(source / L"数据 空格.bin", bytes, 23, true) ||
        !PatternFile(source / L"zero.bin", 0, 0, true) || !MakeTree(source / L"tree") ||
        !PatternFile(source / L"move.bin", 65549, 59, true) || !MakeTree(source / L"move-tree")) {
        result.Block(L"cannot prepare transfer fixture", GetLastError());
        return;
    }
    session.Run(Request(OpType::Copy, {source / L"数据 空格.bin", source / L"zero.bin"}, destination), L"copy files");
    result.Check(PatternFile(source / L"数据 空格.bin", bytes, 23) &&
        PatternFile(destination / L"数据 空格.bin", bytes, 23) &&
        PatternFile(source / L"zero.bin", 0, 0) && PatternFile(destination / L"zero.bin", 0, 0),
        L"copy preserves both complete byte streams, Unicode names and zero-byte files");
    session.Run(Request(OpType::Copy, {source / L"tree"}, destination), L"copy a nonempty directory");
    result.Check(TreeMatches(source / L"tree") && TreeMatches(destination / L"tree"),
        L"directory copy preserves the exact tree, empty directory and every byte");
    session.Run(Request(OpType::Move, {source / L"move.bin"}, destination), L"move a file");
    result.Check(Missing(source / L"move.bin") && PatternFile(destination / L"move.bin", 65549, 59),
        L"file move commits all bytes and removes the source");
    const bool file_undo = session.Undo(L"undo file move");
    result.Check(file_undo && PatternFile(source / L"move.bin", 65549, 59) && Missing(destination / L"move.bin"),
        L"file move Undo restores the original bytes and removes its product");
    session.Run(Request(OpType::Move, {source / L"move-tree"}, destination), L"move a nonempty directory");
    result.Check(Missing(source / L"move-tree") && TreeMatches(destination / L"move-tree"),
        L"directory move commits the complete tree before source removal");
    const bool tree_undo = session.Undo(L"undo directory move");
    result.Check(tree_undo && TreeMatches(source / L"move-tree") && Missing(destination / L"move-tree"),
        L"directory move Undo restores the complete original tree");

    const auto conflict_source = source / L"conflict-source";
    if (!CreateDirectoryW(conflict_source.c_str(), nullptr) ||
        !PatternFile(conflict_source / L"数据 空格.bin", bytes, 181, true)) {
        result.Block(L"cannot prepare different conflict contents", GetLastError());
        return;
    }
    const auto copy = Request(OpType::Copy, {conflict_source / L"数据 空格.bin"}, destination);
    session.Run(copy, L"cancel a copy conflict", true, ConflictChoice::Cancel, OpPhase::Cancelled);
    result.Check(session.conflicts == 1 && PatternFile(conflict_source / L"数据 空格.bin", bytes, 181) &&
        PatternFile(destination / L"数据 空格.bin", bytes, 23), L"conflict cancellation preserves both versions");
    session.Run(copy, L"skip a copy conflict", true, ConflictChoice::Skip);
    result.Check(session.conflicts == 1 && PatternFile(conflict_source / L"数据 空格.bin", bytes, 181) &&
        PatternFile(destination / L"数据 空格.bin", bytes, 23), L"conflict skip preserves both versions");
    session.Run(copy, L"replace a copy conflict", true, ConflictChoice::Replace);
    result.Check(session.conflicts == 1 && PatternFile(conflict_source / L"数据 空格.bin", bytes, 181) &&
        PatternFile(destination / L"数据 空格.bin", bytes, 181), L"accepted replacement commits the complete new version");
    session.manager.DrainCompletions();
    session.Run(copy, L"keep both copies on a conflict", true, ConflictChoice::KeepBoth);
    const auto completions = session.manager.DrainCompletions();
    bool kept = false;
    for (const auto& completion : completions)
        for (const auto& name : completion.destinations)
            kept = kept || (fs::path(name).parent_path() == destination &&
                fs::path(name).filename() != L"数据 空格.bin" && PatternFile(name, bytes, 181));
    result.Check(session.conflicts == 1 && kept && PatternFile(destination / L"数据 空格.bin", bytes, 181),
        L"keep-both records a distinct destination with the exact contents");
}

void Mutations(Session& session, const fs::path& root, bool network) {
    auto& result = session.result;
    const auto created_folder = root / L"新建 目录", created_file = root / L"新建 文件.txt";
    session.Run(Request(OpType::CreateFolder, {created_folder}), L"create a directory through the Shell host");
    result.Check(Directory(created_folder), L"created directory is present");
    session.Run(Request(OpType::CreateTextFile, {created_file}), L"create a file through the Shell host");
    result.Check(PatternFile(created_file, 0, 0), L"created file exists and is empty");

    // Rename and deletion have independent setup so a failed creation request
    // cannot prevent exercising the remaining production operation paths.
    const auto folder = root / L"rename-source", renamed = root / L"重命名 目录";
    const auto file = folder / L"原始 文件.txt";
    if (!CreateDirectoryW(folder.c_str(), nullptr) || !PatternFile(file, 4099, 47, true)) {
        result.Block(L"cannot prepare independent rename fixture", GetLastError());
        return;
    }
    session.Run(Request(OpType::Rename, {file}, {}, L"first.txt"), L"first file rename");
    session.Run(Request(OpType::Rename, {folder / L"first.txt"}, {}, L"second.txt"), L"second file rename");
    result.Check(Missing(file) && Missing(folder / L"first.txt") && PatternFile(folder / L"second.txt", 4099, 47),
        L"consecutive renames preserve file contents and remove earlier names");
    const bool second_undo = session.Undo(L"undo the second rename");
    const bool first_undo = session.Undo(L"undo the first rename");
    result.Check(second_undo && first_undo && PatternFile(file, 4099, 47) &&
        Missing(folder / L"first.txt") && Missing(folder / L"second.txt"),
        L"both rename inverses restore the exact original file");
    session.Run(Request(OpType::Rename, {folder}, {}, renamed.filename().c_str()), L"rename a nonempty directory");
    result.Check(Missing(folder) && PatternFile(renamed / file.filename(), 4099, 47),
        L"directory rename preserves its child and removes the original entry");
    session.Run(Request(OpType::Rename, {renamed / file.filename()}, {}, L"delete-first.txt"),
        L"retain first rename history before deletion");
    session.Run(Request(OpType::Rename, {renamed / L"delete-first.txt"}, {}, L"delete-final.txt"),
        L"retain second rename history before deletion");
    const auto leaf = renamed / L"delete-final.txt";
    session.Run(Request(OpType::RealDelete, {leaf}), L"reject permanent delete confirmation", false,
        ConflictChoice::Cancel, OpPhase::Cancelled);
    result.Check(session.confirmations == 1 && PatternFile(leaf, 4099, 47),
        L"rejected permanent deletion preserves the exact file");
    session.Run(Request(OpType::RealDelete, {leaf}), L"confirm permanent file deletion");
    result.Check(session.confirmations == 1 && session.permanent && Missing(leaf),
        L"permanently deleted file disappears while earlier Undo references still exist");
    if (MakeTree(root / L"delete-tree")) {
        session.Run(Request(OpType::RealDelete, {root / L"delete-tree", root / L"delete-tree" / L"sub"}),
            L"delete a directory with an overlapping child selection");
        result.Check(session.confirmations == 1 && Missing(root / L"delete-tree") && Directory(renamed),
            L"recursive deletion removes the selected tree and preserves its sibling");
    } else result.Block(L"cannot prepare recursive deletion fixture", GetLastError());

    session.manager.UndoFromJson(L"[]");
    const auto ordinary = root / L"ordinary-delete.bin";
    if (!PatternFile(ordinary, 8197, 101, true)) {
        result.Block(L"cannot prepare default deletion fixture", GetLastError());
        return;
    }
    session.Run(Request(OpType::RecycleDelete, {ordinary}), L"default deletion", network);
    result.Check(Missing(ordinary) && (network ? session.confirmations == 1 && session.permanent && !session.manager.CanUndo() :
        session.confirmations == 0 && session.manager.CanUndo()),
        network ? L"network default delete requires permanent confirmation and creates no Undo" :
                  L"local default delete uses the Recycle Bin and records Undo");
    if (!network) {
        const bool recycle_undo = session.Undo(L"undo local Recycle Bin deletion");
        result.Check(recycle_undo && PatternFile(ordinary, 8197, 101) && !session.manager.CanUndo(),
            L"Recycle Bin Undo restores the exact owned file");
    }
}

bool NoStaging(const fs::path& root) {
    std::error_code error;
    fs::recursive_directory_iterator it(root, error), end;
    while (!error && it != end) {
        const auto name = it->path().filename().wstring();
        if (name.starts_with(L".pulse-") && name != L".pulse-test-owner") return false;
        it.increment(error);
    }
    return !error;
}
}

int RunFileOperationMatrix(const fs::path& local_parent, const fs::path& network_parent) {
    _setmode(_fileno(stdout), _O_U8TEXT);
    setvbuf(stdout, nullptr, _IONBF, 0);
    l10n::Initialize(GetModuleHandleW(nullptr), L"en-US");
    Results local{L"local"}, network{L"network"}, outward{L"local-to-network"}, inward{L"network-to-local"};
    OwnedFixture local_fixture, network_fixture;
    const bool have_local = local_fixture.Create(local_parent, DRIVE_FIXED, local);
    const bool have_network = !network_parent.empty() && network_fixture.Create(network_parent, DRIVE_REMOTE, network);
    const HWND owner = CreateWindowExW(0, L"STATIC", L"Pulse file operation matrix", 0,
        0, 0, 0, 0, HWND_MESSAGE, nullptr, nullptr, nullptr);
    const auto owner_error = owner ? ERROR_SUCCESS : GetLastError();
    const auto within = [&](OwnedFixture& fixture, Results& result, bool remote) {
        if (!owner) { result.Block(L"cannot create operation owner window", owner_error); return; }
        {
            Session session(owner, result, fixture, fixture);
            Mutations(session, fixture.root, remote);
        }
        {
            Session session(owner, result, fixture, fixture);
            Transfers(session, fixture.root / L"transfer-source", fixture.root / L"transfer-destination");
        }
        result.Check(NoStaging(fixture.root), L"no transfer staging or backup files remain");
    };
    if (have_local) within(local_fixture, local, false);
    if (have_network) within(network_fixture, network, true);
    const auto across = [&](OwnedFixture& from, OwnedFixture& to, Results& result, const wchar_t* name) {
        if (!have_local || !have_network || !owner) {
            result.Block(L"both writable roots and operation owner are required", ERROR_NOT_READY);
            return;
        }
        {
            Session session(owner, result, from, to);
            Transfers(session, from.root / (std::wstring(name) + L"-source"),
                to.root / (std::wstring(name) + L"-destination"));
        }
        result.Check(NoStaging(from.root) && NoStaging(to.root), L"no cross-volume staging or backups remain");
    };
    if (!network_parent.empty()) {
        across(local_fixture, network_fixture, outward, L"outward");
        across(network_fixture, local_fixture, inward, L"inward");
    }
    if (owner) DestroyWindow(owner);
    local_fixture.Cleanup(local);
    network_fixture.Cleanup(network);
    int failed = 0, blocked = 0;
    for (const auto* result : {&local, &network, &outward, &inward}) {
        result->Print(); failed += result->failed; blocked += result->blocked;
    }
    return failed ? 1 : blocked ? 2 : 0;
}
}
