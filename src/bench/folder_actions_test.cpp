#include "ops/ops_manager.h"
#include "common/localization.h"
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace pulse::ops {
struct TransferIntegrationProbe {
    static void Initialize(OpsManager& manager) {
        manager.running_ = true; manager.accepting_ = true;
    }
    static void Run(OpsManager& manager, OpRequest request) {
        OpsManager::QueueItem item;
        item.req = std::move(request); item.seq = manager.next_seq_++;
        manager.ExecuteFileOperation(item);
        const auto status = manager.Status();
        if (status.phase != OpPhase::Completed)
            std::printf("[DIAG] phase=%d hr=0x%08lX failure_path=%ls\n",
                static_cast<int>(status.phase), static_cast<unsigned long>(status.failure_hr), status.failed_path.c_str());
    }
    static bool Undo(OpsManager& manager) {
        manager.Undo();
        unsigned steps = 0;
        while (!manager.queue_.empty() && ++steps < 32) {
            auto item = std::move(manager.queue_.front()); manager.queue_.pop_front();
            const auto type = item.req.type;
            const auto action = item.req.folder_action;
            manager.ExecuteFileOperation(item);
            const auto status = manager.Status();
            std::printf("[DIAG] undo_step=%u type=%d action=%d phase=%d hr=0x%08lX history=%zu failure_path=%ls\n",
                steps, static_cast<int>(type), static_cast<int>(action), static_cast<int>(status.phase),
                static_cast<unsigned long>(status.failure_hr), manager.undo_.size(), status.failed_path.c_str());
        }
        return steps && manager.queue_.empty() && manager.Status().phase == OpPhase::Completed;
    }
    static void Notify(OpsManager& manager, std::function<void()> callback) {
        manager.notify_ = std::move(callback);
    }
    static void Cleanup(OpsManager& manager, std::function<void(const std::wstring&)> callback) {
        manager.before_source_cleanup_ = std::move(callback);
    }
    static size_t History(const OpsManager& manager) { return manager.undo_.size(); }
};
}
namespace {
namespace fs = std::filesystem;
using namespace pulse::ops;
int failures = 0;
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
        OpsManager manager; TransferIntegrationProbe::Initialize(manager);
        const auto drive = root.root_name().wstring();
        const auto parent = fs::path(L"\\\\?\\" + drive + L"\\");
        // An invalid leaf prevents creation while distinguishing parent
        // admission from the later filename validation, without touching a drive root.
        TransferIntegrationProbe::Run(manager, Action(FolderAction::GroupSelection, parent,
            {(parent / L"pulse-path-probe-a").wstring(), (parent / L"pulse-path-probe-b").wstring()}, L".."));
        Check(manager.Status().phase == OpPhase::Failed &&
            manager.Status().failure_hr == HRESULT_FROM_WIN32(ERROR_INVALID_NAME),
            "extended drive-root parents reach filename validation without losing the root separator");
    }
    {
        const auto dir = root / L"group"; fs::create_directory(dir);
        Write(dir / L"a.txt", "a"); fs::create_directory(dir / L"folder"); Write(dir / L"folder" / L"b.txt", "b");
        OpsManager manager; TransferIntegrationProbe::Initialize(manager);
        TransferIntegrationProbe::Run(manager, Action(FolderAction::GroupSelection, dir,
            {(dir / L"a.txt").wstring(), (dir / L"folder").wstring()}));
        Check(manager.Status().phase == OpPhase::Completed && Read(dir / L"Grouped" / L"a.txt") == "a" &&
            Read(dir / L"Grouped" / L"folder" / L"b.txt") == "b" && !fs::exists(dir / L"a.txt"),
            "group moves selected files and directories into the new sibling folder");
        Check(TransferIntegrationProbe::Undo(manager) && Read(dir / L"a.txt") == "a" &&
            Read(dir / L"folder" / L"b.txt") == "b" && !fs::exists(dir / L"Grouped") &&
            !TransferIntegrationProbe::History(manager), "one undo restores the selection and removes its empty created folder");
        fs::create_directory(dir / L"Grouped"); Write(dir / L"Grouped" / L"existing", "original");
        TransferIntegrationProbe::Run(manager, Action(FolderAction::GroupSelection, dir,
            {(dir / L"a.txt").wstring(), (dir / L"folder").wstring()}));
        Check(manager.Status().phase == OpPhase::Failed && Read(dir / L"a.txt") == "a" &&
            Read(dir / L"Grouped" / L"existing") == "original", "existing destination is never reused or overwritten");
        TransferIntegrationProbe::Run(manager, Action(FolderAction::GroupSelection, dir,
            {(dir / L"a.txt").wstring(), (dir / L"folder").wstring()}, L"..\\escape"));
        Check(manager.Status().phase == OpPhase::Failed && fs::exists(dir / L"a.txt"), "invalid grouping names cannot escape the parent");
    }
    {
        const auto dir = root / L"dissolve"; fs::create_directories(dir / L"one"); fs::create_directory(dir / L"two");
        Write(dir / L"same.txt", "existing"); Write(dir / L"one" / L"same.txt", "one"); Write(dir / L"two" / L"same.txt", "two");
        fs::create_directories(dir / L"one" / L"nested"); Write(dir / L"one" / L"nested" / L"content", "nested");
        OpsManager manager; TransferIntegrationProbe::Initialize(manager);
        TransferIntegrationProbe::Run(manager, Action(FolderAction::DissolveFolders, dir,
            {(dir / L"one").wstring(), (dir / L"two").wstring()}));
        Check(manager.Status().phase == OpPhase::Completed && !fs::exists(dir / L"one") && !fs::exists(dir / L"two") &&
            Read(dir / L"same.txt") == "existing" && Read(dir / L"nested" / L"content") == "nested",
            "dissolve moves child directories and preserves preexisting colliding files");
        size_t copies = 0;
        for (const auto& entry : fs::directory_iterator(dir)) if (entry.is_regular_file()) ++copies;
        Check(copies == 3, "collisions from several selected folders keep all three files");
        const auto history = manager.UndoToJson();
        OpsManager restored; TransferIntegrationProbe::Initialize(restored);
        Check(restored.UndoFromJson(history) && restored.CanUndo(), "compound dissolve history persists and reloads");
        Check(TransferIntegrationProbe::Undo(manager) && Read(dir / L"one" / L"same.txt") == "one" &&
            Read(dir / L"two" / L"same.txt") == "two" && Read(dir / L"one" / L"nested" / L"content") == "nested" &&
            !TransferIntegrationProbe::History(manager), "one undo recreates every removed container before moving children back");
    }
    {
        const auto dir = root / L"wrapper"; fs::create_directories(dir / L"X" / L"X"); Write(dir / L"X" / L"X" / L"data", "x");
        OpsManager manager; TransferIntegrationProbe::Initialize(manager);
        TransferIntegrationProbe::Run(manager, Action(FolderAction::DissolveFolders, dir, {(dir / L"X").wstring()}));
        Check(manager.Status().phase == OpPhase::Completed && Read(dir / L"X" / L"data") == "x" &&
            !fs::exists(dir / L"X (2)"), "X/X unwrap retains the inner folder name");
        Check(TransferIntegrationProbe::Undo(manager) && Read(dir / L"X" / L"X" / L"data") == "x" &&
            !fs::exists(dir / L"X (2)"), "unwrap undo restores contents before restoring the outer name");
    }
    {
        const auto dir = root / L"empty"; fs::create_directories(dir / L"empty");
        OpsManager manager; TransferIntegrationProbe::Initialize(manager);
        TransferIntegrationProbe::Run(manager, Action(FolderAction::DissolveFolders, dir, {(dir / L"empty").wstring()}));
        Check(manager.Status().phase == OpPhase::Completed && !fs::exists(dir / L"empty"), "empty folders dissolve without a transfer request");
        Check(TransferIntegrationProbe::Undo(manager) && fs::is_directory(dir / L"empty"), "empty folder dissolve remains undoable");
        TransferIntegrationProbe::Run(manager, Action(FolderAction::DissolveFolders, dir, {(dir / L"empty").wstring()}));
        fs::create_directory(dir / L"empty"); Write(dir / L"empty" / L"unrelated", "safe");
        Check(!TransferIntegrationProbe::Undo(manager) && Read(dir / L"empty" / L"unrelated") == "safe" && manager.CanUndo(),
            "undo refuses an independently recreated container and retains its history");
    }
    {
        const auto dir = root / L"cancel"; fs::create_directories(dir / L"folder"); Write(dir / L"folder" / L"original", "safe");
        OpsManager manager; TransferIntegrationProbe::Initialize(manager);
        TransferIntegrationProbe::Notify(manager, [&] { if (manager.Status().phase == OpPhase::Scanning) manager.CancelCurrent(); });
        TransferIntegrationProbe::Run(manager, Action(FolderAction::DissolveFolders, dir, {(dir / L"folder").wstring()}));
        TransferIntegrationProbe::Notify(manager, {});
        Check(manager.Status().phase == OpPhase::Cancelled && Read(dir / L"folder" / L"original") == "safe" &&
            !TransferIntegrationProbe::History(manager), "cancellation during preparation leaves the selected folder intact");
    }
    {
        const auto dir = root / L"arriving"; fs::create_directories(dir / L"folder"); Write(dir / L"folder" / L"original", "moved");
        OpsManager manager; TransferIntegrationProbe::Initialize(manager);
        TransferIntegrationProbe::Cleanup(manager, [&](const auto& path) { if (path == (dir / L"folder").wstring()) Write(dir / L"folder" / L"arrived", "safe"); });
        TransferIntegrationProbe::Run(manager, Action(FolderAction::DissolveFolders, dir, {(dir / L"folder").wstring()}));
        TransferIntegrationProbe::Cleanup(manager, {});
        Check(manager.Status().phase == OpPhase::Failed && Read(dir / L"folder" / L"arrived") == "safe" &&
            Read(dir / L"original") == "moved", "an arriving child prevents removal of the emptied container");
        Check(TransferIntegrationProbe::Undo(manager) && Read(dir / L"folder" / L"original") == "moved" &&
            Read(dir / L"folder" / L"arrived") == "safe", "partially completed dissolve can restore moved children without touching new files");
    }
    {
        const auto dir = root / L"replacement"; fs::create_directories(dir / L"folder");
        OpsManager manager; TransferIntegrationProbe::Initialize(manager);
        TransferIntegrationProbe::Cleanup(manager, [&](const auto& path) {
            if (path == (dir / L"folder").wstring()) { fs::rename(dir / L"folder", dir / L"old"); fs::create_directory(dir / L"folder"); }
        });
        TransferIntegrationProbe::Run(manager, Action(FolderAction::DissolveFolders, dir, {(dir / L"folder").wstring()}));
        TransferIntegrationProbe::Cleanup(manager, {});
        Check(manager.Status().phase == OpPhase::Failed && fs::is_directory(dir / L"folder") && fs::is_directory(dir / L"old"),
            "container replacement after enumeration fails identity validation without deleting its replacement");
    }
    {
        const auto dir = root / L"boundaries"; fs::create_directory(dir); Write(dir / L"file", "safe");
        OpsManager manager; TransferIntegrationProbe::Initialize(manager);
        TransferIntegrationProbe::Run(manager, Action(FolderAction::DissolveFolders, dir, {(dir / L"file").wstring()}));
        Check(manager.Status().phase == OpPhase::Failed && Read(dir / L"file") == "safe", "dissolve refuses ordinary files");
        TransferIntegrationProbe::Run(manager, Action(FolderAction::GroupSelection, dir, {(root / L"outside").wstring(), (dir / L"file").wstring()}));
        Check(manager.Status().phase == OpPhase::Failed && !fs::exists(dir / L"Grouped"), "group refuses sources outside the current parent");
        fs::create_directory(dir / L"target");
        if (CreateSymbolicLinkW((dir / L"link").c_str(), (dir / L"target").c_str(),
            SYMBOLIC_LINK_FLAG_DIRECTORY | SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE)) {
            TransferIntegrationProbe::Run(manager, Action(FolderAction::DissolveFolders, dir, {(dir / L"link").wstring()}));
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
    try {
        fs::create_directories(parent);
        if (!fs::create_directory(root)) throw std::runtime_error("fixture already exists");
        Tests(root);
    } catch (const std::exception& error) { Check(false, error.what()); }
    if (root.parent_path() == parent && root.filename().wstring().starts_with(L"folder-actions-")) {
        std::error_code error; fs::remove_all(root, error);
        Check(!error, "isolated fixture cleanup");
    }
    std::printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
