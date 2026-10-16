#pragma once

#include "ops_manager.h"
#include "../common/display_path.h"
#include "../common/file_operation_error.h"
#include "../common/progress_percent.h"

namespace pulse::ops {

inline void ApplyShellProgress(OpStatus& status, float percent, const std::wstring& item,
                               uint32_t items_done, uint32_t total_items, bool deleting) {
    status.percent = NormalizeProgressPercent(percent);
    status.current_item = item;
    if (!deleting) {
        if (total_items > 0) status.total_items = total_items;
        status.completed_items = (std::min)(static_cast<uint64_t>(items_done), status.total_items);
    }
    const auto suffix = status.summary.find(L"  (");
    if (suffix != std::wstring::npos) status.summary.erase(suffix);
    if (!item.empty() && status.percent >= 0.0f) {
        wchar_t text[32];
        swprintf_s(text, L"  (%.0f%%)", status.percent);
        status.summary += text;
    }
}

inline OpStatus PresentOperationStatus(OpStatus status) {
    if (status.phase == OpPhase::Completed) status.percent = 100.0f;
    if (status.phase == OpPhase::Failed && status.last_error.empty() && FAILED(status.failure_hr))
        status.last_error = format::FileOperationErrorText(status.failure_hr);
    status.summary = path::FriendlyPathText(status.summary);
    status.last_error = path::FriendlyPathText(status.last_error);
    status.source_label = path::FriendlyPathText(status.source_label);
    status.destination_label = path::FriendlyPathText(status.destination_label);
    status.current_item = path::FriendlyPathText(status.current_item);
    if (status.source_label.empty())
        status.source_label = !status.failed_path.empty() ? path::FriendlyPathText(status.failed_path) : status.current_item;
    return status;
}

inline std::wstring OperationFailureDetails(const OpStatus& status) {
    std::wstring text = status.last_error;
    const auto failed_path = path::FriendlyPathText(status.failed_path);
    if (!failed_path.empty() && text.find(failed_path) == std::wstring::npos) {
        if (!text.empty()) text += L"\n\n";
        text += failed_path;
    }
    return text;
}

} // namespace pulse::ops
