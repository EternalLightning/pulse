// ops_manager.cpp — See ops_manager.h for the contract.
#include "ops_manager.h"
#include "operation_presentation.h"
#include "delete_operation.h"
#include "../fs/fs_recycle.h"
#include "../ipc/shell_client.h"
#include "../common/json_utils.h"
#include "../common/path_utils.h"
#include "../common/utf8_file.h"
#include <objbase.h>
#include <bcrypt.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cwctype>
#include <filesystem>
#include <system_error>
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

std::atomic<unsigned> open_workers{0};
constexpr unsigned kOpenWorkerLimit = 2;

bool ReserveOpenWorker() {
    unsigned current = open_workers.load();
    while (current < kOpenWorkerLimit) {
        if (open_workers.compare_exchange_weak(current, current + 1)) return true;
    }
    return false;
}

// Merged Explorer properties sheet for several items. Runs on its own STA
// thread: the sheet may call back into the data object through COM, so the
// owning apartment must keep pumping messages while the sheet holds it.
void ShowMultiFilePropertiesAsync(std::vector<std::wstring> paths) {
    if (!ReserveOpenWorker()) return;
    try {
    std::thread([paths = std::move(paths)]() {
        struct ReleaseSlot { ~ReleaseSlot() { open_workers.fetch_sub(1); } } release;
        if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE))) return;
        std::vector<PIDLIST_ABSOLUTE> pidls;
        pidls.reserve(paths.size());
        for (const auto& path : paths) {
            const std::wstring shell_path = pulse::path::StripExtendedPathPrefix(path);
            PIDLIST_ABSOLUTE pidl = nullptr;
            if (SUCCEEDED(SHParseDisplayName(shell_path.c_str(), nullptr, &pidl, 0, nullptr)) && pidl)
                pidls.push_back(pidl);
        }
        IDataObject* data = nullptr;
        if (pidls.size() > 1) {
            IShellItemArray* array = nullptr;
            if (SUCCEEDED(SHCreateShellItemArrayFromIDLists(static_cast<UINT>(pidls.size()),
                    const_cast<PCIDLIST_ABSOLUTE_ARRAY>(pidls.data()), &array)) && array) {
                array->BindToHandler(nullptr, BHID_DataObject, IID_PPV_ARGS(&data));
                array->Release();
            }
        }
        for (auto* pidl : pidls) CoTaskMemFree(pidl);
        bool shown = data && SUCCEEDED(SHMultiFileProperties(data, 0));
        if (!shown) {
            wchar_t message[160]{};
            swprintf_s(message, L"Pulse: SHMultiFileProperties failed for %zu items\n", paths.size());
            OutputDebugStringW(message);
            const std::wstring first = pulse::path::StripExtendedPathPrefix(paths.front());
            SHObjectProperties(nullptr, SHOP_FILEPATH, first.c_str(), nullptr);
        }
        // The sheet may live on this thread (a thread's windows die with it)
        // or on a shell thread that calls back into `data` through COM. Pump
        // while either holds on; a short grace covers asynchronous creation.
        const ULONGLONG started = GetTickCount64();
        while (shown) {
            MsgWaitForMultipleObjectsEx(0, nullptr, 200, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            MSG msg;
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                if (!IsDialogMessageW(GetActiveWindow(), &msg)) {
                    TranslateMessage(&msg);
                    DispatchMessageW(&msg);
                }
            }
            bool owns_window = false;
            EnumThreadWindows(GetCurrentThreadId(), [](HWND hwnd, LPARAM found) -> BOOL {
                if (!IsWindowVisible(hwnd)) return TRUE;
                *reinterpret_cast<bool*>(found) = true;
                return FALSE;
            }, reinterpret_cast<LPARAM>(&owns_window));
            ULONG refs = 1;
            if (data) {
                data->AddRef();
                refs = data->Release();
            }
            if (!owns_window && refs <= 1 && GetTickCount64() - started > 3000) break;
        }
        if (data) data->Release();
        CoUninitialize();
    }).detach();
    } catch (...) {
        open_workers.fetch_sub(1);
    }
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
    if (path.starts_with(L"\\\\?\\UNC\\")) return L"\\\\" + path.substr(8);
    if (path.starts_with(L"\\\\?\\")) return path.substr(4);
    return path;
}

const wchar_t* OpVerb(OpType t) {
    switch (t) {
    case OpType::Copy: return L"复制";
    case OpType::Move: return L"移动";
    case OpType::RecycleDelete: return L"删除";
    case OpType::RealDelete: return L"永久删除";
    case OpType::Rename: return L"重命名";
    case OpType::CreateFolder: return L"新建文件夹";
    case OpType::CreateTextFile: return L"新建文本文档";
    case OpType::RestoreRecycle: return L"还原";
    case OpType::EmptyRecycle: return L"清空回收站";
    case OpType::BatchRename: return L"批量重命名";
    }
    return L"操作";
}

std::wstring Describe(const OpRequest& r) {
    std::wstring s = OpVerb(r.type);
    if (r.type == OpType::EmptyRecycle) return s;
    s += L" ";
    if (!r.sources.empty()) s += FileName(r.sources.front());
    if (r.sources.size() > 1) {
        wchar_t buf[32];
        swprintf_s(buf, L" 等 %zu 项", r.sources.size());
        s += buf;
    }
    if (r.type == OpType::Copy || r.type == OpType::Move) {
        s += L" → " + DisplayPath(r.dest_dir);
    } else if (r.type == OpType::Rename) {
        s += L" → " + r.new_name;
    } else if (r.type == OpType::BatchRename && r.sources.size() > 1) {
        wchar_t buf[32];
        swprintf_s(buf, L" %zu 项", r.sources.size());
        s += buf;
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
                     std::wstring* error, DWORD* failure_code = nullptr, FILE_ID_INFO* source_identity = nullptr,
    const FILE_ID_INFO* expected_identity = nullptr, bool* have_identity = nullptr) {
    if (failure_code) *failure_code = ERROR_INVALID_NAME;
    if (have_identity) *have_identity = false;
    if (source.empty() || !IsRenameComponent(new_name)) {
        if (error) *error = L"名称无效";
        return RenameResult::Rejected;
    }
    const std::wstring target = JoinPath(ParentOf(source), new_name);
    if (target.empty()) {
        if (error) *error = L"名称无效";
        return RenameResult::Rejected;
    }
    // The current shell host suppresses confirmation UI, so sending an existing
    // target there could silently overwrite it. Reject that conflict here.
    if (CompareStringOrdinal(source.c_str(), -1, target.c_str(), -1, TRUE) != CSTR_EQUAL &&
        GetFileAttributesW(target.c_str()) != INVALID_FILE_ATTRIBUTES) {
        if (error) *error = L"目标名称已存在";
        if (failure_code) *failure_code = ERROR_ALREADY_EXISTS;
        return RenameResult::Rejected;
    }
    DestinationGuard::Handle identity_handle(CreateFileW(source.c_str(), 0,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (identity_handle.get() == INVALID_HANDLE_VALUE) identity_handle.release();
    FILE_ID_INFO identity{};
    const bool identified = identity_handle && GetFileInformationByHandleEx(identity_handle.get(), FileIdInfo,
        &identity, sizeof(identity));
    if (have_identity) *have_identity = identified;
    if (identified && source_identity) *source_identity = identity;
    if (expected_identity && (!identified || identity.VolumeSerialNumber != expected_identity->VolumeSerialNumber ||
        std::memcmp(&identity.FileId, &expected_identity->FileId, sizeof(identity.FileId)) != 0)) {
        if (failure_code) *failure_code = ERROR_INVALID_DATA;
        if (error) *error = L"重试来源身份已变化，操作已停止";
        return RenameResult::Rejected;
    }
    if (expected_identity) {
        DestinationGuard::Handle rename_handle(CreateFileW(source.c_str(), DELETE | FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        if (rename_handle.get() == INVALID_HANDLE_VALUE) {
            const DWORD code = GetLastError();
            rename_handle.release();
            if (failure_code) *failure_code = code;
            if (error) *error = L"重命名失败（错误 " + std::to_wstring(code) + L"）";
            return RenameResult::Rejected;
        }
        FILE_ID_INFO current{};
        if (!GetFileInformationByHandleEx(rename_handle.get(), FileIdInfo, &current, sizeof(current)) ||
            current.VolumeSerialNumber != expected_identity->VolumeSerialNumber ||
            std::memcmp(&current.FileId, &expected_identity->FileId, sizeof(current.FileId)) != 0) {
            if (failure_code) *failure_code = ERROR_INVALID_DATA;
            if (error) *error = L"重试来源身份已变化，操作已停止";
            return RenameResult::Rejected;
        }
        if (!DestinationGuard::RenameHandle(rename_handle.get(), target)) {
            const DWORD code = GetLastError();
            if (failure_code) *failure_code = code;
            if (error) *error = L"重命名失败（错误 " + std::to_wstring(code) + L"）";
            return RenameResult::Rejected;
        }
        return RenameResult::Completed;
    }
    if (!MoveFileW(source.c_str(), target.c_str())) {
        const DWORD code = GetLastError();
        if (failure_code) *failure_code = code;
        if (error) *error = L"重命名失败（错误 " + std::to_wstring(code) + L"）";
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
    FILE_ID_INFO destination_id{};
    bool destination_owned = false;
    COPYFILE2_COPY_PHASE error_phase = COPYFILE2_PHASE_NONE;
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
    switch (message->Type) {
    case COPYFILE2_CALLBACK_CHUNK_STARTED: destination = message->Info.ChunkStarted.hDestinationFile; break;
    case COPYFILE2_CALLBACK_CHUNK_FINISHED: destination = message->Info.ChunkFinished.hDestinationFile; break;
    case COPYFILE2_CALLBACK_STREAM_STARTED: destination = message->Info.StreamStarted.hDestinationFile; break;
    case COPYFILE2_CALLBACK_STREAM_FINISHED: destination = message->Info.StreamFinished.hDestinationFile; break;
    default: break;
    }
    if (destination && !ctx->destination_owned) {
        ctx->destination_owned = GetFileInformationByHandleEx(destination, FileIdInfo,
            &ctx->destination_id, sizeof(ctx->destination_id)) != FALSE;
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

bool ReadEntryMetadata(const std::wstring& path, TransferEntry& entry) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) return false;
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
    std::wstring text = raw ? raw : L"文件操作失败";
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
    HANDLE source = CreateFileW(entry.source.c_str(), 0,
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
    CloseHandle(source);
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
        error = L"无法初始化 SHA-256 校验";
        return false;
    }
    std::vector<uint8_t> object(object_bytes);
    if (BCryptCreateHash(algorithm, &hash, object.data(), object_bytes,
                         nullptr, 0, 0) < 0) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        error = L"无法初始化 SHA-256 校验";
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

std::vector<std::wstring> ExtractObjectArray(const std::wstring& input,
                                             const std::wstring& key) {
    std::vector<std::wstring> result;
    size_t pos = json::ValuePosition(input, key);
    if (pos == std::wstring::npos || pos >= input.size() || input[pos] != L'[') return result;
    ++pos;
    while (pos < input.size()) {
        json::SkipWhitespace(input, pos);
        if (pos >= input.size() || input[pos] == L']') break;
        if (input[pos] == L',') { ++pos; continue; }
        if (input[pos] != L'{') return {};
        const size_t start = pos++;
        int depth = 1;
        bool quoted = false;
        bool escaped = false;
        while (pos < input.size() && depth > 0) {
            const wchar_t c = input[pos++];
            if (quoted) {
                if (escaped) escaped = false;
                else if (c == L'\\') escaped = true;
                else if (c == L'\"') quoted = false;
                continue;
            }
            if (c == L'\"') quoted = true;
            else if (c == L'{') ++depth;
            else if (c == L'}') --depth;
        }
        if (depth != 0) return {};
        result.push_back(input.substr(start, pos - start));
    }
    return result;
}

bool ParseRecoveryEntry(const std::wstring& object, RecoveryEntry& entry) {
    const int type = json::ExtractInt(object, L"type", -1);
    const int policy = json::ExtractInt(object, L"policy", -1);
    if (type < static_cast<int>(OpType::Copy) || type > static_cast<int>(OpType::BatchRename) ||
        policy < static_cast<int>(CollisionPolicy::System) ||
        policy > static_cast<int>(CollisionPolicy::KeepBoth)) return false;
    entry.sequence = _wcstoui64(json::ExtractString(object, L"seq", L"0").c_str(), nullptr, 10);
    entry.was_active = json::ExtractBool(object, L"active", false);
    entry.request.type = static_cast<OpType>(type);
    entry.request.collision_policy = static_cast<CollisionPolicy>(policy);
    entry.request.sources = json::ExtractStringArray(object, L"sources");
    entry.request.recycle_paths = json::ExtractStringArray(object, L"recycle_paths");
    entry.request.dest_dir = json::ExtractString(object, L"dest");
    entry.request.new_name = json::ExtractString(object, L"name");
    entry.request.new_names = json::ExtractStringArray(object, L"names");
    entry.request.is_undo = json::ExtractBool(object, L"undo", false);
    for (const auto& target_json : ExtractObjectArray(object, L"delete_targets")) {
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
    if (!ReadUtf8File(journal_path_, text) || json::ExtractInt(text, L"schema", 0) != 1) return;
    for (const auto& object : ExtractObjectArray(text, L"entries")) {
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
            if (recovery.was_active && IsDeleteOperation(recovery.request.type)) {
                all_retryable = false;
                status_.last_error = L"A deletion may have partially executed. Inspect the affected paths before discarding this recovery record; it will not be replayed.";
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
    auto append = [&](const QueueItem& item, bool active) {
        if (item.req.sources.empty() && item.req.type != OpType::EmptyRecycle) return;
        if (!first) out += L",";
        first = false;
        out += L"\n    {\"seq\":" + JsonString(std::to_wstring(item.seq)) +
            L",\"active\":" + (active ? std::wstring(L"true") : std::wstring(L"false")) +
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
        append(item, recovery.was_active);
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
    thread_ = std::thread([this] { WorkerThread(); });
    menu_thread_ = std::thread([this] { MenuThread(); });
    if (ReserveOpenWorker()) {
        auto mailbox = std::make_shared<OpenMailbox>();
        mailbox->owner.store(ui_hwnd_.load());
        {
            std::lock_guard<std::mutex> lock(open_mutex_);
            open_mailbox_ = mailbox;
        }
        try {
            open_thread_ = std::thread([mailbox = std::move(mailbox)] { OpenThread(mailbox); });
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
    PersistJournal();
}

uint64_t OpsManager::Submit(OpRequest req) {
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
    {
        std::lock_guard<std::mutex> lock(mutex_);
        lock_retry_generation_.fetch_add(1);
        lock_retry_cancel_.store(true);
        if (transfer_active_.load()) {
            transfer_cancel_.store(true);
            transfer_pause_.store(false);
        }
        lock_retry_.reset();
        // A retry already dequeued retains its generation; a queued one is
        // revoked here. Neither can be revived by the worker's next reset.
        std::erase_if(queue_, [](const QueueItem& item) { return item.lock_retry; });
    }
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
    if (report.owners.empty() || (cancelled && cancelled())) return {};
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
        status.summary = was_cancelled ? L"已取消" : failure;
        status.failure_hr = was_cancelled ? S_OK : end_failure_hr;
        status.failed_path = was_cancelled ? L"" : item.retry_failed_path;
        status.locked_path = was_cancelled ? L"" : locks.path;
        status.lock_owners = was_cancelled ? std::vector<LockOwner>{} : locks.owners;
        ++status.completed_ops;
    });
    return false;
}

void OpsManager::ExecuteFileOperation(QueueItem& item) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const bool stale = item.lock_retry && item.retry_generation != lock_retry_generation_.load();
        const bool cancelled = stopping_.load() || !running_ || !accepting_ || stale;
        lock_retry_cancel_.store(cancelled);
        shell_cancel_requested_.store(cancelled);
        lock_retry_.reset();
        status_.lock_retry_available = false;
        status_.locked_path.clear(); status_.lock_owners.clear();
        status_.failure_hr = S_OK; status_.failed_path.clear();
    }
    if (!PrepareLockRetry(item)) return;
    if (IsDeleteOperation(item.req.type)) RunDelete(item);
    else if (item.req.type == OpType::Copy || item.req.type == OpType::Move)
        RunTransfer(item.req, item.seq, item.retry_completed_sources, item.retry_transfer_plan);
    else RunShellOp(item.req, item.seq, item.retry_rename_identity ? &item.rename_source_id : nullptr, item.lock_retry);
}

void OpsManager::SetStatus(const std::function<void(OpStatus&)>& fn) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        fn(status_);
    }
    if (notify_) notify_();
}

bool OpsManager::CanUndo() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (undo_.empty() || !undo_.back().supported || !move_undo_reservations_.empty()) return false;
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
    return std::wstring(L"撤销") + Describe(r);
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
                          const std::vector<std::wstring>* actual_destinations) {
    CompletedOperation completed;
    completed.type = req.type;
    completed.sources = req.sources;
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
    {
        std::lock_guard<std::mutex> lock(mutex_);
        completions_.push_back(std::move(completed));
    }
    if (req.is_undo) {
        if (req.type == OpType::Move && req.undo_revision && actual_destinations) {
            std::lock_guard<std::mutex> lock(mutex_);
            if (req.undo_revision != undo_revision_ || undo_.empty() || undo_.back().type != OpType::Move) return;
            auto& remaining = undo_.back();
            for (size_t i = remaining.sources.size(); i-- > 0;) {
                const std::wstring actual = i < remaining.destinations.size()
                    ? remaining.destinations[i] : JoinPath(remaining.dest_dir, FileName(remaining.sources[i]));
                for (size_t j = 0; j < req.sources.size() && j < actual_destinations->size(); ++j) {
                    if (actual == req.sources[j] && remaining.sources[i] == (*actual_destinations)[j]) {
                        remaining.sources.erase(remaining.sources.begin() + i);
                        if (i < remaining.destinations.size()) remaining.destinations.erase(remaining.destinations.begin() + i);
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
    if (actual_destinations) e.destinations = *actual_destinations;
    e.dest_dir = req.dest_dir;
    e.new_name = req.new_name;
    if (req.type == OpType::RealDelete) return;              // never undoable, not recorded
    if (req.type == OpType::RestoreRecycle) return;
    if (req.type == OpType::EmptyRecycle) return;
    std::lock_guard<std::mutex> lock(mutex_);
    undo_.push_back(std::move(e));
    ++undo_revision_;
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
        if (undo_.empty()) return;
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
            status_.last_error = L"此撤销的执行结果或回收项身份无法确认，请先检查文件；不会自动重试删除。";
        } else if (e.type == OpType::Move) {
            if (!move_undo_reservations_.empty()) return;
            deletion_reservation = undo_revision_;
            move_undo_reservations_.insert(deletion_reservation);
        } else if (e.type != OpType::Copy && e.type != OpType::CreateFolder &&
                   e.type != OpType::CreateTextFile) {
            undo_.pop_back();
            ++undo_revision_;
        } else deletion_reservation = undo_revision_;
    }
    // Reserve copy/create inverses until actual completion. Rejection must
    // leave the entry available, and stale reservations must not delete again.
    if (!e.supported) {
        if (notify_) notify_();
        return;
    }

    switch (e.type) {
    case OpType::Move: {
        OpRequest inv;
        inv.type = OpType::Move;
        inv.is_undo = true;
        inv.undo_revision = deletion_reservation;
        for (size_t i = 0; i < e.sources.size(); ++i) {
            inv.sources.push_back(i < e.destinations.size()
                ? e.destinations[i] : JoinPath(e.dest_dir, FileName(e.sources[i])));
        }
        // Move inverses carry full original paths, not just original parents.
        inv.new_names = e.sources;
        if (!e.sources.empty()) inv.dest_dir = ParentOf(e.sources.front());
        if (!Submit(std::move(inv))) {
            std::lock_guard<std::mutex> lock(mutex_);
            move_undo_reservations_.erase(deletion_reservation);
        }
        break;
    }
    case OpType::Rename: {
        OpRequest inv;
        inv.type = OpType::Rename;
        inv.is_undo = true;
        std::wstring new_path = JoinPath(ParentOf(e.sources.front()), e.new_name);
        inv.sources.push_back(new_path);
        inv.new_name = FileName(e.sources.front());
        Submit(std::move(inv));
        break;
    }
    case OpType::Copy: {
        // Explorer semantics: undo copy = delete the produced copies (to bin).
        OpRequest inv;
        inv.type = OpType::RecycleDelete;
        inv.is_undo = true;
        inv.delete_origin = DeleteOrigin::Undo;
        inv.undo_revision = deletion_reservation;
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
    case OpType::BatchRename: {
        for (int i = static_cast<int>(e.sources.size()) - 1; i >= 0; --i) {
            const size_t index = static_cast<size_t>(i);
            OpRequest inv;
            inv.type = OpType::Rename;
            inv.is_undo = true;
            inv.sources.push_back(index < e.destinations.size()
                ? e.destinations[index]
                : JoinPath(ParentOf(e.sources[index]), e.new_name));
            inv.new_name = FileName(e.sources[index]);
            Submit(std::move(inv));
        }
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
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    for (;;) {
        QueueItem item;
        {
            std::unique_lock<std::mutex> lock(mailbox->mutex);
            mailbox->cv.wait(lock, [&] { return !mailbox->queue.empty() || !mailbox->running; });
            if (!mailbox->running) break;
            if (mailbox->queue.empty()) continue;
            item = std::move(mailbox->queue.front());
            mailbox->queue.pop_front();
        }

        if (mailbox->before_execute) mailbox->before_execute();
        // Dialogs and association errors belong to the Pulse window, the way
        // Explorer parents them to the folder window. The fallback covers a UI
        // thread that has not registered its HWND yet.
        HWND dialog_owner = mailbox->owner.load();
        if (dialog_owner && !IsWindow(dialog_owner)) dialog_owner = nullptr;
        {
            std::lock_guard<std::mutex> lock(mailbox->mutex);
            if (!mailbox->running) break;
        }

        if (_wcsicmp(item.open_verb.c_str(), L"__cmdline") == 0) {
            std::wstring cmd = item.open_file;
            if (!cmd.empty()) {
                std::vector<wchar_t> buf(cmd.begin(), cmd.end());
                buf.push_back(0);
                STARTUPINFOW si{ sizeof(si) };
                PROCESS_INFORMATION pi{};
                si.dwFlags = STARTF_USESHOWWINDOW | STARTF_FORCEOFFFEEDBACK;
                si.wShowWindow = SW_SHOWNORMAL;
                const wchar_t* dir = item.open_path.empty() ? nullptr : item.open_path.c_str();
                if (CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE, 0, nullptr, dir,
                                   &si, &pi)) {
                    CloseHandle(pi.hThread);
                    CloseHandle(pi.hProcess);
                }
            }
            continue;
        }

        if (_wcsicmp(item.open_verb.c_str(), L"openas") == 0 ||
            _wcsicmp(item.open_verb.c_str(), L"openwith") == 0) {
            const std::wstring shell_path =
                pulse::path::StripExtendedPathPrefix(item.open_path);
            OPENASINFO info{};
            info.pcszFile = shell_path.c_str();
            info.oaifInFlags = OAIF_ALLOW_REGISTRATION | OAIF_REGISTER_EXT | OAIF_EXEC;
            const ULONGLONG dialog_started = GetTickCount64();
            const HRESULT hr = SHOpenWithDialog(dialog_owner, &info);
            if (item.enqueued_at != 0) {
                // Queue wait and dialog cost need different fixes, so report both
                // (DebugView) instead of guessing which one the user feels.
                wchar_t timing[192]{};
                swprintf_s(timing, L"Pulse: open-with queued %llu ms, dialog %llu ms\n",
                           dialog_started - item.enqueued_at,
                           GetTickCount64() - dialog_started);
                OutputDebugStringW(timing);
            }
            bool active = false;
            {
                std::lock_guard<std::mutex> lock(mailbox->mutex);
                active = mailbox->running;
            }
            if (active && FAILED(hr) && hr != HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
                SHELLEXECUTEINFOW fallback{sizeof(fallback)};
                fallback.hwnd = dialog_owner;
                fallback.lpVerb = L"openas";
                fallback.lpFile = shell_path.c_str();
                fallback.nShow = SW_SHOWNORMAL;
                fallback.fMask = SEE_MASK_INVOKEIDLIST;
                const BOOL opened = ShellExecuteExW(&fallback);
                if (!opened) {
                    // Used to fail silently, which read as a dead menu row.
                    wchar_t message[320]{};
                    swprintf_s(message,
                        L"Pulse: open-with failed for %ls (SHOpenWithDialog 0x%08lX, "
                        L"shell error %lu)\n",
                        shell_path.c_str(), static_cast<unsigned long>(hr), GetLastError());
                    OutputDebugStringW(message);
                }
            }
            continue;
        }

        if (_wcsicmp(item.open_verb.c_str(), L"properties") == 0 && item.open_paths.size() > 1) {
            ShowMultiFilePropertiesAsync(std::move(item.open_paths));
            continue;
        }

        if (_wcsicmp(item.open_verb.c_str(), L"properties") == 0) {
            const std::wstring shell_path = pulse::path::StripExtendedPathPrefix(item.open_path);
            bool shown = SHObjectProperties(dialog_owner, SHOP_FILEPATH,
                                            shell_path.c_str(), nullptr) != FALSE;
            bool active = false;
            {
                std::lock_guard<std::mutex> lock(mailbox->mutex);
                active = mailbox->running;
            }
            if (active && !shown) {
                SHELLEXECUTEINFOW fallback{sizeof(fallback)};
                fallback.lpVerb = L"properties";
                fallback.lpFile = shell_path.c_str();
                fallback.nShow = SW_SHOWNORMAL;
                fallback.fMask = SEE_MASK_INVOKEIDLIST | SEE_MASK_NOASYNC;
                shown = ShellExecuteExW(&fallback) != FALSE;
            }
            if (!shown) {
                const DWORD error = GetLastError();
                wchar_t message[256]{};
                swprintf_s(message, L"Pulse: SHObjectProperties failed for %ls (error %lu)\n",
                           shell_path.c_str(), error);
                OutputDebugStringW(message);
            }
            continue;
        }

        // Dedicated open thread: never wait behind transfers. Omit
        // SEE_MASK_NOASYNC so association handoff does not block this worker.
        SHELLEXECUTEINFOW sei{ sizeof(sei) };
        sei.hwnd = dialog_owner;
        sei.lpVerb = item.open_verb.empty() ? L"open" : item.open_verb.c_str();
        sei.lpFile = item.open_file.empty() ? item.open_path.c_str()
                                            : item.open_file.c_str();
        sei.lpParameters = item.open_args.empty() ? nullptr : item.open_args.c_str();
        sei.lpDirectory = item.open_file.empty() ? nullptr : item.open_path.c_str();
        sei.nShow = SW_SHOWNORMAL;
        sei.fMask = SEE_MASK_FLAG_NO_UI;
        ShellExecuteExW(&sei); // best effort; errors surface via the OS association UI
    }
    CoUninitialize();
    {
        std::lock_guard<std::mutex> lock(mailbox->mutex);
        mailbox->finished = true;
    }
    mailbox->finished_cv.notify_all();
}

void OpsManager::WorkerThread() {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    ipc::ShellClient::Callbacks cb;
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
        if (ConsumeCtxInvokeDone(id)) {
            ctx_invoke_done_.fetch_add(1, std::memory_order_acq_rel);
            if (notify_) notify_();
            return;
        }
        {
            std::lock_guard<std::mutex> lock(done_mutex_);
            done_id_ = id;
            done_hr_ = hr;
            done_cancelled_ = cancelled;
            done_error_ = std::move(error);
            done_deleted_paths_.clear();
            done_recycled_paths_.clear();
            done_recycle_destinations_.clear();
            done_ready_ = true;
        }
        done_cv_.notify_one();
    };
    cb.delete_done = [this](uint32_t id, uint32_t hr, bool cancelled, std::wstring error,
                            std::vector<std::wstring> deleted_paths, std::vector<std::wstring> recycled_paths, std::vector<std::wstring> recycle_destinations) {
        shell_activity_tick_ = GetTickCount64();
        {
            std::lock_guard<std::mutex> lock(done_mutex_);
            done_id_ = id; done_hr_ = hr; done_cancelled_ = cancelled;
            done_error_ = std::move(error);
            done_deleted_paths_ = std::move(deleted_paths);
            done_recycled_paths_ = std::move(recycled_paths);
            done_recycle_destinations_ = std::move(recycle_destinations);
            done_ready_ = true;
        }
        done_cv_.notify_one();
    };
    cb.ctx_items = [this](uint32_t id, std::vector<ipc::CtxMenuItem> items, bool partial,
                          std::vector<std::wstring> slow_clsids) {
        std::vector<ShellMenuItem> out;
        out.reserve(items.size());
        for (auto& it : items) {
            ShellMenuItem m;
            m.id = it.id;
            m.enabled = it.enabled;
            m.separator_after = it.separator_after;
            m.has_children = it.has_children;
            m.child = it.child;
            m.verb = std::move(it.verb);
            m.text = std::move(it.text);
            m.clsid = std::move(it.clsid);
            m.handler = std::move(it.handler);
            out.push_back(std::move(m));
        }
        OnCtxItems(id, std::move(out), partial, std::move(slow_clsids));
    };
    ipc::ShellClient::Instance().Start(cb);

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

bool OpsManager::ConsumeCtxInvokeDone(uint32_t id) {
    std::lock_guard<std::mutex> lock(menu_mutex_);
    return ctx_invoke_ids_.erase(id) != 0;
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
                job.disabled_clsids);
            std::lock_guard<std::mutex> lock(menu_mutex_);
            if (id != 0) {
                menu_session_by_token_[job.token] = id;
                menu_token_by_session_[id] = job.token;
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
                                                         job.verb, job.text);
            if (id != 0) {
                std::lock_guard<std::mutex> lock(menu_mutex_);
                ctx_invoke_ids_.insert(id);
            }
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
    const std::vector<std::wstring>& prior_completed, const std::vector<TransferPlanEntry>& prior_plan) {
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
        st.summary = std::wstring(L"正在准备") + OpVerb(req.type) + L"…";
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
    if (req.undo_revision) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (req.undo_revision != undo_revision_ || undo_.empty())
            failure = L"撤销记录已变化，操作已停止";
    }
    std::vector<TransferEntry> entries;
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
        failure = L"复制或移动请求缺少来源/目标";
    }

    bool plan_complete = !prior_plan.empty();
    namespace fsys = std::filesystem;
    if (failure.empty() && !cancelled && !prior_plan.empty()) {
        for (const auto& planned : prior_plan) {
            if (std::find(prior_completed.begin(), prior_completed.end(), planned.source) != prior_completed.end()) continue;
            TransferEntry current = planned;
            if (!ReadEntryMetadata(planned.source, current)) {
                const DWORD code = GetLastError();
                fail_win32(code, planned.source);
                break;
            }
            if (current.directory != planned.directory || current.reparse != planned.reparse) {
                failure_hr = HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
                failed_path = planned.source;
                failure = L"重试来源类型已变化，操作已停止：" + planned.source;
                break;
            }
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
            if (!ReadEntryMetadata(source, root)) {
                const auto error = GetLastError();
                fail_win32(error, source);
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
                failure = L"不能将目录复制或移动到其自身内部：" + source;
                break;
            }
            root.destination_preexisting = PathExists(root.destination);
            root_destination_preexisting[source_index] = root.destination_preexisting;
            entries.push_back(root);
            if (!root.directory || root.reparse) continue;

            std::error_code ec;
            fsys::recursive_directory_iterator it(fsys::path(source),
                fsys::directory_options::none, ec);
            fsys::recursive_directory_iterator end;
            if (ec) {
                fail_win32(static_cast<DWORD>(ec.value()), source);
                break;
            }
            for (; it != end; it.increment(ec)) {
                if (transfer_cancel_.load() || stopping_.load()) { cancelled = true; break; }
                if (ec) {
                    fail_win32(static_cast<DWORD>(ec.value()), source);
                    break;
                }
                TransferEntry child;
                child.source = it->path().wstring();
                if (std::any_of(prior_completed.begin(), prior_completed.end(), [&](const auto& done) {
                        return _wcsicmp(child.source.c_str(), done.c_str()) == 0;
                    })) {
                    it.disable_recursion_pending();
                    continue;
                }
                child.destination = (fsys::path(root.destination) /
                    it->path().lexically_relative(fsys::path(source))).wstring();
                if (!ReadEntryMetadata(child.source, child)) {
                    const auto error = GetLastError();
                    fail_win32(error, child.source);
                    break;
                }
                if (req.undo_revision && std::any_of(entries.begin(), entries.end(), [&](const auto& prior) {
                        return prior.source == child.source;
                    })) {
                    if (child.directory && child.reparse) it.disable_recursion_pending();
                    continue;
                }
                entries.push_back(std::move(child));
                if (entries.back().reparse && entries.back().directory) it.disable_recursion_pending();
            }
            if (!failure.empty()) break;
        }
        plan_complete = failure.empty() && !cancelled;
    }

    // Accidental same-folder drops leave zero work. Do not flash the status bar
    // with "移动 foo → dest 完成" for a no-op Explorer already ignores.
    if (failure.empty() && !cancelled && entries.empty()) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (req.undo_revision) { move_undo_reservations_.erase(req.undo_revision); ++undo_revision_; }
        }
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

    DestinationGuard destination_guard;
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
                st.summary = L"正在等待确认目标链接的影响";
            });
            std::unique_lock<std::mutex> lock(transfer_control_mutex_);
            const bool answered = transfer_control_cv_.wait_for(lock, std::chrono::minutes(2), [&] {
                return transfer_cancel_.load() || stopping_.load() || resolved_conflict_token_ == info.token;
            });
            if (!answered || transfer_cancel_.load() || stopping_.load() ||
                resolved_conflict_choice_ != ConflictChoice::Continue) cancelled = true;
            else continue_all_links = resolved_conflict_apply_all_;
            pending_conflict_.reset();
        }
    }
    // Pin only after the modal decision, then compare every scanned descendant.
    // A changed link is a failure even when “continue all” was checked.
    for (const auto& entry : entries) {
        if (!failure.empty() || cancelled) break;
        if (!guard_result(destination_guard.PinDirectories(ParentOf(entry.destination), failure), ParentOf(entry.destination)) ||
            !guard_result(destination_guard.Check(entry.destination, failure), entry.destination)) break;
    }

    // Restore the atomic same-volume fast path only after destination admission.
    if (failure.empty() && !cancelled && prior_plan.empty() && req.type == OpType::Move && req.sources.size() == 1 &&
        !entries.empty() && !PathExists(entries.front().destination)) {
        const auto& root = entries.front();
        if (guard_result(destination_guard.EnsureDirectories(ParentOf(root.destination), failure), ParentOf(root.destination)) &&
            guard_result(destination_guard.Check(root.destination, failure), root.destination) &&
            MoveFileExW(root.source.c_str(), root.destination.c_str(), MOVEFILE_WRITE_THROUGH)) {
            completed_destinations.push_back(root.destination);
            PushUndo(req, &completed_destinations);
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (req.undo_revision) {
                    move_undo_reservations_.erase(req.undo_revision);
                    ++undo_revision_;
                }
            }
            SetStatus([&](OpStatus& st) {
                st.active = false; st.phase = OpPhase::Completed; st.percent = 100.0f;
                st.total_items = st.completed_items = 1;
                st.summary = Describe(req) + L" 完成"; ++st.completed_ops;
            });
            transfer_active_.store(false);
            return;
        }
        if (failure.empty()) {
            const auto error = GetLastError();
            if (error != ERROR_NOT_SAME_DEVICE) fail_win32(error, root.source);
        }
    }

    uint64_t total_bytes = 0;
    for (const auto& entry : entries) total_bytes += entry.bytes;
    SetStatus([&](OpStatus& st) {
        st.total_bytes = total_bytes;
        st.total_items = entries.size();
        if (failure.empty()) {
            st.phase = OpPhase::Running;
            st.percent = 0.0f;
            st.summary = L"正在" + std::wstring(OpVerb(req.type)) + L" "
                + std::to_wstring(entries.size()) + L" 个项目";
        }
    });

    bool apply_all = req.collision_policy != CollisionPolicy::System;
    ConflictChoice repeated = req.collision_policy == CollisionPolicy::KeepBoth
        ? ConflictChoice::KeepBoth : ConflictChoice::Replace;
    std::vector<std::wstring> skipped_prefixes;
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
        bool skipped = false;
        for (const auto& prefix : skipped_prefixes) {
            if (StartsWithPath(entry.source, prefix)) { skipped = true; break; }
        }
        if (skipped) {
            mark_skipped(entry);
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
                st.summary = L"正在等待处理文件冲突";
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
                st.summary = L"正在" + std::wstring(OpVerb(req.type)) + L" "
                    + std::to_wstring(entries.size()) + L" 个项目";
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
                failure = L"撤销必须恢复完整原名，不能保留两个名称：" + entry.destination;
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
                failure = L"保留两者的新目标在确认后出现链接，操作已停止：" + unique;
            if (!failure.empty() || !guard_result(destination_guard.PinDirectories(ParentOf(entry.destination), failure), ParentOf(entry.destination)) ||
                !guard_result(destination_guard.Check(entry.destination, failure), entry.destination)) break;
        }
        const bool destination_reparse = destination_exists &&
            (GetFileAttributesW(entry.destination.c_str()) & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
        if (conflict && choice == ConflictChoice::Replace &&
            (entry.directory != destination_is_directory || entry.reparse || destination_reparse)) {
            if (req.type == OpType::Move) {
                failure = L"移动时无法原子替换不同类型或重解析目标：" + entry.destination;
                break;
            }
            const std::wstring backup = UniqueTemporaryPath(
                entry.destination, L".pulse-backup-", task_id, index);
            DWORD pin_error = ERROR_SUCCESS;
            auto leaf = destination_guard.PinLeaf(entry.destination, failure, &pin_error);
            if (!leaf) { fail_win32(pin_error, entry.destination); break; }
            if (!guard_result(destination_guard.Check(entry.destination, failure), entry.destination)) break;
            if (!DestinationGuard::RenameHandle(leaf.get(), backup)) {
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

        if (entry.reparse) {
            if (!guard_result(destination_guard.EnsureDirectories(ParentOf(entry.destination), failure), ParentOf(entry.destination)) ||
                !guard_result(destination_guard.Check(entry.destination, failure), entry.destination)) break;
            if (!CopyReparsePoint(entry, destination_guard, failure, failure_hr, failed_path)) break;
            if (req.type == OpType::Move) {
                const BOOL removed = entry.directory ? RemoveDirectoryW(entry.source.c_str())
                                                     : DeleteFileW(entry.source.c_str());
                if (!removed) {
                    const DWORD error = GetLastError();
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

        if (!guard_result(destination_guard.EnsureDirectories(ParentOf(entry.destination), failure), ParentOf(entry.destination)) ||
            !guard_result(destination_guard.Check(entry.destination, failure), entry.destination)) break;

        if (req.type == OpType::Move && !destination_exists) {
            if (MoveFileExW(entry.source.c_str(), entry.destination.c_str(), MOVEFILE_WRITE_THROUGH)) {
                if (!guard_result(destination_guard.RefreshOwnedLeaf(entry.destination, failure), entry.destination)) break;
                completed_sources.push_back(entry.source);
                completed_destinations.push_back(entry.destination);
                ++processed_items;
                publish_progress(entry, entry.bytes, entry.bytes, processed_items);
                committed_bytes += entry.bytes;
                continue;
            }
            const DWORD code = GetLastError();
            if (code != ERROR_NOT_SAME_DEVICE) {
                fail_win32(code, entry.source);
                break;
            }
        }
        std::wstring copy_destination = UniqueTemporaryPath(
            entry.destination, L".pulse-copy-", task_id, index);
        const bool replacing = destination_exists && choice == ConflictChoice::Replace;

        uint64_t known_file_total = entry.bytes;
        CopyProgressContext context;
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
            if (destination_guard.AdoptCopyLeaf(copy_destination, context.destination_id,
                    context.destination_owned, ignored)) destination_guard.DeleteOwnedLeaf(copy_destination, ignored);
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
            copy_result = CopyFile2(entry.source.c_str(), copy_destination.c_str(), &parameters);
            if (copy_result == HRESULT_FROM_WIN32(ERROR_INVALID_PARAMETER) && extra_flags != 0 && !resume) {
                extra_flags = 0;
                continue;
            }
            if (copy_result != HRESULT_FROM_WIN32(ERROR_REQUEST_PAUSED)) break;

            SetStatus([&](OpStatus& st) {
                st.phase = OpPhase::Paused;
                st.summary = std::wstring(OpVerb(req.type)) + L"已暂停";
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
            resume = true;
            reset_rate();
            SetStatus([&](OpStatus& st) {
                st.phase = OpPhase::Running;
                st.summary = L"正在" + std::wstring(OpVerb(req.type)) + L" "
                    + std::to_wstring(entries.size()) + L" 个项目";
            });
        }
        if (cancelled || transfer_cancel_.load()) {
            cancelled = true;
            cleanup_temporary();
            break;
        }
        if (FAILED(copy_result)) {
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
                st.summary = L"正在校验 " + FileName(entry.source);
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
                failure = L"SHA-256 校验失败 | " + entry.source;
                break;
            }
            SetStatus([&](OpStatus& st) { st.phase = OpPhase::Running; });
        }

        if (!guard_result(destination_guard.AdoptCopyLeaf(copy_destination, context.destination_id,
                context.destination_owned, failure), copy_destination) ||
            !guard_result(destination_guard.Check(entry.destination, failure), entry.destination)) {
            cleanup_temporary();
            break;
        }
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
            if (!DestinationGuard::RenameHandle(leaf.get(), backup)) {
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
        DWORD pin_error = ERROR_SUCCESS;
        auto temporary = destination_guard.PinLeaf(copy_destination, failure, &pin_error);
        if (!temporary) {
            fail_win32(pin_error, copy_destination);
            cleanup_temporary();
            break;
        }
        if (!guard_result(destination_guard.Check(entry.destination, failure), entry.destination)) { cleanup_temporary(); break; }
        if (!DestinationGuard::RenameHandle(temporary.get(), entry.destination)) {
            const DWORD error = GetLastError();
            fail_win32(error, entry.destination);
            cleanup_temporary();
            break;
        }

        temporary.reset();
        if (!guard_result(destination_guard.AdoptCopyLeaf(entry.destination, context.destination_id,
                context.destination_owned, failure), entry.destination)) break;

        if (req.type == OpType::Move) {
            DWORD source_attributes = GetFileAttributesW(entry.source.c_str());
            if (source_attributes != INVALID_FILE_ATTRIBUTES &&
                (source_attributes & FILE_ATTRIBUTE_READONLY))
                SetFileAttributesW(entry.source.c_str(), source_attributes & ~FILE_ATTRIBUTE_READONLY);
            if (!DeleteFileW(entry.source.c_str())) {
                const DWORD error = GetLastError();
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
            if (!it->directory) continue;
            if (req.type == OpType::Move) {
                if (std::any_of(skipped_prefixes.begin(), skipped_prefixes.end(), [&](const auto& prefix) {
                        return StartsWithPath(it->source, prefix) || StartsWithPath(prefix, it->source);
                    })) continue;
                if (!RemoveDirectoryW(it->source.c_str())) {
                    const DWORD error = GetLastError();
                    if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) {
                        fail_win32(error, it->source);
                        break;
                    }
                }
                removed_directories.push_back(it->source);
            }
        }
    }

    if (failure.empty() && !cancelled && req.type == OpType::Move) {
        for (auto it = entries.rbegin(); it != entries.rend(); ++it) {
            if (it->directory && !it->reparse &&
                !guard_result(destination_guard.SetDirectoryMetadata(it->destination,
                    it->created, it->accessed, it->modified, it->attributes, failure), it->destination)) break;
        }
    }

    std::vector<std::wstring> restored_backups;
    for (auto it = replacement_backups.rbegin(); it != replacement_backups.rend(); ++it) {
        const bool committed_move = req.type == OpType::Move &&
            std::find(completed_destinations.begin(), completed_destinations.end(), it->original) != completed_destinations.end();
        std::wstring cleanup_error;
        if (!destination_guard.Check(it->backup, cleanup_error)) continue;
        if ((failure.empty() && !cancelled) || committed_move) {
            // Handle deletion removes a file/reparse name or an empty directory.
            // Never recursively delete a renamed directory's unverified children.
            destination_guard.DeleteOwnedLeaf(it->backup, cleanup_error);
        } else {
            if (!destination_guard.Check(it->original, cleanup_error)) continue;
            if (PathExists(it->original) &&
                !destination_guard.DeleteOwnedLeaf(it->original, cleanup_error)) continue;
            auto backup = destination_guard.PinLeaf(it->backup, cleanup_error);
            if (!backup || !destination_guard.Check(it->original, cleanup_error) ||
                !DestinationGuard::RenameHandle(backup.get(), it->original)) continue;
            destination_guard.RefreshOwnedLeaf(it->original, cleanup_error);
            restored_backups.push_back(it->original);
        }
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
            if (root == entries.end() || !root->directory || root->reparse) continue;
            std::vector<std::wstring> remaining_actual, remaining_original;
            bool had_success = false;
            for (const auto& entry : entries) {
                if (!StartsWithPath(entry.source, root->source)) continue;
                if (std::find(completed_sources.begin(), completed_sources.end(), entry.source) != completed_sources.end()) {
                    had_success = true;
                    continue;
                }
                if (entry.directory && !entry.reparse) continue;
                remaining_actual.push_back(entry.source);
                remaining_original.push_back(entry.destination);
            }
            if (!had_success) continue;
            // Empty ordinary directories can remain after per-file move success.
            // Add them deepest-first as merge inverses, without replaying moved files.
            for (auto entry = entries.rbegin(); entry != entries.rend(); ++entry) {
                if (!entry->directory || entry->reparse || !StartsWithPath(entry->source, root->source)) continue;
                if (!RemoveDirectoryW(entry->source.c_str())) {
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
                    }
                }
            }
            if (remaining_actual.empty()) {
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
                remainder.sources.insert(remainder.sources.end(), remaining_original.begin(), remaining_original.end());
                remainder.destinations.insert(remainder.destinations.end(), remaining_actual.begin(), remaining_actual.end());
                // Retry known remaining leaves first, then merge the leftover root.
                // Its next scan contains no already-moved files, so no inverse repeats.
                if (PathExists(root->source) && std::find(remaining_actual.begin(), remaining_actual.end(),
                        root->source) == remaining_actual.end()) {
                    remainder.sources.push_back(root->destination);
                    remainder.destinations.push_back(root->source);
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
                    std::all_of(entries.begin(), entries.end(), [&](const auto& entry) {
                        return !StartsWithPath(entry.source, root->source) || entry.directory ||
                            std::find(completed_sources.begin(), completed_sources.end(), entry.source) != completed_sources.end();
                    });
                if (!confirmed_file && !confirmed_directory) continue;
                committed.sources.push_back(req.sources[i]);
                committed_destinations.push_back(req.new_names[i]);
            }
        }
        if (!committed_destinations.empty()) PushUndo(committed, &committed_destinations);
    } else if (!completed_destinations.empty()) {
        OpRequest partial = req;
        partial.sources = completed_sources;
        PushUndo(partial, &completed_destinations);
    }

    {
        std::lock_guard<std::mutex> lock(transfer_control_mutex_);
        pending_conflict_.reset();
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (req.undo_revision) {
            move_undo_reservations_.erase(req.undo_revision);
            ++undo_revision_;
        }
    }
    LockReport locks;
    if (!failure.empty() && !cancelled && !transfer_cancel_.load() && !req.undo_revision) {
        QueueItem retry;
        retry.req = req; retry.seq = task_id;
        retry.retry_completed_sources = prior_completed;
        retry.retry_completed_sources.insert(retry.retry_completed_sources.end(), completed_sources.begin(), completed_sources.end());
        retry.retry_completed_sources.insert(retry.retry_completed_sources.end(), skipped_sources.begin(), skipped_sources.end());
        retry.retry_completed_sources.insert(retry.retry_completed_sources.end(), removed_directories.begin(), removed_directories.end());
        retry.retry_transfer_plan = prior_plan;
        if (plan_complete) {
            for (const auto& entry : entries) {
                const auto prior = std::find_if(retry.retry_transfer_plan.begin(), retry.retry_transfer_plan.end(),
                    [&](const auto& planned) { return planned.source == entry.source; });
                if (prior == retry.retry_transfer_plan.end()) retry.retry_transfer_plan.push_back(entry);
                else *prior = entry; // retain nested KeepBoth remapping, not just root targets
            }
        } // A failed initial scan made no mutations; retry may finish its scan.
        retry.retry_probe_paths = failed_candidates;
        locks = CaptureLockedFailure(retry, failure_hr, failed_path,
            [&] { return stopping_.load() || transfer_cancel_.load(); });
    }
    SetStatus([&](OpStatus& st) {
        st.active = false;
        st.failure_hr = failure.empty() ? S_OK : FAILED(failure_hr) ? failure_hr : E_FAIL;
        st.failed_path = failed_path;
        st.locked_path = locks.path; st.lock_owners = locks.owners;
        st.completed_ops++;
        st.bytes_per_second = 0.0;
        st.eta_seconds = 0;
        if (cancelled || transfer_cancel_.load()) {
            st.phase = OpPhase::Failed;
            st.last_error = L"已取消";
            st.summary = std::wstring(OpVerb(req.type)) + L"已取消";
        } else if (!failure.empty()) {
            st.phase = OpPhase::Failed;
            st.last_error = failure;
            st.summary = std::wstring(OpVerb(req.type)) + L"失败";
        } else {
            st.phase = OpPhase::Completed;
            st.percent = 100.0f;
            st.transferred_bytes = st.total_bytes;
            st.completed_items = st.total_items;
            st.summary = Describe(req) + L" 完成";
            st.last_error.clear();
        }
    });
    transfer_active_.store(false);
}

void OpsManager::RunShellOp(const OpRequest& req, uint64_t task_id, const FILE_ID_INFO* rename_identity, bool lock_retry) {
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
    shell_activity_tick_ = GetTickCount64();
    // Status: active.
    SetStatus([&](OpStatus& st) {
        st.active = true;
        st.type = req.type;
        st.task_id = task_id;
        st.phase = OpPhase::Running;
        st.percent = 0.0f;
        st.summary = Describe(req);
        st.last_error.clear();
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

    auto& client = ipc::ShellClient::Instance();

    if (req.type == OpType::Rename || req.type == OpType::BatchRename) {
        bool valid = !req.sources.empty() && (req.type != OpType::Rename || req.sources.size() == 1);
        for (size_t i = 0; i < req.sources.size(); ++i) {
            const auto& name = req.type == OpType::BatchRename && i < req.new_names.size() ? req.new_names[i] : req.new_name;
            valid = valid && !req.sources[i].empty() && IsRenameComponent(name);
        }
        if (!valid || shell_cancel_requested_.load() || stopping_.load() || (lock_retry && lock_retry_cancel_.load())) {
            SetStatus([&](OpStatus& st) {
                st.active = false; st.phase = OpPhase::Failed; st.percent = -1.0f;
                ++st.completed_ops; st.last_error = valid ? L"已取消" : L"名称无效";
            });
            return;
        }
    }

    if (req.type == OpType::BatchRename) {
        std::vector<std::wstring> ok_sources;
        std::vector<std::wstring> ok_names;
        std::vector<std::wstring> ok_destinations;
        std::wstring last_error;
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
            const auto local = RenameInProcess(req.sources[i], name, &local_error);
            if (local == RenameResult::Rejected) { last_error = local_error; continue; }
            if (local == RenameResult::Completed) {
                ok_sources.push_back(req.sources[i]);
                ok_names.push_back(name);
                ok_destinations.push_back(JoinPath(ParentOf(req.sources[i]), name));
                continue;
            }

        }
        current_req_id_.store(0);
        shell_cancel_requested_ = false;
        if (!ok_sources.empty()) {
            OpRequest recorded = req;
            recorded.sources = ok_sources;
            recorded.new_names = ok_names;
            PushUndo(recorded, &ok_destinations);
        }
        SetStatus([&](OpStatus& st) {
            st.active = false;
            st.percent = -1.0f;
            st.completed_ops++;
            st.completed_items = ok_sources.size();
            if (cancelled) {
                st.phase = OpPhase::Failed;
                st.last_error = L"已取消";
            } else if (ok_sources.empty()) {
                st.phase = OpPhase::Failed;
                st.last_error = last_error.empty() ? L"操作失败" : last_error;
            } else {
                st.phase = OpPhase::Completed;
                st.summary = Describe(req) + L" 完成";
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
        FILE_ID_INFO identity{};
        bool identified = false;
        const auto local = RenameInProcess(req.sources.front(), req.new_name, &local_error,
            &code, &identity, rename_identity, &identified);
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
                ++st.completed_ops; st.last_error = local_error;
                st.failure_hr = HRESULT_FROM_WIN32(code); st.failed_path = req.sources.front();
                st.lock_retry_available = sharing && identified && !cancelled();
                st.locked_path = sharing ? req.sources.front() : L""; st.lock_owners = locks.owners;
            });
            return;
        }
        if (local == RenameResult::Completed) {
            PushUndo(req);
            SetStatus([&](OpStatus& st) {
                st.active = false;
                st.percent = -1.0f;
                st.completed_ops++;
                st.completed_items = st.total_items;
                st.phase = OpPhase::Completed;
                st.summary = Describe(req) + L" 完成";
            });
            return;
        }
    }

    if (stopping_.load() || shell_cancel_requested_.load()) {
        SetStatus([&](OpStatus& st) {
            st.active = false; st.phase = OpPhase::Cancelled;
            st.last_error.clear(); st.summary = L"已取消"; ++st.completed_ops;
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
                RunShellOp(verified, task_id);
                return;
            }
        }
        if (!valid && req.is_undo) {
            SetStatus([&](OpStatus& st) {
                st.active = false; st.phase = OpPhase::Failed; ++st.completed_ops;
                st.last_error = L"回收项身份已变化，无法撤销删除";
            });
            RefreshRecycleUndoValidity();
            return;
        }
        id = client.RestoreRecycle(req.sources, req.recycle_paths);
        break;
    }
    }
    current_req_id_.store(id);
    if (shell_cancel_requested_.load() && id != 0) client.Cancel(id);

    if (id == 0) {
        SetStatus([&](OpStatus& st) {
            st.active = false;
            st.phase = OpPhase::Failed;
            st.percent = -1.0f;
            st.last_error = L"操作层未启动";
            st.completed_ops++;
        });
        return;
    }

    uint32_t hr = 0;
    bool cancelled = false;
    std::wstring error;
    if (!WaitShellDone(id, hr, cancelled, error)) return;

    const bool ok = SUCCEEDED((HRESULT)hr) && !cancelled;
    if (ok) PushUndo(req);

    SetStatus([&](OpStatus& st) {
        st.active = false;
        st.percent = -1.0f;
        st.completed_ops++;
        if (cancelled) {
            st.phase = OpPhase::Failed;
            st.last_error = L"已取消";
        } else if (FAILED((HRESULT)hr)) {
            st.phase = OpPhase::Failed;
            st.last_error = error.empty() ? L"操作失败" : error;
        } else {
            st.phase = OpPhase::Completed;
            st.completed_items = st.total_items;
            st.summary = Describe(req) + L" 完成";
        }
    });
}

bool OpsManager::WaitShellDone(uint32_t id, uint32_t& hr, bool& cancelled, std::wstring& error) {
    auto& client = ipc::ShellClient::Instance();
    constexpr ULONGLONG kShellInactivityTimeoutMs = 10ull * 60ull * 1000ull;
    {
        std::unique_lock<std::mutex> lock(done_mutex_);
        while (!(done_ready_ && done_id_ == id) && !stopping_.load()) {
            done_cv_.wait_for(lock, std::chrono::seconds(5));
            if (GetTickCount64() - shell_activity_tick_.load() < kShellInactivityTimeoutMs)
                continue;
            lock.unlock();
            client.Abort(id);
            lock.lock();
        }
        if (stopping_.load() && !(done_ready_ && done_id_ == id)) {
            current_req_id_.store(0);
            shell_cancel_requested_ = false;
            SetStatus([](OpStatus& st) {
                st.active = false;
                st.phase = OpPhase::Failed;
                st.last_error = L"操作已停止";
                st.summary = L"操作已停止";
                st.completed_ops++;
            });
            return false;
        }
    }
    hr = done_hr_;
    cancelled = done_cancelled_;
    error = std::move(done_error_);
    done_ready_ = false;
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
        out += L"]}";
        if (i + 1 < undo_.size()) out += L",";
        out += L"\n";
    }
    out += L"]";
    return out;
}

bool OpsManager::UndoFromJson(const std::wstring& in) {
    std::deque<UndoEntry> parsed;
    size_t i = in.find(L'[');
    if (i == std::wstring::npos) return false;
    ++i;
    auto skipWs = [&] {
        while (i < in.size() && (in[i] == L' ' || in[i] == L'\n' || in[i] == L'\r' || in[i] == L'\t' || in[i] == L',')) ++i;
    };
    auto skipSpace = [&] {
        while (i < in.size() && (in[i] == L' ' || in[i] == L'\n' ||
               in[i] == L'\r' || in[i] == L'\t')) ++i;
    };
    auto readString = [&](std::wstring& out) -> bool {
        skipWs();
        if (i >= in.size() || in[i] != L'"') return false;
        ++i;
        out.clear();
        while (i < in.size() && in[i] != L'"') {
            if (in[i] == L'\\' && i + 1 < in.size()) {
                ++i;
                if (in[i] == L'n') out += L'\n';
                else if (in[i] == L'r') out += L'\r';
                else if (in[i] == L't') out += L'\t';
                else out += in[i];
            } else {
                out += in[i];
            }
            ++i;
        }
        if (i < in.size()) ++i;
        return true;
    };
    auto readValue = [&](const std::wstring& key, std::wstring& val) -> bool {
        skipWs();
        if (i >= in.size() || in[i] != L'"') return false;
        std::wstring k;
        if (!readString(k)) return false;
        skipWs();
        if (i >= in.size() || in[i] != L':') return false;
        ++i;
        skipWs();
        if (k != key) return false;
        if (i < in.size() && in[i] == L'"') return readString(val);
        size_t start = i;
        while (i < in.size() && in[i] != L',' && in[i] != L'}' && in[i] != L']') ++i;
        val = in.substr(start, i - start);
        while (!val.empty() && (val.back() == L' ')) val.pop_back();
        return true;
    };

    while (true) {
        skipWs();
        if (i >= in.size() || in[i] == L']') break;
        if (in[i] != L'{') return false;
        ++i;
        UndoEntry e;
        std::wstring v;
        if (!readValue(L"type", v)) return false;
        e.type = (OpType)_wtoi(v.c_str());
        skipWs();
        if (i < in.size() && in[i] == L',') ++i;
        if (!readValue(L"sup", v)) return false;
        e.supported = (v == L"true");
        skipWs();
        if (i < in.size() && in[i] == L',') ++i;
        if (!readValue(L"dest", e.dest_dir)) return false;
        skipWs();
        if (i < in.size() && in[i] == L',') ++i;
        if (!readValue(L"name", e.new_name)) return false;
        skipWs();
        if (i < in.size() && in[i] == L',') ++i;
        // src array
        skipWs();
        if (i >= in.size() || in[i] != L'"') return false;
        std::wstring k;
        if (!readString(k) || k != L"src") return false;
        skipWs();
        if (i >= in.size() || in[i] != L':') return false;
        ++i;
        skipWs();
        if (i >= in.size() || in[i] != L'[') return false;
        ++i;
        while (true) {
            skipWs();
            if (i >= in.size()) return false;
            if (in[i] == L']') { ++i; break; }
            std::wstring s;
            if (!readString(s)) return false;
            e.sources.push_back(std::move(s));
        }
        skipSpace();
        // Version 2 adds actual committed destination paths. Version 1 ended
        // the object after src, so this field must remain optional.
        if (i < in.size() && in[i] == L',') {
            ++i;
            skipSpace();
            std::wstring destination_key;
            if (!readString(destination_key) || destination_key != L"dst") return false;
            skipSpace();
            if (i >= in.size() || in[i] != L':') return false;
            ++i;
            skipSpace();
            if (i >= in.size() || in[i] != L'[') return false;
            ++i;
            while (true) {
                skipWs();
                if (i >= in.size()) return false;
                if (in[i] == L']') { ++i; break; }
                std::wstring destination;
                if (!readString(destination)) return false;
                e.destinations.push_back(std::move(destination));
            }
            skipSpace();
        }
        if (i < in.size() && in[i] == L'}') ++i;
        parsed.push_back(std::move(e));
    }
    std::lock_guard<std::mutex> lock(mutex_);
    undo_ = std::move(parsed);
    undo_validation_requested_.store(true);
    ++undo_revision_;
    return true;
}

} // namespace pulse::ops
