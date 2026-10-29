#pragma once
#include <windows.h>
#include <string>
#include <cstring>

namespace pulse::ops {
class DestinationGuard;
struct RecycleUndoIdentity {
    FILE_ID_INFO payload{};
    FILE_ID_INFO index{};
    bool operator==(const RecycleUndoIdentity& other) const {
        return payload.VolumeSerialNumber == other.payload.VolumeSerialNumber &&
            index.VolumeSerialNumber == other.index.VolumeSerialNumber &&
            std::memcmp(&payload.FileId, &other.payload.FileId, sizeof(payload.FileId)) == 0 &&
            std::memcmp(&index.FileId, &other.index.FileId, sizeof(index.FileId)) == 0;
    }
};
// Read-only worker helpers. Pair validation is also usable with isolated fixture
// $R/$I files; production separately requires current-user recycle ownership.
bool IsCurrentUserRecyclePayload(const std::wstring& original, const std::wstring& payload);
bool ValidateRecycleUndoPair(const std::wstring& original, const std::wstring& payload,
    RecycleUndoIdentity& identity, const RecycleUndoIdentity* expected = nullptr,
    bool require_empty_destination = true);
// Exact identity rename without replacing the destination. Also usable by isolated
// fixtures; production must separately check current-user recycle ownership.
// Retains the orphan index like the existing exact Undo backend.
bool RestoreRecyclePair(const std::wstring& original, const std::wstring& payload,
    const std::wstring& destination, const RecycleUndoIdentity& expected, DWORD& error,
    DestinationGuard* continuation_guard = nullptr);
}
