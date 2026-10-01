#include "user_storage.h"
#include "json_utils.h"
#include "utf8_file.h"
#include <shlobj.h>
#include <filesystem>
#include <mutex>
#include <vector>
#include <array>

namespace pulse::storage {
namespace {
namespace fs = std::filesystem;
struct Locator { std::wstring active[2], pending[2], error[2]; };
std::recursive_mutex gate;
std::wstring test_root;
std::wstring test_locator_key;
Locator cache;
bool loaded = false;
size_t Slot(Kind kind) { return kind == Kind::Configuration ? 0 : 1; }
std::wstring Anchor() { return DefaultRoot() + L"\\storage-locations.json"; }
std::wstring LocatorKey() {
    if (!test_root.empty()) return test_locator_key;
#ifdef PULSE_WITH_SELFTEST
    wchar_t isolated[2]{};
    if (GetEnvironmentVariableW(L"PULSE_TEST_DATA_DIR", isolated, ARRAYSIZE(isolated))) return {};
#endif
    return L"Software\\Pulse";
}
bool ReadRegistryLocator(const std::wstring& key, std::wstring& text) {
    DWORD bytes = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, key.c_str(), L"StorageLocations", RRF_RT_REG_SZ,
                    nullptr, nullptr, &bytes) != ERROR_SUCCESS || bytes > 1024 * 1024) return false;
    std::vector<wchar_t> data(bytes / sizeof(wchar_t) + 1);
    if (RegGetValueW(HKEY_CURRENT_USER, key.c_str(), L"StorageLocations", RRF_RT_REG_SZ,
                    nullptr, data.data(), &bytes) != ERROR_SUCCESS) return false;
    text.assign(data.data()); return true;
}
bool WriteRegistryLocator(const std::wstring& key, const std::wstring& text) {
    HKEY registry = nullptr;
    const auto created = RegCreateKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, nullptr, 0, KEY_SET_VALUE,
                        nullptr, &registry, nullptr);
    if (created != ERROR_SUCCESS) { SetLastError(created); return false; }
    const auto status = RegSetValueExW(registry, L"StorageLocations", 0, REG_SZ,
        reinterpret_cast<const BYTE*>(text.c_str()), static_cast<DWORD>((text.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(registry); SetLastError(status); return status == ERROR_SUCCESS;
}
bool Same(const std::wstring& a, const std::wstring& b) {
    return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL;
}
std::wstring Normalize(const std::wstring& input) {
    if (input.empty() || input.find(L'\0') != std::wstring::npos || !fs::path(input).is_absolute()) return {};
    std::wstring value = fs::path(input).lexically_normal().wstring();
    while (value.size() > 3 && (value.back() == L'\\' || value.back() == L'/')) value.pop_back();
    return value;
}
bool Within(const std::wstring& child, const std::wstring& parent) {
    return Same(child, parent) || (child.size() > parent.size() && child[parent.size()] == L'\\' &&
        CompareStringOrdinal(child.c_str(), static_cast<int>(parent.size()), parent.c_str(),
                             static_cast<int>(parent.size()), TRUE) == CSTR_EQUAL);
}
bool NoReparse(const std::wstring& path, std::wstring& error) {
    for (fs::path current(path); !current.empty(); current = current.parent_path()) {
        const DWORD attrs = GetFileAttributesW(current.c_str());
        if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_REPARSE_POINT)) {
            error = L"目录不能包含符号链接或联接点。"; return false;
        }
        if (current == current.parent_path()) break;
    }
    return true;
}
struct Lock {
    HANDLE file = INVALID_HANDLE_VALUE;
    Lock() {
        if (DefaultRoot().empty()) return;
        std::error_code ec;
        fs::create_directories(DefaultRoot(), ec);
        const auto path = DefaultRoot() + L"\\storage-locations.lock";
        for (unsigned attempt = 0; attempt < 500; ++attempt) {
            file = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                               OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file != INVALID_HANDLE_VALUE || GetLastError() != ERROR_SHARING_VIOLATION) break;
            Sleep(20);
        }
    }
    ~Lock() { if (file != INVALID_HANDLE_VALUE) CloseHandle(file); }
    explicit operator bool() const { return file != INVALID_HANDLE_VALUE; }
};
Locator Read(bool allow_import = true) {
    Locator result;
    if (DefaultRoot().empty()) return result;
    std::wstring text;
    const auto key = LocatorKey();
    bool found = !key.empty() && ReadRegistryLocator(key, text);
    if (!found && ReadUtf8File(Anchor(), text)) {
        found = true;
        // Import older file locators before users remove their former AppData directory.
        if (allow_import && !key.empty()) WriteRegistryLocator(key, text);
    }
    if (found) {
        result.active[0] = json::ExtractString(text, L"configuration");
        result.active[1] = json::ExtractString(text, L"index");
        result.pending[0] = json::ExtractString(text, L"pending_configuration");
        result.pending[1] = json::ExtractString(text, L"pending_index");
        result.error[0] = json::ExtractString(text, L"configuration_error");
        result.error[1] = json::ExtractString(text, L"index_error");
    }
    return result;
}
bool Save(const Locator& value, std::wstring& error) {
    if (DefaultRoot().empty()) { error = L"无法确定默认存储目录。"; return false; }
    const wchar_t* keys[] = {L"configuration", L"index", L"pending_configuration", L"pending_index", L"configuration_error", L"index_error"};
    const std::wstring* fields[] = {&value.active[0], &value.active[1], &value.pending[0], &value.pending[1], &value.error[0], &value.error[1]};
    std::wstring text = L"{\n";
    for (size_t i = 0; i < 6; ++i) {
        text += L"  \"" + std::wstring(keys[i]) + L"\":\"";
        json::Escape(*fields[i], text); text += (i == 5 ? L"\"\n}" : L"\",\n");
    }
    const auto key = LocatorKey();
    const bool saved = key.empty() ? WriteUtf8FileAtomic(Anchor(), text) : WriteRegistryLocator(key, text);
    if (!saved) { error = L"无法保存存储位置设置（系统错误 " + std::to_wstring(GetLastError()) + L"）。"; return false; }
    cache = value; loaded = true; return true;
}
void LoadCache() { if (!loaded) { Lock disk; cache = Read(static_cast<bool>(disk)); loaded = true; } }
std::wstring Root(const Locator& value, size_t slot) {
    return value.active[slot].empty() ? DefaultRoot() : value.active[slot];
}
bool Validate(const Locator& value, const std::wstring& target, std::wstring& error) {
    if (target.empty() || fs::path(target) == fs::path(target).root_path() ||
        target.rfind(L"\\\\?", 0) == 0 || target.rfind(L"\\\\.", 0) == 0) {
        error = L"请选择完整的独立文件夹路径，不能使用磁盘根目录。"; return false;
    }
    for (size_t slot = 0; slot < 2; ++slot) {
        const auto active = Root(value, slot);
        if (Within(target, active) || Within(active, target)) {
            error = L"目标目录不能与当前配置或索引目录重叠。"; return false;
        }
        if (!value.pending[slot].empty() && Same(target, value.pending[slot])) continue;
        if (!value.pending[slot].empty() && (Within(target, value.pending[slot]) || Within(value.pending[slot], target))) {
            error = L"目标目录不能与待迁移目录重叠。"; return false;
        }
    }
    if (!NoReparse(target, error)) return false;
    std::error_code ec;
    fs::create_directories(target, ec);
    if (ec || !fs::is_empty(target, ec) || ec) { error = L"目标目录必须为空且可写。"; return false; }
    const auto probe = target + L"\\.pulse-storage-probe";
    HANDLE file = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                               FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (file == INVALID_HANDLE_VALUE) { error = L"无法写入目标目录。"; return false; }
    CloseHandle(file); return true;
}
struct File {
    HANDLE value = INVALID_HANDLE_VALUE;
    ~File() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
bool CopyVerified(const fs::path& input, const fs::path& output, std::vector<fs::path>& copied) {
    File source{CreateFileW(input.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
    if (source.value == INVALID_HANDLE_VALUE || !CopyFileW(input.c_str(), output.c_str(), TRUE)) return false;
    copied.push_back(output);
    File destination{CreateFileW(output.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ,
                                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
    if (destination.value == INVALID_HANDLE_VALUE || !FlushFileBuffers(destination.value)) return false;
    LARGE_INTEGER a{}, b{};
    if (!GetFileSizeEx(source.value, &a) || !GetFileSizeEx(destination.value, &b) || a.QuadPart != b.QuadPart) return false;
    std::array<unsigned char, 65536> first{}, second{};
    for (;;) {
        DWORD source_bytes = 0, destination_bytes = 0;
        if (!ReadFile(source.value, first.data(), static_cast<DWORD>(first.size()), &source_bytes, nullptr) ||
            !ReadFile(destination.value, second.data(), static_cast<DWORD>(second.size()), &destination_bytes, nullptr) ||
            source_bytes != destination_bytes || !std::equal(first.begin(), first.begin() + source_bytes, second.begin())) return false;
        if (!source_bytes) return true;
    }
}
}
std::wstring DefaultRoot() {
    std::lock_guard lock(gate);
    if (!test_root.empty()) return test_root;
#ifdef PULSE_WITH_SELFTEST
    wchar_t isolated[32768]{};
    DWORD size = GetEnvironmentVariableW(L"PULSE_TEST_DATA_DIR", isolated, ARRAYSIZE(isolated));
    if (size && size < ARRAYSIZE(isolated)) return isolated;
#endif
    PWSTR local = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local)) || !local) return {};
    const std::wstring result = std::wstring(local) + L"\\Pulse";
    CoTaskMemFree(local); return result;
}
void OverrideDefaultRootForTesting(const std::wstring& root, const std::wstring& locator_registry_key) {
    std::lock_guard lock(gate); test_root = Normalize(root); test_locator_key = locator_registry_key;
    cache = {}; loaded = false;
}
std::wstring ConfigurationRoot() { std::lock_guard lock(gate); LoadCache(); return Root(cache, 0); }
std::wstring UserIndexRoot() { std::lock_guard lock(gate); LoadCache(); return Root(cache, 1); }
std::wstring Pending(Kind kind) { std::lock_guard lock(gate); LoadCache(); return cache.pending[Slot(kind)]; }
std::wstring LastError(Kind kind) { std::lock_guard lock(gate); LoadCache(); return cache.error[Slot(kind)]; }
void Refresh() { std::lock_guard lock(gate); Lock disk; cache = Read(static_cast<bool>(disk)); loaded = true; }
bool Schedule(Kind kind, const std::wstring& requested, std::wstring& error) {
    std::lock_guard lock(gate); error.clear(); Lock disk;
    if (!disk) { error = L"存储位置设置正被其他进程使用。"; return false; }
    auto value = Read(); const auto slot = Slot(kind); const auto target = Normalize(requested);
    if (Same(target, Root(value, slot))) { value.pending[slot].clear(); value.error[slot].clear(); return Save(value, error); }
    if (!Validate(value, target, error)) return false;
    if (!value.pending[1 - slot].empty() && Same(target, value.pending[1 - slot])) {
        error = L"配置和索引必须使用不同目录。"; return false;
    }
    value.pending[slot] = target; value.error[slot].clear(); return Save(value, error);
}
bool Commit(Kind kind, const std::wstring& target, std::wstring& error) {
    std::lock_guard lock(gate); Lock disk;
    if (!disk) { error = L"无法锁定存储位置设置。"; return false; }
    auto value = Read(); const auto slot = Slot(kind);
    const auto normalized = Normalize(target);
    if (normalized.empty() || !Same(value.pending[slot], normalized)) { error = L"迁移请求已更改，未切换存储位置。"; return false; }
    if (!NoReparse(normalized, error)) return false;
    value.active[slot] = target; value.pending[slot].clear(); value.error[slot].clear(); return Save(value, error);
}
bool ClearPending(Kind kind, std::wstring& error) {
    std::lock_guard lock(gate); Lock disk;
    if (!disk) { error = L"无法锁定存储位置设置。"; return false; }
    auto value = Read(); value.pending[Slot(kind)].clear(); value.error[Slot(kind)].clear(); return Save(value, error);
}
void RecordError(Kind kind, const std::wstring& error) {
    std::lock_guard lock(gate); Lock disk; if (!disk) return;
    auto value = Read(); value.error[Slot(kind)] = error; std::wstring ignored; Save(value, ignored);
}
bool ApplyConfiguration(std::wstring& error) {
    std::lock_guard lock(gate); error.clear(); Lock disk;
    if (!disk) { error = L"无法锁定配置迁移设置。"; return false; }
    auto value = Read(); cache = value; loaded = true;
    const auto target = value.pending[0]; if (target.empty()) return true;
    const auto source = Root(value, 0);
    std::vector<fs::path> copied;
    auto fail = [&]() {
        for (auto it = copied.rbegin(); it != copied.rend(); ++it) DeleteFileW(it->c_str());
        value.error[0] = error; std::wstring ignored; Save(value, ignored); return false;
    };
    try {
    if (!Validate(value, target, error) || !NoReparse(source, error)) return fail();
    const wchar_t* names[] = {L"app.json", L"session.json", L"places.json", L"tags.json", L"context_menu.json", L"saved_searches.json", L"search_history.json", L"folder_sizes.json", L"operations.json", L"network-index.json", L"content-index.config"};
    std::vector<fs::path> inputs;
    for (auto name : names) inputs.push_back(fs::path(source) / name);
    const auto modern_content = fs::path(source) / L"content-index.config";
    if (GetFileAttributesW(modern_content.c_str()) == INVALID_FILE_ATTRIBUTES &&
        (GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_PATH_NOT_FOUND)) {
        inputs.push_back(fs::path(source) / L"ContentIndex" / L"content-v1.sqlite.config");
    }
    std::error_code ec;
    if (fs::exists(source, ec)) {
        for (fs::directory_iterator it(source, ec), end; !ec && it != end; it.increment(ec)) {
            const auto name = it->path().filename().wstring();
            if (name.rfind(L"wallpaper.", 0) == 0) inputs.push_back(it->path());
        }
        if (ec) { error = L"无法读取原配置目录。"; return fail(); }
    }
    for (const auto& input : inputs) {
        const DWORD attrs = GetFileAttributesW(input.c_str());
        if (attrs == INVALID_FILE_ATTRIBUTES) {
            if (GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_PATH_NOT_FOUND) continue;
            error = L"无法读取原配置文件。"; return fail();
        }
        if ((attrs & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0) {
            error = L"配置文件不能是目录、符号链接或联接点。"; return fail();
        }
        if (!NoReparse(input.wstring(), error)) return fail();
        const auto output_name = input.filename() == L"content-v1.sqlite.config" ?
            fs::path(L"content-index.config") : input.filename();
        const auto output = fs::path(target) / output_name;
        if (!CopyVerified(input, output, copied)) { error = L"无法复制或验证配置文件，原配置已保留。"; return fail(); }
    }
    const auto app = target + L"\\app.json";
    if (GetFileAttributesW(app.c_str()) != INVALID_FILE_ATTRIBUTES) {
        std::wstring text;
        if (!ReadUtf8File(app, text)) { error = L"无法读取已复制的应用配置。"; return fail(); }
        const auto wallpaper = json::ExtractString(text, L"background_image");
        if (!wallpaper.empty() && Same(fs::path(wallpaper).parent_path().wstring(), source) &&
            fs::path(wallpaper).filename().wstring().rfind(L"wallpaper.", 0) == 0) {
            size_t begin = json::ValuePosition(text, L"background_image");
            if (begin != std::wstring::npos && text[begin] == L'"') {
                size_t end = begin + 1; json::UnescapeString(text, end);
                std::wstring replacement = L"\"";
                json::Escape((fs::path(target) / fs::path(wallpaper).filename()).wstring(), replacement);
                replacement += L'"'; text.replace(begin, end - begin, replacement);
                if (!WriteUtf8FileAtomic(app, text)) { error = L"无法更新壁纸位置。"; return fail(); }
            }
        }
    }
    auto committed = value;
    committed.active[0] = target; committed.pending[0].clear(); committed.error[0].clear();
    if (!Save(committed, error)) return fail();
    return true;
    } catch (const std::exception&) {
        error = L"配置迁移失败，原配置已保留。"; return fail();
    }
}
}


