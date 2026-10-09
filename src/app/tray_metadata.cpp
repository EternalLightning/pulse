#include "tray_metadata.h"
#include "app_worker.h"

namespace pulse::app {

void ResolveTrayMetadata(std::vector<TrayMetadataResult>& results) {
    for (auto& result : results) {
        if (result.request.ticket.expired()) continue;
        WIN32_FILE_ATTRIBUTE_DATA data{};
        if (GetFileAttributesExW(result.request.path.c_str(), GetFileExInfoStandard, &data)) {
            result.state = TrayMetadataState::Ready;
            result.attrs = data.dwFileAttributes;
            result.size = (static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
        } else {
            const DWORD error = GetLastError();
            result.state = error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND
                ? TrayMetadataState::Missing : TrayMetadataState::Unavailable;
        }
    }
}

bool PumpTrayMetadata(StagingTray& tray, WorkerPool& worker, TrayMetadataWorkerState& state) {
    std::vector<TrayMetadataResult> completed;
    {
        std::lock_guard lock(state.mailbox->mutex);
        completed.swap(state.mailbox->completed);
    }
    const bool changed = tray.ApplyMetadataResults(completed);
    const auto requests = tray.BeginMetadataQueries();
    if (requests.empty()) return changed;
    auto results = std::make_shared<std::vector<TrayMetadataResult>>();
    results->reserve(requests.size());
    // Defaults remain Unavailable if a worker task throws before completing.
    for (const auto& request : requests) results->push_back({request});
    const auto mailbox = state.mailbox;
    if (!worker.EnqueueIo([results] { ResolveTrayMetadata(*results); }, [results, mailbox] {
            std::lock_guard lock(mailbox->mutex);
            for (auto& result : *results) mailbox->completed.push_back(std::move(result));
        })) tray.RetryMetadataQueries(requests);
    return changed;
}

} // namespace pulse::app
