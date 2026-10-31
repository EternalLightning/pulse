// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once
#include "../common/json_utils.h"
#include "../common/user_storage.h"
#include "../common/utf8_file.h"
#include "../index/content_config_storage.h"
#include <filesystem>
#include <fstream>
#include <iterator>

namespace pulse::bench {
// The storage override is process-local; child agents must receive DatabasePath().
// Declare this before clients so the override outlives every worker and helper.
class ContentSearchFixture {
public:
    ~ContentSearchFixture() {
        if (storage_overridden_) storage::OverrideDefaultRootForTesting({}, {});
        if (profile_overridden_) SetEnvironmentVariableW(L"LOCALAPPDATA", had_profile_ ? previous_profile_.c_str() : nullptr);
    }
    ContentSearchFixture() = default;
    ContentSearchFixture(const ContentSearchFixture&) = delete;
    ContentSearchFixture& operator=(const ContentSearchFixture&) = delete;

    bool Initialize(const std::filesystem::path& parent, const std::wstring& name) {
        std::error_code error;
        stage_ = "absolute fixture parent";
        const auto absolute = std::filesystem::absolute(parent, error).lexically_normal();
        attempted_path_ = absolute;
        if (error) return false;
        stage_ = "exclusive fixture directory";
        std::filesystem::create_directories(absolute, error);
        if (error) { filesystem_error_ = error.value(); return false; }
        for (unsigned attempt = 0; attempt < 32; ++attempt) {
            const auto candidate = absolute / (name + L"-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(attempt));
            attempted_path_ = candidate;
            if (std::filesystem::create_directory(candidate, error)) { root_ = candidate; break; }
            if (error) { filesystem_error_ = error.value(); return false; }
        }
        if (root_.empty()) return false;
        stage_ = "private fixture directories";
        const auto profile = root_ / L"profile", storage_root = root_ / L"storage";
        const auto default_configuration = root_ / L"default-configuration";
        for (const auto& path : {profile, storage_root, default_configuration, root_ / L"content-db"}) {
            std::filesystem::create_directories(path, error);
            if (error) return false;
        }
        stage_ = "private LOCALAPPDATA";
        const DWORD size = GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0);
        had_profile_ = size != 0;
        if (had_profile_) {
            previous_profile_.resize(size);
            const DWORD copied = GetEnvironmentVariableW(L"LOCALAPPDATA", previous_profile_.data(), size);
            if (!copied || copied >= size) return false;
            previous_profile_.resize(copied);
        }
        if (!SetEnvironmentVariableW(L"LOCALAPPDATA", profile.c_str())) return false;
        profile_overridden_ = true;
        storage::OverrideDefaultRootForTesting(storage_root.wstring(), {});
        storage_overridden_ = true;

        stage_ = "private file locator";
        locator_path_ = storage_root / L"storage-locations.json";
        std::wstring locator = L"{\"configuration\":\"";
        json::Escape(default_configuration.wstring(), locator);
        locator += L"\",\"index\":\"\",\"pending_configuration\":\"\",\"pending_index\":\"\","
            L"\"configuration_error\":\"\",\"index_error\":\"\"}\n";
        if (!WriteUtf8FileAtomic(locator_path_.wstring(), locator)) return false;
        default_configuration_path_ = default_configuration / L"content-index.config";
        stage_ = "default configuration sentinel";
        index::ContentIndexConfig sentinel;
        sentinel.maximum_file_bytes = 1234567;
        ipc::PayloadWriter payload;
        index::content::PutConfig(payload, sentinel);
        if (!index::config_storage::WriteSidecar(default_configuration_path_.wstring(), 1, payload.data())) return false;
        stage_ = "sentinel snapshots";
        if (!ReadBytes(locator_path_, locator_sentinel_) || !ReadBytes(default_configuration_path_, configuration_sentinel_)) return false;

        database_path_ = (root_ / L"content-db" / L"fixture.sqlite").wstring();
        const auto default_database = index::config_storage::DatabasePath();
        index::ContentIndexConfig loaded;
        stage_ = "non-default explicit database";
        if (default_database.empty() || index::config_storage::SameStoredPath(database_path_, default_database)) return false;
        stage_ = "explicit configuration sidecar";
        if (index::config_storage::ConfigurationPath(database_path_) != database_path_ + L".config") return false;
        stage_ = "private locator configuration root";
        if (!index::config_storage::SameStoredPath(storage::ConfigurationRoot(), default_configuration.wstring())) return false;
        stage_ = "default configuration sentinel load";
        if (!index::LoadContentIndexConfig(default_database, loaded)) return false;
        stage_ = "default configuration sentinel value";
        if (!loaded.roots.empty() || loaded.maximum_file_bytes != sentinel.maximum_file_bytes) return false;
        stage_ = "ready";
        return true;
    }
    const char* Stage() const { return stage_; }
    const std::filesystem::path& AttemptedPath() const { return attempted_path_; }
    int FilesystemError() const { return filesystem_error_; }
    const std::filesystem::path& Root() const { return root_; }
    const std::wstring& DatabasePath() const { return database_path_; }
    bool DefaultConfigurationUnchanged() const {
        std::vector<uint8_t> bytes;
        return ReadBytes(default_configuration_path_, bytes) && bytes == configuration_sentinel_;
    }
    bool LocatorUnchanged() const {
        std::vector<uint8_t> bytes;
        return ReadBytes(locator_path_, bytes) && bytes == locator_sentinel_;
    }
    bool ConfigurationMatches(const index::ContentIndexConfig& expected) const {
        index::ContentIndexConfig actual;
        return index::LoadContentIndexConfig(database_path_, actual) && index::config_storage::SameConfig(actual, expected);
    }
private:
    static bool ReadBytes(const std::filesystem::path& path, std::vector<uint8_t>& bytes) {
        std::ifstream file(path, std::ios::binary);
        if (!file) return false;
        bytes.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
        return !file.bad();
    }
    std::filesystem::path root_, locator_path_, default_configuration_path_;
    std::wstring database_path_, previous_profile_;
    std::vector<uint8_t> locator_sentinel_, configuration_sentinel_;
    bool had_profile_ = false, profile_overridden_ = false, storage_overridden_ = false;
    const char* stage_ = "not initialized";
    std::filesystem::path attempted_path_;
    int filesystem_error_ = 0;
};
}
