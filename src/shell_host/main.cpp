#include "../common/windows_compat.h"
#include "../common/file_operation_error.h"
#include "../common/progress_percent.h"
#include "host_log_line.h"
// main.cpp — Pulse.Shell.exe: windowless STA+COM shell-operation proxy.
//
// Pipe server on \\.\pipe\pulse_shell_<ui-pid> (ui-pid passed as argv[1],
// defaults to own pid for standalone runs). Requests are read on a dedicated
// thread and marshalled to the STA main thread (message-only window), where
// IFileOperation executes synchronously with an IFileOperationProgressSink
// reporting RSP_PROGRESS / RSP_DONE back over the pipe.
//
// Only read-only requests may be retried after restart (see src/ipc/shell_client.cpp).
// Launch requests use separate workers and are never automatically replayed.
#include "../ipc/protocol.h"
#include "../ipc/delete_plan_protocol.h"
#include "../ipc/ctx_menu_util.h"
#include "ctx_handlers.h"
#include "ctx_session_process.h"
#include "shell_launch_process.h"
#include "shell_query_worker.h"
#include "deletion_identity.h"
#include "delete_fileop.h"
#include "recycle_fileop.h"
#include "../common/current_user_security.h"
#include "../common/path_utils.h"
#include "../common/file_create.h"
#include "../common/file_identity.h"
#include "../ipc/file_identity_protocol.h"
#include "../common/crash_reporter.h"
#ifdef PULSE_OPERATION_ELEVATION_HOST
#include "../ops/operation_elevation_adapter.h"
#endif
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <algorithm>
#include <cstdio>
#include <atomic>
#include <map>
#include <set>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <cwctype>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "uuid.lib")

using namespace pulse::ipc;

namespace {

constexpr UINT WM_EXEC_REQUEST = WM_APP + 1;
constexpr UINT WM_QUIT_HOST = WM_APP + 2;
// Context-menu session thread messages (defined below with the session code).
constexpr UINT WM_CTX_INVOKE = WM_APP + 10;  // lParam = CtxInvokeMsg*
constexpr UINT WM_CTX_CLOSE = WM_APP + 11;

struct CtxInvokeMsg {
    uint32_t invoke_req_id = 0;
    uint32_t item_id = 0;
    std::wstring verb;
    std::wstring text;
};

void StartCtxSession(uint32_t session_id, const uint8_t* payload, size_t size);
void PostCtxMessage(uint32_t session_id, UINT message, WPARAM wParam, LPARAM lParam);

struct Request {
    uint32_t type = 0;
    uint32_t id = 0;
    std::vector<std::wstring> sources;
    uint64_t delete_epoch = 0;
    uint64_t delete_token = 0;
    std::vector<pulse::FileIdentity> expected_identities;
    HWND owner = nullptr;
};

bool IsAuthorizedDeletion(uint32_t type) {
    return type == REQ_AUTHORIZED_DELETE || type == REQ_AUTHORIZED_RECYCLE;
}

struct HostState {
    HANDLE pipe = INVALID_HANDLE_VALUE;
    DWORD client_pid = 0;
    std::mutex write_mutex;      // reader thread (PONG) vs main thread (progress/done)
    HWND hwnd_msg = nullptr;
    std::atomic<uint32_t> cancel_id{0};
    std::atomic<bool> running{true};
} g;

bool g_context_worker = false;
std::unique_ptr<pulse::shell::ContextSessionProcesses> g_context_processes;
std::unique_ptr<pulse::shell::ShellLaunchProcesses> g_launch_processes;
std::unique_ptr<pulse::shell::ShellLaunchProcesses> g_interaction_processes;

// Crash-only diagnostics (plan §11: crashes are a normal design case).
// Log under %LOCALAPPDATA%\Pulse: an installed copy lives in Program Files,
// where the exe directory is not writable for a non-admin user.
void HostLog(const wchar_t* msg) {
    wchar_t dir[MAX_PATH]{};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, dir))) return;
    std::wstring path = std::wstring(dir) + L"\\Pulse";
    CreateDirectoryW(path.c_str(), nullptr);
    path += L"\\pulse_shell_host.log";
    if (HANDLE f = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
            OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr); f != INVALID_HANDLE_VALUE) {
        const auto line = pulse::shell::FormatHostLogLine(GetCurrentProcessId(), msg);
        if (line.length) {
            DWORD written = 0;
            WriteFile(f, line.bytes.data(), static_cast<DWORD>(line.length), &written, nullptr);
        }
        CloseHandle(f);
    }
}

bool SendMsg(uint32_t type, uint32_t request_id, const std::vector<uint8_t>& payload,
             const PipeIoOptions& options = {}) {
    MsgHeader h;
    h.type = type;
    h.request_id = request_id;
    h.payload_size = (uint32_t)payload.size();
    // Header + payload under one lock: ctx session threads write concurrently
    // with the main STA, an interleaved frame would desync the whole pipe.
    std::lock_guard<std::mutex> lock(g.write_mutex);
    if (!g.running.load()) return false;
    if (!PipeWrite(g.pipe, reinterpret_cast<const uint8_t*>(&h), sizeof(h), options)) return false;
    if (!payload.empty())
        return PipeWrite(g.pipe, payload.data(), (DWORD)payload.size(), options);
    return true;
}

void SendProgress(uint32_t id, float percent, const std::wstring& item,
                  size_t items_done, size_t total_items) {
    PayloadWriter w;
    w.PutF32(percent);
    w.PutString(item);
    w.PutU32(static_cast<uint32_t>((std::min)(items_done,
        static_cast<size_t>(UINT32_MAX))));
    w.PutU32(static_cast<uint32_t>((std::min)(total_items,
        static_cast<size_t>(UINT32_MAX))));
    SendMsg(RSP_PROGRESS, id, w.data());
}

void SendDeleteDone(uint32_t id, HRESULT hr, bool cancelled, const std::wstring& error,
                    const std::vector<std::wstring>& deleted_paths,
                    const std::vector<std::wstring>& recycled_paths = {},
                    const std::vector<std::wstring>& recycle_destinations = {},
                    const std::wstring& failed_path = {}) {
    PayloadWriter writer;
    writer.PutU32(static_cast<uint32_t>(hr)); writer.PutU32(cancelled ? 1 : 0);
    writer.PutString(error); writer.PutStringArray(deleted_paths); writer.PutStringArray(recycled_paths); writer.PutStringArray(recycle_destinations);
    writer.PutString(failed_path);
    SendMsg(RSP_DONE, id, writer.data());
}

void SendDone(uint32_t id, HRESULT hr, bool cancelled, const std::wstring& error,
              const std::vector<pulse::FileIdentity>& produced_identities = {}) {
    PayloadWriter w;
    w.PutU32((uint32_t)hr);
    w.PutU32(cancelled ? 1 : 0);
    w.PutString(error);
    if (!produced_identities.empty()) WriteFileIdentities(w, produced_identities);
    SendMsg(RSP_DONE, id, w.data());
}

// ---------------------------------------------------------------------------
// Progress sink: lives on the STA main thread, drives RSP_PROGRESS + cancel.
// ---------------------------------------------------------------------------
class ProgressSink : public IFileOperationProgressSink {
public:
    ProgressSink(uint32_t req_id, size_t total_items, std::wstring authorized_root = {})
        : authorized_root_(std::move(authorized_root)), req_id_(req_id),
          total_items_(total_items ? total_items : 1) {}

    // IUnknown — stack-allocated, no real refcounting.
    IFACEMETHODIMP QueryInterface(REFIID riid, void** out) override {
        if (!out) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IFileOperationProgressSink) {
            *out = static_cast<IFileOperationProgressSink*>(this);
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    IFACEMETHODIMP_(ULONG) AddRef() override { return 2; }
    IFACEMETHODIMP_(ULONG) Release() override { return 1; }

    IFACEMETHODIMP StartOperations() override { return CheckCancel(); }
    IFACEMETHODIMP FinishOperations(HRESULT) override { return S_OK; }

    IFACEMETHODIMP PreRenameItem(DWORD, IShellItem* psi, LPCWSTR new_name) override {
        if (new_name) RememberName(new_name);
        else RememberItem(psi);
        return CheckCancel();
    }
    IFACEMETHODIMP PostRenameItem(DWORD, IShellItem*, LPCWSTR, HRESULT hr, IShellItem*) override {
        NoteItemResult(hr);
        ++items_done_;
        MaybeReport(items_done_ == total_items_);
        return CheckCancel();
    }
    IFACEMETHODIMP PreMoveItem(DWORD, IShellItem* psi, IShellItem*, LPCWSTR) override {
        RememberItem(psi);
        return CheckCancel();
    }
    IFACEMETHODIMP PostMoveItem(DWORD, IShellItem*, IShellItem*, LPCWSTR, HRESULT hr, IShellItem*) override {
        NoteItemResult(hr);
        ++items_done_;
        MaybeReport(items_done_ == total_items_);
        return CheckCancel();
    }
    IFACEMETHODIMP PreCopyItem(DWORD, IShellItem* psi, IShellItem*, LPCWSTR) override {
        RememberItem(psi);
        return CheckCancel();
    }
    IFACEMETHODIMP PostCopyItem(DWORD, IShellItem*, IShellItem*, LPCWSTR, HRESULT hr, IShellItem*) override {
        NoteItemResult(hr);
        ++items_done_;
        MaybeReport(items_done_ == total_items_);
        return CheckCancel();
    }
    IFACEMETHODIMP PreDeleteItem(DWORD, IShellItem* psi) override {
        RememberItem(psi);
        return CheckCancel();
    }
    IFACEMETHODIMP PostDeleteItem(DWORD, IShellItem* source, HRESULT hr, IShellItem* destination) override {
        NoteItemResult(hr);
        if (GetEnvironmentVariableW(L"PULSE_DELETE_DIAGNOSTICS", nullptr, 0)) {
            wchar_t result[96]{};
            swprintf_s(result, L"post-delete hr=0x%08X destination=%d", static_cast<unsigned>(hr), destination != nullptr);
            delete_diagnostics_ += result;
            PWSTR diagnostic_path = nullptr;
            const HRESULT path_hr = source ? source->GetDisplayName(SIGDN_FILESYSPATH, &diagnostic_path) : E_POINTER;
            swprintf_s(result, L" path_hr=0x%08X path=", static_cast<unsigned>(path_hr));
            delete_diagnostics_ += result;
            if (diagnostic_path) { delete_diagnostics_ += diagnostic_path; CoTaskMemFree(diagnostic_path); }
        }
        // Per-item sinks bind the result to the exact queued root. A Shell item
        // queried after deletion may already resolve only to its parent/root.
        // DONT_PROCESS_CHILDREN is a completed-item success, not a skipped item.
        if (!authorized_root_.empty() &&
            (hr == S_OK || hr == COPYENGINE_S_DONT_PROCESS_CHILDREN)) {
            if (destination) {
                recycled_paths_.push_back(authorized_root_);
                PWSTR path = nullptr;
                const HRESULT result = destination->GetDisplayName(SIGDN_FILESYSPATH, &path);
                recycle_destinations_.push_back(SUCCEEDED(result) && path ? path : L"");
                if (path) CoTaskMemFree(path);
            } else deleted_paths_.push_back(authorized_root_);
        }
        ++items_done_;
        MaybeReport(items_done_ == total_items_);
        return CheckCancel();
    }
    IFACEMETHODIMP PreNewItem(DWORD, IShellItem*, LPCWSTR) override { return CheckCancel(); }
    IFACEMETHODIMP PostNewItem(DWORD, IShellItem*, LPCWSTR, LPCWSTR, DWORD, HRESULT, IShellItem*) override { return CheckCancel(); }

    IFACEMETHODIMP UpdateProgress(UINT work_total, UINT work_done) override {
        work_percent_ = pulse::WorkProgressPercent(work_done, work_total);
        MaybeReport();
        return CheckCancel();
    }
    IFACEMETHODIMP ResetTimer() override { return S_OK; }
    IFACEMETHODIMP PauseTimer() override { return S_OK; }
    IFACEMETHODIMP ResumeTimer() override { return S_OK; }

    const std::wstring& last_failed_item() const { return last_failed_item_; }
    HRESULT item_failure() const { return item_failure_; }
    const std::wstring& delete_diagnostics() const { return delete_diagnostics_; }
    std::wstring delete_diagnostics_;
    const std::vector<std::wstring>& deleted_paths() const { return deleted_paths_; }
    std::vector<std::wstring> deleted_paths_;
    std::vector<std::wstring> recycled_paths_;
    std::vector<std::wstring> recycle_destinations_;
    void NoteSetupFailure(const std::wstring& src) { last_failed_item_ = src; }

private:
    HRESULT CheckCancel() {
        if (g.cancel_id.load() == req_id_)
            return HRESULT_FROM_WIN32(ERROR_CANCELLED);
        return S_OK;
    }
    void RememberItem(IShellItem* psi) {
        if (!psi) return;
        current_path_.clear();
        PWSTR path = nullptr;
        if (SUCCEEDED(psi->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
            current_path_ = path;
            CoTaskMemFree(path);
        }
        PWSTR name = nullptr;
        if (SUCCEEDED(psi->GetDisplayName(SIGDN_PARENTRELATIVEPARSING, &name)) && name) {
            current_item_ = name;
            CoTaskMemFree(name);
        }
    }
    void RememberName(LPCWSTR name) { current_item_ = name ? name : L""; }
    void NoteItemResult(HRESULT hr) {
        if (FAILED(hr) && hr != HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
            if (SUCCEEDED(item_failure_)) {
                item_failure_ = hr;
                last_failed_item_ = !authorized_root_.empty() ? authorized_root_
                    : current_path_.empty() ? current_item_ : current_path_;
            }
        }
    }
    void MaybeReport(bool force = false) {
        // Root sinks record exact deletion outcomes; only the advised sink
        // publishes progress for the entire operation.
        if (!authorized_root_.empty()) return;
        // Throttle to ~20 updates/sec to keep the pipe quiet.
        auto now = GetTickCount64();
        if (!force && now - last_send_ < 50) return;
        last_send_ = now;
        // PostDeleteItem includes descendants, while total_items_ counts queued
        // roots. Their ratio is not an operation percentage.
        SendProgress(req_id_, work_percent_, current_item_, items_done_, total_items_);
    }

    std::wstring authorized_root_;
    uint32_t req_id_ = 0;
    size_t total_items_ = 1;
    size_t items_done_ = 0;
    float work_percent_ = -1.0f;
    std::wstring current_item_;
    std::wstring current_path_;
    std::wstring last_failed_item_;
    HRESULT item_failure_ = S_OK;
    ULONGLONG last_send_ = 0;
};

// ---------------------------------------------------------------------------
// IFileOperation execution (STA main thread only).
// ---------------------------------------------------------------------------
std::wstring ToParsingPath(std::wstring path) {
    // IFileOperation / SHCreateItemFromParsingName reject \\?\ prefixes
    // (ERROR_INVALID_PARAMETER) even for short paths.
    return pulse::path::StripExtendedPathPrefix(path);
}

HRESULT MakeItem(const std::wstring& path, IShellItem** out) {
    const std::wstring parsed = ToParsingPath(path);
    HRESULT hr = SHCreateItemFromParsingName(parsed.c_str(), nullptr, IID_PPV_ARGS(out));
    if (FAILED(hr) && parsed != path)
        hr = SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(out));
    return hr;
}

std::wstring DescribeCreateError(HRESULT hr) {
    const DWORD code = HRESULT_FACILITY(hr) == FACILITY_WIN32
        ? HRESULT_CODE(hr) : static_cast<DWORD>(hr);
    switch (code) {
    case ERROR_ACCESS_DENIED:
    case ERROR_WRITE_PROTECT:
    case ERROR_PRIVILEGE_NOT_HELD:
        return L"没有权限在此位置新建";
    case ERROR_PATH_NOT_FOUND:
    case ERROR_FILE_NOT_FOUND:
        return L"目标文件夹不存在";
    case ERROR_FILE_EXISTS:
    case ERROR_ALREADY_EXISTS:
        return L"已存在同名项目";
    case ERROR_INVALID_NAME:
        return L"名称无效";
    default:
        break;
    }
    LPWSTR msg = nullptr;
    std::wstring text;
    if (FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                           FORMAT_MESSAGE_IGNORE_INSERTS,
                       nullptr, code, 0, reinterpret_cast<LPWSTR>(&msg), 0, nullptr) && msg) {
        text = msg;
        LocalFree(msg);
        while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n'))
            text.pop_back();
    }
    return text.empty() ? L"新建失败" : text;
}

// REQ_NEW_FOLDER / REQ_NEW_FILE: plain Win32 creation (no conflict UI, the UI
// side already picked a unique name). Runs on the STA thread like other ops.
HRESULT ExecuteCreate(const uint32_t type, const std::wstring& path,
                      std::vector<pulse::FileIdentity>& produced_identities) {
    if (path.empty()) return E_INVALIDARG;
    const std::wstring parsed = ToParsingPath(path);
    const wchar_t* create_path = parsed.empty() ? path.c_str() : parsed.c_str();
    if (GetFileAttributesW(create_path) != INVALID_FILE_ATTRIBUTES)
        return HRESULT_FROM_WIN32(ERROR_FILE_EXISTS);
    if (type == REQ_NEW_FOLDER) {
        const HANDLE folder = pulse::CreateDirectoryHandle(create_path);
        if (!folder) return HRESULT_FROM_WIN32(GetLastError());
        pulse::FileIdentity identity;
        if (pulse::ReadFileIdentity(folder, identity) && identity.Valid()) produced_identities.push_back(identity);
        CloseHandle(folder);
        return S_OK;
    }
    HANDLE f = CreateFileW(create_path, GENERIC_WRITE, 0, nullptr,
                           CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return HRESULT_FROM_WIN32(GetLastError());
    pulse::FileIdentity identity;
    if (pulse::ReadFileIdentity(f, identity) && identity.Valid()) produced_identities.push_back(identity);
    CloseHandle(f);
    return S_OK;
}

void ExecuteRequest(Request* req) {
    if (req->type == REQ_DELETE_RECYCLE || req->type == REQ_REALDELETE) {
        SendDone(req->id, E_ACCESSDENIED, false, L"Legacy deletion frame has no deletion-service authorization.");
        delete req;
        return;
    }
    if (IsAuthorizedDeletion(req->type)) {
        FILETIME created{}, exited{}, kernel{}, user{};
        ULARGE_INTEGER epoch{};
        const bool have_epoch = GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user) != FALSE;
        epoch.LowPart = created.dwLowDateTime; epoch.HighPart = created.dwHighDateTime;
        static DeleteHostAdmission admission;
        const DeleteWirePlan authorized{req->delete_epoch, req->delete_token, req->sources, req->expected_identities};
        if (!have_epoch || !admission.Consume(authorized, epoch.QuadPart)) {
            SendDeleteDone(req->id, E_ACCESSDENIED, false, L"Expired or repeated deletion authorization.", {});
            delete req;
            return;
        }
    }
    if (req->type == REQ_AUTHORIZED_RECYCLE) {
        const auto result = pulse::shell::RecycleAuthorized(req->sources,
            [id = req->id] { return g.cancel_id.load() == id; },
            [id = req->id](size_t done, size_t total, const std::wstring& path) {
                SendProgress(id, total ? static_cast<float>(done * 100.0 / total) : 0.0f, path, done, total);
            }, req->expected_identities);
        const auto error = result.identity_mismatch
            ? L"原操作产物已改变或无法核对身份，撤销已停止；不会删除当前同名项目。 | " + result.failed_path
            : FAILED(result.error) && !result.cancelled
                ? L"回收失败：" + pulse::format::FileOperationErrorText(result.error) + L" | " + result.failed_path
                : std::wstring{};
        if (g.cancel_id.load() == req->id) g.cancel_id.store(0);
        SendDeleteDone(req->id, result.error, result.cancelled, error, {}, result.sources, result.destinations, result.failed_path);
        delete req;
        return;
    }
    if (req->type == REQ_NEW_FOLDER || req->type == REQ_NEW_FILE) {
        std::vector<pulse::FileIdentity> produced_identities;
        HRESULT hr = req->sources.empty() ? E_INVALIDARG
                                          : ExecuteCreate(req->type, req->sources.front(), produced_identities);
        SendDone(req->id, hr, false, FAILED(hr) ? DescribeCreateError(hr) : L"", produced_identities);
        delete req;
        return;
    }

    HRESULT hr = S_OK;
    IFileOperation* op = nullptr;
    if (SUCCEEDED(hr)) {
        hr = CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_ALL,
                              IID_PPV_ARGS(&op));
    }

    ProgressSink sink(req->id, req->sources.size());
    DWORD sink_cookie = 0;
    if (SUCCEEDED(hr)) {
        hr = op->SetOperationFlags(pulse::shell::AuthorizedDeleteFlags(false));
        if (SUCCEEDED(hr)) hr = op->Advise(&sink, &sink_cookie);
    }

    std::vector<std::unique_ptr<ProgressSink>> root_sinks;
    bool setup_failed = false;
    bool produced_identity_mismatch = false;
    if (SUCCEEDED(hr)) {
        for (const auto& src : req->sources) {
            if (!req->expected_identities.empty()) {
                const size_t index = static_cast<size_t>(&src - req->sources.data());
                pulse::FileIdentity current;
                if (!pulse::ReadPathFileIdentity(src, current) || current != req->expected_identities[index]) {
                    hr = E_ACCESSDENIED;
                    produced_identity_mismatch = true;
                    setup_failed = true;
                    sink.NoteSetupFailure(src);
                    break;
                }
            }
            IShellItem* item = nullptr;
            HRESULT ihr = MakeItem(src, &item);
            if (SUCCEEDED(ihr)) {
                PWSTR resolved = nullptr;
                ihr = item->GetDisplayName(SIGDN_FILESYSPATH, &resolved);
                if (SUCCEEDED(ihr) && (!resolved || !pulse::shell::IsSameDeletionItem(src, resolved))) ihr = E_ACCESSDENIED;
                if (resolved) CoTaskMemFree(resolved);
                if (FAILED(ihr)) { item->Release(); item = nullptr; }
            }
            if (FAILED(ihr)) {
                hr = ihr;
                setup_failed = true;
                sink.NoteSetupFailure(src);
                break;
            }
            switch (req->type) {
            case REQ_AUTHORIZED_DELETE:
                root_sinks.push_back(std::make_unique<ProgressSink>(req->id, 1, src));
                ihr = op->DeleteItem(item, root_sinks.back().get());
                break;
            default: ihr = E_INVALIDARG; break;
            }
            item->Release();
            if (FAILED(ihr)) { hr = ihr; setup_failed = true; sink.NoteSetupFailure(src); break; }
        }
    }

    bool cancelled = false;
    if (SUCCEEDED(hr)) {
        for (size_t index = 0; index < req->expected_identities.size(); ++index) {
            pulse::FileIdentity current;
            if (!pulse::ReadPathFileIdentity(req->sources[index], current) || current != req->expected_identities[index]) {
                hr = E_ACCESSDENIED;
                produced_identity_mismatch = true;
                setup_failed = true;
                sink.NoteSetupFailure(req->sources[index]);
                break;
            }
        }
    }
    if (SUCCEEDED(hr)) {
        hr = op->PerformOperations();
        BOOL aborted = FALSE;
        const HRESULT aborted_hr = op->GetAnyOperationsAborted(&aborted);
        if (FAILED(aborted_hr) && SUCCEEDED(hr)) hr = aborted_hr;
        cancelled = (g.cancel_id.load() == req->id) || hr == HRESULT_FROM_WIN32(ERROR_CANCELLED);
        if (SUCCEEDED(hr) && FAILED(sink.item_failure())) hr = sink.item_failure();
        if (aborted && !cancelled && SUCCEEDED(hr)) {
            // User declined a system conflict/confirm dialog.
            cancelled = true;
        }
    }
    if (op) {
        if (sink_cookie) op->Unadvise(sink_cookie);
        op->Release();
    }
    std::wstring failed_path = sink.last_failed_item();
    for (const auto& root_sink : root_sinks) {
        if (FAILED(root_sink->item_failure())) {
            if (SUCCEEDED(hr)) hr = root_sink->item_failure();
            if (failed_path.empty()) failed_path = root_sink->last_failed_item();
        }
    }

    if (SUCCEEDED(hr) || cancelled) failed_path.clear();
    std::wstring error;
    if (FAILED(hr) &&
        GetEnvironmentVariableW(L"PULSE_DELETE_DIAGNOSTICS", nullptr, 0)) {
        wchar_t stage[160]{};
        swprintf_s(stage, L"delete diagnostics: setup_failed=%d hr=0x%08X item_hr=0x%08X", setup_failed,
            static_cast<unsigned>(hr), static_cast<unsigned>(sink.item_failure()));
        error = stage;
    }
    if (FAILED(hr) && !cancelled) {
        const auto reason = pulse::format::FileOperationErrorText(hr);
        error = error.empty() ? reason : reason + L"\n" + error;
        if (!failed_path.empty()) {
            if (!error.empty()) error += L" | ";
            error += failed_path;
        }
    }

    if (FAILED(hr) && !sink.delete_diagnostics().empty()) error += L"; " + sink.delete_diagnostics();
    if (produced_identity_mismatch)
        error = L"原操作产物已改变或无法核对身份，撤销已停止；不会删除当前同名项目。 | " + failed_path;
    if (g.cancel_id.load() == req->id) g.cancel_id.store(0);
    std::vector<std::wstring> deleted, recycled, destinations;
    for (const auto& root_sink : root_sinks) {
        deleted.insert(deleted.end(), root_sink->deleted_paths().begin(), root_sink->deleted_paths().end());
        recycled.insert(recycled.end(), root_sink->recycled_paths_.begin(), root_sink->recycled_paths_.end());
        destinations.insert(destinations.end(), root_sink->recycle_destinations_.begin(), root_sink->recycle_destinations_.end());
    }
    SendDeleteDone(req->id, hr, cancelled, error, deleted, recycled, destinations, failed_path);
    delete req;
}

// ---------------------------------------------------------------------------
// Pipe reader thread: parses frames, posts requests to the STA thread.
// ---------------------------------------------------------------------------
static DWORD WINAPI ReaderThreadImpl();

bool ReadRaw(void* out, DWORD size) {
    return PipeRead(g.pipe, static_cast<uint8_t*>(out), size);
}

DWORD WINAPI ReaderThread(LPVOID) {
    __try {
        return ReaderThreadImpl();
    } __except (pulse::crash::ReportFatal(GetExceptionInformation(), "shell-reader")) {
        TerminateProcess(GetCurrentProcess(), GetExceptionCode());
        return 0;
    }
}

DWORD WINAPI ReaderThreadImpl() {
    while (g.running.load()) {
        // (Re)connect — overlapped handle requires an explicit OVERLAPPED here.
        OVERLAPPED ol{};
        ol.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!ol.hEvent) break;
        BOOL connected = ConnectNamedPipe(g.pipe, &ol);
        if (!connected) {
            DWORD err = GetLastError();
            if (err == ERROR_IO_PENDING) {
                DWORD ignored = 0;
                connected = GetOverlappedResult(g.pipe, &ol, &ignored, TRUE);
            } else if (err == ERROR_PIPE_CONNECTED) {
                connected = TRUE;
            }
        }
        CloseHandle(ol.hEvent);
        if (!connected) {
            if (!g.running.load()) break;
            Sleep(200);
            continue;
        }
        ULONG client_pid = 0;
        if (!GetNamedPipeClientProcessId(g.pipe, &client_pid) || client_pid != g.client_pid) {
            DisconnectNamedPipe(g.pipe);
            continue;
        }
        // Frame loop.
        for (;;) {
            MsgHeader h{};
            if (!ReadRaw(&h, sizeof(h))) break;
            if (h.magic != kMagic || h.payload_size > kMaxPayload) break;
            std::vector<uint8_t> payload(h.payload_size);
            if (h.payload_size && !ReadRaw(payload.data(), h.payload_size)) break;

            switch (h.type) {
            case REQ_PING: {
                SendMsg(RSP_PONG, h.request_id, {});
                break;
            }
            case REQ_CANCEL: {
                if (g_launch_processes && g_launch_processes->Cancel(h.request_id)) break;
                if (g_interaction_processes && g_interaction_processes->Cancel(h.request_id)) break;
                g.cancel_id.store(h.request_id);
                break;
            }
            case REQ_SHUTDOWN: {
                PostMessageW(g.hwnd_msg, WM_QUIT_HOST, 0, 0);
                break;
            }
            case REQ_CTX_QUERY: {
                if (g_context_processes) g_context_processes->Query(h.request_id, std::move(payload));
                else StartCtxSession(h.request_id, payload.data(), payload.size());
                break;
            }
            case REQ_SHELL_LAUNCH: {
                if (g_launch_processes) {
                    pulse::ipc::ShellLaunchRequest request;
                    const bool interaction = pulse::ipc::ReadShellLaunch(payload.data(), payload.size(), request) &&
                        (request.kind == pulse::ipc::ShellLaunchKind::PickFile ||
                         request.kind == pulse::ipc::ShellLaunchKind::PickFolder ||
                         request.kind == pulse::ipc::ShellLaunchKind::ElevatedWait);
                    // Long-lived Photos/property sessions cannot starve modal selection.
                    auto& pool = interaction ? g_interaction_processes : g_launch_processes;
                    pool->Submit(h.request_id, std::move(payload));
                }
                else {
                    PayloadWriter response;
                    response.PutU32(static_cast<uint32_t>(E_ACCESSDENIED));
                    response.PutU32(0);
                    response.PutString(L"This worker cannot broker Shell launches; no launch occurred.");
                    SendMsg(RSP_SHELL_LAUNCH_DONE, h.request_id, response.data());
                }
                break;
            }
            case REQ_CTX_INVOKE: {
                PayloadReader r(payload.data(), payload.size());
                ContextInvokePayload invocation;
                if (ReadContextInvokePayload(r, invocation)) {
                    if (g_context_processes) {
                        g_context_processes->Invoke(invocation.session, h.request_id, std::move(payload));
                        break;
                    }
                    auto* inv = new CtxInvokeMsg{ h.request_id, invocation.item, std::move(invocation.verb),
                                                  std::move(invocation.text) };
                    PostCtxMessage(invocation.session, WM_CTX_INVOKE, 0,
                                   reinterpret_cast<LPARAM>(inv));
                } else {
                    SendDone(h.request_id, E_INVALIDARG, false, L"malformed ctx invoke");
                }
                break;
            }
            case REQ_CTX_CLOSE: {
                PayloadReader r(payload.data(), payload.size());
                uint32_t session = 0;
                if (r.GetU32(session)) {
                    if (g_context_processes) g_context_processes->Close(session);
                    else PostCtxMessage(session, WM_CTX_CLOSE, 0, 0);
                }
                break;
            }
            case REQ_DELETE_RECYCLE:
            case REQ_REALDELETE:
            case REQ_AUTHORIZED_RECYCLE:
            case REQ_AUTHORIZED_DELETE:
            case REQ_NEW_FOLDER:
            case REQ_NEW_FILE: {
                if (g_context_worker) {
                    SendDone(h.request_id, E_ACCESSDENIED, false, L"Context workers cannot perform file operations");
                    break;
                }
                auto* req = new Request();
                req->type = h.type;
                req->id = h.request_id;
                PayloadReader r(payload.data(), payload.size());
                bool ok = true;
                if (req->type == REQ_NEW_FOLDER || req->type == REQ_NEW_FILE) {
                    std::wstring path;
                    ok = r.GetString(path);
                    req->sources.push_back(std::move(path));
                } else {
                    if (IsAuthorizedDeletion(req->type)) {
                        DeleteWirePlan plan;
                        const bool recycle = req->type == REQ_AUTHORIZED_RECYCLE;
                        ok = ReadDeleteWirePlan(r, plan, !recycle);
                        uint64_t owner = 0;
                        if (recycle) {
                            ok = ok && r.GetU64(owner) && r.remaining() == 0;
                            req->owner = reinterpret_cast<HWND>(static_cast<uintptr_t>(owner));
                        }
                        req->delete_epoch = plan.host_epoch; req->delete_token = plan.token;
                        req->sources = std::move(plan.paths);
                        req->expected_identities = std::move(plan.expected_identities);
                    } else {
                        ok = r.GetStringArray(req->sources);
                    }
                }
                if (!ok || r.remaining() != 0) {
                    SendDone(req->id, E_INVALIDARG, false, L"malformed request payload");
                    delete req;
                    break;
                }
                if (!PostMessageW(g.hwnd_msg, WM_EXEC_REQUEST, 0,
                                  reinterpret_cast<LPARAM>(req))) {
                    SendDone(req->id, HRESULT_FROM_WIN32(GetLastError()), false,
                             L"host message queue unavailable");
                    delete req;
                }
                break;
            }
            default:
                break;
            }
        }
        // Client disconnected: clean up and listen again.
        DisconnectNamedPipe(g.pipe);
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Context-menu sessions (Explorer verbs). One STA thread per session so a hung
// third-party extension only wedges its own thread: file operations on the
// main STA and the next right-click are unaffected (优化.md §7.2).
// ---------------------------------------------------------------------------
constexpr UINT kCtxIdFirst = 1;
constexpr UINT kCtxIdLast = 0x7FFF;
constexpr DWORD kCtxSessionExpireMs = pulse::ipc::kContextMenuSessionLifetimeMs;

struct CtxSessionData {
    uint32_t session_id = 0;   // == REQ_CTX_QUERY request id
    HWND owner = nullptr;
    bool extended = false;
    bool background = false;
    std::vector<std::wstring> paths;
    std::vector<std::wstring> disabled_clsids;
};

struct CtxSlot {
    DWORD thread_id = 0;       // 0 until the session thread has a message queue
    bool close_requested = false;
};

std::mutex g_ctx_mutex;
std::map<uint32_t, CtxSlot> g_ctx_sessions;

using pulse::shell::CtxItemOut;

void SendCtxItems(uint32_t session_id, const std::vector<CtxItemOut>& items,
                  uint32_t flags = 0,
                  const std::vector<std::wstring>& slow_clsids = {}) {
    PayloadWriter w;
    w.PutU32(session_id);
    w.PutU32(flags);
    w.PutU32((uint32_t)items.size());
    for (const auto& it : items) {
        w.PutU32(it.id);
        uint32_t item_flags = 0;
        if (it.enabled) item_flags |= CTX_ITEM_ENABLED;
        if (it.separator_after) item_flags |= CTX_ITEM_SEPARATOR_AFTER;
        if (it.has_children) item_flags |= CTX_ITEM_HAS_CHILDREN;
        if (it.child) item_flags |= CTX_ITEM_CHILD;
        w.PutU32(item_flags);
        w.PutString(it.verb);
        w.PutString(it.text);
        w.PutString(it.clsid);
        w.PutString(it.handler);
    }
    w.PutStringArray(slow_clsids);
    SendMsg(RSP_CTX_ITEMS, session_id, w.data());
}

UINT QueryContextMenuFlags(const CtxSessionData& d) {
    UINT flags = CMF_NORMAL;
    if (d.extended) flags |= CMF_EXTENDEDVERBS;
    if (!d.background) {
        bool all_dirs = !d.paths.empty();
        for (const auto& p : d.paths) {
            const std::wstring path = ToParsingPath(p);
            const DWORD attr = GetFileAttributesW(path.c_str());
            if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
                all_dirs = false;
                break;
            }
        }
        if (all_dirs) flags |= CMF_EXPLORE;
    }
    return flags;
}

// Walks the populated HMENU via the same nested-flyout rules as per-handler
// collection so fallback and worker paths cannot drift.
void CollectCtxItems(IContextMenu* menu, IContextMenu2* menu2, HMENU hmenu,
                     bool background, std::vector<CtxItemOut>& out) {
    pulse::shell::CtxHandlerSlot slot;
    slot.menu = menu;
    slot.menu2 = menu2;
    slot.hmenu = hmenu;
    slot.id_first = kCtxIdFirst;
    slot.id_last = kCtxIdLast;
    if (menu) menu->QueryInterface(IID_PPV_ARGS(&slot.menu3));
    pulse::shell::CollectHandlerItems(slot, background, out);
    if (slot.menu3) slot.menu3->Release();
    slot.menu = nullptr;
    slot.menu2 = nullptr;
    slot.menu3 = nullptr;
    slot.hmenu = nullptr;
}

HRESULT BuildCtxMenu(const CtxSessionData& d, IContextMenu** out_menu, HMENU* out_hmenu,
                     std::vector<CtxItemOut>& items) {
    *out_menu = nullptr;
    *out_hmenu = nullptr;
    if (d.paths.empty()) return E_INVALIDARG;

    IContextMenu* menu = nullptr;
    HRESULT hr = S_OK;
    if (d.background) {
        IShellItem* folder = nullptr;
        hr = MakeItem(d.paths.front(), &folder);
        IShellFolder* sf = nullptr;
        if (SUCCEEDED(hr))
            hr = folder->BindToHandler(nullptr, BHID_SFObject, IID_PPV_ARGS(&sf));
        if (SUCCEEDED(hr))
            hr = sf->CreateViewObject(d.owner, IID_IContextMenu,
                                      reinterpret_cast<void**>(&menu));
        if (sf) sf->Release();
        if (folder) folder->Release();
    } else {
        std::vector<PIDLIST_ABSOLUTE> pidls;
        for (const auto& p : d.paths) {
            PIDLIST_ABSOLUTE pidl = nullptr;
            if (SUCCEEDED(SHParseDisplayName(ToParsingPath(p).c_str(), nullptr, &pidl, 0, nullptr)))
                pidls.push_back(pidl);
        }
        if (pidls.empty()) return E_INVALIDARG;
        IShellItemArray* array = nullptr;
        hr = SHCreateShellItemArrayFromIDLists((UINT)pidls.size(),
            const_cast<PCIDLIST_ABSOLUTE_ARRAY>(pidls.data()), &array);
        if (SUCCEEDED(hr)) {
            hr = array->BindToHandler(nullptr, BHID_SFUIObject, IID_PPV_ARGS(&menu));
            array->Release();
        }
        for (auto pidl : pidls) CoTaskMemFree(pidl);
    }
    if (FAILED(hr) || !menu) return FAILED(hr) ? hr : E_FAIL;

    HMENU hmenu = CreatePopupMenu();
    UINT flags = QueryContextMenuFlags(d);
    hr = menu->QueryContextMenu(hmenu, 0, kCtxIdFirst, kCtxIdLast, flags);
    if (FAILED(hr)) {
        DestroyMenu(hmenu);
        menu->Release();
        return hr;
    }
    IContextMenu2* menu2 = nullptr;
    menu->QueryInterface(IID_PPV_ARGS(&menu2));
    CollectCtxItems(menu, menu2, hmenu, d.background, items);
    if (menu2) menu2->Release();
    *out_menu = menu;
    *out_hmenu = hmenu;
    return S_OK;
}

void CtxInvoke(const CtxSessionData& d, IContextMenu* menu, UINT id_first,
               uint32_t invoke_req_id, uint32_t item_id) {
    if (!menu || item_id < id_first) {
        SendDone(invoke_req_id, E_INVALIDARG, false, L"context menu invoke failed");
        return;
    }
    std::wstring dir = ToParsingPath(d.paths.front());
    if (!d.background) {
        const auto slash = dir.find_last_of(L'\\');
        if (slash != std::wstring::npos && slash > 2) dir.resize(slash);
    }
    CMINVOKECOMMANDINFOEX info{};
    info.cbSize = sizeof(info);
    info.fMask = CMIC_MASK_UNICODE;
    info.hwnd = d.owner;
    info.lpVerb = MAKEINTRESOURCEA(item_id - id_first);
    info.lpVerbW = MAKEINTRESOURCEW(item_id - id_first);
    info.lpDirectoryW = dir.c_str();
    info.nShow = SW_SHOWNORMAL;
    const HRESULT hr = menu->InvokeCommand(reinterpret_cast<CMINVOKECOMMANDINFO*>(&info));
    SendDone(invoke_req_id, hr, false,
             FAILED(hr) ? L"context menu invoke failed" : L"");
}

bool SessionCloseRequested(uint32_t sid) {
    std::lock_guard<std::mutex> lock(g_ctx_mutex);
    auto it = g_ctx_sessions.find(sid);
    return it == g_ctx_sessions.end() || it->second.close_requested;
}

struct HandlerWorker : pulse::shell::CtxWorkerHandles {
    ~HandlerWorker() { Stop(); }
    pulse::shell::CtxHandlerDesc desc;
    std::vector<std::wstring> paths;
    bool background = false;
    HWND owner = nullptr;
    UINT id_first = 0;
    UINT id_last = 0;
    UINT qcm_flags = 0;
    pulse::shell::CtxHandlerSlot slot;
    std::vector<CtxItemOut> items;
    DWORD thread_id = 0;
    uint32_t elapsed_ms = 0;
};

DWORD HandlerWorkerThreadImpl(LPVOID param) {
    auto* w = static_cast<HandlerWorker*>(param);
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE))) {
        if (w->done_event) SetEvent(w->done_event);
        return 0;
    }
    MSG msg;
    PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
    w->thread_id = GetCurrentThreadId();

    const ULONGLONG t0 = GetTickCount64();
    pulse::shell::CtxBind bind;
#ifdef PULSE_CONTEXT_PROCESS_TESTING
    const bool fixture = w->paths.size() == 1 && w->paths.front() == L":pulse-context-process-test:";
    if (fixture && w->desc.name == L"fixture-hang") Sleep(INFINITE);
    if (fixture && w->desc.name == L"fixture-slow") Sleep(400);
    if (fixture) {
        CtxItemOut item;
        item.id = w->id_first; item.verb = L"fixture-ready";
        item.text = std::to_wstring(GetCurrentProcessId());
        w->items.push_back(item);
        item.id++; item.verb = L"fixture-invoke-hang";
        w->items.push_back(item);
        item.id++; item.verb = L"fixture-modeless";
        w->items.push_back(item);
        item.id++; item.verb = L"fixture-launch-child";
        w->items.push_back(item);
    } else
#endif
    if (pulse::shell::BindCtxSelection(w->paths, w->background, bind)) {
        const HRESULT hr = pulse::shell::QueryOneHandler(
            w->desc, bind, w->owner, w->id_first, w->id_last, w->qcm_flags, w->slot);
        if (SUCCEEDED(hr) && w->slot.menu)
            pulse::shell::CollectHandlerItems(w->slot, w->background, w->items);
        else
            pulse::shell::ReleaseHandlerSlot(w->slot);
        if (pulse::ipc::IsNativeNewMenuHandler(w->desc.clsid_text)) {
            wchar_t diagnostic[160]{};
            swprintf_s(diagnostic, L"Native New query hr=0x%08X rows=%zu ids=%u..%u",
                static_cast<unsigned>(hr), w->items.size(), w->id_first, w->id_last);
            HostLog(diagnostic);
        }
    }
    w->elapsed_ms = static_cast<uint32_t>(GetTickCount64() - t0);
    if (w->done_event) SetEvent(w->done_event);

    bool invocation_finished = false;
    ULONGLONG invoke_finished_at = 0;
    for (;;) {
        HANDLE waits[1] = { w->exit_event };
        const DWORD n = w->exit_event && !invocation_finished ? 1 : 0;
        MsgWaitForMultipleObjects(n, n ? waits : nullptr, FALSE,
            invocation_finished ? 100 : INFINITE, QS_ALLINPUT);
        const bool exiting = w->exit_event &&
            WaitForSingleObject(w->exit_event, 0) == WAIT_OBJECT_0;
        bool invoked = false;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_CTX_INVOKE) {
                std::unique_ptr<CtxInvokeMsg> inv(
                    reinterpret_cast<CtxInvokeMsg*>(msg.lParam));
                CtxSessionData d;
                d.owner = w->owner;
                d.paths = w->paths;
                d.background = w->background;
#ifdef PULSE_CONTEXT_PROCESS_TESTING
                if (fixture && pulse::shell::FindItemId(w->items, inv->item_id,
                        inv->verb, inv->text, w->id_first, w->id_last)) {
                    std::wstring result;
                    HRESULT fixture_result = S_OK;
                    if (inv->verb == L"fixture-invoke-hang") {
                        const auto name = L"Local\\Pulse.ContextInvokeEntered." + std::to_wstring(GetCurrentProcessId());
                        HANDLE entered = OpenEventW(EVENT_MODIFY_STATE, FALSE, name.c_str());
                        if (entered) { SetEvent(entered); CloseHandle(entered); }
                        Sleep(INFINITE);
                    }
                    if (inv->verb == L"fixture-modeless") {
                        const auto title = L"Pulse Context Modeless " + std::to_wstring(GetCurrentProcessId());
                        CreateWindowExW(WS_EX_NOACTIVATE, L"STATIC", title.c_str(), WS_POPUP | WS_VISIBLE,
                            -30000, -30000, 100, 100, w->owner, nullptr, GetModuleHandleW(nullptr), nullptr);
                    }
                    if (inv->verb == L"fixture-launch-child") {
                        wchar_t executable[32768]{};
                        GetModuleFileNameW(nullptr, executable, static_cast<DWORD>(std::size(executable)));
                        auto command = L"\"" + std::wstring(executable) + L"\" --context-test-child";
                        STARTUPINFOW startup{sizeof(startup)};
                        PROCESS_INFORMATION child{};
                        if (CreateProcessW(executable, command.data(), nullptr, nullptr, FALSE,
                                CREATE_NO_WINDOW, nullptr, nullptr, &startup, &child)) {
                            result = std::to_wstring(child.dwProcessId);
                            CloseHandle(child.hThread); CloseHandle(child.hProcess);
                        } else fixture_result = HRESULT_FROM_WIN32(GetLastError());
                    }
                    SendDone(inv->invoke_req_id, fixture_result, false, result);
                } else
#endif
                if (w->slot.menu && pulse::shell::FindItemId(w->items, inv->item_id,
                        inv->verb, inv->text, w->id_first, w->id_last))
                    CtxInvoke(d, w->slot.menu, w->id_first, inv->invoke_req_id, inv->item_id);
                else
                    SendDone(inv->invoke_req_id, E_INVALIDARG, false,
                             L"context menu invoke failed");
                invoked = true;
            } else if (msg.message == WM_CTX_CLOSE) {
                // coordinator uses exit_event; ignore stray close
            } else {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }
        if (invoked) {
            invocation_finished = true;
            invoke_finished_at = GetTickCount64();
        }
        if (invocation_finished) {
            if (GetTickCount64() - invoke_finished_at >= 750 &&
                !pulse::shell::ContextProcessHasVisibleWindows(GetCurrentProcessId())) break;
        } else if (exiting) break;
    }
    pulse::shell::ReleaseHandlerSlot(w->slot);
    CoUninitialize();
    return 0;
}

DWORD WINAPI HandlerWorkerThread(LPVOID param) {
    auto* w = static_cast<HandlerWorker*>(param);
    __try {
        return HandlerWorkerThreadImpl(param);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (w && w->done_event) SetEvent(w->done_event);
        return 0;
    }
}

void JoinHandlerWorkers(std::vector<std::unique_ptr<HandlerWorker>>& workers) {
    for (auto& w : workers)
        if (w && w->exit_event) SetEvent(w->exit_event);
    for (auto& w : workers) {
        if (!w) continue;
        w->Stop();
    }
    workers.clear();
}

DWORD WINAPI CtxSessionThreadImpl(LPVOID param) {
    std::unique_ptr<CtxSessionData> data(static_cast<CtxSessionData*>(param));
    const uint32_t sid = data->session_id;
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE))) {
        SendCtxItems(sid, {});
        std::lock_guard<std::mutex> lock(g_ctx_mutex);
        g_ctx_sessions.erase(sid);
        return 0;
    }
    MSG msg;
    PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
    bool closed_early = false;
    {
        std::lock_guard<std::mutex> lock(g_ctx_mutex);
        auto it = g_ctx_sessions.find(sid);
        if (it == g_ctx_sessions.end() || it->second.close_requested) {
            closed_early = true;
            g_ctx_sessions.erase(sid);
        } else {
            it->second.thread_id = GetCurrentThreadId();
        }
    }
    if (closed_early) {
        CoUninitialize();
        return 0;
    }

    constexpr UINT kIdsPerHandler = 256;
    constexpr DWORD kFastBudgetMs = 80;
    const ULONGLONG started = GetTickCount64();
    UINT qcm_flags = QueryContextMenuFlags(*data);

    std::vector<std::unique_ptr<HandlerWorker>> workers;
    std::vector<CtxItemOut> items;
    IContextMenu* fallback_menu = nullptr;
    HMENU fallback_hmenu = nullptr;
    std::unique_ptr<CtxInvokeMsg> pending_invoke;

    auto pump_session_messages = [&] {
        MSG m{};
        while (PeekMessageW(&m, nullptr, WM_CTX_CLOSE, WM_CTX_CLOSE, PM_REMOVE)) {
            std::lock_guard<std::mutex> lock(g_ctx_mutex);
            auto it = g_ctx_sessions.find(sid);
            if (it != g_ctx_sessions.end()) it->second.close_requested = true;
        }
        while (PeekMessageW(&m, nullptr, WM_CTX_INVOKE, WM_CTX_INVOKE, PM_REMOVE))
            pending_invoke.reset(reinterpret_cast<CtxInvokeMsg*>(m.lParam));
    };

    auto handlers = pulse::shell::EnumerateCtxHandlers(
        data->background, data->paths.front(), data->disabled_clsids);
#ifdef PULSE_CONTEXT_PROCESS_TESTING
    if (data->paths.size() == 1 && data->paths.front() == L":pulse-context-process-test:") {
        handlers = {{ {}, L"fixture-ready", L"fixture-ready" },
                    { {}, L"fixture-hang", L"fixture-hang" },
                    { {}, L"fixture-slow", L"fixture-slow" }};
    }
#endif
    if (handlers.size() > MAXIMUM_WAIT_OBJECTS)
        handlers.resize(MAXIMUM_WAIT_OBJECTS);

    UINT next_id = kCtxIdFirst;
    for (const auto& handler : handlers) {
        if (next_id + kIdsPerHandler > kCtxIdLast) break;
        auto worker = std::make_unique<HandlerWorker>();
        worker->desc = handler;
        worker->paths = data->paths;
        worker->background = data->background;
        worker->owner = data->owner;
        worker->id_first = next_id;
        worker->id_last = next_id + kIdsPerHandler - 1;
        worker->qcm_flags = qcm_flags;
        next_id += kIdsPerHandler;
        if (!worker->Start(HandlerWorkerThread, worker.get())) continue;
        workers.push_back(std::move(worker));
    }

    auto worker_done = [](const HandlerWorker& w) {
        return w.done_event && WaitForSingleObject(w.done_event, 0) == WAIT_OBJECT_0;
    };
    auto all_workers_done = [&] {
        if (workers.empty()) return true;
        for (const auto& w : workers)
            if (w && !worker_done(*w)) return false;
        return true;
    };
    auto collect_items = [&] {
        std::vector<CtxItemOut> out;
        for (const auto& w : workers) {
            if (!w || !worker_done(*w)) continue;
            out.insert(out.end(), w->items.begin(), w->items.end());
        }
        return out;
    };
    auto collect_slow = [&] {
        std::vector<std::wstring> slow;
        for (const auto& w : workers) {
            if (!w || !worker_done(*w) || w->elapsed_ms < 1000) continue;
            if (!w->desc.clsid_text.empty()) slow.push_back(w->desc.clsid_text);
        }
        return slow;
    };

    if (!workers.empty()) {
        bool sent_partial = false;
        size_t published_workers = 0;
        while (!all_workers_done() && !SessionCloseRequested(sid)) {
            pump_session_messages();
            if (SessionCloseRequested(sid) || pending_invoke) break;
            const ULONGLONG elapsed = GetTickCount64() - started;
            const size_t completed_workers = static_cast<size_t>(std::count_if(workers.begin(), workers.end(),
                [&](const auto& worker) { return worker && worker_done(*worker); }));
            if (elapsed >= kFastBudgetMs && (!sent_partial || completed_workers != published_workers)) {
                items = collect_items();
                SendCtxItems(sid, items, CTX_ITEMS_PARTIAL);
                sent_partial = true;
                published_workers = completed_workers;
            }
            std::vector<HANDLE> waits;
            waits.reserve(workers.size());
            for (const auto& w : workers)
                if (w && w->done_event && !worker_done(*w)) waits.push_back(w->done_event);
            if (waits.empty()) break;
            DWORD timeout = 200;
            if (!sent_partial) {
                const ULONGLONG left = elapsed >= kFastBudgetMs ? 1 : (kFastBudgetMs - elapsed);
                timeout = left > 0xFFFFFFFFULL ? 200 : static_cast<DWORD>(left);
                if (timeout == 0) timeout = 1;
            }
            MsgWaitForMultipleObjects(static_cast<DWORD>(waits.size()), waits.data(),
                                      FALSE, timeout, QS_ALLINPUT);
        }
        pump_session_messages();
        items = collect_items();
    } else {
        BuildCtxMenu(*data, &fallback_menu, &fallback_hmenu, items);
    }

    const uint32_t elapsed = static_cast<uint32_t>(GetTickCount64() - started);
    wchar_t timing[160];
    swprintf_s(timing, L"QueryContextMenu %ums items=%zu handlers=%zu background=%d",
               elapsed, items.size(), workers.size(), data->background ? 1 : 0);
    HostLog(timing);
    if (elapsed >= 500) {
        wchar_t slow[192];
        swprintf_s(slow, L"slow handler %ums path=%ls", elapsed,
                   data->paths.empty() ? L"" : data->paths.front().c_str());
        HostLog(slow);
    }
    SendCtxItems(sid, items, 0, collect_slow());

    auto dispatch_invoke = [&](std::unique_ptr<CtxInvokeMsg> inv) {
        if (!inv) return;
        uint32_t live = pulse::shell::FindItemId(
            items, inv->item_id, inv->verb, inv->text);
        if (!workers.empty()) {
            HandlerWorker* target = nullptr;
            for (auto& w : workers) {
                if (w && live >= w->id_first && live <= w->id_last) {
                    target = w.get();
                    break;
                }
            }
            if (target && target->thread_id && live != 0) {
                inv->item_id = live;
                CtxInvokeMsg* raw = inv.release();
                if (!PostThreadMessageW(target->thread_id, WM_CTX_INVOKE, 0,
                                        reinterpret_cast<LPARAM>(raw))) {
                    std::unique_ptr<CtxInvokeMsg> back(raw);
                    SendDone(back->invoke_req_id, HRESULT_FROM_WIN32(GetLastError()),
                             false, L"context menu session not ready");
                }
            } else {
                SendDone(inv->invoke_req_id, E_INVALIDARG, false,
                         L"context menu invoke failed");
            }
            return;
        }
        if (fallback_menu && live != 0)
            CtxInvoke(*data, fallback_menu, kCtxIdFirst, inv->invoke_req_id, live);
        else
            SendDone(inv->invoke_req_id, E_INVALIDARG, false,
                     L"context menu invoke failed");
    };

    bool done = workers.empty() && fallback_menu == nullptr;
    if (pending_invoke) {
        dispatch_invoke(std::move(pending_invoke));
        done = true;
    }
    const ULONGLONG deadline = GetTickCount64() + kCtxSessionExpireMs;
    while (!done) {
        const ULONGLONG now = GetTickCount64();
        if (now >= deadline) break;
        const DWORD wait = MsgWaitForMultipleObjects(
            0, nullptr, FALSE, (DWORD)(deadline - now), QS_ALLINPUT);
        if (wait == WAIT_TIMEOUT) break;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_CTX_INVOKE) {
                dispatch_invoke(std::unique_ptr<CtxInvokeMsg>(
                    reinterpret_cast<CtxInvokeMsg*>(msg.lParam)));
                done = true;
                break;
            }
            if (msg.message == WM_CTX_CLOSE) {
                done = true;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    {
        std::lock_guard<std::mutex> lock(g_ctx_mutex);
        g_ctx_sessions.erase(sid);
    }
    JoinHandlerWorkers(workers);
    if (fallback_hmenu) DestroyMenu(fallback_hmenu);
    if (fallback_menu) fallback_menu->Release();
    CoUninitialize();
    return 0;
}

int CtxCrashFilter(EXCEPTION_POINTERS* ep) {
    wchar_t mod[MAX_PATH]{};
    HMODULE hm = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)ep->ExceptionRecord->ExceptionAddress, &hm);
    if (hm) GetModuleFileNameW(hm, mod, ARRAYSIZE(mod));
    wchar_t buf[640];
    swprintf_s(buf, L"CtxSessionThread CRASH code=%08X addr=%p mod=%s",
               ep->ExceptionRecord->ExceptionCode,
               ep->ExceptionRecord->ExceptionAddress, hm ? mod : L"?");
    HostLog(buf);
    pulse::crash::ReportRecoverable(ep, "shell-context-menu");
    return EXCEPTION_EXECUTE_HANDLER;
}

// Separate function: __except bodies cannot contain objects with unwinding.
void CtxCrashCleanup(uint32_t sid) {
    SendCtxItems(sid, {});
    std::lock_guard<std::mutex> lock(g_ctx_mutex);
    g_ctx_sessions.erase(sid);
}

DWORD WINAPI CtxSessionThread(LPVOID param) {
    // Grab the session id up front: on a crash we still answer the query with
    // an empty item list so the UI process is not left waiting for RSP_CTX_ITEMS.
    const uint32_t sid = static_cast<CtxSessionData*>(param)->session_id;
    __try {
        return CtxSessionThreadImpl(param);
    } __except (CtxCrashFilter(GetExceptionInformation())) {
        CtxCrashCleanup(sid);
        return 0;
    }
}

void StartCtxSession(uint32_t session_id, const uint8_t* payload, size_t size) {
    auto data = std::make_unique<CtxSessionData>();
    data->session_id = session_id;
    PayloadReader r(payload, size);
    uint32_t owner = 0, flags = 0;
    if (!r.GetU32(owner) || !r.GetU32(flags) || !r.GetStringArray(data->paths) ||
        data->paths.empty()) {
        SendCtxItems(session_id, {});
        return;
    }
    if (!r.TryStringArray(data->disabled_clsids) || r.remaining() != 0) {
        SendCtxItems(session_id, {});
        return;
    }
    data->owner = reinterpret_cast<HWND>(static_cast<uintptr_t>(owner));
    data->extended = (flags & CTXF_EXTENDED) != 0;
    data->background = (flags & CTXF_BACKGROUND) != 0;
    {
        std::lock_guard<std::mutex> lock(g_ctx_mutex);
        g_ctx_sessions[session_id] = CtxSlot{};
    }
    HANDLE thread = CreateThread(nullptr, 0, CtxSessionThread, data.get(), 0, nullptr);
    if (!thread) {
        std::lock_guard<std::mutex> lock(g_ctx_mutex);
        g_ctx_sessions.erase(session_id);
        SendCtxItems(session_id, {});
        return;
    }
    data.release(); // owned by the thread now
    CloseHandle(thread);
}

// Route invoke/close from the reader thread to the owning session thread.
void PostCtxMessage(uint32_t session_id, UINT message, WPARAM wParam, LPARAM lParam) {
    std::lock_guard<std::mutex> lock(g_ctx_mutex);
    auto it = g_ctx_sessions.find(session_id);
    if (it == g_ctx_sessions.end()) {
        if (message == WM_CTX_INVOKE) {
            std::unique_ptr<CtxInvokeMsg> inv(reinterpret_cast<CtxInvokeMsg*>(lParam));
            SendDone(inv->invoke_req_id, HRESULT_FROM_WIN32(ERROR_NOT_FOUND), false,
                     L"context menu session expired");
        }
        return;
    }
    if (it->second.thread_id == 0) {
        it->second.close_requested = true;
        if (message == WM_CTX_INVOKE) {
            std::unique_ptr<CtxInvokeMsg> inv(reinterpret_cast<CtxInvokeMsg*>(lParam));
            SendDone(inv->invoke_req_id, HRESULT_FROM_WIN32(ERROR_NOT_READY), false,
                     L"context menu session not ready");
        }
        return;
    }
    if (!PostThreadMessageW(it->second.thread_id, message, wParam, lParam) &&
        message == WM_CTX_INVOKE) {
        std::unique_ptr<CtxInvokeMsg> inv(reinterpret_cast<CtxInvokeMsg*>(lParam));
        SendDone(inv->invoke_req_id, HRESULT_FROM_WIN32(GetLastError()), false,
                 L"context menu session not ready");
    }
}

DWORD WINAPI ParentWatchdog(LPVOID param) {
    DWORD pid = (DWORD)(uintptr_t)param;
    HANDLE h = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return 0; // parent already gone or not ours; keep serving standalone

    // A recycled PID must not become an unrelated watchdog target. The real
    // UI necessarily started before this host, so a candidate created later
    // cannot be its parent even if the numeric PID matches.
    FILETIME parent_created{}, parent_exit{}, parent_kernel{}, parent_user{};
    FILETIME host_created{}, host_exit{}, host_kernel{}, host_user{};
    if (!GetProcessTimes(h, &parent_created, &parent_exit, &parent_kernel, &parent_user) ||
        !GetProcessTimes(GetCurrentProcess(), &host_created, &host_exit,
                         &host_kernel, &host_user) ||
        CompareFileTime(&parent_created, &host_created) > 0) {
        CloseHandle(h);
        return 0;
    }
    WaitForSingleObject(h, INFINITE);
    CloseHandle(h);
    if (g.running.load()) ExitProcess(0);
    return 0;
}

LRESULT CALLBACK HostWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_EXEC_REQUEST) {
        ExecuteRequest(reinterpret_cast<Request*>(lParam));
        return 0;
    }
    if (msg == WM_QUIT_HOST) {
        g.running.store(false);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int) {
#ifdef PULSE_SHELL_LAUNCH_TESTING
    if (__argc == 3 && wcscmp(__wargv[1], L"--launch-test-command") == 0) {
        return pulse::shell::RunShellLaunchTestCommand(__wargv[2]);
    }
    if (__argc == 2 && wcscmp(__wargv[1], L"--launch-test-child") == 0) {
        Sleep(INFINITE);
        return 0;
    }
#endif
#ifdef PULSE_CONTEXT_PROCESS_TESTING
    if (__argc == 2 && wcscmp(__wargv[1], L"--context-test-child") == 0) {
        Sleep(INFINITE);
        return 0;
    }
#endif
    pulse::crash::Initialize({pulse::crash::ProcessRole::Shell, false, {}});
#ifdef PULSE_OPERATION_ELEVATION_HOST
    if (__argc > 1 && wcscmp(__wargv[1], L"--file-operation-worker") == 0)
        return pulse::ops::OperationElevationAdapter::RunWorker(__argc, __wargv);
#endif
    // IFileOperation creates its conflict/confirmation UI in this process.
    // Declare PMv2 before COM or any HWND exists so Windows does not bitmap-scale
    // those dialogs on high-DPI displays.
    pulse::compat::EnableDpiAwareness();

    if (__argc == 4 && wcscmp(__wargv[1], L"--query-worker") == 0) {
        const DWORD parent = static_cast<DWORD>(_wcstoui64(__wargv[2], nullptr, 10));
        const uint32_t session = static_cast<uint32_t>(_wcstoui64(__wargv[3], nullptr, 10));
        return parent && session ? pulse::shell::RunShellQueryWorker(parent, session) : 1;
    }

    if (__argc == 4 && wcscmp(__wargv[1], L"--launch-worker") == 0) {
        const DWORD parent = static_cast<DWORD>(_wcstoui64(__wargv[2], nullptr, 10));
        const uint32_t request = static_cast<uint32_t>(_wcstoui64(__wargv[3], nullptr, 10));
        return parent && request ? pulse::shell::RunShellLaunchWorker(parent, request) : 1;
    }

    DWORD ui_pid = GetCurrentProcessId();
    uint32_t worker_session = 0;
    if (__argc == 4 && wcscmp(__wargv[1], L"--context-worker") == 0) {
        g_context_worker = true;
        ui_pid = static_cast<DWORD>(_wcstoui64(__wargv[2], nullptr, 10));
        worker_session = static_cast<uint32_t>(_wcstoui64(__wargv[3], nullptr, 10));
        if (!ui_pid || !worker_session) return 1;
    } else if (__argc > 1) {
        ui_pid = (DWORD)_wtoi(__wargv[1]);
        if (ui_pid == 0) ui_pid = GetCurrentProcessId();
    }

    g.client_pid = ui_pid;
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (FAILED(hr)) return 1;

    std::wstring pipe_name = g_context_worker
        ? pulse::shell::ContextWorkerPipeName(ui_pid, worker_session) : PipeNameFor(ui_pid);
    pulse::CurrentUserSecurityAttributes pipe_security;
    if (!pipe_security) {
        HostLog(L"Cannot create pipe security descriptor");
        CoUninitialize();
        return 2;
    }
    g.pipe = CreateNamedPipeW(pipe_name.c_str(),
        PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
        1, 64 * 1024, 64 * 1024, 0, pipe_security.get());
    if (g.pipe == INVALID_HANDLE_VALUE) {
        HostLog(L"CreateNamedPipe failed");
        CoUninitialize();
        return 2;
    }

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = HostWndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = L"PulseShellHost";
    RegisterClassExW(&wc);
    g.hwnd_msg = CreateWindowExW(0, wc.lpszClassName, L"PulseShellHost", 0,
        0, 0, 0, 0, HWND_MESSAGE, nullptr, hInstance, nullptr);
    if (!g.hwnd_msg) {
        CloseHandle(g.pipe);
        CoUninitialize();
        return 3;
    }

    if (!g_context_worker) {
        wchar_t executable[32768]{};
        if (!GetModuleFileNameW(nullptr, executable, static_cast<DWORD>(std::size(executable)))) return 4;
        g_context_processes = std::make_unique<pulse::shell::ContextSessionProcesses>(executable,
            [](uint32_t type, uint32_t id, const std::vector<uint8_t>& payload) {
                PipeIoOptions options;
                options.pending_timeout_ms = 2000;
                if (g.running.load()) SendMsg(type, id, payload, options);
            });
        g_launch_processes = std::make_unique<pulse::shell::ShellLaunchProcesses>(executable,
            [](uint32_t type, uint32_t id, const std::vector<uint8_t>& payload) {
                PipeIoOptions options;
                options.pending_timeout_ms = 2000;
                if (g.running.load()) SendMsg(type, id, payload, options);
            });
        g_launch_processes->SetUiProcess(g.client_pid);
        g_interaction_processes = std::make_unique<pulse::shell::ShellLaunchProcesses>(executable,
            [](uint32_t type, uint32_t id, const std::vector<uint8_t>& payload) {
                PipeIoOptions options;
                options.pending_timeout_ms = 2000;
                if (g.running.load()) SendMsg(type, id, payload, options);
            });
        g_interaction_processes->SetUiProcess(g.client_pid);
    }
    HANDLE reader = CreateThread(nullptr, 0, ReaderThread, nullptr, 0, nullptr);
    if (ui_pid != GetCurrentProcessId()) {
        HANDLE wd = CreateThread(nullptr, 0, ParentWatchdog,
            reinterpret_cast<LPVOID>((uintptr_t)ui_pid), 0, nullptr);
        if (wd) CloseHandle(wd);
    }

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    g.running.store(false);
    CancelIoEx(g.pipe, nullptr); // unblock reader thread's pending I/O
    if (g_context_processes) g_context_processes->Stop();
    if (g_launch_processes) g_launch_processes->Stop();
    if (g_interaction_processes) g_interaction_processes->Stop();
    if (reader) {
        WaitForSingleObject(reader, 2000);
        CloseHandle(reader);
    }
    DestroyWindow(g.hwnd_msg);
    CloseHandle(g.pipe);
    CoUninitialize();
    return 0;
}
