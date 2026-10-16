#pragma once

#include "../ops/archive_progress.h"
#include "../ops/archive.h"
#include "../ops/ops_manager.h"

namespace pulse::app {

ops::OpStatus ArchiveOperationStatus(const ops::ArchiveProgress& progress,
    const std::wstring& archive, const std::wstring& destination, bool cancelling);

ops::OpStatus ArchiveFailureStatus(const ops::ArchiveOperationResult& result,
    const std::wstring& archive, const std::wstring& destination);

bool ShowArchiveProgressWindow(HWND owner, bool dark, COLORREF accent,
    std::atomic_bool& done, std::atomic_bool& cancel,
    const std::function<ops::OpStatus()>& read_status);

} // namespace pulse::app
