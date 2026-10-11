// ops_manager.cpp — See ops_manager.h for the contract.
#include "ops_manager.h"
#include "operation_elevation_adapter.h"
#include "operation_presentation.h"
#include "delete_operation.h"
#include "produced_undo.h"
#include "../fs/fs_recycle.h"
#include "../fs/fs_enum.h"
#include "../ipc/shell_client.h"
#include "../common/json_utils.h"
#include "../common/path_utils.h"
#include "../common/utf8_file.h"
#include "../common/localization.h"
#include "../common/file_operation_error.h"
#include <objbase.h>
#include <bcrypt.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cwctype>
#include <thread>
#include <vector>
#include <winioctl.h>

namespace pulse::ops {

namespace {

std::wstring FileName(const std::wstring& path) {
    std::wstring_view v = path;
    if (v.size() > 1 && v.back() == L'\\') v.remove_suffix(1);
    auto pos = v.find_last_of(L"\\/");
    if (pos != std::wstring_view::npos) return std::wstring(v.substr(pos + 1));
    return std::wstring(v);
}

bool IsMutationOperation(OpType type) {
    return type == OpType::Move || type == OpType::Rename || type == OpType::BatchRename;
}

bool RequiresProducedIdentity(OpType type) {
    return IsMutationOperation(type) || type == OpType::Copy ||
        type == OpType::CreateFolder || type == OpType::CreateTextFile;
}

bool HasProducedIdentities(size_t count, const std::vector<FileIdentity>& identities) {
    return count != 0 && identities.size() == count &&
        std::all_of(identities.begin(), identities.end(), [](const auto& id) { return id.Valid(); });
}

std::vector<FileIdentity> SelectUndoIdentities(const OpRequest& req, const std::vector<std::wstring>& sources) {
    std::vector<FileIdentity> identities;
    identities.reserve(sources.size());
    for (const auto& source : sources) {
        const auto found = std::find(req.sources.begin(), req.sources.end(), source);
        const auto index = static_cast<size_t>(found - req.sources.begin());
        identities.push_back(index < req.undo_produced_identities.size() ? req.undo_produced_identities[index] : FileIdentity{});
    }
    return identities;
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
constexpr unsigned kOpenWorkerLimit = 2;

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

void ReplaceAll(std::wstring& hay, std::wstring_view from, const std::wstring& to) {
    if (from.empty()) return;
    size_t i = 0;
    while ((i = hay.find(from, i)) != std::wstring::npos) {
        hay.replace(i, from.size(), to);
        i += to.size();
    }
}

std::wstring ExpandShellCommand(std::wstring command, const std::wstring& path) {
    const std::wstring file = pulse::path::StripExtendedPathPrefix(path);
    std::wstring quoted = L"\"";
    quoted += file;
    quoted += L'"';
    ReplaceAll(command, L"\"%1\"", quoted);
    ReplaceAll(command, L"\"%L\"", quoted);
    ReplaceAll(command, L"\"%l\"", quoted);
    ReplaceAll(command, L"\"%V\"", quoted);
    ReplaceAll(command, L"\"%v\"", quoted);
    ReplaceAll(command, L"%1", quoted);
    ReplaceAll(command, L"%L", quoted);
    ReplaceAll(command, L"%l", quoted);
    ReplaceAll(command, L"%V", quoted);
    ReplaceAll(command, L"%v", quoted);
    ReplaceAll(command, L"%*", quoted);
    return command;
}

std::wstring JoinPath(const std::wstring& dir, const std::wstring& name) {
    if (dir.empty()) return name;
    if (dir.back() == L'\\') return dir + name;
    return dir + L"\\" + name;
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

// In-place rename avoids the Shell IPC round trip. Failed renames report their
// error directly: the current Shell host suppresses confirmation and can
// overwrite an existing target, so it is not a safe rename fallback.
bool IsRenameComponent(const std::wstring& name) {
    if (name.empty() || name == L"." || name == L".." ||
        name.back() == L'.' || name.back() == L' ') return false;
    for (const auto c : name)
        if (c < 32 || std::wstring_view(L"\\/:*?\"<>|").find(c) != std::wstring_view::npos) return false;
    std::wstring stem = name.substr(0, name.find(L'.'));
    while (!stem.empty() && stem.back() == L' ') stem.pop_back();
    for (auto& c : stem) c = static_cast<wchar_t>(towupper(c));
    if (stem == L"CON" || stem == L"PRN" || stem == L"AUX" || stem == L"NUL" ||
        stem == L"CONIN$" || stem == L"CONOUT$") return false;
    if (stem.size() == 4 && (stem.starts_with(L"COM") || stem.starts_with(L"LPT")) &&
        ((stem[3] >= L'1' && stem[3] <= L'9') || stem[3] == L'\u00b9' || stem[3] == L'\u00b2' || stem[3] == L'\u00b3')) return false;
    return true;
}

enum class RenameResult { Completed, Rejected };
RenameResult RenameInProcess(const std::wstring& source, const std::wstring& new_name,
                     std::wstring* error, DWORD* failure_code = nullptr, FileIdentity* source_identity = nullptr,
    const FileIdentity* expected_identity = nullptr, bool* have_identity = nullptr,
    DestinationGuard* continuation_guard = nullptr) {
    if (failure_code) *failure_code = ERROR_INVALID_NAME;
    if (have_identity) *have_identity = false;
    if (source.empty() || !IsRenameComponent(new_name)) {
        if (error) *error = l10n::Get(l10n::StringId::InvalidName);
        return RenameResult::Rejected;
    }
    const std::wstring target = JoinPath(ParentOf(source), new_name);
    if (target.empty()) {
        if (error) *error = l10n::Get(l10n::StringId::InvalidName);
        return RenameResult::Rejected;
    }
    // The current shell host suppresses confirmation UI, so sending an existing
    // target there could silently overwrite it. Reject that conflict here.
    if (CompareStringOrdinal(source.c_str(), -1, target.c_str(), -1, TRUE) != CSTR_EQUAL &&
        GetFileAttributesW(target.c_str()) != INVALID_FILE_ATTRIBUTES) {
        if (error) *error = l10n::Get(l10n::StringId::RenameTargetExists);
        if (failure_code) *failure_code = ERROR_ALREADY_EXISTS;
        return RenameResult::Rejected;
    }
    DestinationGuard::Handle identity_handle(CreateFileW(source.c_str(), 0,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (identity_handle.get() == INVALID_HANDLE_VALUE) identity_handle.release();
    FileIdentity identity;
    const bool identified = identity_handle && ReadFileIdentity(identity_handle.get(), identity);
    if (have_identity) *have_identity = identified;
    if (identified && source_identity) *source_identity = identity;
    if (expected_identity && (!identified || identity != *expected_identity)) {
        if (failure_code) *failure_code = ERROR_INVALID_DATA;
        if (error) *error = l10n::Get(l10n::StringId::OperationSourceIdentityChanged);
        return RenameResult::Rejected;
    }
    DestinationGuard::Handle rename_handle(CreateFileW(source.c_str(), DELETE | FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (rename_handle.get() == INVALID_HANDLE_VALUE) {
        const DWORD code = GetLastError();
        rename_handle.release();
        if (failure_code) *failure_code = code;
        if (error) *error = l10n::Format(l10n::StringId::OperationRenameFailedFormat, code);
        return RenameResult::Rejected;
    }
    FileIdentity current;
    const bool current_identified = ReadFileIdentity(rename_handle.get(), current);
    if ((expected_identity && (!current_identified || current != *expected_identity)) ||
        (identified && (!current_identified || current != identity))) {
        if (failure_code) *failure_code = ERROR_INVALID_DATA;
        if (error) *error = l10n::Get(l10n::StringId::OperationSourceIdentityChanged);
        return RenameResult::Rejected;
    }
    if (source_identity) *source_identity = current;
    if (have_identity) *have_identity = current_identified;
    if (!(continuation_guard ? continuation_guard->RenameLeaf(rename_handle.get(), target)
                             : DestinationGuard::RenameHandle(rename_handle.get(), target))) {
        const DWORD code = GetLastError();
        if (failure_code) *failure_code = code;
        if (error) *error = l10n::Format(l10n::StringId::OperationRenameFailedFormat, code);
        return RenameResult::Rejected;
    }
    return RenameResult::Completed;
}

using TransferEntry = TransferPlanEntry;

struct CopyProgressContext {
    std::atomic<bool>* cancel = nullptr;
    std::atomic<bool>* pause = nullptr;
    std::function<void(uint64_t, uint64_t)> report;
    bool pause_sent = false;
    ULONGLONG last_report = 0;
    DestinationGuard::Handle destination_handle;
    COPYFILE2_COPY_PHASE error_phase = COPYFILE2_PHASE_NONE;
    const FileObjectReference* source_identity = nullptr;
    decltype(&GetFileInformationByHandleEx) identity_query = GetFileInformationByHandleEx;
    bool source_checked = false;
    bool source_changed = false;
};

class TransferRateEstimator {
public:
    void Reset(ULONGLONG tick, uint64_t bytes) {
        samples_.clear();
        samples_.push_back({ tick, bytes });
        smoothed_speed_ = 0.0;
        eta_seconds_ = 0;
        last_rate_tick_ = tick;
        last_eta_tick_ = tick;
    }

    void Observe(ULONGLONG tick, uint64_t bytes, uint64_t total_bytes) {
        if (samples_.empty() || bytes < samples_.back().bytes) {
            Reset(tick, bytes);
            return;
        }

        constexpr ULONGLONG kMinimumSampleMs = 100;
        constexpr ULONGLONG kWindowMs = 4000;
        constexpr ULONGLONG kWarmupMs = 750;
        if (tick - samples_.back().tick < kMinimumSampleMs && bytes < total_bytes)
            return;

        if (tick == samples_.back().tick) {
            samples_.back().bytes = bytes;
        } else {
            samples_.push_back({ tick, bytes });
        }
        while (samples_.size() > 2 && samples_[1].tick + kWindowMs <= tick)
            samples_.pop_front();

        const ULONGLONG span = tick - samples_.front().tick;
        const uint64_t byte_delta = bytes - samples_.front().bytes;
        if (span < kWarmupMs || byte_delta == 0) return;

        const double window_speed = static_cast<double>(byte_delta) * 1000.0
            / static_cast<double>(span);
        if (smoothed_speed_ <= 0.0) {
            smoothed_speed_ = window_speed;
        } else {
            const double seconds = static_cast<double>(tick - last_rate_tick_) / 1000.0;
            const double alpha = 1.0 - std::exp(-seconds);
            smoothed_speed_ += alpha * (window_speed - smoothed_speed_);
        }
        last_rate_tick_ = tick;

        if (bytes >= total_bytes || smoothed_speed_ <= 1.0) {
            eta_seconds_ = 0;
            return;
        }
        if (eta_seconds_ != 0 && tick - last_eta_tick_ < 1000) return;

        const double raw_eta = static_cast<double>(total_bytes - bytes) / smoothed_speed_;
        if (eta_seconds_ == 0) {
            eta_seconds_ = (std::max)(uint64_t{ 1 },
                static_cast<uint64_t>(std::ceil(raw_eta)));
        } else {
            // React faster to a slowdown than to a transient speed-up.
            const double alpha = raw_eta > static_cast<double>(eta_seconds_) ? 0.45 : 0.20;
            const double blended = static_cast<double>(eta_seconds_)
                + alpha * (raw_eta - static_cast<double>(eta_seconds_));
            eta_seconds_ = (std::max)(uint64_t{ 1 },
                static_cast<uint64_t>(std::llround(blended)));
        }
        last_eta_tick_ = tick;
    }

    double speed() const { return smoothed_speed_; }
    uint64_t eta_seconds() const { return eta_seconds_; }

private:
    struct Sample {
        ULONGLONG tick = 0;
        uint64_t bytes = 0;
    };
    std::deque<Sample> samples_;
    double smoothed_speed_ = 0.0;
    uint64_t eta_seconds_ = 0;
    ULONGLONG last_rate_tick_ = 0;
    ULONGLONG last_eta_tick_ = 0;
};

COPYFILE2_MESSAGE_ACTION CALLBACK CopyProgress(const COPYFILE2_MESSAGE* message,
                                               void* raw) {
    auto* ctx = static_cast<CopyProgressContext*>(raw);
    if (!ctx || !message) return COPYFILE2_PROGRESS_CANCEL;
    HANDLE destination = nullptr;
    HANDLE source = nullptr;
    switch (message->Type) {
    case COPYFILE2_CALLBACK_CHUNK_STARTED:
        destination = message->Info.ChunkStarted.hDestinationFile; source = message->Info.ChunkStarted.hSourceFile; break;
    case COPYFILE2_CALLBACK_CHUNK_FINISHED:
        destination = message->Info.ChunkFinished.hDestinationFile; source = message->Info.ChunkFinished.hSourceFile; break;
    case COPYFILE2_CALLBACK_STREAM_STARTED:
        destination = message->Info.StreamStarted.hDestinationFile; source = message->Info.StreamStarted.hSourceFile; break;
    case COPYFILE2_CALLBACK_STREAM_FINISHED:
        destination = message->Info.StreamFinished.hDestinationFile; source = message->Info.StreamFinished.hSourceFile; break;
    default: break;
    }
    if (source && ctx->source_identity) {
        if (!ctx->source_identity->Matches(source, ctx->identity_query)) {
            ctx->source_changed = true;
            return COPYFILE2_PROGRESS_CANCEL;
        }
        ctx->source_checked = true;
    }
    if (destination && !ctx->destination_handle) {
        HANDLE owned = nullptr;
        if (!DuplicateHandle(GetCurrentProcess(), destination, GetCurrentProcess(), &owned,
                FILE_READ_ATTRIBUTES, FALSE, 0)) return COPYFILE2_PROGRESS_CANCEL;
        ctx->destination_handle.reset(owned);
    }
    uint64_t transferred = 0;
    uint64_t total = 0;
    switch (message->Type) {
    case COPYFILE2_CALLBACK_CHUNK_FINISHED:
        transferred = message->Info.ChunkFinished.uliTotalBytesTransferred.QuadPart;
        total = message->Info.ChunkFinished.uliTotalFileSize.QuadPart;
        break;
    case COPYFILE2_CALLBACK_STREAM_FINISHED:
        transferred = message->Info.StreamFinished.uliTotalBytesTransferred.QuadPart;
        total = message->Info.StreamFinished.uliTotalFileSize.QuadPart;
        break;
    case COPYFILE2_CALLBACK_ERROR:
        ctx->error_phase = message->Info.Error.CopyPhase;
        transferred = message->Info.Error.uliTotalBytesTransferred.QuadPart;
        total = message->Info.Error.uliTotalFileSize.QuadPart;
        break;
    default:
        break;
    }
    const ULONGLONG now = GetTickCount64();
    if (ctx->report && (transferred == total || now - ctx->last_report >= 100)) {
        ctx->last_report = now;
        ctx->report(transferred, total);
    }
    if (ctx->cancel && ctx->cancel->load()) return COPYFILE2_PROGRESS_CANCEL;
    if (ctx->pause && ctx->pause->load() && !ctx->pause_sent) {
        ctx->pause_sent = true;
        return COPYFILE2_PROGRESS_PAUSE;
    }
    return COPYFILE2_PROGRESS_CONTINUE;
}

bool ReadEntryMetadata(const std::wstring& path, TransferEntry& entry,
                       decltype(&GetFileInformationByHandleEx) identity_query = nullptr, bool require_file_identity = true) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) return false;
    if (identity_query && (require_file_identity || (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))) {
        entry.source_identity = FileObjectReference::Open(path, identity_query);
        if (!entry.source_identity.handle) return false;
        BY_HANDLE_FILE_INFORMATION held{};
        if (!GetFileInformationByHandle(entry.source_identity.Get(), &held)) return false;
        data.dwFileAttributes = held.dwFileAttributes;
        data.ftCreationTime = held.ftCreationTime;
        data.ftLastAccessTime = held.ftLastAccessTime;
        data.ftLastWriteTime = held.ftLastWriteTime;
        data.nFileSizeHigh = held.nFileSizeHigh;
        data.nFileSizeLow = held.nFileSizeLow;
    }
    entry.directory = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    entry.reparse = (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
    entry.attributes = data.dwFileAttributes;
    entry.created = data.ftCreationTime;
    entry.accessed = data.ftLastAccessTime;
    entry.modified = data.ftLastWriteTime;
    ULARGE_INTEGER size{};
    size.HighPart = data.nFileSizeHigh;
    size.LowPart = data.nFileSizeLow;
    entry.bytes = entry.directory ? 0 : size.QuadPart;
    return true;
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
        std::wstring name = stem + L" - 副本";
        if (index > 1) name += L" (" + std::to_wstring(index) + L")";
        std::wstring candidate = JoinPath(parent, name + extension);
        if (!PathExists(candidate)) return candidate;
    }
    return JoinPath(parent, stem + L" - 副本 " + std::to_wstring(GetTickCount64()) + extension);
}

std::wstring UniqueTemporaryPath(const std::wstring& destination,
                                 const wchar_t* marker,
                                 uint64_t task_id,
                                 size_t index) {
    const std::wstring base = destination + marker + std::to_wstring(GetCurrentProcessId())
        + L"-" + std::to_wstring(task_id) + L"-" + std::to_wstring(index);
    if (!PathExists(base)) return base;
    for (unsigned suffix = 2; suffix < 10000; ++suffix) {
        const std::wstring candidate = base + L"-" + std::to_wstring(suffix);
        if (!PathExists(candidate)) return candidate;
    }
    return base + L"-" + std::to_wstring(GetTickCount64());
}

bool CopyReparsePoint(const TransferEntry& entry, DestinationGuard& guard, std::wstring& error,
                      HRESULT& failure_hr, std::wstring& failed_path) {
    auto fail = [&](DWORD code, const std::wstring& path) {
        failure_hr = HRESULT_FROM_WIN32(code ? code : ERROR_GEN_FAILURE);
        failed_path = path;
        error = Win32Message(HRESULT_CODE(failure_hr)) + L" | " + path;
    };
    HANDLE source = entry.source_identity.Get();
    const bool borrowed = source != nullptr;
    if (!source) source = CreateFileW(entry.source.c_str(), 0,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (source == INVALID_HANDLE_VALUE) {
        const DWORD code = GetLastError();
        fail(code, entry.source);
        return false;
    }
    std::vector<BYTE> buffer(MAXIMUM_REPARSE_DATA_BUFFER_SIZE);
    DWORD bytes = 0;
    const BOOL read = DeviceIoControl(source, FSCTL_GET_REPARSE_POINT, nullptr, 0,
        buffer.data(), static_cast<DWORD>(buffer.size()), &bytes, nullptr);
    const DWORD read_error = read ? ERROR_SUCCESS : GetLastError();
    if (!borrowed) CloseHandle(source);
    if (!read) {
        fail(read_error, entry.source);
        return false;
    }

    DestinationGuard::Handle destination;
    if (entry.directory) {
        destination = DestinationGuard::CreateDirectoryLeaf(entry.destination, true, error);
    } else {
        HANDLE file = CreateFileW(entry.destination.c_str(), GENERIC_WRITE | DELETE,
            FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL |
            FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (file != INVALID_HANDLE_VALUE) destination.reset(file);
    }
    if (!destination) {
        const DWORD code = GetLastError();
        if (error.empty()) fail(code, entry.destination);
        else { failure_hr = E_FAIL; failed_path = entry.destination; }
        return false;
    }
    DWORD written = 0;
    const BOOL set = DeviceIoControl(destination.get(), FSCTL_SET_REPARSE_POINT,
        buffer.data(), bytes, nullptr, 0, &written, nullptr);
    const DWORD set_error = set ? ERROR_SUCCESS : GetLastError();
    if (set) SetFileTime(destination.get(), &entry.created, &entry.accessed, &entry.modified);
    const bool adopted = set && guard.AdoptCreatedLeaf(entry.destination, destination.get(), error);
    if (!set || !adopted) {
        if (!set) fail(set_error, entry.destination);
        else { failure_hr = E_FAIL; failed_path = entry.destination; }
        FILE_DISPOSITION_INFO disposition{TRUE};
        SetFileInformationByHandle(destination.get(), FileDispositionInfo, &disposition, sizeof(disposition));
        return false;
    }
    return true;
}

bool StartsWithPath(const std::wstring& path, const std::wstring& prefix) {
    if (path.size() < prefix.size() || _wcsnicmp(path.c_str(), prefix.c_str(), prefix.size()) != 0)
        return false;
    return path.size() == prefix.size() || path[prefix.size()] == L'\\' || path[prefix.size()] == L'/';
}

bool Sha256File(const std::wstring& path, const std::atomic<bool>& cancel,
                const std::atomic<bool>& pause, std::array<uint8_t, 32>& digest,
                std::wstring& error) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD object_bytes = 0;
    DWORD hash_bytes = 0;
    DWORD returned = 0;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0 ||
        BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
                          reinterpret_cast<PUCHAR>(&object_bytes), sizeof(object_bytes),
                          &returned, 0) < 0 ||
        BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH,
                          reinterpret_cast<PUCHAR>(&hash_bytes), sizeof(hash_bytes),
                          &returned, 0) < 0 || hash_bytes != digest.size()) {
        if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
        error = l10n::Get(l10n::StringId::OperationHashInitializeFailed);
        return false;
    }
    std::vector<uint8_t> object(object_bytes);
    if (BCryptCreateHash(algorithm, &hash, object.data(), object_bytes,
                         nullptr, 0, 0) < 0) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        error = l10n::Get(l10n::StringId::OperationHashInitializeFailed);
        return false;
    }
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    bool ok = file != INVALID_HANDLE_VALUE;
    std::vector<uint8_t> buffer(1024 * 1024);
    while (ok && !cancel.load()) {
        while (pause.load() && !cancel.load()) Sleep(20);
        if (cancel.load()) break;
        DWORD read = 0;
        if (!ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr)) {
            ok = false;
            break;
        }
        if (read == 0) break;
        if (BCryptHashData(hash, buffer.data(), read, 0) < 0) {
            ok = false;
            break;
        }
    }
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    if (ok && !cancel.load() && BCryptFinishHash(hash, digest.data(),
                                                 static_cast<ULONG>(digest.size()), 0) < 0)
        ok = false;
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (!ok && error.empty()) error = Win32Message(GetLastError()) + L" | " + path;
    return ok && !cancel.load();
}

std::wstring JsonString(const std::wstring& value) {
    std::wstring escaped;
    json::Escape(value, escaped);
    return L"\"" + escaped + L"\"";
}

std::wstring StringArrayJson(const std::vector<std::wstring>& values) {
    std::wstring out = L"[";
    for (size_t i = 0; i < values.size(); ++i) {
        if (i) out += L",";
        out += JsonString(values[i]);
    }
    out += L"]";
    return out;
}

bool ParseRecoveryEntry(const std::wstring& object, RecoveryEntry& entry) {
    const json::Document doc(object);
    if (!doc.Is(json::Kind::Object)) return false;
    const int type = json::ExtractInt(doc, L"type", -1);
    const int policy = json::ExtractInt(doc, L"policy", -1);
    if (type < static_cast<int>(OpType::Copy) || type > static_cast<int>(OpType::BatchRename) ||
        policy < static_cast<int>(CollisionPolicy::System) ||
        policy > static_cast<int>(CollisionPolicy::KeepBoth)) return false;
    if (!json::ParseUnsigned(json::ExtractString(doc, L"seq"), entry.sequence) ||
        entry.sequence == UINT64_MAX) return false;
    entry.was_active = json::ExtractBool(doc, L"active", false);
    entry.outcome_unknown = json::ExtractBool(doc, L"outcome_unknown", false);
    entry.request.type = static_cast<OpType>(type);
    entry.request.collision_policy = static_cast<CollisionPolicy>(policy);
    entry.request.sources = json::ExtractStringArray(doc, L"sources");
    entry.request.recycle_paths = json::ExtractStringArray(doc, L"recycle_paths");
    entry.request.dest_dir = json::ExtractString(doc, L"dest");
    entry.request.new_name = json::ExtractString(doc, L"name");
    entry.request.new_names = json::ExtractStringArray(doc, L"names");
    entry.request.is_undo = json::ExtractBool(doc, L"undo", false);
    for (const auto& target_json : json::ExtractObjectArray(doc, L"delete_targets")) {
        DeleteRequestTarget target;
        target.path = json::ExtractString(target_json, L"path", L"");
        target.physical_paths = json::ExtractStringArray(target_json, L"roots");
        target.recycle_item = json::ExtractBool(target_json, L"recycle_item", false);
        entry.request.delete_targets.push_back(std::move(target));
    }
    if (entry.request.type == OpType::EmptyRecycle) return entry.sequence != 0;
    return entry.sequence != 0 && !entry.request.sources.empty();
}

} // namespace

OpsManager::~OpsManager() {
    Stop();
}

void OpsManager::SetJournalPath(std::wstring path) {
    if (running_) return;
    journal_path_ = std::move(path);
    LoadRecoveryJournal();
}

void OpsManager::LoadRecoveryJournal() {
    pending_recovery_.clear();
    if (journal_path_.empty()) return;
    std::wstring text;
    if (!ReadUtf8File(journal_path_, text)) return;
    const json::Document doc(text);
    if (!doc.Is(json::Kind::Object) || json::ExtractInt(doc, L"schema", 0) != 1) return;
    for (const auto& object : json::ExtractObjectArray(doc, L"entries")) {
        RecoveryEntry entry;
        if (ParseRecoveryEntry(object, entry)) {
            next_seq_ = std::max(next_seq_, entry.sequence + 1);
            pending_recovery_.push_back(std::move(entry));
        }
    }
}

RecoverySnapshot OpsManager::PendingRecovery() const {
    std::lock_guard<std::mutex> lock(mutex_);
    RecoverySnapshot result;
    result.entries = pending_recovery_;
    result.has_uncertain_destructive = std::any_of(
        result.entries.begin(), result.entries.end(), [](const RecoveryEntry& entry) {
            return entry.was_active && IsDeleteOperation(entry.request.type);
        });
    return result;
}

bool OpsManager::RetryRecovery() {
    bool all_retryable = true;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!accepting_) return false;
        for (const auto& recovery : pending_recovery_) {
            if (scheduled_recovery_.contains(recovery.sequence)) continue;
            if (recovery.outcome_unknown) {
                all_retryable = false;
                status_.last_error = l10n::Get(l10n::StringId::PermissionOperationOutcomeUnknown);
                status_.summary = status_.last_error; status_.phase = OpPhase::Failed;
                continue;
            }
            if (recovery.was_active && IsDeleteOperation(recovery.request.type)) {
                all_retryable = false;
                status_.last_error = L"A deletion may have partially executed. Inspect the affected paths before discarding this recovery record; it will not be replayed.";
                status_.summary = status_.last_error; status_.phase = OpPhase::Failed;
                continue;
            }
            if (recovery.request.is_undo && IsMutationOperation(recovery.request.type)) {
                all_retryable = false;
                status_.last_error = l10n::Get(l10n::StringId::OperationUndoReservationMissing);
                status_.summary = status_.last_error; status_.phase = OpPhase::Failed;
                continue;
            }
            // A matching .pulse-copy/.pulse-backup name is not ownership proof.
            // Recovery that first requires destructive reconciliation stops.
            if (!recovery.request.dest_dir.empty()) {
                all_retryable = false;
                status_.last_error = L"Recovery stopped: temporary-file ownership cannot be proven; nothing was deleted.";
                status_.summary = status_.last_error;
                status_.phase = OpPhase::Failed;
                continue;
            }
            QueueItem item;
            item.seq = next_seq_++;
            item.req = recovery.request;
            item.req.delete_origin = DeleteOrigin::Recovery;
            item.recovery_sequence = recovery.sequence;
            scheduled_recovery_.insert(recovery.sequence);
            queue_.push_back(std::move(item));
        }
    }
    cv_.notify_one();
    if (notify_) notify_();
    return all_retryable;
}

void OpsManager::DiscardRecovery() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        // Discard forgets the journal only. Never delete files by a temporary
        // name pattern, which may also match user-owned files.
        pending_recovery_.clear();
        scheduled_recovery_.clear();
        std::erase_if(queue_, [](const QueueItem& item) { return item.recovery_sequence != 0; });
    }
    if (!journal_path_.empty()) DeleteFileW(journal_path_.c_str());
    cv_.notify_one();
}

std::wstring OpsManager::JournalJsonLocked() const {
    std::wstring out = L"{\n  \"schema\":1,\n  \"entries\":[";
    bool first = true;
    auto append = [&](const QueueItem& item, bool active, bool outcome_unknown = false) {
        if (item.req.sources.empty() && item.req.type != OpType::EmptyRecycle) return;
        if (!first) out += L",";
        first = false;
        out += L"\n    {\"seq\":" + JsonString(std::to_wstring(item.seq)) +
            L",\"active\":" + (active ? std::wstring(L"true") : std::wstring(L"false")) +
            L",\"outcome_unknown\":" + ((outcome_unknown || (active && item.elevated_retry)) ? std::wstring(L"true") : std::wstring(L"false")) +
            L",\"type\":" + std::to_wstring(static_cast<int>(item.req.type)) +
            L",\"policy\":" + std::to_wstring(static_cast<int>(item.req.collision_policy)) +
            L",\"undo\":" + (item.req.is_undo ? std::wstring(L"true") : std::wstring(L"false")) +
            L",\"sources\":" + StringArrayJson(item.req.sources) +
            L",\"dest\":" + JsonString(item.req.dest_dir) +
            L",\"name\":" + JsonString(item.req.new_name) +
            L",\"names\":" + StringArrayJson(item.req.new_names) +
            L",\"recycle_paths\":" + StringArrayJson(item.req.recycle_paths);
        if (!item.req.delete_targets.empty()) {
            out += L",\"delete_targets\":[";
            for (size_t i = 0; i < item.req.delete_targets.size(); ++i) {
                const auto& target = item.req.delete_targets[i];
                if (i != 0) out += L",";
                out += L"{\"path\":" + JsonString(target.path) + L",\"roots\":" +
                    StringArrayJson(target.physical_paths) + L",\"recycle_item\":" +
                    (target.recycle_item ? L"true" : L"false") + L"}";
            }
            out += L"]";
        }
        out += L"}";
    };
    if (active_item_) append(*active_item_, true);
    for (const auto& item : queue_)
        if (!IsDeleteOperation(item.req.type) && item.recovery_sequence == 0) append(item, false);
    for (const auto& recovery : pending_recovery_) {
        if (active_item_ && active_item_->recovery_sequence == recovery.sequence) continue;
        QueueItem item;
        item.seq = recovery.sequence; item.req = recovery.request;
        append(item, recovery.was_active, recovery.outcome_unknown);
    }
    out += first ? L"]\n}\n" : L"\n  ]\n}\n";
    return out;
}

void OpsManager::PersistJournal() {
    if (journal_path_.empty()) return;
    std::wstring text;
    bool empty = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        empty = !active_item_ && pending_recovery_.empty() &&
            std::none_of(queue_.begin(), queue_.end(), [](const QueueItem& item) {
                return !IsDeleteOperation(item.req.type);
            });
        if (!empty) text = JournalJsonLocked();
    }
    if (empty) DeleteFileW(journal_path_.c_str());
    else WriteUtf8FileAtomic(journal_path_, text);
}

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
    if (ReserveOpenWorker()) {
        auto mailbox = std::make_shared<OpenMailbox>();
        mailbox->owner.store(ui_hwnd_.load());
        {
            std::lock_guard<std::mutex> lock(open_mutex_);
            open_mailbox_ = mailbox;
        }
    }
    StartShellClient();
    thread_ = std::thread([this] { WorkerThread(); });
    menu_thread_ = std::thread([this] { MenuThread(); });
    if (const auto mailbox = open_mailbox_) {
        try {
            open_thread_ = std::thread([mailbox] { OpenThread(mailbox); });
        } catch (...) {
            std::lock_guard<std::mutex> lock(open_mutex_);
            open_mailbox_.reset();
            open_workers.fetch_sub(1);
        }
    }
}

bool OpsManager::HasPendingFileOperations() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return file_operation_in_flight_ || std::any_of(queue_.begin(), queue_.end(),
        [](const QueueItem& item) { return item.open_path.empty(); });
}

void OpsManager::BeginShutdown() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        accepting_ = false;
        running_ = false;
        for (const auto& item : queue_) FinishMutationUndoLocked(item.req);
        queue_.clear();
        pending_recovery_.clear();
        scheduled_recovery_.clear();
    }
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
    if (!thread_.joinable() && !menu_thread_.joinable() && !open_thread_.joinable()) return;
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
    }
    CancelCurrent(); // unblock an in-flight RunShellOp via DONE(cancelled)
    cv_.notify_all();
    done_cv_.notify_all();
    if (thread_.joinable()) thread_.join();
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
        std::lock_guard<std::mutex> lock(mutex_);
        if (!accepting_) return 0;
        seq = next_seq_++;
        item.seq = seq;
        queue_.push_back(std::move(item));
    }
    if (notify_) notify_();
    cv_.notify_one();
    return seq;
}

void OpsManager::OpenWith(const std::wstring& path) {
    QueueItem item;
    item.open_path = path;
    item.open_verb = L"open";
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
    item.open_args = L"\"" + file + L"\"";
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
    std::wstring quoted = L"\"";
    size_t slashes = 0;
    for (const wchar_t c : dir) {
        if (c == L'\\') {
            ++slashes;
            continue;
        }
        if (c == L'\"') {
            quoted.append(slashes * 2 + 1, L'\\');
            quoted.push_back(c);
        } else {
            quoted.append(slashes, L'\\');
            quoted.push_back(c);
        }
        slashes = 0;
    }
    // Backslashes immediately before a closing quote must be doubled.
    quoted.append(slashes * 2, L'\\');
    quoted.push_back(L'\"');
    return L"-d " + quoted;
}

void OpsManager::OpenTerminal(const std::wstring& dir) {
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
    if (!transfer_active_.load() || transfer_cancel_.load()) return;
    transfer_pause_.store(true);
}

void OpsManager::ResumeCurrent() {
    if (!transfer_active_.load()) return;
    transfer_pause_.store(false);
    transfer_control_cv_.notify_all();
}

std::optional<ConflictItemInfo> OpsManager::PendingConflict() const {
    std::lock_guard<std::mutex> lock(transfer_control_mutex_);
    return pending_conflict_;
}

void OpsManager::ResolveConflict(uint64_t token, ConflictChoice choice, bool apply_to_all) {
    {
        std::lock_guard<std::mutex> lock(transfer_control_mutex_);
        if (!pending_conflict_ || pending_conflict_->token != token) return;
        resolved_conflict_token_ = token;
        resolved_conflict_choice_ = choice;
        resolved_conflict_apply_all_ = apply_to_all;
    }
    transfer_control_cv_.notify_all();
}

OpStatus OpsManager::Status() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return PresentOperationStatus(status_);
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
    LockReport report;
    if (failed_item.empty() && !retry.retry_probe_paths.empty()) {
        // Unknown CopyFile2 phase: inspect each endpoint independently so a
        // destination lock is not presented as a source lock.
        for (const auto& path : retry.retry_probe_paths) {
            auto endpoint = ProbeFileLocks(error, path, {path}, LockProtectedDirectory(), cancelled);
            if (report.path.empty()) report.path = endpoint.path;
            for (auto& owner : endpoint.owners)
                if (std::none_of(report.owners.begin(), report.owners.end(), [&](const auto& prior) {
                    return prior.pid == owner.pid;
                })) report.owners.push_back(std::move(owner));
            if (cancelled && cancelled()) return {};
        }
    } else {
        report = ProbeFileLocks(error, failed_item, retry.req.sources, LockProtectedDirectory(), cancelled);
    }
    const bool plain_transfer_retry = (retry.req.type == OpType::Copy || retry.req.type == OpType::Move) && IsLockFailure(error);
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

void OpsManager::ExecuteFileOperation(QueueItem& item) {
    struct UndoCompletion {
        OpsManager& manager;
        const OpRequest& request;
        ~UndoCompletion() { manager.FinishMutationUndo(request); }
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
    if (item.elevated_retry) {
        OperationElevationAdapter::RunParent(*this, item);
        return;
    }
    if (!PrepareLockRetry(item)) return;
    if (IsDeleteOperation(item.req.type)) RunDelete(item);
    else if (item.req.type == OpType::Copy || item.req.type == OpType::Move)
        RunTransfer(item.req, item.seq, item.retry_completed_sources, item.retry_transfer_plan,
            item.retry_source_directories, item.retry_destination_guard);
    else RunShellOp(item.req, item.seq, item.retry_rename_identity ? &item.rename_source_id : nullptr, item.lock_retry);
}

void OpsManager::SetStatus(const std::function<void(OpStatus&)>& fn) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        fn(status_);
        RefreshElevationStatusLocked();
    }
    if (notify_) notify_();
}

bool OpsManager::CanUndo() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (undo_.empty() || !undo_.back().supported || pending_mutation_undo_revision_) return false;
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
    const auto description = Describe(r);
    const auto& pattern = l10n::Get(l10n::StringId::OperationUndoFormat);
    const int length = _scwprintf(pattern.c_str(), description.c_str());
    if (length <= 0) return {};
    std::vector<wchar_t> text(static_cast<size_t>(length) + 1);
    swprintf_s(text.data(), text.size(), pattern.c_str(), description.c_str());
    return std::wstring(text.data(), static_cast<size_t>(length));
}

void OpsManager::RefreshRecycleUndoValidity() {
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
                ValidateRecycleUndoPair(entry.sources[i], entry.destinations[i], identities[i], expected);
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
                          const std::vector<FileIdentity>* produced_identities) {
    CompletedOperation completed;
    completed.type = req.type;
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
    const auto destinations = completed.destinations;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        completions_.push_back(std::move(completed));
    }
    if (req.type == OpType::Rename && req.sources.size() == 1 &&
        pulse::path::IsDriveRootPath(req.sources.front())) return;
    if (req.is_undo) {
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
    e.sources = req.sources;
    e.destinations = destinations;
    e.dest_dir = req.dest_dir;
    e.new_name = req.new_name;
    if (RequiresProducedIdentity(req.type)) {
        const size_t count = req.type == OpType::Copy ? e.destinations.size() : e.sources.size();
        if (produced_identities) e.produced_identities = *produced_identities;
        e.supported = HasProducedIdentities(count, e.produced_identities) &&
            (!IsMutationOperation(req.type) || e.destinations.size() == count);
        if (!e.supported) e.produced_identities.clear();
    }
    if (req.type == OpType::RealDelete) return;              // never undoable, not recorded
    if (req.type == OpType::RestoreRecycle) return;
    if (req.type == OpType::EmptyRecycle) return;
    std::lock_guard<std::mutex> lock(mutex_);
    undo_.push_back(std::move(e));
    ++undo_revision_;
}

void OpsManager::FinishMutationUndo(const OpRequest& req) {
    if (!req.is_undo || !IsMutationOperation(req.type) || !req.undo_revision) return;
    bool changed = false;
    {
        std::lock_guard lock(mutex_);
        changed = FinishMutationUndoLocked(req);
    }
    if (changed && notify_) notify_();
}

bool OpsManager::FinishMutationUndoLocked(const OpRequest& req) {
    if (!req.is_undo || !IsMutationOperation(req.type) || !req.undo_revision) return false;
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
        if (undo_.empty() || pending_mutation_undo_revision_) return;
        e = undo_.back();
        if (e.type == OpType::RecycleDelete && !e.recycle_verified && e.supported) {
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
        } else if (IsMutationOperation(e.type)) {
            deletion_reservation = undo_revision_;
            pending_mutation_undo_revision_ = deletion_reservation;
        } else if (e.type != OpType::Copy && e.type != OpType::CreateFolder &&
                   e.type != OpType::CreateTextFile) {
            undo_.pop_back();
            ++undo_revision_;
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
        inv.undo_produced_identities = e.produced_identities;
        inv.sources = e.sources;
        Submit(std::move(inv));
        break;
    }
    case OpType::RecycleDelete: {
        OpRequest inv;
        inv.type = OpType::RestoreRecycle;
        inv.is_undo = true;
        inv.sources = e.sources;
        inv.recycle_paths = e.destinations;
        inv.recycle_undo_identities = e.recycle_identities;
        Submit(std::move(inv));
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
        ipc::ShellClient::Instance().Launch(request);
    }
    {
        std::lock_guard lock(mailbox->mutex);
        mailbox->finished = true;
    }
    mailbox->finished_cv.notify_all();
}

void OpsManager::StartShellClient() {
    const auto mailbox = open_mailbox_;
    const auto notify = notify_;
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
    cb.progress = [this](uint32_t, float pct, std::wstring item,
                         uint32_t items_done, uint32_t total_items) {
        shell_activity_tick_ = GetTickCount64();
        SetStatus([&](OpStatus& st) {
            st.percent = pct;
            st.current_item = item;
            if (!delete_active_.load()) {
                if (total_items > 0) st.total_items = total_items;
                st.completed_items = (std::min)(static_cast<uint64_t>(items_done), st.total_items);
            }
            if (!item.empty()) {
                std::wstring base = st.summary;
                auto sep = base.find(L"  (");
                if (sep != std::wstring::npos) base.erase(sep);
                wchar_t buf[32];
                swprintf_s(buf, L"  (%.0f%%)", pct);
                st.summary = base + buf;
            }
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
    ipc::ShellClient::Instance().Start(cb);
}

void OpsManager::WorkerThread() {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);



    for (;;) {
        QueueItem item;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait_for(lock, std::chrono::seconds(1), [this] {
                return !queue_.empty() || !running_ || undo_validation_requested_.load();
            });
            if (!running_) break;
            if (queue_.empty()) {
                lock.unlock();
                RefreshRecycleUndoValidity();
                continue;
            }
            {
                item = std::move(queue_.front());
                queue_.pop_front();
                file_operation_in_flight_ = item.open_path.empty();
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
        }
        if (!IsDeleteOperation(item.req.type)) PersistJournal();
    }

    ipc::ShellClient::Instance().Stop();
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
        case MenuJob::Kind::Query: {
            const uint32_t id = client.QueryContextMenu(
                job.paths, job.owner_hwnd, job.background, job.extended,
                job.disabled_clsids, [&](uint32_t request_id) {
                    RegisterShellMenuQuery(job.token, request_id);
                });
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
                });
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

void OpsManager::RunTransfer(const OpRequest& req, uint64_t task_id,
    const std::vector<std::wstring>& prior_completed, const std::vector<TransferPlanEntry>& prior_plan,
    const std::shared_ptr<DestinationGuard>& prior_source_directories,
    const std::shared_ptr<DestinationGuard>& prior_destination_guard) {
    const auto cleanup_only = [&](const std::wstring& source) {
        return std::find(req.undo_cleanup_directories.begin(), req.undo_cleanup_directories.end(), source) !=
            req.undo_cleanup_directories.end();
    };
    {
        std::lock_guard<std::mutex> lock(mutex_);
        transfer_active_.store(true);
        transfer_cancel_.store(stopping_.load() || lock_retry_cancel_.load());
        transfer_pause_.store(false);
    }
    {
        std::lock_guard<std::mutex> lock(transfer_control_mutex_);
        pending_conflict_.reset();
        resolved_conflict_token_ = 0;
    }

    SetStatus([&](OpStatus& st) {
        st.active = true;
        st.type = req.type;
        st.task_id = task_id;
        st.phase = OpPhase::Scanning;
        st.percent = -1.0f;
        st.summary = l10n::Format(l10n::StringId::OperationPreparingFormat, OpVerb(req.type).c_str());
        st.last_error.clear();
        st.source_label = req.sources.empty() ? L"" : FileName(req.sources.front());
        st.destination_label = FileName(req.dest_dir);
        if (st.destination_label.empty()) st.destination_label = req.dest_dir;
        st.current_item.clear();
        st.total_bytes = st.transferred_bytes = 0;
        st.total_items = st.completed_items = 0;
        st.bytes_per_second = st.peak_bytes_per_second = 0.0;
        st.eta_seconds = 0;
    });

    std::wstring failure;
    HRESULT failure_hr = S_OK;
    std::wstring failed_path;
    std::vector<std::wstring> failed_candidates;
    auto fail_win32 = [&](DWORD code, const std::wstring& path) {
        failure_hr = HRESULT_FROM_WIN32(code ? code : ERROR_GEN_FAILURE);
        failed_path = path;
        failure = Win32Message(HRESULT_CODE(failure_hr)) + L" | " + path;
    };
    bool cancelled = stopping_.load() || transfer_cancel_.load();
    std::vector<TransferEntry> entries;
    auto source_directories = prior_source_directories ? prior_source_directories
        : std::make_shared<DestinationGuard>(transfer_identity_query_);
    struct SourceLeases {
        DestinationGuard& guard;
        ~SourceLeases() { guard.ReleaseDirectories(); }
    } source_leases{*source_directories};
    const auto admit_source = [&](const std::wstring& path) {
        if (source_directories->Capture(ParentOf(path), failure) &&
            source_directories->PinDirectories(ParentOf(path), failure)) return true;
        fail_win32(ERROR_FILE_INVALID, path);
        return false;
    };
    const auto protect_directory = [&](const TransferEntry& entry) {
        if (source_directories->PinPlainDirectory(entry.source, entry.source_identity.identity, failure)) return true;
        fail_win32(ERROR_FILE_INVALID, entry.source);
        return false;
    };
    const auto delete_source = [&](const TransferEntry& entry, DWORD& error) {
        source_directories->ReleaseDirectory(entry.source);
        if (before_source_cleanup_) before_source_cleanup_(entry.source);
        if (!entry.source_identity.Delete(entry.source, error, transfer_identity_query_)) return false;
        source_directories->ForgetLeaf(entry.source);
        return true;
    };
    std::vector<bool> root_destination_preexisting(req.sources.size(), false);
    std::vector<std::wstring> completed_sources;
    std::vector<std::wstring> completed_destinations;
    std::vector<std::wstring> skipped_sources;
    std::vector<std::wstring> removed_directories;
    struct ReplacementBackup {
        std::wstring original;
        std::wstring backup;
    };
    std::vector<ReplacementBackup> replacement_backups;

    if (req.sources.empty() || req.dest_dir.empty()) {
        failure = l10n::Get(l10n::StringId::OperationTransferMissingPaths);
    }

    bool plan_complete = !prior_plan.empty();
    if (failure.empty() && !cancelled && !prior_plan.empty()) {
        for (const auto& planned : prior_plan) {
            if (std::any_of(prior_completed.begin(), prior_completed.end(), [&](const auto& done) {
                    return StartsWithPath(planned.source, done);
                })) continue;
            TransferEntry current = planned;
            if (!admit_source(planned.source)) break;
            if (!ReadEntryMetadata(planned.source, current, transfer_identity_query_,
                req.type == OpType::Move || planned.source_identity.Get())) {
                const DWORD code = GetLastError();
                fail_win32(code, planned.source);
                break;
            }
            if (current.directory != planned.directory || current.reparse != planned.reparse ||
                ((req.type == OpType::Move || current.directory || planned.source_identity.Get()) &&
                    !planned.source_identity.Matches(current.source_identity.Get(), transfer_identity_query_))) {
                failure_hr = HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
                failed_path = planned.source;
                failure = l10n::Format(l10n::StringId::OperationRetryTypeChangedFormat, planned.source.c_str());
                break;
            }
            current.source_identity = planned.source_identity;
            if (current.directory && !current.reparse && !current.directory_rename_only && !cleanup_only(current.source) &&
                !protect_directory(current)) break;
            entries.push_back(std::move(current));
        }
        for (size_t i = 0; i < req.sources.size(); ++i) {
            const auto root = std::find_if(prior_plan.begin(), prior_plan.end(), [&](const auto& entry) {
                return entry.source == req.sources[i];
            });
            if (root != prior_plan.end()) root_destination_preexisting[i] = root->destination_preexisting;
        }
    } else if (failure.empty() && !cancelled) {
        for (size_t source_index = 0; source_index < req.sources.size(); ++source_index) {
            if (transfer_cancel_.load() || stopping_.load()) { cancelled = true; break; }
            const auto& source = req.sources[source_index];
            if (std::any_of(prior_completed.begin(), prior_completed.end(), [&](const auto& done) {
                    return _wcsicmp(source.c_str(), done.c_str()) == 0;
                })) continue;
            TransferEntry root;
            root.source = source;
            root.destination = req.type == OpType::Move && req.is_undo &&
                source_index < req.new_names.size() ? req.new_names[source_index]
                : JoinPath(req.dest_dir, FileName(source));
            if (!admit_source(source)) break;
            if (!ReadEntryMetadata(source, root, transfer_identity_query_, req.type == OpType::Move)) {
                const auto error = GetLastError();
                fail_win32(error, source);
                break;
            }
            if (req.is_undo && req.type == OpType::Move && (source_index >= req.undo_produced_identities.size() ||
                root.source_identity.identity != req.undo_produced_identities[source_index])) {
                failure_hr = E_ACCESSDENIED;
                failed_path = source;
                failure = l10n::Format(l10n::StringId::OperationUndoResultChangedFormat, source.c_str());
                break;
            }
            const bool same_location =
                _wcsicmp(root.source.c_str(), root.destination.c_str()) == 0;
            if (same_location && req.type == OpType::Move) {
                // Dropping an item onto its current folder is a no-op, like Explorer.
                continue;
            }
            if (same_location && req.type == OpType::Copy) {
                root.destination = UniqueCopyPath(root.destination, root.directory);
            }
            if (root.directory && StartsWithPath(req.dest_dir, root.source)) {
                failure = l10n::Format(l10n::StringId::OperationNestedDestinationFormat, source.c_str());
                break;
            }
            root.destination_preexisting = PathExists(root.destination);
            root_destination_preexisting[source_index] = root.destination_preexisting;
            if (!force_copy_move_ && req.type == OpType::Move && root.directory && !root.reparse && !cleanup_only(source) &&
                !root.destination_preexisting) {
                DestinationGuard::Handle destination_parent(CreateFileW(req.dest_dir.c_str(), 0,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                    FILE_FLAG_BACKUP_SEMANTICS, nullptr));
                FileIdentity destination_identity;
                if (destination_parent.get() == INVALID_HANDLE_VALUE) destination_parent.release();
                if (destination_parent && ReadFileIdentity(destination_parent.get(), destination_identity, transfer_identity_query_) &&
                    destination_identity.extended == root.source_identity.identity.extended &&
                    destination_identity.value.VolumeSerialNumber == root.source_identity.identity.value.VolumeSerialNumber)
                    root.directory_rename_only = true;
            }
            entries.push_back(root);
            if (!root.directory || root.reparse || root.directory_rename_only || cleanup_only(source)) continue;
            if (before_source_enumeration_) before_source_enumeration_(source);
            if (!protect_directory(root)) break;

            struct ScanFrame {
                std::wstring source, destination;
                fs::DirectoryEnumerator cursor;
                ScanFrame(std::wstring s, std::wstring d) : source(std::move(s)),
                    destination(std::move(d)), cursor(source) {}
            };
            std::vector<ScanFrame> scan;
            scan.emplace_back(source, root.destination);
            while (!scan.empty()) {
                auto& frame = scan.back();
                fs::DirEntry item;
                fs::DirectoryRead read;
                try {
                    read = frame.cursor.Next(item, [&] { return transfer_cancel_.load() || stopping_.load(); });
                } catch (const fs::EnumerationError& error) {
                    fail_win32(error.Code(), frame.source);
                    break;
                }
                if (read == fs::DirectoryRead::Cancelled) { cancelled = true; break; }
                if (read == fs::DirectoryRead::End) { scan.pop_back(); continue; }
                TransferEntry child;
                child.source = JoinPath(frame.source, item.name);
                if (std::any_of(prior_completed.begin(), prior_completed.end(), [&](const auto& done) {
                        return _wcsicmp(child.source.c_str(), done.c_str()) == 0;
                    })) continue;
                child.destination = JoinPath(frame.destination, item.name);
                // Re-read metadata: admission still uses the current source, not
                // a potentially stale directory record (including reparse type).
                if (!admit_source(child.source)) break;
                if (!ReadEntryMetadata(child.source, child, transfer_identity_query_, req.type == OpType::Move)) {
                    const auto error = GetLastError();
                    fail_win32(error, child.source);
                    break;
                }
                if (req.undo_revision && std::any_of(entries.begin(), entries.end(), [&](const auto& prior) {
                        return prior.source == child.source;
                    })) {
                    if (child.directory && !child.reparse) {
                        if (!protect_directory(child)) break;
                        scan.emplace_back(child.source, child.destination);
                    }
                    continue;
                }
                entries.push_back(std::move(child));
                if (entries.back().directory && !entries.back().reparse) {
                    if (before_source_enumeration_) before_source_enumeration_(entries.back().source);
                    if (!protect_directory(entries.back())) break;
                    scan.emplace_back(entries.back().source, entries.back().destination);
                }
            }
            if (!failure.empty()) break;
        }
        plan_complete = failure.empty() && !cancelled;
    }

    // Accidental same-folder drops leave zero work. Do not flash the status bar
    // with "移动 foo → dest 完成" for a no-op Explorer already ignores.
    if (failure.empty() && !cancelled && entries.empty()) {
        SetStatus([&](OpStatus& st) {
            st.completed_ops++;
            st.active = false;
            st.phase = OpPhase::Completed;
            st.percent = 100.0f;
            st.summary.clear();
            st.last_error.clear();
            st.current_item.clear();
            st.total_bytes = st.transferred_bytes = 0;
            st.total_items = st.completed_items = 0;
            st.bytes_per_second = st.peak_bytes_per_second = 0.0;
            st.eta_seconds = 0;
        });
        transfer_active_.store(false);
        return;
    }

    auto destination_snapshot = prior_destination_guard ? prior_destination_guard
        : std::make_shared<DestinationGuard>(transfer_identity_query_, source_directories->SharedDirectoryLeases());
    auto& destination_guard = *destination_snapshot;
    auto guard_result = [&](bool accepted, const std::wstring& path) {
        if (!accepted) {
            // Identity/admission refusals remain failures, not guessed Win32
            // lock errors. PinLeaf supplies its actual API code separately.
            if (SUCCEEDED(failure_hr)) failure_hr = E_FAIL;
            if (failed_path.empty()) failed_path = path;
        }
        return accepted;
    };
    for (const auto& entry : entries) {
        if (cleanup_only(entry.source)) continue;
        if (!failure.empty() || !guard_result(destination_guard.Capture(entry.destination, failure), entry.destination)) break;
    }
    bool continue_all_links = false; // task-local; unrelated to collision policy
    for (const auto& impact : destination_guard.Impacts()) {
        if (!failure.empty() || cancelled) break;
        if (!continue_all_links) {
            ConflictItemInfo info;
            info.task_id = task_id;
            info.link_confirmation = true;
            info.link_impact = impact;
            info.destination = impact.path;
            info.remaining = destination_guard.Impacts().size();
            {
                std::lock_guard<std::mutex> lock(transfer_control_mutex_);
                info.token = next_conflict_token_++;
                const uint64_t token = info.token;
                info.still_valid = [this, token] {
                    std::lock_guard<std::mutex> lock(transfer_control_mutex_);
                    return pending_conflict_ && pending_conflict_->token == token &&
                        !transfer_cancel_.load() && !stopping_.load();
                };
                pending_conflict_ = info;
                resolved_conflict_token_ = 0;
            }
            SetStatus([&](OpStatus& st) {
                st.phase = OpPhase::WaitingForConflict;
                st.summary = l10n::Get(l10n::StringId::OperationWaitingLink);
            });
            std::unique_lock<std::mutex> lock(transfer_control_mutex_);
            const bool answered = transfer_control_cv_.wait_for(lock, std::chrono::minutes(2), [&] {
                return transfer_cancel_.load() || stopping_.load() || resolved_conflict_token_ == info.token;
            });
            if (!answered || transfer_cancel_.load() || stopping_.load() ||
                resolved_conflict_choice_ != ConflictChoice::Continue) cancelled = true;
            else continue_all_links = true;
            pending_conflict_.reset();
        }
    }
    // Pin only after the modal decision, then compare every scanned descendant.
    // Accepting the task's link destinations never accepts an identity change.
    for (const auto& entry : entries) {
        if (!failure.empty() || cancelled) break;
        if (cleanup_only(entry.source)) continue;
        if (!guard_result(destination_guard.PinDirectories(ParentOf(entry.destination), failure), ParentOf(entry.destination)) ||
            !guard_result(destination_guard.Check(entry.destination, failure), entry.destination)) break;
    }

    uint64_t total_bytes = 0;
    for (const auto& entry : entries) total_bytes += entry.bytes;
    SetStatus([&](OpStatus& st) {
        st.total_bytes = total_bytes;
        st.total_items = entries.size();
        if (failure.empty()) {
            st.phase = OpPhase::Running;
            st.percent = 0.0f;
            st.summary = l10n::Format(l10n::StringId::OperationProgressFormat,
                    OpVerb(req.type).c_str(), static_cast<unsigned long long>(entries.size()));
        }
    });

    bool apply_all = req.collision_policy != CollisionPolicy::System;
    ConflictChoice repeated = req.collision_policy == CollisionPolicy::KeepBoth
        ? ConflictChoice::KeepBoth : ConflictChoice::Replace;
    std::vector<std::wstring> skipped_prefixes;
    std::vector<std::wstring> renamed_directories;
    auto already_renamed = [&](const std::wstring& source) {
        return std::any_of(renamed_directories.begin(), renamed_directories.end(), [&](const auto& prefix) {
            return StartsWithPath(source, prefix);
        });
    };
    uint64_t committed_bytes = 0;
    uint64_t published_bytes = 0;
    uint64_t processed_items = 0;
    TransferRateEstimator rate;
    rate.Reset(GetTickCount64(), 0);

    auto publish_progress = [&](const TransferEntry& entry, uint64_t file_done,
                                uint64_t file_total, uint64_t completed_items) {
        const uint64_t bounded_file_done = file_total > 0
            ? (std::min)(file_done, file_total) : file_done;
        uint64_t overall = committed_bytes + bounded_file_done;
        if (overall < committed_bytes) overall = UINT64_MAX;
        overall = (std::max)(published_bytes, overall);
        if (total_bytes > 0) overall = (std::min)(overall, total_bytes);
        published_bytes = overall;
        const ULONGLONG now = GetTickCount64();
        rate.Observe(now, overall, total_bytes);
        SetStatus([&](OpStatus& st) {
            st.current_item = FileName(entry.source);
            st.transferred_bytes = overall;
            st.completed_items = completed_items;
            st.bytes_per_second = rate.speed();
            st.peak_bytes_per_second = (std::max)(st.peak_bytes_per_second, rate.speed());
            if (st.total_bytes > 0) {
                st.percent = static_cast<float>((std::min)(100.0,
                    static_cast<double>(overall) * 100.0 / st.total_bytes));
                st.eta_seconds = rate.eta_seconds();
            } else if (st.total_items > 0) {
                st.percent = static_cast<float>(completed_items * 100.0 / st.total_items);
            }
        });
    };

    auto reset_rate = [&] {
        rate.Reset(GetTickCount64(), published_bytes);
        SetStatus([&](OpStatus& st) {
            st.bytes_per_second = 0.0;
            st.eta_seconds = 0;
        });
    };

    auto mark_skipped = [&](const TransferEntry& entry) {
        skipped_sources.push_back(entry.source);
        total_bytes -= (std::min)(total_bytes, entry.bytes);
        ++processed_items;
        rate.Observe(GetTickCount64(), published_bytes, total_bytes);
        SetStatus([&](OpStatus& st) {
            st.total_bytes = total_bytes;
            st.transferred_bytes = published_bytes;
            st.completed_items = processed_items;
            st.current_item = FileName(entry.source);
            st.eta_seconds = rate.eta_seconds();
            if (st.total_bytes > 0) {
                st.percent = static_cast<float>((std::min)(100.0,
                    static_cast<double>(published_bytes) * 100.0 / st.total_bytes));
            } else if (st.total_items > 0) {
                st.percent = static_cast<float>(processed_items * 100.0 / st.total_items);
            }
        });
    };

    auto remaining_conflicts = [&](size_t start) {
        size_t count = 0;
        for (size_t j = start; j < entries.size(); ++j) {
            bool dest_dir = false;
            if (PathExists(entries[j].destination, &dest_dir) &&
                !(entries[j].directory && !entries[j].reparse && dest_dir)) ++count;
        }
        return count;
    };

    for (size_t index = 0; failure.empty() && !cancelled && index < entries.size(); ++index) {
        auto& entry = entries[index];
        if (transfer_cancel_.load()) { cancelled = true; break; }
        if (already_renamed(entry.source)) {
            ++processed_items;
            publish_progress(entry, entry.bytes, entry.bytes, processed_items);
            committed_bytes += entry.bytes;
            continue;
        }
        bool skipped = false;
        for (const auto& prefix : skipped_prefixes) {
            if (StartsWithPath(entry.source, prefix)) { skipped = true; break; }
        }
        if (skipped) {
            mark_skipped(entry);
            continue;
        }

        if (cleanup_only(entry.source)) {
            DWORD error = ERROR_SUCCESS;
            if (!entry.directory || entry.reparse) { fail_win32(ERROR_INVALID_DATA, entry.source); break; }
            if (!delete_source(entry, error)) {
                fail_win32(error, entry.source);
                break;
            }
            completed_sources.push_back(entry.source);
            completed_destinations.push_back(entry.destination);
            removed_directories.push_back(entry.source);
            ++processed_items;
            publish_progress(entry, 0, 0, processed_items);
            continue;
        }

        bool destination_is_directory = false;
        bool destination_exists = PathExists(entry.destination, &destination_is_directory);
        ConflictChoice choice = repeated;
        const bool conflict = destination_exists &&
            !(entry.directory && !entry.reparse && destination_is_directory);
        bool selected_apply_all = false;
        if (conflict && !apply_all) {
            ConflictItemInfo info;
            info.task_id = task_id;
            info.source = entry.source;
            info.destination = entry.destination;
            info.source_size = entry.bytes;
            info.source_modified = entry.modified;
            info.source_is_directory = entry.directory;
            TransferEntry destination_entry;
            if (ReadEntryMetadata(entry.destination, destination_entry)) {
                info.destination_size = destination_entry.bytes;
                info.destination_modified = destination_entry.modified;
                info.destination_is_directory = destination_entry.directory;
            }
            info.remaining = remaining_conflicts(index);
            {
                std::lock_guard<std::mutex> lock(transfer_control_mutex_);
                info.token = next_conflict_token_++;
                const uint64_t token = info.token;
                info.still_valid = [this, token] {
                    std::lock_guard<std::mutex> lock(transfer_control_mutex_);
                    return pending_conflict_ && pending_conflict_->token == token &&
                        !transfer_cancel_.load() && !stopping_.load();
                };
                pending_conflict_ = info;
                resolved_conflict_token_ = 0;
            }
            SetStatus([&](OpStatus& st) {
                st.phase = OpPhase::WaitingForConflict;
                st.current_item = FileName(entry.source);
                st.summary = l10n::Get(l10n::StringId::OperationWaitingConflict);
                st.bytes_per_second = 0.0;
                st.eta_seconds = 0;
            });
            std::unique_lock<std::mutex> lock(transfer_control_mutex_);
            transfer_control_cv_.wait(lock, [&] {
                return transfer_cancel_.load() || stopping_.load() || resolved_conflict_token_ == info.token;
            });
            if (transfer_cancel_.load() || stopping_.load()) {
                pending_conflict_.reset();
                cancelled = true;
                break;
            }
            choice = resolved_conflict_choice_;
            selected_apply_all = resolved_conflict_apply_all_;
            pending_conflict_.reset();
            lock.unlock();
            if (choice == ConflictChoice::Cancel) {
                cancelled = true;
                transfer_cancel_.store(true);
                break;
            }
            if (selected_apply_all) {
                apply_all = true;
                repeated = choice;
            }
            rate.Reset(GetTickCount64(), published_bytes);
            SetStatus([&](OpStatus& st) {
                st.phase = OpPhase::Running;
                st.summary = l10n::Format(l10n::StringId::OperationProgressFormat,
                    OpVerb(req.type).c_str(), static_cast<unsigned long long>(entries.size()));
            });
        }

        if (conflict && choice == ConflictChoice::Skip) {
            if (entry.directory) skipped_prefixes.push_back(entry.source);
            mark_skipped(entry);
            continue;
        }
        if (!guard_result(destination_guard.Check(entry.destination, failure), entry.destination)) break;
        if (conflict && choice == ConflictChoice::KeepBoth) {
            if (req.undo_revision) {
                failure = l10n::Format(l10n::StringId::OperationUndoExactNameFormat, entry.destination.c_str());
                break;
            }
            const std::wstring old_destination = entry.destination;
            const std::wstring unique = UniqueCopyPath(entry.destination, entry.directory);
            entry.destination = unique;
            if (entry.directory) {
                for (size_t j = index + 1; j < entries.size(); ++j) {
                    if (StartsWithPath(entries[j].destination, old_destination))
                        entries[j].destination = unique + entries[j].destination.substr(old_destination.size());
                }
            }
            destination_exists = false;
            destination_is_directory = false;
            const size_t admitted_impacts = destination_guard.Impacts().size();
            for (size_t j = index; j < entries.size(); ++j) {
                if (StartsWithPath(entries[j].destination, unique) &&
                    !guard_result(destination_guard.Capture(entries[j].destination, failure), entries[j].destination)) break;
            }
            if (destination_guard.Impacts().size() != admitted_impacts)
                failure = l10n::Format(l10n::StringId::OperationKeepBothLinkFormat, unique.c_str());
            if (!failure.empty() || !guard_result(destination_guard.PinDirectories(ParentOf(entry.destination), failure), ParentOf(entry.destination)) ||
                !guard_result(destination_guard.Check(entry.destination, failure), entry.destination)) break;
        }
        const bool destination_reparse = destination_exists &&
            (GetFileAttributesW(entry.destination.c_str()) & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
        if (conflict && choice == ConflictChoice::Replace &&
            (entry.directory != destination_is_directory || entry.reparse || destination_reparse)) {
            if (req.type == OpType::Move) {
                failure = l10n::Format(l10n::StringId::OperationMoveReplaceUnsupportedFormat, entry.destination.c_str());
                break;
            }
            const std::wstring backup = UniqueTemporaryPath(
                entry.destination, L".pulse-backup-", task_id, index);
            DWORD pin_error = ERROR_SUCCESS;
            auto leaf = destination_guard.PinLeaf(entry.destination, failure, &pin_error);
            if (!leaf) { fail_win32(pin_error, entry.destination); break; }
            if (!guard_result(destination_guard.Check(entry.destination, failure), entry.destination)) break;
            if (!destination_guard.RenameLeaf(leaf.get(), backup)) {
                const DWORD error = GetLastError();
                fail_win32(error, entry.destination);
                break;
            }
            replacement_backups.push_back({ entry.destination, backup });
            if (!guard_result(destination_guard.AdoptCreatedLeaf(backup, leaf.get(), failure), backup) ||
                !guard_result(destination_guard.RefreshOwnedLeaf(entry.destination, failure), entry.destination)) break;
            destination_exists = false;
            destination_is_directory = false;
        }

        if (!force_copy_move_ && req.type == OpType::Move && !destination_exists &&
            (!entry.directory || entry.reparse || entry.directory_rename_only)) {
            if (!guard_result(destination_guard.EnsureDirectories(ParentOf(entry.destination), failure), ParentOf(entry.destination)) ||
                !guard_result(destination_guard.Check(entry.destination, failure), entry.destination)) break;
            DestinationGuard::Handle source(CreateFileW(entry.source.c_str(), DELETE | FILE_READ_ATTRIBUTES,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
            if (source.get() == INVALID_HANDLE_VALUE) {
                source.release();
                fail_win32(GetLastError(), entry.source);
                break;
            }
            if (!entry.source_identity.Matches(source.get(), transfer_identity_query_)) {
                fail_win32(ERROR_FILE_INVALID, entry.source);
                break;
            }
            if (destination_guard.RenameLeaf(source.get(), entry.destination)) {
                if (!guard_result(destination_guard.AdoptCreatedLeaf(entry.destination, source.get(), failure), entry.destination)) break;
                source_directories->ForgetLeaf(entry.source);
                completed_sources.push_back(entry.source);
                completed_destinations.push_back(entry.destination);
                if (entry.directory && !entry.reparse) renamed_directories.push_back(entry.source);
                ++processed_items;
                publish_progress(entry, entry.bytes, entry.bytes, processed_items);
                committed_bytes += entry.bytes;
                continue;
            }
            const DWORD code = GetLastError();
            if (code != ERROR_NOT_SAME_DEVICE || entry.directory_rename_only) {
                fail_win32(code, entry.source);
                break;
            }
        }

        if (entry.reparse) {
            if (!guard_result(destination_guard.EnsureDirectories(ParentOf(entry.destination), failure), ParentOf(entry.destination)) ||
                !guard_result(destination_guard.Check(entry.destination, failure), entry.destination)) break;
            if (!CopyReparsePoint(entry, destination_guard, failure, failure_hr, failed_path)) break;
            if (req.type == OpType::Move) {
                DWORD error = ERROR_SUCCESS;
                if (!delete_source(entry, error)) {
                    fail_win32(error, entry.source);
                    break;
                }
            }
            completed_sources.push_back(entry.source);
            completed_destinations.push_back(entry.destination);
            ++processed_items;
            publish_progress(entry, entry.bytes, entry.bytes, processed_items);
            committed_bytes += entry.bytes;
            continue;
        }

        if (entry.directory) {
            if (!guard_result(destination_guard.EnsureDirectories(entry.destination, failure), entry.destination) ||
                !guard_result(destination_guard.Check(entry.destination, failure), entry.destination)) break;
            ++processed_items;
            SetStatus([&](OpStatus& st) {
                st.current_item = FileName(entry.source);
                st.completed_items = processed_items;
                if (st.total_bytes == 0 && st.total_items > 0)
                    st.percent = static_cast<float>(st.completed_items * 100.0 / st.total_items);
            });
            continue;
        }

        // Keep the copied object's data immutable until its destructive cleanup.
        // Share deletion so a name replacement is still rejected by object proof.
        DestinationGuard::Handle source_content;
        if (req.type == OpType::Move) {
            source_content.reset(CreateFileW(entry.source.c_str(), GENERIC_READ,
                FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
            if (source_content.get() == INVALID_HANDLE_VALUE) {
                const DWORD code = GetLastError();
                source_content.release();
                fail_win32(code, entry.source);
                break;
            }
            if (!entry.source_identity.Matches(source_content.get(), transfer_identity_query_)) {
                fail_win32(ERROR_FILE_INVALID, entry.source);
                break;
            }
        }

        if (!guard_result(destination_guard.EnsureDirectories(ParentOf(entry.destination), failure), ParentOf(entry.destination)) ||
            !guard_result(destination_guard.Check(entry.destination, failure), entry.destination)) break;

        const bool replacing = destination_exists && choice == ConflictChoice::Replace;
        const bool temporary_copy = (conflict && choice == ConflictChoice::Replace) ||
            entry.bytes >= 64ull * 1024ull * 1024ull;
        const std::wstring copy_destination = temporary_copy
            ? UniqueTemporaryPath(entry.destination, L".pulse-copy-", task_id, index)
            : entry.destination;

        uint64_t known_file_total = entry.bytes;
        CopyProgressContext context;
        if (entry.source_identity.Get()) context.source_identity = &entry.source_identity;
        context.identity_query = transfer_identity_query_;
        context.cancel = &transfer_cancel_;
        context.pause = &transfer_pause_;
        context.report = [&](uint64_t done, uint64_t actual_total) {
            if (actual_total > 0 && actual_total != known_file_total) {
                if (actual_total > known_file_total) total_bytes += actual_total - known_file_total;
                else total_bytes -= (std::min)(total_bytes, known_file_total - actual_total);
                known_file_total = actual_total;
                entry.bytes = actual_total;
                SetStatus([&](OpStatus& st) { st.total_bytes = total_bytes; });
            }
            publish_progress(entry, done, actual_total, processed_items);
        };

        auto cleanup_temporary = [&] {
            std::wstring ignored;
            if (context.destination_handle) {
                if (!destination_guard.AdoptCopyLeaf(copy_destination, context.destination_handle.get(), ignored)) return;
                context.destination_handle.reset();
            }
            destination_guard.DeleteOwnedLeaf(copy_destination, ignored);
        };
        HRESULT copy_result = E_FAIL;
        bool resume = false;
        DWORD extra_flags = COPY_FILE_COPY_SYMLINK;
        for (;;) {
            context.pause_sent = false;
            COPYFILE2_EXTENDED_PARAMETERS parameters{};
            parameters.dwSize = sizeof(parameters);
            parameters.dwCopyFlags = extra_flags;
            if (!resume) parameters.dwCopyFlags |= COPY_FILE_FAIL_IF_EXISTS;
            if (resume) parameters.dwCopyFlags |= COPY_FILE_RESUME_FROM_PAUSE;
            parameters.pfCancel = nullptr;
            parameters.pProgressRoutine = CopyProgress;
            parameters.pvCallbackContext = &context;
            copy_result = transfer_copy_file_(entry.source.c_str(), copy_destination.c_str(), &parameters);
            if (copy_result == HRESULT_FROM_WIN32(ERROR_INVALID_PARAMETER) && extra_flags != 0 && !resume) {
                extra_flags = 0;
                continue;
            }
            if (copy_result != HRESULT_FROM_WIN32(ERROR_REQUEST_PAUSED)) break;

            // CopyFile2 must reopen the paused stream for writing. Keep a checked
            // reference with permissive sharing instead of its original copy handle.
            if (!guard_result(destination_guard.AdoptCopyLeaf(copy_destination,
                    context.destination_handle.get(), failure), copy_destination)) break;
            context.destination_handle.reset();

            SetStatus([&](OpStatus& st) {
                st.phase = OpPhase::Paused;
                st.summary = l10n::Format(l10n::StringId::OperationStateFormat,
                OpVerb(req.type).c_str(), l10n::Get(l10n::StringId::OpPaused).c_str());
                st.bytes_per_second = 0.0;
                st.eta_seconds = 0;
            });
            std::unique_lock<std::mutex> lock(transfer_control_mutex_);
            transfer_control_cv_.wait(lock, [&] {
                return transfer_cancel_.load() || !transfer_pause_.load();
            });
            if (transfer_cancel_.load()) {
                cancelled = true;
                break;
            }
            if (!guard_result(destination_guard.Check(copy_destination, failure), copy_destination)) break;
            resume = true;
            reset_rate();
            SetStatus([&](OpStatus& st) {
                st.phase = OpPhase::Running;
                st.summary = l10n::Format(l10n::StringId::OperationProgressFormat,
                    OpVerb(req.type).c_str(), static_cast<unsigned long long>(entries.size()));
            });
        }
        if (cancelled || transfer_cancel_.load()) {
            cancelled = true;
            cleanup_temporary();
            break;
        }
        if (context.source_changed ||
            (SUCCEEDED(copy_result) && req.type == OpType::Move && !context.source_checked)) {
            cleanup_temporary();
            fail_win32(ERROR_FILE_INVALID, entry.source);
            break;
        }
        if (FAILED(copy_result)) {
            if (!failure.empty()) { cleanup_temporary(); break; }
            failure_hr = copy_result;
            switch (context.error_phase) {
            case COPYFILE2_PHASE_PREPARE_SOURCE:
            case COPYFILE2_PHASE_READ_SOURCE:
                failed_path = entry.source;
                break;
            case COPYFILE2_PHASE_PREPARE_DEST:
            case COPYFILE2_PHASE_WRITE_DESTINATION:
                failed_path = copy_destination;
                break;
            default:
                // CopyFile2 can fail before delivering a phase callback. Do not
                // assert that the source is locked when the endpoint is unknown.
                failed_candidates = {entry.source, copy_destination};
                break;
            }
            failure = Win32Message(HRESULT_CODE(copy_result)) + L" | " +
                (failed_path.empty() ? entry.source + L" → " + copy_destination : failed_path);
            cleanup_temporary();
            break;
        }

        if (verify_copies_.load()) {
            SetStatus([&](OpStatus& st) {
                st.phase = OpPhase::Verifying;
                st.summary = l10n::Format(l10n::StringId::OperationVerifyingFormat, FileName(entry.source).c_str());
                st.bytes_per_second = 0.0;
                st.eta_seconds = 0;
            });
            std::array<uint8_t, 32> source_hash{};
            std::array<uint8_t, 32> copied_hash{};
            if (!Sha256File(entry.source, transfer_cancel_, transfer_pause_, source_hash, failure) ||
                !Sha256File(copy_destination, transfer_cancel_, transfer_pause_, copied_hash, failure)) {
                cleanup_temporary();
                if (transfer_cancel_.load()) cancelled = true;
                break;
            }
            if (source_hash != copied_hash) {
                cleanup_temporary();
                failure = l10n::Format(l10n::StringId::OperationHashFailedFormat, entry.source.c_str());
                break;
            }
            SetStatus([&](OpStatus& st) { st.phase = OpPhase::Running; });
        }

        if (!guard_result(destination_guard.AdoptCopyLeaf(copy_destination, context.destination_handle.get(),
                failure), copy_destination) ||
            !guard_result(destination_guard.Check(entry.destination, failure), entry.destination)) {
            cleanup_temporary();
            break;
        }
        context.destination_handle.reset();
        if (replacing) {
            // Rename the checked object through its handle. Never write through a
            // symlink/hardlink or retry a replacement against a changed pathname.
            const std::wstring backup = UniqueTemporaryPath(entry.destination,
                L".pulse-backup-", task_id, index);
            DWORD pin_error = ERROR_SUCCESS;
            auto leaf = destination_guard.PinLeaf(entry.destination, failure, &pin_error);
            if (!leaf) {
                fail_win32(pin_error, entry.destination);
                cleanup_temporary();
                break;
            }
            if (!guard_result(destination_guard.Check(entry.destination, failure), entry.destination)) { cleanup_temporary(); break; }
            if (!destination_guard.RenameLeaf(leaf.get(), backup)) {
                const DWORD error = GetLastError();
                fail_win32(error, entry.destination);
                cleanup_temporary();
                break;
            }
            replacement_backups.push_back({entry.destination, backup});
            if (!guard_result(destination_guard.AdoptCreatedLeaf(backup, leaf.get(), failure), backup) ||
                !guard_result(destination_guard.RefreshOwnedLeaf(entry.destination, failure), entry.destination)) {
                cleanup_temporary();
                break;
            }
        }
        if (!guard_result(destination_guard.Check(entry.destination, failure), entry.destination)) {
            cleanup_temporary();
            break;
        }
        if (temporary_copy) {
            DWORD pin_error = ERROR_SUCCESS;
            auto temporary = destination_guard.PinLeaf(copy_destination, failure, &pin_error);
            if (!temporary) {
                fail_win32(pin_error, copy_destination);
                cleanup_temporary();
                break;
            }
            if (!guard_result(destination_guard.Check(entry.destination, failure), entry.destination)) { cleanup_temporary(); break; }
            if (!destination_guard.RenameLeaf(temporary.get(), entry.destination)) {
                const DWORD error = GetLastError();
                fail_win32(error, entry.destination);
                cleanup_temporary();
                break;
            }

            if (!guard_result(destination_guard.AdoptCreatedLeaf(entry.destination, temporary.get(),
                    failure), entry.destination)) break;
            temporary.reset();
        }

        if (req.type == OpType::Move) {
            DWORD error = ERROR_SUCCESS;
            if (!delete_source(entry, error)) {
                fail_win32(error, entry.source);
                break;
            }
        }
        completed_sources.push_back(entry.source);
        completed_destinations.push_back(entry.destination);
        ++processed_items;
        publish_progress(entry, entry.bytes, entry.bytes, processed_items);
        committed_bytes += entry.bytes;
    }

    if (failure.empty() && !cancelled && req.type == OpType::Copy) {
        for (auto it = entries.rbegin(); it != entries.rend(); ++it) {
            if (it->directory && !it->reparse &&
                !guard_result(destination_guard.SetDirectoryMetadata(it->destination,
                    it->created, it->accessed, it->modified, it->attributes, failure), it->destination)) break;
        }
    }
    if (failure.empty() && !cancelled) {
        for (auto it = entries.rbegin(); it != entries.rend(); ++it) {
            if (!it->directory || already_renamed(it->source) || cleanup_only(it->source)) continue;
            if (req.type == OpType::Move) {
                if (std::any_of(skipped_prefixes.begin(), skipped_prefixes.end(), [&](const auto& prefix) {
                        return StartsWithPath(it->source, prefix) || StartsWithPath(prefix, it->source);
                    })) continue;
                if (it->reparse) continue; // the link itself was already removed above
                DWORD error = ERROR_SUCCESS;
                if (!delete_source(*it, error)) {
                    fail_win32(error, it->source);
                    break;
                }
                removed_directories.push_back(it->source);
            }
        }
    }

    if (failure.empty() && !cancelled && req.type == OpType::Move) {
        for (auto it = entries.rbegin(); it != entries.rend(); ++it) {
            if (!already_renamed(it->source) && it->directory && !it->reparse && !cleanup_only(it->source) &&
                !guard_result(destination_guard.SetDirectoryMetadata(it->destination,
                    it->created, it->accessed, it->modified, it->attributes, failure), it->destination)) break;
        }
    }

    std::vector<std::wstring> restored_backups;
    std::vector<std::wstring> retained_backups;
    for (auto it = replacement_backups.rbegin(); it != replacement_backups.rend(); ++it) {
        const bool committed_move = req.type == OpType::Move &&
            std::find(completed_destinations.begin(), completed_destinations.end(), it->original) != completed_destinations.end();
        std::wstring cleanup_error;
        auto retain_backup = [&] {
            retained_backups.push_back(it->backup);
        };
        if (!destination_guard.Check(it->backup, cleanup_error)) { retain_backup(); continue; }
        if ((failure.empty() && !cancelled) || committed_move) {
            // Handle deletion removes a file/reparse name or an empty directory.
            // Never recursively delete a renamed directory's unverified children.
            if (!destination_guard.DeleteOwnedLeaf(it->backup, cleanup_error)) retain_backup();
        } else {
            if (!destination_guard.Check(it->original, cleanup_error)) { retain_backup(); continue; }
            if (PathExists(it->original) &&
                !destination_guard.DeleteOwnedLeaf(it->original, cleanup_error)) { retain_backup(); continue; }
            auto backup = destination_guard.PinLeaf(it->backup, cleanup_error);
            if (!backup || !destination_guard.Check(it->original, cleanup_error) ||
                !destination_guard.RenameLeaf(backup.get(), it->original)) { retain_backup(); continue; }
            destination_guard.RefreshOwnedLeaf(it->original, cleanup_error);
            restored_backups.push_back(it->original);
        }
    }
    for (const auto& backup : retained_backups) {
        if (!failure.empty()) failure += L"\n";
        failure += l10n::Format(l10n::StringId::OperationBackupRecoveryFailedFormat, backup.c_str());
    }
    for (size_t i = completed_destinations.size(); i-- > 0;) {
        if (std::any_of(restored_backups.begin(), restored_backups.end(), [&](const auto& restored) {
                return StartsWithPath(completed_destinations[i], restored);
            })) {
            completed_destinations.erase(completed_destinations.begin() + i);
            completed_sources.erase(completed_sources.begin() + i);
        }
    }

    if (req.undo_revision && (!failure.empty() || cancelled)) {
        // Keep only still-present source subtrees after a partial directory inverse.
        // Never infer success from absence: the completed list below is evidence.
        for (size_t i = 0; i < req.sources.size(); ++i) {
            const auto root = std::find_if(entries.begin(), entries.end(), [&](const auto& entry) {
                return entry.source == req.sources[i];
            });
            if (root == entries.end() || !root->directory || root->reparse || already_renamed(root->source) || cleanup_only(root->source)) continue;
            std::vector<std::wstring> remaining_actual, remaining_original, cleanup_directories;
            std::vector<FileIdentity> remaining_identities;
            bool had_success = false;
            for (const auto& entry : entries) {
                if (!StartsWithPath(entry.source, root->source)) continue;
                if (already_renamed(entry.source) ||
                    std::find(completed_sources.begin(), completed_sources.end(), entry.source) != completed_sources.end()) {
                    had_success = true;
                    continue;
                }
                if (entry.directory && !entry.reparse) continue;
                remaining_actual.push_back(entry.source);
                remaining_original.push_back(entry.destination);
                remaining_identities.push_back(entry.source_identity.identity);
            }
            if (!had_success) continue;
            // Empty ordinary directories can remain after per-file move success.
            // Add them deepest-first as merge inverses, without replaying moved files.
            bool source_identity_changed = false;
            for (auto entry = entries.rbegin(); entry != entries.rend(); ++entry) {
                if (!entry->directory || entry->reparse || already_renamed(entry->source) ||
                    !StartsWithPath(entry->source, root->source) ||
                    std::find(removed_directories.begin(), removed_directories.end(), entry->source) != removed_directories.end()) continue;
                DWORD cleanup_error = ERROR_SUCCESS;
                if (!delete_source(*entry, cleanup_error)) {
                    if (cleanup_error == ERROR_FILE_INVALID) {
                        fail_win32(cleanup_error, entry->source);
                        source_identity_changed = true;
                        break;
                    }
                    WIN32_FIND_DATAW data{};
                    HANDLE find = FindFirstFileW(JoinPath(entry->source, L"*").c_str(), &data);
                    bool empty = find != INVALID_HANDLE_VALUE;
                    if (find != INVALID_HANDLE_VALUE) {
                        do {
                            if (wcscmp(data.cFileName, L".") != 0 && wcscmp(data.cFileName, L"..") != 0) empty = false;
                        } while (FindNextFileW(find, &data));
                        FindClose(find);
                    }
                    if (empty) {
                        remaining_actual.push_back(entry->source);
                        remaining_original.push_back(entry->destination);
                        remaining_identities.push_back(entry->source_identity.identity);
                        cleanup_directories.push_back(entry->source);
                    }
                } else {
                    removed_directories.push_back(entry->source);
                }
            }
            if (source_identity_changed) {
                // Retain the unresolved history, but a retry must not admit the
                // replacement path as a fresh source for this inverse.
                std::lock_guard<std::mutex> lock(mutex_);
                if (req.undo_revision == undo_revision_ && !undo_.empty()) undo_.back().supported = false;
                continue;
            }
            if (remaining_actual.empty() &&
                std::find(removed_directories.begin(), removed_directories.end(), root->source) != removed_directories.end()) {
                completed_sources.push_back(root->source);
                completed_destinations.push_back(root->destination);
                continue;
            }
            std::lock_guard<std::mutex> lock(mutex_);
            if (req.undo_revision != undo_revision_ || undo_.empty()) break;
            auto& remainder = undo_.back();
            for (size_t j = 0; j < remainder.sources.size(); ++j) {
                const auto actual = j < remainder.destinations.size() ? remainder.destinations[j]
                    : JoinPath(remainder.dest_dir, FileName(remainder.sources[j]));
                if (actual != root->source) continue;
                if (remainder.destinations.empty()) {
                    for (const auto& original : remainder.sources)
                        remainder.destinations.push_back(JoinPath(remainder.dest_dir, FileName(original)));
                }
                remainder.sources.erase(remainder.sources.begin() + j);
                remainder.destinations.erase(remainder.destinations.begin() + j);
                remainder.produced_identities.erase(remainder.produced_identities.begin() + j);
                std::erase(remainder.cleanup_directories, root->source);
                remainder.sources.insert(remainder.sources.end(), remaining_original.begin(), remaining_original.end());
                remainder.destinations.insert(remainder.destinations.end(), remaining_actual.begin(), remaining_actual.end());
                remainder.produced_identities.insert(remainder.produced_identities.end(), remaining_identities.begin(), remaining_identities.end());
                remainder.cleanup_directories.insert(remainder.cleanup_directories.end(), cleanup_directories.begin(), cleanup_directories.end());
                // The leftover root is cleanup only. A retry cannot admit new descendants.
                if (std::find(remaining_actual.begin(), remaining_actual.end(), root->source) == remaining_actual.end()) {
                    remainder.sources.push_back(root->destination);
                    remainder.destinations.push_back(root->source);
                    remainder.produced_identities.push_back(root->source_identity.identity);
                    remainder.cleanup_directories.push_back(root->source);
                }
                break;
            }
        }
    }

    if (failure.empty() && !cancelled) {
        OpRequest committed = req;
        committed.sources.clear();
        std::vector<std::wstring> committed_destinations;
        for (size_t root_index = 0; root_index < req.sources.size(); ++root_index) {
            const auto found = std::find_if(entries.begin(), entries.end(), [&](const TransferEntry& entry) {
                return _wcsicmp(entry.source.c_str(), req.sources[root_index].c_str()) == 0;
            });
            if (found == entries.end()) continue;
            const std::wstring original_destination = req.type == OpType::Move && req.is_undo &&
                root_index < req.new_names.size() ? req.new_names[root_index]
                : JoinPath(req.dest_dir, FileName(req.sources[root_index]));
            const bool independent_root = root_index >= root_destination_preexisting.size()
                || !root_destination_preexisting[root_index]
                || _wcsicmp(found->destination.c_str(), original_destination.c_str()) != 0;
            if (independent_root) {
                committed.sources.push_back(req.sources[root_index]);
                committed_destinations.push_back(found->destination);
                continue;
            }
            for (size_t item = 0; item < completed_sources.size(); ++item) {
                if (StartsWithPath(completed_sources[item], req.sources[root_index])) {
                    committed.sources.push_back(completed_sources[item]);
                    committed_destinations.push_back(completed_destinations[item]);
                }
            }
        }
        if (req.undo_revision) {
            committed.sources.clear();
            committed_destinations.clear();
            for (size_t i = 0; i < req.sources.size() && i < req.new_names.size(); ++i) {
                if (PathExists(req.sources[i])) continue;
                const auto root = std::find_if(entries.begin(), entries.end(), [&](const auto& entry) {
                    return entry.source == req.sources[i];
                });
                if (root == entries.end()) continue;
                const bool confirmed_file = std::find(completed_sources.begin(), completed_sources.end(),
                                                     root->source) != completed_sources.end();
                const bool confirmed_directory = root->directory && !root->reparse &&
                    (already_renamed(root->source) ||
                    (std::find(removed_directories.begin(), removed_directories.end(), root->source) != removed_directories.end() &&
                    std::all_of(entries.begin(), entries.end(), [&](const auto& entry) {
                        return !StartsWithPath(entry.source, root->source) || entry.directory || already_renamed(entry.source) ||
                            std::find(completed_sources.begin(), completed_sources.end(), entry.source) != completed_sources.end();
                    })));
                if (!confirmed_file && !confirmed_directory) continue;
                committed.sources.push_back(req.sources[i]);
                committed_destinations.push_back(req.new_names[i]);
            }
        }
        if (!committed_destinations.empty()) {
            std::vector<FileIdentity> identities;
            if (req.type == OpType::Copy || req.type == OpType::Move) {
                identities.resize(committed_destinations.size());
                for (size_t i = 0; i < committed_destinations.size(); ++i)
                    destination_guard.CapturedIdentity(committed_destinations[i], identities[i]);
            }
            if (req.is_undo) {
                committed.undo_produced_identities = SelectUndoIdentities(req, committed.sources);
            }
            PushUndo(committed, &committed_destinations, &identities);
        }
    } else if (!completed_destinations.empty()) {
        OpRequest partial = req;
        partial.sources = completed_sources;
        std::vector<FileIdentity> identities;
        if (req.type == OpType::Copy || req.type == OpType::Move) {
            identities.resize(completed_destinations.size());
            for (size_t i = 0; i < completed_destinations.size(); ++i)
                destination_guard.CapturedIdentity(completed_destinations[i], identities[i]);
        }
        if (req.is_undo) {
            partial.undo_produced_identities = SelectUndoIdentities(req, partial.sources);
        }
        PushUndo(partial, &completed_destinations, &identities);
    }

    {
        std::lock_guard<std::mutex> lock(transfer_control_mutex_);
        pending_conflict_.reset();
    }
    LockReport locks;
    if (!failure.empty() && !cancelled && !transfer_cancel_.load()) {
        QueueItem retry;
        retry.req = req; retry.seq = task_id;
        retry.retry_completed_sources = prior_completed;
        retry.retry_completed_sources.insert(retry.retry_completed_sources.end(), completed_sources.begin(), completed_sources.end());
        retry.retry_completed_sources.insert(retry.retry_completed_sources.end(), skipped_sources.begin(), skipped_sources.end());
        retry.retry_completed_sources.insert(retry.retry_completed_sources.end(), removed_directories.begin(), removed_directories.end());
        retry.retry_transfer_plan = prior_plan;
        retry.retry_source_directories = source_directories;
        retry.retry_destination_guard = destination_snapshot;
        if (plan_complete) {
            for (const auto& entry : entries) {
                const auto prior = std::find_if(retry.retry_transfer_plan.begin(), retry.retry_transfer_plan.end(),
                    [&](const auto& planned) { return planned.source == entry.source; });
                if (prior == retry.retry_transfer_plan.end()) retry.retry_transfer_plan.push_back(entry);
                else *prior = entry; // retain nested KeepBoth remapping, not just root targets
            }
        } // A failed initial scan made no mutations; retry may finish its scan.
        retry.retry_probe_paths = failed_candidates;
        CaptureElevationFailure(retry, failure_hr, failed_path);
        if (!req.undo_revision) locks = CaptureLockedFailure(retry, failure_hr, failed_path,
            [&] { return stopping_.load() || transfer_cancel_.load(); });
    }
    SetStatus([&](OpStatus& st) {
        st.active = false;
        st.failure_hr = failure.empty() ? S_OK : FAILED(failure_hr) ? failure_hr : E_FAIL;
        st.failed_path = failed_path;
        st.locked_path = locks.path; st.lock_owners = locks.owners;
        st.lock_retry_available = !cancelled && (IsLockFailure(st.failure_hr) || !locks.owners.empty());
        st.completed_ops++;
        st.bytes_per_second = 0.0;
        st.eta_seconds = 0;
        if (cancelled || transfer_cancel_.load()) {
            st.phase = OpPhase::Cancelled;
            st.last_error = retained_backups.empty() ? L"" : failure;
            st.failure_hr = retained_backups.empty() ? S_OK : E_FAIL;
            st.failed_path = retained_backups.empty() ? L"" : retained_backups.front();
            st.summary = l10n::Format(l10n::StringId::OperationStateFormat,
                OpVerb(req.type).c_str(), l10n::Get(l10n::StringId::OperationCancelled).c_str());
        } else if (!failure.empty()) {
            st.phase = OpPhase::Failed;
            st.last_error = failure;
            st.summary = l10n::Format(l10n::StringId::OperationStateFormat,
                OpVerb(req.type).c_str(), l10n::Get(l10n::StringId::OpFailedBadge).c_str());
        } else {
            st.phase = OpPhase::Completed;
            st.percent = 100.0f;
            st.transferred_bytes = st.total_bytes;
            st.completed_items = st.total_items;
            st.summary = l10n::Format(l10n::StringId::OperationStateFormat,
                Describe(req).c_str(), l10n::Get(l10n::StringId::OpCompleted).c_str());
            st.last_error.clear();
        }
    });
    transfer_active_.store(false);
}

void OpsManager::RunDriveLabelRename(const OpRequest& req, uint64_t task_id,
    decltype(&SetVolumeLabelW) set_label) {
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
    const bool cancelled = stopping_.load() || shell_cancel_requested_.load() || lock_retry_cancel_.load();
    DWORD code = ERROR_SUCCESS;
    if (!cancelled) {
        const auto source = pulse::path::StripExtendedPathPrefix(req.sources.front());
        const wchar_t root[] = {source.front(), L':', L'\\', 0};
        if (req.new_name.find(L'\0') != std::wstring::npos) code = ERROR_INVALID_NAME;
        else if (!set_label(root, req.new_name.c_str())) {
            code = GetLastError();
            if (code == ERROR_SUCCESS) code = ERROR_GEN_FAILURE;
        }
        if (code == ERROR_SUCCESS) {
            const std::vector<std::wstring> destinations;
            PushUndo(req, &destinations);
        }
    }
    if (!cancelled && code != ERROR_SUCCESS) {
        QueueItem retry; retry.req = req; retry.seq = task_id;
        CaptureElevationFailure(std::move(retry), HRESULT_FROM_WIN32(code), req.sources.front());
    }
    SetStatus([&](OpStatus& status) {
        status.active = false;
        ++status.completed_ops;
        status.failure_hr = code == ERROR_SUCCESS ? S_OK : HRESULT_FROM_WIN32(code);
        status.failed_path = code == ERROR_SUCCESS ? L"" : req.sources.front();
        if (cancelled) {
            status.phase = OpPhase::Cancelled; status.summary = l10n::Get(l10n::StringId::OperationCancelled);
        } else if (code != ERROR_SUCCESS) {
            status.phase = OpPhase::Failed; status.last_error = Win32Message(code);
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
    QueueItem retry; retry.req = req; retry.seq = Status().task_id;
    retry.req.sources.clear(); retry.req.recycle_paths.clear(); retry.req.recycle_undo_identities.clear();
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
        if (code == ERROR_ACCESS_DENIED || code == ERROR_PRIVILEGE_NOT_HELD || code == ERROR_ELEVATION_REQUIRED) {
            retry.req.sources.push_back(result.original_path);
            retry.req.recycle_paths.push_back(result.recycle_path);
            retry.req.recycle_undo_identities.push_back(identity);
        }
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
    if (!cancelled && !retry.req.sources.empty()) CaptureElevationFailure(std::move(retry), E_ACCESSDENIED, failed_path);
    if (!restored_sources.empty()) {
        OpRequest completed = req;
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
        FinishDelete(rejected, L"Deletion was not admitted by the deletion service.");
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
        st.current_item = req.type == OpType::EmptyRecycle
            ? OpVerb(OpType::EmptyRecycle) : st.source_label;
        st.total_bytes = st.transferred_bytes = 0;
        st.total_items = req.type == OpType::EmptyRecycle ? 0
            : (req.sources.empty() ? 1 : req.sources.size());
        st.completed_items = 0;
        st.bytes_per_second = st.peak_bytes_per_second = 0.0;
        st.eta_seconds = 0;
        if (req.type == OpType::EmptyRecycle) st.percent = -1.0f;
    });

    if (req.type == OpType::RestoreRecycle && !req.is_undo) {
        RunManualRecycleRestore(req, &IsCurrentUserRecyclePayload, continuation_guard);
        return;
    }

    auto& client = ipc::ShellClient::Instance();

    if (req.type == OpType::Rename || req.type == OpType::BatchRename) {
        bool valid = !req.sources.empty() && (req.type != OpType::Rename || req.sources.size() == 1);
        for (size_t i = 0; i < req.sources.size(); ++i) {
            const auto& name = req.type == OpType::BatchRename && i < req.new_names.size() ? req.new_names[i] : req.new_name;
            valid = valid && !req.sources[i].empty() && IsRenameComponent(name);
        }
        if (!valid || shell_cancel_requested_.load() || stopping_.load() || (lock_retry && lock_retry_cancel_.load())) {
            const bool cancelled = shell_cancel_requested_.load() || stopping_.load() || (lock_retry && lock_retry_cancel_.load());
            SetStatus([&](OpStatus& st) {
                st.active = false; st.phase = cancelled ? OpPhase::Cancelled : OpPhase::Failed; st.percent = -1.0f;
                ++st.completed_ops; st.last_error = cancelled ? L"" : l10n::Get(l10n::StringId::InvalidName);
                st.summary = cancelled ? l10n::Get(l10n::StringId::OperationCancelled) : st.last_error;
                st.failure_hr = cancelled ? S_OK : HRESULT_FROM_WIN32(ERROR_INVALID_NAME);
                st.failed_path = cancelled || req.sources.empty() ? L"" : req.sources.front();
            });
            return;
        }
    }

    if (req.type == OpType::BatchRename) {
        std::vector<std::wstring> ok_sources;
        std::vector<std::wstring> ok_names;
        std::vector<std::wstring> ok_destinations;
        std::vector<FileIdentity> identities;
        QueueItem retry; retry.req = req; retry.seq = task_id;
        retry.req.sources.clear(); retry.req.new_names.clear();
        retry.req.undo_produced_identities.clear();
        std::wstring last_error;
        HRESULT failure_hr = S_OK;
        std::wstring failed_path;
        bool cancelled = false;
        for (size_t i = 0; i < req.sources.size(); ++i) {
            if (stopping_.load() || shell_cancel_requested_.load()) { cancelled = true; break; }
            const std::wstring& name = i < req.new_names.size() ? req.new_names[i] : req.new_name;
            SetStatus([&](OpStatus& st) {
                st.current_item = FileName(req.sources[i]);
                st.completed_items = i;
                if (!req.sources.empty())
                    st.percent = 100.0f * static_cast<float>(i) / static_cast<float>(req.sources.size());
            });
            if (stopping_.load() || shell_cancel_requested_.load()) { cancelled = true; break; }
            // Each item uses the same no-overwrite filesystem rename.
            std::wstring local_error;
            DWORD code = ERROR_SUCCESS;
            FileIdentity identity;
            const auto* expected = req.is_undo ? &req.undo_produced_identities[i]
                : continuation_identities && i < continuation_identities->size() ? &(*continuation_identities)[i] : nullptr;
            const auto local = RenameInProcess(req.sources[i], name, &local_error, &code, &identity, expected, nullptr, continuation_guard);
            if (local == RenameResult::Rejected) {
                last_error = local_error;
                failure_hr = HRESULT_FROM_WIN32(code == ERROR_SUCCESS ? ERROR_GEN_FAILURE : code);
                failed_path = req.sources[i];
                if (format::FileOperationMessageCode(failure_hr) == ERROR_ACCESS_DENIED ||
                    format::FileOperationMessageCode(failure_hr) == ERROR_PRIVILEGE_NOT_HELD ||
                    format::FileOperationMessageCode(failure_hr) == ERROR_ELEVATION_REQUIRED) {
                    auto reference = FileObjectReference::Open(req.sources[i]);
                    if (reference.Get() && reference.identity == identity) {
                        retry.retry_failed_path = req.sources[i];
                        retry.req.sources.push_back(req.sources[i]);
                        retry.req.new_names.push_back(name);
                        retry.retry_source_references.push_back(std::move(reference));
                        if (req.is_undo) retry.req.undo_produced_identities.push_back(req.undo_produced_identities[i]);
                    }
                }
                continue;
            }
            if (local == RenameResult::Completed) {
                ok_sources.push_back(req.sources[i]);
                ok_names.push_back(name);
                ok_destinations.push_back(JoinPath(ParentOf(req.sources[i]), name));
                identities.push_back(identity);
                continue;
            }

        }
        current_req_id_.store(0);
        shell_cancel_requested_ = false;
        if (!ok_sources.empty()) {
            OpRequest recorded = req;
            recorded.sources = ok_sources;
            recorded.new_names = ok_names;
            if (req.is_undo) recorded.undo_produced_identities = SelectUndoIdentities(req, ok_sources);
            PushUndo(recorded, &ok_destinations, &identities);
        }
        if (!cancelled && !retry.req.sources.empty()) {
            const auto denied_path = retry.retry_failed_path;
            CaptureElevationFailure(std::move(retry), E_ACCESSDENIED, denied_path);
        }
        SetStatus([&](OpStatus& st) {
            st.active = false;
            st.percent = -1.0f;
            st.completed_ops++;
            st.completed_items = ok_sources.size();
            st.failure_hr = cancelled ? S_OK : failure_hr;
            st.failed_path = cancelled ? L"" : failed_path;
            if (cancelled) {
                st.phase = OpPhase::Cancelled;
                st.last_error.clear(); st.summary = l10n::Get(l10n::StringId::OperationCancelled);
            } else if (ok_sources.empty() || !last_error.empty()) {
                st.phase = OpPhase::Failed;
                st.last_error = last_error.empty() ? l10n::Get(l10n::StringId::OperationFailedMessage) : last_error;
            } else {
                st.phase = OpPhase::Completed;
                st.summary = l10n::Format(l10n::StringId::OperationStateFormat,
                Describe(req).c_str(), l10n::Get(l10n::StringId::OpCompleted).c_str());
                if (ok_sources.size() != req.sources.size())
                    st.last_error = last_error;
            }
        });
        return;
    }

    // A single rename shares the batch path's validation and no-overwrite rules.
    if (req.type == OpType::Rename && req.sources.size() == 1) {
        std::wstring local_error;
        DWORD code = ERROR_SUCCESS;
        FileIdentity identity;
        bool identified = false;
        const auto local = RenameInProcess(req.sources.front(), req.new_name, &local_error,
            &code, &identity, req.is_undo ? &req.undo_produced_identities.front() : rename_identity ? rename_identity
                : continuation_identities && !continuation_identities->empty() ? &continuation_identities->front() : nullptr, &identified, continuation_guard);
        if (local == RenameResult::Rejected) {
            const auto cancelled = [&] { return stopping_.load() || shell_cancel_requested_.load() || lock_retry_cancel_.load(); };
            LockReport locks;
            bool sharing = code == ERROR_SHARING_VIOLATION || code == ERROR_LOCK_VIOLATION;
            if (code == ERROR_ACCESS_DENIED) {
                // Error 5 also means permissions/read-only/identity refusals.
                // A DELETE-access sharing violation is concrete independent evidence.
                HANDLE probe = CreateFileW(req.sources.front().c_str(), DELETE | FILE_READ_ATTRIBUTES,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                    FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
                const DWORD probe_error = probe == INVALID_HANDLE_VALUE ? GetLastError() : ERROR_SUCCESS;
                if (probe != INVALID_HANDLE_VALUE) CloseHandle(probe);
                sharing = probe_error == ERROR_SHARING_VIOLATION || probe_error == ERROR_LOCK_VIOLATION;
                locks = ProbeFileLocks(HRESULT_FROM_WIN32(code), req.sources.front(), req.sources,
                    LockProtectedDirectory(), cancelled);
                sharing = sharing || !locks.owners.empty();
            }
            if (!sharing && identified && !cancelled()) {
                QueueItem retry; retry.req = req; retry.seq = task_id;
                retry.retry_rename_identity = true; retry.rename_source_id = identity;
                CaptureElevationFailure(std::move(retry), HRESULT_FROM_WIN32(code), req.sources.front());
            }
            if (sharing && identified && !cancelled()) {
                QueueItem retry;
                retry.req = req; retry.seq = task_id;
                retry.retry_rename_identity = true; retry.rename_source_id = identity;
                retry.retry_failure_hr = HRESULT_FROM_WIN32(code); retry.retry_failed_path = req.sources.front();
                if (locks.owners.empty()) locks = CaptureLockedFailure(retry, HRESULT_FROM_WIN32(code), req.sources.front(), cancelled);
                std::lock_guard<std::mutex> lock(mutex_);
                if (running_ && accepting_ && !cancelled()) lock_retry_ = retry;
            }
            SetStatus([&](OpStatus& st) {
                st.active = false; st.phase = cancelled() ? OpPhase::Cancelled : OpPhase::Failed; st.percent = -1.0f;
                ++st.completed_ops; st.last_error = cancelled() ? L"" : local_error;
                st.failure_hr = cancelled() ? S_OK : HRESULT_FROM_WIN32(code);
                st.failed_path = cancelled() ? L"" : req.sources.front();
                st.lock_retry_available = sharing && identified && !cancelled();
                st.locked_path = sharing ? req.sources.front() : L""; st.lock_owners = locks.owners;
            });
            return;
        }
        if (local == RenameResult::Completed) {
            const std::vector<FileIdentity> identities{identity};
            PushUndo(req, nullptr, &identities);
            SetStatus([&](OpStatus& st) {
                st.active = false;
                st.percent = -1.0f;
                st.completed_ops++;
                st.completed_items = st.total_items;
                st.phase = OpPhase::Completed;
                st.summary = l10n::Format(l10n::StringId::OperationStateFormat,
                Describe(req).c_str(), l10n::Get(l10n::StringId::OpCompleted).c_str());
            });
            return;
        }
    }

    if (stopping_.load() || shell_cancel_requested_.load()) {
        SetStatus([&](OpStatus& st) {
            st.active = false; st.phase = OpPhase::Cancelled;
            st.last_error.clear(); st.summary = l10n::Get(l10n::StringId::OperationCancelled); ++st.completed_ops;
        });
        return;
    }
    uint32_t id = 0;
    switch (req.type) {
    case OpType::Copy:
    case OpType::Move:
    case OpType::EmptyRecycle:
    case OpType::BatchRename:
        break;
    case OpType::RecycleDelete:
    case OpType::RealDelete:
        break; // Only RunDelete can admit and execute deletion.
    case OpType::Rename: // Handled above; never send rename to the Shell host.
        break;
    case OpType::CreateFolder:
        if (!req.sources.empty()) id = client.CreateFolder(req.sources.front());
        break;
    case OpType::CreateTextFile:
        if (!req.sources.empty()) id = client.CreateNewFile(req.sources.front());
        break;
    case OpType::RestoreRecycle: {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            pending_recycle_undo_revision_ = 0;
        }
        bool valid = req.sources.size() == req.recycle_paths.size() &&
            req.sources.size() == req.recycle_undo_identities.size();
        for (size_t i = 0; valid && i < req.sources.size(); ++i) {
            RecycleUndoIdentity current;
            valid = IsCurrentUserRecyclePayload(req.sources[i], req.recycle_paths[i]) &&
                ValidateRecycleUndoPair(req.sources[i], req.recycle_paths[i], current, &req.recycle_undo_identities[i]);
        }
        if (req.is_undo && req.recycle_undo_identities.empty() && req.undo_revision) {
            RefreshRecycleUndoValidity();
            OpRequest verified = req;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if ((undo_revision_ == req.undo_revision || undo_revision_ == req.undo_revision + 1) &&
                    !undo_.empty() && undo_.back().type == OpType::RecycleDelete && undo_.back().supported &&
                    undo_.back().recycle_verified && undo_.back().sources == req.sources &&
                    undo_.back().destinations == req.recycle_paths) {
                    verified.recycle_undo_identities = undo_.back().recycle_identities;
                    undo_.pop_back(); ++undo_revision_;
                }
            }
            if (!verified.recycle_undo_identities.empty()) {
                RunShellOp(verified, task_id, nullptr, false, nullptr, continuation_guard);
                return;
            }
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
    }
    current_req_id_.store(id);
    if (shell_cancel_requested_.load() && id != 0) client.Cancel(id);

    if (id == 0) {
        SetStatus([&](OpStatus& st) {
            st.active = false;
            st.phase = OpPhase::Failed;
            st.percent = -1.0f;
            st.last_error = l10n::Get(l10n::StringId::OperationWorkerNotStarted);
            st.failure_hr = E_FAIL;
            st.completed_ops++;
        });
        return;
    }

    ShellCompletion completion;
    if (!WaitShellDone(id, completion)) return;
    const uint32_t hr = completion.hr;
    const bool cancelled = completion.cancelled;
    const auto& error = completion.error;

    const bool ok = SUCCEEDED((HRESULT)hr) && !cancelled;
    if (ok) PushUndo(req, nullptr, &completion.produced_identities);
    else if (!cancelled) {
        QueueItem retry; retry.req = req; retry.seq = task_id;
        CaptureElevationFailure(std::move(retry), static_cast<HRESULT>(hr),
            req.sources.empty() ? L"" : req.sources.front());
    }

    SetStatus([&](OpStatus& st) {
        st.active = false;
        st.percent = -1.0f;
        st.completed_ops++;
        st.failure_hr = cancelled ? S_OK : static_cast<HRESULT>(hr);
        st.failed_path = !cancelled && FAILED(static_cast<HRESULT>(hr)) && !req.sources.empty() ? req.sources.front() : L"";
        if (cancelled) {
            st.phase = OpPhase::Cancelled;
            st.last_error.clear(); st.summary = l10n::Get(l10n::StringId::OperationCancelled);
        } else if (FAILED((HRESULT)hr)) {
            st.phase = OpPhase::Failed;
            st.last_error = error.empty() ? l10n::Get(l10n::StringId::OperationFailedMessage) : error;
        } else {
            st.phase = OpPhase::Completed;
            st.completed_items = st.total_items;
            st.summary = l10n::Format(l10n::StringId::OperationStateFormat,
                Describe(req).c_str(), l10n::Get(l10n::StringId::OpCompleted).c_str());
        }
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

// ---------------------------------------------------------------------------
// Undo stack JSON persistence (minimal parser, same style as StagingTray).
// ---------------------------------------------------------------------------
std::wstring OpsManager::UndoToJson() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::wstring out = L"[\n";
    for (size_t i = 0; i < undo_.size(); ++i) {
        const auto& e = undo_[i];
        wchar_t head[64];
        swprintf_s(head, L"  {\"type\":%d,\"sup\":%s,", (int)e.type, e.supported ? L"true" : L"false");
        out += head;
        out += L"\"dest\":\"";
        pulse::json::Escape(e.dest_dir, out);
        out += L"\",\"name\":\"";
        pulse::json::Escape(e.new_name, out);
        out += L"\",\"src\":[";
        for (size_t j = 0; j < e.sources.size(); ++j) {
            out += L"\"";
            pulse::json::Escape(e.sources[j], out);
            out += L"\"";
            if (j + 1 < e.sources.size()) out += L",";
        }
        out += L"],\"dst\":[";
        for (size_t j = 0; j < e.destinations.size(); ++j) {
            out += L"\"";
            pulse::json::Escape(e.destinations[j], out);
            out += L"\"";
            if (j + 1 < e.destinations.size()) out += L",";
        }
        out += L"],\"produced\":[";
        for (size_t j = 0; j < e.produced_identities.size(); ++j) {
            out += L"\"" + ProducedIdentityText(e.produced_identities[j]) + L"\"";
            if (j + 1 < e.produced_identities.size()) out += L",";
        }
        out += L"],\"cleanup\":" + StringArrayJson(e.cleanup_directories) + L"}";
        if (i + 1 < undo_.size()) out += L",";
        out += L"\n";
    }
    out += L"]";
    return out;
}

bool OpsManager::UndoFromJson(const std::wstring& in) {
    const json::Document doc(in);
    if (!doc.Is(json::Kind::Array)) return false;
    std::deque<UndoEntry> parsed;
    for (const auto& object : doc.Root().children) {
        if (object.kind != json::Kind::Object) return false;
        UndoEntry e;
        const int type = json::Int(object.Find(L"type"), -1);
        if (type < static_cast<int>(OpType::Copy) || type > static_cast<int>(OpType::BatchRename)) return false;
        e.type = static_cast<OpType>(type);
        const auto* supported = object.Find(L"sup");
        const auto* destination = object.Find(L"dest");
        const auto* name = object.Find(L"name");
        const auto* sources = object.Find(L"src");
        if (!supported || supported->kind != json::Kind::Boolean ||
            !destination || destination->kind != json::Kind::String ||
            !name || name->kind != json::Kind::String ||
            !sources || sources->kind != json::Kind::Array) return false;
        e.supported = json::Bool(supported);
        e.dest_dir = json::String(destination);
        e.new_name = json::String(name);
        for (const auto& source : sources->children) {
            if (source.kind != json::Kind::String) return false;
            e.sources.push_back(source.text);
        }
        if (const auto* destinations = object.Find(L"dst")) {
            if (destinations->kind != json::Kind::Array) return false;
            for (const auto& value : destinations->children) {
                if (value.kind != json::Kind::String) return false;
                e.destinations.push_back(value.text);
            }
        }
        if (const auto* produced = object.Find(L"produced")) {
            if (produced->kind != json::Kind::Array) return false;
            for (const auto& value : produced->children) {
                FileIdentity identity;
                if (value.kind != json::Kind::String || !ParseProducedIdentity(value.text, identity)) return false;
                e.produced_identities.push_back(identity);
            }
        }
        if (const auto* cleanup = object.Find(L"cleanup")) {
            if (cleanup->kind != json::Kind::Array) return false;
            for (const auto& value : cleanup->children) {
                if (value.kind != json::Kind::String || e.type != OpType::Move ||
                    std::find(e.destinations.begin(), e.destinations.end(), value.text) == e.destinations.end()) return false;
                e.cleanup_directories.push_back(value.text);
            }
        }
        if (RequiresProducedIdentity(e.type)) {
            const size_t count = e.type == OpType::Copy ? e.destinations.size() : e.sources.size();
            e.supported = e.supported && HasProducedIdentities(count, e.produced_identities) &&
                (!IsMutationOperation(e.type) || e.destinations.size() == count);
        }
        parsed.push_back(std::move(e));
    }
    std::lock_guard<std::mutex> lock(mutex_);
    undo_ = std::move(parsed);
    undo_validation_requested_.store(true);
    ++undo_revision_;
    return true;
}

} // namespace pulse::ops
