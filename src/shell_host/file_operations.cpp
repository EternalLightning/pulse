// SPDX-License-Identifier: Apache-2.0
#include "file_operations.h"
#include "recycle_item.h"
#include "../common/path_utils.h"
#include "../common/shell_id.h"
#include "../common/progress_percent.h"
#include <shlobj.h>
#include <shellapi.h>
#include <sherrors.h>
#include <memory>
#include <algorithm>
#include <map>
#ifdef PULSE_FILE_OPERATION_TRACE
#include <cstdio>
#endif

namespace pulse::shell {
namespace {
using namespace ipc;
template<class T> struct ReleaseCom { void operator()(T* p) const { if (p) p->Release(); } };
template<class T> using ComPtr = std::unique_ptr<T, ReleaseCom<T>>;
std::wstring Name(IShellItem* item) {
    PWSTR text = nullptr;
    if (!item || FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &text))) return {};
    std::wstring result = text ? path::StripExtendedPathPrefix(text) : std::wstring{};
    CoTaskMemFree(text); return result;
}
std::wstring Parent(const std::wstring& name) {
    const auto slash = name.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring{} : name.substr(0, slash == 2 && name[1] == L':' ? 3 : slash);
}
std::wstring Leaf(const std::wstring& name) {
    const auto slash = name.find_last_of(L"\\/");
    return slash == std::wstring::npos ? name : name.substr(slash + 1);
}
bool SameName(std::wstring left, std::wstring right) {
    left = path::StripExtendedPathPrefix(left); right = path::StripExtendedPathPrefix(right);
    std::replace(left.begin(), left.end(), L'/', L'\\');
    std::replace(right.begin(), right.end(), L'/', L'\\');
    return path::EqualInsensitive(left, right);
}
std::wstring ShellPath(const std::wstring& name) {
    auto parsed = path::StripExtendedPathPrefix(name);
    std::replace(parsed.begin(), parsed.end(), L'/', L'\\');
    return parsed;
}
HRESULT MakeItem(const std::wstring& name, IShellItem** out) {
    const auto parsed = ShellPath(name);
    return shell_id::CreateItem(parsed, IID_PPV_ARGS(out));
}
bool Skipped(HRESULT hr) {
    return hr == COPYENGINE_S_USER_IGNORED ||
        hr == COPYENGINE_S_PENDING || hr == COPYENGINE_S_PENDING_DELETE || hr == COPYENGINE_S_PENDING_BATCH_COPY;
}
bool Cancelled(HRESULT hr) {
    return hr == COPYENGINE_E_USER_CANCELLED || hr == HRESULT_FROM_WIN32(ERROR_CANCELLED) || hr == E_ABORT;
}
DWORD OperationFlags(const ShellOperationRequest& request, bool reporting_bytes) {
    DWORD flags = FOF_SILENT | FOF_NOERRORUI | FOF_NOCONFIRMMKDIR |
        FOF_NO_CONNECTED_ELEMENTS | FOFX_EARLYFAILURE;
    if (request.elevation_authorized && request.kind != ShellOperationKind::Recycle)
        flags |= FOFX_SHOWELEVATIONPROMPT | FOFX_REQUIREELEVATION;
    if (request.kind == ShellOperationKind::Recycle) {
        flags |= FOF_ALLOWUNDO | FOFX_ADDUNDORECORD | FOFX_RECYCLEONDELETE;
    } else flags |= FOF_NOCONFIRMATION;
    if (request.collision != ShellCollisionPolicy::Replace) {
        flags |= FOF_RENAMEONCOLLISION;
        if (request.kind != ShellOperationKind::Rename) flags |= FOFX_PRESERVEFILEEXTENSIONS;
    }
    if (reporting_bytes) flags &= ~FOF_SILENT;
    return flags;
}
class ProgressSink final : public IFileOperationProgressSink {
public:
    ProgressSink(const ShellOperationRequest& request, const ShellOperationItem& item,
        const FileOperationCallbacks& callbacks, size_t index, size_t* batch_done = nullptr)
        : request_(request), item_(item), callbacks_(callbacks), index_(index), batch_done_(batch_done) {
        result.source = item.source;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (iid != IID_IUnknown && iid != IID_IFileOperationProgressSink) return E_NOINTERFACE;
        *out = this; AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override { return --references_; }
    HRESULT Check() {
        while (callbacks_.paused && callbacks_.paused()) {
            if (callbacks_.cancelled && callbacks_.cancelled()) return Error(HRESULT_FROM_WIN32(ERROR_CANCELLED));
            // The independent pipe reader resumes/cancels us; do not dispatch
            // another queued mutation reentrantly from this callback.
            Sleep(30);
        }
        return callbacks_.cancelled && callbacks_.cancelled() ? Error(HRESULT_FROM_WIN32(ERROR_CANCELLED)) : S_OK;
    }
    HRESULT Error(HRESULT hr, const std::wstring& failed = {}) {
        if (FAILED(hr) && SUCCEEDED(failure)) { failure = hr; failed_path = failed.empty() ? item_.source : failed; }
        return hr;
    }
    HRESULT Pre(DWORD flags, IShellItem* source, IShellItem* folder, LPCWSTR new_name) {
#ifdef PULSE_FILE_OPERATION_TRACE
        std::wprintf(L"PRE kind=%u flags=%lx source=%ls new=%ls\n", static_cast<unsigned>(request_.kind), flags, Name(source).c_str(), new_name ? new_name : L"");
#endif
        HRESULT hr = Check();
        if (FAILED(hr)) return hr;
        const auto source_name = Name(source);
        if (source) original_names_[source] = source_name;
        if (batch_done_ && !SameName(source_name, item_.source)) return Error(E_UNEXPECTED);
        if (item_.expected_identity.Valid() && SameName(source_name, item_.source)) {
            FileIdentity current;
            if (!ReadPathFileIdentity(item_.source, current) || current != item_.expected_identity)
                return Error(HRESULT_FROM_WIN32(ERROR_FILE_INVALID), item_.source);
        }
        if (request_.collision == ShellCollisionPolicy::Fail && folder) {
            const auto parent = Name(folder);
            const auto leaf = new_name && *new_name ? std::wstring(new_name) : Leaf(source_name);
            const auto target = parent + L"\\" + leaf;
            if (!(request_.kind == ShellOperationKind::Rename && SameName(target, source_name)) &&
                ((flags & TSF_OVERWRITE_EXIST) || GetFileAttributesW(target.c_str()) != INVALID_FILE_ATTRIBUTES))
                return Error(HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS), target);
        }
        if (item_.expected_destination_identity.Valid() && folder && SameName(source_name, item_.source)) {
            FileIdentity current;
            if (!ReadPathFileIdentity(item_.destination, current) || current != item_.expected_destination_identity)
                return Error(HRESULT_FROM_WIN32(ERROR_FILE_INVALID), item_.destination);
        }
        return S_OK;
    }
    HRESULT Post(IShellItem* source, HRESULT hr, IShellItem* created) {
#ifdef PULSE_FILE_OPERATION_TRACE
        std::wprintf(L"POST kind=%u hr=%lx source=%ls created=%ls\n", static_cast<unsigned>(request_.kind), hr, Name(source).c_str(), Name(created).c_str());
        if (created) { PWSTR value = nullptr; const auto named = created->GetDisplayName(SIGDN_DESKTOPABSOLUTEPARSING, &value); std::wprintf(L"CREATED pointer=%p parsingHr=%lx parsing=%ls\n", created, named, value ? value : L""); CoTaskMemFree(value); }
#endif
        const auto original = original_names_.find(source);
        const auto source_name = original == original_names_.end() ? Name(source) : original->second;
        const auto created_name = Name(created);
        const bool primary = batch_done_ || item_.source.empty() || request_.kind == ShellOperationKind::Rename ||
            request_.kind == ShellOperationKind::Recycle ||
            SameName(source_name, item_.source) ||
            (!created_name.empty() && !item_.destination.empty() &&
             SameName(Parent(created_name), Parent(item_.destination)));
        if (primary) {
            result.hr = hr; result.skipped = Skipped(hr);
            result.destination.clear();
            if (SUCCEEDED(hr) && !result.skipped) {
                result.destination = created_name;
                if (request_.kind != ShellOperationKind::DeletePermanent && result.destination.empty())
                    result.hr = Error(E_UNEXPECTED);
                if (request_.collision == ShellCollisionPolicy::Fail && !item_.destination.empty() &&
                    !result.destination.empty() && !SameName(result.destination, item_.destination))
                    Error(HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS), item_.destination);
            }
        } else {
            ShellOperationItemResult completed;
            completed.source = source_name; completed.hr = hr; completed.skipped = Skipped(hr);
            if (SUCCEEDED(hr) && !completed.skipped) completed.destination = Name(created);
            if (!completed.source.empty()) {
                const auto [found, inserted] = nested_indices_.try_emplace(completed.source, nested.size());
                if (inserted) nested.push_back(std::move(completed));
                else nested[found->second] = std::move(completed);
            }
        }
        return Check();
    }
    HRESULT STDMETHODCALLTYPE StartOperations() override { return Check(); }
    HRESULT STDMETHODCALLTYPE FinishOperations(HRESULT hr) override {
        // Shell can report an error before prompting and retrying the same item.
        // Only its last Post result is final; caller/validation errors stay sticky.
        const auto record = [&](const ShellOperationItemResult& item) {
            if (item.hr == E_PENDING) return;
            const bool destination_error = item.hr == COPYENGINE_E_SHARING_VIOLATION_DEST ||
                item.hr == COPYENGINE_E_ACCESS_DENIED_DEST || item.hr == COPYENGINE_E_PATH_NOT_FOUND_DEST ||
                item.hr == COPYENGINE_E_NET_DISCONNECT_DEST || item.hr == COPYENGINE_E_PATH_TOO_DEEP_DEST;
            Error(item.hr, destination_error ? item_.destination : item.source);
        };
        record(result);
        for (const auto& item : nested) record(item);
        Error(hr);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE PreRenameItem(DWORD flags, IShellItem* source, LPCWSTR name) override {
        IShellItem* raw = nullptr;
        const HRESULT hr = source->GetParent(&raw);
        ComPtr<IShellItem> parent(raw);
        return FAILED(hr) ? Error(hr) : Pre(flags, source, parent.get(), name);
    }
    HRESULT STDMETHODCALLTYPE PostRenameItem(DWORD, IShellItem* source, LPCWSTR, HRESULT hr, IShellItem* created) override { return Post(source, hr, created); }
    HRESULT STDMETHODCALLTYPE PreMoveItem(DWORD flags, IShellItem* source, IShellItem* folder, LPCWSTR name) override { return Pre(flags, source, folder, name); }
    HRESULT STDMETHODCALLTYPE PostMoveItem(DWORD, IShellItem* source, IShellItem*, LPCWSTR, HRESULT hr, IShellItem* created) override { return Post(source, hr, created); }
    HRESULT STDMETHODCALLTYPE PreCopyItem(DWORD flags, IShellItem* source, IShellItem* folder, LPCWSTR name) override {
        HRESULT hr = Pre(flags, source, folder, name);
        if (!batch_done_ || FAILED(hr)) return hr;
        const DWORD attributes = GetFileAttributesW(item_.source.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)))
            return Error(HRESULT_FROM_WIN32(ERROR_FILE_INVALID));
        if (callbacks_.progress) callbacks_.progress(0.0f, item_.source, *batch_done_, request_.items.size());
        hr = Check();
        started = started || SUCCEEDED(hr);
        return hr;
    }
    HRESULT STDMETHODCALLTYPE PostCopyItem(DWORD, IShellItem* source, IShellItem*, LPCWSTR, HRESULT hr, IShellItem* created) override {
        const HRESULT checked = Post(source, hr, created);
        if (!batch_done_) return checked;
        if (SUCCEEDED(result.hr) && !result.skipped && !completion_reported_) {
            completion_reported_ = true;
            ++*batch_done_;
            if (callbacks_.progress) callbacks_.progress(0.0f, item_.source, *batch_done_, request_.items.size());
        }
        return hr == COPYENGINE_S_USER_IGNORED ? E_ABORT : checked;
    }
    HRESULT STDMETHODCALLTYPE PreDeleteItem(DWORD flags, IShellItem* source) override {
        // Providers report transfer flags such as 0x202 for valid recycle requests.
        // The recycle-only Shell path selects the mode; these flags cannot prove a permanent fallback.
        return Pre(flags, source, nullptr, nullptr);
    }
    HRESULT STDMETHODCALLTYPE PostDeleteItem(DWORD, IShellItem* source, HRESULT hr, IShellItem* created) override { return Post(source, hr, created); }
    HRESULT STDMETHODCALLTYPE PreNewItem(DWORD flags, IShellItem* folder, LPCWSTR name) override { return Pre(flags, nullptr, folder, name); }
    HRESULT STDMETHODCALLTYPE PostNewItem(DWORD, IShellItem*, LPCWSTR, LPCWSTR, DWORD, HRESULT hr, IShellItem* created) override { return Post(nullptr, hr, created); }
    HRESULT STDMETHODCALLTYPE UpdateProgress(UINT total, UINT done) override {
        if (!batch_done_ && callbacks_.progress) callbacks_.progress(WorkProgressPercent(done, total),
            item_.source.empty() ? item_.destination : item_.source, index_, request_.items.size());
        return Check();
    }
    HRESULT STDMETHODCALLTYPE ResetTimer() override { return Check(); }
    HRESULT STDMETHODCALLTYPE PauseTimer() override { return Check(); }
    HRESULT STDMETHODCALLTYPE ResumeTimer() override { return Check(); }
    ShellOperationItemResult result;
    std::vector<ShellOperationItemResult> nested;
    HRESULT failure = S_OK;
    std::wstring failed_path;
    bool started = false;
private:
    ULONG references_ = 1;
    const ShellOperationRequest& request_;
    const ShellOperationItem& item_;
    const FileOperationCallbacks& callbacks_;
    size_t index_;
    size_t* batch_done_;
    bool completion_reported_ = false;
    std::map<IShellItem*, std::wstring> original_names_;
    std::map<std::wstring, size_t> nested_indices_;
};

// Shell measures transferred bytes; Pulse supplies the visible progress UI.
class ProgressDialog final : public IOperationsProgressDialog {
public:
    ProgressDialog(ProgressSink& sink, const FileOperationCallbacks& callbacks) : sink_(sink), callbacks_(callbacks) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (iid != IID_IUnknown && iid != IID_IOperationsProgressDialog) return E_NOINTERFACE;
        *out = this; AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override { return --references_; }
    HRESULT STDMETHODCALLTYPE StartProgressDialog(HWND, OPPROGDLGF) override { return sink_.Check(); }
    HRESULT STDMETHODCALLTYPE StopProgressDialog() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE SetOperation(SPACTION) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE SetMode(PDMODE) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE UpdateProgress(ULONGLONG, ULONGLONG, ULONGLONG current, ULONGLONG total,
        ULONGLONG, ULONGLONG) override {
        if (callbacks_.bytes_progress) callbacks_.bytes_progress(current, total);
        return sink_.Check();
    }
    HRESULT STDMETHODCALLTYPE UpdateLocations(IShellItem*, IShellItem*, IShellItem*) override { return sink_.Check(); }
    HRESULT STDMETHODCALLTYPE ResetTimer() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE PauseTimer() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE ResumeTimer() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetMilliseconds(ULONGLONG* elapsed, ULONGLONG* remaining) override {
        if (!elapsed || !remaining) return E_POINTER;
        *elapsed = *remaining = 0; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetOperationStatus(PDOPSTATUS* status) override {
        if (!status) return E_POINTER;
        *status = callbacks_.cancelled && callbacks_.cancelled() ? PDOPS_CANCELLED :
            callbacks_.paused && callbacks_.paused() ? PDOPS_PAUSED : PDOPS_RUNNING;
        return S_OK;
    }
private:
    ULONG references_ = 1;
    ProgressSink& sink_;
    const FileOperationCallbacks& callbacks_;
};

HRESULT ConfigureOperation(IFileOperation& operation, const ShellOperationRequest& request, ProgressDialog* dialog) {
    HRESULT hr = operation.SetOperationFlags(OperationFlags(request, dialog != nullptr));
    if (SUCCEEDED(hr) && dialog) hr = operation.SetProgressDialog(dialog);
    if (SUCCEEDED(hr)) hr = operation.SetOwnerWindow(reinterpret_cast<HWND>(static_cast<uintptr_t>(request.owner_hwnd)));
    return hr;
}

HRESULT PerformOperations(IFileOperation& operation, BOOL& aborted) {
    const HRESULT hr = operation.PerformOperations();
    const HRESULT queried = operation.GetAnyOperationsAborted(&aborted);
    return SUCCEEDED(hr) && FAILED(queried) ? queried : hr;
}

bool CanBatchCopy(const ShellOperationRequest& request) {
    if (request.collision != ShellCollisionPolicy::KeepBoth ||
        request.elevation_authorized || request.items.size() < 2 || request.items.size() > kShellCopyBatchItems) return false;
    const auto parent = Parent(ShellPath(request.items.front().destination));
    return std::all_of(request.items.begin(), request.items.end(), [&](const auto& item) {
        return item.expected_identity.Valid() && !item.expected_destination_identity.Valid() &&
            Parent(ShellPath(item.destination)) == parent;
    });
}

ShellOperationResult CopyBatch(const ShellOperationRequest& request, const FileOperationCallbacks& callbacks) {
    ShellOperationResult result;
    const auto fail = [&](HRESULT hr, const std::wstring& path) {
        if (SUCCEEDED(result.hr)) { result.hr = hr; result.failed_path = path; }
    };
    size_t completed = 0;
    std::vector<std::unique_ptr<ProgressSink>> sinks;
    for (size_t i = 0; i < request.items.size(); ++i)
        sinks.push_back(std::make_unique<ProgressSink>(request, request.items[i], callbacks, i, &completed));
    ProgressDialog dialog(*sinks.front(), callbacks);
    IFileOperation* raw = nullptr;
    HRESULT prepare_hr = sinks.front()->Check();
    if (SUCCEEDED(prepare_hr)) prepare_hr = CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&raw));
    ComPtr<IFileOperation> operation(raw);
    if (SUCCEEDED(prepare_hr)) prepare_hr = ConfigureOperation(*operation, request, callbacks.bytes_progress ? &dialog : nullptr);
    IShellItem* folder = nullptr;
    if (SUCCEEDED(prepare_hr)) prepare_hr = MakeItem(Parent(ShellPath(request.items.front().destination)), &folder);
    ComPtr<IShellItem> destination(folder);
    size_t queued = 0;
    for (; SUCCEEDED(prepare_hr) && queued < request.items.size(); ++queued) {
        const auto& item = request.items[queued];
        IShellItem* parsed = nullptr;
        prepare_hr = MakeItem(item.source, &parsed);
        ComPtr<IShellItem> source(parsed);
        if (SUCCEEDED(prepare_hr)) prepare_hr = operation->CopyItem(source.get(), destination.get(),
            Leaf(item.destination).c_str(), sinks[queued].get());
        if (FAILED(prepare_hr)) break;
    }
    BOOL aborted = FALSE;
    const HRESULT run_hr = queued ? PerformOperations(*operation, aborted) : S_OK;
    size_t first_pending = request.items.size();
    for (size_t i = 0; i < request.items.size(); ++i) {
        auto& sink = *sinks[i];
        auto& item = sink.result;
        sink.FinishOperations(S_OK);
        // A cancellation after PostCopyItem still leaves a confirmed product.
        if (!sink.started && (item.hr == E_PENDING || Cancelled(item.hr)) && Cancelled(sink.failure)) {
            item.hr = E_PENDING;
            item.skipped = true;
            fail(sink.failure, item.source);
        } else if (item.hr == E_PENDING || FAILED(item.hr) || item.skipped) {
            if (FAILED(sink.failure)) item.hr = sink.failure;
            else if (i == queued && FAILED(prepare_hr) && !Cancelled(prepare_hr)) item.hr = prepare_hr;
            else if (item.hr != E_PENDING || sink.started) item.hr = FAILED(run_hr) ? run_hr : E_ABORT;
            if (item.hr == E_PENDING) first_pending = (std::min)(first_pending, i);
            else fail(item.hr, sink.failed_path.empty() ? item.source : sink.failed_path);
        }
        result.items.push_back(std::move(item));
    }
    if (SUCCEEDED(result.hr) && (FAILED(run_hr) || aborted || FAILED(prepare_hr) || first_pending < result.items.size())) {
        const HRESULT hr = FAILED(run_hr) ? run_hr : FAILED(prepare_hr) ? prepare_hr : aborted ? E_ABORT : E_FAIL;
        fail(hr, request.items[(std::min)(first_pending, request.items.size() - 1)].source);
    }
    result.cancelled = Cancelled(result.hr) && ((callbacks.cancelled && callbacks.cancelled()) ||
        aborted || result.hr == COPYENGINE_E_USER_CANCELLED);
    return result;
}

ShellOperationResult RenameCase(const ShellOperationRequest& request, const ShellOperationItem& item,
    const FileOperationCallbacks& callbacks) {
    FileIdentity identity;
    GUID nonce{};
    wchar_t suffix[40]{};
    ShellOperationResult failed;
    failed.failed_path = item.source;
    if (!ReadPathFileIdentity(item.source, identity) ||
        (item.expected_identity.Valid() && identity != item.expected_identity)) {
        failed.hr = HRESULT_FROM_WIN32(ERROR_FILE_INVALID);
        return failed;
    }
    failed.hr = CoCreateGuid(&nonce);
    if (FAILED(failed.hr)) return failed;
    StringFromGUID2(nonce, suffix, static_cast<int>(std::size(suffix)));
    // exFAT and provider Shell handlers can ignore or reject a case-only rename.
    // Both steps use IFileOperation; retain the confirmed intermediate if rollback also fails.
    auto step = request;
    step.collision = ShellCollisionPolicy::Fail;
    step.items = {{item.source, Parent(item.source) + L"\\pulse-rename-" + std::wstring(suffix + 1, 36) + L".tmp", identity, {}}};
    auto first = ExecuteFileOperation(step, callbacks);
    if (FAILED(first.hr) || first.items.empty()) return first;
    const auto temporary = first.items.front().destination;
    // exFAT may assign a new file ID when the directory entry is renamed.
    if (!ReadPathFileIdentity(temporary, identity)) {
        first.hr = HRESULT_FROM_WIN32(ERROR_FILE_INVALID);
        first.failed_path = temporary;
        return first;
    }
    step.items = {{temporary, item.destination, identity, {}}};
    auto second = ExecuteFileOperation(step, callbacks);
    if (SUCCEEDED(second.hr) && !second.items.empty()) {
        second.items.front().source = item.source;
        return second;
    }
    step.items = {{temporary, item.source, identity, {}}};
    const auto restored = ExecuteFileOperation(step, {});
    if (SUCCEEDED(restored.hr)) second.items.clear();
    else second.items = std::move(first.items);
    second.failed_path = item.destination;
    return second;
}
}

ipc::ShellOperationResult ExecuteFileOperation(const ipc::ShellOperationRequest& request,
    const FileOperationCallbacks& callbacks) {
    ShellOperationResult result;
    if (request.items.empty()) { result.hr = E_INVALIDARG; return result; }
    if (request.kind == ShellOperationKind::CopyBatch) {
        if (CanBatchCopy(request)) return CopyBatch(request, callbacks);
        result.hr = E_INVALIDARG;
        return result;
    }
    for (size_t index = 0; index < request.items.size(); ++index) {
        const auto& item = request.items[index];
        if (request.kind == ShellOperationKind::Rename && SameName(item.source, item.destination) &&
            Leaf(item.source) != Leaf(item.destination)) {
            auto renamed = RenameCase(request, item, callbacks);
            for (auto& completed : renamed.items) result.items.push_back(std::move(completed));
            if (FAILED(renamed.hr)) {
                result.hr = renamed.hr; result.cancelled = renamed.cancelled;
                result.failed_path = std::move(renamed.failed_path);
                break;
            }
            continue;
        }
        ProgressSink sink(request, item, callbacks, index);
        ProgressDialog dialog(sink, callbacks);
        HRESULT hr = sink.Check();
        if (SUCCEEDED(hr) && item.expected_identity.Valid()) {
            FileIdentity identity;
            if (!ReadPathFileIdentity(item.source, identity) || identity != item.expected_identity)
                hr = HRESULT_FROM_WIN32(ERROR_FILE_INVALID);
        }
        IFileOperation* raw = nullptr;
        const bool recycling = request.kind == ShellOperationKind::Recycle;
        if (SUCCEEDED(hr) && !recycling) hr = CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&raw));
        ComPtr<IFileOperation> operation(raw);
        DWORD cookie = 0;
        bool advised = false;
        if (SUCCEEDED(hr) && operation) {
            const bool reporting_bytes = callbacks.bytes_progress &&
                (request.kind == ShellOperationKind::Copy || request.kind == ShellOperationKind::Move);
            hr = ConfigureOperation(*operation, request, reporting_bytes ? &dialog : nullptr);
            if (SUCCEEDED(hr)) { hr = operation->Advise(&sink, &cookie); advised = SUCCEEDED(hr); }
        }
        const bool creating = request.kind == ShellOperationKind::CreateFolder || request.kind == ShellOperationKind::CreateFile;
        const bool deleting = request.kind == ShellOperationKind::DeletePermanent || request.kind == ShellOperationKind::Recycle;
        ComPtr<IShellItem> source, destination;
        if (SUCCEEDED(hr) && !creating) {
            IShellItem* parsed = nullptr; hr = MakeItem(item.source, &parsed); source.reset(parsed);
        }
        const auto target = ShellPath(item.destination);
        if (SUCCEEDED(hr) && !deleting && request.kind != ShellOperationKind::Rename) {
            IShellItem* parsed = nullptr; hr = MakeItem(Parent(target), &parsed); destination.reset(parsed);
            if (hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)) hr = HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND);
        }
        if (SUCCEEDED(hr) && recycling) {
            IShellItem* recycled = nullptr;
            hr = RecycleShellItem(source.get(), callbacks, &recycled,
                [&] { return sink.PreDeleteItem(0, source.get()); });
            ComPtr<IShellItem> created(recycled);
            sink.PostDeleteItem(0, source.get(), hr, created.get());
        } else if (SUCCEEDED(hr)) {
            switch (request.kind) {
            case ShellOperationKind::Copy: hr = operation->CopyItem(source.get(), destination.get(), Leaf(target).c_str(), nullptr); break;
            case ShellOperationKind::Move: hr = operation->MoveItem(source.get(), destination.get(), Leaf(target).c_str(), nullptr); break;
            case ShellOperationKind::Rename:
                if (!SameName(Parent(item.source), Parent(target))) hr = E_INVALIDARG;
                else hr = operation->RenameItem(source.get(), Leaf(target).c_str(), nullptr);
                break;
            case ShellOperationKind::CreateFolder: case ShellOperationKind::CreateFile:
                hr = operation->NewItem(destination.get(), request.kind == ShellOperationKind::CreateFolder ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL,
                    Leaf(target).c_str(), nullptr, nullptr); break;
            case ShellOperationKind::DeletePermanent: hr = operation->DeleteItem(source.get(), nullptr); break;
            case ShellOperationKind::CopyBatch:
            case ShellOperationKind::Recycle: hr = E_UNEXPECTED; break;
            }
        }
        BOOL aborted = FALSE;
        if (SUCCEEDED(hr) && operation) hr = PerformOperations(*operation, aborted);
        if (advised) operation->Unadvise(cookie);
        sink.FinishOperations(hr);
        if (FAILED(sink.failure)) hr = sink.failure;
        if (SUCCEEDED(hr) && (aborted || sink.result.skipped || FAILED(sink.result.hr)))
            hr = FAILED(sink.result.hr) && sink.result.hr != E_PENDING ? sink.result.hr : E_ABORT;
        if (FAILED(hr)) sink.result.hr = hr;
        result.items.push_back(std::move(sink.result));
        for (auto& completed : sink.nested) result.items.push_back(std::move(completed));
        result.cancelled = Cancelled(hr) && ((callbacks.cancelled && callbacks.cancelled()) || aborted || hr == COPYENGINE_E_USER_CANCELLED);
        if (FAILED(hr)) {
            result.hr = hr;
            result.failed_path = sink.failed_path.empty() ? (item.source.empty() ? item.destination : item.source) : sink.failed_path;
            break;
        }
        if (callbacks.progress) callbacks.progress(100.0f, item.source.empty() ? item.destination : item.source, index + 1, request.items.size());
    }
    return result;
}
}
