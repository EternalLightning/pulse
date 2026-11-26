// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

// ops_manager.cpp — See ops_manager.h for the contract.
#include "ops_manager_internal.h"
#include "operation_elevation_adapter.h"
#include "operation_presentation.h"
#include "delete_operation.h"
#include "produced_undo.h"
#include "shell_command.h"
#include "drive_label_backend.h"
#include "../fs/fs_recycle.h"
#include "../fs/fs_enum.h"
#include "../ipc/shell_client.h"
#include "../common/path_utils.h"
#include "../common/file_create.h"
#include "../common/file_name.h"
#include "../common/localization.h"
#include "../common/file_operation_error.h"
#include <objbase.h>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iterator>
#include <string_view>
#include <thread>
#include <vector>

namespace pulse::ops {

namespace {

constexpr size_t kFinishedTaskLimit = 64;

std::wstring FileName(const std::wstring& path) {
    std::wstring_view v = path;
    if (v.size() > 1 && v.back() == L'\\') v.remove_suffix(1);
    auto pos = v.find_last_of(L"\\/");
    if (pos != std::wstring_view::npos) return std::wstring(v.substr(pos + 1));
    return std::wstring(v);
}

bool HasBlockedDriveSource(const OpRequest& req) {
    const bool deletion = req.type == OpType::RecycleDelete || req.type == OpType::RealDelete;
    const bool restricted = deletion || req.type == OpType::Copy || req.type == OpType::Move ||
        req.type == OpType::BatchRename || (req.type == OpType::Rename && req.sources.size() != 1);
    if (!restricted) return false;
    if (std::any_of(req.sources.begin(), req.sources.end(), pulse::path::IsDriveRootPath)) return true;
    if (deletion) {
        for (const auto& target : req.delete_targets) {
            if (pulse::path::IsDriveRootPath(target.path) ||
                std::any_of(target.physical_paths.begin(), target.physical_paths.end(), pulse::path::IsDriveRootPath)) return true;
        }
    }
    return false;
}

std::atomic<unsigned> open_workers{0};
// Reserve one worker for each of the 32 supported windows, with room for
// bounded retirement of workers still returning from a blocked provider.
constexpr unsigned kOpenWorkerLimit = 64;

bool ReserveOpenWorker() {
    unsigned current = open_workers.load();
    while (current < kOpenWorkerLimit) {
        if (open_workers.compare_exchange_weak(current, current + 1)) return true;
    }
    return false;
}

std::wstring ParentOf(const std::wstring& path) {
    std::wstring p = path;
    while (p.size() > 1 && (p.back() == L'\\' || p.back() == L'/')) p.pop_back();

    std::wstring prefix;
    std::wstring_view core = p;
    if (p.starts_with(L"\\\\?\\UNC\\")) {
        prefix = L"\\\\?\\UNC\\";
        core.remove_prefix(8);
    } else if (p.starts_with(L"\\\\?\\")) {
        prefix = L"\\\\?\\";
        core.remove_prefix(4);
    } else if (p.starts_with(L"\\\\")) {
        prefix = L"\\\\";
        core.remove_prefix(2);
    }

    std::wstring temp(core);
    const auto pos = temp.find_last_of(L"\\/");
    if (pos == std::wstring::npos || pos == 0) return path;
    temp.resize(pos);
    if (temp.size() == 2 && temp[1] == L':') temp += L'\\';
    return prefix + temp;
}

std::wstring JoinPath(const std::wstring& dir, const std::wstring& name) {
    if (dir.empty()) return name;
    if (dir.back() == L'\\') return dir + name;
    return dir + L"\\" + name;
}

std::wstring FixedPath(std::wstring_view path) {
    auto plain = pulse::path::StripExtendedPathPrefix(path);
    if (plain.size() < 3 || plain[1] != L':' || plain[2] != L'\\') return {};
    const wchar_t root[]{plain[0], L':', L'\\', L'\0'};
    return GetDriveTypeW(root) == DRIVE_FIXED ? plain : std::wstring{};
}

std::wstring ExistingFixedPath(const std::wstring& path) {
    if (FixedPath(path).empty()) return {};
    const auto handle = CreateFileW(path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return {};
    std::wstring canonical(32768, L'\0');
    const auto n = GetFinalPathNameByHandleW(handle, canonical.data(), static_cast<DWORD>(canonical.size()),
        FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    CloseHandle(handle);
    if (!n || n >= canonical.size()) return {};
    canonical.resize(n);
    return FixedPath(canonical);
}

bool SameOrChild(std::wstring_view path, std::wstring_view parent) {
    if (path.size() < parent.size()) return false;
    if (path.size() != parent.size() && !parent.ends_with(L'\\') && path[parent.size()] != L'\\') return false;
    const auto length = static_cast<int>(parent.size());
    return CompareStringOrdinal(path.data(), length, parent.data(), length, TRUE) == CSTR_EQUAL;
}

std::wstring IndependentFolderPath(const std::wstring& folder, const OpRequest& copy) {
    const auto name = FileName(folder);
    if (!pulse::path::IsValidFileName(name)) return {};
    const auto parent = ExistingFixedPath(ParentOf(folder));
    const auto destination = ExistingFixedPath(copy.dest_dir);
    if (parent.empty() || destination.empty()) return {};
    const auto path = JoinPath(parent, name);
    const auto overlaps = [&](const auto& other) { return SameOrChild(path, other) || SameOrChild(other, path); };
    for (const auto& source : copy.sources) {
        const auto input = ExistingFixedPath(source);
        if (input.empty() || overlaps(input)) return {};
        const auto output = JoinPath(destination, FileName(source));
        if (overlaps(output)) return {};
        const auto existing = ExistingFixedPath(output);
        if (!existing.empty() && overlaps(existing)) return {};
    }
    return L"\\\\?\\" + path;
}

std::wstring DisplayPath(const std::wstring& path) {
    return pulse::path::StripExtendedPathPrefix(path);
}

const std::wstring& OpVerb(OpType t) {
    using I = l10n::StringId;
    switch (t) {
    case OpType::Copy: return l10n::Get(I::Copy);
    case OpType::Move: return l10n::Get(I::OperationMove);
    case OpType::RecycleDelete: return l10n::Get(I::Delete);
    case OpType::RealDelete: return l10n::Get(I::PermanentDelete);
    case OpType::Rename: return l10n::Get(I::Rename);
    case OpType::CreateFolder: return l10n::Get(I::NewFolder);
    case OpType::CreateTextFile: return l10n::Get(I::NewTextDocument);
    case OpType::RestoreRecycle: return l10n::Get(I::Restore);
    case OpType::EmptyRecycle: return l10n::Get(I::EmptyRecycleBin);
    case OpType::BatchRename: return l10n::Get(I::BatchRename);
    case OpType::RemoveEmptyFolder: return l10n::Get(I::UngroupFolder);
    }
    return l10n::Get(I::OperationFailedMessage);
}

std::wstring Describe(const OpRequest& r) {
    std::wstring s = OpVerb(r.type);
    if (r.type == OpType::EmptyRecycle) return s;
    s += L" ";
    if (!r.sources.empty()) s += FileName(r.sources.front());
    if (r.sources.size() > 1) {
        const auto& pattern = l10n::Get(l10n::StringId::OperationItemsFormat);
        const auto first = s;
        const auto count = static_cast<unsigned long long>(r.sources.size());
        const int length = _scwprintf(pattern.c_str(), first.c_str(), count);
        if (length > 0) {
            std::vector<wchar_t> text(static_cast<size_t>(length) + 1);
            swprintf_s(text.data(), text.size(), pattern.c_str(), first.c_str(), count);
            s.assign(text.data(), static_cast<size_t>(length));
        }
    }
    if (r.type == OpType::Copy || r.type == OpType::Move) {
        s += L" → " + DisplayPath(r.dest_dir);
    } else if (r.type == OpType::Rename) {
        s += L" → " + r.new_name;
    }
    return s;
}

bool IsFinishedTask(const OpStatus& status) {
    return !status.active && (status.phase == OpPhase::Completed ||
        status.phase == OpPhase::Failed || status.phase == OpPhase::Cancelled);
}

void SetTaskPaths(OpStatus& status, const OpRequest& request) {
    status.source_label = !request.delete_targets.empty() ? request.delete_targets.front().path :
        request.sources.empty() ? L"" : request.sources.front();
    status.destination_label = request.type == OpType::Rename ? request.new_name : request.dest_dir;
}

OpStatus QueuedTaskStatus(const OpRequest& request, uint64_t task_id) {
    OpStatus status;
    status.type = request.type;
    status.active = false;
    status.phase = OpPhase::Queued;
    status.task_id = task_id;
    status.last_error.clear(); status.failed_path.clear(); status.failure_hr = S_OK;
    status.elevation_retry_available = status.lock_retry_available = false;
    status.locked_path.clear(); status.lock_owners.clear();
    status.summary = Describe(request);
    SetTaskPaths(status, request);
    status.total_items = !request.delete_targets.empty() ? request.delete_targets.size() : request.sources.size();
    return status;
}

OpStatus TaskStatusWithPaths(OpStatus status, const OpRequest& request) {
    SetTaskPaths(status, request);
    return status;
}

bool PathExists(const std::wstring& path, bool* directory = nullptr) {
    const DWORD attrs = GetFileAttributesW(path.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) return false;
    if (directory) *directory = (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
    return true;
}

std::wstring Win32Message(DWORD code) {
    wchar_t* raw = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                       FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, code, 0, reinterpret_cast<wchar_t*>(&raw), 0, nullptr);
    std::wstring text = raw ? raw : l10n::Get(l10n::StringId::OperationFileFailed);
    if (raw) LocalFree(raw);
    while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n')) text.pop_back();
    return text;
}

std::wstring UniqueCopyPath(const std::wstring& destination, bool directory) {
    if (!PathExists(destination)) return destination;
    const std::wstring parent = ParentOf(destination);
    const std::wstring leaf = FileName(destination);
    std::wstring stem = leaf;
    std::wstring extension;
    if (!directory) {
        const size_t dot = leaf.find_last_of(L'.');
        if (dot != std::wstring::npos && dot > 0) {
            stem = leaf.substr(0, dot);
            extension = leaf.substr(dot);
        }
    }
    for (unsigned index = 1; index < 10000; ++index) {
        std::wstring name = l10n::Format(l10n::StringId::OperationCopyNameFormat, stem.c_str());
        if (index > 1) name += L" (" + std::to_wstring(index) + L")";
        std::wstring candidate = JoinPath(parent, name + extension);
        if (!PathExists(candidate)) return candidate;
    }
    return JoinPath(parent, l10n::Format(l10n::StringId::OperationCopyUniqueNameFormat, stem.c_str(), static_cast<unsigned long long>(GetTickCount64())) + extension);
}

bool StartsWithPath(const std::wstring& path, const std::wstring& prefix) {
    if (path.size() < prefix.size() || _wcsnicmp(path.c_str(), prefix.c_str(), prefix.size()) != 0)
        return false;
    return path.size() == prefix.size() || path[prefix.size()] == L'\\' || path[prefix.size()] == L'/';
}

} // namespace

void OpsManager::Start(std::function<void()> notify) {
    if (running_) return;
    notify_ = std::move(notify);
    stopping_ = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        running_ = true;
        accepting_ = true;
    }
    {
        std::lock_guard<std::mutex> lock(menu_mutex_);
        menu_running_ = true;
    }
    const bool open_worker_reserved = ReserveOpenWorker();
    {
        auto mailbox = std::make_shared<OpenMailbox>();
        mailbox->running = open_worker_reserved;
        mailbox->owner.store(ui_hwnd_.load());
        {
            std::lock_guard<std::mutex> lock(open_mutex_);
            open_mailbox_ = mailbox;
        }
    }
    StartShellClient();
    thread_ = std::thread([this] { WorkerThread(); });
    menu_thread_ = std::thread([this] { MenuThread(); });
    if (const auto mailbox = open_mailbox_; mailbox && open_worker_reserved) {
        try {
            open_thread_ = std::thread([mailbox] { OpenThread(mailbox); });
        } catch (...) {
            std::lock_guard<std::mutex> lock(open_mutex_);
            open_mailbox_->running = false;
            open_workers.fetch_sub(1);
        }
    }
}

bool OpsManager::HasPendingFileOperations() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return file_operation_in_flight_ || concurrent_folder_status_ || std::any_of(queue_.begin(), queue_.end(),
        [](const QueueItem& item) { return item.open_path.empty(); });
}

void OpsManager::BeginShutdown() {
    bool undo_released = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        accepting_ = false;
        running_ = false;
        for (const auto& item : queue_) {
            undo_released = FinishMutationUndoLocked(item.req) || undo_released;
            undo_released = FinishRecycleUndoLocked(item.req) || undo_released;
        }
        queue_.clear();
        scheduled_recovery_.clear();
    }
    if (undo_released && notify_) notify_();
    {
        std::lock_guard<std::mutex> lock(delete_wait_mutex_);
        stopping_.store(true);
    }
    {
        std::lock_guard<std::mutex> lock(open_mutex_);
        if (open_mailbox_) {
            std::lock_guard<std::mutex> jobs(open_mailbox_->mutex);
            open_mailbox_->owner.store(nullptr);
            open_mailbox_->running = false;
            open_mailbox_->queue.clear();
            open_mailbox_->cv.notify_all();
        }
    }
    ui_hwnd_.store(nullptr);
    CancelCurrent();
    transfer_control_cv_.notify_all();
    delete_wait_cv_.notify_all();
    done_cv_.notify_all();
    cv_.notify_all();
}

void OpsManager::Stop() {
    if (!thread_.joinable() && !menu_thread_.joinable() && !open_thread_.joinable()) {
        ReleaseShellClients(open_mailbox_);
        return;
    }
    {
        std::lock_guard<std::mutex> lock(delete_wait_mutex_);
        stopping_ = true;
    }
    CancelCurrent(); // Release a deletion presenter before waiting on other threads.
    delete_wait_cv_.notify_all();
    {
        std::lock_guard<std::mutex> lock(menu_mutex_);
        menu_running_ = false;
    }
    menu_cv_.notify_all();
    if (menu_thread_.joinable()) menu_thread_.join();
    std::shared_ptr<OpenMailbox> mailbox;
    {
        std::lock_guard<std::mutex> lock(open_mutex_);
        mailbox = std::move(open_mailbox_);
    }
    if (mailbox) {
        std::unique_lock<std::mutex> lock(mailbox->mutex);
        mailbox->running = false;
        mailbox->owner.store(nullptr);
        mailbox->queue.clear();
        mailbox->cv.notify_all();
        mailbox->finished_cv.wait_for(lock, std::chrono::milliseconds(200), [&] { return mailbox->finished; });
        const bool finished = mailbox->finished;
        lock.unlock();
        if (open_thread_.joinable()) {
            if (finished) open_thread_.join();
            else open_thread_.detach(); // static worker owns mailbox, never this
        }
    } else if (open_thread_.joinable()) open_thread_.join();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        running_ = false;
        concurrent_folder_status_.reset();
    }
    CancelCurrent(); // unblock an in-flight RunShellOp via DONE(cancelled)
    cv_.notify_all();
    done_cv_.notify_all();
    if (thread_.joinable()) thread_.join();
    ReleaseShellClients(mailbox);
    {
        std::lock_guard lock(done_mutex_);
        done_results_.clear();
    }
    PersistJournal();
}

uint64_t OpsManager::Submit(OpRequest req) {
    if (HasBlockedDriveSource(req)) return 0;
    QueueItem item;
    item.req = std::move(req);
    uint64_t seq = 0;
    {
        std::scoped_lock lock(mutex_, menu_mutex_);
        if (!accepting_) return 0;
        seq = next_seq_++;
        item.seq = seq;
        if (item.req.type == OpType::CreateFolder && item.req.sources.size() == 1 &&
            item.req.folder_action == FolderAction::None && !item.req.is_undo && !item.req.undo_group &&
            !item.req.folder_metadata && menu_running_ && !concurrent_folder_status_ && queue_.empty() &&
            active_item_ && active_item_->req.type == OpType::Copy && transfer_active_.load() &&
            active_item_->req.sources.size() <= 32) {
            MenuJob job;
            job.kind = MenuJob::Kind::CreateFolder;
            auto status = QueuedTaskStatus(item.req, item.seq);
            job.create_folder = [this, item = std::move(item), copy = active_item_->req]() mutable {
                CreateFolderDuringCopy(std::move(item), copy);
            };
            menu_queue_.push_front(std::move(job));
            concurrent_folder_status_ = std::move(status);
            menu_cv_.notify_one();
        } else queue_.push_back(std::move(item));
    }
    if (notify_) notify_();
    cv_.notify_one();
    return seq;
}

void OpsManager::CreateFolderDuringCopy(QueueItem item, const OpRequest& copy) {
    FileIdentity identity;
    bool created = false;
    if (!stopping_.load()) {
        const auto path = IndependentFolderPath(item.req.sources.front(), copy);
        if (!path.empty() && !stopping_.load()) {
            if (const HANDLE handle = pulse::CreateDirectoryHandle(path)) {
                created = true;
                ReadFileIdentity(handle, identity);
                CloseHandle(handle);
            }
        }
    }
    if (created) {
        const std::vector<FileIdentity> identities{identity};
        PushUndo(item.req, &item.req.sources, &identities, nullptr, item.seq);
    }
    {
        std::lock_guard lock(mutex_);
        concurrent_folder_status_.reset();
        if (created) {
            auto done = QueuedTaskStatus(item.req, item.seq);
            done.phase = OpPhase::Completed;
            done.summary = l10n::Get(l10n::StringId::OpCompleted);
            done.completed_items = 1;
            done.percent = 100.0f;
            finished_tasks_.push_back(std::move(done));
            if (finished_tasks_.size() > kFinishedTaskLimit) finished_tasks_.pop_front();
            ++status_.completed_ops;
        } else if (accepting_ && !stopping_.load()) {
            queue_.push_front(std::move(item));
        }
    }
    cv_.notify_one();
    if (notify_) notify_();
}

void OpsManager::OpenWith(const std::wstring& path, std::function<ImageOpenContext()> image_context) {
    QueueItem item;
    item.open_path = path;
    item.open_verb = L"open";
    item.image_context = std::move(image_context);
    EnqueueOpen(std::move(item));
}

void OpsManager::RevealInExplorer(const std::wstring& path) {
    QueueItem item;
    item.open_path = path;
    item.open_verb = L"__reveal";
    EnqueueOpen(std::move(item));
}

std::vector<std::wstring> OpsManager::DrainShellLaunchErrors() {
    std::shared_ptr<OpenMailbox> mailbox;
    {
        std::lock_guard lock(open_mutex_);
        mailbox = open_mailbox_;
    }
    std::vector<std::wstring> errors;
    if (mailbox) {
        std::lock_guard lock(mailbox->mutex);
        errors.assign(std::make_move_iterator(mailbox->errors.begin()), std::make_move_iterator(mailbox->errors.end()));
        mailbox->errors.clear();
    }
    return errors;
}

void OpsManager::ShowProperties(const std::wstring& path) {
    ExecuteVerb(path, L"properties");
}

void OpsManager::ShowProperties(const std::vector<std::wstring>& paths) {
    if (paths.empty()) return;
    if (paths.size() == 1) {
        ShowProperties(paths.front());
        return;
    }
    QueueItem item;
    item.open_path = paths.front();
    item.open_paths = paths;
    item.open_verb = L"properties";
    EnqueueOpen(std::move(item));
}

void OpsManager::ExecuteVerb(const std::wstring& path, const std::wstring& verb) {
    QueueItem item;
    item.open_path = path;
    item.open_verb = verb.empty() ? L"open" : verb;
    // 打开方式… / 属性 are interactive dialogs: they must answer the click even
    // when a slow open is still in flight, so they go to the front of the queue.
    EnqueueOpen(std::move(item), true);
}

void OpsManager::OpenWithApp(const std::wstring& app_exe, const std::wstring& file) {
    QueueItem item;
    item.open_path = ParentOf(file);      // lpDirectory
    item.open_file = app_exe;
    item.open_verb = L"open";
    item.open_args = QuoteWindowsArgument(file);
    EnqueueOpen(std::move(item));
}

void OpsManager::ExecuteCommand(const std::wstring& command, const std::wstring& path) {
    std::wstring expanded = ExpandShellCommand(command, path);
    if (expanded.empty()) return;
    QueueItem item;
    item.open_path = ParentOf(path);
    item.open_file = expanded;
    item.open_verb = L"__cmdline";
    EnqueueOpen(std::move(item));
}

std::wstring TerminalCommandLine(const std::wstring& dir) {
    return L"-d " + QuoteWindowsArgument(dir);
}

void OpsManager::OpenTerminal(const std::wstring& dir) {
    if (dir.empty() || fs::IsVirtualPath(dir)) return;
    QueueItem item;
    item.open_path = dir;
    item.open_file = L"wt.exe";
    item.open_verb = L"open";
    item.open_args = TerminalCommandLine(dir);
    EnqueueOpen(std::move(item));
}

void OpsManager::CancelCurrent() {
    bool undo_released = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        lock_retry_generation_.fetch_add(1);
        lock_retry_cancel_.store(true);
        if (transfer_active_.load()) {
            transfer_cancel_.store(true);
            transfer_pause_.store(false);
        }
        lock_retry_.reset();
        elevation_retry_.reset();
        status_.elevation_retry_available = false;
        // A retry already dequeued retains its generation; a queued one is
        // revoked here. Neither can be revived by the worker's next reset.
        std::erase_if(queue_, [&](const QueueItem& item) {
            if (!item.lock_retry && !item.elevated_retry) return false;
            undo_released = FinishMutationUndoLocked(item.req) || undo_released;
            undo_released = FinishRecycleUndoLocked(item.req) || undo_released;
            return true;
        });
    }
    if (undo_released && notify_) notify_();
    if (delete_active_.load()) {
        {
            std::lock_guard<std::mutex> lock(delete_wait_mutex_);
            delete_cancel_.store(true);
            delete_service_.Cancel();
        }
        delete_wait_cv_.notify_all();
    }
    if (transfer_active_.load()) {
        transfer_cancel_.store(true);
        transfer_pause_.store(false);
        transfer_control_cv_.notify_all();
        SetStatus([](OpStatus& status) {
            if (status.active) status.phase = OpPhase::Cancelling;
        });
    }
    shell_cancel_requested_ = true;
    uint32_t id = current_req_id_.load();
    if (id != 0) ipc::ShellClient::Instance().Cancel(id);
}

void OpsManager::PauseCurrent() {
    PauseTask(Status().task_id);
}

void OpsManager::ResumeCurrent() {
    ResumeTask(Status().task_id);
}

bool OpsManager::MatchesActiveTransferLocked(uint64_t task_id) const {
    return running_ && accepting_ && task_id && active_item_ && active_item_->seq == task_id &&
        status_.task_id == task_id && status_.active && transfer_active_.load() &&
        (active_item_->req.type == OpType::Copy || active_item_->req.type == OpType::Move);
}

bool OpsManager::CancelTask(uint64_t task_id) {
    uint32_t request = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!MatchesActiveTransferLocked(task_id) || transfer_cancel_.load()) return false;
        transfer_cancel_.store(true);
        transfer_pause_.store(false);
        status_.phase = OpPhase::Cancelling;
        request = current_req_id_.load();
    }
    transfer_control_cv_.notify_all();
    if (request) ipc::ShellClient::Instance().Cancel(request);
    if (notify_) notify_();
    return true;
}

bool OpsManager::PauseTask(uint64_t task_id) {
    uint32_t request = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!MatchesActiveTransferLocked(task_id) || transfer_cancel_.load()) return false;
        transfer_pause_.store(true);
        status_.phase = OpPhase::Paused;
        request = current_req_id_.load();
    }
    if (request) ipc::ShellClient::Instance().SetFileOperationPaused(request, true);
    if (notify_) notify_();
    return true;
}

bool OpsManager::ResumeTask(uint64_t task_id) {
    uint32_t request = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!MatchesActiveTransferLocked(task_id) || transfer_cancel_.load()) return false;
        transfer_pause_.store(false);
        status_.phase = OpPhase::Running;
        request = current_req_id_.load();
    }
    transfer_control_cv_.notify_all();
    if (request) ipc::ShellClient::Instance().SetFileOperationPaused(request, false);
    if (notify_) notify_();
    return true;
}

std::optional<ConflictItemInfo> OpsManager::PendingConflict() const {
    std::lock_guard<std::mutex> lock(transfer_control_mutex_);
    return pending_conflict_;
}

void OpsManager::ResolveConflict(uint64_t token, ConflictChoice choice, bool apply_to_all) {
    {
        std::lock_guard<std::mutex> lock(transfer_control_mutex_);
        if (!pending_conflict_ || pending_conflict_->token != token || resolved_conflict_token_ == token) return;
        resolved_conflict_token_ = token;
        resolved_conflict_choice_ = choice;
        resolved_conflict_apply_all_ = apply_to_all;
        resolved_conflict_rows_.clear();
    }
    transfer_control_cv_.notify_all();
}

bool OpsManager::ResolveConflicts(uint64_t token, const std::vector<ConflictResolution>& rows) {
    {
        std::lock_guard<std::mutex> lock(transfer_control_mutex_);
        if (!pending_conflict_ || pending_conflict_->token != token || pending_conflict_->link_confirmation ||
            transfer_cancel_.load() || stopping_.load() || resolved_conflict_token_ == token ||
            !ValidConflictResolutions(pending_conflict_->rows, rows)) return false;
        resolved_conflict_rows_ = rows;
        resolved_conflict_token_ = token;
        resolved_conflict_apply_all_ = false;
    }
    transfer_control_cv_.notify_all();
    return true;
}

std::vector<TransferOutcome> OpsManager::DrainTransferOutcomes() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<TransferOutcome> outcomes;
    outcomes.reserve(transfer_outcomes_.size());
    std::move(transfer_outcomes_.begin(), transfer_outcomes_.end(), std::back_inserter(outcomes));
    transfer_outcomes_.clear();
    return outcomes;
}

bool OpsManager::CanResumeTransfer(uint64_t task_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return running_ && accepting_ && std::any_of(incomplete_transfers_.begin(), incomplete_transfers_.end(),
        [&](const auto& item) { return item.seq == task_id; });
}

uint64_t OpsManager::ResumeIncompleteTransfer(uint64_t task_id) {
    uint64_t submitted = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_ || !accepting_) return 0;
        const auto found = std::find_if(incomplete_transfers_.begin(), incomplete_transfers_.end(),
            [&](const auto& item) { return item.seq == task_id; });
        if (found == incomplete_transfers_.end()) return 0;
        QueueItem resumed = std::move(*found);
        incomplete_transfers_.erase(found);
        submitted = resumed.seq = next_seq_++;
        queue_.push_back(std::move(resumed));
    }
    if (notify_) notify_();
    cv_.notify_one();
    return submitted;
}

OpStatus OpsManager::Status() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return PresentOperationStatus(status_);
}

std::vector<OpStatus> OpsManager::TaskStatuses() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<OpStatus> tasks;
    tasks.reserve(finished_tasks_.size() + queue_.size() + 1);
    const auto present_current = [&](const OpStatus& current) {
        return PresentOperationStatus(active_item_ && active_item_->seq == current.task_id
            ? TaskStatusWithPaths(current, active_item_->req) : current);
    };
    const bool continuing = false;
    for (const auto& finished : finished_tasks_) {
        const bool superseded = (finished.task_id == status_.task_id && (!IsFinishedTask(status_) || continuing)) ||
            std::any_of(queue_.begin(), queue_.end(), [&](const auto& item) { return item.seq == finished.task_id; });
        if (!superseded) tasks.push_back(PresentOperationStatus(finished));
    }
    if (dequeued_task_status_) {
        const auto& current = status_.task_id == dequeued_task_status_->task_id ? status_ : *dequeued_task_status_;
        if (!IsFinishedTask(current) || continuing) tasks.push_back(present_current(current));
    } else if (status_.task_id && (!IsFinishedTask(status_) || continuing)) {
        tasks.push_back(present_current(status_));
    }
    if (concurrent_folder_status_) tasks.push_back(PresentOperationStatus(*concurrent_folder_status_));
    for (const auto& item : queue_) {
        if (item.open_path.empty() && item.seq)
            tasks.push_back(PresentOperationStatus(QueuedTaskStatus(item.req, item.seq)));
    }
    return tasks;
}

void OpsManager::ClearFinishedTasks() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        finished_tasks_.clear();
        incomplete_transfers_.clear();
    }
    if (notify_) notify_();
}

bool OpsManager::IsLockedFailureCurrent(uint64_t task_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return running_ && accepting_ && lock_retry_ && lock_retry_->seq == task_id &&
        status_.task_id == task_id && !status_.active && status_.phase == OpPhase::Failed;
}

bool OpsManager::RetryLockedOperation(uint64_t task_id, bool end_owners) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_ || !accepting_ || !lock_retry_ || lock_retry_->seq != task_id ||
            status_.task_id != task_id || status_.active || status_.phase != OpPhase::Failed) return false;
        if (end_owners && lock_retry_->req.type == OpType::Rename) return false;
        if (end_owners && (status_.lock_owners.empty() || std::any_of(status_.lock_owners.begin(),
                status_.lock_owners.end(), [](const auto& owner) { return !owner.can_terminate; }))) return false;
        QueueItem retry = *lock_retry_;
        if (retry.req.is_undo && IsMutationOperation(retry.req.type)) {
            if (pending_mutation_undo_revision_ || retry.req.undo_revision != undo_revision_ ||
                undo_.empty() || !undo_.back().supported) return false;
            pending_mutation_undo_revision_ = retry.req.undo_revision;
        }
        retry.seq = next_seq_++;
        retry.lock_retry = true;
        retry.retry_generation = lock_retry_generation_.load();
        retry.recovery_sequence = IsDeleteOperation(retry.req.type) ? task_id : 0;
        if (end_owners) retry.close_first = status_.lock_owners;
        lock_retry_.reset();
        queue_.push_front(std::move(retry));
    }
    cv_.notify_one();
    return true;
}

LockReport OpsManager::CaptureLockedFailure(const QueueItem& retry, HRESULT error,
    const std::wstring& failed_item, const LockCancelled& cancelled) {
    const auto generation = lock_retry_generation_.load();
    LockReport report = ProbeFileLocks(error, failed_item, retry.req.sources, LockProtectedDirectory(), cancelled);
    const bool plain_transfer_retry = (retry.req.type == OpType::Copy || retry.req.type == OpType::Move ||
        retry.req.type == OpType::Rename || retry.req.type == OpType::BatchRename) && IsLockFailure(error);
    if ((!plain_transfer_retry && report.owners.empty()) || (cancelled && cancelled())) return {};
    std::lock_guard<std::mutex> lock(mutex_);
    if (!running_ || stopping_.load() || generation != lock_retry_generation_.load() ||
        (cancelled && cancelled())) return {};
    lock_retry_ = retry;
    lock_retry_->retry_failure_hr = error;
    lock_retry_->retry_failed_path = failed_item;
    return report;
}

bool OpsManager::PrepareLockRetry(QueueItem& item) {
    if (!item.lock_retry) return true;
    lock_retry_active_.store(true);
    SetStatus([&](OpStatus& status) {
        status.active = true; status.task_id = item.seq; status.type = item.req.type;
        status.phase = OpPhase::Running; status.summary = Describe(item.req);
        status.last_error.clear(); status.locked_path.clear(); status.lock_owners.clear();
        status.failure_hr = S_OK; status.failed_path.clear();
    });
    const auto cancelled = [&] {
        return stopping_.load() || lock_retry_cancel_.load() ||
            item.retry_generation != lock_retry_generation_.load();
    };
    std::wstring failure;
    HRESULT end_failure_hr = S_OK;
    for (const auto& owner : item.close_first) {
        DWORD error = ERROR_SUCCESS;
        const auto result = EndLockOwner(owner, LockProtectedDirectory(), cancelled, 3000, &error);
        if (result == EndLockResult::Ended || result == EndLockResult::AlreadyGone) continue;
        if (!cancelled()) {
            const DWORD code = error == ERROR_SUCCESS ? ERROR_ACCESS_DENIED : error;
            end_failure_hr = HRESULT_FROM_WIN32(code);
            failure = LockOwnerDescription(owner) + L" | " + Win32Message(code);
        }
        break;
    }
    item.close_first.clear();
    lock_retry_active_.store(false);
    if (failure.empty() && !cancelled()) return true;
    LockReport locks;
    if (!cancelled()) {
        // Some owners may already have exited. Reprobe instead of carrying a
        // consumed list of PIDs into another termination attempt.
        locks = CaptureLockedFailure(item, item.retry_failure_hr, item.retry_failed_path, cancelled);
        std::lock_guard<std::mutex> lock(mutex_);
        if (running_ && !cancelled()) lock_retry_ = item; // plain retry remains available even if RM is empty
    }
    SetStatus([&](OpStatus& status) {
        status.active = false;
        const bool was_cancelled = cancelled();
        status.phase = was_cancelled ? OpPhase::Cancelled : OpPhase::Failed;
        status.last_error = was_cancelled ? L"" : failure;
        status.summary = was_cancelled ? l10n::Get(l10n::StringId::OperationCancelled) : failure;
        status.failure_hr = was_cancelled ? S_OK : end_failure_hr;
        status.failed_path = was_cancelled ? L"" : item.retry_failed_path;
        status.locked_path = was_cancelled ? L"" : locks.path;
        status.lock_owners = was_cancelled ? std::vector<LockOwner>{} : locks.owners;
        ++status.completed_ops;
    });
    return false;
}

bool OpsManager::RefreshProducedUndoReferences(OpRequest& request) {
    if (!request.is_undo || !request.undo_revision) return true;
    UndoEntry history;
    {
        std::lock_guard lock(mutex_);
        if (request.undo_revision != undo_revision_ || undo_.empty() || !undo_.back().session_only) return true;
        history = undo_.back();
    }
    const auto& paths = history.destinations.empty() ? history.sources : history.destinations;
    if (history.produced_references.size() != paths.size() ||
        history.produced_identities.size() != paths.size() ||
        request.undo_produced_identities.size() != request.sources.size()) return false;
    auto identities = history.produced_identities;
    auto inverse_identities = request.undo_produced_identities;
    for (size_t i = 0; i < request.sources.size(); ++i) {
        const auto found = std::find(paths.begin(), paths.end(), request.sources[i]);
        const size_t index = static_cast<size_t>(found - paths.begin());
        if (index >= paths.size() || history.produced_identities[index] != inverse_identities[i]) return false;
        const auto& reference = history.produced_references[index];
        auto current = FileObjectReference::Open(request.sources[i]);
        FileIdentity live;
        if (!reference.Get() || !current.Get() || !ReadFileIdentity(reference.Get(), live) ||
            !live.Valid() || current.identity != live) return false;
        identities[index] = inverse_identities[i] = live;
    }
    std::lock_guard lock(mutex_);
    if (request.undo_revision != undo_revision_ || undo_.empty() ||
        undo_.back().produced_identities != history.produced_identities) return false;
    undo_.back().produced_identities = std::move(identities);
    request.undo_produced_identities = std::move(inverse_identities);
    return true;
}

void OpsManager::ExecuteFileOperation(QueueItem& item) {
    struct RestoreElevation {
        bool& value;
        bool previous;
        ~RestoreElevation() { value = previous; }
    } restore_elevation{shell_elevation_authorized_, shell_elevation_authorized_};
    shell_elevation_authorized_ = item.elevated_retry;
    struct UndoCompletion {
        OpsManager& manager;
        const OpRequest& request;
        ~UndoCompletion() {
            manager.FinishMutationUndo(request);
            manager.FinishRecycleUndo(request);
            manager.ContinueFolderUndo(request);
        }
    } completion{*this, item.req};
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const bool stale = (item.lock_retry || item.elevated_retry) && item.retry_generation != lock_retry_generation_.load();
        const bool cancelled = stopping_.load() || !running_ || !accepting_ || stale;
        lock_retry_cancel_.store(cancelled);
        shell_cancel_requested_.store(cancelled);
        lock_retry_.reset();
        status_.lock_retry_available = false;
        elevation_retry_.reset();
        status_.elevation_retry_available = false;
        status_.locked_path.clear(); status_.lock_owners.clear();
        status_.failure_hr = S_OK; status_.failed_path.clear();
    }
    if (!RefreshProducedUndoReferences(item.req)) {
        SetStatus([&](OpStatus& status) {
            status.active = false; status.type = item.req.type; status.task_id = item.seq;
            status.phase = OpPhase::Failed; status.failure_hr = HRESULT_FROM_WIN32(ERROR_FILE_INVALID);
            status.failed_path = item.req.sources.empty() ? L"" : item.req.sources.front();
            status.last_error = status.summary = l10n::Format(l10n::StringId::OperationUndoResultChangedFormat,
                status.failed_path.c_str());
            ++status.completed_ops;
        });
        return;
    }
    if (item.req.is_undo && IsMutationOperation(item.req.type)) {
        bool valid = false;
        {
            std::lock_guard lock(mutex_);
            valid = item.req.undo_revision != 0 && item.req.undo_revision == undo_revision_ &&
                pending_mutation_undo_revision_ == item.req.undo_revision && !undo_.empty() &&
                undo_.back().supported && undo_.back().type == item.req.type &&
                HasProducedIdentities(item.req.sources.size(), item.req.undo_produced_identities);
            if (valid) {
                const auto& history = undo_.back();
                valid = item.req.sources.size() == history.destinations.size() &&
                    item.req.undo_cleanup_directories == history.cleanup_directories;
                for (size_t i = 0; valid && i < item.req.sources.size(); ++i) {
                    const auto found = std::find(history.destinations.begin(), history.destinations.end(), item.req.sources[i]);
                    const auto index = static_cast<size_t>(found - history.destinations.begin());
                    valid = index < history.produced_identities.size() &&
                        history.produced_identities[index] == item.req.undo_produced_identities[i];
                    if (valid && item.req.type == OpType::Move)
                        valid = i < item.req.new_names.size() && item.req.new_names[i] == history.sources[index];
                    else if (valid) {
                        const auto& name = item.req.type == OpType::Rename ? item.req.new_name
                            : i < item.req.new_names.size() ? item.req.new_names[i] : item.req.new_name;
                        valid = ParentOf(item.req.sources[i]) == ParentOf(history.sources[index]) &&
                            name == FileName(history.sources[index]);
                    }
                }
            }
        }
        if (!valid) {
            SetStatus([&](OpStatus& status) {
                status.active = false; status.type = item.req.type; status.task_id = item.seq;
                status.phase = OpPhase::Failed; status.failure_hr = E_ACCESSDENIED;
                status.last_error = status.summary = l10n::Get(l10n::StringId::OperationUndoRecordChanged);
                status.failed_path = item.req.sources.empty() ? L"" : item.req.sources.front();
                ++status.completed_ops;
            });
            return;
        }
    }
    if (HasBlockedDriveSource(item.req)) {
        SetStatus([&](OpStatus& status) {
            status.active = false; status.type = item.req.type; status.task_id = item.seq;
            status.phase = OpPhase::Failed; status.percent = -1.0f;
            status.summary = status.last_error = l10n::Get(l10n::StringId::OperationDriveUnsupported);
            status.failure_hr = E_ACCESSDENIED;
            ++status.completed_ops;
        });
        return;
    }
    if (item.req.folder_action != FolderAction::None) {
        RunFolderAction(item.req, item.seq);
        return;
    }
    if (item.elevated_retry && item.req.type == OpType::Rename && item.req.sources.size() == 1 &&
        pulse::path::IsDriveRootPath(item.req.sources.front())) {
        OperationElevationAdapter::RunParent(*this, item);
        return;
    }
    if (!PrepareLockRetry(item)) return;
    if (IsDeleteOperation(item.req.type)) RunDelete(item);
    else if (item.req.type == OpType::Copy || item.req.type == OpType::Move)
        RunTransfer(item.req, item.seq, item.retry_transfer_plan);
    else {
        std::vector<FileIdentity> continuation_identities;
        for (const auto& reference : item.retry_source_references) continuation_identities.push_back(reference.identity);
        RunShellOp(item.req, item.seq, item.retry_rename_identity ? &item.rename_source_id : nullptr, item.lock_retry,
            continuation_identities.empty() ? nullptr : &continuation_identities);
    }
}

void OpsManager::SetStatus(const std::function<void(OpStatus&)>& fn) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const bool was_finished = IsFinishedTask(status_);
        const auto previous_task = status_.task_id;
        const auto previous_completed = status_.completed_ops;
        fn(status_);
        RefreshElevationStatusLocked();
        RememberFinishedTaskLocked(!was_finished || previous_task != status_.task_id ||
            previous_completed != status_.completed_ops);
    }
    if (notify_) notify_();
}

void OpsManager::RememberFinishedTaskLocked(bool allow_new) {
    if (!status_.task_id || !IsFinishedTask(status_)) return;
    const auto snapshot = active_item_ && active_item_->seq == status_.task_id
        ? TaskStatusWithPaths(status_, active_item_->req) : status_;
    const auto found = std::find_if(finished_tasks_.begin(), finished_tasks_.end(), [&](const OpStatus& task) {
        return task.task_id == status_.task_id;
    });
    if (found != finished_tasks_.end()) {
        const auto source = found->source_label, destination = found->destination_label;
        *found = snapshot;
        if (!active_item_ || active_item_->seq != status_.task_id) {
            found->source_label = source;
            found->destination_label = destination;
        }
    }
    else if (allow_new) {
        finished_tasks_.push_back(snapshot);
        if (finished_tasks_.size() > kFinishedTaskLimit) finished_tasks_.pop_front();
    }
}

bool OpsManager::CanUndo() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (undo_.empty() || !undo_.back().supported || pending_mutation_undo_revision_ || pending_recycle_undo_revision_) return false;
    if (undo_.back().type == OpType::RecycleDelete) {
        undo_validation_requested_.store(true);
        return undo_.back().recycle_verified;
    }
    return true;
}

std::wstring OpsManager::UndoLabel() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (undo_.empty()) return L"";
    const UndoEntry& e = undo_.back();
    if (!e.supported) return L"";
    if (e.type == OpType::RecycleDelete && !e.recycle_verified) {
        undo_validation_requested_.store(true);
        return L"";
    }
    OpRequest r;
    r.type = e.type;
    r.sources = e.sources;
    r.dest_dir = e.dest_dir;
    r.new_name = e.new_name;
    if (!e.undo_label.empty()) {
        return l10n::Format(l10n::StringId::OperationUndoFormat, e.undo_label.c_str());
    }
    const auto description = Describe(r);
    const auto& pattern = l10n::Get(l10n::StringId::OperationUndoFormat);
    const int length = _scwprintf(pattern.c_str(), description.c_str());
    if (length <= 0) return {};
    std::vector<wchar_t> text(static_cast<size_t>(length) + 1);
    swprintf_s(text.data(), text.size(), pattern.c_str(), description.c_str());
    return std::wstring(text.data(), static_cast<size_t>(length));
}

void OpsManager::RefreshRecycleUndoValidity() {
    if (!record_undo_) return;
    std::vector<std::pair<size_t, UndoEntry>> entries;
    uint64_t revision = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        revision = undo_revision_;
        for (size_t i = 0; i < undo_.size(); ++i)
            if (undo_[i].type == OpType::RecycleDelete && undo_[i].supported) entries.emplace_back(i, undo_[i]);
    }
    bool changed = false;
    for (auto& [index, entry] : entries) {
        bool valid = !entry.sources.empty() && entry.destinations.size() == entry.sources.size();
        std::vector<RecycleUndoIdentity> identities(entry.sources.size());
        for (size_t i = 0; valid && i < entry.sources.size(); ++i) {
            const auto* expected = i < entry.recycle_identities.size() ? &entry.recycle_identities[i] : nullptr;
            valid = IsCurrentUserRecyclePayload(entry.sources[i], entry.destinations[i]) &&
                ValidateRecycleUndoPair(entry.sources[i], entry.destinations[i], identities[i], expected, false);
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if (revision != undo_revision_ || index >= undo_.size()) return;
        auto& current = undo_[index];
        changed = changed || current.supported != valid || current.recycle_verified != valid;
        current.supported = valid; current.recycle_verified = valid;
        if (valid) current.recycle_identities = std::move(identities);
    }
    undo_validation_requested_.store(false);
    if (changed) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (revision == undo_revision_) ++undo_revision_; // invalidate a queued inverse reservation
    }
    if (changed && notify_) notify_();
}

void OpsManager::InvalidateRecycleUndo(const std::vector<std::wstring>& changed, bool uncertain) {
    std::lock_guard<std::mutex> lock(mutex_);
    bool invalidated = false;
    for (auto& entry : undo_) {
        if (entry.type != OpType::RecycleDelete || !entry.supported) continue;
        if (uncertain || std::any_of(entry.destinations.begin(), entry.destinations.end(), [&](const auto& payload) {
            const auto index = fs::RecycleIndexPath(payload);
            return std::any_of(changed.begin(), changed.end(), [&](const auto& path) {
                return StartsWithPath(payload, path) || StartsWithPath(index, path) ||
                    StartsWithPath(path, payload) || StartsWithPath(path, index);
            });
        })) {
            entry.supported = false; entry.recycle_verified = false; invalidated = true;
        }
    }
    if (invalidated) ++undo_revision_;
}

void OpsManager::PushUndo(const OpRequest& req,
                          const std::vector<std::wstring>* actual_destinations,
                          const std::vector<FileIdentity>* produced_identities,
                          const std::vector<FileObjectReference>* produced_references,
                          uint64_t task_id) {
    CompletedOperation completed;
    completed.type = req.type;
    completed.cut_clipboard_sequence = req.cut_clipboard_sequence;
    completed.sources = req.sources;
    if (req.type == OpType::Rename && req.sources.size() == 1 &&
        pulse::path::IsDriveRootPath(req.sources.front())) completed.volume_label = req.new_name;
    if (actual_destinations) {
        completed.destinations = *actual_destinations;
    } else if (req.type == OpType::Rename && !req.sources.empty()) {
        completed.destinations.push_back(JoinPath(ParentOf(req.sources.front()), req.new_name));
    } else if (req.type == OpType::BatchRename) {
        for (size_t i = 0; i < req.sources.size(); ++i) {
            const std::wstring name = i < req.new_names.size() ? req.new_names[i] : req.new_name;
            completed.destinations.push_back(JoinPath(ParentOf(req.sources[i]), name));
        }
    } else if (req.type == OpType::Copy || req.type == OpType::Move) {
        for (const auto& source : req.sources)
            completed.destinations.push_back(JoinPath(req.dest_dir, FileName(source)));
    }
    const auto destinations = record_undo_ ? completed.destinations : std::vector<std::wstring>{};
    {
        std::lock_guard<std::mutex> lock(mutex_);
        completed.task_id = task_id ? task_id : status_.task_id;
        completions_.push_back(std::move(completed));
    }
    if (!record_undo_) return;
    if (req.type == OpType::Rename && req.sources.size() == 1 &&
        pulse::path::IsDriveRootPath(req.sources.front())) return;
    if (req.is_undo) {
        if (req.folder_action == FolderAction::RemoveCreatedFolder) {
            std::lock_guard lock(mutex_);
            if (req.undo_revision == undo_revision_ && !undo_.empty() &&
                undo_.back().type == OpType::CreateFolder && undo_.back().sources == req.sources &&
                undo_.back().produced_identities == req.undo_produced_identities) undo_.pop_back();
            return;
        }
        if (req.type == OpType::CreateFolder && req.folder_action == FolderAction::RestoreEmptyFolder) {
            std::lock_guard lock(mutex_);
            if (req.undo_revision == undo_revision_ && !undo_.empty() &&
                undo_.back().type == OpType::RemoveEmptyFolder && undo_.back().sources == req.sources) {
                const auto& removed = undo_.back();
                if (req.undo_group && removed.undo_group == req.undo_group &&
                    removed.produced_identities.size() == 1 && produced_identities && produced_references &&
                    produced_identities->size() == 1 && produced_references->size() == 1 &&
                    produced_references->front().Get() && produced_identities->front().Valid() &&
                    produced_references->front().identity == produced_identities->front()) {
                    // X/X unwrap removes the temporary outer container. Its
                    // authorized recreation has a new ID; only the preceding
                    // rename in this same action may adopt that exact object.
                    for (auto previous = std::next(undo_.rbegin()); previous != undo_.rend(); ++previous) {
                        if (previous->undo_group != req.undo_group) break;
                        if (previous->type != OpType::Rename || previous->destinations != req.sources ||
                            previous->produced_identities != removed.produced_identities) continue;
                        previous->produced_identities = *produced_identities;
                        if (previous->session_only) previous->produced_references = *produced_references;
                        break;
                    }
                }
                undo_.pop_back();
            }
            return;
        }
        if (req.type == OpType::RestoreRecycle && req.undo_revision) {
            std::lock_guard<std::mutex> lock(mutex_);
            if (pending_recycle_undo_revision_ != req.undo_revision || undo_.empty()) return;
            auto& remaining = undo_.back();
            if (remaining.type != OpType::RecycleDelete) return;
            bool consumed = false;
            for (size_t i = remaining.sources.size(); i-- > 0;) {
                const auto restored = std::find(req.sources.begin(), req.sources.end(), remaining.sources[i]);
                const auto index = static_cast<size_t>(restored - req.sources.begin());
                if (index >= req.recycle_paths.size() || i >= remaining.destinations.size() ||
                    remaining.destinations[i] != req.recycle_paths[index] ||
                    i >= remaining.recycle_identities.size() || index >= req.recycle_undo_identities.size() ||
                    !(remaining.recycle_identities[i] == req.recycle_undo_identities[index])) continue;
                remaining.sources.erase(remaining.sources.begin() + i);
                remaining.destinations.erase(remaining.destinations.begin() + i);
                if (i < remaining.recycle_identities.size()) remaining.recycle_identities.erase(remaining.recycle_identities.begin() + i);
                consumed = true;
            }
            if (remaining.sources.empty()) undo_.pop_back();
            if (consumed) ++undo_revision_;
        }
        if (IsMutationOperation(req.type) && req.undo_revision) {
            std::lock_guard<std::mutex> lock(mutex_);
            if (req.undo_revision != undo_revision_ || undo_.empty()) return;
            auto& remaining = undo_.back();
            for (size_t i = remaining.sources.size(); i-- > 0;) {
                for (size_t j = 0; j < req.sources.size() && j < destinations.size(); ++j) {
                    if (i < remaining.destinations.size() && i < remaining.produced_identities.size() &&
                        j < req.undo_produced_identities.size() && remaining.destinations[i] == req.sources[j] &&
                        remaining.sources[i] == destinations[j] && remaining.produced_identities[i] == req.undo_produced_identities[j]) {
                        remaining.sources.erase(remaining.sources.begin() + i);
                        remaining.destinations.erase(remaining.destinations.begin() + i);
                        remaining.produced_identities.erase(remaining.produced_identities.begin() + i);
                        if (i < remaining.produced_references.size())
                            remaining.produced_references.erase(remaining.produced_references.begin() + i);
                        std::erase(remaining.cleanup_directories, req.sources[j]);
                        break;
                    }
                }
            }
            if (remaining.sources.empty()) undo_.pop_back();
        }
        return;
    }
    UndoEntry e;
    e.type = req.type;
    e.undo_group = req.undo_group;
    e.undo_label = req.undo_label;
    e.folder_metadata = req.folder_metadata;
    e.sources = req.sources;
    e.destinations = destinations;
    e.dest_dir = req.dest_dir;
    e.new_name = req.new_name;
    if (req.type == OpType::RemoveEmptyFolder && produced_identities)
        e.produced_identities = *produced_identities;
    if (RequiresProducedIdentity(req.type)) {
        const size_t count = req.type == OpType::Copy ? e.destinations.size() : e.sources.size();
        if (produced_identities) e.produced_identities = *produced_identities;
        e.supported = HasProducedIdentities(count, e.produced_identities) &&
            (!IsMutationOperation(req.type) || e.destinations.size() == count);
        if (!e.supported) e.produced_identities.clear();
        if (e.supported) {
            const auto& paths = e.destinations.empty() ? e.sources : e.destinations;
            for (const auto& path : paths) {
                const auto plain = pulse::path::StripExtendedPathPrefix(path);
                const std::wstring root = std::filesystem::path(plain).root_path().wstring();
                if (plain.starts_with(L"\\\\") || (!root.empty() && GetDriveTypeW(root.c_str()) == DRIVE_REMOTE))
                    e.session_only = true;
            }
            if (e.session_only) {
                if (produced_references) e.produced_references = *produced_references;
                else for (size_t i = 0; i < paths.size(); ++i) {
                    auto reference = FileObjectReference::Open(paths[i]);
                    if (reference.identity != e.produced_identities[i]) reference = {};
                    e.produced_references.push_back(std::move(reference));
                }
            }
            if (e.session_only && (e.produced_references.size() != count ||
                std::any_of(e.produced_references.begin(), e.produced_references.end(),
                    [](const auto& reference) { return !reference.Get(); }))) e.supported = false;
        }
    }
    if (req.type == OpType::RealDelete) return;              // never undoable, not recorded
    if (req.type == OpType::RestoreRecycle) return;
    if (req.type == OpType::EmptyRecycle) return;
    std::lock_guard<std::mutex> lock(mutex_);
    undo_.push_back(std::move(e));
    ++undo_revision_;
}

void OpsManager::FinishMutationUndo(const OpRequest& req) {
    if (!req.is_undo || (!IsMutationOperation(req.type) && req.folder_action != FolderAction::RestoreEmptyFolder &&
        req.folder_action != FolderAction::RemoveCreatedFolder) || !req.undo_revision) return;
    bool changed = false;
    {
        std::lock_guard lock(mutex_);
        changed = FinishMutationUndoLocked(req);
    }
    if (changed && notify_) notify_();
}

void OpsManager::FinishRecycleUndo(const OpRequest& req) {
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        changed = FinishRecycleUndoLocked(req);
    }
    if (changed && notify_) notify_();
}

bool OpsManager::FinishRecycleUndoLocked(const OpRequest& req) {
    if (!req.is_undo || req.type != OpType::RestoreRecycle || !req.undo_revision) return false;
    if (pending_recycle_undo_revision_ != req.undo_revision) return false;
    pending_recycle_undo_revision_ = 0;
    return true;
}

bool OpsManager::FinishMutationUndoLocked(const OpRequest& req) {
    if (!req.is_undo || (!IsMutationOperation(req.type) && req.folder_action != FolderAction::RestoreEmptyFolder &&
        req.folder_action != FolderAction::RemoveCreatedFolder) || !req.undo_revision) return false;
    const bool released = pending_mutation_undo_revision_ == req.undo_revision;
    if (released) pending_mutation_undo_revision_ = 0;
    if (undo_revision_ == req.undo_revision) {
        ++undo_revision_;
        if (lock_retry_ && lock_retry_->req.undo_revision == req.undo_revision)
            lock_retry_->req.undo_revision = undo_revision_;
        if (elevation_retry_ && elevation_retry_->req.undo_revision == req.undo_revision)
            elevation_retry_->req.undo_revision = undo_revision_;
    }
    return released;
}

std::vector<CompletedOperation> OpsManager::DrainCompletions() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<CompletedOperation> out;
    out.reserve(completions_.size());
    while (!completions_.empty()) {
        out.push_back(std::move(completions_.front()));
        completions_.pop_front();
    }
    return out;
}

void OpsManager::Undo() {
    UndoEntry e;
    uint64_t deletion_reservation = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (undo_.empty() || pending_mutation_undo_revision_ || pending_recycle_undo_revision_) return;
        e = undo_.back();
        if (e.type == OpType::RecycleDelete && !e.recycle_verified && e.supported) {
            if (!accepting_) return;
            undo_validation_requested_.store(true);
            if (pending_recycle_undo_revision_) return;
            pending_recycle_undo_revision_ = undo_revision_;
            QueueItem validation;
            validation.req.type = OpType::RestoreRecycle; validation.req.is_undo = true;
            validation.req.sources = e.sources; validation.req.recycle_paths = e.destinations;
            validation.req.undo_revision = undo_revision_;
            validation.seq = next_seq_++;
            queue_.push_front(std::move(validation));
            cv_.notify_one();
            return; // the worker validates before executing; the UI performs no IO
        }
        if (!e.supported) {
            // Leave the entry; report why it cannot be undone.
            status_.last_error = l10n::Get(l10n::StringId::OperationUndoIdentityUnverified);
        } else if (IsMutationOperation(e.type) || e.type == OpType::RemoveEmptyFolder ||
                   (e.type == OpType::CreateFolder && e.undo_group)) {
            deletion_reservation = undo_revision_;
            pending_mutation_undo_revision_ = deletion_reservation;
        } else if (e.type == OpType::RecycleDelete) {
            deletion_reservation = undo_revision_;
            pending_recycle_undo_revision_ = deletion_reservation;
        } else deletion_reservation = undo_revision_;
    }
    // Produced-object inverses consume history only after actual completion.
    if (!e.supported) {
        if (notify_) notify_();
        return;
    }

    switch (e.type) {
    case OpType::Move:
    case OpType::Rename:
    case OpType::BatchRename: {
        OpRequest inv;
        inv.type = e.type;
        inv.is_undo = true;
        inv.undo_revision = deletion_reservation;
        inv.undo_group = e.undo_group;
        inv.undo_label = e.undo_label;
        inv.sources = e.destinations;
        inv.undo_produced_identities = e.produced_identities;
        inv.undo_cleanup_directories = e.cleanup_directories;
        if (e.type == OpType::Move) {
            inv.new_names = e.sources;
            inv.dest_dir = ParentOf(e.sources.front());
        } else {
            for (const auto& source : e.sources) inv.new_names.push_back(FileName(source));
            inv.new_name = inv.new_names.front();
            if (e.type == OpType::BatchRename) {
                std::reverse(inv.sources.begin(), inv.sources.end());
                std::reverse(inv.new_names.begin(), inv.new_names.end());
                std::reverse(inv.undo_produced_identities.begin(), inv.undo_produced_identities.end());
            }
        }
        if (!Submit(std::move(inv))) {
            OpRequest rejected;
            rejected.type = e.type; rejected.is_undo = true; rejected.undo_revision = deletion_reservation;
            FinishMutationUndo(rejected);
        }
        break;
    }
    case OpType::Copy: {
        // Explorer semantics: undo copy = delete the produced copies (to bin).
        OpRequest inv;
        inv.type = OpType::RecycleDelete;
        inv.is_undo = true;
        inv.delete_origin = DeleteOrigin::Undo;
        inv.undo_revision = deletion_reservation;
        inv.undo_group = e.undo_group;
        inv.undo_produced_identities = e.produced_identities;
        if (!e.destinations.empty()) inv.sources = e.destinations;
        else for (const auto& src : e.sources)
            inv.sources.push_back(JoinPath(e.dest_dir, FileName(src)));
        Submit(std::move(inv));
        break;
    }
    case OpType::CreateFolder:
    case OpType::CreateTextFile: {
        // Undo create = recycle-delete the created item.
        OpRequest inv;
        inv.type = OpType::RecycleDelete;
        inv.is_undo = true;
        inv.delete_origin = DeleteOrigin::Undo;
        inv.undo_revision = deletion_reservation;
        inv.undo_group = e.undo_group;
        inv.undo_produced_identities = e.produced_identities;
        inv.sources = e.sources;
        if (e.type == OpType::CreateFolder && e.undo_group) {
            inv.type = OpType::RemoveEmptyFolder;
            inv.folder_action = FolderAction::RemoveCreatedFolder;
        }
        Submit(std::move(inv));
        break;
    }
    case OpType::RecycleDelete: {
        OpRequest inv;
        inv.type = OpType::RestoreRecycle;
        inv.is_undo = true;
        inv.undo_revision = deletion_reservation;
        inv.undo_group = e.undo_group;
        inv.sources = e.sources;
        inv.recycle_paths = e.destinations;
        inv.recycle_undo_identities = e.recycle_identities;
        if (!Submit(std::move(inv))) {
            OpRequest rejected;
            rejected.type = OpType::RestoreRecycle; rejected.is_undo = true; rejected.undo_revision = deletion_reservation;
            FinishRecycleUndo(rejected);
        }
        break;
    }
    case OpType::RemoveEmptyFolder: {
        OpRequest inverse;
        inverse.type = OpType::CreateFolder;
        inverse.folder_action = FolderAction::RestoreEmptyFolder;
        inverse.sources = e.sources;
        inverse.is_undo = true;
        inverse.undo_revision = deletion_reservation;
        inverse.undo_group = e.undo_group;
        inverse.folder_metadata = e.folder_metadata;
        if (!Submit(inverse)) FinishMutationUndo(inverse);
        break;
    }
    default:
        break;
    }
}

// ---------------------------------------------------------------------------
// Worker thread: serialized queue -> ShellClient -> event wait.
// ---------------------------------------------------------------------------

void OpsManager::SetUiWindow(HWND hwnd) noexcept {
    ui_hwnd_.store(hwnd);
    std::lock_guard<std::mutex> lock(open_mutex_);
    if (open_mailbox_) open_mailbox_->owner.store(hwnd);
}

void OpsManager::EnqueueOpen(QueueItem item, bool front) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!accepting_ || stopping_.load()) return;
        item.seq = next_seq_++;
    }
    std::shared_ptr<OpenMailbox> mailbox;
    {
        std::lock_guard<std::mutex> lock(open_mutex_);
        mailbox = open_mailbox_;
    }
    if (!mailbox) return;
    {
        std::lock_guard<std::mutex> lock(mailbox->mutex);
        if (!mailbox->running || mailbox->queue.size() >= 64) return;
        item.enqueued_at = GetTickCount64();
        if (front) mailbox->queue.push_front(std::move(item));
        else mailbox->queue.push_back(std::move(item));
    }
    mailbox->cv.notify_one();
}

void OpsManager::OpenThread(std::shared_ptr<OpenMailbox> mailbox) {
    struct ReleaseSlot { ~ReleaseSlot() { open_workers.fetch_sub(1); } } release;
    for (;;) {
        QueueItem item;
        {
            std::unique_lock lock(mailbox->mutex);
            mailbox->cv.wait(lock, [&] { return !mailbox->queue.empty() || !mailbox->running; });
            if (!mailbox->running) break;
            item = std::move(mailbox->queue.front());
            mailbox->queue.pop_front();
        }
        if (mailbox->before_execute) mailbox->before_execute();
        {
            std::lock_guard lock(mailbox->mutex);
            if (!mailbox->running) break;
        }
        ipc::ShellLaunchRequest request;
        request.owner_hwnd = reinterpret_cast<uintptr_t>(mailbox->owner.load());
        request.verb = item.open_verb;
        request.parameters = std::move(item.open_args);
        if (item.image_context) request.image_context = item.image_context();
        if (item.open_verb == L"__cmdline") {
            request.kind = ipc::ShellLaunchKind::Command;
            request.executable = std::move(item.open_file);
            request.directory = std::move(item.open_path);
        } else if (item.open_verb == L"__reveal") {
            request.kind = ipc::ShellLaunchKind::Reveal;
            request.paths.push_back(std::move(item.open_path));
        } else if (!item.open_file.empty()) {
            request.executable = std::move(item.open_file);
            request.directory = std::move(item.open_path);
        } else {
            request.paths = item.open_paths.empty()
                ? std::vector<std::wstring>{std::move(item.open_path)} : std::move(item.open_paths);
        }
        // Forward only; extension code and interactive dialogs run in a child.
        ipc::ShellClient::Instance().Launch(request, {}, mailbox->shell_owner);
    }
    {
        std::lock_guard lock(mailbox->mutex);
        mailbox->finished = true;
    }
    mailbox->finished_cv.notify_all();
}

ipc::ShellClient::Owner OpsManager::ShellOwner() const {
    return shell_owner_;
}

void OpsManager::StartOpenShellClient(const std::shared_ptr<OpenMailbox>& mailbox,
                                     std::function<void()> notify) {
    ipc::ShellClient::Callbacks cb;
    cb.launch_done = [mailbox, notify](uint32_t, uint32_t hr, bool cancelled, std::wstring error) {
        if (!mailbox || cancelled || SUCCEEDED(static_cast<HRESULT>(hr))) return;
        {
            std::lock_guard lock(mailbox->mutex);
            if (!mailbox->running) return;
            if (mailbox->errors.size() >= 32) mailbox->errors.pop_front();
            mailbox->errors.push_back(error.empty() ? l10n::Get(l10n::StringId::OperationShellFailed) : std::move(error));
        }
        if (notify) notify();
    };
    mailbox->shell_owner = ipc::ShellClient::Instance().Acquire(std::move(cb));
}

void OpsManager::ReleaseShellClients(const std::shared_ptr<OpenMailbox>& mailbox) {
    auto& client = ipc::ShellClient::Instance();
    client.Release(shell_owner_);
    shell_owner_.reset();
    if (mailbox) client.Release(mailbox->shell_owner);
}

void OpsManager::StartShellClient() {
    ipc::ShellClient::Callbacks cb;
    cb.transfer_bytes = [this](uint32_t id, uint64_t done, uint64_t total) {
        shell_activity_tick_ = GetTickCount64();
        if (id != current_req_id_.load() || !transfer_active_.load()) return;
        SetStatus([&](OpStatus& status) {
            if (!status.active) return;
            const auto base = shell_bytes_base_.load();
            status.transferred_bytes = (std::max)(status.transferred_bytes, base + done);
            status.total_bytes = (std::max)(status.total_bytes, base + total);
            shell_transfer_rate_.Observe(GetTickCount64(), status.transferred_bytes, status.total_bytes);
            status.bytes_per_second = shell_transfer_rate_.speed();
            status.peak_bytes_per_second = (std::max)(status.peak_bytes_per_second, status.bytes_per_second);
            status.eta_seconds = shell_transfer_rate_.eta_seconds();
        });
    };
    cb.progress = [this](uint32_t, float pct, std::wstring item,
                         uint32_t items_done, uint32_t total_items) {
        shell_activity_tick_ = GetTickCount64();
        SetStatus([&](OpStatus& st) {
            if (!st.active) return;
            const bool deleting = delete_active_.load();
            if (deleting) {
                // Authorized backends report this path only after its root
                // has been removed; recursive work totals are not row counts.
                if (items_done > 0) delete_item_progress_.Complete(item);
                st.completed_items = delete_item_progress_.Completed();
                pct = st.total_items ? 100.0f * static_cast<float>(st.completed_items) /
                    static_cast<float>(st.total_items) : -1.0f;
            }
            if (!deleting && shell_progress_total_.load()) {
                st.total_items = shell_progress_total_.load();
                const bool batched = transfer_active_.load() && total_items > 1;
                st.completed_items = shell_progress_base_.load() + (batched ? (std::min)(items_done, total_items) : 0);
                st.percent = 100.0f * (static_cast<float>(st.completed_items) +
                    (std::max)(0.0f, pct) / 100.0f) / static_cast<float>(st.total_items);
                st.current_item = std::move(item);
                if (st.phase != OpPhase::Cancelling)
                    st.phase = transfer_pause_.load() ? OpPhase::Paused : OpPhase::Running;
            } else ApplyShellProgress(st, pct, item, items_done, total_items, deleting);
        });
    };
    // Done results land here (reader thread) and RunShellOp waits on them.
    // Context-menu invoke completions share RSP_DONE; route them away first so
    // they can never overwrite the file-op completion a RunShellOp is waiting on.
    cb.done = [this](uint32_t id, uint32_t hr, bool cancelled, std::wstring error) {
        shell_activity_tick_ = GetTickCount64();
        OnShellDone(id, ShellCompletion{hr, cancelled, std::move(error)});
    };
    cb.produced_done = [this](uint32_t id, uint32_t hr, bool cancelled, std::wstring error,
                              std::vector<FileIdentity> identities) {
        shell_activity_tick_ = GetTickCount64();
        ShellCompletion result{hr, cancelled, std::move(error)};
        result.produced_identities = std::move(identities);
        OnShellDone(id, std::move(result));
    };
    cb.on_file_operation_done = [this](uint32_t id, ipc::ShellOperationResult operation) {
        shell_activity_tick_ = GetTickCount64();
        ShellCompletion result;
        result.hr = static_cast<uint32_t>(operation.hr);
        result.cancelled = operation.cancelled;
        result.failed_path = std::move(operation.failed_path);
        result.shell_items = std::move(operation.items);
        OnShellDone(id, std::move(result));
    };
    cb.delete_done = [this](uint32_t id, uint32_t hr, bool cancelled, std::wstring error,
                            std::vector<std::wstring> deleted_paths, std::vector<std::wstring> recycled_paths,
                            std::vector<std::wstring> recycle_destinations, std::wstring failed_path) {
        shell_activity_tick_ = GetTickCount64();
        OnShellDone(id, ShellCompletion{hr, cancelled, std::move(error), std::move(failed_path),
            std::move(deleted_paths), std::move(recycled_paths), std::move(recycle_destinations)});
    };
    cb.ctx_items = [this](uint32_t id, std::vector<ipc::CtxMenuItem> items, bool partial,
                          std::vector<std::wstring> slow_clsids) {
        OnCtxItems(id, std::move(items), partial, std::move(slow_clsids));
    };
    shell_owner_ = ipc::ShellClient::Instance().Acquire(std::move(cb));
    if (!open_mailbox_) open_mailbox_ = std::make_shared<OpenMailbox>();
    StartOpenShellClient(open_mailbox_, notify_);
}

void OpsManager::WorkerThread() {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);



    for (;;) {
        QueueItem item;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            const auto ready = [this] {
                return !running_ || (!concurrent_folder_status_ &&
                    (!queue_.empty() || undo_validation_requested_.load()));
            };
            if (record_undo_) cv_.wait_for(lock, std::chrono::seconds(1), ready);
            else cv_.wait(lock, ready);
            if (!running_) break;
            if (concurrent_folder_status_) continue;
            if (queue_.empty()) {
                lock.unlock();
                RefreshRecycleUndoValidity();
                continue;
            }
            {
                item = std::move(queue_.front());
                queue_.pop_front();
                file_operation_in_flight_ = item.open_path.empty();
                if (item.open_path.empty()) dequeued_task_status_ = QueuedTaskStatus(item.req, item.seq);
                if (item.open_path.empty() && !IsDeleteOperation(item.req.type)) {
                    active_item_ = item;
                    if (item.recovery_sequence != 0) {
                        std::erase_if(pending_recovery_, [&](const RecoveryEntry& entry) {
                            return entry.sequence == item.recovery_sequence;
                        });
                        scheduled_recovery_.erase(item.recovery_sequence);
                    }
                }
            }
        }

        if (item.open_path.empty() && !IsDeleteOperation(item.req.type)) PersistJournal();

        // Opens/verbs run on OpenThread — never block transfers.
        if (!item.open_path.empty()) continue;

        ExecuteFileOperation(item);
        RefreshRecycleUndoValidity();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            active_item_.reset();
            file_operation_in_flight_ = false;
            dequeued_task_status_.reset();
        }
        if (!IsDeleteOperation(item.req.type)) PersistJournal();
    }

    CoUninitialize();
}

// ---------------------------------------------------------------------------
// Context-menu forwarding thread: keeps ShellClient::Submit (which may spawn
// or reconnect to pulse_shell) off the UI thread and out of the transfer queue.
// ---------------------------------------------------------------------------
void OpsManager::SetShellMenuCallback(ShellMenuCallback cb) {
    std::lock_guard<std::mutex> lock(menu_mutex_);
    menu_cb_ = std::move(cb);
}

uint32_t OpsManager::QueryShellMenu(std::vector<std::wstring> paths, void* owner_hwnd,
                                    bool background, bool extended,
                                    std::vector<std::wstring> disabled_clsids) {
    MenuJob job;
    job.kind = MenuJob::Kind::Query;
    job.owner_hwnd = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(owner_hwnd));
    job.background = background;
    job.extended = extended;
    job.paths = std::move(paths);
    job.disabled_clsids = std::move(disabled_clsids);
    uint32_t token = 0;
    {
        std::lock_guard<std::mutex> lock(menu_mutex_);
        if (!menu_running_) return 0;
        token = next_menu_token_++;
        job.token = token;
        menu_queue_.push_back(std::move(job));
    }
    menu_cv_.notify_one();
    return token;
}

void OpsManager::InvokeShellMenu(uint32_t token, uint32_t item_id,
                                 std::wstring verb, std::wstring text) {
    MenuJob job;
    job.kind = MenuJob::Kind::Invoke;
    job.token = token;
    job.item_id = item_id;
    job.verb = std::move(verb);
    job.text = std::move(text);
    {
        std::lock_guard<std::mutex> lock(menu_mutex_);
        if (!menu_running_) return;
        menu_queue_.push_back(std::move(job));
    }
    menu_cv_.notify_one();
}

void OpsManager::CloseShellMenu(uint32_t token) {
    MenuJob job;
    job.kind = MenuJob::Kind::Close;
    job.token = token;
    {
        std::lock_guard<std::mutex> lock(menu_mutex_);
        if (!menu_running_) return;
        menu_queue_.push_back(std::move(job));
    }
    menu_cv_.notify_one();
}

void OpsManager::RegisterShellMenuQuery(uint32_t token, uint32_t request_id) {
    std::lock_guard lock(menu_mutex_);
    menu_session_by_token_[token] = request_id;
    menu_token_by_session_[request_id] = token;
}

void OpsManager::RegisterShellMenuInvoke(uint32_t request_id) {
    std::lock_guard lock(menu_mutex_);
    ctx_invoke_ids_.insert(request_id);
}

bool OpsManager::ConsumeCtxInvokeDone(uint32_t id) {
    std::lock_guard<std::mutex> lock(menu_mutex_);
    return ctx_invoke_ids_.erase(id) != 0;
}

void OpsManager::OnShellDone(uint32_t id, ShellCompletion result) {
    if (ConsumeCtxInvokeDone(id)) {
        ctx_invoke_done_.fetch_add(1, std::memory_order_acq_rel);
        if (notify_) notify_();
        return;
    }
    ShellMenuCallback callback;
    uint32_t token = 0;
    {
        std::lock_guard lock(menu_mutex_);
        const auto query = menu_token_by_session_.find(id);
        if (query != menu_token_by_session_.end()) {
            token = query->second;
            menu_session_by_token_.erase(token);
            menu_token_by_session_.erase(query);
            callback = menu_cb_;
        }
    }
    if (token) {
        if (callback) callback(token, {}, false, {});
        return;
    }
    {
        std::lock_guard lock(done_mutex_);
        done_results_.try_emplace(id, std::move(result));
    }
    done_cv_.notify_all();
}

bool OpsManager::TakeCtxInvokeDone() {
    return ctx_invoke_done_.exchange(0, std::memory_order_acq_rel) != 0;
}

void OpsManager::OnCtxItems(uint32_t client_id, std::vector<ShellMenuItem> items,
                            bool partial, std::vector<std::wstring> slow_clsids) {
    ShellMenuCallback cb;
    uint32_t token = 0;
    {
        std::lock_guard<std::mutex> lock(menu_mutex_);
        auto it = menu_token_by_session_.find(client_id);
        if (it == menu_token_by_session_.end()) return; // session already closed
        token = it->second;
        cb = menu_cb_;
        if (!partial) {
            // Keep token mapping until close/invoke; partial must not drop it.
        }
    }
    if (cb) cb(token, std::move(items), partial, std::move(slow_clsids));
}

void OpsManager::MenuThread() {
    for (;;) {
        MenuJob job;
        {
            std::unique_lock<std::mutex> lock(menu_mutex_);
            menu_cv_.wait(lock, [this] { return !menu_queue_.empty() || !menu_running_; });
            if (!menu_running_) break; // drop queued jobs; sessions die with the host
            job = std::move(menu_queue_.front());
            menu_queue_.pop_front();
        }
        auto& client = ipc::ShellClient::Instance();
        switch (job.kind) {
        case MenuJob::Kind::CreateFolder:
            job.create_folder();
            break;
        case MenuJob::Kind::Query: {
            const uint32_t id = client.QueryContextMenu(
                job.paths, job.owner_hwnd, job.background, job.extended,
                job.disabled_clsids, [&](uint32_t request_id) {
                    RegisterShellMenuQuery(job.token, request_id);
                }, ShellOwner());
            if (id == 0) {
                ShellMenuCallback callback;
                { std::lock_guard lock(menu_mutex_); callback = menu_cb_; }
                if (callback) callback(job.token, {}, false, {});
            }
            break;
        }
        case MenuJob::Kind::Invoke: {
            uint32_t session = 0;
            {
                std::lock_guard<std::mutex> lock(menu_mutex_);
                auto it = menu_session_by_token_.find(job.token);
                if (it == menu_session_by_token_.end()) break;
                session = it->second;
                menu_session_by_token_.erase(it);
                menu_token_by_session_.erase(session);
            }
            const uint32_t id = client.InvokeContextMenu(session, job.item_id,
                job.verb, job.text, [&](uint32_t request_id) {
                    RegisterShellMenuInvoke(request_id);
                }, ShellOwner());
            if (id == 0) { ctx_invoke_done_.fetch_add(1); if (notify_) notify_(); }
            break;
        }
        case MenuJob::Kind::Close: {
            uint32_t session = 0;
            {
                std::lock_guard<std::mutex> lock(menu_mutex_);
                auto it = menu_session_by_token_.find(job.token);
                if (it == menu_session_by_token_.end()) break;
                session = it->second;
                menu_session_by_token_.erase(it);
                menu_token_by_session_.erase(session);
            }
            client.CloseContextMenu(session);
            break;
        }
        }
    }
}

void OpsManager::RunDriveLabelRename(const OpRequest& req, uint64_t task_id,
    decltype(&SetVolumeLabelW) set_label, const DriveLabelBackend* injected_backend) {
    SetStatus([&](OpStatus& status) {
        status.active = true; status.type = req.type; status.task_id = task_id;
        status.phase = OpPhase::Running; status.percent = -1.0f;
        status.summary = Describe(req); status.last_error.clear();
        status.source_label = status.current_item = req.sources.front();
        status.destination_label.clear();
        status.total_items = 1; status.completed_items = 0;
        status.total_bytes = status.transferred_bytes = 0;
        status.bytes_per_second = status.peak_bytes_per_second = 0; status.eta_seconds = 0;
    });
    bool cancelled = stopping_.load() || shell_cancel_requested_.load() || lock_retry_cancel_.load();
    DWORD code = ERROR_SUCCESS;
    HRESULT failure = S_OK;
    bool mapped = false;
    bool readback_failed = false;
    if (!cancelled) {
        const auto source = pulse::path::StripExtendedPathPrefix(req.sources.front());
        const wchar_t root[] = {source.front(), L':', L'\\', 0};
        const DriveLabelBackend defaults;
        const auto& backend = injected_backend ? *injected_backend : defaults;
        // Preserve the existing injected volume API contract without probing a real drive.
        mapped = (injected_backend || set_label == &SetVolumeLabelW) && backend.drive_type(root) == DRIVE_REMOTE;
        std::optional<std::wstring> mapped_label;
        if (req.new_name.find(L'\0') != std::wstring::npos) code = ERROR_INVALID_NAME;
        else if (mapped) {
            failure = backend.set_mapped_alias(root, req.new_name);
            cancelled = failure == E_ABORT || failure == HRESULT_FROM_WIN32(ERROR_CANCELLED) ||
                failure == COPYENGINE_E_USER_CANCELLED || stopping_.load() ||
                shell_cancel_requested_.load() || lock_retry_cancel_.load();
            if (SUCCEEDED(failure) && !cancelled) {
                mapped_label = backend.read_mapped_label(root);
                // The alias may already have changed. A failed readback must not
                // publish an invented empty name that later refreshes overwrite.
                if (!mapped_label || mapped_label->empty()) {
                    readback_failed = true;
                    failure = HRESULT_FROM_WIN32(ERROR_READ_FAULT);
                }
            }
            if (FAILED(failure)) code = HRESULT_FACILITY(failure) == FACILITY_WIN32
                ? HRESULT_CODE(failure) : ERROR_GEN_FAILURE;
        } else if (!(injected_backend ? backend.set_volume_label : set_label)(root, req.new_name.c_str())) {
            code = GetLastError();
            if (code == ERROR_SUCCESS) code = ERROR_GEN_FAILURE;
        }
        if (mapped && (stopping_.load() || shell_cancel_requested_.load() || lock_retry_cancel_.load())) cancelled = true;
        if (code == ERROR_SUCCESS && !cancelled) {
            const std::vector<std::wstring> destinations;
            auto completed_request = req;
            if (mapped_label) completed_request.new_name = *mapped_label;
            PushUndo(completed_request, &destinations);
        }
    }
    if (!cancelled && code != ERROR_SUCCESS && !mapped) {
        QueueItem retry; retry.req = req; retry.seq = task_id;
        CaptureElevationFailure(std::move(retry), HRESULT_FROM_WIN32(code), req.sources.front());
    }
    SetStatus([&](OpStatus& status) {
        status.active = false;
        ++status.completed_ops;
        status.failure_hr = FAILED(failure) ? failure : code == ERROR_SUCCESS ? S_OK : HRESULT_FROM_WIN32(code);
        status.failed_path = code == ERROR_SUCCESS ? L"" : req.sources.front();
        if (cancelled) {
            status.phase = OpPhase::Cancelled; status.summary = l10n::Get(l10n::StringId::OperationCancelled);
        } else if (code != ERROR_SUCCESS) {
            status.phase = OpPhase::Failed; status.last_error = Win32Message(code);
            if (readback_failed) status.last_error = l10n::Get(l10n::StringId::MappedDriveReadbackFailed);
            status.summary = l10n::Format(l10n::StringId::OperationStateFormat,
                OpVerb(req.type).c_str(), l10n::Get(l10n::StringId::OpFailedBadge).c_str());
        } else {
            status.phase = OpPhase::Completed; status.percent = 100.0f;
            status.completed_items = status.total_items; status.summary = l10n::Format(l10n::StringId::OperationStateFormat,
                Describe(req).c_str(), l10n::Get(l10n::StringId::OpCompleted).c_str());
        }
    });
}

void OpsManager::RunManualRecycleRestore(const OpRequest& req,
    decltype(&IsCurrentUserRecyclePayload) owns_payload, DestinationGuard* continuation_guard) {
    std::vector<RestoreItemResult> results;
    std::vector<std::wstring> restored_sources, destinations;
    std::wstring failures, failed_path;
    HRESULT failure = S_OK;
    bool cancelled = false;
    const LANGID language = l10n::effective_language() == l10n::Language::EnUS
        ? MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US) : MAKELANGID(LANG_CHINESE, SUBLANG_CHINESE_SIMPLIFIED);
    for (size_t i = 0; i < req.sources.size(); ++i) {
        RestoreItemResult result;
        result.original_path = req.sources[i];
        if (i < req.recycle_paths.size()) result.recycle_path = req.recycle_paths[i];
        cancelled = cancelled || stopping_.load() || shell_cancel_requested_.load();
        DWORD code = ERROR_SUCCESS;
        RecycleUndoIdentity identity;
        if (cancelled) code = ERROR_CANCELLED;
        else if (req.sources.size() != req.recycle_paths.size() || !owns_payload ||
            !owns_payload(result.original_path, result.recycle_path)) code = ERROR_INVALID_DATA;
        else if (!ValidateRecycleUndoPair(result.original_path, result.recycle_path, identity,
            i < req.recycle_undo_identities.size() ? &req.recycle_undo_identities[i] : nullptr, false))
            code = ERROR_INVALID_DATA;
        else {
            const DWORD attributes = GetFileAttributesW(result.recycle_path.c_str());
            const bool directory = attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            // Re-evaluate the same naming rule after an atomic no-replace collision.
            // Another process can create a candidate between probing and renaming.
            for (unsigned attempt = 0; attempt < 32; ++attempt) {
                if (stopping_.load() || shell_cancel_requested_.load()) {
                    cancelled = true; code = ERROR_CANCELLED; break;
                }
                result.destination = req.is_undo ? result.original_path : UniqueCopyPath(result.original_path, directory);
                result.restored = RestoreRecyclePair(result.original_path, result.recycle_path,
                    result.destination, identity, code, continuation_guard);
                if (result.restored || req.is_undo || (code != ERROR_ALREADY_EXISTS && code != ERROR_FILE_EXISTS)) break;
            }
        }
        result.error = HRESULT_FROM_WIN32(code);
        if (result.restored) {
            restored_sources.push_back(result.original_path);
            destinations.push_back(result.destination);
        }
        if (code != ERROR_SUCCESS && code != ERROR_CANCELLED) {
            if (SUCCEEDED(failure)) { failure = result.error; failed_path = result.original_path; }
            if (!failures.empty()) failures += L"\n\n";
            failures += DisplayPath(result.original_path) + L" [" + FileName(result.recycle_path) + L"]\n" +
                format::FileOperationErrorText(result.error, language);
        }
        results.push_back(std::move(result));
        SetStatus([&](OpStatus& status) {
            status.completed_items = restored_sources.size();
            status.current_item = req.sources[i];
            status.percent = req.sources.empty() ? 0.0f : 100.0f * static_cast<float>(i + 1) / static_cast<float>(req.sources.size());
            status.restore_results = results;
        });
    }
    if (req.sources.empty()) { failure = E_INVALIDARG; failures = format::FileOperationErrorText(failure, language); }
    if (!restored_sources.empty()) {
        OpRequest completed = req;
        completed.recycle_paths.clear();
        completed.recycle_undo_identities.clear();
        for (size_t i = 0; i < results.size(); ++i) if (results[i].restored) {
            completed.recycle_paths.push_back(results[i].recycle_path);
            if (i < req.recycle_undo_identities.size()) completed.recycle_undo_identities.push_back(req.recycle_undo_identities[i]);
        }
        completed.sources = std::move(restored_sources);
        PushUndo(completed, &destinations);
    }
    SetStatus([&](OpStatus& status) {
        status.active = false; ++status.completed_ops;
        status.completed_items = destinations.size();
        status.failure_hr = cancelled ? S_OK : failure;
        status.failed_path = failed_path;
        status.restore_results = std::move(results);
        status.lock_retry_available = false; status.locked_path.clear(); status.lock_owners.clear();
        status.last_error = std::move(failures);
        if (cancelled) {
            status.phase = OpPhase::Cancelled;
            status.summary = l10n::Get(l10n::StringId::DeleteCancelled);
        } else if (FAILED(failure)) status.phase = OpPhase::Failed;
        else {
            status.phase = OpPhase::Completed;
            status.summary = Describe(req) + L" " + l10n::Get(l10n::StringId::OpCompleted);
        }
    });
}

void OpsManager::RunShellOp(const OpRequest& req, uint64_t task_id, const FileIdentity* rename_identity, bool lock_retry,
    const std::vector<FileIdentity>* continuation_identities, DestinationGuard* continuation_guard) {
    if (IsDeleteOperation(req.type)) {
        QueueItem rejected;
        rejected.req = req; rejected.seq = task_id;
        FinishDelete(rejected, l10n::Get(l10n::StringId::DeleteNotAdmitted).c_str());
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!lock_retry) shell_cancel_requested_.store(stopping_.load());
    }
    if (req.type == OpType::Rename && req.sources.size() == 1 &&
        pulse::path::IsDriveRootPath(req.sources.front())) {
        RunDriveLabelRename(req, task_id);
        return;
    }
    shell_activity_tick_ = GetTickCount64();
    // Status: active.
    SetStatus([&](OpStatus& st) {
        st.active = true;
        st.type = req.type;
        st.task_id = task_id;
        st.phase = OpPhase::Running;
        st.percent = 0.0f;
        st.summary = Describe(req);
        st.last_error.clear(); st.restore_results.clear();
        st.failure_hr = S_OK; st.failed_path.clear();
        st.source_label = req.sources.empty() ? L"" : FileName(req.sources.front());
        st.destination_label.clear();
        st.current_item = st.source_label;
        st.total_bytes = st.transferred_bytes = 0;
        st.total_items = req.sources.empty() ? 1 : req.sources.size();
        st.completed_items = 0;
        st.bytes_per_second = st.peak_bytes_per_second = 0.0;
        st.eta_seconds = 0;
    });

    if (req.type == OpType::RestoreRecycle && !req.is_undo) {
        RunManualRecycleRestore(req, &IsCurrentUserRecyclePayload, continuation_guard);
        return;
    }

    if (req.type == OpType::Rename || req.type == OpType::BatchRename ||
        req.type == OpType::CreateFolder || req.type == OpType::CreateTextFile) {
        RunBasicShellOperation(req, task_id, rename_identity, continuation_identities);
        return;
    }

    if (stopping_.load() || shell_cancel_requested_.load()) {
        SetStatus([&](OpStatus& st) {
            st.active = false; st.phase = OpPhase::Cancelled;
            st.last_error.clear(); st.summary = l10n::Get(l10n::StringId::OperationCancelled); ++st.completed_ops;
        });
        return;
    }
    if (req.type == OpType::RestoreRecycle) {
        bool valid = req.sources.size() == req.recycle_paths.size() &&
            req.sources.size() == req.recycle_undo_identities.size();
        for (size_t i = 0; valid && i < req.sources.size(); ++i) {
            RecycleUndoIdentity current;
            valid = IsCurrentUserRecyclePayload(req.sources[i], req.recycle_paths[i]) &&
                ValidateRecycleUndoPair(req.sources[i], req.recycle_paths[i], current, &req.recycle_undo_identities[i], false);
        }
        if (req.is_undo && req.recycle_undo_identities.empty() && req.undo_revision) {
            RefreshRecycleUndoValidity();
            OpRequest verified = req;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if ((undo_revision_ == req.undo_revision || undo_revision_ == req.undo_revision + 1) &&
                    !undo_.empty() && undo_.back().type == OpType::RecycleDelete && undo_.back().supported &&
                    undo_.back().recycle_verified && undo_.back().sources == req.sources &&
                    undo_.back().destinations == req.recycle_paths &&
                    pending_recycle_undo_revision_ == req.undo_revision) {
                    verified.recycle_undo_identities = undo_.back().recycle_identities;
                }
            }
            if (!verified.recycle_undo_identities.empty()) {
                RunShellOp(verified, task_id, nullptr, false, nullptr, continuation_guard);
                return;
            }
        }
        if (valid && req.is_undo && req.undo_revision) {
            std::lock_guard<std::mutex> lock(mutex_);
            valid = req.undo_revision && pending_recycle_undo_revision_ == req.undo_revision &&
                (undo_revision_ == req.undo_revision || undo_revision_ == req.undo_revision + 1) &&
                !undo_.empty() && undo_.back().type == OpType::RecycleDelete &&
                undo_.back().sources == req.sources && undo_.back().destinations == req.recycle_paths;
        }
        if (!valid && req.is_undo) {
            SetStatus([&](OpStatus& st) {
                st.active = false; st.phase = OpPhase::Failed; ++st.completed_ops;
                st.last_error = l10n::Get(l10n::StringId::OperationRecycledIdentityChanged);
            });
            RefreshRecycleUndoValidity();
            return;
        }
        RunManualRecycleRestore(req, &IsCurrentUserRecyclePayload, continuation_guard);
        return;
    }
    SetStatus([&](OpStatus& status) {
        status.active = false; status.phase = OpPhase::Failed; ++status.completed_ops;
        status.failure_hr = E_INVALIDARG;
        status.last_error = status.summary = l10n::Get(l10n::StringId::OperationFailedMessage);
    });
}

bool OpsManager::WaitShellDone(uint32_t id, ShellCompletion& result) {
    auto& client = ipc::ShellClient::Instance();
    constexpr ULONGLONG kShellInactivityTimeoutMs = 10ull * 60ull * 1000ull;
    {
        std::unique_lock<std::mutex> lock(done_mutex_);
        while (!done_results_.contains(id) && !stopping_.load()) {
            done_cv_.wait_for(lock, std::chrono::seconds(5));
            if (done_results_.contains(id) ||
                GetTickCount64() - shell_activity_tick_.load() < kShellInactivityTimeoutMs)
                continue;
            lock.unlock();
            client.Abort(id);
            lock.lock();
            // Abort cannot synthesize a result after transport has already removed
            // its pending entry. The waiter must still terminate in that case.
            done_results_.try_emplace(id, ShellCompletion{
                static_cast<uint32_t>(HRESULT_FROM_WIN32(ERROR_TIMEOUT)), false, l10n::Get(l10n::StringId::OperationTimeout)});
        }
        if (stopping_.load() && !done_results_.contains(id)) {
            lock.unlock();
            current_req_id_.store(0);
            shell_cancel_requested_ = false;
            SetStatus([](OpStatus& st) {
                st.active = false;
                st.phase = OpPhase::Cancelled;
                st.last_error.clear(); st.failure_hr = S_OK; st.failed_path.clear();
                st.summary = l10n::Get(l10n::StringId::OperationStopped);
                st.completed_ops++;
            });
            return false;
        }
        auto found = done_results_.find(id);
        result = std::move(found->second);
        done_results_.erase(found);
    }
    current_req_id_.store(0);
    shell_cancel_requested_ = false;
    return true;
}

} // namespace pulse::ops
