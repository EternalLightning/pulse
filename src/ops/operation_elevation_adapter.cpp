// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "operation_elevation_adapter.h"
#include "operation_elevation_transport.h"
#include "../common/path_utils.h"
#include "../common/localization.h"
#include "../common/file_operation_error.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>

namespace pulse::ops {
namespace {
constexpr std::array<uint8_t, 4> kMagic{'V', 'L', 0, 1};
constexpr size_t kMaxLabelLength = 32767;

struct VolumeLabelRequest {
    std::wstring root;
    std::wstring label;
};
bool ValidRequest(const OpRequest& request) {
    return request.type == OpType::Rename && request.sources.size() == 1 &&
        path::IsDriveRootPath(request.sources.front()) && !request.is_undo &&
        request.new_name.size() <= kMaxLabelLength &&
        request.new_name.find(L'\0') == std::wstring::npos;
}
std::vector<uint8_t> Encode(const OpRequest& request) {
    if (!ValidRequest(request)) return {};
    const auto root = path::StripExtendedPathPrefix(request.sources.front());
    std::vector<uint8_t> bytes(5 + request.new_name.size() * sizeof(wchar_t));
    std::copy(kMagic.begin(), kMagic.end(), bytes.begin());
    bytes[4] = static_cast<uint8_t>(root.front());
    if (!request.new_name.empty())
        std::memcpy(bytes.data() + 5, request.new_name.data(), request.new_name.size() * sizeof(wchar_t));
    return bytes;
}
bool Decode(std::span<const uint8_t> bytes, VolumeLabelRequest& request) {
    if (bytes.size() < 5 || (bytes.size() - 5) % sizeof(wchar_t) ||
        (bytes.size() - 5) / sizeof(wchar_t) > kMaxLabelLength ||
        !std::equal(kMagic.begin(), kMagic.end(), bytes.begin())) return false;
    const wchar_t drive = static_cast<wchar_t>(bytes[4]);
    if (!((drive >= L'A' && drive <= L'Z') || (drive >= L'a' && drive <= L'z'))) return false;
    request.root = std::wstring(1, drive) + L":\\";
    request.label.resize((bytes.size() - 5) / sizeof(wchar_t));
    if (!request.label.empty())
        std::memcpy(request.label.data(), bytes.data() + 5, bytes.size() - 5);
    return request.label.find(L'\0') == std::wstring::npos;
}
OperationElevationResult Apply(const VolumeLabelRequest& request, bool cancelled,
    decltype(&SetVolumeLabelW) set_label = &SetVolumeLabelW,
    decltype(&GetDriveTypeW) drive_type = &GetDriveTypeW) {
    if (cancelled) return {HRESULT_FROM_WIN32(ERROR_CANCELLED), true, false, {}};
    const UINT type = drive_type(request.root.c_str());
    if (type == DRIVE_REMOTE || type == DRIVE_UNKNOWN || type == DRIVE_NO_ROOT_DIR)
        return {E_ACCESSDENIED, false, false, {}};
    if (!set_label(request.root.c_str(), request.label.c_str())) {
        const DWORD code = GetLastError();
        return {HRESULT_FROM_WIN32(code ? code : ERROR_GEN_FAILURE), false, false, {}};
    }
    return {S_OK, false, false, {}};
}
OperationElevationResult ExecuteWorker(void*, std::span<const uint8_t> bytes,
    OperationElevationWorkerChannel& channel) {
    VolumeLabelRequest request;
    if (!Decode(bytes, request)) return {E_INVALIDARG, false, false, {}};
    return Apply(request, channel.IsCancelled());
}
std::wstring HelperPath() {
    std::wstring executable(32768, L'\0');
    const DWORD count = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    if (!count || count >= executable.size()) return {};
    executable.resize(count);
    return (std::filesystem::path(executable).parent_path() / L"Pulse.Shell.exe").wstring();
}
} // namespace

void OperationElevationAdapter::RunParent(OpsManager& manager, const OpsManager::QueueItem& item) {
    manager.SetStatus([&](OpStatus& status) {
        status.active = true;
        status.task_id = item.seq;
        status.type = item.req.type;
        status.phase = OpPhase::Running;
        status.percent = -1.0f;
        status.source_label = status.current_item = item.req.sources.empty() ? L"" : item.req.sources.front();
        status.destination_label.clear();
        status.total_items = 1;
        status.completed_items = 0;
        status.total_bytes = status.transferred_bytes = 0;
        status.bytes_per_second = status.peak_bytes_per_second = 0;
        status.eta_seconds = 0;
        status.summary = l10n::Get(l10n::StringId::PermissionOperationContinue);
        status.last_error.clear();
        status.failed_path.clear();
        status.failure_hr = S_OK;
        status.lock_retry_available = false;
        status.locked_path.clear();
        status.lock_owners.clear();
    });
    const auto payload = Encode(item.req);
    std::pair<OpsManager*, uint64_t> context{&manager, item.retry_generation};
    OperationElevationCallbacks callbacks;
    callbacks.context = &context;
    callbacks.cancelled = [](void* raw) {
        const auto& state = *static_cast<std::pair<OpsManager*, uint64_t>*>(raw);
        return state.first->stopping_.load() || state.first->shell_cancel_requested_.load() ||
            state.first->lock_retry_cancel_.load() ||
            state.second != state.first->lock_retry_generation_.load();
    };
    OperationElevationResult result;
    if (payload.empty()) result.error = E_INVALIDARG;
    else if (callbacks.cancelled(callbacks.context)) {
        result.error = HRESULT_FROM_WIN32(ERROR_CANCELLED);
        result.cancelled = true;
    } else result = RunElevatedOperation(HelperPath(), manager.UiWindow(), payload, callbacks);
    if (!result.outcome_unknown && !result.cancelled && SUCCEEDED(result.error)) {
        const std::vector<std::wstring> destinations;
        manager.PushUndo(item.req, &destinations);
    }
    manager.SetStatus([&](OpStatus& status) {
        status.active = false;
        ++status.completed_ops;
        status.failure_hr = result.error;
        status.failed_path = item.req.sources.empty() ? L"" : item.req.sources.front();
        if (result.outcome_unknown) {
            status.phase = OpPhase::Failed;
            status.failure_hr = FAILED(result.error) ? result.error : E_FAIL;
            status.last_error = status.summary = l10n::Get(l10n::StringId::PermissionOperationOutcomeUnknown);
        } else if (result.cancelled || result.error == HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
            status.phase = OpPhase::Cancelled;
            status.failure_hr = S_OK;
            status.last_error.clear();
            status.failed_path.clear();
            status.summary = l10n::Get(l10n::StringId::OperationCancelled);
        } else if (FAILED(result.error)) {
            status.phase = OpPhase::Failed;
            status.last_error = status.summary = format::FileOperationErrorText(result.error);
        } else {
            status.phase = OpPhase::Completed;
            status.percent = 100.0f;
            status.completed_items = status.total_items;
            status.failed_path.clear();
            status.summary = l10n::Get(l10n::StringId::OpCompleted);
        }
    });
}

int OperationElevationAdapter::RunWorker(int argc, wchar_t** argv) {
    return RunOperationElevationWorker(argc, argv, {nullptr, &ExecuteWorker});
}

#ifdef PULSE_OPERATION_ELEVATION_TESTING
namespace {
unsigned test_calls = 0;
BOOL WINAPI TestSetLabel(LPCWSTR root, LPCWSTR label) {
    ++test_calls;
    return std::wstring_view(root) == L"C:\\" && std::wstring_view(label) == L"卷标";
}
UINT WINAPI FixedDrive(LPCWSTR) { return DRIVE_FIXED; }
UINT WINAPI RemoteDrive(LPCWSTR) { return DRIVE_REMOTE; }
BOOL WINAPI FailedSetLabel(LPCWSTR, LPCWSTR) { SetLastError(ERROR_ACCESS_DENIED); return FALSE; }
}
bool OperationElevationAdapter::CodecChecksForTest() {
    OpRequest request;
    request.type = OpType::Rename;
    request.sources = {L"\\\\?\\C:\\"};
    request.new_name = L"卷标";
    const auto bytes = Encode(request);
    VolumeLabelRequest decoded;
    if (!Decode(bytes, decoded) || decoded.root != L"C:\\" || decoded.label != request.new_name) return false;
    test_calls = 0;
    if (Apply(decoded, false, &TestSetLabel, &FixedDrive).error != S_OK || test_calls != 1) return false;
    if (!Apply(decoded, true, &TestSetLabel, &FixedDrive).cancelled || test_calls != 1) return false;
    if (Apply(decoded, false, &TestSetLabel, &RemoteDrive).error != E_ACCESSDENIED || test_calls != 1) return false;
    if (Apply(decoded, false, &FailedSetLabel, &FixedDrive).error != E_ACCESSDENIED) return false;
    for (const auto type : {OpType::Copy, OpType::Move, OpType::CreateFolder, OpType::CreateTextFile,
        OpType::RealDelete, OpType::RecycleDelete, OpType::RestoreRecycle, OpType::BatchRename}) {
        request.type = type;
        if (!Encode(request).empty()) return false;
    }
    request.type = OpType::Rename;
    request.sources = {L"C:\\ordinary.txt"};
    if (!Encode(request).empty()) return false;
    request.sources = {L"\\\\server\\share\\"};
    if (!Encode(request).empty()) return false;
    request.sources = {L"C:\\"};
    request.new_name = std::wstring(L"a\0b", 3);
    if (!Encode(request).empty()) return false;
    request.new_name.clear();
    if (!Decode(Encode(request), decoded) || !decoded.label.empty()) return false;
    auto corrupt = bytes;
    corrupt[0] = 0;
    if (Decode(corrupt, decoded)) return false;
    corrupt = bytes;
    corrupt[4] = '\\';
    if (Decode(corrupt, decoded)) return false;
    corrupt = bytes;
    corrupt.push_back(0);
    if (Decode(corrupt, decoded)) return false;
    corrupt = bytes;
    corrupt[5] = corrupt[6] = 0;
    if (Decode(corrupt, decoded) || Decode({}, decoded)) return false;
    request.new_name.assign(kMaxLabelLength + 1, L'a');
    return Encode(request).empty();
}
#endif

} // namespace pulse::ops
