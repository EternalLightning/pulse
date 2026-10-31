// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "cloud_download.h"
#include <algorithm>
#include <atomic>
#include <mutex>
#include <thread>

namespace pulse::app {
namespace {
std::atomic<uint64_t> next_job{1};
bool SamePath(const std::wstring& a, const std::wstring& b) {
    return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL;
}
}

struct CloudDownloads::Job {
    std::wstring path;
    uint64_t size = 0;
    uint64_t id = 0;
    std::atomic<bool> cancel{false};
    std::atomic<bool> finished{false};
    std::atomic<uint64_t> done{0};
    std::mutex thread_mutex;
    HANDLE thread = nullptr;
    std::shared_ptr<WindowResultMailbox<CloudDownloadResult>> mailbox;
};

CloudDownloads::~CloudDownloads() { Close(); }

void CloudDownloads::Run(const std::shared_ptr<Job>& job) {
    {
        std::lock_guard lock(job->thread_mutex);
        DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(),
            &job->thread, 0, FALSE, DUPLICATE_SAME_ACCESS);
    }
    ULONGLONG last_post = 0;
    DWORD error = ERROR_SUCCESS;
    fs::HydrateResult result = fs::HydrateResult::Failed;
    try {
        result = fs::HydrateByReading(job->path, job->cancel, [&](uint64_t bytes) {
            job->done.store(bytes, std::memory_order_relaxed);
            const ULONGLONG now = GetTickCount64();
            if (now - last_post >= 120) {
                last_post = now;
                job->mailbox->Publish({job->id, job->path, CloudDownloadEvent::Progress});
            }
        }, &error);
    } catch (...) { error = ERROR_NOT_ENOUGH_MEMORY; }
    {
        std::lock_guard lock(job->thread_mutex);
        if (job->thread) { CloseHandle(job->thread); job->thread = nullptr; }
    }
    job->finished.store(true, std::memory_order_release);
    const auto event = result == fs::HydrateResult::Done ? CloudDownloadEvent::Done
        : result == fs::HydrateResult::Cancelled ? CloudDownloadEvent::Cancelled : CloudDownloadEvent::Failed;
    CloudDownloadResult completed{job->id, job->path, event, error};
    if (result == fs::HydrateResult::Done) {
        WIN32_FILE_ATTRIBUTE_DATA data{};
        if (GetFileAttributesExW(job->path.c_str(), GetFileExInfoStandard, &data)) {
            completed.attrs = data.dwFileAttributes;
            completed.size = (static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
            completed.modified = (static_cast<uint64_t>(data.ftLastWriteTime.dwHighDateTime) << 32) | data.ftLastWriteTime.dwLowDateTime;
        }
    }
    job->mailbox->Publish(std::move(completed));
}

bool CloudDownloads::Start(const std::wstring& path, uint64_t size, HWND notify, UINT message) {
    if (path.empty() || !notify || !message) return false;
    if (job_ && !job_->finished.load(std::memory_order_acquire) &&
        !job_->cancel.load(std::memory_order_relaxed) && SamePath(job_->path, path)) return true;
    Cancel();
    if (!mailbox_) mailbox_ = std::make_shared<WindowResultMailbox<CloudDownloadResult>>(notify, message);
    auto job = std::make_shared<Job>();
    job->path = path; job->size = size; job->id = next_job.fetch_add(1); job->mailbox = mailbox_;
    job_ = job;
    try { std::thread([job] { Run(job); }).detach(); }
    catch (...) { job_.reset(); return false; }
    return true;
}

int CloudDownloads::Percent(const std::wstring& path) const {
    if (!job_ || job_->finished.load(std::memory_order_acquire) || !SamePath(job_->path, path)) return -1;
    if (!job_->size) return 0;
    return static_cast<int>((std::min)(100.0L,
        static_cast<long double>(job_->done.load(std::memory_order_relaxed)) * 100.0L / job_->size));
}

void CloudDownloads::Cancel() {
    if (!job_) return;
    job_->cancel.store(true, std::memory_order_relaxed);
    std::lock_guard lock(job_->thread_mutex);
    if (job_->thread) CancelSynchronousIo(job_->thread);
}

void CloudDownloads::Close() {
    if (mailbox_) mailbox_->Close();
    Cancel();
    job_.reset();
    mailbox_.reset();
}

std::vector<CloudDownloadResult> CloudDownloads::Take(WPARAM identity) {
    if (!mailbox_) return {};
    auto results = mailbox_->Take(identity);
    std::erase_if(results, [&](const CloudDownloadResult& result) { return !job_ || result.job != job_->id; });
    if (std::any_of(results.begin(), results.end(), [](const auto& result) {
        return result.event != CloudDownloadEvent::Progress;
    })) job_.reset();
    return results;
}
} // namespace pulse::app
