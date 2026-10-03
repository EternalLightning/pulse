#pragma once
#include <windows.h>
#include <string>
#include <string_view>
#include <vector>

namespace pulse::app {
inline constexpr wchar_t kThisPcParsingName[] = L"::{20D04FE0-3AEA-1069-A2D8-08002B30309D}";
struct DefaultManagerOptions {
    bool folders = false;
    bool win_e = false;
    bool this_pc = false;
};
struct DefaultManagerRegistryValue {
    bool exists = false;
    DWORD type = REG_NONE;
    std::vector<BYTE> data;
    bool operator==(const DefaultManagerRegistryValue&) const = default;
};
// Optional isolated storage, also allows deterministic access/error injection in tests.
class DefaultManagerRegistryStore {
public:
    virtual ~DefaultManagerRegistryStore() = default;
    virtual LSTATUS Read(const std::wstring& path, const std::wstring& name,
                         DefaultManagerRegistryValue& value) = 0;
    virtual LSTATUS Write(const std::wstring& path, const std::wstring& name,
                          const DefaultManagerRegistryValue& value) = 0;
    virtual LSTATUS Flush(const std::wstring& path) = 0;
    virtual LSTATUS RemoveTree(const std::wstring& path) = 0;
    virtual void RemoveEmpty(const std::wstring& path) = 0;
};
// An opened test key replaces HKCU; relative paths below remain identical.
struct DefaultManagerRegistry {
    HKEY root = HKEY_CURRENT_USER;
    bool notify_shell = true;
    DefaultManagerRegistryStore* store = nullptr;
};
struct DefaultManagerResult {
    LSTATUS error = ERROR_SUCCESS;
    bool external_change = false;
    bool changed = false;
    explicit operator bool() const { return error == ERROR_SUCCESS && !external_change; }
};
bool IsThisPcArgument(std::wstring_view argument);
bool DefaultManagerCommandIsOurs(std::wstring_view command, std::wstring_view executable);
DefaultManagerOptions ReadDefaultFileManager(const std::wstring& executable,
    DefaultManagerRegistry registry = {});
// Serialize calls. Invoke off the UI thread. The backup survives restart/uninstall.
DefaultManagerResult ApplyDefaultFileManager(const std::wstring& executable,
    DefaultManagerOptions options, DefaultManagerRegistry registry = {});
// The same restore operation is used by disabling and the pre-uninstall command.
DefaultManagerResult RestoreDefaultFileManager(const std::wstring& executable,
    DefaultManagerRegistry registry = {});
} // namespace pulse::app
