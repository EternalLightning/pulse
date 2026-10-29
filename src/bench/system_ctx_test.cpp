// System context menu registry: enumeration, CMM markers, enable/disable round
// trips and batch validation, all against a scratch key under HKCU.

#include "../app/system_ctx_registry.h"

#include <windows.h>
#include <winternl.h>

#include <cstdio>
#include <exception>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

using namespace pulse::app::sysctx;

namespace {

int g_failures = 0;
int g_fixture_errors = 0;

void Check(bool ok, const char* name) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++g_failures;
}

std::wstring g_base;

void FixtureError(const char* operation, DWORD error, const std::wstring& path) {
    std::printf("[FAIL] fixture: %s error=%lu path=%ls\n", operation, error, path.c_str());
    ++g_fixture_errors;
    ++g_failures;
}

void PrintStartupEnvironment() {
    for (const wchar_t* name : {L"TEMP", L"TMP"}) {
        std::wstring value(32768, L'\0');
        const DWORD length = GetEnvironmentVariableW(name, value.data(), static_cast<DWORD>(value.size()));
        std::printf("[INIT] %ls=%ls (length=%lu)\n", name,
                    length && length < value.size() ? value.c_str() : L"<unset or too long>", length);
    }
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        std::printf("[INIT] OpenProcessToken error=%lu\n", GetLastError());
        return;
    }
    const auto read_token = [&](TOKEN_INFORMATION_CLASS kind, void* data, DWORD bytes, const char* name) {
        DWORD size = 0;
        if (!GetTokenInformation(token, kind, data, bytes, &size))
            std::printf("[INIT] GetTokenInformation(%s) error=%lu\n", name, GetLastError());
    };
    TOKEN_ELEVATION elevation{MAXDWORD};
    DWORD container = MAXDWORD, virtualized = MAXDWORD, integrity = MAXDWORD;
    read_token(TokenElevation, &elevation, sizeof(elevation), "elevation");
    read_token(TokenIsAppContainer, &container, sizeof(container), "app container");
    read_token(TokenVirtualizationEnabled, &virtualized, sizeof(virtualized), "virtualization");
    DWORD size = 0;
    GetTokenInformation(token, TokenIntegrityLevel, nullptr, 0, &size);
    std::vector<BYTE> label(size);
    if (size && GetTokenInformation(token, TokenIntegrityLevel, label.data(), size, &size)) {
        const PSID sid = reinterpret_cast<const TOKEN_MANDATORY_LABEL*>(label.data())->Label.Sid;
        if (IsValidSid(sid) && *GetSidSubAuthorityCount(sid))
            integrity = *GetSidSubAuthority(sid, static_cast<DWORD>(*GetSidSubAuthorityCount(sid) - 1));
    }
    std::printf("[INIT] token: integrity=%lu elevated=%lu restricted=%d app_container=%lu virtualized=%lu\n",
                integrity, elevation.TokenIsElevated, IsTokenRestricted(token), container, virtualized);
    CloseHandle(token);
}

struct ScratchFixture {
    std::wstring directory;
    bool owns_registry = false;
    bool owns_directory = false;
    bool cleaned = false;
    bool Cleanup() {
        if (cleaned) return true;
        cleaned = true;
        bool ok = true;
        if (owns_registry) {
            const LSTATUS deleted = RegDeleteTreeW(HKEY_CURRENT_USER, g_base.c_str());
            ok = deleted == ERROR_SUCCESS || deleted == ERROR_FILE_NOT_FOUND;
            if (!ok) std::printf("[FAIL] fixture cleanup: RegDeleteTreeW error=%ld path=%ls\n", deleted, g_base.c_str());
        }
        if (owns_directory) {
            std::error_code error;
            std::filesystem::remove_all(directory, error);
            if (error) std::printf("[FAIL] fixture cleanup: remove_all error=%d path=%ls\n", error.value(), directory.c_str());
            ok = !error && ok;
        }
        return ok;
    }
    ~ScratchFixture() {
        Cleanup();
        SetVelocitySupportedForTesting(-1);
    }
};

bool CreateFixtureFile(const std::wstring& path, DWORD attributes) {
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, attributes, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        FixtureError("CreateFileW", GetLastError(), path);
        return false;
    }
    CloseHandle(file);
    return true;
}

void SetString(const std::wstring& rel, const wchar_t* name, const std::wstring& value) {
    HKEY k = nullptr;
    const std::wstring path = g_base + L"\\" + rel;
    const LSTATUS created = RegCreateKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, nullptr, 0, KEY_SET_VALUE,
                                           nullptr, &k, nullptr);
    if (created != ERROR_SUCCESS) { FixtureError("RegCreateKeyExW", created, path); return; }
    const LSTATUS written = RegSetValueExW(k, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                                           static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(k);
    if (written != ERROR_SUCCESS) FixtureError("RegSetValueExW", written, path);
}

void SetDword(const std::wstring& rel, const wchar_t* name, DWORD value) {
    HKEY k = nullptr;
    const std::wstring path = g_base + L"\\" + rel;
    const LSTATUS created = RegCreateKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, nullptr, 0, KEY_SET_VALUE,
                                           nullptr, &k, nullptr);
    if (created != ERROR_SUCCESS) { FixtureError("RegCreateKeyExW", created, path); return; }
    const LSTATUS written = RegSetValueExW(k, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value));
    RegCloseKey(k);
    if (written != ERROR_SUCCESS) FixtureError("RegSetValueExW", written, path);
}

bool SetRaw(const std::wstring& rel, const wchar_t* name, DWORD type, const std::vector<BYTE>& data) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, (g_base + L"\\" + rel).c_str(), 0, nullptr, 0, KEY_SET_VALUE,
                        nullptr, &key, nullptr) != ERROR_SUCCESS) return false;
    using SetValue = NTSTATUS (NTAPI*)(HANDLE, PUNICODE_STRING, ULONG, ULONG, PVOID, ULONG);
    const auto set_value = reinterpret_cast<SetValue>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtSetValueKey"));
    const auto length = static_cast<USHORT>(wcslen(name) * sizeof(wchar_t));
    UNICODE_STRING unicode{length, static_cast<USHORT>(length + sizeof(wchar_t)), const_cast<wchar_t*>(name)};
    const bool ok = set_value && set_value(key, &unicode, 0, type,
        data.empty() ? nullptr : const_cast<BYTE*>(data.data()), static_cast<ULONG>(data.size())) >= 0;
    RegCloseKey(key);
    return ok;
}

bool SetWin32Raw(const std::wstring& rel, const wchar_t* name, DWORD type, const std::vector<BYTE>& data) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, (g_base + L"\\" + rel).c_str(), 0, nullptr, 0, KEY_SET_VALUE,
                        nullptr, &key, nullptr) != ERROR_SUCCESS) return false;
    auto buffer = data;
    buffer.resize(buffer.size() + sizeof(wchar_t), 0);
    const bool ok = RegSetValueExW(key, name, 0, type, buffer.data(), static_cast<DWORD>(data.size())) == ERROR_SUCCESS;
    RegCloseKey(key);
    return ok;
}

bool RawEquals(const std::wstring& rel, const wchar_t* name, DWORD expected_type, const std::vector<BYTE>& expected) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, (g_base + L"\\" + rel).c_str(), 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
        return false;
    DWORD type = 0, size = 0;
    bool ok = RegQueryValueExW(key, name, nullptr, &type, nullptr, &size) == ERROR_SUCCESS &&
              type == expected_type && size == expected.size();
    std::vector<BYTE> data(size);
    if (ok) ok = RegQueryValueExW(key, name, nullptr, &type, data.empty() ? nullptr : data.data(), &size) == ERROR_SUCCESS &&
                  type == expected_type && size == expected.size() && data == expected;
    RegCloseKey(key);
    return ok;
}

bool HasValue(const std::wstring& rel, const wchar_t* name) {
    HKEY k = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, (g_base + L"\\" + rel).c_str(), 0, KEY_READ, &k) != ERROR_SUCCESS)
        return false;
    const bool has = RegQueryValueExW(k, name, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
    RegCloseKey(k);
    return has;
}

DWORD GetDword(const std::wstring& rel, const wchar_t* name) {
    HKEY k = nullptr;
    DWORD v = 0, size = sizeof(v);
    if (RegOpenKeyExW(HKEY_CURRENT_USER, (g_base + L"\\" + rel).c_str(), 0, KEY_READ, &k) != ERROR_SUCCESS) return 0;
    RegQueryValueExW(k, name, nullptr, nullptr, reinterpret_cast<BYTE*>(&v), &size);
    RegCloseKey(k);
    return v;
}

std::wstring GetString(const std::wstring& rel, const wchar_t* name) {
    HKEY k = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, (g_base + L"\\" + rel).c_str(), 0, KEY_READ, &k) != ERROR_SUCCESS) return {};
    wchar_t buf[256] = {};
    DWORD type = 0, size = sizeof(buf) - sizeof(wchar_t);
    const LSTATUS st = RegQueryValueExW(k, name, nullptr, &type, reinterpret_cast<BYTE*>(buf), &size);
    RegCloseKey(k);
    return st == ERROR_SUCCESS && type == REG_SZ ? std::wstring(buf) : std::wstring();
}

bool KeyExists(const std::wstring& rel) {
    HKEY k = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, (g_base + L"\\" + rel).c_str(), 0, KEY_READ, &k) != ERROR_SUCCESS)
        return false;
    RegCloseKey(k);
    return true;
}

const Item* Find(const std::vector<Item>& items, const std::wstring& key_name) {
    for (const auto& it : items) if (it.key_name == key_name) return &it;
    return nullptr;
}

std::wstring Snapshot(const Roots& roots) {
    // Export the whole scratch tree through one fake op per layer root.
    std::wstring out;
    for (const wchar_t* layer : {L"User", L"Machine"}) {
        Roots r = roots;
        r.user_classes = g_base + L"\\" + layer;
        Op all;
        all.root = Root::User;
        all.kind = Kind::Verb;
        all.key_path = L"";
        // ExportReg skips nothing for an empty path: it exports the layer root.
        out += ExportReg(r, {all});
    }
    return out;
}

// Read-only: lists what Settings would show for the real registry.
int Scan(const wchar_t* out_path) {
    const Roots roots;
    std::vector<Scene> scenes = BuiltinScenes();
    for (const wchar_t* ext : {L".png", L".jpg", L".mp4", L".dwg", L".zip", L".pdf", L".txt", L".docx"})
        scenes.push_back(ExtensionScene(roots, ext));
    for (const wchar_t* type : {L"image", L"audio", L"text", L"video"}) scenes.push_back(PerceivedScene(type));
    std::wstring out;
    size_t total = 0, disabled = 0, missing = 0;
    const ULONGLONG start = GetTickCount64();
    for (const Scene& scene : scenes) {
        for (const Item& it : Enumerate(roots, scene)) {
            ++total;
            disabled += it.enabled ? 0 : 1;
            missing += it.missing ? 1 : 0;
            static const wchar_t* kinds[] = {L"verb", L"shellex", L"dragdrop", L"shellnew", L"sendto", L"openwith"};
            out += scene.id + L"\t" + kinds[static_cast<int>(it.kind)] + L"\t" +
                   (it.root == Root::User ? L"HKCU" : it.root == Root::Machine ? L"HKLM" : L"file") + L"\t" +
                   it.key_path + L"\t" + (it.enabled ? L"on" : L"off") + L"\t" + (it.missing ? L"missing" : L"") +
                   L"\t" + (it.is_protected ? L"prot" : L"") + L"\t" + (it.trusted_installer ? L"ti" : L"") +
                   L"\t" + it.display + L"\t" + it.file + L"\n";
        }
    }
    const ULONGLONG ms = GetTickCount64() - start;
    HANDLE f = CreateFileW(out_path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return 1;
    const int n = WideCharToMultiByte(CP_UTF8, 0, out.data(), static_cast<int>(out.size()), nullptr, 0, nullptr,
                                      nullptr);
    std::string bytes(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, out.data(), static_cast<int>(out.size()), bytes.data(), n, nullptr, nullptr);
    DWORD written = 0;
    WriteFile(f, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr);
    CloseHandle(f);
    std::printf("scan: %zu items, %zu disabled, %zu missing, %llu ms\n", total, disabled, missing, ms);
    return 0;
}

} // namespace

int Run(int argc, wchar_t** argv) {
    if (argc == 3 && wcscmp(argv[1], L"--scan") == 0) return Scan(argv[2]);
    wchar_t tmp[MAX_PATH]{};
    const DWORD temporary_length = GetTempPathW(ARRAYSIZE(tmp), tmp);
    if (!temporary_length || temporary_length >= ARRAYSIZE(tmp)) {
        std::printf("[FAIL] fixture: GetTempPathW length=%lu capacity=%lu error=%lu\n",
                    temporary_length, static_cast<DWORD>(ARRAYSIZE(tmp)), temporary_length ? ERROR_INSUFFICIENT_BUFFER : GetLastError());
        return 2;
    }
    const std::wstring token = std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(GetTickCount64());
    g_base = L"Software\\PulseTest\\SysCtx_" + token;
    ScratchFixture fixture;
    fixture.directory = std::wstring(tmp) + L"PulseSystemCtx_" + token;
    std::printf("[INIT] temporary=%ls fixture=%ls registry=HKCU\\%ls\n", tmp, fixture.directory.c_str(), g_base.c_str());
    PrintStartupEnvironment();
    HKEY probe = nullptr;
    DWORD disposition = 0;
    const LSTATUS created = RegCreateKeyExW(HKEY_CURRENT_USER, g_base.c_str(), 0, nullptr, 0,
                                           KEY_READ | KEY_WRITE | DELETE, nullptr, &probe, &disposition);
    if (created != ERROR_SUCCESS) {
        FixtureError("RegCreateKeyExW scratch root", created, g_base);
        return 2;
    }
    RegCloseKey(probe);
    if (disposition != REG_CREATED_NEW_KEY) {
        std::printf("[FAIL] fixture: scratch registry root already exists; refusing to reuse or remove it\n");
        return 2;
    }
    fixture.owns_registry = true;
    if (!CreateDirectoryW(fixture.directory.c_str(), nullptr)) {
        FixtureError("CreateDirectoryW", GetLastError(), fixture.directory);
        return 2;
    }
    fixture.owns_directory = true;
    Roots roots;
    roots.user = HKEY_CURRENT_USER;
    roots.user_classes = g_base + L"\\User";
    roots.machine = HKEY_CURRENT_USER;
    roots.machine_classes = g_base + L"\\Machine";
    roots.file_exts = g_base + L"\\FileExts";
    roots.user_trash = g_base + L"\\TrashUser";
    roots.machine_trash = g_base + L"\\TrashMachine";
    roots.check_owner = false;
    roots.resolve_strings = false;
    roots.sendto_dir = fixture.directory + L"\\SendTo";
    if (!CreateDirectoryW(roots.sendto_dir.c_str(), nullptr)) {
        FixtureError("CreateDirectoryW", GetLastError(), roots.sendto_dir);
        return 2;
    }
    SetVelocitySupportedForTesting(1);

    // ---- fixtures --------------------------------------------------------
    SetString(L"User\\*\\shell\\FooVerb", L"MUIVerb", L"Foo");
    SetString(L"User\\*\\shell\\FooVerb\\command", nullptr, L"\"C:\\pulse_missing_dir\\gone.exe\" \"%1\"");
    SetString(L"User\\*\\shell\\Dup", L"MUIVerb", L"Dup (user)");
    SetString(L"Machine\\*\\shell\\Dup", L"MUIVerb", L"Dup (machine)");
    SetDword(L"Machine\\Directory\\shell\\cmd", L"HideBasedOnVelocityId", 0x639bc8);
    SetString(L"Machine\\Directory\\shell\\cmd\\command", nullptr, L"cmd.exe /s /k pushd \"%V\"");
    SetDword(L"Machine\\Directory\\shell\\BarVerb", L"CommandFlags", 0x28);
    SetString(L"Machine\\Directory\\shell\\BarVerb", L"Extended", L"");
    SetString(L"Machine\\*\\shellex\\ContextMenuHandlers\\Baz", nullptr, L"{11111111-2222-3333-4444-555555555555}");
    SetString(L"Machine\\*\\shellex\\ContextMenuHandlers\\Baz\\Nested", L"Keep", L"me");
    SetString(L"Machine\\CLSID\\{11111111-2222-3333-4444-555555555555}\\InprocServer32", nullptr,
              L"C:\\Windows\\System32\\shell32.dll");
    SetString(L"Machine\\*\\shellex\\-ContextMenuHandlers\\Qux", nullptr, L"{66666666-2222-3333-4444-555555555555}");
    SetString(L"Machine\\Folder\\shell\\open\\command", nullptr, L"explorer.exe");
    SetString(L"Machine\\Folder\\shell\\opennewwindow", L"MUIVerb", L"new window");
    SetString(L"Machine\\Directory\\shellex\\DragDropHandlers\\Zip", nullptr,
              L"{11111111-2222-3333-4444-555555555555}");
    SetString(L"Machine\\Applications\\tool.exe\\shell\\open\\command", nullptr, L"\"C:\\pulse_missing_dir\\tool.exe\" \"%1\"");
    SetString(L"Machine\\Applications\\tool.exe", L"NoOpenWith", L"");
    SetString(L"User\\.pulsetest", nullptr, L"PulseTest.File");
    SetString(L"User\\.pulsetest\\ShellNew", L"NullFile", L"");
    SetString(L"User\\PulseTest.File", nullptr, L"Pulse test file");
    // .png: HKCU key with an empty default still inherits HKLM's ProgID; the
    // user's choice comes first; printto is never listed.
    SetString(L"User\\.pulsepng", L"Content Type", L"image/png");
    SetString(L"Machine\\.pulsepng", nullptr, L"PulsePng.File");
    SetString(L"Machine\\PulsePng.File\\shell\\printto\\command", nullptr, L"print.exe");
    SetString(L"Machine\\PulsePng.File\\shell\\Rotate\\command", nullptr, L"rotate.exe");
    SetString(L"FileExts\\.pulsepng\\UserChoice", L"ProgId", L"PulsePng.Choice");
    SetString(L"User\\PulsePng.Choice\\shell\\Edit\\command", nullptr, L"edit.exe");
    // Inert ShellNew (no creation value), ProgID-scoped ShellNew wins, Folder.
    SetString(L"Machine\\.pulseinert", nullptr, L"Inert.File");
    SetString(L"Machine\\.pulseinert\\Inert.File\\ShellNew", L"ItemName", L"x");
    SetString(L"Machine\\.pulsescoped", nullptr, L"Scoped.File");
    SetString(L"Machine\\.pulsescoped\\ShellNew", L"NullFile", L"");
    SetString(L"Machine\\.pulsescoped\\Scoped.File\\ShellNew", L"FileName", L"template.bin");
    SetString(L"Machine\\Folder\\ShellNew", L"Directory", L"");
    // .docx-style: no default ProgID, but the user's choice has a ShellNew.
    SetString(L"Machine\\.pulsedoc\\Pulse.Doc.12\\ShellNew", L"FileName", L"template.docx");
    SetString(L"Machine\\Pulse.Doc.12", nullptr, L"Pulse document");
    SetString(L"FileExts\\.pulsedoc\\UserChoice", L"ProgId", L"Pulse.Doc.12");
    SetString(L"Machine\\Applications\\Some.ProgId.cpp\\shell\\open\\command", nullptr, L"x.exe");
    // Pulse's 新建 menu: Data bytes, an absolute template, a COM-only creator,
    // a disabled entry.
    SetString(L"Machine\\.pulsedata\\ShellNew", L"Data", L"AB");
    SetString(L"Machine\\.pulsedata\\ShellNew", L"ItemName", L"A data file");
    const std::wstring template_path = fixture.directory + L"\\template.bin";
    CreateFixtureFile(template_path, FILE_ATTRIBUTE_NORMAL);
    SetString(L"Machine\\.pulsetpl\\ShellNew", L"FileName", template_path);
    SetString(L"Machine\\.pulsetpl\\ShellNew", L"ItemName", L"B template file");
    SetString(L"Machine\\.pulselib\\ShellNew", L"Handler", L"{11111111-2222-3333-4444-555555555555}");
    SetString(L"Machine\\.pulselib\\ShellNew", L"NullFile", L"");
    SetString(L"Machine\\.pulseoff\\-ShellNew", L"NullFile", L"");
    const std::wstring hidden_link = roots.sendto_dir + L"\\Hidden target.lnk";
    CreateFixtureFile(hidden_link, FILE_ATTRIBUTE_HIDDEN);
    CreateFixtureFile(roots.sendto_dir + L"\\Visible.lnk", FILE_ATTRIBUTE_NORMAL);
    if (g_fixture_errors) {
        std::printf("[FAIL] fixture initialization aborted after %d setup error(s)\n", g_fixture_errors);
        return 2;
    }
    std::printf("[INIT] isolated registry and filesystem fixtures are ready\n");

    const Scene file{L"file", {L"*"}};
    const Scene folder{L"folder", {L"Directory", L"Folder"}};

    // ---- S1: enumeration ---------------------------------------------------
    auto files = Enumerate(roots, file);
    const Item* foo = Find(files, L"FooVerb");
    Check(foo && foo->enabled && foo->display == L"Foo" && foo->root == Root::User, "verb: user layer, MUIVerb");
    Check(foo && foo->missing, "verb: command pointing at a deleted program is reported as missing");
    const Item* dup = Find(files, L"Dup");
    int dup_count = 0;
    for (const auto& it : files) if (it.key_name == L"Dup") ++dup_count;
    Check(dup && dup_count == 1 && dup->root == Root::User, "merged view: user layer wins, listed once");
    const Item* baz = Find(files, L"Baz");
    Check(baz && baz->kind == Kind::ShellEx && baz->enabled && baz->root == Root::Machine &&
              baz->clsid == L"{11111111-2222-3333-4444-555555555555}" && !baz->file.empty() && !baz->missing,
          "shellex: CLSID and InprocServer32 resolved");
    const Item* qux = Find(files, L"Qux");
    Check(qux && !qux->enabled && qux->dash_marker && qux->missing, "shellex: -ContextMenuHandlers (CMM) = disabled");

    auto folders = Enumerate(roots, folder);
    const Item* cmd = Find(folders, L"cmd");
    Check(cmd && !cmd->enabled && cmd->velocity_hidden && !cmd->missing, "verb: HideBasedOnVelocityId = disabled");
    const Item* bar = Find(folders, L"BarVerb");
    Check(bar && !bar->enabled && bar->command_flags_hidden && bar->extended, "verb: CommandFlags & 8 = disabled; Extended");
    const Item* open = Find(folders, L"open");
    Check(open && open->is_protected, "protected: Folder open");
    Check(Find(folders, L"Zip") == nullptr, "folder scene does not list drag-drop handlers");

    auto drags = Enumerate(roots, Scene{L"dragdrop", {L"Directory"}, SceneMode::DragDrop});
    Check(Find(drags, L"Zip") && Find(drags, L"Zip")->kind == Kind::DragDrop, "drag-drop scene lists DragDropHandlers");
    auto apps = Enumerate(roots, Scene{L"openwith", {}, SceneMode::OpenWith});
    const Item* tool = Find(apps, L"tool.exe");
    Check(tool && !tool->enabled && tool->no_open_with && tool->missing, "open with: NoOpenWith = disabled");
    auto news = Enumerate(roots, Scene{L"shellnew", {}, SceneMode::ShellNew});
    const Item* sn = Find(news, L".pulsetest");
    Check(sn && sn->enabled && sn->display == L"Pulse test file", "shell new: listed with the ProgID name");
    Check(Find(news, L".pulseinert") == nullptr, "shell new: keys without a creation value are ignored");
    const Item* scoped = Find(news, L".pulsescoped");
    Check(scoped && scoped->key_path == L".pulsescoped\\Scoped.File\\ShellNew", "shell new: .ext\\<ProgID>\\ShellNew wins");
    const Item* doc = Find(news, L".pulsedoc");
    Check(doc && doc->display == L"Pulse document", "shell new: ProgID from the user's choice when there is no default");
    const Item* nf = Find(news, L"Folder");
    Check(nf && nf->is_protected, "shell new: Folder is listed and protected");
    {
        const auto templates = ListNewTemplates(roots);
        const auto find = [&](const wchar_t* ext) -> const NewTemplate* {
            for (const auto& t : templates) if (t.ext == ext) return &t;
            return nullptr;
        };
        const NewTemplate* data = find(L".pulsedata");
        const NewTemplate* tpl = find(L".pulsetpl");
        const NewTemplate* test = find(L".pulsetest");
        Check(data && data->display == L"A data file" && data->data == std::vector<BYTE>{'A', 'B'},
              "new menu: string Data is written as ANSI bytes");
        Check(tpl && tpl->template_file == template_path, "new menu: absolute FileName resolves to the template");
        Check(test && test->null_file && test->display == L"Pulse test file", "new menu: NullFile entry with the ProgID name");
        Check(!find(L".pulselib") && !find(L".pulseoff") && !find(L".pulseinert") && !find(L"Folder"),
              "new menu: COM-only, disabled, inert entries and Folder are left out");
        Check(!find(L".pulsescoped") && !find(L".pulsedoc"), "new menu: templates that resolve to nothing are left out");
        Check(data && tpl && test && data < tpl && tpl < test, "new menu: sorted by menu text");
        NewTemplate lnk;
        lnk.ext = L".lnk";
        lnk.display = L"Shortcut";
        lnk.handler = true;
        Check(CanCreateNewTemplate(lnk), "new menu: .lnk keeps its handler entry (shortcut wizard)");
        DeleteFileW(template_path.c_str());
    }
    Check(Find(apps, L"Some.ProgId.cpp") == nullptr, "open with: only Applications\\*.exe");
    const Scene png = ExtensionScene(roots, L".pulsepng");
    Check(png.class_keys.size() == 3 && png.class_keys[1] == L"PulsePng.Choice" && png.class_keys[2] == L"PulsePng.File",
          "extension scene: user choice, then the inherited ProgID");
    auto pngs = Enumerate(roots, png);
    Check(Find(pngs, L"Edit") && Find(pngs, L"Rotate") && !Find(pngs, L"printto"), "extension scene: printto hidden");
    auto sendto = Enumerate(roots, Scene{L"sendto", {}, SceneMode::SendTo});
    const Item* hid = Find(sendto, L"Hidden target.lnk");
    const Item* vis = Find(sendto, L"Visible.lnk");
    Check(hid && !hid->enabled && vis && vis->enabled, "send to: Hidden attribute = disabled");

    // ---- S2: disable -> enable restores byte for byte -----------------------
    const std::wstring before = Snapshot(roots);
    std::vector<Op> done;
    auto run = [&](const Item* it, Action a) {
        if (!it) return OpResult::NotFound;
        Op op = MakeOp(*it, a);
        PlanOp(roots, op);
        const OpResult r = Apply(roots, op);
        if (r == OpResult::Ok) done.push_back(op);
        return r;
    };
    Check(run(foo, Action::Disable) == OpResult::Ok &&
              HasValue(L"User\\*\\shell\\FooVerb", L"LegacyDisable") &&
              HasValue(L"User\\*\\shell\\FooVerb", L"ProgrammaticAccessOnly") &&
              GetDword(L"User\\*\\shell\\FooVerb", L"HideBasedOnVelocityId") == 0x639bc8,
          "disable verb writes the three CMM markers");
    Check(run(baz, Action::Disable) == OpResult::Ok &&
              !KeyExists(L"Machine\\*\\shellex\\ContextMenuHandlers\\Baz") &&
              KeyExists(L"Machine\\*\\shellex\\-ContextMenuHandlers\\Baz\\Nested"),
          "disable shellex moves the key tree to -ContextMenuHandlers");
    auto after = Enumerate(roots, file);
    Check(Find(after, L"FooVerb") && !Find(after, L"FooVerb")->enabled && Find(after, L"Baz") &&
              !Find(after, L"Baz")->enabled, "enumeration sees the disabled state");
    Check(run(bar, Action::Enable) == OpResult::Ok && GetDword(L"Machine\\Directory\\shell\\BarVerb", L"CommandFlags") == 0x20,
          "enable verb clears CommandFlags bit 8 only");
    Check(run(qux, Action::Enable) == OpResult::Ok && KeyExists(L"Machine\\*\\shellex\\ContextMenuHandlers\\Qux"),
          "enable CMM-disabled shellex moves it back");
    // Undo everything in reverse order.
    bool undo_ok = true;
    for (auto it = done.rbegin(); it != done.rend(); ++it) undo_ok = Apply(roots, Inverse(*it)) == OpResult::Ok && undo_ok;
    Check(undo_ok && Snapshot(roots) == before, "undo restores the registry byte for byte");

    {
        const std::wstring rel = L"Machine\\*\\shell\\RawApiComparison";
        const std::vector<std::vector<BYTE>> requested = {{'x', 0, 'y', 0}, {'x', 0, 0, 0, 'z', 0}, {}};
        bool compared = true;
        for (const auto& bytes : requested) {
            const bool win32_written = SetWin32Raw(rel, L"Win32", REG_SZ, bytes);
            const bool native_written = SetRaw(rel, L"Native", REG_SZ, bytes);
            const bool native_exact = RawEquals(rel, L"Native", REG_SZ, bytes);
            HKEY key = nullptr;
            bool read = RegOpenKeyExW(HKEY_CURRENT_USER, (g_base + L"\\" + rel).c_str(), 0, KEY_QUERY_VALUE, &key) == ERROR_SUCCESS;
            std::printf("[DIAG] raw string API comparison: requested=%zu", bytes.size());
            if (read) {
                for (const wchar_t* name : {L"Win32", L"Native"}) {
                    DWORD type = 0, size = 0;
                    bool value_read = RegQueryValueExW(key, name, nullptr, &type, nullptr, &size) == ERROR_SUCCESS;
                    std::vector<BYTE> actual(size);
                    value_read = value_read && RegQueryValueExW(key, name, nullptr, &type,
                        actual.empty() ? nullptr : actual.data(), &size) == ERROR_SUCCESS;
                    std::printf(" %ls(type=%lu,bytes=%lu,exact=%d,data=", name, type, size, value_read && actual == bytes);
                    for (const BYTE b : actual) std::printf("%02x", static_cast<unsigned>(b));
                    std::printf(")");
                    read = value_read && read;
                }
                RegCloseKey(key);
            }
            std::printf("\n");
            compared = win32_written && native_written && native_exact && read && compared;
        }
        Check(compared, "compare Win32 and native stored string bytes directly; native writes preserve each requested byte");
        RegDeleteTreeW(HKEY_CURRENT_USER, (g_base + L"\\" + rel).c_str());
    }

    {
        Roots fault_roots = roots;
        fault_roots.value_write_fault_for_testing = std::make_shared<ValueWriteFaultForTesting>();
        auto& fault = *fault_roots.value_write_fault_for_testing;
        const std::wstring path = L"*\\shell\\TransactionalValues";
        const std::wstring rel = L"Machine\\" + path;
        SetString(rel, L"MUIVerb", L"keep unrelated value");
        Op disable{Action::Disable, Kind::Verb, Root::Machine, path};
        PlanOp(fault_roots, disable);
        fault.fail_write_call = 2;
        const OpResult disabled = Apply(fault_roots, disable);
        Check(disabled == OpResult::Failed && fault.write_calls == 2 && fault.rollback_calls == 2 &&
                  !HasValue(rel, L"HideBasedOnVelocityId") && !HasValue(rel, L"LegacyDisable") &&
                  !HasValue(rel, L"ProgrammaticAccessOnly") && GetString(rel, L"MUIVerb") == L"keep unrelated value",
              "second disable write failure rolls back introduced values to their original missing state");

        const std::vector<BYTE> velocity = {0x78, 0x56, 0x34, 0x12};
        const std::vector<BYTE> legacy = {0x10, 0x00, 0xFF};
        const std::vector<BYTE> programmatic = {'x', 0, 0, 0, 'z', 0};
        const std::vector<BYTE> flags = {0x28, 0x01, 0x00, 0x00};
        const bool seeded = SetRaw(rel, L"HideBasedOnVelocityId", REG_DWORD, velocity) &&
                            SetRaw(rel, L"LegacyDisable", REG_BINARY, legacy) &&
                            SetRaw(rel, L"ProgrammaticAccessOnly", REG_SZ, programmatic) &&
                            SetRaw(rel, L"CommandFlags", REG_DWORD, flags);
        const auto original = [&] {
            return RawEquals(rel, L"HideBasedOnVelocityId", REG_DWORD, velocity) &&
                   RawEquals(rel, L"LegacyDisable", REG_BINARY, legacy) &&
                   RawEquals(rel, L"ProgrammaticAccessOnly", REG_SZ, programmatic) &&
                   RawEquals(rel, L"CommandFlags", REG_DWORD, flags);
        };
        const auto enabled = [&] {
            return !HasValue(rel, L"HideBasedOnVelocityId") && !HasValue(rel, L"LegacyDisable") &&
                   !HasValue(rel, L"ProgrammaticAccessOnly") && GetDword(rel, L"CommandFlags") == 0x120;
        };
        Op enable{Action::Enable, Kind::Verb, Root::Machine, path};
        PlanOp(fault_roots, enable);
        fault = {};
        fault.fail_write_call = 2;
        Check(seeded && Apply(fault_roots, enable) == OpResult::Failed && original() &&
                  fault.write_calls == 2 && fault.rollback_calls == 2,
              "second enable deletion failure restores original marker types and raw bytes");
        fault = {};
        const bool applied = Apply(fault_roots, enable) == OpResult::Ok && enabled();
        fault = {};
        fault.fail_write_call = 2;
        const Op inverse = Inverse(enable);
        const bool inverse_failed = Apply(fault_roots, inverse) == OpResult::Failed && enabled() &&
                                    fault.write_calls == 2 && fault.rollback_calls == 2;
        fault = {};
        Check(applied && inverse_failed && Apply(fault_roots, inverse) == OpResult::Ok && original(),
              "failed inverse restores its starting state and can be retried without losing the original snapshot");

        fault = {};
        fault.fail_write_call = 2;
        fault.fail_rollback_call = 2;
        const OpResult recovery = Apply(fault_roots, enable);
        const bool partial = !HasValue(rel, L"HideBasedOnVelocityId") && RawEquals(rel, L"LegacyDisable", REG_BINARY, legacy) &&
                             RawEquals(rel, L"ProgrammaticAccessOnly", REG_SZ, programmatic);
        fault = {};
        Check(recovery == OpResult::RecoveryRequired && partial && Apply(fault_roots, inverse) == OpResult::Ok && original(),
              "failed rollback reports RecoveryRequired and the recorded inverse can recover the remaining partial change");

        const std::vector<BYTE> position = {0x01, 0x00, 0xFE};
        const bool position_seeded = SetRaw(rel, L"Position", REG_BINARY, position);
        Op place{Action::Position, Kind::Verb, Root::Machine, path};
        place.place = Place::Top;
        fault = {};
        fault.fail_write_call = 1;
        Check(position_seeded && Apply(fault_roots, place) == OpResult::Failed && fault.rollback_calls == 1 &&
                  RawEquals(rel, L"Position", REG_BINARY, position),
              "single Position write failure also restores the exact previous value");

        const std::vector<BYTE> oversized(kMaxValueSnapshotBytes + 1, 0xAC);
        const bool oversized_seeded = SetRaw(rel, L"LegacyDisable", REG_BINARY, oversized);
        Op forced_disable{Action::Disable, Kind::Verb, Root::Machine, path, true, 7};
        fault = {};
        Check(oversized_seeded && Apply(fault_roots, forced_disable) == OpResult::Failed && fault.write_calls == 0 &&
                  RawEquals(rel, L"HideBasedOnVelocityId", REG_DWORD, velocity) &&
                  RawEquals(rel, L"LegacyDisable", REG_BINARY, oversized),
              "all rollback snapshots are captured before the first write; a later oversized value prevents every mutation");
        HKEY isolated_handle = nullptr;
        const bool opened = RegOpenKeyExW(HKEY_CURRENT_USER, (g_base + L"\\Machine").c_str(), 0,
                                          KEY_READ | KEY_WRITE, &isolated_handle) == ERROR_SUCCESS;
        bool seam_ignored = false;
        if (opened) {
            Roots handle_roots = fault_roots;
            handle_roots.machine = isolated_handle;
            handle_roots.machine_classes.clear();
            const std::wstring no_fault_path = L"*\\shell\\NoTestFault";
            SetString(L"Machine\\" + no_fault_path, L"MUIVerb", L"scratch handle");
            Op no_fault{Action::Disable, Kind::Verb, Root::Machine, no_fault_path};
            fault = {};
            fault.fail_write_call = 1;
            seam_ignored = Apply(handle_roots, no_fault) == OpResult::Ok && fault.write_calls == 0 &&
                           HasValue(L"Machine\\" + no_fault_path, L"LegacyDisable");
            RegCloseKey(isolated_handle);
            RegDeleteTreeW(HKEY_CURRENT_USER, (g_base + L"\\Machine\\" + no_fault_path).c_str());
        }
        Check(seam_ignored, "fault injection is ignored outside the explicit HKCU scratch-root seam using a separate isolated handle");
        RegDeleteTreeW(HKEY_CURRENT_USER, (g_base + L"\\" + rel).c_str());
    }

    {
        const std::wstring rel = L"Machine\\*\\shell\\RawRestore";
        const std::vector<BYTE> velocity = {0x78, 0x56, 0x34, 0x12};
        const std::vector<BYTE> legacy = {0x10, 0x00, 0xFF};
        const std::vector<BYTE> programmatic = {'x', 0, 0, 0, 'z', 0};
        const std::vector<BYTE> flags = {0x28, 0x01, 0x00, 0x00};
        const bool seeded = SetRaw(rel, L"HideBasedOnVelocityId", REG_DWORD, velocity) &&
                            SetRaw(rel, L"LegacyDisable", REG_BINARY, legacy) &&
                            SetRaw(rel, L"ProgrammaticAccessOnly", REG_SZ, programmatic) &&
                            SetRaw(rel, L"CommandFlags", REG_DWORD, flags);
        const auto unchanged = [&] {
            return RawEquals(rel, L"HideBasedOnVelocityId", REG_DWORD, velocity) &&
                   RawEquals(rel, L"LegacyDisable", REG_BINARY, legacy) &&
                   RawEquals(rel, L"ProgrammaticAccessOnly", REG_SZ, programmatic) &&
                   RawEquals(rel, L"CommandFlags", REG_DWORD, flags);
        };
        Op enable{Action::Enable, Kind::Verb, Root::Machine, L"*\\shell\\RawRestore"};
        PlanOp(roots, enable);
        const auto raw_export = ExportReg(roots, {enable});
        Check(seeded && unchanged() &&
                  raw_export.find(L"\"ProgrammaticAccessOnly\"=hex(1):78,00,00,00,7a,00") != std::wstring::npos,
              "backup preserves embedded and unterminated string bytes using typed hex");
        if (!RawEquals(rel, L"ProgrammaticAccessOnly", REG_SZ, programmatic)) {
            for (const auto& value : enable.previous_values) {
                if (value.name != ValueName::ProgrammaticAccessOnly) continue;
                std::printf("[DIAG] seeded programmatic value: requested=%zu stored=%zu type=%lu bytes=",
                    programmatic.size(), value.data.size(), value.type);
                for (BYTE byte : value.data) std::printf("%02x", static_cast<unsigned>(byte));
                std::printf("\n");
            }
        }
        const OpResult enable_result = Apply(roots, enable);
        const bool enabled = enable_result == OpResult::Ok && !HasValue(rel, L"LegacyDisable") &&
                             !HasValue(rel, L"HideBasedOnVelocityId") && !HasValue(rel, L"ProgrammaticAccessOnly") &&
                             GetDword(rel, L"CommandFlags") == 0x120;
        int inverse_result = -1;
        if (seeded && enable.has_previous_values && enabled)
            inverse_result = static_cast<int>(Apply(roots, Inverse(enable)));
        const bool direct_restored = seeded && enable.has_previous_values && enabled &&
                                     inverse_result == static_cast<int>(OpResult::Ok) && unchanged();
        Check(direct_restored,
              "enable undo restores noncanonical marker types and bytes, including CommandFlags");
        const auto diagnose_raw = [&](const char* stage) {
            std::printf("[DIAG] %s: velocity_exact=%d legacy_exact=%d programmatic_exact=%d flags_exact=%d flags=0x%08lx\n",
                        stage, RawEquals(rel, L"HideBasedOnVelocityId", REG_DWORD, velocity),
                        RawEquals(rel, L"LegacyDisable", REG_BINARY, legacy),
                        RawEquals(rel, L"ProgrammaticAccessOnly", REG_SZ, programmatic),
                        RawEquals(rel, L"CommandFlags", REG_DWORD, flags), GetDword(rel, L"CommandFlags"));
        };
        if (!direct_restored) {
            std::printf("[DIAG] raw enable: seeded=%d planned=%d plan_error=%lu previous=%d apply=%d enabled=%d inverse=%d\n",
                        seeded, enable.planned, enable.plan_error, enable.has_previous_values,
                        static_cast<int>(enable_result), enabled, inverse_result);
            diagnose_raw("direct restore");
        }
        std::vector<Op> wire, undo_wire;
        const bool forward = ParseBatch(SerializeBatch({enable}), wire) && wire.size() == 1 &&
                             Apply(roots, wire[0]) == OpResult::Ok;
        const bool restored = forward && ParseBatch(SerializeBatch({Inverse(wire[0])}), undo_wire) && undo_wire.size() == 1 &&
                              undo_wire[0].restore_previous_values && Apply(roots, undo_wire[0]) == OpResult::Ok;
        const bool wire_restored = restored && unchanged();
        Check(wire_restored, "elevated batch wire preserves exact marker restoration data");
        if (!wire_restored) {
            std::printf("[DIAG] raw wire: forward=%d forward_count=%zu inverse_count=%zu restore_previous=%d apply_restored=%d\n",
                        forward, wire.size(), undo_wire.size(),
                        !undo_wire.empty() && undo_wire[0].restore_previous_values, restored);
            diagnose_raw("wire restore");
        }

        const std::vector<BYTE> oversized(kMaxValueSnapshotBytes + 1, 0xAC);
        const bool large_seeded = SetRaw(rel, L"LegacyDisable", REG_BINARY, oversized);
        Op too_large{Action::Enable, Kind::Verb, Root::Machine, L"*\\shell\\RawRestore"};
        PlanOp(roots, too_large);
        Check(large_seeded && too_large.plan_error == ERROR_FILE_TOO_LARGE && !too_large.planned &&
                  Apply(roots, too_large) == OpResult::Failed && RawEquals(rel, L"LegacyDisable", REG_BINARY, oversized) &&
                  SerializeBatch({too_large}).empty(),
              "oversized snapshot refuses planning and mutation instead of losing recovery data");
        RegDeleteTreeW(HKEY_CURRENT_USER, (g_base + L"\\" + rel).c_str());
    }

    // opennewwindow keeps Win+E: no LegacyDisable.
    const Item* onw = Find(folders, L"opennewwindow");
    Check(onw && Apply(roots, MakeOp(*onw, Action::Disable)) == OpResult::Ok &&
              !HasValue(L"Machine\\Folder\\shell\\opennewwindow", L"LegacyDisable") &&
              HasValue(L"Machine\\Folder\\shell\\opennewwindow", L"ProgrammaticAccessOnly"),
          "Folder opennewwindow is disabled without LegacyDisable");
    if (onw) Apply(roots, MakeOp(*onw, Action::Enable));

    // Position = Top / Bottom for static verbs; undo restores the old placement.
    {
        const std::wstring snap = Snapshot(roots);
        const auto items = Enumerate(roots, file);
        const Item* verb = Find(items, L"FooVerb");
        Op top = verb ? MakePositionOp(*verb, Place::Top) : Op{};
        PlanOp(roots, top);
        const bool wrote = verb && Apply(roots, top) == OpResult::Ok &&
                           GetString(L"User\\*\\shell\\FooVerb", L"Position") == L"Top";
        const auto moved = Enumerate(roots, file);
        const Item* moved_verb = Find(moved, L"FooVerb");
        const bool seen = moved_verb && moved_verb->position == Place::Top && moved_verb->enabled;
        Op bottom = moved_verb ? MakePositionOp(*moved_verb, Place::Bottom) : Op{};
        PlanOp(roots, bottom);
        const bool second = moved_verb && Apply(roots, bottom) == OpResult::Ok &&
                            GetString(L"User\\*\\shell\\FooVerb", L"Position") == L"Bottom";
        const bool undone = Apply(roots, Inverse(bottom)) == OpResult::Ok && Apply(roots, Inverse(top)) == OpResult::Ok;
        Check(wrote && seen && second && undone && Snapshot(roots) == snap,
              "position: Top / Bottom written, enumerated, and undone byte for byte");
        Op shellex_pos{Action::Position, Kind::ShellEx, Root::Machine, L"*\\shellex\\ContextMenuHandlers\\Baz"};
        shellex_pos.place = Place::Top;
        Check(Apply(roots, shellex_pos) == OpResult::Unsupported && Snapshot(roots) == snap,
              "position: refused for shellex handlers");
    }

    {
        const std::wstring rel = L"Machine\\*\\shell\\RawPosition";
        const std::vector<std::pair<DWORD, std::vector<BYTE>>> originals = {
            {REG_EXPAND_SZ, {'t', 0, 'O', 0, 'p', 0, 0, 0}}, {REG_BINARY, {0x01, 0x00, 0xFE, 0x42, 0x33}},
            {REG_SZ, {}}};
        bool all_restored = true;
        for (const auto& [type, data] : originals) {
            const bool seeded = SetRaw(rel, L"Position", type, data);
            Op position{Action::Position, Kind::Verb, Root::Machine, L"*\\shell\\RawPosition"};
            position.place = Place::Bottom;
            PlanOp(roots, position);
            const bool direct = seeded && Apply(roots, position) == OpResult::Ok &&
                                Apply(roots, Inverse(position)) == OpResult::Ok && RawEquals(rel, L"Position", type, data);
            std::vector<Op> wire, undo_wire;
            const bool restored = ParseBatch(SerializeBatch({position}), wire) && wire.size() == 1 &&
                                  Apply(roots, wire[0]) == OpResult::Ok &&
                                  ParseBatch(SerializeBatch({Inverse(wire[0])}), undo_wire) && undo_wire.size() == 1 &&
                                  Apply(roots, undo_wire[0]) == OpResult::Ok && RawEquals(rel, L"Position", type, data);
            all_restored = direct && restored && all_restored;
        }
        Check(all_restored, "position undo preserves original expanded, binary and empty raw values through the batch wire");
        RegDeleteTreeW(HKEY_CURRENT_USER, (g_base + L"\\" + rel).c_str());
    }

    // 删除残留项: the key moves into the trash whole; Restore puts it back.
    {
        const std::wstring snap = Snapshot(roots);
        const auto items = Enumerate(roots, file);
        const Item* leftover = Find(items, L"FooVerb");  // its command points at a missing exe
        const bool missing = leftover && leftover->missing;
        Op del = leftover ? MakeOp(*leftover, Action::Delete) : Op{};
        PlanOp(roots, del);
        const bool deleted = leftover && Apply(roots, del) == OpResult::Ok && !KeyExists(L"User\\*\\shell\\FooVerb") &&
                             KeyExists(L"TrashUser\\*\\shell\\FooVerb\\command");
        const bool gone_from_scan = !Find(Enumerate(roots, file), L"FooVerb");
        const bool restored = Apply(roots, Inverse(del)) == OpResult::Ok &&
                              KeyExists(L"User\\*\\shell\\FooVerb\\command") && !KeyExists(L"TrashUser\\*");
        Check(missing && deleted && gone_from_scan && restored && Snapshot(roots) == snap,
              "delete leftover verb: into the trash, gone from the scan, restored byte for byte");
        const Item* handler = Find(items, L"Baz");
        Op hdel = handler ? MakeOp(*handler, Action::Delete) : Op{};
        const bool handler_deleted = handler && Apply(roots, hdel) == OpResult::Ok &&
                                     !KeyExists(L"Machine\\*\\shellex\\ContextMenuHandlers\\Baz") &&
                                     KeyExists(L"TrashMachine\\*\\shellex\\ContextMenuHandlers\\Baz\\Nested");
        const bool handler_back = Apply(roots, Inverse(hdel)) == OpResult::Ok;
        Check(handler_deleted && handler_back && Snapshot(roots) == snap && !KeyExists(L"TrashMachine\\*"),
              "delete shellex key with subkeys and put it back");
        Op stray{Action::Restore, Kind::Verb, Root::User, L"*\\shell\\NotInTrash"};
        Op stray_new{Action::Delete, Kind::ShellNew, Root::User, L".pulsetest\\ShellNew"};
        Check(Apply(roots, stray) == OpResult::NotFound && Apply(roots, stray_new) == OpResult::Unsupported &&
                  Snapshot(roots) == snap,
              "restore without a trashed copy reports NotFound; other kinds are refused");
    }

    // Conflict: both ContextMenuHandlers\\X and -ContextMenuHandlers\\X exist.
    SetString(L"Machine\\*\\shellex\\ContextMenuHandlers\\Clash", nullptr, L"{11111111-2222-3333-4444-555555555555}");
    SetString(L"Machine\\*\\shellex\\-ContextMenuHandlers\\Clash", nullptr, L"{11111111-2222-3333-4444-555555555555}");
    Op clash{Action::Disable, Kind::ShellEx, Root::Machine, L"*\\shellex\\ContextMenuHandlers\\Clash"};
    Check(Apply(roots, clash) == OpResult::Conflict && KeyExists(L"Machine\\*\\shellex\\ContextMenuHandlers\\Clash"),
          "shellex move refuses to overwrite an existing -ContextMenuHandlers key");
    SetString(L"Machine\\*\\shellex\\ContextMenuHandlers\\Clash", L"SourceOnly", L"live");
    SetString(L"Machine\\*\\shellex\\-ContextMenuHandlers\\Clash", L"DestinationOnly", L"keep");
    Check(Apply(roots, clash) == OpResult::Conflict &&
              GetString(L"Machine\\*\\shellex\\ContextMenuHandlers\\Clash", L"SourceOnly") == L"live" &&
              GetString(L"Machine\\*\\shellex\\-ContextMenuHandlers\\Clash", L"DestinationOnly") == L"keep" &&
              !HasValue(L"Machine\\*\\shellex\\-ContextMenuHandlers\\Clash", L"SourceOnly"),
          "same-target conflict preserves both source and destination without merging values");
    {
        SetString(L"User\\*\\shell\\TrashClash", L"SourceOnly", L"new registration");
        SetString(L"TrashUser\\*\\shell\\TrashClash\\Nested", L"UndoOnly", L"earlier deletion");
        Op del{Action::Delete, Kind::Verb, Root::User, L"*\\shell\\TrashClash"};
        Check(Apply(roots, del) == OpResult::Conflict && Apply(roots, Inverse(del)) == OpResult::Conflict &&
                  GetString(L"User\\*\\shell\\TrashClash", L"SourceOnly") == L"new registration" &&
                  GetString(L"TrashUser\\*\\shell\\TrashClash\\Nested", L"UndoOnly") == L"earlier deletion" &&
                  !HasValue(L"TrashUser\\*\\shell\\TrashClash", L"SourceOnly"),
              "delete and restore refuse an occupied target while retaining the earlier undo copy");
        RegDeleteTreeW(HKEY_CURRENT_USER, (g_base + L"\\User\\*\\shell\\TrashClash").c_str());
        RegDeleteTreeW(HKEY_CURRENT_USER, (g_base + L"\\TrashUser\\*\\shell\\TrashClash").c_str());
    }
    Op gone{Action::Disable, Kind::Verb, Root::Machine, L"*\\shell\\DoesNotExist"};
    Check(Apply(roots, gone) == OpResult::NotFound, "missing key reports NotFound");

    // ---- batches for the elevated helper ------------------------------------
    std::vector<Op> batch = {Op{Action::Disable, Kind::Verb, Root::Machine, L"Directory\\Background\\shell\\git_gui", true, 7},
                             Op{Action::Enable, Kind::ShellEx, Root::Machine, L"*\\shellex\\-ContextMenuHandlers\\Qux"}};
    Op place_op{Action::Position, Kind::Verb, Root::Machine, L"Directory\\shell\\git_shell"};
    place_op.place = Place::Bottom;
    std::vector<Op> parsed;
    Check(ParseBatch(SerializeBatch({place_op}), parsed) && parsed.size() == 1 &&
              parsed[0].action == Action::Position && parsed[0].place == Place::Bottom && !parsed[0].planned,
          "batch round trip keeps a position op");
    Check(ParseBatch(SerializeBatch(batch), parsed) && parsed.size() == 2 && parsed[0].key_path == batch[0].key_path &&
              parsed[0].planned && parsed[0].mask == 7 && parsed[1].kind == Kind::ShellEx &&
              parsed[1].action == Action::Enable && !parsed[1].planned,
          "batch round trip keeps the verb plan");
    const wchar_t* bad[] = {
        L"PULSE-SYSCTX 1\ndisable\tverb\tmachine\t..\\..\\Microsoft\\Windows\\shell\\x\n",
        L"PULSE-SYSCTX 1\ndisable\tverb\tmachine\tC:\\x\\shell\\y\n",
        L"PULSE-SYSCTX 1\ndisable\tverb\tmachine\t\\*\\shell\\y\n",
        L"PULSE-SYSCTX 1\ndisable\tverb\tmachine\t*\\shellex\\y\n",
        L"PULSE-SYSCTX 1\ndisable\tshellex\tmachine\t*\\shell\\ContextMenuHandlers\\y\n",
        L"PULSE-SYSCTX 1\nremove\tverb\tmachine\t*\\shell\\y\n",  // delete / restore are real ops now
        L"PULSE-SYSCTX 1\ndisable\tshellnew\tmachine\t.txt\\ShellNew\n",
        L"disable\tverb\tmachine\t*\\shell\\y\n",
        L"PULSE-SYSCTX 1\ndisable\tverb\tmachine\t*\\shell\\y\t99\n",
        L"PULSE-SYSCTX 1\ndisable\tshellex\tmachine\t*\\shellex\\ContextMenuHandlers\\y\t3\n",
        L"PULSE-SYSCTX 1\nposition\tshellex\tmachine\t*\\shellex\\ContextMenuHandlers\\y\ttop\n",
        L"PULSE-SYSCTX 1\nposition\tverb\tmachine\t*\\shell\\y\tleft\n",
        L"PULSE-SYSCTX 1\nposition\tverb\tmachine\t*\\shell\\y\n",
    };
    bool all_rejected = true;
    for (const auto* b : bad) all_rejected = !ParseBatch(b, parsed) && all_rejected;
    Check(all_rejected, "batch parser rejects traversal, absolute paths, wrong layouts and unknown ops");

    Check(ParseBatch(L"PULSE-SYSCTX 1\nenable\tverb\tmachine\t*\\shell\\legacy\t15\n", parsed) &&
              parsed.size() == 1 && parsed[0].planned && parsed[0].mask == 15 && !parsed[0].has_previous_values &&
              ParseBatch(L"PULSE-SYSCTX 1\nposition\tverb\tmachine\t*\\shell\\legacy\ttop\n", parsed) &&
              parsed.size() == 1 && parsed[0].place == Place::Top,
          "version 2 parser retains legacy version 1 masks and positions");
    const wchar_t* malformed_snapshots[] = {
        L"PULSE-SYSCTX 2\nenable\tverb\tmachine\t*\\shell\\x\t1\trestore|command:1:00\n",
        L"PULSE-SYSCTX 2\nenable\tverb\tmachine\t*\\shell\\x\t1\trestore|velocity:4:0\n",
        L"PULSE-SYSCTX 2\nenable\tverb\tmachine\t*\\shell\\x\t1\trestore|velocity:4:zz\n",
        L"PULSE-SYSCTX 2\nenable\tverb\tmachine\t*\\shell\\x\t1\trestore|velocity:4294967296:00\n",
        L"PULSE-SYSCTX 2\nenable\tverb\tmachine\t*\\shell\\x\t1\trestore|velocity:-:00\n",
        L"PULSE-SYSCTX 2\nenable\tverb\tmachine\t*\\shell\\x\t1\trestore|velocity:-;velocity:-\n",
        L"PULSE-SYSCTX 2\nenable\tverb\tmachine\t*\\shell\\x\t3\trestore|velocity:-\n",
        L"PULSE-SYSCTX 2\nenable\tverb\tmachine\t*\\shell\\x\t1\trestore|legacy:-\n",
        L"PULSE-SYSCTX 2\nenable\tverb\tmachine\t*\\shell\\x\t-\trestore|velocity:-\n",
        L"PULSE-SYSCTX 2\ndelete\tverb\tmachine\t*\\shell\\x\t-\trestore|position:-\n",
        L"PULSE-SYSCTX 2\ndisable\tshellex\tmachine\t*\\shellex\\ContextMenuHandlers\\x\t-\trestore|position:-\n",
        L"PULSE-SYSCTX 2\nposition\tverb\tmachine\t*\\shell\\x\ttop,default\trestore|velocity:-\n",
        L"PULSE-SYSCTX 2\nenable\tverb\tmachine\t*\\shell\\x\t1\trestore|velocity:-;\n",
        L"PULSE-SYSCTX 2\nenable\tverb\tmachine\t..\\shell\\x\t1\trestore|velocity:-\n",
        L"PULSE-SYSCTX 2\nremove\tverb\tmachine\t*\\shell\\x\t1\trestore|velocity:-\n",
        L"PULSE-SYSCTX 2\nenable\tverb\tmachine\t*\\shell\\x\t1\trestore|velocity:-\textra\n",
    };
    bool malformed_rejected = true;
    for (const auto* text : malformed_snapshots)
        malformed_rejected = !ParseBatch(text, parsed) && parsed.empty() && malformed_rejected;
    Check(malformed_rejected, "raw snapshot parser rejects arbitrary names, malformed data, duplicates and mismatched actions or masks");
    {
        Op payload{Action::Enable, Kind::Verb, Root::Machine, L"*\\shell\\large", true, 1};
        payload.has_previous_values = true;
        payload.previous_values.push_back({ValueName::Velocity, true, REG_BINARY,
                                           std::vector<BYTE>(kMaxValueSnapshotBytes, 0xAC)});
        const bool at_limit = ParseBatch(SerializeBatch({payload}), parsed) && parsed.size() == 1 &&
                              parsed[0].previous_values[0].data.size() == kMaxValueSnapshotBytes;
        const std::wstring oversized = L"PULSE-SYSCTX 2\nenable\tverb\tmachine\t*\\shell\\large\t1\trestore|velocity:3:" +
                                       std::wstring((kMaxValueSnapshotBytes + 1) * 2, L'0') + L"\n";
        Check(at_limit && !ParseBatch(oversized, parsed) && SerializeBatch(std::vector<Op>(8, payload)).empty() &&
                  !ParseBatch(std::wstring(kMaxBatchBytes + 1, L' '), parsed) &&
                  SerializeBatch(std::vector<Op>(kMaxBatchOperations + 1, batch[0])).empty(),
              "batch snapshot allocation, operation count and total wire size are bounded");
        payload.previous_values.push_back(payload.previous_values[0]);
        const std::wstring snap = Snapshot(roots);
        Check(SerializeBatch({payload}).empty() && Apply(roots, payload) == OpResult::Unsupported && Snapshot(roots) == snap,
              "direct Apply also refuses invalid duplicate snapshots before touching the registry");
    }

    {
        const std::vector<Op> trash_ops = {
            Op{Action::Delete, Kind::ShellEx, Root::Machine, L"*\\shellex\\ContextMenuHandlers\\Gone"},
            Op{Action::Restore, Kind::Verb, Root::Machine, L"*\\shell\\Gone"}};
        std::vector<Op> back;
        const bool round_trip = ParseBatch(SerializeBatch(trash_ops), back) && back.size() == 2 &&
                                back[0].action == Action::Delete && back[1].action == Action::Restore &&
                                back[0].key_path == trash_ops[0].key_path;
        Check(round_trip && !ParseBatch(L"PULSE-SYSCTX 1\ndelete\tverb\tmachine\t*\\shell\\x\t3\n", back) &&
                  !ParseBatch(L"PULSE-SYSCTX 1\ndelete\tverb\tmachine\t..\\shell\\x\n", back),
              "batch: delete / restore round trip; masks and traversal refused");
    }
    std::vector<OpResult> results;
    Check(ParseResults(SerializeResults({OpResult::Ok, OpResult::Conflict, OpResult::AccessDenied}), results) &&
              results.size() == 3 && results[1] == OpResult::Conflict && results[2] == OpResult::AccessDenied &&
              !ParseResults(L"PULSE-SYSCTX-RESULT 1\nmaybe\n", results) && !ParseResults(L"ok\n", results),
          "helper result file round trip");
    // The helper refuses per-user ops and malformed input before touching anything.
    const std::wstring batch_path = fixture.directory + L"\\batch.txt";
    const std::wstring result_path = fixture.directory + L"\\result.txt";
    bool helper_fixture = CreateFixtureFile(result_path, FILE_ATTRIBUTE_NORMAL);
    {
        HANDLE f = CreateFileW(batch_path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        const char user_batch[] = "PULSE-SYSCTX 1\ndisable\tverb\tuser\t*\\shell\\FooVerb\n";
        DWORD written = 0;
        if (f == INVALID_HANDLE_VALUE) {
            FixtureError("CreateFileW", GetLastError(), batch_path);
            helper_fixture = false;
        } else {
            const bool saved = WriteFile(f, user_batch, sizeof(user_batch) - 1, &written, nullptr) && written == sizeof(user_batch) - 1;
            if (!saved) FixtureError("WriteFile", GetLastError(), batch_path);
            helper_fixture = saved && helper_fixture;
            CloseHandle(f);
        }
    }
    Check(helper_fixture && RunElevatedBatch(batch_path.c_str(), result_path.c_str()) == 2 &&
              RunElevatedBatch(L"Z:\\no\\such.txt", result_path.c_str()) == 2,
          "elevated helper rejects user-root and unreadable batches");
    DeleteFileW(batch_path.c_str());
    DeleteFileW(result_path.c_str());

    const std::wstring reg = ExportReg(roots, {Op{Action::Disable, Kind::Verb, Root::Machine, L"Directory\\shell\\BarVerb"}});
    Check(reg.rfind(L"Windows Registry Editor Version 5.00", 0) == 0 &&
              reg.find(L"Directory\\shell\\BarVerb]") != std::wstring::npos &&
              reg.find(L"\"CommandFlags\"=dword:00000028") != std::wstring::npos &&
              reg.find(L"\"Extended\"=\"\"") != std::wstring::npos,
          "ExportReg writes REGEDIT5 text");

    {
        const std::wstring backup = roots.sendto_dir + L"\\checked-backup.reg";
        DWORD backup_error = ERROR_SUCCESS;
        const std::vector<Op> backup_ops = {
            Op{Action::Disable, Kind::Verb, Root::Machine, L"Directory\\shell\\BarVerb"}};
        Check(WriteRegBackup(roots, backup_ops, backup, backup_error) && backup_error == ERROR_SUCCESS &&
                  GetFileAttributesW(backup.c_str()) != INVALID_FILE_ATTRIBUTES,
              "checked backup writes and flushes a new complete REGEDIT5 file");
        Check(!WriteRegBackup(roots, backup_ops, backup, backup_error) && backup_error == ERROR_FILE_EXISTS,
              "backup refuses to overwrite an existing recovery file");
        const std::wstring incomplete = roots.sendto_dir + L"\\incomplete-backup.reg";
        auto missing = backup_ops;
        missing.push_back(Op{Action::Disable, Kind::Verb, Root::User, L"*\\shell\\NoSuchKey"});
        Check(!WriteRegBackup(roots, missing, incomplete, backup_error) && backup_error == ERROR_FILE_NOT_FOUND &&
                  GetFileAttributesW(incomplete.c_str()) == INVALID_FILE_ATTRIBUTES,
              "an unreadable later key rejects the whole backup instead of saving a partial export");
        DeleteFileW(backup.c_str());
    }

    // ---- cleanup -------------------------------------------------------------
    Check(fixture.Cleanup(), "isolated registry and filesystem fixtures are removed");
    std::printf("%s (%d failure%s)\n", g_failures ? "FAILED" : "ALL PASSED", g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}

int wmain(int argc, wchar_t** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("[INIT] system context registry test starting\n");
    try { return Run(argc, argv); }
    catch (const std::exception& error) {
        std::printf("[FAIL] fixture exception: %s\n", error.what());
        return 2;
    }
}
