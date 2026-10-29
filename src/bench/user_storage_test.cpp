#include "../common/user_storage.h"
#include "../common/utf8_file.h"
#include "../common/json_utils.h"
#include <filesystem>
#include <cstdio>
#include <windows.h>
#include <vector>
#include <winioctl.h>

namespace fs = std::filesystem;
bool WriteBytes(const std::wstring& path, const std::vector<unsigned char>& bytes) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool ok = WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) != 0 && written == bytes.size();
    CloseHandle(file); return ok;
}
std::vector<unsigned char> ReadBytes(const std::wstring& path) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return {};
    std::vector<unsigned char> bytes(GetFileSize(file, nullptr));
    DWORD read = 0;
    const bool ok = ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr) != 0 && read == bytes.size();
    CloseHandle(file); return ok ? bytes : std::vector<unsigned char>{};
}

int ConfigurationIdentityTest() {
    using namespace pulse::storage;
    const auto fixture = fs::absolute(fs::path(L"bench_data") /
        (L"configuration-identity-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64())));
    const auto source = fixture / L"source", target = fixture / L"target";
    fs::create_directories(source);
    OverrideDefaultRootForTesting(source.wstring());
    pulse::WriteUtf8FileAtomic((source / L"app.json").wstring(), L"{}");
    int failed = 0;
    auto check = [&](bool ok, const char* name) {
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
        if (!ok) ++failed;
    };
    std::wstring error;
    check(Schedule(Kind::Configuration, target.wstring(), error), "schedule isolated identity fixture");
    bool attempted = false, replaced = false, moved_directory = false;
    SetConfigurationCopyHookForTesting([&](const std::wstring& output) {
        attempted = true;
        replaced = MoveFileExW(output.c_str(), (fixture / L"original-copy.json").c_str(), 0) != FALSE;
        if (replaced) pulse::WriteUtf8FileAtomic(output, L"replacement-owned-by-another-writer");
        if (!replaced) moved_directory = MoveFileExW(target.c_str(), (fixture / L"renamed-target").c_str(), 0) != FALSE;
        if (moved_directory) fs::create_directories(target);
        pulse::WriteUtf8FileAtomic((target / L"unrelated.txt").wstring(), L"keep");
        return false;
    });
    check(!ApplyConfiguration(error) && attempted, "abort migration after first verified copy");
    SetConfigurationCopyHookForTesting({});
    std::wstring content;
    check(!replaced || (pulse::ReadUtf8File((target / L"app.json").wstring(), content) &&
        content == L"replacement-owned-by-another-writer"), "rollback never deletes replacement object");
    check(!replaced && !moved_directory, "destination file and directory retain leases until rollback");
    check(ConfigurationRoot() == source.wstring() && !Pending(Kind::Configuration).empty() &&
        !LastError(Kind::Configuration).empty(), "failure preserves active root and reports pending failure");
    check(fs::exists(target / L"unrelated.txt"), "rollback preserves unrelated writer file");
    std::printf("replacement=%d directory_rename=%d; fixture retained\n", replaced, moved_directory);
    return failed ? 1 : 0;
}

int ConfigurationMetadataTest() {
    using namespace pulse::storage;
    const auto fixture = fs::absolute(fs::path(L"bench_data") /
        (L"configuration-metadata-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64())));
    const auto source = fixture / L"ads-source", target = fixture / L"ads-target";
    fs::create_directories(source);
    OverrideDefaultRootForTesting(source.wstring());
    int failed = 0;
    auto check = [&](bool ok, const char* name) {
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
        if (!ok) ++failed;
    };
    const auto app = (source / L"app.json").wstring();
    const std::vector<unsigned char> metadata{'m', 'e', 't', 'a', 0, 0xFF};
    check(pulse::WriteUtf8FileAtomic(app, L"{}") && WriteBytes(app + L":metadata", metadata),
        "seed a real named stream in an isolated configuration");
    const auto old_copy = (fixture / L"copyfile-reference.json").wstring();
    check(CopyFileW(app.c_str(), old_copy.c_str(), TRUE) && ReadBytes(old_copy + L":metadata") == metadata,
        "CopyFileW baseline preserves the named stream");
    const auto plain_copy = (fixture / L"default-stream-only.json").wstring();
    check(WriteBytes(plain_copy, ReadBytes(app)) && ReadBytes(plain_copy + L":metadata").empty(),
        "default-stream-only copying demonstrates metadata loss");
    std::wstring error;
    check(Schedule(Kind::Configuration, target.wstring(), error), "schedule isolated named-stream configuration");
    const bool migrated = ApplyConfiguration(error);
    check(!migrated && ConfigurationRoot() == source.wstring() && Pending(Kind::Configuration) == target.wstring() &&
        !error.empty() && !fs::exists(target / L"app.json") && ReadBytes(app + L":metadata") == metadata,
        "unsupported named stream refuses migration without switching root or losing the stream");

    const auto wallpaper_source = fixture / L"wallpaper-source", wallpaper_target = fixture / L"wallpaper-target";
    fs::create_directories(wallpaper_source);
    OverrideDefaultRootForTesting(wallpaper_source.wstring());
    const auto wallpaper = (wallpaper_source / L"wallpaper.png").wstring();
    check(pulse::WriteUtf8FileAtomic((wallpaper_source / L"app.json").wstring(), L"{}") &&
        WriteBytes(wallpaper, {'i', 'm', 'a', 'g', 'e'}) && WriteBytes(wallpaper + L":metadata", metadata),
        "seed a wallpaper named stream after an ordinary configuration");
    check(Schedule(Kind::Configuration, wallpaper_target.wstring(), error) && !ApplyConfiguration(error) &&
        ConfigurationRoot() == wallpaper_source.wstring() && !fs::exists(wallpaper_target / L"app.json") &&
        !fs::exists(wallpaper_target / L"wallpaper.png") && ReadBytes(wallpaper + L":metadata") == metadata,
        "late wallpaper metadata refusal rolls back earlier owned copies and preserves original streams");

    const auto basic_source = fixture / L"basic-source", basic_target = fixture / L"basic-target";
    fs::create_directories(basic_source);
    OverrideDefaultRootForTesting(basic_source.wstring());
    const auto basic_app = (basic_source / L"app.json").wstring();
    check(pulse::WriteUtf8FileAtomic(basic_app, L"{}"), "seed ordinary configuration metadata fixture");
    HANDLE file = CreateFileW(basic_app.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    FILE_BASIC_INFO basic{};
    basic.CreationTime.QuadPart = 132223104000000000LL;
    basic.LastWriteTime.QuadPart = 132224104000000000LL;
    basic.FileAttributes = FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_ARCHIVE | FILE_ATTRIBUTE_READONLY;
    check(file != INVALID_HANDLE_VALUE && SetFileInformationByHandle(file, FileBasicInfo, &basic, sizeof(basic)),
        "set ordinary attributes and timestamps through the owned fixture handle");
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    check(Schedule(Kind::Configuration, basic_target.wstring(), error) && ApplyConfiguration(error),
        "ordinary file metadata remains migratable");
    const auto copied_app = (basic_target / L"app.json").wstring();
    file = CreateFileW(copied_app.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    FILE_BASIC_INFO copied_basic{};
    check(file != INVALID_HANDLE_VALUE && GetFileInformationByHandleEx(file, FileBasicInfo, &copied_basic, sizeof(copied_basic)) &&
        copied_basic.CreationTime.QuadPart == basic.CreationTime.QuadPart &&
        copied_basic.LastWriteTime.QuadPart == basic.LastWriteTime.QuadPart &&
        (copied_basic.FileAttributes & basic.FileAttributes) == basic.FileAttributes,
        "migration preserves creation time last-write time and ordinary file attributes");
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);

    const auto rollback = fixture / L"readonly-rollback";
    check(Schedule(Kind::Configuration, rollback.wstring(), error), "schedule readonly metadata rollback fixture");
    HANDLE locator = INVALID_HANDLE_VALUE;
    SetConfigurationCopyHookForTesting([&](const std::wstring&) {
        locator = CreateFileW((basic_source / L"storage-locations.json").c_str(), GENERIC_READ, 0, nullptr,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        return locator != INVALID_HANDLE_VALUE;
    });
    const bool rollback_result = ApplyConfiguration(error);
    SetConfigurationCopyHookForTesting({});
    if (locator != INVALID_HANDLE_VALUE) CloseHandle(locator);
    check(!rollback_result && locator != INVALID_HANDLE_VALUE && ConfigurationRoot() == basic_target.wstring() &&
        !fs::exists(rollback / L"app.json"), "failed locator commit removes only the owned readonly copy");
    const auto special_source = fixture / L"sparse-source", special_target = fixture / L"sparse-target";
    fs::create_directories(special_source);
    OverrideDefaultRootForTesting(special_source.wstring());
    const auto special_app = (special_source / L"app.json").wstring();
    check(pulse::WriteUtf8FileAtomic(special_app, L"{}"), "seed sparse configuration fixture");
    file = CreateFileW(special_app.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    DWORD returned = 0;
    const bool sparse = file != INVALID_HANDLE_VALUE && DeviceIoControl(file, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &returned, nullptr);
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    check(sparse && Schedule(Kind::Configuration, special_target.wstring(), error) && !ApplyConfiguration(error) &&
        ConfigurationRoot() == special_source.wstring() && !fs::exists(special_target / L"app.json"),
        "unsupported sparse source refuses migration without switching root");

    const auto encrypted_source = fixture / L"efs-source", encrypted_target = fixture / L"efs-target";
    fs::create_directories(encrypted_source);
    OverrideDefaultRootForTesting(encrypted_source.wstring());
    const auto encrypted_app = (encrypted_source / L"app.json").wstring();
    check(pulse::WriteUtf8FileAtomic(encrypted_app, L"{}"), "seed encrypted configuration fixture");
    if (EncryptFileW(encrypted_app.c_str())) {
        check(Schedule(Kind::Configuration, encrypted_target.wstring(), error) && !ApplyConfiguration(error) &&
            ConfigurationRoot() == encrypted_source.wstring() && !fs::exists(encrypted_target / L"app.json") &&
            (GetFileAttributesW(encrypted_app.c_str()) & FILE_ATTRIBUTE_ENCRYPTED),
            "unsupported EFS source is never published as plaintext");
    } else {
        std::printf("[SKIP] EFS fixture unavailable; Win32 error=%lu\n", GetLastError());
    }
    std::printf("fixture retained; failures=%d\n", failed);
    return failed ? 1 : 0;
}

int wmain(int argc, wchar_t** argv) {
    if (argc == 2 && wcscmp(argv[1], L"--configuration-metadata") == 0) return ConfigurationMetadataTest();
    if (argc == 2 && wcscmp(argv[1], L"--configuration-identity") == 0) return ConfigurationIdentityTest();
    wchar_t temp[MAX_PATH]{}; GetTempPathW(ARRAYSIZE(temp), temp);
    const fs::path fixture = fs::path(temp) / (L"pulse-storage-test-" + std::to_wstring(GetCurrentProcessId()));
    const auto original = (fixture / L"original").wstring();
    const auto migrated = (fixture / L"配置目录").wstring();
    const auto locator_key = L"Software\\Pulse\\StorageTests\\" + std::to_wstring(GetCurrentProcessId());
    RegDeleteTreeW(HKEY_CURRENT_USER, locator_key.c_str());
    fs::create_directories(original);
    pulse::storage::OverrideDefaultRootForTesting(original, locator_key);
    int failed = 0;
    std::wstring error;
    auto check = [&](bool ok, const char* name) {
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
        if (!ok) { ++failed; std::fwprintf(stderr, L"  storage error: %ls\n", error.c_str()); }
    };
    using namespace pulse::storage;
    std::wstring image; pulse::json::Escape(original + L"\\wallpaper.png", image);
    pulse::WriteUtf8FileAtomic(original + L"\\app.json", L"{\"background_image\":\"" + image + L"\",\"theme_mode\":2}");
    pulse::WriteUtf8FileAtomic(original + L"\\session.json", L"{\"version\":7}");
    pulse::WriteUtf8FileAtomic(original + L"\\preferences.json", L"{\"version\":8,\"sidebarHidden\":2}");
    pulse::WriteUtf8FileAtomic(original + L"\\network-index.json", L"{\"test\":true}");
    pulse::WriteUtf8FileAtomic(original + L"\\wallpaper.png", L"image fixture");
    pulse::WriteUtf8FileAtomic(original + L"\\pulse-index.bin", L"must remain original");
    pulse::WriteUtf8FileAtomic(original + L"\\unrelated.txt", L"never copy");
    fs::create_directories(original + L"\\ContentIndex");
    const std::vector<unsigned char> content_config{0, 0xFF, 0x80, 42, 0};
    check(WriteBytes(original + L"\\ContentIndex\\content-v1.sqlite.config", content_config), "create binary legacy content configuration");
    pulse::WriteUtf8FileAtomic(original + L"\\ContentIndex\\content-v1.sqlite", L"exclude database");
    pulse::WriteUtf8FileAtomic(original + L"\\ContentIndex\\content-v1.sqlite.status", L"exclude status");
    check(ConfigurationRoot() == original && UserIndexRoot() == original, "legacy roots stay unchanged");
    check(!Schedule(Kind::Configuration, original + L"\\nested", error), "reject source overlap");
    check(!Schedule(Kind::Configuration, L"relative", error), "reject relative path");
    check(Schedule(Kind::Configuration, migrated, error), "schedule empty Unicode destination");
    check(ConfigurationRoot() == original && Pending(Kind::Configuration) == migrated, "schedule deferred until startup");
    check(!Schedule(Kind::Index, migrated, error), "reject shared pending destination");
    check(ApplyConfiguration(error), "apply migration");
    std::wstring app; pulse::ReadUtf8File(migrated + L"\\app.json", app);
    check(ConfigurationRoot() == migrated && Pending(Kind::Configuration).empty(), "commit root and clear pending");
    check(pulse::json::ExtractString(app, L"background_image") == migrated + L"\\wallpaper.png", "rewrite managed wallpaper path");
    check(fs::exists(migrated + L"\\network-index.json") && fs::exists(original + L"\\app.json"), "copy network config and retain originals");
    std::wstring migrated_preferences;
    check(pulse::ReadUtf8File(migrated + L"\\preferences.json", migrated_preferences) &&
        migrated_preferences == L"{\"version\":8,\"sidebarHidden\":2}",
        "configuration migration retains the separate session preferences");
    check(!fs::exists(migrated + L"\\pulse-index.bin") && !fs::exists(migrated + L"\\unrelated.txt"), "exclude index and unrelated files");
    check(ReadBytes(migrated + L"\\content-index.config") == content_config &&
          ReadBytes(original + L"\\ContentIndex\\content-v1.sqlite.config") == content_config &&
          !fs::exists(migrated + L"\\ContentIndex"), "migrate binary legacy content sidecar and preserve source without database or status");
    fs::create_directories(migrated + L"\\ContentIndex");
    pulse::WriteUtf8FileAtomic(migrated + L"\\ContentIndex\\content-v1.sqlite.config", L"obsolete legacy configuration");
    const auto occupied = (fixture / L"occupied").wstring(); fs::create_directories(occupied);
    pulse::WriteUtf8FileAtomic(occupied + L"\\sentinel", L"keep");
    check(!Schedule(Kind::Configuration, occupied, error), "reject occupied destination");
    const auto cancelled = (fixture / L"cancelled").wstring();
    check(Schedule(Kind::Configuration, cancelled, error) && Schedule(Kind::Configuration, migrated, error) && Pending(Kind::Configuration).empty(), "selecting active root cancels pending");
    const auto blocked = (fixture / L"blocked").wstring();
    check(Schedule(Kind::Configuration, blocked, error), "schedule rollback fixture");
    pulse::WriteUtf8FileAtomic(blocked + L"\\sentinel", L"keep");
    check(!ApplyConfiguration(error) && ConfigurationRoot() == migrated && fs::exists(blocked + L"\\sentinel") && !LastError(Kind::Configuration).empty(), "startup revalidation preserves active root and unrelated files");
    ClearPending(Kind::Configuration, error);
    const auto rollback = (fixture / L"rollback").wstring();
    check(Schedule(Kind::Configuration, rollback, error), "schedule partial-copy rollback fixture");
    HANDLE locked = CreateFileW((migrated + L"\\session.json").c_str(), GENERIC_READ, 0, nullptr,
                                 OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    check(locked != INVALID_HANDLE_VALUE && !ApplyConfiguration(error) && ConfigurationRoot() == migrated &&
          !fs::exists(rollback + L"\\app.json") && Pending(Kind::Configuration) == rollback,
          "locked source rolls back copied files and preserves pending and active root");
    if (locked != INVALID_HANDLE_VALUE) CloseHandle(locked);
    check(ApplyConfiguration(error) && ConfigurationRoot() == rollback, "retry succeeds after source unlock");
    check(ReadBytes(rollback + L"\\content-index.config") == content_config, "modern content sidecar takes precedence over legacy");
    const auto final_configuration = ConfigurationRoot();
    const auto index = (fixture / L"index").wstring();
    check(Schedule(Kind::Index, index, error) && ClearPending(Kind::Index, error) && !Commit(Kind::Index, index, error), "reject stale migration commit");
    check(Schedule(Kind::Index, index, error) && Commit(Kind::Index, index, error) && UserIndexRoot() == index, "commit independent user index root");
    OverrideDefaultRootForTesting(original, locator_key);
    check(ConfigurationRoot() == final_configuration && UserIndexRoot() == index, "locator survives fresh process cache");
    const auto pending = (fixture / L"pending-after-removal").wstring();
    check(Schedule(Kind::Configuration, pending, error), "schedule before deleting former AppData directory");
    fs::remove_all(original);
    OverrideDefaultRootForTesting(original, locator_key);
    check(ConfigurationRoot() == final_configuration && UserIndexRoot() == index &&
          Pending(Kind::Configuration) == pending,
          "custom roots and pending migration survive deletion of former AppData directory");
    check(ApplyConfiguration(error) && ConfigurationRoot() == pending && UserIndexRoot() == index,
          "configuration migration remains functional after former directory deletion");
    RegDeleteTreeW(HKEY_CURRENT_USER, locator_key.c_str());
    fs::create_directories(original);
    std::wstring legacy_root; pulse::json::Escape(final_configuration, legacy_root);
    check(pulse::WriteUtf8FileAtomic(original + L"\\storage-locations.json",
          L"{\"configuration\":\"" + legacy_root + L"\"}"), "create legacy locator fixture");
    OverrideDefaultRootForTesting(original, locator_key);
    check(ConfigurationRoot() == final_configuration, "import legacy file locator into registry");
    fs::remove_all(original);
    OverrideDefaultRootForTesting(original, locator_key);
    check(ConfigurationRoot() == final_configuration, "imported locator survives former AppData deletion");
    OverrideDefaultRootForTesting((fixture / L"isolated-end").wstring());
    RegDeleteTreeW(HKEY_CURRENT_USER, locator_key.c_str());
    std::error_code ec; fs::remove_all(fixture, ec);
    return failed ? 1 : 0;
}
