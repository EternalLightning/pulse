#include "default_file_manager.h"
#include <shlobj.h>
#include <cstring>
#include <cwctype>

namespace pulse::app {
namespace {
constexpr wchar_t kBackup[] = L"Software\\Pulse\\DefaultManagerBackup\\v1\\";
struct Key {
    HKEY h = nullptr;
    ~Key() { if (h) RegCloseKey(h); }
};
using Value = DefaultManagerRegistryValue;
struct Change { std::wstring path; std::wstring name; Value wanted; };
struct Group { const wchar_t* id; std::wstring verb; std::vector<Change> changes; };
Value Text(std::wstring_view text) {
    Value v{true, REG_SZ, std::vector<BYTE>((text.size() + 1) * sizeof(wchar_t), 0)};
    if (!text.empty()) memcpy(v.data.data(), text.data(), text.size() * sizeof(wchar_t));
    return v;
}
std::wstring AsText(const Value& v) {
    if (!v.exists || (v.type != REG_SZ && v.type != REG_EXPAND_SZ) ||
        v.data.size() % sizeof(wchar_t)) return {};
    std::wstring s(v.data.size() / sizeof(wchar_t), L'\0');
    if (!s.empty()) memcpy(s.data(), v.data.data(), v.data.size());
    while (!s.empty() && s.back() == L'\0') s.pop_back();
    if (s.find(L'\0') != std::wstring::npos) return {};
    return s;
}
LSTATUS Read(DefaultManagerRegistry r, const std::wstring& path, const std::wstring& name, Value& v) {
    v = {};
    if (r.store) return r.store->Read(path, name, v);
    Key key;
    LSTATUS e = RegOpenKeyExW(r.root, path.c_str(), 0, KEY_QUERY_VALUE, &key.h);
    if (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND) return ERROR_SUCCESS;
    if (e != ERROR_SUCCESS) return e;
    DWORD bytes = 0;
    e = RegQueryValueExW(key.h, name.c_str(), nullptr, &v.type, nullptr, &bytes);
    if (e == ERROR_FILE_NOT_FOUND) return ERROR_SUCCESS;
    if (e != ERROR_SUCCESS) return e;
    if (bytes > 1024 * 1024) return ERROR_INVALID_DATA;
    v.data.resize(bytes);
    e = RegQueryValueExW(key.h, name.c_str(), nullptr, &v.type, v.data.data(), &bytes);
    if (e == ERROR_SUCCESS) { v.exists = true; v.data.resize(bytes); }
    return e;
}
LSTATUS Write(DefaultManagerRegistry r, const std::wstring& path, const std::wstring& name, const Value& v) {
    if (r.store) return r.store->Write(path, name, v);
    Key key;
    LSTATUS e = v.exists
        ? RegCreateKeyExW(r.root, path.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key.h, nullptr)
        : RegOpenKeyExW(r.root, path.c_str(), 0, KEY_SET_VALUE, &key.h);
    if (!v.exists && (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND)) return ERROR_SUCCESS;
    if (e != ERROR_SUCCESS) return e;
    e = v.exists ? RegSetValueExW(key.h, name.c_str(), 0, v.type, v.data.data(),
        static_cast<DWORD>(v.data.size())) : RegDeleteValueW(key.h, name.c_str());
    return e == ERROR_FILE_NOT_FOUND ? ERROR_SUCCESS : e;
}
std::vector<Group> Groups(const std::wstring& exe) {
    std::vector<Group> groups;
    const std::wstring shell = L"Software\\Classes\\";
    const std::wstring folder_command = L"\"" + exe + L"\" \"%1\"";
    auto add = [&](const wchar_t* id, const std::wstring& base, const wchar_t* verb,
                   const std::wstring& command, bool default_verb) {
        Group g{id, base + L"\\shell\\" + verb, {}};
        g.changes.push_back({g.verb + L"\\command", L"", Text(command)});
        // Shell versions consult either location for DelegateExecute.
        g.changes.push_back({g.verb + L"\\command", L"DelegateExecute", Text(L"")});
        g.changes.push_back({g.verb, L"DelegateExecute", Text(L"")});
        if (default_verb) g.changes.push_back({base + L"\\shell", L"", Text(verb)});
        groups.push_back(std::move(g));
    };
    add(L"Directory", shell + L"Directory", L"open", folder_command, true);
    add(L"Drive", shell + L"Drive", L"open", folder_command, true);
    add(L"WinE", shell + L"CLSID\\{52205fd8-5dfb-447d-801a-d0b52f2e83e1}",
        L"opennewwindow", L"\"" + exe + L"\"", false);
    add(L"ThisPC", shell + L"CLSID\\{20D04FE0-3AEA-1069-A2D8-08002B30309D}",
        L"open", L"\"" + exe + L"\" \"" + kThisPcParsingName + L"\"", true);
    return groups;
}
LSTATUS Save(DefaultManagerRegistry root, const std::wstring& backup, size_t index, const Value& v) {
    Value encoded{true, REG_BINARY, std::vector<BYTE>(8 + v.data.size())};
    const DWORD exists = v.exists ? 1 : 0;
    memcpy(encoded.data.data(), &exists, 4);
    memcpy(encoded.data.data() + 4, &v.type, 4);
    if (!v.data.empty()) memcpy(encoded.data.data() + 8, v.data.data(), v.data.size());
    return Write(root, backup, std::to_wstring(index), encoded);
}
LSTATUS Load(DefaultManagerRegistry root, const std::wstring& backup, size_t index, Value& v) {
    Value encoded;
    LSTATUS e = Read(root, backup, std::to_wstring(index), encoded);
    if (e != ERROR_SUCCESS) return e;
    if (!encoded.exists || encoded.type != REG_BINARY || encoded.data.size() < 8) return ERROR_INVALID_DATA;
    DWORD exists = 0;
    memcpy(&exists, encoded.data.data(), 4);
    if (exists > 1) return ERROR_INVALID_DATA;
    v.exists = exists != 0;
    memcpy(&v.type, encoded.data.data() + 4, 4);
    v.data.assign(encoded.data.begin() + 8, encoded.data.end());
    return ERROR_SUCCESS;
}
void DeleteEmpty(DefaultManagerRegistry r, const std::wstring& path) {
    if (r.store) { r.store->RemoveEmpty(path); return; }
    Key key;
    if (RegOpenKeyExW(r.root, path.c_str(), 0, KEY_QUERY_VALUE | KEY_ENUMERATE_SUB_KEYS, &key.h) != ERROR_SUCCESS) return;
    DWORD subkeys = 0, values = 0;
    if (RegQueryInfoKeyW(key.h, nullptr, nullptr, nullptr, &subkeys, nullptr, nullptr,
        &values, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS && !subkeys && !values)
        RegDeleteKeyW(r.root, path.c_str());
}
DefaultManagerResult ApplyGroup(const std::wstring& exe, const Group& g, bool enabled, DefaultManagerRegistry root) {
    const std::wstring backup = std::wstring(kBackup) + g.id;
    Value owner;
    LSTATUS e = Read(root, backup, L"Owner", owner);
    if (e != ERROR_SUCCESS) return {e};
    if (owner.exists && AsText(owner) != exe) return {ERROR_SUCCESS, true};
    std::vector<Value> before(g.changes.size());
    for (size_t i = 0; i < before.size(); ++i) {
        e = Read(root, g.changes[i].path, g.changes[i].name, before[i]);
        if (e != ERROR_SUCCESS) return {e};
    }
    if (enabled) {
        if (owner.exists) {
            Value pending;
            e = Read(root, backup, L"Pending", pending);
            if (e != ERROR_SUCCESS) return {e};
            if (pending.exists && AsText(pending) != L"1") return {ERROR_INVALID_DATA};
            // A third party can change the delegate or default verb independently
            // of the command. Never reclaim such a group on repeated enable.
            // A failed apply rolls the command back first, so originals are
            // retryable only while that command has not been installed. Once
            // installed, even an independently restored/deleted delegate is a
            // conflict; silently writing the empty Pulse delegate would steal it.
            const bool installed = !pending.exists && before[0] == g.changes[0].wanted;
            for (size_t i = 0; i < before.size(); ++i) {
                Value original;
                e = Load(root, backup, i, original);
                if (e != ERROR_SUCCESS) return {e};
                if (before[i] != g.changes[i].wanted && (installed || before[i] != original))
                    return {ERROR_SUCCESS, true};
            }
        }
        if (!owner.exists) {
            for (size_t i = 0; i < before.size(); ++i) {
                // Legacy Pulse has no reversible backups. Drop only its known values.
                Value original = before[i];
                if (DefaultManagerCommandIsOurs(AsText(before[0]), exe) &&
                    (i == 0 || before[i] == g.changes[i].wanted)) original = {};
                e = Save(root, backup, i, original);
                if (e != ERROR_SUCCESS) return {e};
            }
            // A legacy command can already equal the desired command before its
            // delegates are repaired. Persist the incomplete transaction instead
            // of treating that command equality as a committed registration.
            e = Write(root, backup, L"Pending", Text(L"1"));
            if (e != ERROR_SUCCESS) return {e};
            e = Write(root, backup, L"Owner", Text(exe));
            if (e != ERROR_SUCCESS) return {e};
            if (root.store) e = root.store->Flush(backup);
            else {
                Key key;
                e = RegOpenKeyExW(root.root, backup.c_str(), 0, KEY_QUERY_VALUE, &key.h);
                if (e == ERROR_SUCCESS) e = RegFlushKey(key.h);
            }
            if (e != ERROR_SUCCESS) return {e};
        }
        bool changed = false;
        for (size_t i = 0; i < before.size(); ++i) {
            if (before[i] == g.changes[i].wanted) continue;
            e = Write(root, g.changes[i].path, g.changes[i].name, g.changes[i].wanted);
            if (e != ERROR_SUCCESS) {
                for (size_t j = 0; j < i; ++j) Write(root, g.changes[j].path, g.changes[j].name, before[j]);
                bool residual = false;
                for (size_t j = 0; j < i; ++j) {
                    Value current;
                    residual = Read(root, g.changes[j].path, g.changes[j].name, current) != ERROR_SUCCESS ||
                        current != before[j] || residual;
                }
                return {e, false, residual};
            }
            changed = true;
        }
        e = Write(root, backup, L"Pending", {});
        if (e != ERROR_SUCCESS) return {e, false, changed};
        return {ERROR_SUCCESS, false, changed};
    }
    if (!owner.exists) {
        // Older Pulse versions wrote associations without a backup. Only clear
        // a command that still points to this executable and its known values.
        if (!DefaultManagerCommandIsOurs(AsText(before[0]), exe)) return {};
        bool changed = false, external = false;
        for (size_t i = 0; i < before.size(); ++i) {
            if (!before[i].exists) continue;
            if (i != 0 && before[i] != g.changes[i].wanted) { external = true; continue; }
            e = Write(root, g.changes[i].path, g.changes[i].name, {});
            if (e != ERROR_SUCCESS) return {e, external, changed};
            changed = true;
        }
        DeleteEmpty(root, g.verb + L"\\command");
        DeleteEmpty(root, g.verb);
        DeleteEmpty(root, g.verb.substr(0, g.verb.rfind(L'\\')));
        return {ERROR_SUCCESS, external, changed};
    }
    std::vector<Value> originals(before.size());
    for (size_t i = 0; i < before.size(); ++i) {
        e = Load(root, backup, i, originals[i]);
        if (e != ERROR_SUCCESS) return {e};
    }
    // Also accept the original command after a partially completed restore.
    if (!DefaultManagerCommandIsOurs(AsText(before[0]), exe) && before[0] != originals[0])
        return {ERROR_SUCCESS, true};
    bool external = false;
    bool changed = false;
    for (size_t i = 0; i < before.size(); ++i) {
        if (before[i] == originals[i]) continue;
        if (before[i] != g.changes[i].wanted) { external = true; continue; }
        e = Write(root, g.changes[i].path, g.changes[i].name, originals[i]);
        if (e != ERROR_SUCCESS) return {e, external, changed};
        changed = true;
    }
    e = root.store ? root.store->RemoveTree(backup) : RegDeleteTreeW(root.root, backup.c_str());
    if (e != ERROR_SUCCESS && e != ERROR_FILE_NOT_FOUND) return {e, external, changed};
    DeleteEmpty(root, g.verb + L"\\command");
    DeleteEmpty(root, g.verb);
    DeleteEmpty(root, g.verb.substr(0, g.verb.rfind(L'\\')));
    return {ERROR_SUCCESS, external, changed};
}
bool Equal(std::wstring_view a, std::wstring_view b) {
    return a.size() == b.size() && CompareStringOrdinal(a.data(), static_cast<int>(a.size()),
        b.data(), static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
}
}
bool DefaultManagerCommandIsOurs(std::wstring_view line, std::wstring_view exe) {
    while (!line.empty() && iswspace(line.front())) line.remove_prefix(1);
    if (line.empty() || exe.empty()) return false;
    if (line.front() == L'"') {
        line.remove_prefix(1);
        const size_t end = line.find(L'"');
        return end != std::wstring_view::npos && Equal(line.substr(0, end), exe);
    }
    return Equal(line.substr(0, line.find_first_of(L" \t\r\n")), exe);
}
bool IsThisPcArgument(std::wstring_view raw) {
    while (!raw.empty() && (iswspace(raw.front()) || raw.front() == L'"')) raw.remove_prefix(1);
    while (!raw.empty() && (iswspace(raw.back()) || raw.back() == L'"' || raw.back() == L'\\')) raw.remove_suffix(1);
    if (raw.size() >= 6 && Equal(raw.substr(0, 6), L"shell:")) raw.remove_prefix(6);
    return Equal(raw, kThisPcParsingName) || Equal(raw, L"MyComputerFolder");
}
DefaultManagerOptions ReadDefaultFileManager(const std::wstring& exe, DefaultManagerRegistry r) {
    const auto groups = Groups(exe);
    bool matches[4]{};
    for (size_t i = 0; i < groups.size(); ++i) {
        matches[i] = !exe.empty();
        for (const auto& change : groups[i].changes) {
            Value value;
            matches[i] = Read(r, change.path, change.name, value) == ERROR_SUCCESS &&
                value == change.wanted && matches[i];
        }
    }
    return {matches[0] && matches[1], matches[2], matches[3]};
}
DefaultManagerResult ApplyDefaultFileManager(const std::wstring& exe, DefaultManagerOptions options,
                                           DefaultManagerRegistry r) {
    if ((!r.root && !r.store) || exe.empty() || exe.find_first_of(L"\"\r\n") != std::wstring::npos)
        return {ERROR_INVALID_PARAMETER};
    const auto groups = Groups(exe);
    const bool wanted[] = {options.folders, options.folders, options.win_e, options.this_pc};
    DefaultManagerResult result;
    for (size_t i = 0; i < groups.size(); ++i) {
        const auto part = ApplyGroup(exe, groups[i], wanted[i], r);
        if (part.error != ERROR_SUCCESS && result.error == ERROR_SUCCESS) result.error = part.error;
        result.external_change = result.external_change || part.external_change;
        result.changed = result.changed || part.changed;
    }
    if (result.changed && r.notify_shell && !r.store)
        SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return result;
}
DefaultManagerResult RestoreDefaultFileManager(const std::wstring& exe, DefaultManagerRegistry registry) {
    return ApplyDefaultFileManager(exe, {}, registry);
}
} // namespace pulse::app
