// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "ops/ops_manager.h"
#include "common/localization.h"
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace {
namespace fs = std::filesystem;
using namespace pulse::ops;
int failures = 0;
HWND owner = nullptr;
fs::path journal_root;

void PumpMessages() {
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}
bool Wait(OpsManager& manager, uint64_t before) {
    const auto deadline = GetTickCount64() + 30000;
    while (GetTickCount64() < deadline) {
        PumpMessages();
        if (manager.Status().completed_ops > before && !manager.HasPendingFileOperations()) return true;
        Sleep(2);
    }
    manager.CancelCurrent();
    return false;
}
struct FolderActionSession {
    static void Initialize(OpsManager& manager, std::function<void()> notify = [] {}) {
        static unsigned sequence = 0;
        manager.SetUiWindow(owner);
        manager.SetJournalPath((journal_root / (L"journal-" + std::to_wstring(++sequence) + L".json")).wstring());
        manager.Start(std::move(notify));
    }
    static void Run(OpsManager& manager, OpRequest request) {
        const auto before = manager.Status().completed_ops;
        const auto task = manager.Submit(std::move(request));
        if (!task || !Wait(manager, before)) throw std::runtime_error("folder action did not terminate within its deadline");
        const auto status = manager.Status();
        if (status.task_id != task) throw std::runtime_error("folder action returned another task's status");
        if (status.phase != OpPhase::Completed)
            std::printf("[DIAG] phase=%d hr=0x%08lX failure_path=%ls\n",
                static_cast<int>(status.phase), static_cast<unsigned long>(status.failure_hr), status.failed_path.c_str());
    }
    static bool Undo(OpsManager& manager) {
        const auto before = manager.Status().completed_ops;
        manager.Undo();
        if (!Wait(manager, before)) throw std::runtime_error("folder undo did not terminate within its deadline");
        return manager.Status().phase == OpPhase::Completed;
    }
};
bool EmptyHistory(const OpsManager& manager) {
    return manager.UndoToJson().find(L'{') == std::wstring::npos;
}
void Check(bool value, const char* label) {
    std::printf("[%s] %s\n", value ? "PASS" : "FAIL", label);
    failures += !value;
}
void Write(const fs::path& path, const char* text) { std::ofstream(path, std::ios::binary) << text; }
std::string Read(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
OpRequest Action(FolderAction action, const fs::path& parent, std::vector<std::wstring> sources,
                 const wchar_t* name = L"Grouped") {
    OpRequest request;
    request.type = OpType::Move; request.folder_action = action;
    request.dest_dir = parent.wstring(); request.sources = std::move(sources); request.new_name = name;
    request.undo_label = action == FolderAction::GroupSelection ? L"New Folder with Selection" : L"Dissolve Folders";
    request.collision_policy = CollisionPolicy::KeepBoth;
    return request;
}
void Tests(const fs::path& root) {
    {
        OpsManager manager; FolderActionSession::Initialize(manager);
        const auto drive = root.root_name().wstring();
        const auto parent = fs::path(L"\\\\?\\" + drive + L"\\");
        // An invalid leaf prevents creation while distinguishing parent
        // admission from the later filename validation, without touching a drive root.
        FolderActionSession::Run(manager, Action(FolderAction::GroupSelection, parent,
            {(parent / L"pulse-path-probe-a").wstring(), (parent / L"pulse-path-probe-b").wstring()}, L".."));
        Check(manager.Status().phase == OpPhase::Failed &&
            manager.Status().failure_hr == HRESULT_FROM_WIN32(ERROR_INVALID_NAME),
            "extended drive-root parents reach filename validation without losing the root separator");
    }
    {
        const auto dir = root / L"group"; fs::create_directory(dir);
        Write(dir / L"a.txt", "a"); fs::create_directory(dir / L"folder"); Write(dir / L"folder" / L"b.txt", "b");
        OpsManager manager; FolderActionSession::Initialize(manager);
        FolderActionSession::Run(manager, Action(FolderAction::GroupSelection, dir,
            {(dir / L"a.txt").wstring(), (dir / L"folder").wstring()}, L"CON .txt"));
        Check(manager.Status().phase == OpPhase::Failed && Read(dir / L"a.txt") == "a" &&
            Read(dir / L"folder" / L"b.txt") == "b" && EmptyHistory(manager),
            "group uses shared reserved-name validation before moving any source");
        FolderActionSession::Run(manager, Action(FolderAction::GroupSelection, dir,
            {(dir / L"a.txt").wstring(), (dir / L"folder").wstring()}));
        Check(manager.Status().phase == OpPhase::Completed && Read(dir / L"Grouped" / L"a.txt") == "a" &&
            Read(dir / L"Grouped" / L"folder" / L"b.txt") == "b" && !fs::exists(dir / L"a.txt"),
            "group moves selected files and directories into the new sibling folder");
        Check(FolderActionSession::Undo(manager) && Read(dir / L"a.txt") == "a" &&
            Read(dir / L"folder" / L"b.txt") == "b" && !fs::exists(dir / L"Grouped") &&
            EmptyHistory(manager), "one undo restores the selection and removes its empty created folder");
        fs::create_directory(dir / L"Grouped"); Write(dir / L"Grouped" / L"existing", "original");
        FolderActionSession::Run(manager, Action(FolderAction::GroupSelection, dir,
            {(dir / L"a.txt").wstring(), (dir / L"folder").wstring()}));
        Check(manager.Status().phase == OpPhase::Failed && Read(dir / L"a.txt") == "a" &&
            Read(dir / L"Grouped" / L"existing") == "original", "existing destination is never reused or overwritten");
        FolderActionSession::Run(manager, Action(FolderAction::GroupSelection, dir,
            {(dir / L"a.txt").wstring(), (dir / L"folder").wstring()}, L"..\\escape"));
        Check(manager.Status().phase == OpPhase::Failed && fs::exists(dir / L"a.txt"), "invalid grouping names cannot escape the parent");
    }
    {
        const auto dir = root / L"dissolve"; fs::create_directories(dir / L"one"); fs::create_directory(dir / L"two");
        Write(dir / L"same.txt", "existing"); Write(dir / L"one" / L"same.txt", "one"); Write(dir / L"two" / L"same.txt", "two");
        fs::create_directories(dir / L"one" / L"nested"); Write(dir / L"one" / L"nested" / L"content", "nested");
        OpsManager manager; FolderActionSession::Initialize(manager);
        FolderActionSession::Run(manager, Action(FolderAction::DissolveFolders, dir,
            {(dir / L"one").wstring(), (dir / L"two").wstring()}));
        Check(manager.Status().phase == OpPhase::Completed && !fs::exists(dir / L"one") && !fs::exists(dir / L"two") &&
            Read(dir / L"same.txt") == "existing" && Read(dir / L"nested" / L"content") == "nested",
            "dissolve moves child directories and preserves preexisting colliding files");
        size_t copies = 0;
        for (const auto& entry : fs::directory_iterator(dir)) if (entry.is_regular_file()) ++copies;
        Check(copies == 3, "collisions from several selected folders keep all three files");
        const auto history = manager.UndoToJson();
        OpsManager restored; FolderActionSession::Initialize(restored);
        Check(restored.UndoFromJson(history) && restored.CanUndo(), "compound dissolve history persists and reloads");
        Check(FolderActionSession::Undo(manager) && Read(dir / L"one" / L"same.txt") == "one" &&
            Read(dir / L"two" / L"same.txt") == "two" && Read(dir / L"one" / L"nested" / L"content") == "nested" &&
            EmptyHistory(manager), "one undo recreates every removed container before moving children back");
    }
    {
        const auto dir = root / L"wrapper"; fs::create_directories(dir / L"X" / L"X"); Write(dir / L"X" / L"X" / L"data", "x");
        OpsManager manager; FolderActionSession::Initialize(manager);
        FolderActionSession::Run(manager, Action(FolderAction::DissolveFolders, dir, {(dir / L"X").wstring()}));
        Check(manager.Status().phase == OpPhase::Completed && Read(dir / L"X" / L"data") == "x" &&
            !fs::exists(dir / L"X (2)"), "X/X unwrap retains the inner folder name");
        Check(FolderActionSession::Undo(manager) && Read(dir / L"X" / L"X" / L"data") == "x" &&
            !fs::exists(dir / L"X (2)"), "unwrap undo restores contents before restoring the outer name");
    }
    {
        const auto dir = root / L"empty"; fs::create_directories(dir / L"empty");
        OpsManager manager; FolderActionSession::Initialize(manager);
        FolderActionSession::Run(manager, Action(FolderAction::DissolveFolders, dir, {(dir / L"empty").wstring()}));
        Check(manager.Status().phase == OpPhase::Completed && !fs::exists(dir / L"empty"), "empty folders dissolve without a transfer request");
        Check(FolderActionSession::Undo(manager) && fs::is_directory(dir / L"empty"), "empty folder dissolve remains undoable");
        FolderActionSession::Run(manager, Action(FolderAction::DissolveFolders, dir, {(dir / L"empty").wstring()}));
        fs::create_directory(dir / L"empty"); Write(dir / L"empty" / L"unrelated", "safe");
        Check(!FolderActionSession::Undo(manager) && Read(dir / L"empty" / L"unrelated") == "safe" && manager.CanUndo(),
            "undo refuses an independently recreated container and retains its history");
    }
    {
        const auto dir = root / L"cancel"; fs::create_directories(dir / L"folder"); Write(dir / L"folder" / L"original", "safe");
        OpsManager manager;
        FolderActionSession::Initialize(manager, [&] { if (manager.Status().phase == OpPhase::Scanning) manager.CancelCurrent(); });
        FolderActionSession::Run(manager, Action(FolderAction::DissolveFolders, dir, {(dir / L"folder").wstring()}));
        Check(manager.Status().phase == OpPhase::Cancelled && Read(dir / L"folder" / L"original") == "safe" &&
            EmptyHistory(manager), "cancellation during preparation leaves the selected folder intact");
    }
    {
        const auto dir = root / L"boundaries"; fs::create_directory(dir); Write(dir / L"file", "safe");
        OpsManager manager; FolderActionSession::Initialize(manager);
        FolderActionSession::Run(manager, Action(FolderAction::DissolveFolders, dir, {(dir / L"file").wstring()}));
        Check(manager.Status().phase == OpPhase::Failed && Read(dir / L"file") == "safe", "dissolve refuses ordinary files");
        FolderActionSession::Run(manager, Action(FolderAction::GroupSelection, dir, {(root / L"outside").wstring(), (dir / L"file").wstring()}));
        Check(manager.Status().phase == OpPhase::Failed && !fs::exists(dir / L"Grouped"), "group refuses sources outside the current parent");
        fs::create_directory(dir / L"target");
        if (CreateSymbolicLinkW((dir / L"link").c_str(), (dir / L"target").c_str(),
            SYMBOLIC_LINK_FLAG_DIRECTORY | SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE)) {
            FolderActionSession::Run(manager, Action(FolderAction::DissolveFolders, dir, {(dir / L"link").wstring()}));
            Check(manager.Status().phase == OpPhase::Failed && fs::exists(dir / L"link") && fs::is_directory(dir / L"target"),
                "dissolve refuses directory reparse points");
        } else std::printf("[SKIP] directory symlink fixture unavailable (%lu)\n", GetLastError());
    }
}
}
int main() {
    pulse::l10n::Initialize(GetModuleHandleW(nullptr), L"en-US");
    const auto parent = fs::absolute(L"bench_data").lexically_normal();
    const auto root = parent / (L"folder-actions-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    owner = CreateWindowExW(0, L"STATIC", L"Pulse folder action fixture", 0,
        0, 0, 0, 0, HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!owner) { Check(false, "create Shell dialog owner"); return 1; }
    journal_root = root;
    try {
        fs::create_directories(parent);
        if (!fs::create_directory(root)) throw std::runtime_error("fixture already exists");
        Tests(root);
    } catch (const std::exception& error) { Check(false, error.what()); }
    DestroyWindow(owner);
    if (root.parent_path() == parent && root.filename().wstring().starts_with(L"folder-actions-")) {
        std::error_code error; fs::remove_all(root, error);
        Check(!error, "isolated fixture cleanup");
    }
    std::printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
