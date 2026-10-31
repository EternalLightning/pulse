// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once
#include "window_result_mailbox.h"
#include "../fs/cloud_hydrate.h"
#include <cstdint>
#include <memory>
#include <string>

namespace pulse::app {

enum class CloudDownloadEvent : uint8_t { Progress, Done, Failed, Cancelled };
struct CloudDownloadResult {
    uint64_t job = 0;
    std::wstring path;
    CloudDownloadEvent event = CloudDownloadEvent::Progress;
    DWORD error = ERROR_SUCCESS;
    DWORD attrs = INVALID_FILE_ATTRIBUTES;
    uint64_t size = 0;
    uint64_t modified = 0;
};

// Each window owns its download and mailbox. Closing one window cannot cancel
// another window's download or deliver a completion to a reused HWND.
class CloudDownloads {
public:
    CloudDownloads() = default;
    ~CloudDownloads();
    CloudDownloads(const CloudDownloads&) = delete;
    CloudDownloads& operator=(const CloudDownloads&) = delete;
    bool Start(const std::wstring& path, uint64_t size, HWND notify, UINT message);
    int Percent(const std::wstring& path) const;
    void Cancel();
    void Close();
    std::vector<CloudDownloadResult> Take(WPARAM identity);
private:
    struct Job;
    static void Run(const std::shared_ptr<Job>& job);
    std::shared_ptr<Job> job_;
    std::shared_ptr<WindowResultMailbox<CloudDownloadResult>> mailbox_;
};

} // namespace pulse::app
