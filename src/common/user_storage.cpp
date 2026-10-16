#include "user_storage.h"
#include "localization.h"
#include "json_utils.h"
#include "utf8_file.h"
#include <shlobj.h>
#include <filesystem>
#include <mutex>
#include <vector>
#include <array>
#include <cstddef>
#include <cstring>
#include <aclapi.h>

namespace pulse::storage {
namespace {
namespace fs = std::filesystem;
struct Locator { std::wstring active[2], pending[2], error[2]; };
std::recursive_mutex gate;
std::wstring test_root;
std::wstring test_locator_key;
Locator cache;
bool loaded = false;
std::function<bool(const std::wstring&)> copy_hook;
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
            error = l10n::Get(l10n::StringId::StorageReparseNotAllowed); return false;
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
    if (DefaultRoot().empty()) { error = l10n::Get(l10n::StringId::StorageDefaultUnavailable); return false; }
    const wchar_t* keys[] = {L"configuration", L"index", L"pending_configuration", L"pending_index", L"configuration_error", L"index_error"};
    const std::wstring* fields[] = {&value.active[0], &value.active[1], &value.pending[0], &value.pending[1], &value.error[0], &value.error[1]};
    std::wstring text = L"{\n";
    for (size_t i = 0; i < 6; ++i) {
        text += L"  \"" + std::wstring(keys[i]) + L"\":\"";
        json::Escape(*fields[i], text); text += (i == 5 ? L"\"\n}" : L"\",\n");
    }
    const auto key = LocatorKey();
    const bool saved = key.empty() ? WriteUtf8FileAtomic(Anchor(), text) : WriteRegistryLocator(key, text);
    if (!saved) { error = l10n::Format(l10n::StringId::StorageSaveFailedFormat, GetLastError()); return false; }
    cache = value; loaded = true; return true;
}
void LoadCache() { if (!loaded) { Lock disk; cache = Read(static_cast<bool>(disk)); loaded = true; } }
std::wstring Root(const Locator& value, size_t slot) {
    return value.active[slot].empty() ? DefaultRoot() : value.active[slot];
}
bool Validate(const Locator& value, const std::wstring& target, std::wstring& error) {
    if (target.empty() || fs::path(target) == fs::path(target).root_path() ||
        target.rfind(L"\\\\?", 0) == 0 || target.rfind(L"\\\\.", 0) == 0) {
        error = l10n::Get(l10n::StringId::StorageInvalidTarget); return false;
    }
    for (size_t slot = 0; slot < 2; ++slot) {
        const auto active = Root(value, slot);
        if (Within(target, active) || Within(active, target)) {
            error = l10n::Get(l10n::StringId::StorageActiveOverlap); return false;
        }
        if (!value.pending[slot].empty() && Same(target, value.pending[slot])) continue;
        if (!value.pending[slot].empty() && (Within(target, value.pending[slot]) || Within(value.pending[slot], target))) {
            error = l10n::Get(l10n::StringId::StoragePendingOverlap); return false;
        }
    }
    if (!NoReparse(target, error)) return false;
    std::error_code ec;
    fs::create_directories(target, ec);
    if (ec || !fs::is_empty(target, ec) || ec) { error = l10n::Get(l10n::StringId::StorageTargetNotEmptyWritable); return false; }
    const auto probe = target + L"\\.pulse-storage-probe";
    HANDLE file = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                               FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (file == INVALID_HANDLE_VALUE) { error = l10n::Get(l10n::StringId::StorageTargetWriteFailed); return false; }
    CloseHandle(file); return true;
}
struct File {
    HANDLE value = INVALID_HANDLE_VALUE;
    bool rollback = false;
    fs::path path;
    FILE_BASIC_INFO basic{};
    explicit File(HANDLE handle) : value(handle) {}
    File(const File&) = delete;
    File& operator=(const File&) = delete;
    File(File&& other) noexcept : value(other.value), rollback(other.rollback), path(std::move(other.path)), basic(other.basic) {
        other.value = INVALID_HANDLE_VALUE;
    }
    ~File() {
        if (value == INVALID_HANDLE_VALUE) return;
        if (rollback) {
            FILE_BASIC_INFO writable{};
            writable.FileAttributes = FILE_ATTRIBUTE_NORMAL;
            SetFileInformationByHandle(value, FileBasicInfo, &writable, sizeof(writable));
            FILE_DISPOSITION_INFO disposition{TRUE};
            SetFileInformationByHandle(value, FileDispositionInfo, &disposition, sizeof(disposition));
        }
        CloseHandle(value);
    }
};
bool LeaseDirectories(const fs::path& path, std::vector<File>& leases) {
    std::vector<fs::path> ancestors;
    for (auto current = path; !current.empty(); current = current.parent_path()) {
        ancestors.push_back(current);
        if (current == current.parent_path()) break;
    }
    for (auto it = ancestors.rbegin(); it != ancestors.rend(); ++it) {
        File directory{CreateFileW(it->c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ, nullptr,
            OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
        BY_HANDLE_FILE_INFORMATION info{};
        if (directory.value == INVALID_HANDLE_VALUE || !GetFileInformationByHandle(directory.value, &info) ||
            !(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
        leases.push_back(std::move(directory));
    }
    return true;
}
bool WriteText(HANDLE file, const std::wstring& text) {
    std::vector<uint8_t> bytes;
    LARGE_INTEGER begin{};
    if (!EncodeUtf8Bytes(text, bytes) || !SetFilePointerEx(file, begin, nullptr, FILE_BEGIN)) return false;
    size_t done = 0;
    while (done < bytes.size()) {
        DWORD written = 0;
        const auto count = static_cast<DWORD>((std::min)(bytes.size() - done, size_t{65536}));
        if (!WriteFile(file, bytes.data() + done, count, &written, nullptr) || !written) return false;
        done += written;
    }
    return SetEndOfFile(file) && FlushFileBuffers(file);
}
// This migration writes a single data stream. Refuse metadata that it cannot preserve
// before creating the destination instead of silently dropping streams or decrypting files.
bool IsSupportedPlainFile(HANDLE source, FILE_BASIC_INFO& basic) {
    constexpr DWORD supported = FILE_ATTRIBUTE_ARCHIVE | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM |
        FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_TEMPORARY | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED | FILE_ATTRIBUTE_NORMAL;
    if (!GetFileInformationByHandleEx(source, FileBasicInfo, &basic, sizeof(basic)) ||
        (basic.FileAttributes & ~supported)) return false;
    PACL resources = nullptr;
    PSECURITY_DESCRIPTOR security = nullptr;
    const DWORD security_status = GetSecurityInfo(source, SE_FILE_OBJECT, ATTRIBUTE_SECURITY_INFORMATION,
        nullptr, nullptr, nullptr, &resources, &security);
    const bool no_resources = security_status == ERROR_SUCCESS && (!resources || resources->AceCount == 0);
    if (security) LocalFree(security);
    if (!no_resources) return false;
    void* context = nullptr;
    std::array<BYTE, 65536> buffer{};
    size_t offset = 0, available = 0;
    auto read_header = [&](WIN32_STREAM_ID& stream, DWORD& bytes) {
        bytes = 0;
        constexpr DWORD header_size = static_cast<DWORD>(offsetof(WIN32_STREAM_ID, cStreamName));
        while (bytes < header_size) {
            if (offset == available) {
                DWORD read = 0;
                if (!BackupRead(source, buffer.data(), static_cast<DWORD>(buffer.size()), &read, FALSE, FALSE, &context)) return false;
                offset = 0; available = read;
                if (!read) break;
            }
            const auto count = (std::min)(available - offset, size_t{header_size - bytes});
            std::memcpy(reinterpret_cast<BYTE*>(&stream) + bytes, buffer.data() + offset, count);
            offset += count;
            bytes += static_cast<DWORD>(count);
        }
        return bytes == 0 || bytes == header_size;
    };
    bool supported_streams = true;
    for (;;) {
        WIN32_STREAM_ID stream{};
        DWORD read = 0;
        if (!read_header(stream, read)) { supported_streams = false; break; }
        if (!read) break;
        if (stream.dwStreamId != BACKUP_DATA || stream.dwStreamNameSize || stream.Size.QuadPart < 0) {
            supported_streams = false; break;
        }
        const auto buffered = (std::min)(static_cast<uint64_t>(available - offset), static_cast<uint64_t>(stream.Size.QuadPart));
        offset += static_cast<size_t>(buffered);
        ULARGE_INTEGER remaining{};
        remaining.QuadPart = static_cast<uint64_t>(stream.Size.QuadPart) - buffered;
        if (remaining.QuadPart) {
            DWORD low = 0, high = 0;
            if (!BackupSeek(source, remaining.LowPart, remaining.HighPart, &low, &high, &context) ||
                low != remaining.LowPart || high != remaining.HighPart) {
                supported_streams = false; break;
            }
        }
    }
    DWORD ignored = 0;
    const bool released = BackupRead(source, nullptr, 0, &ignored, TRUE, FALSE, &context) != FALSE;
    LARGE_INTEGER begin{};
    return supported_streams && released && SetFilePointerEx(source, begin, nullptr, FILE_BEGIN);
}
bool CopyVerified(const fs::path& input, const fs::path& output, std::vector<File>& copied) {
    File source{CreateFileW(input.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
    BY_HANDLE_FILE_INFORMATION info{};
    if (source.value == INVALID_HANDLE_VALUE || !GetFileInformationByHandle(source.value, &info) ||
        (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) return false;
    FILE_BASIC_INFO basic{};
    if (!IsSupportedPlainFile(source.value, basic)) return false;
    File created{CreateFileW(output.c_str(), GENERIC_READ | GENERIC_WRITE | DELETE, FILE_SHARE_READ,
                            nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr)};
    if (created.value == INVALID_HANDLE_VALUE) return false;
    created.rollback = true;
    created.path = output;
    created.basic = basic;
    copied.push_back(std::move(created));
    const HANDLE destination = copied.back().value;
    std::array<unsigned char, 65536> first{}, second{};
    for (;;) {
        DWORD read = 0;
        if (!ReadFile(source.value, first.data(), static_cast<DWORD>(first.size()), &read, nullptr)) return false;
        if (!read) break;
        DWORD done = 0;
        while (done < read) {
            DWORD written = 0;
            if (!WriteFile(destination, first.data() + done, read - done, &written, nullptr) || !written) return false;
            done += written;
        }
    }
    LARGE_INTEGER begin{}, a{}, b{};
    if (!FlushFileBuffers(destination) || !GetFileSizeEx(source.value, &a) ||
        !GetFileSizeEx(destination, &b) || a.QuadPart != b.QuadPart ||
        !SetFilePointerEx(source.value, begin, nullptr, FILE_BEGIN) ||
        !SetFilePointerEx(destination, begin, nullptr, FILE_BEGIN)) return false;
    for (;;) {
        DWORD source_bytes = 0, destination_bytes = 0;
        if (!ReadFile(source.value, first.data(), static_cast<DWORD>(first.size()), &source_bytes, nullptr) ||
            !ReadFile(destination, second.data(), static_cast<DWORD>(second.size()), &destination_bytes, nullptr) ||
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
void SetConfigurationCopyHookForTesting(std::function<bool(const std::wstring&)> hook) {
    std::lock_guard lock(gate); copy_hook = std::move(hook);
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
    if (!disk) { error = l10n::Get(l10n::StringId::StorageBusy); return false; }
    auto value = Read(); const auto slot = Slot(kind); const auto target = Normalize(requested);
    if (Same(target, Root(value, slot))) { value.pending[slot].clear(); value.error[slot].clear(); return Save(value, error); }
    if (!Validate(value, target, error)) return false;
    if (!value.pending[1 - slot].empty() && Same(target, value.pending[1 - slot])) {
        error = l10n::Get(l10n::StringId::StorageSeparateDirectories); return false;
    }
    value.pending[slot] = target; value.error[slot].clear(); return Save(value, error);
}
bool Commit(Kind kind, const std::wstring& target, std::wstring& error) {
    std::lock_guard lock(gate); Lock disk;
    if (!disk) { error = l10n::Get(l10n::StringId::StorageLockFailed); return false; }
    auto value = Read(); const auto slot = Slot(kind);
    const auto normalized = Normalize(target);
    if (normalized.empty() || !Same(value.pending[slot], normalized)) { error = l10n::Get(l10n::StringId::StorageRequestChanged); return false; }
    if (!NoReparse(normalized, error)) return false;
    value.active[slot] = target; value.pending[slot].clear(); value.error[slot].clear(); return Save(value, error);
}
bool ClearPending(Kind kind, std::wstring& error) {
    std::lock_guard lock(gate); Lock disk;
    if (!disk) { error = l10n::Get(l10n::StringId::StorageLockFailed); return false; }
    auto value = Read(); value.pending[Slot(kind)].clear(); value.error[Slot(kind)].clear(); return Save(value, error);
}
void RecordError(Kind kind, const std::wstring& error) {
    std::lock_guard lock(gate); Lock disk; if (!disk) return;
    auto value = Read(); value.error[Slot(kind)] = error; std::wstring ignored; Save(value, ignored);
}
bool ApplyConfiguration(std::wstring& error) {
    std::lock_guard lock(gate); error.clear(); Lock disk;
    if (!disk) { error = l10n::Get(l10n::StringId::StorageMigrationLockFailed); return false; }
    auto value = Read(); cache = value; loaded = true;
    const auto target = value.pending[0]; if (target.empty()) return true;
    const auto source = Root(value, 0);
    std::vector<File> directories;
    std::vector<File> copied;
    auto fail = [&]() {
        copied.clear();
        value.error[0] = error; std::wstring ignored; Save(value, ignored); return false;
    };
    try {
    if (!Validate(value, target, error) || !NoReparse(source, error)) return fail();
    if (!LeaseDirectories(target, directories) || !LeaseDirectories(source, directories)) {
        error = l10n::Get(l10n::StringId::StoragePlainFileRequired); return fail();
    }
    // Path-only validation must be repeated while ancestor leases prevent replacement.
    if (!Validate(value, target, error)) return fail();
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
        if (ec) { error = l10n::Get(l10n::StringId::StorageReadSourceFailed); return fail(); }
    }
    for (const auto& input : inputs) {
        const DWORD attrs = GetFileAttributesW(input.c_str());
        if (attrs == INVALID_FILE_ATTRIBUTES) {
            if (GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_PATH_NOT_FOUND) continue;
            error = l10n::Get(l10n::StringId::StorageReadFileFailed); return fail();
        }
        if ((attrs & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0) {
            error = l10n::Get(l10n::StringId::StoragePlainFileRequired); return fail();
        }
        if (!NoReparse(input.wstring(), error) || !LeaseDirectories(input.parent_path(), directories)) {
            error = l10n::Get(l10n::StringId::StoragePlainFileRequired); return fail();
        }
        const auto output_name = input.filename() == L"content-v1.sqlite.config" ?
            fs::path(L"content-index.config") : input.filename();
        const auto output = fs::path(target) / output_name;
        if (!CopyVerified(input, output, copied) || (copy_hook && !copy_hook(output.wstring()))) { error = l10n::Get(l10n::StringId::StorageCopyVerifyFailed); return fail(); }
    }
    const auto app = target + L"\\app.json";
    if (GetFileAttributesW(app.c_str()) != INVALID_FILE_ATTRIBUTES) {
        std::wstring text;
        if (!ReadUtf8File(app, text)) { error = l10n::Get(l10n::StringId::StorageCopiedAppReadFailed); return fail(); }
        const auto wallpaper = json::ExtractString(text, L"background_image");
        if (!wallpaper.empty() && Same(fs::path(wallpaper).parent_path().wstring(), source) &&
            fs::path(wallpaper).filename().wstring().rfind(L"wallpaper.", 0) == 0) {
            size_t begin = json::ValuePosition(text, L"background_image");
            if (begin != std::wstring::npos && text[begin] == L'"') {
                size_t end = begin + 1; json::UnescapeString(text, end);
                std::wstring replacement = L"\"";
                json::Escape((fs::path(target) / fs::path(wallpaper).filename()).wstring(), replacement);
                replacement += L'"'; text.replace(begin, end - begin, replacement);
                const auto owned = std::find_if(copied.begin(), copied.end(), [&](const File& file) { return file.path == fs::path(app); });
                if (owned == copied.end() || !WriteText(owned->value, text)) { error = l10n::Get(l10n::StringId::StorageWallpaperUpdateFailed); return fail(); }
                owned->basic.LastWriteTime.QuadPart = 0;
                owned->basic.ChangeTime.QuadPart = 0;
            }
        }
    }
    for (auto& file : copied) {
        if (!SetFileInformationByHandle(file.value, FileBasicInfo, &file.basic, sizeof(file.basic)) ||
            !FlushFileBuffers(file.value)) {
            error = l10n::Get(l10n::StringId::StorageCopyVerifyFailed); return fail();
        }
    }
    auto committed = value;
    committed.active[0] = target; committed.pending[0].clear(); committed.error[0].clear();
    if (!Save(committed, error)) return fail();
    for (auto& file : copied) file.rollback = false;
    return true;
    } catch (const std::exception&) {
        error = l10n::Get(l10n::StringId::StorageMigrationFailed); return fail();
    }
}
}


