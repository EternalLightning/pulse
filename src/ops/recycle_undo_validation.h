#pragma once
#include <windows.h>
#include <string>

namespace pulse::ops {
struct RecycleUndoIdentity {
    FILE_ID_INFO payload{};
    FILE_ID_INFO index{};
};
// Read-only worker helpers. Pair validation is also usable with isolated fixture
// $R/$I files; production separately requires current-user recycle ownership.
bool IsCurrentUserRecyclePayload(const std::wstring& original, const std::wstring& payload);
bool ValidateRecycleUndoPair(const std::wstring& original, const std::wstring& payload,
    RecycleUndoIdentity& identity, const RecycleUndoIdentity* expected = nullptr);
}
