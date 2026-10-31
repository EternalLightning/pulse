// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "system_ctx_registry.h"
#include "system_ctx_exchange.h"

#include <aclapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <sddl.h>
#include <winternl.h>

#include <algorithm>
#include <limits>
#include <map>
#include <cwctype>
#include <set>
#include <utility>

namespace pulse::app::sysctx {
namespace {

constexpr DWORD kVelocityHideId = 0x639bc8;  // the value ContextMenuManager writes
constexpr wchar_t kBatchHeader[] = L"PULSE-SYSCTX 2";
constexpr wchar_t kLegacyBatchHeader[] = L"PULSE-SYSCTX 1";
int g_velocity_override = -1;

struct Key {
    HKEY h = nullptr;
    Key() = default;
    Key(const Key&) = delete;
    Key& operator=(const Key&) = delete;
    ~Key() { if (h) RegCloseKey(h); }
    explicit operator bool() const { return h != nullptr; }
};

std::wstring Lower(std::wstring_view s) {
    std::wstring out(s);
    for (auto& c : out) c = static_cast<wchar_t>(std::towlower(c));
    return out;
}

bool IEquals(std::wstring_view a, std::wstring_view b) {
    return a.size() == b.size() && CompareStringOrdinal(a.data(), static_cast<int>(a.size()),
                                                         b.data(), static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
}

std::wstring Join(const std::wstring& a, std::wstring_view b) {
    if (a.empty()) return std::wstring(b);
    if (b.empty()) return a;
    return a + L"\\" + std::wstring(b);
}

LSTATUS Open(HKEY root, const std::wstring& path, REGSAM access, Key& key) {
    return RegOpenKeyExW(root, path.c_str(), 0, access, &key.h);
}

bool ReadString(HKEY key, const wchar_t* name, std::wstring& out) {
    DWORD type = 0, bytes = 0;
    if (RegQueryValueExW(key, name, nullptr, &type, nullptr, &bytes) != ERROR_SUCCESS) return false;
    if (type != REG_SZ && type != REG_EXPAND_SZ) return false;
    std::wstring buf(bytes / sizeof(wchar_t) + 1, L'\0');
    if (RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<BYTE*>(buf.data()), &bytes) != ERROR_SUCCESS)
        return false;
    buf.resize(wcsnlen(buf.c_str(), buf.size()));
    if (type == REG_EXPAND_SZ) {
        wchar_t expanded[MAX_PATH * 2];
        const DWORD n = ExpandEnvironmentStringsW(buf.c_str(), expanded, ARRAYSIZE(expanded));
        if (n > 0 && n <= ARRAYSIZE(expanded)) buf = expanded;
    }
    out = std::move(buf);
    return true;
}

// Unknown Position strings count as the default placement, as in Explorer.
Place ReadPlace(HKEY v) {
    std::wstring text;
    if (!ReadString(v, L"Position", text)) return Place::Default;
    if (IEquals(text, L"Top")) return Place::Top;
    if (IEquals(text, L"Bottom")) return Place::Bottom;
    return Place::Default;
}

bool HasValue(HKEY key, const wchar_t* name) {
    return RegQueryValueExW(key, name, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
}

bool ReadDword(HKEY key, const wchar_t* name, DWORD& out) {
    DWORD type = 0, bytes = sizeof(out);
    return RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<BYTE*>(&out), &bytes) == ERROR_SUCCESS &&
           type == REG_DWORD;
}

std::vector<std::wstring> Subkeys(HKEY key) {
    std::vector<std::wstring> out;
    wchar_t name[256];
    for (DWORD i = 0;; ++i) {
        DWORD len = ARRAYSIZE(name);
        const LSTATUS st = RegEnumKeyExW(key, i, name, &len, nullptr, nullptr, nullptr, nullptr);
        if (st == ERROR_NO_MORE_ITEMS) break;
        if (st == ERROR_SUCCESS) out.emplace_back(name, len);
        else if (st != ERROR_MORE_DATA) break;
    }
    return out;
}

HKEY RootKey(const Roots& r, Root root) { return root == Root::Machine ? r.machine : r.user; }
const std::wstring& RootBase(const Roots& r, Root root) {
    return root == Root::Machine ? r.machine_classes : r.user_classes;
}
std::wstring FullPath(const Roots& r, Root root, std::wstring_view rel) { return Join(RootBase(r, root), rel); }

std::wstring ResolveIndirect(const Roots& roots, const std::wstring& s) {
    if (!roots.resolve_strings || s.empty() || s[0] != L'@') return s;
    wchar_t buf[512];
    if (SUCCEEDED(SHLoadIndirectString(s.c_str(), buf, ARRAYSIZE(buf), nullptr))) return buf;
    return s;
}

std::wstring StripMnemonic(std::wstring s) {
    std::wstring out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == L'&') {
            if (i + 1 < s.size() && s[i + 1] == L'&') { out += L'&'; ++i; }
            continue;
        }
        out += s[i];
    }
    return out;
}

bool LooksLikeGuid(std::wstring_view s) {
    return s.size() == 38 && s.front() == L'{' && s.back() == L'}';
}

// First token of a command line or an Icon value, with environment expanded.
std::wstring RawPath(std::wstring c) {
    while (!c.empty() && std::iswspace(c.front())) c.erase(c.begin());
    if (c.empty()) return {};
    wchar_t expanded[MAX_PATH * 2];
    const DWORD n = ExpandEnvironmentStringsW(c.c_str(), expanded, ARRAYSIZE(expanded));
    if (n > 0 && n <= ARRAYSIZE(expanded)) c = expanded;
    std::wstring path;
    if (c.front() == L'"') {
        const size_t end = c.find(L'"', 1);
        path = c.substr(1, end == std::wstring::npos ? std::wstring::npos : end - 1);
    } else {
        const std::wstring lower = Lower(c);
        size_t cut = std::wstring::npos;
        for (const wchar_t* ext : {L".exe", L".dll", L".ico", L".cpl"}) {
            const size_t at = lower.find(ext);
            if (at != std::wstring::npos) cut = (std::min)(cut, at + 4);
        }
        path = cut != std::wstring::npos ? c.substr(0, cut) : c.substr(0, c.find(L' '));
    }
    if (!path.empty() && path[0] == L'@') path.erase(0, 1);
    const size_t comma = path.rfind(L',');
    if (comma != std::wstring::npos && comma > 2) {
        bool digits = comma + 1 < path.size();
        for (size_t i = comma + 1; i < path.size(); ++i)
            if (!(std::iswdigit(path[i]) || (i == comma + 1 && path[i] == L'-'))) digits = false;
        if (digits) path.resize(comma);
    }
    return path;
}

bool IsAbsolute(const std::wstring& p) {
    return p.size() > 2 && std::iswalpha(p[0]) && p[1] == L':' && (p[2] == L'\\' || p[2] == L'/');
}

bool FileExists(const std::wstring& p) {
    const DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

// Resolves bare names ("notepad.exe", "shell32.dll") through the search path.
std::wstring ResolveFile(const std::wstring& raw) {
    if (raw.empty()) return {};
    if (IsAbsolute(raw)) return FileExists(raw) ? raw : std::wstring{};
    wchar_t found[MAX_PATH];
    const DWORD length = SearchPathW(nullptr, raw.c_str(), nullptr, ARRAYSIZE(found), found, nullptr);
    if (length > 0 && length < ARRAYSIZE(found)) return found;
    return {};
}

TargetState FileState(const std::wstring& value) {
    const std::wstring raw = RawPath(value);
    if (raw.empty()) return TargetState::Unknown;
    if (!IsAbsolute(raw)) return ResolveFile(raw).empty() ? TargetState::Unknown : TargetState::Present;
    const DWORD attributes = GetFileAttributesW(raw.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES)
        return attributes & FILE_ATTRIBUTE_DIRECTORY ? TargetState::Unavailable : TargetState::Present;
    const DWORD error = GetLastError();
    return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ? TargetState::Missing
                                                                                     : TargetState::Unavailable;
}

void SetTarget(Item& item, const std::wstring& value) {
    item.target_state = FileState(value);
    item.missing = item.target_state == TargetState::Missing;
}

bool IsProtectedVerb(std::wstring_view verb) {
    static const wchar_t* const kVerbs[] = {L"open", L"opennewwindow", L"opennewprocess", L"opennewtab",
                                            L"explore", L"runas", L"runasuser", L"properties", L"find"};
    for (const auto* v : kVerbs) if (IEquals(verb, v)) return true;
    return false;
}

bool IsProtectedClsid(std::wstring_view clsid) {
    static const wchar_t* const kClsids[] = {
        L"{09799AFB-AD67-11d1-ABCD-00C04FC30936}",  // Open With
        L"{D969A300-E7FF-11d0-A93B-00A0C90F2719}",  // New
        L"{37ea3a21-7493-4208-a011-7f9ea79ce9f5}",  // Open containing folder
        L"{7BA4C740-9E81-11CF-99D3-00AA004AE837}",  // Send To
    };
    for (const auto* c : kClsids) if (IEquals(clsid, c)) return true;
    return false;
}

bool OwnedByTrustedInstaller(const Roots& roots, Root root, const std::wstring& rel) {
    if (!roots.check_owner || root != Root::Machine || roots.machine != HKEY_LOCAL_MACHINE) return false;
    const std::wstring object = L"MACHINE\\" + FullPath(roots, root, rel);
    PSID owner = nullptr;
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (GetNamedSecurityInfoW(object.c_str(), SE_REGISTRY_KEY, OWNER_SECURITY_INFORMATION, &owner, nullptr,
                              nullptr, nullptr, &sd) != ERROR_SUCCESS) return false;
    bool ti = false;
    LPWSTR sid = nullptr;
    if (owner && ConvertSidToStringSidW(owner, &sid)) {
        ti = wcscmp(sid, L"S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464") == 0;
        LocalFree(sid);
    }
    LocalFree(sd);
    return ti;
}

bool VelocitySupported() {
    if (g_velocity_override >= 0) return g_velocity_override != 0;
    using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
    static const bool supported = [] {
        HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        auto fn = ntdll ? reinterpret_cast<RtlGetVersionFn>(
            reinterpret_cast<void*>(GetProcAddress(ntdll, "RtlGetVersion"))) : nullptr;
        RTL_OSVERSIONINFOW v{sizeof(v)};
        return fn && fn(&v) == 0 && (v.dwMajorVersion > 10 || (v.dwMajorVersion == 10 && v.dwBuildNumber >= 15063));
    }();
    return supported;
}

// HKCR view of a class key: the user layer wins.
LSTATUS OpenMergedStatus(const Roots& roots, const std::wstring& rel, Key& key) {
    const LSTATUS user = Open(roots.user, FullPath(roots, Root::User, rel), KEY_READ, key);
    if (user != ERROR_FILE_NOT_FOUND && user != ERROR_PATH_NOT_FOUND) return user;
    return Open(roots.machine, FullPath(roots, Root::Machine, rel), KEY_READ, key);
}

bool OpenMerged(const Roots& roots, const std::wstring& rel, Key& key) {
    return OpenMergedStatus(roots, rel, key) == ERROR_SUCCESS;
}

// HKCR merges per value: an HKCU key with an empty default (common for .png
// after "Open with") still inherits HKLM's ProgID.
std::wstring ReadMergedString(const Roots& roots, const std::wstring& rel, const wchar_t* name) {
    for (const Root root : {Root::User, Root::Machine}) {
        Key k;
        std::wstring value;
        if (Open(RootKey(roots, root), FullPath(roots, root, rel), KEY_READ, k) == ERROR_SUCCESS &&
            ReadString(k.h, name, value) && !value.empty()) return value;
    }
    return {};
}

bool MergedKeyExists(const Roots& roots, const std::wstring& rel) {
    Key k;
    return OpenMerged(roots, rel, k);
}

// Explorer's effective ProgIDs for an extension: the user's choice, then the
// registered default.
std::vector<std::wstring> ExtensionProgIds(const Roots& roots, const std::wstring& ext) {
    std::vector<std::wstring> ids;
    std::wstring choice;
    if (!roots.file_exts.empty()) {
        Key uc;
        if (Open(roots.user, roots.file_exts + L"\\" + ext + L"\\UserChoice", KEY_READ, uc) == ERROR_SUCCESS)
            ReadString(uc.h, L"ProgId", choice);
    }
    for (auto& id : {choice, ReadMergedString(roots, ext, nullptr)}) {
        if (id.empty()) continue;
        if (std::none_of(ids.begin(), ids.end(), [&](const std::wstring& x) { return IEquals(x, id); }))
            ids.push_back(id);
    }
    return ids;
}

void FillHandler(const Roots& roots, Item& it) {
    Key cls;
    if (it.clsid.empty()) return;
    const LSTATUS status = OpenMergedStatus(roots, L"CLSID\\" + it.clsid, cls);
    if (status != ERROR_SUCCESS) {
        it.target_state = status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND
                              ? TargetState::Missing : TargetState::Unavailable;
        it.missing = it.target_state == TargetState::Missing;
        return;
    }
    std::wstring name;
    if (roots.resolve_strings && ReadString(cls.h, nullptr, name) && !name.empty() && LooksLikeGuid(it.display))
        it.display = name;
    Key inproc;
    std::wstring dll;
    if (Open(cls.h, L"InprocServer32", KEY_READ, inproc) == ERROR_SUCCESS && ReadString(inproc.h, nullptr, dll)) {
        it.file = ResolveFile(RawPath(dll));
        SetTarget(it, dll);
    }
}

void ScanVerbs(const Roots& roots, const Scene& scene, const std::wstring& cls, Root root,
               std::vector<Item>& out) {
    Key shell;
    if (Open(RootKey(roots, root), FullPath(roots, root, cls + L"\\shell"), KEY_READ, shell) != ERROR_SUCCESS)
        return;
    for (const auto& verb : Subkeys(shell.h)) {
        // printto only serves drag-onto-printer; Explorer never lists it.
        if (IEquals(verb, L"printto")) continue;
        Key v;
        if (Open(shell.h, verb, KEY_READ, v) != ERROR_SUCCESS) continue;
        Item it;
        it.kind = Kind::Verb;
        it.root = root;
        it.scene = scene.id;
        it.class_key = cls;
        it.key_name = verb;
        it.key_path = cls + L"\\shell\\" + verb;
        std::wstring text;
        if (!ReadString(v.h, L"MUIVerb", text) || text.empty()) ReadString(v.h, nullptr, text);
        it.display = text.empty() ? verb : StripMnemonic(ResolveIndirect(roots, text));
        if (!it.display.empty() && it.display[0] == L'@') it.display = verb;
        ReadString(v.h, L"Icon", it.icon);
        Key cmd;
        if (Open(v.h, L"command", KEY_READ, cmd) == ERROR_SUCCESS) ReadString(cmd.h, nullptr, it.command);
        it.file = ResolveFile(RawPath(it.command));
        if (it.file.empty() && !it.icon.empty()) it.file = ResolveFile(RawPath(it.icon));
        SetTarget(it, it.command);
        it.icon_state = FileState(it.icon);
        it.extended = HasValue(v.h, L"Extended");
        it.position = ReadPlace(v.h);
        it.legacy_disable = HasValue(v.h, L"LegacyDisable");
        it.programmatic_only = HasValue(v.h, L"ProgrammaticAccessOnly");
        DWORD velocity = 0, flags = 0;
        it.velocity_hidden = ReadDword(v.h, L"HideBasedOnVelocityId", velocity);
        it.command_flags_hidden = ReadDword(v.h, L"CommandFlags", flags) && (flags & 0x8);
        it.enabled = !(it.legacy_disable || it.programmatic_only || it.velocity_hidden || it.command_flags_hidden);
        it.is_protected = IsProtectedVerb(verb);
        it.trusted_installer = OwnedByTrustedInstaller(roots, root, it.key_path);
        out.push_back(std::move(it));
    }
}

void ScanHandlers(const Roots& roots, const Scene& scene, const std::wstring& cls, Root root,
                  const wchar_t* group, Kind kind, std::vector<Item>& out) {
    for (const bool dashed : {false, true}) {
        const std::wstring sub = cls + L"\\shellex\\" + (dashed ? L"-" : L"") + group;
        Key handlers;
        if (Open(RootKey(roots, root), FullPath(roots, root, sub), KEY_READ, handlers) != ERROR_SUCCESS) continue;
        for (const auto& name : Subkeys(handlers.h)) {
            Key h;
            if (Open(handlers.h, name, KEY_READ, h) != ERROR_SUCCESS) continue;
            Item it;
            it.kind = kind;
            it.root = root;
            it.scene = scene.id;
            it.class_key = cls;
            it.key_name = name;
            it.key_path = sub + L"\\" + name;
            if (LooksLikeGuid(name)) it.clsid = name;
            else ReadString(h.h, nullptr, it.clsid);
            if (!LooksLikeGuid(it.clsid)) it.clsid.clear();
            it.display = name;
            FillHandler(roots, it);
            it.dash_marker = dashed;
            it.enabled = !dashed;
            it.is_protected = IsProtectedClsid(it.clsid);
            it.trusted_installer = OwnedByTrustedInstaller(roots, root, it.key_path);
            out.push_back(std::move(it));
        }
    }
}

bool IsEffectiveShellNew(HKEY sn) {
    for (const wchar_t* name : {L"NullFile", L"Data", L"FileName", L"Directory", L"Command", L"Handler"})
        if (HasValue(sn, name)) return true;
    return false;
}

void ScanShellNew(const Roots& roots, const Scene& scene, std::vector<Item>& out) {
    // Explorer: .ext's ProgID decides; .ext\<ProgID>\ShellNew when that key
    // exists, otherwise .ext\ShellNew. Keys without a creation value are inert.
    std::set<std::wstring> exts;
    for (const Root root : {Root::User, Root::Machine}) {
        Key classes;
        if (Open(RootKey(roots, root), RootBase(roots, root), KEY_READ, classes) != ERROR_SUCCESS) continue;
        for (const auto& ext : Subkeys(classes.h))
            if (ext.size() >= 2 && ext[0] == L'.') exts.insert(ext);
    }
    auto add = [&](Root root, const std::wstring& cls, const std::wstring& base, const std::wstring& progid) {
        Key classes;
        if (Open(RootKey(roots, root), RootBase(roots, root), KEY_READ, classes) != ERROR_SUCCESS) return;
        for (const bool dashed : {false, true}) {
            const std::wstring path = base + (dashed ? L"\\-ShellNew" : L"\\ShellNew");
            Key sn;
            if (Open(classes.h, path, KEY_READ, sn) != ERROR_SUCCESS || !IsEffectiveShellNew(sn.h)) continue;
            Item it;
            it.kind = Kind::ShellNew;
            it.root = root;
            it.scene = scene.id;
            it.class_key = cls;
            it.key_name = cls;
            it.key_path = path;
            it.dash_marker = dashed;
            it.enabled = !dashed;
            ReadString(sn.h, L"Command", it.command);
            std::wstring text;
            if (!ReadString(sn.h, L"MenuText", text) || text.empty()) ReadString(sn.h, L"ItemName", text);
            if ((text.empty() || text[0] == L'@') && !progid.empty()) {
                std::wstring friendly = ReadMergedString(roots, progid, L"FriendlyTypeName");
                if (friendly.empty()) friendly = ReadMergedString(roots, progid, nullptr);
                if (!friendly.empty()) text = friendly;
            }
            std::wstring resolved = text.empty() ? std::wstring() : ResolveIndirect(roots, text);
            it.display = resolved.empty() || resolved[0] == L'@' ? cls : resolved;
            SetTarget(it, it.command);
            out.push_back(std::move(it));
        }
    };
    for (const auto& ext : exts) {
        std::wstring progid, base = ext;
        for (const auto& id : ExtensionProgIds(roots, ext)) {
            if (progid.empty()) progid = id;
            if (MergedKeyExists(roots, ext + L"\\" + id)) {
                progid = id;
                base = ext + L"\\" + id;
                break;
            }
        }
        for (const Root root : {Root::User, Root::Machine}) add(root, ext, base, progid);
    }
    // 新建 → 文件夹 lives on the Folder class; listed but never switchable.
    const size_t first_folder = out.size();
    for (const Root root : {Root::User, Root::Machine}) add(root, L"Folder", L"Folder", L"");
    for (size_t k = first_folder; k < out.size(); ++k) out[k].is_protected = true;
}

void ScanSendTo(const Roots& roots, const Scene& scene, std::vector<Item>& out) {
    std::wstring dir = roots.sendto_dir;
    if (dir.empty()) {
        PWSTR p = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_SendTo, 0, nullptr, &p))) dir = p;
        CoTaskMemFree(p);
    }
    if (dir.empty()) return;
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (f == INVALID_HANDLE_VALUE) return;
    do {
        const std::wstring name = fd.cFileName;
        if (name == L"." || name == L".." || IEquals(name, L"desktop.ini")) continue;
        Item it;
        it.kind = Kind::SendTo;
        it.root = Root::File;
        it.scene = scene.id;
        it.key_name = name;
        it.key_path = dir + L"\\" + name;
        const size_t dot = name.rfind(L'.');
        it.display = dot == std::wstring::npos || dot == 0 ? name : name.substr(0, dot);
        it.enabled = !(fd.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN);
        it.file = it.key_path;
        out.push_back(std::move(it));
    } while (FindNextFileW(f, &fd));
    FindClose(f);
}

void ScanOpenWith(const Roots& roots, const Scene& scene, std::vector<Item>& out) {
    for (const Root root : {Root::User, Root::Machine}) {
        Key apps;
        if (Open(RootKey(roots, root), FullPath(roots, root, L"Applications"), KEY_READ, apps) != ERROR_SUCCESS)
            continue;
        for (const auto& exe : Subkeys(apps.h)) {
            // Open With only offers Applications\<name>.exe; ProgID-like
            // keys some installers drop here are never shown.
            if (exe.size() < 5 || !IEquals(std::wstring_view(exe).substr(exe.size() - 4), L".exe")) continue;
            Key ak, cmd;
            if (Open(apps.h, exe, KEY_READ, ak) != ERROR_SUCCESS) continue;
            if (Open(ak.h, L"shell\\open\\command", KEY_READ, cmd) != ERROR_SUCCESS) continue;
            Item it;
            it.kind = Kind::OpenWith;
            it.root = root;
            it.scene = scene.id;
            it.key_name = exe;
            it.key_path = L"Applications\\" + exe;
            ReadString(cmd.h, nullptr, it.command);
            it.file = ResolveFile(RawPath(it.command));
            SetTarget(it, it.command);
            std::wstring friendly;
            if (ReadString(ak.h, L"FriendlyAppName", friendly) && !friendly.empty())
                it.display = ResolveIndirect(roots, friendly);
            if (it.display.empty() || it.display[0] == L'@') it.display = exe;
            it.no_open_with = HasValue(ak.h, L"NoOpenWith");
            it.enabled = !it.no_open_with;
            out.push_back(std::move(it));
        }
    }
}

std::wstring ReplaceSegment(const std::wstring& path, std::wstring_view from, std::wstring_view to) {
    const std::wstring lower = Lower(path);
    const std::wstring needle = L"\\" + Lower(from) + L"\\";
    const size_t at = lower.rfind(needle);
    if (at == std::wstring::npos) return {};
    return path.substr(0, at + 1) + std::wstring(to) + path.substr(at + 1 + from.size());
}

const wchar_t* GroupName(Kind kind) {
    return kind == Kind::DragDrop ? L"DragDropHandlers" : L"ContextMenuHandlers";
}

OpResult FromStatus(LSTATUS st) {
    switch (st) {
    case ERROR_SUCCESS: return OpResult::Ok;
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND: return OpResult::NotFound;
    case ERROR_ACCESS_DENIED: return OpResult::AccessDenied;
    default: return OpResult::Failed;
    }
}

bool SkipsLegacyDisable(const std::wstring& key_path) {
    // Folder\\shell\\opennewwindow without LegacyDisable keeps Win+E working.
    return IEquals(key_path, L"Folder\\shell\\opennewwindow");
}

const wchar_t* PlaceToken(Place place) {
    return place == Place::Top ? L"top" : place == Place::Bottom ? L"bottom" : L"default";
}

unsigned PlanVerb(HKEY v, const Op& op) {
    const bool velocity = HasValue(v, L"HideBasedOnVelocityId");
    const bool legacy = HasValue(v, L"LegacyDisable");
    const bool programmatic = HasValue(v, L"ProgrammaticAccessOnly");
    DWORD flags = 0;
    const bool flag_bit = ReadDword(v, L"CommandFlags", flags) && (flags & 0x8);
    unsigned mask = 0;
    if (op.action == Action::Disable) {
        if (VelocitySupported() && !velocity) mask |= kMarkVelocity;
        if (!SkipsLegacyDisable(op.key_path) && !legacy) mask |= kMarkLegacy;
        if (!programmatic) mask |= kMarkProgrammatic;
    } else {
        if (velocity) mask |= kMarkVelocity;
        if (legacy) mask |= kMarkLegacy;
        if (programmatic) mask |= kMarkProgrammatic;
        if (flag_bit) mask |= kMarkFlags;
    }
    return mask;
}

const wchar_t* SnapshotValueName(ValueName name) {
    switch (name) {
    case ValueName::Velocity: return L"HideBasedOnVelocityId";
    case ValueName::LegacyDisable: return L"LegacyDisable";
    case ValueName::ProgrammaticAccessOnly: return L"ProgrammaticAccessOnly";
    case ValueName::CommandFlags: return L"CommandFlags";
    case ValueName::Position: return L"Position";
    }
    return nullptr;
}

unsigned SnapshotValueBit(ValueName name) {
    switch (name) {
    case ValueName::Velocity: return kMarkVelocity;
    case ValueName::LegacyDisable: return kMarkLegacy;
    case ValueName::ProgrammaticAccessOnly: return kMarkProgrammatic;
    case ValueName::CommandFlags: return kMarkFlags;
    case ValueName::Position: return 0x10;
    }
    return 0;
}

bool ValidSnapshots(const Op& op) {
    if (op.recover_tree && (op.planned || op.has_previous_values ||
        !((op.action == Action::Delete || op.action == Action::Restore) ||
          ((op.kind == Kind::ShellEx || op.kind == Kind::DragDrop) &&
           (op.action == Action::Enable || op.action == Action::Disable))))) return false;
    if (!op.has_previous_values) return op.previous_values.empty() && !op.restore_previous_values;
    if (!op.planned || op.kind != Kind::Verb || op.previous_values.size() > 5) return false;
    unsigned expected = 0;
    if (op.action == Action::Position) {
        if (op.mask != 0) return false;
        expected = 0x10;
    } else if (op.action == Action::Enable || op.action == Action::Disable) {
        if (op.mask > 0xF) return false;
        expected = op.mask;
    } else return false;
    unsigned found = 0;
    for (const auto& value : op.previous_values) {
        const unsigned bit = SnapshotValueBit(value.name);
        if (!bit || !(expected & bit) || (found & bit) || value.data.size() > kMaxValueSnapshotBytes) return false;
        if (!value.present && (value.type != REG_NONE || !value.data.empty())) return false;
        found |= bit;
    }
    return found == expected;
}

LSTATUS CaptureValue(HKEY key, ValueName name, ValueSnapshot& value) {
    value = {};
    value.name = name;
    DWORD bytes = 0;
    LSTATUS st = RegQueryValueExW(key, SnapshotValueName(name), nullptr, &value.type, nullptr, &bytes);
    if (st == ERROR_FILE_NOT_FOUND) { value.type = REG_NONE; return ERROR_SUCCESS; }
    if (st != ERROR_SUCCESS) return st;
    if (bytes > kMaxValueSnapshotBytes) return ERROR_FILE_TOO_LARGE;
    value.present = true;
    value.data.resize(bytes);
    st = RegQueryValueExW(key, SnapshotValueName(name), nullptr, &value.type,
                          value.data.empty() ? nullptr : value.data.data(), &bytes);
    if (st != ERROR_SUCCESS) return st;
    if (bytes > value.data.size()) return ERROR_MORE_DATA;
    value.data.resize(bytes);
    return ERROR_SUCCESS;
}

LSTATUS WriteValue(HKEY key, const ValueSnapshot& value, bool exact) {
    if (!value.present) {
        const LSTATUS st = RegDeleteValueW(key, SnapshotValueName(value.name));
        return st == ERROR_FILE_NOT_FOUND ? ERROR_SUCCESS : st;
    }
    if (!exact) {
        return RegSetValueExW(key, SnapshotValueName(value.name), 0, value.type,
                              value.data.empty() ? nullptr : value.data.data(), static_cast<DWORD>(value.data.size()));
    }
    // Native writes preserve malformed string data, including its exact length.
    using SetValue = NTSTATUS (NTAPI*)(HANDLE, PUNICODE_STRING, ULONG, ULONG, PVOID, ULONG);
    using StatusError = ULONG (NTAPI*)(NTSTATUS);
    static const auto set_value = reinterpret_cast<SetValue>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtSetValueKey"));
    static const auto status_error = reinterpret_cast<StatusError>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlNtStatusToDosError"));
    if (!set_value || !status_error) return ERROR_PROC_NOT_FOUND;
    const wchar_t* name = SnapshotValueName(value.name);
    const auto length = static_cast<USHORT>(wcslen(name) * sizeof(wchar_t));
    UNICODE_STRING unicode{length, static_cast<USHORT>(length + sizeof(wchar_t)), const_cast<wchar_t*>(name)};
    const NTSTATUS status = set_value(key, &unicode, 0, value.type,
        value.data.empty() ? nullptr : const_cast<BYTE*>(value.data.data()), static_cast<ULONG>(value.data.size()));
    return status >= 0 ? ERROR_SUCCESS : static_cast<LSTATUS>(status_error(status));
}

bool InjectValueWriteFailure(const Roots& roots, Root root, bool rollback) {
    auto* fault = roots.value_write_fault_for_testing.get();
    constexpr std::wstring_view prefix = L"Software\\PulseTest\\";
    const auto& base = RootBase(roots, root);
    if (!fault || RootKey(roots, root) != HKEY_CURRENT_USER || base.size() <= prefix.size() ||
        !IEquals(std::wstring_view(base).substr(0, prefix.size()), prefix)) return false;
    size_t& calls = rollback ? fault->rollback_calls : fault->write_calls;
    const size_t fail_call = rollback ? fault->fail_rollback_call : fault->fail_write_call;
    ++calls;
    return fail_call != 0 && calls == fail_call;
}

OpResult ApplyValues(const Roots& roots, Root root, HKEY key, const std::vector<ValueSnapshot>& values, bool exact) {
    std::vector<ValueSnapshot> before;
    before.reserve(values.size());
    for (const auto& value : values) {
        ValueSnapshot previous;
        const LSTATUS st = CaptureValue(key, value.name, previous);
        if (st != ERROR_SUCCESS) return FromStatus(st);
        before.push_back(std::move(previous));
    }
    for (size_t i = 0; i < values.size(); ++i) {
        const LSTATUS st = InjectValueWriteFailure(roots, root, false) ? ERROR_WRITE_FAULT : WriteValue(key, values[i], exact);
        if (st == ERROR_SUCCESS) continue;
        bool restored = true;
        // Include the failed attempt, whose error alone cannot prove it left no change.
        for (size_t n = i + 1; n > 0; --n) {
            const LSTATUS undo = InjectValueWriteFailure(roots, root, true) ? ERROR_WRITE_FAULT : WriteValue(key, before[n - 1], true);
            if (undo != ERROR_SUCCESS) restored = false;
        }
        return restored ? FromStatus(st) : OpResult::RecoveryRequired;
    }
    return OpResult::Ok;
}

ValueSnapshot WrittenValue(ValueName name, DWORD type, const void* data, size_t bytes) {
    ValueSnapshot value;
    value.name = name;
    value.present = true;
    value.type = type;
    const auto* first = static_cast<const BYTE*>(data);
    if (bytes) value.data.assign(first, first + bytes);
    return value;
}

OpResult ApplyPlace(const Roots& roots, Root root, HKEY key, Place place) {
    ValueSnapshot value;
    value.name = ValueName::Position;
    if (place != Place::Default) {
        const wchar_t* text = place == Place::Top ? L"Top" : L"Bottom";
        value = WrittenValue(ValueName::Position, REG_SZ, text, (wcslen(text) + 1) * sizeof(wchar_t));
    }
    return ApplyValues(roots, root, key, {value}, false);
}

OpResult ApplyPreviousValues(const Roots& roots, HKEY key, const Op& op) {
    return ApplyValues(roots, op.root, key, op.previous_values, true);
}

OpResult ApplyVerb(const Roots& roots, const Op& op) {
    Key v;
    const LSTATUS st = Open(RootKey(roots, op.root), FullPath(roots, op.root, op.key_path),
                            KEY_QUERY_VALUE | KEY_SET_VALUE, v);
    if (st != ERROR_SUCCESS) return FromStatus(st);
    if (op.restore_previous_values) return ApplyPreviousValues(roots, v.h, op);
    if (op.action == Action::Position) return ApplyPlace(roots, op.root, v.h, op.place);
    const unsigned mask = op.planned ? op.mask : PlanVerb(v.h, op);
    const wchar_t empty[] = L"";
    std::vector<ValueSnapshot> values;
    DWORD flags = 0;
    const bool has_flags = ReadDword(v.h, L"CommandFlags", flags);
    if (op.action == Action::Disable) {
        if (mask & kMarkVelocity) {
            const DWORD id = kVelocityHideId;
            values.push_back(WrittenValue(ValueName::Velocity, REG_DWORD, &id, sizeof(id)));
        }
        if (mask & kMarkLegacy) {
            values.push_back(WrittenValue(ValueName::LegacyDisable, REG_SZ, empty, sizeof(empty)));
        }
        if (mask & kMarkProgrammatic) {
            values.push_back(WrittenValue(ValueName::ProgrammaticAccessOnly, REG_SZ, empty, sizeof(empty)));
        }
        if ((mask & kMarkFlags) && has_flags) {
            flags |= 0x8;
            values.push_back(WrittenValue(ValueName::CommandFlags, REG_DWORD, &flags, sizeof(flags)));
        }
    } else {
        for (const auto name : {ValueName::Velocity, ValueName::LegacyDisable, ValueName::ProgrammaticAccessOnly}) {
            if (mask & SnapshotValueBit(name)) {
                ValueSnapshot value;
                value.name = name;
                values.push_back(std::move(value));
            }
        }
        if ((mask & kMarkFlags) && has_flags && (flags & 0x8)) {
            flags &= ~DWORD{0x8};
            values.push_back(WrittenValue(ValueName::CommandFlags, REG_DWORD, &flags, sizeof(flags)));
        }
    }
    return ApplyValues(roots, op.root, v.h, values, false);
}

bool IsEmptyKey(HKEY base, const std::wstring& path) {
    Key k;
    if (Open(base, path, KEY_READ, k) != ERROR_SUCCESS) return false;
    DWORD subkeys = 0, values = 0;
    if (RegQueryInfoKeyW(k.h, nullptr, nullptr, nullptr, &subkeys, nullptr, nullptr, &values, nullptr, nullptr,
                         nullptr, nullptr) != ERROR_SUCCESS) return false;
    return subkeys == 0 && values == 0;
}

bool MissingKey(LSTATUS status) {
    return status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND;
}

bool ScratchTreeFault(const Roots& roots, Root root, const std::wstring& src, const std::wstring& dst) {
    constexpr std::wstring_view prefix = L"Software\\PulseTest\\";
    const auto scratch = [&](std::wstring_view path) {
        return path.size() > prefix.size() && IEquals(path.substr(0, prefix.size()), prefix);
    };
    return roots.tree_move_fault_for_testing && RootKey(roots, root) == HKEY_CURRENT_USER &&
           scratch(src) && scratch(dst);
}

LSTATUS CopyTree(const Roots& roots, Root root, const std::wstring& src, const std::wstring& dst,
                 HKEY source, HKEY target) {
    auto* fault = ScratchTreeFault(roots, root, src, dst) ? roots.tree_move_fault_for_testing.get() : nullptr;
    if (fault && ++fault->copy_calls == fault->fail_copy_call) {
        if (fault->partial_copy) {
            DWORD type = 0, bytes = 0;
            if (RegQueryValueExW(source, nullptr, nullptr, &type, nullptr, &bytes) == ERROR_SUCCESS) {
                std::vector<BYTE> data(bytes);
                if (RegQueryValueExW(source, nullptr, nullptr, &type, data.data(), &bytes) == ERROR_SUCCESS)
                    RegSetValueExW(target, nullptr, 0, type, data.data(), bytes);
            }
        }
        return ERROR_WRITE_FAULT;
    }
    return RegCopyTreeW(source, nullptr, target);
}

LSTATUS DeleteTree(const Roots& roots, Root root, const std::wstring& src, const std::wstring& dst,
                   const std::wstring& path) {
    auto* fault = ScratchTreeFault(roots, root, src, dst) ? roots.tree_move_fault_for_testing.get() : nullptr;
    if (fault) ++fault->delete_calls;
    if (fault && (fault->delete_calls == fault->fail_delete_call ||
                  fault->delete_calls == fault->fail_second_delete_call)) {
        if (fault->partial_delete) {
            Key key;
            if (Open(RootKey(roots, root), path, KEY_READ | KEY_WRITE, key) == ERROR_SUCCESS) {
                RegDeleteValueW(key.h, nullptr);
                wchar_t name[256];
                DWORD length = ARRAYSIZE(name);
                if (RegEnumKeyExW(key.h, 0, name, &length, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS)
                    RegDeleteTreeW(key.h, name);
            }
        }
        return ERROR_ACCESS_DENIED;
    }
    return RegDeleteTreeW(RootKey(roots, root), path.c_str());
}

struct TreeValue {
    DWORD type = REG_NONE;
    std::vector<BYTE> data;
    bool operator==(const TreeValue&) const = default;
};
struct TreeNode {
    std::map<std::wstring, TreeValue> values;
    std::vector<BYTE> security;
};
using TreeSnapshot = std::map<std::wstring, TreeNode>;

// Recovery compares raw values, empty keys and security before merging. Failed
// reads and symbolic links never qualify as an absent or matching tree.
LSTATUS CaptureTree(HKEY key, const std::wstring& path, TreeSnapshot& tree, size_t& bytes, size_t depth = 0) {
    if (depth > 64 || tree.size() >= 4096) return ERROR_FILE_TOO_LARGE;
    auto& node = tree[path];
    DWORD size = 0;
    constexpr SECURITY_INFORMATION information = OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION |
                                                  DACL_SECURITY_INFORMATION;
    LSTATUS status = RegGetKeySecurity(key, information, nullptr, &size);
    if (status != ERROR_INSUFFICIENT_BUFFER) return status == ERROR_SUCCESS ? ERROR_INVALID_DATA : status;
    if (size > 1024 * 1024 || bytes > 16 * 1024 * 1024 - size) return ERROR_FILE_TOO_LARGE;
    node.security.resize(size);
    status = RegGetKeySecurity(key, information, reinterpret_cast<PSECURITY_DESCRIPTOR>(node.security.data()), &size);
    if (status != ERROR_SUCCESS) return status;
    node.security.resize(size);
    bytes += size;
    for (DWORD i = 0;; ++i) {
        std::wstring name(16384, L'\0');
        DWORD length = static_cast<DWORD>(name.size()), type = 0, count = 0;
        status = RegEnumValueW(key, i, name.data(), &length, nullptr, &type, nullptr, &count);
        if (status == ERROR_NO_MORE_ITEMS) break;
        if (status != ERROR_SUCCESS) return status;
        if (type == REG_LINK) return ERROR_NOT_SUPPORTED;
        if (count > 16 * 1024 * 1024 || bytes > 16 * 1024 * 1024 - count || i >= 65536)
            return ERROR_FILE_TOO_LARGE;
        TreeValue value{type, std::vector<BYTE>(count)};
        status = RegQueryValueExW(key, name.c_str(), nullptr, &value.type, value.data.data(), &count);
        if (status != ERROR_SUCCESS || count > value.data.size()) return status == ERROR_SUCCESS ? ERROR_MORE_DATA : status;
        value.data.resize(count);
        bytes += count;
        node.values.emplace(Lower(std::wstring_view(name).substr(0, length)), std::move(value));
    }
    for (DWORD i = 0;; ++i) {
        wchar_t name[256];
        DWORD length = ARRAYSIZE(name);
        status = RegEnumKeyExW(key, i, name, &length, nullptr, nullptr, nullptr, nullptr);
        if (status == ERROR_NO_MORE_ITEMS) return ERROR_SUCCESS;
        if (status != ERROR_SUCCESS) return status;
        Key child;
        status = RegOpenKeyExW(key, name, REG_OPTION_OPEN_LINK, KEY_READ, &child.h);
        if (status != ERROR_SUCCESS) return status;
        status = CaptureTree(child.h, Join(path, Lower(std::wstring_view(name, length))), tree, bytes, depth + 1);
        if (status != ERROR_SUCCESS) return status;
    }
}

bool TreeSubset(const TreeSnapshot& partial, const TreeSnapshot& full) {
    for (const auto& [path, node] : partial) {
        const auto found = full.find(path);
        if (found == full.end() || node.security != found->second.security) return false;
        for (const auto& [name, value] : node.values) {
            const auto other = found->second.values.find(name);
            if (other == found->second.values.end() || value != other->second) return false;
        }
    }
    return true;
}

OpResult MoveFailure(LSTATUS status, bool recovered) {
    SetLastError(status); // retain the original failure after rollback/cleanup APIs
    return recovered ? FromStatus(status) : OpResult::RecoveryRequired;
}

// A recovery inverse may see both the partial original and its complete copy.
// Reconcile only compatible trees; differing values/security retain both copies.
OpResult MoveTree(const Roots& roots, Root root, const std::wstring& src, const std::wstring& dst,
                  bool recovery) {
    HKEY base = RootKey(roots, root);
    Key s, d;
    const DWORD options = recovery ? REG_OPTION_OPEN_LINK : 0;
    const LSTATUS source_status = RegOpenKeyExW(base, src.c_str(), options, KEY_READ, &s.h);
    const LSTATUS target_status = RegOpenKeyExW(base, dst.c_str(), options, KEY_READ, &d.h);
    if (source_status != ERROR_SUCCESS) {
        if (recovery && MissingKey(source_status) && target_status == ERROR_SUCCESS) return OpResult::Ok;
        return FromStatus(source_status);
    }
    if (target_status != ERROR_SUCCESS && !MissingKey(target_status)) return FromStatus(target_status);
    bool existing = target_status == ERROR_SUCCESS;
    bool copy = true;
    if (existing) {
        if (!recovery) return OpResult::Conflict;
        TreeSnapshot source, target;
        size_t bytes = 0;
        LSTATUS status = CaptureTree(s.h, L"", source, bytes);
        if (status == ERROR_SUCCESS) status = CaptureTree(d.h, L"", target, bytes);
        if (status != ERROR_SUCCESS) return MoveFailure(status, false);
        if (TreeSubset(source, target)) copy = false;
        else if (!TreeSubset(target, source)) return OpResult::Conflict;
        RegCloseKey(d.h); d.h = nullptr;
        status = Open(base, dst, KEY_ALL_ACCESS, d);
        if (status != ERROR_SUCCESS) return FromStatus(status);
    } else {
        DWORD disposition = 0;
        const LSTATUS status = RegCreateKeyExW(base, dst.c_str(), 0, nullptr, REG_OPTION_NON_VOLATILE,
                                               KEY_ALL_ACCESS, nullptr, &d.h, &disposition);
        if (status != ERROR_SUCCESS) return FromStatus(status);
        if (disposition != REG_CREATED_NEW_KEY) return OpResult::Conflict;
    }
    LSTATUS status = copy ? CopyTree(roots, root, src, dst, s.h, d.h) : ERROR_SUCCESS;
    if (status != ERROR_SUCCESS) {
        if (existing) return MoveFailure(status, false);
        RegCloseKey(d.h); d.h = nullptr;
        const LSTATUS cleanup = DeleteTree(roots, root, src, dst, dst);
        return MoveFailure(status, cleanup == ERROR_SUCCESS || MissingKey(cleanup));
    }
    if (existing) {
        TreeSnapshot source, target;
        size_t bytes = 0;
        status = CaptureTree(s.h, L"", source, bytes);
        if (status == ERROR_SUCCESS) status = CaptureTree(d.h, L"", target, bytes);
        if (status != ERROR_SUCCESS) return MoveFailure(status, false);
        if (!TreeSubset(source, target)) return MoveFailure(ERROR_TRANSACTIONAL_CONFLICT, false);
    }
    RegCloseKey(s.h); s.h = nullptr;
    status = DeleteTree(roots, root, src, dst, src);
    if (status != ERROR_SUCCESS) {
        // An existing recovery destination is the tree we are restoring. Keep
        // it even if retiring the recovery copy only partially succeeds.
        if (existing) return MoveFailure(status, false);
        Key back;
        bool restored = RegCreateKeyExW(base, src.c_str(), 0, nullptr, REG_OPTION_NON_VOLATILE, KEY_ALL_ACCESS,
                                        nullptr, &back.h, nullptr) == ERROR_SUCCESS &&
                        CopyTree(roots, root, src, dst, d.h, back.h) == ERROR_SUCCESS;
        RegCloseKey(d.h); d.h = nullptr;
        if (restored) {
            const LSTATUS cleanup = DeleteTree(roots, root, src, dst, dst);
            restored = cleanup == ERROR_SUCCESS || MissingKey(cleanup);
        }
        return MoveFailure(status, restored);
    }
    return OpResult::Ok;
}

// Removes the empty parents of `path` up to (not including) `stop`.
void PruneEmptyParents(HKEY base, std::wstring path, const std::wstring& stop) {
    for (;;) {
        const size_t slash = path.rfind(L'\\');
        if (slash == std::wstring::npos) return;
        path.resize(slash);
        if (path.size() <= stop.size() || !IsEmptyKey(base, path)) return;
        if (RegDeleteKeyW(base, path.c_str()) != ERROR_SUCCESS) return;
    }
}

// Moves <parent>\\<from>\\<name> to <parent>\\<to>\\<name>, keeping values and subkeys.
OpResult MoveKey(const Roots& roots, Root root, const std::wstring& src_rel, const std::wstring& dst_rel, bool recovery) {
    HKEY base = RootKey(roots, root);
    const std::wstring src = FullPath(roots, root, src_rel);
    const OpResult moved = MoveTree(roots, root, src, FullPath(roots, root, dst_rel), recovery);
    if (moved != OpResult::Ok) return moved;
    // An emptied (-)ContextMenuHandlers group goes too, so a round trip leaves
    // no trace.
    const std::wstring group = src.substr(0, src.rfind(L'\\'));
    if (IsEmptyKey(base, group)) RegDeleteKeyW(base, group.c_str());
    return OpResult::Ok;
}

const std::wstring& TrashBase(const Roots& r, Root root) {
    return root == Root::Machine ? r.machine_trash : r.user_trash;
}

// 删除残留项 and its undo. The trash path comes from the checked key path
// only; nothing from outside names it.
OpResult ApplyTrash(const Roots& roots, const Op& op) {
    HKEY base = RootKey(roots, op.root);
    const std::wstring& trash_base = TrashBase(roots, op.root);
    if (trash_base.empty()) return OpResult::Unsupported;
    const std::wstring live = FullPath(roots, op.root, op.key_path);
    const std::wstring trash = Join(trash_base, op.key_path);
    if (op.action == Action::Delete) {
        // Keep an earlier deletion available to its original undo operation.
        const OpResult moved = MoveTree(roots, op.root, live, trash, op.recover_tree);
        if (moved != OpResult::Ok) return moved;
        // Like MoveKey: an emptied shell / ContextMenuHandlers group goes too.
        const std::wstring group = live.substr(0, live.rfind(L'\\'));
        if (IsEmptyKey(base, group)) RegDeleteKeyW(base, group.c_str());
        return OpResult::Ok;
    }
    const OpResult moved = MoveTree(roots, op.root, trash, live, op.recover_tree);
    if (moved == OpResult::Ok) PruneEmptyParents(base, trash, trash_base);
    return moved;
}

std::vector<std::wstring> Split(std::wstring_view path) {
    std::vector<std::wstring> parts;
    size_t start = 0;
    while (start <= path.size()) {
        const size_t end = path.find(L'\\', start);
        parts.emplace_back(path.substr(start, end == std::wstring_view::npos ? std::wstring_view::npos : end - start));
        if (end == std::wstring_view::npos) break;
        start = end + 1;
    }
    return parts;
}

const wchar_t* KindToken(Kind k) {
    switch (k) {
    case Kind::Verb: return L"verb";
    case Kind::ShellEx: return L"shellex";
    case Kind::DragDrop: return L"dragdrop";
    case Kind::ShellNew: return L"shellnew";
    case Kind::SendTo: return L"sendto";
    case Kind::OpenWith: return L"openwith";
    }
    return L"";
}

const wchar_t* SnapshotToken(ValueName name) {
    switch (name) {
    case ValueName::Velocity: return L"velocity";
    case ValueName::LegacyDisable: return L"legacy";
    case ValueName::ProgrammaticAccessOnly: return L"programmatic";
    case ValueName::CommandFlags: return L"flags";
    case ValueName::Position: return L"position";
    }
    return L"";
}

bool ParseUnsigned(std::wstring_view text, DWORD& value) {
    if (text.empty() || text.size() > 10) return false;
    value = 0;
    for (const wchar_t c : text) {
        if (c < L'0' || c > L'9') return false;
        const DWORD digit = static_cast<DWORD>(c - L'0');
        if (value > ((std::numeric_limits<DWORD>::max)() - digit) / 10) return false;
        value = value * 10 + digit;
    }
    return true;
}

bool ParsePlace(std::wstring_view text, Place& place) {
    if (text == L"top") place = Place::Top;
    else if (text == L"bottom") place = Place::Bottom;
    else if (text == L"default") place = Place::Default;
    else return false;
    return true;
}

void AppendSnapshot(std::wstring& out, const Op& op) {
    if (!op.has_previous_values) { out += L'-'; return; }
    out += op.restore_previous_values ? L"restore|" : L"save|";
    static const wchar_t kHex[] = L"0123456789abcdef";
    bool first = true;
    for (const auto& value : op.previous_values) {
        if (!first) out += L';';
        first = false;
        out += SnapshotToken(value.name);
        out += L':';
        if (!value.present) { out += L'-'; continue; }
        out += std::to_wstring(value.type) + L":";
        for (const BYTE b : value.data) {
            out += kHex[b >> 4];
            out += kHex[b & 0xF];
        }
    }
}

int HexDigit(wchar_t c) {
    if (c >= L'0' && c <= L'9') return c - L'0';
    if (c >= L'a' && c <= L'f') return c - L'a' + 10;
    if (c >= L'A' && c <= L'F') return c - L'A' + 10;
    return -1;
}

bool ParseSnapshot(std::wstring_view text, Op& op) {
    if (text == L"-") return true;
    const size_t bar = text.find(L'|');
    if (bar == std::wstring_view::npos) return false;
    const auto mode = text.substr(0, bar);
    if (mode != L"save" && mode != L"restore") return false;
    op.has_previous_values = true;
    op.restore_previous_values = mode == L"restore";
    text.remove_prefix(bar + 1);
    while (!text.empty()) {
        if (op.previous_values.size() == 5) return false;
        const size_t end = text.find(L';');
        const auto entry = text.substr(0, end);
        const size_t colon = entry.find(L':');
        if (colon == std::wstring_view::npos) return false;
        ValueSnapshot value;
        bool named = false;
        for (const auto name : {ValueName::Velocity, ValueName::LegacyDisable, ValueName::ProgrammaticAccessOnly,
                                ValueName::CommandFlags, ValueName::Position}) {
            if (entry.substr(0, colon) == SnapshotToken(name)) { value.name = name; named = true; break; }
        }
        if (!named) return false;
        const auto raw = entry.substr(colon + 1);
        if (raw != L"-") {
            const size_t data_at = raw.find(L':');
            if (data_at == std::wstring_view::npos || !ParseUnsigned(raw.substr(0, data_at), value.type)) return false;
            const auto hex = raw.substr(data_at + 1);
            if (hex.size() % 2 || hex.size() / 2 > kMaxValueSnapshotBytes) return false;
            value.present = true;
            value.data.reserve(hex.size() / 2);
            for (size_t i = 0; i < hex.size(); i += 2) {
                const int hi = HexDigit(hex[i]), lo = HexDigit(hex[i + 1]);
                if (hi < 0 || lo < 0) return false;
                value.data.push_back(static_cast<BYTE>((hi << 4) | lo));
            }
        }
        op.previous_values.push_back(std::move(value));
        if (end == std::wstring_view::npos) break;
        text.remove_prefix(end + 1);
        if (text.empty()) return false;
    }
    return ValidSnapshots(op);
}

std::wstring RootName(HKEY h) {
    if (h == HKEY_LOCAL_MACHINE) return L"HKEY_LOCAL_MACHINE";
    if (h == HKEY_CURRENT_USER) return L"HKEY_CURRENT_USER";
    if (h == HKEY_CLASSES_ROOT) return L"HKEY_CLASSES_ROOT";
    if (h == HKEY_USERS) return L"HKEY_USERS";
    return L"HKEY_CURRENT_USER";
}

void AppendHex(std::wstring& out, const BYTE* data, DWORD size) {
    static const wchar_t kHex[] = L"0123456789abcdef";
    size_t col = out.size() - out.rfind(L'\n');
    for (DWORD i = 0; i < size; ++i) {
        if (i) { out += L','; ++col; }
        if (col > 76) { out += L"\\\r\n  "; col = 2; }
        out += kHex[data[i] >> 4];
        out += kHex[data[i] & 0xF];
        col += 2;
    }
}

bool ExportKey(HKEY root, const std::wstring& path, std::wstring& out, DWORD& error) {
    Key k;
    const LSTATUS opened = Open(root, path, KEY_READ, k);
    if (opened != ERROR_SUCCESS) { error = opened; return false; }
    out += L"[" + RootName(root) + L"\\" + path + L"]\r\n";
    std::vector<wchar_t> name(16384);
    std::vector<BYTE> data;
    for (DWORD i = 0;; ++i) {
        DWORD name_len = static_cast<DWORD>(name.size()), type = 0, size = 0;
        LSTATUS st = RegEnumValueW(k.h, i, name.data(), &name_len, nullptr, &type, nullptr, &size);
        if (st == ERROR_NO_MORE_ITEMS) break;
        if (st != ERROR_SUCCESS) { error = st; return false; }
        data.assign(size + sizeof(wchar_t), 0);
        name_len = static_cast<DWORD>(name.size());
        st = RegEnumValueW(k.h, i, name.data(), &name_len, nullptr, &type, data.data(), &size);
        if (st != ERROR_SUCCESS) { error = st; return false; }
        std::wstring line;
        if (name_len == 0) line = L"@=";
        else {
            line = L"\"";
            for (DWORD c = 0; c < name_len; ++c) {
                if (name[c] == L'\\' || name[c] == L'"') line += L'\\';
                line += name[c];
            }
            line += L"\"=";
        }
        bool plain = type == REG_SZ && size >= sizeof(wchar_t) && size % sizeof(wchar_t) == 0;
        const wchar_t* text = reinterpret_cast<const wchar_t*>(data.data());
        const size_t chars = size / sizeof(wchar_t);
        if (plain) {
            plain = text[chars - 1] == L'\0';
            for (size_t c = 0; c + 1 < chars; ++c)
                if (!text[c] || text[c] == L'\r' || text[c] == L'\n') plain = false;
        }
        if (plain) {
            line += L'"';
            for (size_t c = 0; c < chars && text[c]; ++c) {
                if (text[c] == L'\\' || text[c] == L'"') line += L'\\';
                line += text[c];
            }
            line += L'"';
        } else if (type == REG_DWORD && size == sizeof(DWORD)) {
            wchar_t buf[16];
            swprintf_s(buf, L"dword:%08lx", *reinterpret_cast<const DWORD*>(data.data()));
            line += buf;
        } else {
            if (type == REG_BINARY) line += L"hex:";
            else {
                wchar_t buf[16];
                swprintf_s(buf, L"hex(%lx):", type);
                line += buf;
            }
            out += line;
            AppendHex(out, data.data(), size);
            out += L"\r\n";
            continue;
        }
        out += line + L"\r\n";
    }
    out += L"\r\n";
    for (DWORD i = 0;; ++i) {
        wchar_t sub[256];
        DWORD length = ARRAYSIZE(sub);
        const LSTATUS status = RegEnumKeyExW(k.h, i, sub, &length, nullptr, nullptr, nullptr, nullptr);
        if (status == ERROR_NO_MORE_ITEMS) break;
        if (status != ERROR_SUCCESS) { error = status; return false; }
        if (!ExportKey(root, path + L"\\" + std::wstring(sub, length), out, error)) return false;
    }
    return true;
}

} // namespace

void SetVelocitySupportedForTesting(int supported) { g_velocity_override = supported; }

const std::vector<Scene>& BuiltinScenes() {
    static const std::vector<Scene> scenes = {
        {L"file", {L"*"}},
        {L"allobj", {L"AllFilesystemObjects"}},
        {L"folder", {L"Directory", L"Folder"}},
        {L"dirbg", {L"Directory\\Background"}},
        {L"desk", {L"DesktopBackground"}},
        {L"drive", {L"Drive"}},
        {L"pc", {L"CLSID\\{20D04FE0-3AEA-1069-A2D8-08002B30309D}"}},
        {L"bin", {L"CLSID\\{645FF040-5081-101B-9F08-00AA002F954E}"}},
        {L"lnk", {L"lnkfile"}},
        {L"exe", {L"exefile"}},
        {L"shellnew", {}, SceneMode::ShellNew},
        {L"sendto", {}, SceneMode::SendTo},
        {L"openwith", {}, SceneMode::OpenWith},
        {L"dragdrop", {L"Directory", L"Drive", L"Folder", L"*"}, SceneMode::DragDrop},
    };
    return scenes;
}

Scene ExtensionScene(const Roots& roots, std::wstring_view ext) {
    Scene s;
    s.id = L"ext:" + Lower(ext);
    s.class_keys.push_back(L"SystemFileAssociations\\" + std::wstring(ext));
    // The user's choice decides which ProgID's verbs Explorer shows; the
    // registered default still applies when there is none.
    for (const auto& id : ExtensionProgIds(roots, std::wstring(ext)))
        if (MergedKeyExists(roots, id)) s.class_keys.push_back(id);
    return s;
}

Scene PerceivedScene(std::wstring_view type) {
    Scene s;
    s.id = L"perc:" + Lower(type);
    s.class_keys.push_back(L"SystemFileAssociations\\" + std::wstring(type));
    return s;
}

std::vector<Item> Enumerate(const Roots& roots, const Scene& scene) {
    std::vector<Item> raw;
    switch (scene.mode) {
    case SceneMode::ShellNew: ScanShellNew(roots, scene, raw); break;
    case SceneMode::SendTo: ScanSendTo(roots, scene, raw); break;
    case SceneMode::OpenWith: ScanOpenWith(roots, scene, raw); break;
    case SceneMode::DragDrop:
        for (const auto& cls : scene.class_keys)
            for (const Root root : {Root::User, Root::Machine})
                ScanHandlers(roots, scene, cls, root, L"DragDropHandlers", Kind::DragDrop, raw);
        break;
    case SceneMode::Menu:
        for (const auto& cls : scene.class_keys)
            for (const Root root : {Root::User, Root::Machine}) {
                ScanVerbs(roots, scene, cls, root, raw);
                ScanHandlers(roots, scene, cls, root, L"ContextMenuHandlers", Kind::ShellEx, raw);
            }
        break;
    }
    // HKCR shows one entry per key; the user layer wins over the machine one.
    std::vector<Item> out;
    std::set<std::wstring> seen;
    for (auto& it : raw) {
        std::wstring id = Lower(it.key_path);
        if (it.kind == Kind::ShellEx || it.kind == Kind::DragDrop) {
            const std::wstring normal = ReplaceSegment(it.key_path, std::wstring(L"-") + GroupName(it.kind),
                                                       GroupName(it.kind));
            if (!normal.empty()) id = Lower(normal);
        } else if (it.kind == Kind::ShellNew && it.dash_marker) {
            id = Lower(it.key_path.substr(0, it.key_path.size() - 10)) + L"\\shellnew";
        }
        id = std::wstring(KindToken(it.kind)) + L"|" + id;
        if (!seen.insert(id).second) continue;
        out.push_back(std::move(it));
    }
    return out;
}

namespace {
// FileName: absolute (after %VAR% expansion), else looked up the way
// Explorer does: the user's Templates, the common Templates, %WINDIR%\ShellNew.
std::wstring ResolveTemplateFile(const std::wstring& raw) {
    if (raw.empty()) return {};
    wchar_t expanded[MAX_PATH * 2]{};
    const DWORD n = ExpandEnvironmentStringsW(raw.c_str(), expanded, ARRAYSIZE(expanded));
    std::wstring name = n > 0 && n <= ARRAYSIZE(expanded) ? expanded : raw;
    if (!PathIsRelativeW(name.c_str())) return FileExists(name) ? name : std::wstring();
    std::vector<std::wstring> dirs;
    for (const KNOWNFOLDERID* id : {&FOLDERID_Templates, &FOLDERID_CommonTemplates}) {
        PWSTR dir = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(*id, KF_FLAG_DONT_VERIFY, nullptr, &dir)) && dir) dirs.emplace_back(dir);
        CoTaskMemFree(dir);
    }
    wchar_t windows[MAX_PATH]{};
    if (GetWindowsDirectoryW(windows, ARRAYSIZE(windows))) dirs.push_back(std::wstring(windows) + L"\\ShellNew");
    for (const auto& dir : dirs) {
        const std::wstring candidate = dir + L"\\" + name;
        if (FileExists(candidate)) return candidate;
    }
    return {};
}

std::vector<BYTE> ReadData(HKEY key) {
    DWORD type = 0, bytes = 0;
    if (RegQueryValueExW(key, L"Data", nullptr, &type, nullptr, &bytes) != ERROR_SUCCESS || bytes == 0) return {};
    std::vector<BYTE> raw(bytes);
    if (RegQueryValueExW(key, L"Data", nullptr, &type, raw.data(), &bytes) != ERROR_SUCCESS) return {};
    raw.resize(bytes);
    if (type != REG_SZ && type != REG_EXPAND_SZ) return raw;
    // A string Data value is written as ANSI text, without the terminator.
    std::wstring text(reinterpret_cast<const wchar_t*>(raw.data()), raw.size() / sizeof(wchar_t));
    text.resize(wcsnlen(text.c_str(), text.size()));
    if (text.empty()) return {};
    const int size = WideCharToMultiByte(CP_ACP, 0, text.c_str(), static_cast<int>(text.size()),
                                         nullptr, 0, nullptr, nullptr);
    std::vector<BYTE> out(static_cast<size_t>(std::max(size, 0)));
    if (size > 0)
        WideCharToMultiByte(CP_ACP, 0, text.c_str(), static_cast<int>(text.size()),
                            reinterpret_cast<char*>(out.data()), size, nullptr, nullptr);
    return out;
}
} // namespace

bool CanCreateNewTemplate(const NewTemplate& t) {
    if (t.ext.size() < 2 || t.ext[0] != L'.' || t.display.empty()) return false;
    if (IEquals(t.ext, L".lnk")) return true;
    if (t.handler) return false;
    if (!t.missing_file.empty() && t.data.empty() && !t.null_file && t.command.empty()) return false;
    return !t.template_file.empty() || !t.data.empty() || t.null_file || !t.command.empty();
}

std::vector<NewTemplate> ListNewTemplates(const Roots& roots) {
    Scene scene;
    scene.id = L"shellnew";
    scene.mode = SceneMode::ShellNew;
    std::vector<NewTemplate> out;
    std::set<std::wstring> seen;
    for (const Item& it : Enumerate(roots, scene)) {
        if (it.kind != Kind::ShellNew || !it.enabled || it.dash_marker) continue;
        if (it.class_key.empty() || it.class_key[0] != L'.') continue;   // Folder
        if (it.missing) continue;                                         // Command exe is gone
        if (!seen.insert(Lower(it.class_key)).second) continue;           // user layer first
        Key sn;
        if (Open(RootKey(roots, it.root), FullPath(roots, it.root, it.key_path), KEY_READ, sn) != ERROR_SUCCESS)
            continue;
        NewTemplate t;
        t.ext = it.class_key;
        t.display = StripMnemonic(it.display);
        if (IEquals(t.display, t.ext) && roots.resolve_strings) {
            // No ItemName / type name in the registry: Explorer shows the
            // shell's type text ("MXG 文件").
            SHFILEINFOW info{};
            if (SHGetFileInfoW(t.ext.c_str(), FILE_ATTRIBUTE_NORMAL, &info, sizeof(info),
                               SHGFI_USEFILEATTRIBUTES | SHGFI_TYPENAME) && info.szTypeName[0])
                t.display = info.szTypeName;
        }
        t.command = it.command;
        t.null_file = HasValue(sn.h, L"NullFile");
        t.handler = HasValue(sn.h, L"Handler");
        t.data = ReadData(sn.h);
        std::wstring file;
        if (ReadString(sn.h, L"FileName", file) && !file.empty()) {
            t.template_file = ResolveTemplateFile(file);
            if (t.template_file.empty()) t.missing_file = file;
        }
        if (CanCreateNewTemplate(t)) out.push_back(std::move(t));
    }
    std::sort(out.begin(), out.end(), [](const NewTemplate& a, const NewTemplate& b) {
        return CompareStringEx(LOCALE_NAME_USER_DEFAULT, NORM_IGNORECASE | SORT_DIGITSASNUMBERS,
                               a.display.c_str(), -1, b.display.c_str(), -1, nullptr, nullptr, 0) == CSTR_LESS_THAN;
    });
    return out;
}

Op MakeOp(const Item& item, Action action) {
    Op op;
    op.action = action;
    op.kind = item.kind;
    op.root = item.root;
    op.key_path = item.key_path;
    return op;
}

Op MakePositionOp(const Item& item, Place place) {
    Op op = MakeOp(item, Action::Position);
    op.place = place;
    op.prev_place = item.position;
    return op;
}

void PlanOp(const Roots& roots, Op& op) {
    if (op.kind != Kind::Verb || op.root == Root::File) return;
    if (op.action == Action::Delete || op.action == Action::Restore) return;  // moves the whole key
    if (op.restore_previous_values) return;
    op.planned = false;
    op.has_previous_values = false;
    op.previous_values.clear();
    op.plan_error = ERROR_SUCCESS;
    Key v;
    op.plan_error = Open(RootKey(roots, op.root), FullPath(roots, op.root, op.key_path), KEY_QUERY_VALUE, v);
    if (op.plan_error != ERROR_SUCCESS) return;
    if (op.action == Action::Position) {
        op.prev_place = ReadPlace(v.h);
        op.mask = 0;
    } else {
        op.mask = PlanVerb(v.h, op);
    }
    const unsigned snapshot_mask = op.action == Action::Position ? 0x10 : op.mask;
    for (const auto name : {ValueName::Velocity, ValueName::LegacyDisable, ValueName::ProgrammaticAccessOnly,
                            ValueName::CommandFlags, ValueName::Position}) {
        if (!(snapshot_mask & SnapshotValueBit(name))) continue;
        ValueSnapshot value;
        op.plan_error = CaptureValue(v.h, name, value);
        if (op.plan_error != ERROR_SUCCESS) { op.previous_values.clear(); return; }
        op.previous_values.push_back(std::move(value));
    }
    op.has_previous_values = true;
    op.planned = true;
}

std::wstring KeyPathAfter(const Op& op) {
    if (op.kind != Kind::ShellEx && op.kind != Kind::DragDrop) return op.key_path;
    if (op.action != Action::Disable && op.action != Action::Enable) return op.key_path;
    const std::wstring group = GroupName(op.kind);
    const std::wstring dashed = L"-" + group;
    const std::wstring moved = op.action == Action::Disable ? ReplaceSegment(op.key_path, group, dashed)
                                                            : ReplaceSegment(op.key_path, dashed, group);
    return moved.empty() ? op.key_path : moved;
}

Op Inverse(const Op& op) {
    Op inv = op;
    if (op.has_previous_values) inv.restore_previous_values = !op.restore_previous_values;
    if (op.action == Action::Position) {
        inv.place = op.prev_place;
        inv.prev_place = op.place;
        return inv;
    }
    if (op.action == Action::Delete || op.action == Action::Restore) {
        inv.action = op.action == Action::Delete ? Action::Restore : Action::Delete;
        return inv;
    }
    inv.action = op.action == Action::Disable ? Action::Enable : Action::Disable;
    inv.key_path = KeyPathAfter(op);
    return inv;
}

bool CanWriteWithoutElevation(const Op& op) { return op.root != Root::Machine; }

OpResult Apply(const Roots& roots, const Op& op) {
    if (op.root == Root::File) return OpResult::Unsupported;
    if (op.plan_error != ERROR_SUCCESS) return FromStatus(op.plan_error);
    if (!IsAllowedKeyPath(op.kind, op.key_path)) return OpResult::Unsupported;
    if (!ValidSnapshots(op)) return OpResult::Unsupported;
    if (op.action == Action::Position && op.kind != Kind::Verb) return OpResult::Unsupported;
    if (op.action == Action::Delete || op.action == Action::Restore) {
        if (op.kind != Kind::Verb && op.kind != Kind::ShellEx && op.kind != Kind::DragDrop) return OpResult::Unsupported;
        return ApplyTrash(roots, op);
    }
    switch (op.kind) {
    case Kind::Verb: return ApplyVerb(roots, op);
    case Kind::ShellEx:
    case Kind::DragDrop: {
        const std::wstring target = KeyPathAfter(op);
        if (IEquals(target, op.key_path)) return OpResult::Ok;  // already in the requested state
        return MoveKey(roots, op.root, op.key_path, target, op.recover_tree);
    }
    default: return OpResult::Unsupported;  // ShellNew / SendTo / OpenWith: S3
    }
}

bool IsAllowedKeyPath(Kind kind, std::wstring_view key_path) {
    if (key_path.empty() || key_path.size() > 512) return false;
    if (key_path.front() == L'\\' || key_path.find(L"..") != std::wstring_view::npos ||
        key_path.find(L':') != std::wstring_view::npos || key_path.find(L'/') != std::wstring_view::npos)
        return false;
    for (const wchar_t c : key_path) if (c < 0x20) return false;
    const auto parts = Split(key_path);
    for (const auto& p : parts) if (p.empty()) return false;
    const size_t n = parts.size();
    switch (kind) {
    case Kind::Verb:
        return n >= 3 && IEquals(parts[n - 2], L"shell");
    case Kind::ShellEx:
    case Kind::DragDrop: {
        const std::wstring group = GroupName(kind);
        return n >= 4 && IEquals(parts[n - 3], L"shellex") &&
               (IEquals(parts[n - 2], group) || IEquals(parts[n - 2], L"-" + group));
    }
    default: return false;
    }
}

std::wstring SerializeBatch(const std::vector<Op>& ops) {
    if (ops.size() > kMaxBatchOperations) return {};
    std::wstring out = std::wstring(kBatchHeader) + L"\n";
    for (const auto& op : ops) {
        if (op.plan_error != ERROR_SUCCESS || !IsAllowedKeyPath(op.kind, op.key_path) || !ValidSnapshots(op) ||
            (op.root != Root::User && op.root != Root::Machine)) return {};
        switch (op.action) {
        case Action::Disable: out += L"disable\t"; break;
        case Action::Enable: out += L"enable\t"; break;
        case Action::Delete: out += L"delete\t"; break;
        case Action::Restore: out += L"restore\t"; break;
        case Action::Position: out += L"position\t"; break;
        default: return {};
        }
        if ((op.action == Action::Position || op.planned) && op.kind != Kind::Verb) return {};
        if (op.planned && (op.action == Action::Delete || op.action == Action::Restore)) return {};
        out += KindToken(op.kind);
        out += op.root == Root::Machine ? L"\tmachine\t" : L"\tuser\t";
        out += op.key_path + L"\t";
        if (op.action == Action::Position) {
            out += PlaceToken(op.place);
            if (op.planned) out += std::wstring(L",") + PlaceToken(op.prev_place);
        } else if (op.kind == Kind::Verb && op.planned) {
            if (op.mask > 0xF) return {};
            out += std::to_wstring(op.mask);
        } else out += op.recover_tree ? L"recover" : L"-";
        out += L'\t';
        AppendSnapshot(out, op);
        out += L'\n';
        if (out.size() > kMaxBatchBytes) return {};
    }
    const int bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, out.data(), static_cast<int>(out.size()),
                                          nullptr, 0, nullptr, nullptr);
    if (bytes <= 0 || static_cast<size_t>(bytes) > kMaxBatchBytes) return {};
    return out;
}

bool ParseBatch(std::wstring_view text, std::vector<Op>& ops) {
    ops.clear();
    if (text.empty() || text.size() > kMaxBatchBytes) return false;
    const int bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
                                          nullptr, 0, nullptr, nullptr);
    if (bytes <= 0 || static_cast<size_t>(bytes) > kMaxBatchBytes) return false;
    std::vector<Op> parsed;
    size_t pos = 0;
    bool header = false, legacy = false;
    while (pos < text.size()) {
        size_t end = text.find(L'\n', pos);
        if (end == std::wstring_view::npos) end = text.size();
        std::wstring_view line = text.substr(pos, end - pos);
        pos = end + 1;
        if (!line.empty() && line.back() == L'\r') line.remove_suffix(1);
        if (!header) {
            if (!line.empty() && line.front() == 0xFEFF) line.remove_prefix(1);
            legacy = line == kLegacyBatchHeader;
            if (!legacy && line != kBatchHeader) return false;
            header = true;
            continue;
        }
        if (line.empty()) continue;
        if (parsed.size() == kMaxBatchOperations) return false;
        std::vector<std::wstring_view> fields;
        size_t start = 0;
        for (;;) {
            const size_t tab = line.find(L'\t', start);
            fields.push_back(line.substr(start, tab == std::wstring_view::npos ? tab : tab - start));
            if (fields.size() > 6) return false;
            if (tab == std::wstring_view::npos) break;
            start = tab + 1;
        }
        if (legacy ? fields.size() < 4 || fields.size() > 5 : fields.size() != 6) return false;
        Op op;
        if (fields[0] == L"disable") op.action = Action::Disable;
        else if (fields[0] == L"enable") op.action = Action::Enable;
        else if (fields[0] == L"delete") op.action = Action::Delete;
        else if (fields[0] == L"restore") op.action = Action::Restore;
        else if (fields[0] == L"position") op.action = Action::Position;
        else return false;
        if (fields[1] == L"verb") op.kind = Kind::Verb;
        else if (fields[1] == L"shellex") op.kind = Kind::ShellEx;
        else if (fields[1] == L"dragdrop") op.kind = Kind::DragDrop;
        else return false;
        if (fields[2] == L"machine") op.root = Root::Machine;
        else if (fields[2] == L"user") op.root = Root::User;
        else return false;
        op.key_path = fields[3];
        if (!IsAllowedKeyPath(op.kind, op.key_path)) return false;
        const auto parameter = fields.size() >= 5 ? fields[4] : std::wstring_view();
        if (op.action == Action::Position) {
            const size_t comma = legacy ? std::wstring_view::npos : parameter.find(L',');
            if (!ParsePlace(parameter.substr(0, comma), op.place)) return false;
            if (comma != std::wstring_view::npos) {
                if (!ParsePlace(parameter.substr(comma + 1), op.prev_place)) return false;
                op.planned = true;
            }
        } else if (!legacy && parameter == L"recover") {
            op.recover_tree = true;
        } else if (!parameter.empty() && (legacy || parameter != L"-")) {
            DWORD mask = 0;
            if (parameter.size() > 2 || !ParseUnsigned(parameter, mask) || mask > 0xF) return false;
            op.mask = mask;
            op.planned = true;
        } else if (!legacy && parameter != L"-") return false;
        if ((op.planned || op.action == Action::Position) && op.kind != Kind::Verb) return false;
        if (op.planned && (op.action == Action::Delete || op.action == Action::Restore)) return false;
        if (!legacy && !ParseSnapshot(fields[5], op)) return false;
        if (!ValidSnapshots(op)) return false;
        parsed.push_back(std::move(op));
    }
    if (!header) return false;
    ops = std::move(parsed);
    return true;
}

std::wstring ExportReg(const Roots& roots, const std::vector<Op>& ops) {
    std::wstring out = L"Windows Registry Editor Version 5.00\r\n\r\n";
    DWORD error = ERROR_SUCCESS;
    std::map<std::wstring, std::wstring> locations;
    std::set<std::wstring> exported;
    for (const auto& op : ops) {
        if (op.root == Root::File) continue;
        const HKEY root = RootKey(roots, op.root);
        const std::wstring path = op.action == Action::Restore ? Join(TrashBase(roots, op.root), op.key_path)
                                                               : FullPath(roots, op.root, op.key_path);
        const std::wstring key = RootName(root) + L"\\" + Lower(path);
        const auto previous = locations.find(key);
        // RestoreAll can reverse several moves of the same registration. A
        // later source then refers to the original tree exported earlier.
        std::wstring original = previous == locations.end() ? path : previous->second;
        const std::wstring recovery_target = op.action == Action::Delete ? Join(TrashBase(roots, op.root), op.key_path)
                                            : op.action == Action::Restore ? FullPath(roots, op.root, op.key_path)
                                            : FullPath(roots, op.root, KeyPathAfter(op));
        if (op.recover_tree && previous == locations.end()) {
            Key probe;
            const LSTATUS source_status = Open(root, path, KEY_READ, probe);
            if (MissingKey(source_status)) original = recovery_target;
            else if (source_status != ERROR_SUCCESS) { SetLastError(source_status); return {}; }
            Key target;
            const LSTATUS target_status = Open(root, recovery_target, KEY_READ, target);
            if (target_status == ERROR_SUCCESS) {
                const std::wstring target_key = RootName(root) + L"\\" + Lower(recovery_target);
                if (!exported.contains(target_key) && !ExportKey(root, recovery_target, out, error)) {
                    SetLastError(error); return {};
                }
                exported.insert(target_key);
            } else if (!MissingKey(target_status)) { SetLastError(target_status); return {}; }
        }
        if (original.empty()) { SetLastError(ERROR_FILE_NOT_FOUND); return {}; }
        const std::wstring export_key = RootName(root) + L"\\" + Lower(original);
        if (!exported.contains(export_key) && !ExportKey(root, original, out, error)) {
            SetLastError(error);
            return {};
        }
        exported.insert(export_key);
        const std::wstring target = op.action == Action::Delete ? Join(TrashBase(roots, op.root), op.key_path)
                                      : op.action == Action::Restore ? FullPath(roots, op.root, op.key_path)
                                      : FullPath(roots, op.root, KeyPathAfter(op));
        locations[key] = IEquals(path, target) ? original : std::wstring();
        locations[RootName(root) + L"\\" + Lower(target)] = original;
    }
    SetLastError(ERROR_SUCCESS);
    return out;
}

bool WriteRegBackup(const Roots& roots, const std::vector<Op>& ops,
                    const std::wstring& path, DWORD& error) {
    error = ERROR_SUCCESS;
    if (ops.empty() || path.empty()) { error = ERROR_INVALID_PARAMETER; return false; }
    std::wstring text = ExportReg(roots, ops);
    if (text.empty()) { error = GetLastError(); return false; }
    text.insert(text.begin(), L'\xFEFF');
    const size_t bytes = text.size() * sizeof(wchar_t);
    if (bytes > (std::numeric_limits<DWORD>::max)()) { error = ERROR_FILE_TOO_LARGE; return false; }
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                               CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) { error = GetLastError(); return false; }
    DWORD written = 0;
    bool ok = WriteFile(file, text.data(), static_cast<DWORD>(bytes), &written, nullptr) != FALSE;
    if (!ok) error = GetLastError();
    else if (written != bytes) { ok = false; error = ERROR_WRITE_FAULT; }
    if (ok && !FlushFileBuffers(file)) { ok = false; error = GetLastError(); }
    if (!CloseHandle(file) && ok) { ok = false; error = GetLastError(); }
    if (!ok) DeleteFileW(path.c_str());
    return ok;
}

namespace {

constexpr std::pair<OpResult, const wchar_t*> kResultNames[] = {
    {OpResult::Ok, L"ok"},           {OpResult::NotFound, L"notfound"},
    {OpResult::Conflict, L"conflict"}, {OpResult::AccessDenied, L"denied"},
    {OpResult::Unsupported, L"unsupported"}, {OpResult::Failed, L"failed"},
    {OpResult::RecoveryRequired, L"recovery_required"},
};

bool ReadUtf8File(const std::wstring& path, std::wstring& text) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    std::string bytes;
    bool ok = GetFileSizeEx(f, &size) && size.QuadPart >= 0 &&
              static_cast<ULONGLONG>(size.QuadPart) <= kMaxBatchBytes;
    if (ok) {
        bytes.resize(static_cast<size_t>(size.QuadPart));
        DWORD read = 0;
        ok = bytes.empty() || (ReadFile(f, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr) &&
                               read == bytes.size());
    }
    CloseHandle(f);
    if (!ok) return false;
    size_t skip = bytes.rfind("\xEF\xBB\xBF", 0) == 0 ? 3 : 0;
    text.clear();
    if (bytes.size() == skip) return true;
    const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data() + skip,
                                      static_cast<int>(bytes.size() - skip), nullptr, 0);
    if (n <= 0) return false;
    text.resize(static_cast<size_t>(n));
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data() + skip, static_cast<int>(bytes.size() - skip),
                        text.data(), n);
    return true;
}

} // namespace

const wchar_t* ResultName(OpResult result) {
    for (const auto& [r, name] : kResultNames) if (r == result) return name;
    return L"failed";
}

std::wstring SerializeResults(const std::vector<OpResult>& results) {
    std::wstring out = L"PULSE-SYSCTX-RESULT 1\n";
    for (const OpResult r : results) {
        out += ResultName(r);
        out += L"\n";
    }
    return out;
}

bool ParseResults(std::wstring_view text, std::vector<OpResult>& results) {
    results.clear();
    bool header = false;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find(L'\n', pos);
        if (end == std::wstring_view::npos) end = text.size();
        std::wstring_view line = text.substr(pos, end - pos);
        pos = end + 1;
        if (!line.empty() && line.back() == L'\r') line.remove_suffix(1);
        if (line.empty()) continue;
        if (!header) {
            if (line != L"PULSE-SYSCTX-RESULT 1") return false;
            header = true;
            continue;
        }
        bool known = false;
        for (const auto& [r, name] : kResultNames) {
            if (line == name) {
                results.push_back(r);
                known = true;
                break;
            }
        }
        if (!known || results.size() > kMaxBatchOperations) return false;
    }
    return header;
}

int RunElevatedBatch(const std::wstring& batch_path, const std::wstring& result_path, std::wstring_view token) {
    std::wstring text;
    std::vector<Op> ops;
    if (!exchange::ValidToken(token) || !ReadUtf8File(batch_path, text) || !ParseBatch(text, ops) || ops.empty()) return 2;
    // Per-user keys never need (or get) the elevated helper.
    for (const Op& op : ops) if (op.root != Root::Machine) return 2;
    HANDLE result_file = exchange::OpenForHelper(result_path, token);
    if (result_file == INVALID_HANDLE_VALUE) return 2;
    const Roots roots;
    std::vector<OpResult> results;
    DWORD backup_error = ERROR_SUCCESS;
    if (!WriteRegBackup(roots, ops, batch_path + L".backup.reg", backup_error)) {
        results.assign(ops.size(), OpResult::Failed);
        const bool reported = exchange::Write(result_file, token, exchange::Phase::Complete, SerializeResults(results));
        CloseHandle(result_file);
        return reported ? 1 : 2;
    }
    const int exit_code = exchange::Run(result_file, token, [&] {
        bool changed = false, all_ok = true;
        for (const Op& op : ops) {
            const OpResult r = Apply(roots, op);
            results.push_back(r);
            changed = changed || r == OpResult::Ok || r == OpResult::RecoveryRequired;
            all_ok = all_ok && r == OpResult::Ok;
        }
        if (changed) SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
        return exchange::Report{SerializeResults(results), all_ok};
    }, exchange::Write);
    CloseHandle(result_file);
    return exit_code;
}

} // namespace pulse::app::sysctx
