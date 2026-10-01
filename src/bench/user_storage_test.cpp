#include "../common/user_storage.h"
#include "../common/utf8_file.h"
#include "../common/json_utils.h"
#include <filesystem>
#include <cstdio>
#include <windows.h>
#include <vector>

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
int wmain() {
    wchar_t temp[MAX_PATH]{}; GetTempPathW(ARRAYSIZE(temp), temp);
    const fs::path fixture = fs::path(temp) / (L"pulse-storage-test-" + std::to_wstring(GetCurrentProcessId()));
    const auto original = (fixture / L"original").wstring();
    const auto migrated = (fixture / L"配置目录").wstring();
    fs::create_directories(original);
    pulse::storage::OverrideDefaultRootForTesting(original);
    int failed = 0;
    auto check = [&](bool ok, const char* name) { std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name); if (!ok) ++failed; };
    using namespace pulse::storage;
    std::wstring error;
    std::wstring image; pulse::json::Escape(original + L"\\wallpaper.png", image);
    pulse::WriteUtf8FileAtomic(original + L"\\app.json", L"{\"background_image\":\"" + image + L"\",\"theme_mode\":2}");
    pulse::WriteUtf8FileAtomic(original + L"\\session.json", L"{\"version\":7}");
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
    OverrideDefaultRootForTesting(original);
    check(ConfigurationRoot() == final_configuration && UserIndexRoot() == index, "locator survives fresh process cache");
    OverrideDefaultRootForTesting((fixture / L"isolated-end").wstring());
    std::error_code ec; fs::remove_all(fixture, ec);
    return failed ? 1 : 0;
}
