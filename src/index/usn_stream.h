// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once
#include "usn_packet_queue.h"
#include "usn_read_wait.h"
#include "usn_record.h"
#include <winioctl.h>
#include <new>
#include <thread>
#include <string>

namespace pulse::index {
// The collector owns its handle/cursor. Applying records, merging the index
// and writing cache files never hold its queue lock or delay the next read.
class UsnStream {
public:
    UsnStream(wchar_t letter, uint64_t journal, int64_t cursor, HANDLE signal)
        : UsnStream(signal) {
        thread_ = std::thread([this, letter, journal, cursor] { Read(letter, journal, cursor); });
    }
    ~UsnStream() {
        if (stop_) SetEvent(stop_);
        if (thread_.joinable()) thread_.join();
        if (stop_) CloseHandle(stop_);
    }
    bool Take(std::vector<BYTE>& records, int64_t& cursor) {
        bool more = false;
        const bool ok = packets_.Take(records, cursor, more);
        if (more) SetEvent(signal_);
        return ok;
    }
    DWORD Error() const { return packets_.Error(); }
    UsnPacketQueue::Stats Memory() const { return packets_.Memory(); }
private:
    friend struct UsnStreamTestAccess;
    explicit UsnStream(HANDLE signal) : signal_(signal), stop_(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}
    void Failed(DWORD error) { packets_.Fail(error); SetEvent(signal_); }
    void Read(wchar_t letter, uint64_t journal, int64_t cursor) {
        if (!stop_) { Failed(ERROR_NOT_ENOUGH_MEMORY); return; }
        const auto path = L"\\\\.\\" + std::wstring(1, letter) + L":";
        HANDLE volume = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        if (volume == INVALID_HANDLE_VALUE) { Failed(GetLastError()); return; }
        HANDLE completed = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!completed) { const auto error = GetLastError(); CloseHandle(volume); Failed(error); return; }
        READ_USN_JOURNAL_DATA_V0 request{};
        request.StartUsn = cursor; request.UsnJournalID = journal;
        request.ReasonMask = 0xffffffffu; request.BytesToWaitFor = 1;
        try {
            // This buffer is never moved into the queue and is reused only after
            // the previous I/O has completed (or cancellation has been drained).
            std::vector<BYTE> buffer(UsnPacketQueue::kReadBytes);
            while (WaitForSingleObject(stop_, 0) != WAIT_OBJECT_0) {
                OVERLAPPED operation{}; operation.hEvent = completed; ResetEvent(completed);
                DWORD received = 0;
                bool ok = DeviceIoControl(volume, FSCTL_READ_USN_JOURNAL, &request, sizeof(request),
                    buffer.data(), static_cast<DWORD>(buffer.size()), &received, &operation) != FALSE;
                if (!ok && GetLastError() == ERROR_IO_PENDING) {
                    DWORD error = ERROR_SUCCESS;
                    const auto result = AwaitUsnRead(volume, stop_, completed, operation, received, error);
                    if (result == UsnReadWait::Stopped) break;
                    if (result == UsnReadWait::Failed) { Failed(error); break; }
                    ok = true;
                }
                if (!ok) { Failed(GetLastError()); break; }
                if (received < sizeof(request.StartUsn) || received > buffer.size() ||
                    !ValidateUsnRecords(buffer.data() + sizeof(request.StartUsn), received - sizeof(request.StartUsn))) {
                    Failed(ERROR_INVALID_DATA); break;
                }
                const bool queued = packets_.Push(buffer.data(), received);
                SetEvent(signal_);
                if (!queued) break;
                std::memcpy(&request.StartUsn, buffer.data(), sizeof(request.StartUsn));
            }
        } catch (const std::bad_alloc&) {
            Failed(ERROR_NOT_ENOUGH_MEMORY);
        }
        CloseHandle(completed); CloseHandle(volume);
    }
    HANDLE signal_, stop_;
    UsnPacketQueue packets_;
    std::thread thread_;
};
}
