#include "ops_manager.h"
#include "../common/file_create.h"
#include "../common/file_operation_error.h"
#include "../common/localization.h"
#include "../common/path_utils.h"
#include <algorithm>
#include <atomic>
#include <cwchar>
#include <cwctype>

namespace pulse::ops {
namespace {
std::wstring Join(const std::wstring& parent, const std::wstring& name) {
    return parent + (parent.ends_with(L"\\") ? L"" : L"\\") + name;
}
std::wstring Parent(const std::wstring& path) {
    const auto slash = path.find_last_of(L"\\/");
    auto result = path.substr(0, slash);
    const auto plain = path::StripExtendedPathPrefix(result);
    if (plain.size() == 2 && plain[1] == L':') result += L'\\';
    return result;
}
std::wstring Leaf(const std::wstring& path) {
    return path.substr(path.find_last_of(L"\\/") + 1);
}
bool ValidName(const std::wstring& name) {
    if (name.empty() || name.size() > 255 || name == L"." || name == L".." ||
        name.back() == L'.' || name.back() == L' ') return false;
    for (const auto c : name)
        if (c < 32 || std::wstring_view(L"\\/:*?\"<>|").find(c) != std::wstring_view::npos) return false;
    std::wstring stem = name.substr(0, name.find(L'.'));
    for (auto& c : stem) c = static_cast<wchar_t>(std::towupper(c));
    if (stem == L"CON" || stem == L"PRN" || stem == L"AUX" || stem == L"NUL" ||
        stem == L"CONIN$" || stem == L"CONOUT$") return false;
    return !(stem.size() == 4 && (stem.starts_with(L"COM") || stem.starts_with(L"LPT")) &&
        ((stem[3] >= L'1' && stem[3] <= L'9') || stem[3] == L'\u00b9' || stem[3] == L'\u00b2' || stem[3] == L'\u00b3'));
}
uint64_t UndoGroup() {
    static std::atomic<uint64_t> next{0};
    FILETIME time{};
    GetSystemTimeAsFileTime(&time);
    const uint64_t now = (static_cast<uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
    uint64_t previous = next.load();
    while (!next.compare_exchange_weak(previous, (std::max)(now, previous + 1))) {}
    return (std::max)(now, previous + 1);
}
bool Children(const std::wstring& folder, std::vector<std::wstring>& names,
              const std::function<bool()>& cancelled, DWORD& error) {
    names.clear();
    WIN32_FIND_DATAW data{};
    const HANDLE find = FindFirstFileExW(Join(folder, L"*").c_str(), FindExInfoBasic,
        &data, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (find == INVALID_HANDLE_VALUE) {
        error = GetLastError();
        return error == ERROR_FILE_NOT_FOUND;
    }
    do {
        if (cancelled()) { FindClose(find); error = ERROR_CANCELLED; return false; }
        if (std::wcscmp(data.cFileName, L".") && std::wcscmp(data.cFileName, L".."))
            names.emplace_back(data.cFileName);
    } while (FindNextFileW(find, &data));
    error = GetLastError();
    FindClose(find);
    return error == ERROR_NO_MORE_FILES;
}
}

void OpsManager::ContinueFolderUndo(const OpRequest& req) {
    if (!req.is_undo || !req.undo_group) return;
    bool next = false;
    {
        std::lock_guard lock(mutex_);
        next = status_.phase == OpPhase::Completed && queue_.empty() && !undo_.empty() &&
            undo_.back().undo_group == req.undo_group && undo_.back().supported &&
            !stopping_.load();
    }
    if (next) Undo();
}

void OpsManager::RunFolderAction(const OpRequest& input, uint64_t task_id) {
    OpRequest request = input;
    if (!request.undo_group && !request.is_undo) request.undo_group = UndoGroup();
    const auto cancelled = [&] {
        return stopping_.load() || lock_retry_cancel_.load() || shell_cancel_requested_.load() || transfer_cancel_.load();
    };
    // The outer request owns cancellation until the first transfer starts.
    transfer_cancel_.store(stopping_.load() || lock_retry_cancel_.load());
    transfer_active_.store(true);
    struct Finish {
        std::atomic<bool>& active;
        ~Finish() { active.store(false); }
    } finish{transfer_active_};
    const auto failure = [&](DWORD error, const std::wstring& path) {
        const bool stopped = error == ERROR_CANCELLED || cancelled();
        SetStatus([&](OpStatus& status) {
            status.type = input.type; status.task_id = task_id;
            status.active = false; status.phase = stopped ? OpPhase::Cancelled : OpPhase::Failed;
            status.failure_hr = stopped ? S_OK : HRESULT_FROM_WIN32(error);
            status.failed_path = stopped ? L"" : path;
            status.summary = status.last_error = stopped ? l10n::Get(l10n::StringId::OperationCancelled)
                : format::FileOperationErrorText(status.failure_hr) + L" | " + path;
            ++status.completed_ops;
        });
    };
    const auto completed = [&] {
        SetStatus([&](OpStatus& status) {
            status.type = input.type; status.task_id = task_id;
            status.active = false; status.phase = OpPhase::Completed; status.percent = 100.0f;
            status.failure_hr = S_OK; status.failed_path.clear(); status.last_error.clear();
            status.summary = request.undo_label;
            ++status.completed_ops;
        });
    };
    SetStatus([&](OpStatus& status) {
        status.type = input.type; status.task_id = task_id; status.active = true;
        status.phase = OpPhase::Scanning; status.percent = -1.0f;
        status.summary = request.undo_label;
    });
    const auto create = [&](const OpRequest& req) {
        if (cancelled()) { failure(ERROR_CANCELLED, req.sources.front()); return false; }
        std::wstring error;
        DestinationGuard parent;
        if (!parent.Capture(Parent(req.sources.front()), error) ||
            !parent.PinDirectories(Parent(req.sources.front()), error)) {
            failure(ERROR_FILE_INVALID, req.sources.front()); return false;
        }
        DestinationGuard::Handle handle(pulse::CreateDirectoryHandle(req.sources.front()));
        if (!handle) { failure(GetLastError(), req.sources.front()); return false; }
        FileIdentity identity;
        if (!ReadFileIdentity(handle.get(), identity)) {
            failure(ERROR_FILE_INVALID, req.sources.front()); return false;
        }
        if (req.folder_metadata && !SetFileInformationByHandle(handle.get(), FileBasicInfo,
            const_cast<FILE_BASIC_INFO*>(&*req.folder_metadata), sizeof(FILE_BASIC_INFO))) {
            const DWORD code = GetLastError();
            handle.reset();
            auto reference = FileObjectReference::Open(req.sources.front());
            DWORD ignored = 0;
            if (reference.identity == identity) reference.Delete(req.sources.front(), ignored);
            failure(code, req.sources.front()); return false;
        }
        const auto reference = FileObjectReference::Open(req.sources.front());
        if (!reference.Get() || reference.identity != identity) {
            failure(ERROR_FILE_INVALID, req.sources.front()); return false;
        }
        handle.reset();
        const std::vector<FileIdentity> identities{identity};
        const std::vector<FileObjectReference> references{reference};
        PushUndo(req, nullptr, &identities, &references);
        return true;
    };
    if (input.folder_action == FolderAction::RemoveCreatedFolder) {
        bool valid = false;
        {
            std::lock_guard lock(mutex_);
            valid = input.is_undo && input.undo_revision == undo_revision_ &&
                pending_mutation_undo_revision_ == input.undo_revision && !undo_.empty() &&
                undo_.back().type == OpType::CreateFolder && undo_.back().sources == input.sources &&
                undo_.back().produced_identities == input.undo_produced_identities &&
                input.sources.size() == 1 && input.undo_produced_identities.size() == 1;
        }
        if (!valid) { failure(ERROR_INVALID_DATA, L""); return; }
        const auto& folder = input.sources.front();
        auto reference = FileObjectReference::Open(folder);
        DWORD error = ERROR_FILE_INVALID;
        if (reference.identity != input.undo_produced_identities.front() || !reference.Delete(folder, error)) {
            failure(error, folder); return;
        }
        PushUndo(input);
        completed();
        return;
    }
    if (input.folder_action == FolderAction::RestoreEmptyFolder) {
        bool valid = false;
        {
            std::lock_guard lock(mutex_);
            valid = input.is_undo && input.undo_revision == undo_revision_ &&
                pending_mutation_undo_revision_ == input.undo_revision && !undo_.empty() &&
                undo_.back().type == OpType::RemoveEmptyFolder && undo_.back().sources == input.sources &&
                input.sources.size() == 1;
        }
        if (!valid) { failure(ERROR_INVALID_DATA, L""); return; }
        if (create(input)) completed();
        return;
    }
    if (input.type != OpType::Move || input.sources.empty() || input.dest_dir.empty() ||
        (input.folder_action != FolderAction::GroupSelection && input.folder_action != FolderAction::DissolveFolders) ||
        input.is_undo || std::any_of(input.sources.begin(), input.sources.end(), [&](const auto& source) {
            return path::IsDriveRootPath(source) || _wcsicmp(Parent(source).c_str(), input.dest_dir.c_str());
        })) {
        failure(ERROR_INVALID_PARAMETER, input.dest_dir); return;
    }
    if (input.folder_action == FolderAction::GroupSelection) {
        if (input.sources.size() < 2 || !ValidName(input.new_name)) {
            failure(ERROR_INVALID_NAME, input.new_name); return;
        }
        OpRequest make;
        make.type = OpType::CreateFolder; make.sources = {Join(input.dest_dir, input.new_name)};
        make.undo_group = request.undo_group; make.undo_label = request.undo_label;
        if (!create(make)) return;
        request.folder_action = FolderAction::None;
        request.dest_dir = make.sources.front();
        if (cancelled()) { failure(ERROR_CANCELLED, request.dest_dir); return; }
        RunTransfer(request, task_id);
        return;
    }
    for (const auto& original : input.sources) {
        if (cancelled()) { failure(ERROR_CANCELLED, original); return; }
        std::wstring folder = original;
        auto reference = FileObjectReference::Open(folder);
        FILE_BASIC_INFO basic{};
        if (!reference.Get() || !GetFileInformationByHandleEx(reference.Get(), FileBasicInfo, &basic, sizeof(basic)) ||
            !(basic.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (basic.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
            failure(ERROR_INVALID_DATA, folder); return;
        }
        auto container = std::make_shared<DestinationGuard>(transfer_identity_query_);
        std::wstring guard_error;
        if (!container->PinPlainDirectory(folder, reference.identity, guard_error)) {
            failure(ERROR_FILE_INVALID, folder); return;
        }
        std::vector<std::wstring> names;
        DWORD error = ERROR_SUCCESS;
        if (!Children(folder, names, cancelled, error)) { failure(error, folder); return; }
        const auto leaf = Leaf(folder);
        if (std::any_of(names.begin(), names.end(), [&](const auto& name) { return !_wcsicmp(name.c_str(), leaf.c_str()); })) {
            std::wstring aside;
            for (unsigned number = 2; number != 100000; ++number) {
                aside = leaf + L" (" + std::to_wstring(number) + L")";
                if (GetFileAttributesW(Join(input.dest_dir, aside).c_str()) == INVALID_FILE_ATTRIBUTES &&
                    GetLastError() == ERROR_FILE_NOT_FOUND) break;
                aside.clear();
            }
            if (aside.empty()) { failure(ERROR_ALREADY_EXISTS, folder); return; }
            container->ReleaseDirectory(folder);
            OpRequest rename;
            rename.type = OpType::Rename; rename.sources = {folder}; rename.new_name = aside;
            rename.undo_group = request.undo_group; rename.undo_label = request.undo_label;
            RunShellOp(rename, task_id, &reference.identity, false, nullptr, container.get());
            if (Status().phase != OpPhase::Completed) return;
            folder = Join(input.dest_dir, aside);
            container->ForgetLeaf(original);
            if (!container->PinPlainDirectory(folder, reference.identity, guard_error)) {
                failure(ERROR_FILE_INVALID, folder); return;
            }
        }
        if (!names.empty()) {
            OpRequest move;
            move.type = OpType::Move; move.dest_dir = input.dest_dir;
            move.collision_policy = CollisionPolicy::KeepBoth;
            move.undo_group = request.undo_group; move.undo_label = request.undo_label;
            for (const auto& name : names) move.sources.push_back(Join(folder, name));
            // Source and destination must share their directory leases: the
            // transfer temporarily admits adding a name to its pinned parent.
            RunTransfer(move, task_id, {}, {}, container);
            transfer_active_.store(true);
            if (Status().phase != OpPhase::Completed) return;
        }
        if (cancelled()) { failure(ERROR_CANCELLED, folder); return; }
        container->ReleaseDirectories();
        if (before_source_cleanup_) before_source_cleanup_(folder);
        // Handle-based disposition only removes an empty, identical directory.
        // A failed/skipped move or a newly arriving child can never be recycled.
        if (!reference.Delete(folder, error)) { failure(error, folder); return; }
        OpRequest remove;
        remove.type = OpType::RemoveEmptyFolder; remove.sources = {folder};
        remove.undo_group = request.undo_group; remove.undo_label = request.undo_label;
        remove.folder_metadata = basic;
        const std::vector<FileIdentity> removed_identities{reference.identity};
        PushUndo(remove, nullptr, &removed_identities);
    }
    completed();
}
} // namespace pulse::ops
