// SPDX-License-Identifier: Apache-2.0

#include "../common/file_dialog_registration.h"

#include <cstdio>
#include <filesystem>

namespace {

namespace bridge = pulse::dialog_bridge;
namespace fs = std::filesystem;

constexpr wchar_t kOpen[] =
    L"Software\\Classes\\CLSID\\{DC1C5A9C-E88A-4DDE-A5A1-60F82A20AEF7}";
constexpr wchar_t kSave[] =
    L"Software\\Classes\\CLSID\\{C0B4E2F3-BA21-4773-8DBA-335EC946EB8B}";
constexpr wchar_t k32Bit[] =
    L"Software\\Classes\\Wow6432Node\\CLSID\\{DC1C5A9C-E88A-4DDE-A5A1-60F82A20AEF7}";

struct Key {
    HKEY handle = nullptr;
    ~Key() { if (handle) RegCloseKey(handle); }
};

struct Hive {
    fs::path directory;
    Key key;
    bool overridden = false;

    ~Hive() {
        if (overridden) RegOverridePredefKey(HKEY_CURRENT_USER, nullptr);
        if (key.handle) {
            RegCloseKey(key.handle);
            key.handle = nullptr;
        }
        if (!directory.empty()) {
            std::error_code error;
            fs::remove_all(directory, error);
        }
    }

    DWORD Open() {
        const fs::path parent = fs::absolute(L"bench_data");
        const auto fixture = parent / (L"file-dialog-registration-" + std::to_wstring(GetCurrentProcessId()) +
            L"-" + std::to_wstring(GetTickCount64()));
        fs::create_directories(parent);
        if (!fs::create_directory(fixture)) return ERROR_ALREADY_EXISTS;
        directory = fixture;
        const DWORD loaded = RegLoadAppKeyW((directory / L"isolated.hiv").c_str(), &key.handle,
            KEY_ALL_ACCESS, REG_PROCESS_APPKEY, 0);
        if (loaded) return loaded;
        Key marker;
        const DWORD marked = RegCreateKeyExW(key.handle, L"PulseIsolatedRegistry", 0, nullptr, 0,
            KEY_READ, nullptr, &marker.handle, nullptr);
        if (marked) return marked;
        const DWORD redirected = RegOverridePredefKey(HKEY_CURRENT_USER, key.handle);
        if (redirected) return redirected;
        overridden = true;
        Key detected;
        return RegOpenKeyExW(HKEY_CURRENT_USER, L"PulseIsolatedRegistry", 0, KEY_READ, &detected.handle);
    }
};

bool Exists(HKEY hive, const wchar_t* path) {
    Key key;
    return RegOpenKeyExW(hive, path, 0, KEY_READ | bridge::RegistryView, &key.handle) == ERROR_SUCCESS;
}

DWORD SetString(HKEY key, const wchar_t* name, const std::wstring& value) {
    return RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
        static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
}

} // namespace

int wmain(int argc, wchar_t** argv) try {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc != 2) {
        std::puts("Usage: pulse_file_dialog_registration_test <absolute sibling bridge DLL>");
        return 2;
    }
    Hive hive;
    if (const DWORD error = hive.Open()) {
        std::printf("[FAIL] redirect HKCU to disposable file-backed hive (%lu)\n", error);
        return 2;
    }
    const HKEY root = hive.key.handle;
    const std::wstring dll(argv[1]);
    const std::wstring other = L"C:\\OtherPulseInstallation\\" + std::wstring(bridge::DllName);
    int failures = 0;
    const auto check = [&](bool condition, const char* label) {
        std::printf("[%s] %s\n", condition ? "PASS" : "FAIL", label);
        if (!condition) ++failures;
    };

    Key alternate, foreign;
    check(RegCreateKeyExW(root, k32Bit, 0, nullptr, 0, KEY_ALL_ACCESS, nullptr,
        &alternate.handle, nullptr) == ERROR_SUCCESS, "create alternate-view sentinel");
    check(RegCreateKeyExW(root, kOpen, 0, nullptr, 0, KEY_ALL_ACCESS | bridge::RegistryView,
        nullptr, &foreign.handle, nullptr) == ERROR_SUCCESS &&
        SetString(foreign.handle, L"ExistingAssociation", L"external") == ERROR_SUCCESS,
        "create unrelated association in the isolated hive");
    check(bridge::Inspect().conflict && bridge::Enable(dll) == ERROR_ALREADY_EXISTS &&
        !Exists(root, kSave) && !Exists(root, bridge::ConfigKey),
        "foreign CLSID prevents any registration or journal write");
    check(RegDeleteTreeW(root, kOpen) == ERROR_SUCCESS, "remove foreign fixture");
    check(bridge::Enable(other) == ERROR_INVALID_NAME &&
        bridge::DisableFor(L"C:\\dir\\..\\" + std::wstring(bridge::DllName)) == ERROR_INVALID_NAME,
        "reject unrelated and traversal DLL paths");

    check(bridge::Enable(dll) == ERROR_SUCCESS && bridge::Enable(dll) == ERROR_SUCCESS &&
        bridge::IsEnabledFor(dll) && bridge::IsEnabledFor(LR"(\\?\)" + dll),
        "enable is idempotent and DOS and extended prefixes identify one installation");
    check(bridge::DisableFor(other) == ERROR_SUCCESS && bridge::IsEnabledFor(dll),
        "uninstall of another installation preserves active registrations");
    check(RegDeleteTreeW(root, kSave) == ERROR_SUCCESS && !bridge::Inspect().enabled &&
        bridge::Enable(dll) == ERROR_SUCCESS && bridge::IsEnabledFor(dll),
        "retry repairs an interrupted two-class registration");

    const std::wstring open_server = std::wstring(kOpen) + L"\\InprocServer32";
    Key server;
    const DWORD opened = RegOpenKeyExW(root, open_server.c_str(), 0,
        KEY_READ | KEY_WRITE | bridge::RegistryView, &server.handle);
    check(opened == ERROR_SUCCESS && SetString(server.handle, L"ExternalValue", L"untouched") == ERROR_SUCCESS,
        "create external value in owned subtree");
    check(bridge::DisableFor(dll) == ERROR_ALREADY_EXISTS && bridge::Inspect().conflict &&
        Exists(root, kSave) && Exists(root, open_server.c_str()),
        "disable preserves both registrations when one contains a foreign value");
    check(server.handle && RegDeleteValueW(server.handle, L"ExternalValue") == ERROR_SUCCESS &&
        bridge::DisableFor(LR"(\\?\)" + dll) == ERROR_SUCCESS &&
        !Exists(root, kOpen) && !Exists(root, kSave) && Exists(root, k32Bit),
        "owned uninstall removes its x64 entries and preserves the alternate view");

    check(bridge::Enable(dll) == ERROR_SUCCESS, "establish isolated registration for orphaned-reference check");
    check(RegDeleteTreeW(root, bridge::ConfigKey) == ERROR_SUCCESS &&
        bridge::DisableFor(dll) == ERROR_ALREADY_EXISTS && bridge::DisableFor(other) == ERROR_SUCCESS &&
        Exists(root, kOpen) && Exists(root, kSave),
        "missing journal blocks uninstall of the referenced DLL and preserves a separate installation");
    return failures != 0;
} catch (const std::exception& error) {
    std::fprintf(stderr, "[FAIL] isolated registration fixture: %s\n", error.what());
    return 2;
}
