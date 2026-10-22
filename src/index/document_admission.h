#pragma once
#include <windows.h>
#include <functional>
#include <string>

namespace pulse::index::document {
inline constexpr unsigned kProcessSlots = 4;
// Verify JOB_LIST support before creating a parser so admission fails closed
// when the process attribute is unavailable.
inline constexpr DWORD_PTR kJobListAttribute = ProcThreadAttributeValue(13, FALSE, TRUE, FALSE);

class AdmissionHandle {
public:
    HANDLE value = INVALID_HANDLE_VALUE;
    AdmissionHandle() = default;
    AdmissionHandle(const AdmissionHandle&) = delete;
    AdmissionHandle& operator=(const AdmissionHandle&) = delete;
    ~AdmissionHandle() { Reset(); }
    void Reset(HANDLE next = INVALID_HANDLE_VALUE) noexcept {
        if (value != INVALID_HANDLE_VALUE) CloseHandle(value);
        value = next;
    }
};

inline HANDLE OpenAdmissionFile(const std::wstring& path, bool inheritable) {
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, inheritable ? TRUE : FALSE};
    return CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, &security,
        OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
}

// The slot handle is inherited by the parser, so it remains occupied until the
// actual parser exit even if its parent dies while the job is terminating it.
inline bool AcquireAdmission(const std::wstring& directory, AdmissionHandle& slot,
                             const std::function<bool()>& cancelled, DWORD& error) {
    AdmissionHandle waiting;
    const auto wait_path = directory + L"\\waiters.lock";
    for (;;) {
        if (cancelled && cancelled()) { error = ERROR_CANCELLED; return false; }
        // Serialize admission attempts as well as advertising demand. A reader
        // yielding a warm parser must not immediately steal its slot back from
        // the reader that caused it to yield.
        if (waiting.value == INVALID_HANDLE_VALUE) {
            waiting.Reset(OpenAdmissionFile(wait_path, false));
            if (waiting.value == INVALID_HANDLE_VALUE) {
                error = GetLastError();
                if (error != ERROR_SHARING_VIOLATION && error != ERROR_LOCK_VIOLATION) return false;
                Sleep(25);
                continue;
            }
        }
        for (unsigned i = 0; i < kProcessSlots; ++i) {
            slot.Reset(OpenAdmissionFile(directory + L"\\slot-" + std::to_wstring(i) + L".lock", true));
            if (slot.value != INVALID_HANDLE_VALUE) { error = ERROR_SUCCESS; return true; }
            error = GetLastError();
            if (error != ERROR_SHARING_VIOLATION && error != ERROR_LOCK_VIOLATION) return false;
        }
        Sleep(25);
    }
}

inline bool AdmissionContended(const std::wstring& directory) {
    AdmissionHandle probe;
    probe.Reset(OpenAdmissionFile(directory + L"\\waiters.lock", false));
    if (probe.value != INVALID_HANDLE_VALUE) return false;
    const auto error = GetLastError();
    return error == ERROR_SHARING_VIOLATION || error == ERROR_LOCK_VIOLATION;
}
}
