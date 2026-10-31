// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

// Registry-only verification of production value transactions. No Shell or UI calls.
#include "../app/system_ctx_registry.h"

#include <winternl.h>

#include <cstdio>
#include <exception>
#include <string>
#include <utility>
#include <vector>

using namespace pulse::app::sysctx;

namespace {

int failures = 0;

void Check(bool ok, const char* message) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", message);
    if (!ok) ++failures;
}

struct RawValue {
    bool present = false;
    DWORD type = REG_NONE;
    std::vector<BYTE> data;
    bool operator==(const RawValue&) const = default;
};

bool ReadRaw(HKEY key, const wchar_t* name, RawValue& value) {
    value = {};
    DWORD bytes = 0;
    LSTATUS status = RegQueryValueExW(key, name, nullptr, &value.type, nullptr, &bytes);
    if (status == ERROR_FILE_NOT_FOUND) { value.type = REG_NONE; return true; }
    if (status != ERROR_SUCCESS || bytes > 64 * 1024) return false;
    value.present = true;
    value.data.resize(bytes);
    status = RegQueryValueExW(key, name, nullptr, &value.type,
                              value.data.empty() ? nullptr : value.data.data(), &bytes);
    if (status != ERROR_SUCCESS || bytes > value.data.size()) return false;
    value.data.resize(bytes);
    return true;
}

bool ReadMarkers(HKEY key, std::vector<RawValue>& values) {
    values.clear();
    for (const wchar_t* name : {L"HideBasedOnVelocityId", L"LegacyDisable", L"ProgrammaticAccessOnly", L"CommandFlags"}) {
        RawValue value;
        if (!ReadRaw(key, name, value)) return false;
        values.push_back(std::move(value));
    }
    return true;
}

bool SetNative(HKEY key, const wchar_t* name, DWORD type, const std::vector<BYTE>& bytes) {
    using SetValue = NTSTATUS (NTAPI*)(HANDLE, PUNICODE_STRING, ULONG, ULONG, PVOID, ULONG);
    static const auto set_value = reinterpret_cast<SetValue>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtSetValueKey"));
    if (!set_value) return false;
    const auto length = static_cast<USHORT>(wcslen(name) * sizeof(wchar_t));
    UNICODE_STRING unicode{length, static_cast<USHORT>(length + sizeof(wchar_t)), const_cast<wchar_t*>(name)};
    return set_value(key, &unicode, 0, type, bytes.empty() ? nullptr : const_cast<BYTE*>(bytes.data()),
                      static_cast<ULONG>(bytes.size())) >= 0;
}

bool SetWin32(HKEY key, const wchar_t* name, DWORD type, const std::vector<BYTE>& bytes) {
    auto buffer = bytes;
    buffer.resize(buffer.size() + sizeof(wchar_t), 0);
    return RegSetValueExW(key, name, 0, type, buffer.data(), static_cast<DWORD>(bytes.size())) == ERROR_SUCCESS;
}

struct Fixture {
    std::wstring path;
    HKEY key = nullptr;
    bool owns_root = false;

    bool Create() {
        FILETIME time{};
        GetSystemTimeAsFileTime(&time);
        const ULONGLONG stamp = (static_cast<ULONGLONG>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
        path = L"Software\\PulseTest\\SystemCtxTransaction_" + std::to_wstring(GetCurrentProcessId()) + L"_" +
               std::to_wstring(stamp) + L"_" + std::to_wstring(GetTickCount64());
        HKEY root = nullptr;
        DWORD disposition = 0;
        LSTATUS status = RegCreateKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, nullptr, 0,
                                         KEY_READ | KEY_WRITE | DELETE, nullptr, &root, &disposition);
        if (status != ERROR_SUCCESS) {
            std::printf("[FAIL] fixture create root error=%ld path=%ls\n", status, path.c_str());
            return false;
        }
        if (disposition != REG_CREATED_NEW_KEY) {
            RegCloseKey(root);
            std::printf("[FAIL] fixture root already exists; refusing to reuse it\n");
            return false;
        }
        owns_root = true;
        status = RegCreateKeyExW(root, L"Classes\\*\\shell\\Probe", 0, nullptr, 0,
                                 KEY_READ | KEY_WRITE, nullptr, &key, nullptr);
        RegCloseKey(root);
        if (status != ERROR_SUCCESS) {
            std::printf("[FAIL] fixture create verb error=%ld\n", status);
            return false;
        }
        std::printf("[INIT] isolated registry=HKCU\\%ls\n", path.c_str());
        return true;
    }

    bool Cleanup() {
        if (key) { RegCloseKey(key); key = nullptr; }
        if (!owns_root) return true;
        const LSTATUS status = RegDeleteTreeW(HKEY_CURRENT_USER, path.c_str());
        if (status != ERROR_SUCCESS && status != ERROR_FILE_NOT_FOUND) {
            std::printf("[FAIL] fixture cleanup error=%ld path=%ls\n", status, path.c_str());
            return false;
        }
        owns_root = false;
        return true;
    }

    ~Fixture() {
        Cleanup();
        SetVelocitySupportedForTesting(-1);
    }
};

void PrintValue(const char* label, const RawValue& value, const std::vector<BYTE>& requested) {
    std::printf(" %s(type=%lu,bytes=%zu,exact=%d,data=", label, value.type, value.data.size(),
                value.present && value.type == REG_SZ && value.data == requested);
    for (const BYTE byte : value.data) std::printf("%02x", static_cast<unsigned>(byte));
    std::printf(")");
}

int Run() {
    Fixture fixture;
    if (!fixture.Create()) return 2;
    Roots roots;
    roots.user = HKEY_CURRENT_USER;
    roots.user_classes = fixture.path + L"\\Classes";
    roots.machine = HKEY_CURRENT_USER;
    roots.machine_classes = fixture.path + L"\\UnusedMachine";
    roots.file_exts.clear();
    roots.user_trash = fixture.path + L"\\Trash";
    roots.machine_trash = fixture.path + L"\\UnusedTrash";
    roots.resolve_strings = false;
    roots.check_owner = false;
    roots.value_write_fault_for_testing = std::make_shared<ValueWriteFaultForTesting>();
    auto& fault = *roots.value_write_fault_for_testing;
    SetVelocitySupportedForTesting(1);
    const std::wstring verb = L"*\\shell\\Probe";

    std::vector<RawValue> missing, actual;
    if (!ReadMarkers(fixture.key, missing)) return 2;
    Op disable{Action::Disable, Kind::Verb, Root::User, verb};
    PlanOp(roots, disable);
    fault.fail_write_call = 2;
    const OpResult disable_result = Apply(roots, disable);
    Check(disable.planned && disable.plan_error == ERROR_SUCCESS && disable_result == OpResult::Failed &&
              fault.write_calls == 2 && fault.rollback_calls == 2 && ReadMarkers(fixture.key, actual) && actual == missing,
          "second disable write failure restores the original missing marker values");

    const std::vector<RawValue> original = {
        {true, REG_DWORD, {0x78, 0x56, 0x34, 0x12}},
        {true, REG_BINARY, {0x10, 0x00, 0xFF}},
        {true, REG_SZ, {'x', 0, 0, 0, 'z', 0}},
        {true, REG_DWORD, {0x28, 0x01, 0x00, 0x00}}};
    bool seeded = true;
    size_t index = 0;
    for (const wchar_t* name : {L"HideBasedOnVelocityId", L"LegacyDisable", L"ProgrammaticAccessOnly", L"CommandFlags"}) {
        const auto& value = original[index++];
        seeded = SetNative(fixture.key, name, value.type, value.data) && seeded;
    }
    const bool seeded_exactly = seeded && ReadMarkers(fixture.key, actual) && actual == original;
    Check(seeded_exactly, "native fixture stores noncanonical marker types and unterminated bytes exactly");
    if (!seeded_exactly) return 1;

    Op enable{Action::Enable, Kind::Verb, Root::User, verb};
    PlanOp(roots, enable);
    fault = {};
    fault.fail_write_call = 2;
    const OpResult enable_failed = Apply(roots, enable);
    Check(enable.planned && enable.has_previous_values && enable_failed == OpResult::Failed &&
              fault.write_calls == 2 && fault.rollback_calls == 2 && ReadMarkers(fixture.key, actual) && actual == original,
          "second enable deletion failure rolls back exact raw types, bytes and presence");

    std::vector<RawValue> enabled(4);
    enabled[3] = {true, REG_DWORD, {0x20, 0x01, 0x00, 0x00}};
    fault = {};
    const OpResult enable_result = Apply(roots, enable);
    Check(enable_result == OpResult::Ok && ReadMarkers(fixture.key, actual) && actual == enabled,
          "production Apply enables the verb and preserves unrelated CommandFlags bits");
    const Op inverse = Inverse(enable);
    fault = {};
    fault.fail_write_call = 2;
    const OpResult inverse_failed = Apply(roots, inverse);
    Check(inverse_failed == OpResult::Failed && fault.write_calls == 2 && fault.rollback_calls == 2 &&
              ReadMarkers(fixture.key, actual) && actual == enabled,
          "failed undo restores the exact state from the beginning of that undo attempt");
    fault = {};
    Check(Apply(roots, inverse) == OpResult::Ok && ReadMarkers(fixture.key, actual) && actual == original,
          "the same recorded inverse can be retried after an undo write failure");

    fault = {};
    fault.fail_write_call = 2;
    fault.fail_rollback_call = 2;
    const OpResult recovery = Apply(roots, enable);
    auto partial = original;
    partial[0] = {};
    Check(recovery == OpResult::RecoveryRequired && fault.write_calls == 2 && fault.rollback_calls == 2 &&
              ReadMarkers(fixture.key, actual) && actual == partial,
          "a rollback failure reports RecoveryRequired with the remaining partial change visible");
    fault = {};
    Check(Apply(roots, inverse) == OpResult::Ok && ReadMarkers(fixture.key, actual) && actual == original,
          "the original inverse repairs a RecoveryRequired partial transaction");

    const RawValue original_position{true, REG_BINARY, {0x01, 0x00, 0xFE}};
    const bool position_seeded = SetNative(fixture.key, L"Position", original_position.type, original_position.data);
    Op position{Action::Position, Kind::Verb, Root::User, verb};
    position.place = Place::Top;
    PlanOp(roots, position);
    fault = {};
    fault.fail_write_call = 1;
    RawValue stored_position;
    Check(position_seeded && Apply(roots, position) == OpResult::Failed && fault.rollback_calls == 1 &&
              ReadRaw(fixture.key, L"Position", stored_position) && stored_position == original_position,
          "Position write failure preserves the original binary value");
    fault = {};
    Check(Apply(roots, position) == OpResult::Ok && Apply(roots, Inverse(position)) == OpResult::Ok &&
              ReadRaw(fixture.key, L"Position", stored_position) && stored_position == original_position,
          "Position can be applied and undone through its exact original snapshot");

    bool compared = true;
    for (const std::vector<BYTE>& requested : std::vector<std::vector<BYTE>>{{'x', 0, 'y', 0}, {'x', 0, 0, 0, 'z', 0}, {}}) {
        const bool win32_written = SetWin32(fixture.key, L"Win32String", REG_SZ, requested);
        const bool native_written = SetNative(fixture.key, L"NativeString", REG_SZ, requested);
        RawValue win32_value, native_value;
        const bool read = ReadRaw(fixture.key, L"Win32String", win32_value) && ReadRaw(fixture.key, L"NativeString", native_value);
        std::printf("[DIAG] raw string APIs requested=%zu", requested.size());
        PrintValue("RegSetValueExW", win32_value, requested);
        PrintValue("NtSetValueKey", native_value, requested);
        std::printf("\n");
        compared = win32_written && native_written && read && win32_value.present && win32_value.type == REG_SZ &&
                   native_value == RawValue{true, REG_SZ, requested} && compared;
    }
    Check(compared, "compare actual Win32/native stored strings; native writes preserve the requested byte count and data");
    Check(fixture.Cleanup(), "remove only this test's owned isolated registry root");
    std::printf("%s (%d failures)\n", failures ? "FAILED" : "ALL PASSED", failures);
    return failures ? 1 : 0;
}

} // namespace

int main() {
    try {
        return Run();
    } catch (const std::exception& error) {
        std::printf("[FAIL] exception: %s\n", error.what());
        return 1;
    }
}
