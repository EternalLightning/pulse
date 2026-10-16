#include "../ops/operation_presentation.h"
#include <cstdio>
#include <limits>

int main() {
    int failures = 0;
    const auto check = [&](bool ok, const char* name) {
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
        failures += !ok;
    };
    using pulse::path::FriendlyPathText;
    check(pulse::WorkProgressPercent(123456, 1) == 100.0f,
          "Shell work counters cannot produce six-digit percentages");
    check(pulse::WorkProgressPercent(123456, 246912) == 50.0f,
          "recursive batch uses Shell work totals instead of selected root count");
    check(pulse::WorkProgressPercent(50, 0) == -1.0f,
          "unknown Shell work total remains indeterminate");
    pulse::ops::OpStatus progress;
    progress.summary = L"Deleting"; progress.total_items = 2;
    pulse::ops::ApplyShellProgress(progress, 123456.0f, L"child.txt", 123456, 2, true);
    check(progress.percent == 100.0f && progress.summary == L"Deleting  (100%)" &&
          progress.total_items == 2 && progress.completed_items == 0,
          "bottom detail text bounds raw percentages and preserves logical delete counts");
    pulse::ops::ApplyShellProgress(progress, 50.0f, L"child.txt", 5000, 2, true);
    check(progress.summary == L"Deleting  (50%)",
          "later Shell progress replaces the previous detail percentage");
    for (float invalid : {-1.0f, std::numeric_limits<float>::infinity(),
                          std::numeric_limits<float>::quiet_NaN()}) {
        pulse::ops::ApplyShellProgress(progress, invalid, L"child.txt", 0, 0, true);
        check(progress.percent == -1.0f && progress.summary == L"Deleting",
              "unknown or invalid progress removes the stale detail percentage");
    }
    check(FriendlyPathText(L"删除 \\\\?\\C:\\文件 完成") == L"删除 C:\\文件 完成", "embedded extended drive path");
    check(FriendlyPathText(L"\\??\\C:\\文件") == L"C:\\文件", "NT drive path");
    check(FriendlyPathText(L"\\\\?\\UNC\\server\\share\\文件") == L"\\\\server\\share\\文件", "extended UNC path");
    check(FriendlyPathText(L"失败：\\??\\unc\\server\\share") == L"失败：\\\\server\\share", "embedded NT UNC path");
    check(FriendlyPathText(L"C:\\文件 → \\\\server\\share") == L"C:\\文件 → \\\\server\\share", "ordinary paths unchanged");
    check(FriendlyPathText(L"\\\\?\\Volume{abc}\\") == L"\\\\?\\Volume{abc}\\", "unknown device path preserved");
    pulse::ops::OpStatus status;
    status.type = pulse::ops::OpType::RecycleDelete;
    status.phase = pulse::ops::OpPhase::Completed;
    status.percent = -1.0f;
    status.summary = L"删除 \\\\?\\C:\\文件 完成";
    status.last_error = L"无法访问 \\??\\C:\\文件";
    status.source_label = status.destination_label = status.current_item = L"\\\\?\\C:\\文件";
    const auto shown = pulse::ops::PresentOperationStatus(status);
    check(shown.percent == 100.0f, "completed shell deletion has full progress");
    check(shown.summary == L"删除 C:\\文件 完成" && shown.last_error == L"无法访问 C:\\文件" &&
          shown.source_label == L"C:\\文件" && shown.destination_label == L"C:\\文件" &&
          shown.current_item == L"C:\\文件", "all operation display fields sanitized");
    check(status.percent == -1.0f && status.current_item == L"\\\\?\\C:\\文件", "original status unchanged");
    for (auto phase : {pulse::ops::OpPhase::Running, pulse::ops::OpPhase::Paused,
                       pulse::ops::OpPhase::Failed, pulse::ops::OpPhase::Cancelling}) {
        status.phase = phase;
        status.percent = 37.0f;
        check(pulse::ops::PresentOperationStatus(status).percent == 37.0f, "unfinished progress preserved");
    }
    const auto reason = [](const std::wstring& text) { return text.substr(0, text.find(L" (0x")); };
    using pulse::format::FileOperationErrorText;
    const auto occupied = FileOperationErrorText(static_cast<HRESULT>(0x80270027u));
    check(reason(occupied) == reason(FileOperationErrorText(HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION))) &&
          occupied.find(L"0x80270027") != std::wstring::npos && occupied.size() > 20,
          "screenshot recycle HRESULT displays sharing violation reason and original hex code");
    check(reason(FileOperationErrorText(COPYENGINE_E_SHARING_VIOLATION_DEST)) == reason(occupied),
          "destination sharing failure has the same human-readable cause");
    check(reason(FileOperationErrorText(COPYENGINE_E_ACCESS_DENIED_SRC)) ==
          reason(FileOperationErrorText(E_ACCESSDENIED)) &&
          reason(FileOperationErrorText(COPYENGINE_E_DISK_FULL)) ==
          reason(FileOperationErrorText(HRESULT_FROM_WIN32(ERROR_DISK_FULL))),
          "permission and disk-full errors describe their actual cause");
    check(FileOperationErrorText(static_cast<HRESULT>(0x81230020u)) == L"0x81230020",
          "unknown facility cannot masquerade as low-word sharing violation");
    check(FileOperationErrorText(S_OK).empty(), "successful result is not described as an error");
    status.phase = pulse::ops::OpPhase::Failed;
    status.failure_hr = static_cast<HRESULT>(0x80270027u);
    status.last_error.clear(); status.source_label.clear(); status.current_item.clear();
    status.failed_path = L"\\\\?\\E:\\Study\\示范项目";
    const auto failed = pulse::ops::PresentOperationStatus(status);
    check(failed.source_label == L"E:\\Study\\示范项目" && failed.failed_path == status.failed_path,
          "failed deletion shows the actual path instead of source placeholder");
    check(failed.last_error == occupied && status.last_error.empty(),
          "structured failure fills a missing error reason without changing raw operation status");
    check(pulse::ops::OperationFailureDetails(failed).find(failed.source_label) != std::wstring::npos,
          "expanded failure retains the complete path separately from the reason");
    status.source_label = L"E:\\Study";
    status.last_error = L"identity changed; deletion stopped | E:\\Study\\示范项目";
    const auto custom = pulse::ops::PresentOperationStatus(status);
    check(custom.last_error == status.last_error &&
          pulse::ops::OperationFailureDetails(custom) == status.last_error,
          "specific safety refusal is preserved and path is not repeated");
    status.phase = pulse::ops::OpPhase::Running;
    status.source_label.clear(); status.failed_path.clear(); status.current_item = L"E:\\Study\\示范项目";
    check(pulse::ops::PresentOperationStatus(status).source_label == status.current_item,
          "current item supplies a real path while deletion is running");
    return failures ? 1 : 0;
}
