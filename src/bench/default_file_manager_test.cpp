#include "../app/default_file_manager.h"
#include "../app/explorer_window_takeover.h"
#include "../app/shell_window_registry.h"
#include "../app/shell_path.h"
#include <cstdio>
#include <vector>
#include <string>
#include <map>
#include <cstring>

namespace {
int failed = 0;
void Check(bool value, const char* name) {
    printf("[%s] %s\n", value ? "PASS" : "FAIL", name);
    if (!value) ++failed;
}
class MemoryRegistry final : public pulse::app::DefaultManagerRegistryStore {
public:
    using Value = pulse::app::DefaultManagerRegistryValue;
    using Address = std::pair<std::wstring, std::wstring>;
    std::map<Address, Value> values;
    std::wstring fail_path;
    int fail_writes = 0;
    std::wstring fail_name;
    LSTATUS Read(const std::wstring& path, const std::wstring& name, Value& value) override {
        const auto it = values.find({path, name});
        value = it == values.end() ? Value() : it->second;
        return ERROR_SUCCESS;
    }
    LSTATUS Write(const std::wstring& path, const std::wstring& name, const Value& value) override {
        if (path == fail_path && fail_writes > 0 && (fail_name.empty() || name == fail_name)) {
            --fail_writes; return ERROR_ACCESS_DENIED;
        }
        if (value.exists) values[{path, name}] = value;
        else values.erase({path, name});
        return ERROR_SUCCESS;
    }
    LSTATUS Flush(const std::wstring&) override { return ERROR_SUCCESS; }
    LSTATUS RemoveTree(const std::wstring& path) override {
        for (auto it = values.begin(); it != values.end();) {
            if (it->first.first == path || it->first.first.starts_with(path + L"\\")) it = values.erase(it);
            else ++it;
        }
        return ERROR_SUCCESS;
    }
    void RemoveEmpty(const std::wstring&) override {}
};
bool Put(MemoryRegistry& root, const std::wstring& path, const wchar_t* name, DWORD type,
         const void* data, DWORD bytes) {
    MemoryRegistry::Value value{true, type, std::vector<BYTE>(bytes)};
    if (bytes) memcpy(value.data.data(), data, bytes);
    return root.Write(path, name ? name : L"", value) == ERROR_SUCCESS;
}
bool PutText(MemoryRegistry& root, const std::wstring& path, const wchar_t* name, const wchar_t* text,
             DWORD type = REG_SZ) {
    return Put(root, path, name, type, text, static_cast<DWORD>((wcslen(text) + 1) * sizeof(wchar_t)));
}
bool HasText(MemoryRegistry& root, const std::wstring& path, const wchar_t* name, const wchar_t* expected,
             DWORD expected_type = REG_SZ) {
    MemoryRegistry::Value value;
    const size_t bytes = (wcslen(expected) + 1) * sizeof(wchar_t);
    return root.Read(path, name ? name : L"", value) == ERROR_SUCCESS && value.exists &&
        value.type == expected_type && value.data.size() == bytes &&
        memcmp(value.data.data(), expected, bytes) == 0;
}
}
int wmain() {
    using namespace pulse::app;
    const std::wstring exe = L"C:\\Program Files\\Pulse\\pulse.exe";
    Check(DefaultManagerCommandIsOurs(L"\"C:\\Program Files\\Pulse\\pulse.exe\" \"%1\"", exe), "quoted executable identity");
    Check(!DefaultManagerCommandIsOurs(L"\"C:\\Program Files\\Pulse\\pulse.exe.evil\"", exe), "reject executable prefix impersonation");
    Check(!DefaultManagerCommandIsOurs(L"\"C:\\Program Files\\Pulse\\pulse.exe", exe), "reject unterminated quote");
    Check(IsThisPcArgument(L" \"shell:::{20D04FE0-3AEA-1069-A2D8-08002B30309D}\\\" "), "This PC shell argument");
    Check(!IsThisPcArgument(L"C:\\"), "drive is not This PC");
    Check(SameShellPath(L"C:\\Folder\\file.txt", L"\\\\?\\c:\\folder\\file.txt"), "local normal and extended paths match");
    Check(SameShellPath(L"C:\\", L"\\\\?\\c:\\"), "extended drive roots match without drive-relative collapse");
    Check(!SameShellPath(L"C:", L"C:\\"), "drive-relative path is not a drive root");
    Check(SameShellPath(L"\\\\server\\share\\folder", L"\\\\?\\UNC\\SERVER\\share\\folder\\"), "UNC normal and extended folder paths match");
    Check(SameShellPath(L"\\\\server\\share\\", L"\\\\?\\UNC\\server\\share"), "UNC share roots match with optional trailing separator");
    Check(SameShellPath(L"", L"") && !SameShellPath(L"", L"C:\\"), "empty This PC stays distinct from filesystem paths");
    Check(ShellPathText(L"\\\\?\\UNC\\server\\share") == L"\\\\server\\share" &&
        ShellPathText(L"\\\\?\\C:\\folder") == L"C:\\folder", "Shell publication strips extended prefixes");
    Check(ShellPathIdentity(L"pulse:search:C:/text\\") == L"pulse:search:C:/text\\" &&
        ShellPathIdentity(kThisPcParsingName) == kThisPcParsingName, "virtual and parsing identities are not normalized as disk paths");
    ShellWindowRegistry registry_worker;
    ExplorerWindowTakeover takeover_worker;
    Check(!registry_worker.Start(nullptr, WM_APP + 17) && registry_worker.DrainSelections().empty(), "registry rejects invalid owner without COM startup");
    Check(!takeover_worker.Start(nullptr, WM_APP + 18) && takeover_worker.DrainRequests().empty(), "takeover rejects invalid owner without COM startup");
    Check(!takeover_worker.IsRequestActive(100), "unstarted takeover cannot deliver a stale token");
    Check(ShellSelectionFlagsNeedDelivery(0), "production Shell COM entrance forwards zero-valued deselect");
    takeover_worker.Acknowledge(100, true); takeover_worker.Stop(); registry_worker.Stop();

    // No user's registry key or real Shell window is accessed by this fixture.
    MemoryRegistry root;
    {
        const DefaultManagerRegistry fixture{nullptr, false, &root};
        const std::wstring this_pc = L"Software\\Classes\\CLSID\\{20D04FE0-3AEA-1069-A2D8-08002B30309D}\\shell";
        const std::wstring command = this_pc + L"\\open\\command";
        const std::wstring directory = L"Software\\Classes\\Directory\\shell";
        Check(PutText(root, command, nullptr, L"\"%OTHER_FM%\\other.exe\" /open", REG_EXPAND_SZ) &&
            PutText(root, command, L"DelegateExecute", L"{01234567-89AB-CDEF-0123-456789ABCDEF}") &&
            PutText(root, this_pc, nullptr, L"customverb") &&
            PutText(root, directory + L"\\open", L"DelegateExecute", L""), "seed prior manager and empty delegate");
        Check(static_cast<bool>(ApplyDefaultFileManager(exe, {true, true, true}, fixture)), "enable all associations");
        const auto state = ReadDefaultFileManager(exe, fixture);
        Check(state.folders && state.win_e && state.this_pc, "read complete association state");
        const auto repeated = ApplyDefaultFileManager(exe, {true, true, true}, fixture);
        Check(static_cast<bool>(repeated) && !repeated.changed, "repeated enable preserves backup without shell refresh");
        Check(static_cast<bool>(RestoreDefaultFileManager(exe, fixture)), "disable restores backups");
        Check(HasText(root, command, nullptr, L"\"%OTHER_FM%\\other.exe\" /open", REG_EXPAND_SZ) &&
            HasText(root, command, L"DelegateExecute", L"{01234567-89AB-CDEF-0123-456789ABCDEF}") &&
            HasText(root, this_pc, nullptr, L"customverb"), "restore This PC command type delegate and verb exactly");
        Check(HasText(root, directory + L"\\open", L"DelegateExecute", L""), "restore existing empty delegate");
        const auto off = ReadDefaultFileManager(exe, fixture);
        Check(!off.folders && !off.win_e && !off.this_pc, "absence restores inherited defaults");
        Check(static_cast<bool>(ApplyDefaultFileManager(exe, {false, false, true}, fixture)), "re-enable after restore");
        const std::wstring impostor = L"C:\\other\\pulse.exe";
        Check(ApplyDefaultFileManager(impostor, {false, false, true}, fixture).external_change,
            "backup owner prevents another executable reclaiming association");
        Check(PutText(root, command, nullptr, L"\"C:\\Other\\manager.exe\""), "simulate later third-party manager");
        Check(RestoreDefaultFileManager(exe, fixture).external_change &&
            HasText(root, command, nullptr, L"\"C:\\Other\\manager.exe\""), "uninstall preserves later manager");
        Check(PutText(root, command, nullptr, (L"\"" + exe + L"\" \"" + kThisPcParsingName + L"\"").c_str()) &&
            PutText(root, command, L"DelegateExecute", L"new-third-party-delegate"), "simulate independently changed delegate");
        Check(RestoreDefaultFileManager(exe, fixture).external_change &&
            HasText(root, command, L"DelegateExecute", L"new-third-party-delegate"), "restore preserves independently changed delegate");
        Check(static_cast<bool>(RestoreDefaultFileManager(exe, fixture)), "restore is idempotent after external delegate preservation");
        Check(PutText(root, command, nullptr, L"\"%OTHER_FM%\\other.exe\" /open", REG_EXPAND_SZ), "reset original for write-failure case");
        root.fail_path = command; root.fail_writes = 1;
        const auto interrupted = ApplyDefaultFileManager(exe, {false, false, true}, fixture);
        Check(interrupted.error == ERROR_ACCESS_DENIED &&
            HasText(root, command, nullptr, L"\"%OTHER_FM%\\other.exe\" /open", REG_EXPAND_SZ),
            "failed registration preserves original command");
        Check(static_cast<bool>(ApplyDefaultFileManager(exe, {false, false, true}, fixture)) &&
            static_cast<bool>(RestoreDefaultFileManager(exe, fixture)) &&
            HasText(root, command, nullptr, L"\"%OTHER_FM%\\other.exe\" /open", REG_EXPAND_SZ),
            "interrupted registration retries with original backup intact");
    }
    for (int changed_value = 0; changed_value < 4; ++changed_value) {
        MemoryRegistry isolated;
        const DefaultManagerRegistry fixture{nullptr, false, &isolated};
        const std::wstring shell = L"Software\\Classes\\CLSID\\{20D04FE0-3AEA-1069-A2D8-08002B30309D}\\shell";
        const std::wstring verb = shell + L"\\open";
        const std::wstring command = verb + L"\\command";
        Check(static_cast<bool>(ApplyDefaultFileManager(exe, {false, false, true}, fixture)), "seed association for independent value conflict");
        const std::wstring target = changed_value < 2 ? command : changed_value == 2 ? verb : shell;
        const wchar_t* name = changed_value == 1 || changed_value == 2 ? L"DelegateExecute" : L"";
        Check(PutText(isolated, target, name, L"third-party-independent-value"), "change command delegate or default verb independently");
        const auto before = isolated.values;
        const auto repeat = ApplyDefaultFileManager(exe, {false, false, true}, fixture);
        Check(repeat.external_change && !repeat.changed && isolated.values == before,
            "repeated enable does not overwrite any independently changed value or backup");
        Check(RestoreDefaultFileManager(exe, fixture).external_change &&
            HasText(isolated, target, name, L"third-party-independent-value"),
            "restore preserves independently changed value");
    }
    {
        MemoryRegistry isolated;
        const DefaultManagerRegistry fixture{nullptr, false, &isolated};
        const std::wstring command = L"Software\\Classes\\CLSID\\{20D04FE0-3AEA-1069-A2D8-08002B30309D}\\shell\\open\\command";
        Check(static_cast<bool>(ApplyDefaultFileManager(exe, {false, false, true}, fixture)), "seed delegation deletion conflict");
        isolated.Write(command, L"DelegateExecute", {});
        const auto repeat = ApplyDefaultFileManager(exe, {false, false, true}, fixture);
        MemoryRegistry::Value delegate;
        isolated.Read(command, L"DelegateExecute", delegate);
        Check(repeat.external_change && !repeat.changed && !delegate.exists,
            "repeated enable preserves independently deleted delegate even when original was absent");
    }
    {
        MemoryRegistry isolated;
        const DefaultManagerRegistry fixture{nullptr, false, &isolated};
        const std::wstring shell = L"Software\\Classes\\Directory\\shell";
        const std::wstring verb = shell + L"\\open";
        const std::wstring command = verb + L"\\command";
        Check(PutText(isolated, command, nullptr, (L"\"" + exe + L"\" \"%1\"").c_str()) &&
            PutText(isolated, verb, L"DelegateExecute", L"") && PutText(isolated, shell, nullptr, L"open") &&
            PutText(isolated, verb, L"CustomValue", L"keep"), "seed legacy association without Pulse backup");
        const auto restored = RestoreDefaultFileManager(exe, fixture);
        MemoryRegistry::Value value;
        isolated.Read(command, L"", value);
        Check(static_cast<bool>(restored) && restored.changed && !value.exists &&
            HasText(isolated, verb, L"CustomValue", L"keep"), "legacy restore removes only own override and retains unrelated verb value");
    }
    {
        MemoryRegistry isolated;
        const DefaultManagerRegistry fixture{nullptr, false, &isolated};
        const std::wstring shell = L"Software\\Classes\\CLSID\\{20D04FE0-3AEA-1069-A2D8-08002B30309D}\\shell";
        const std::wstring command = shell + L"\\open\\command";
        Check(PutText(isolated, command, nullptr, L"original.exe") &&
            PutText(isolated, command, L"DelegateExecute", L"original-delegate") &&
            static_cast<bool>(ApplyDefaultFileManager(exe, {false, false, true}, fixture)),
            "seed interrupted restore with nonempty originals");
        isolated.fail_path = command; isolated.fail_name = L"DelegateExecute"; isolated.fail_writes = 1;
        const auto interrupted = RestoreDefaultFileManager(exe, fixture);
        Check(interrupted.error == ERROR_ACCESS_DENIED && interrupted.changed &&
            HasText(isolated, command, nullptr, L"original.exe"), "partial restore reports mutation despite later delegate write failure");
        Check(static_cast<bool>(RestoreDefaultFileManager(exe, fixture)) &&
            HasText(isolated, command, L"DelegateExecute", L"original-delegate"), "partial restore retries remaining backup values");
    }
    {
        MemoryRegistry isolated;
        const DefaultManagerRegistry fixture{nullptr, false, &isolated};
        const std::wstring shell = L"Software\\Classes\\CLSID\\{20D04FE0-3AEA-1069-A2D8-08002B30309D}\\shell";
        const std::wstring verb = shell + L"\\open";
        const std::wstring command = verb + L"\\command";
        Check(PutText(isolated, command, nullptr, (L"\"" + exe + L"\" \"" + kThisPcParsingName + L"\"").c_str()) &&
            PutText(isolated, command, L"DelegateExecute", L"legacy-delegate"),
            "seed incomplete legacy Pulse command with original delegate");
        isolated.fail_path = verb; isolated.fail_name = L"DelegateExecute"; isolated.fail_writes = 1;
        const auto failed_apply = ApplyDefaultFileManager(exe, {false, false, true}, fixture);
        Check(failed_apply.error == ERROR_ACCESS_DENIED && HasText(isolated, command, L"DelegateExecute", L"legacy-delegate"),
            "legacy repair failure rolls back original delegate while command still equals Pulse");
        Check(static_cast<bool>(ApplyDefaultFileManager(exe, {false, false, true}, fixture)) &&
            ReadDefaultFileManager(exe, fixture).this_pc,
            "incomplete legacy repair retries rather than reporting false external conflict");
        Check(static_cast<bool>(RestoreDefaultFileManager(exe, fixture)) &&
            HasText(isolated, command, L"DelegateExecute", L"legacy-delegate"),
            "repaired legacy delegate is restored from the preserved original backup");
    }
    return failed ? 1 : 0;
}
