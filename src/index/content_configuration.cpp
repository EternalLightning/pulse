#include "content_config_storage.h"
#include "content_scope.h"
#include "../../third_party/sqlite/sqlite3.h"

namespace pulse::index {
namespace {
DWORD StorageError(int result) {
    return result == SQLITE_BUSY || result == SQLITE_LOCKED ? ERROR_BUSY : ERROR_INVALID_DATA;
}
template<class Row>
bool ReadRows(sqlite3* db, const char* sql, Row row) {
    sqlite3_stmt* statement = nullptr;
    int result = sqlite3_prepare_v2(db, sql, -1, &statement, nullptr);
    bool ok = result == SQLITE_OK;
    while (ok && (result = sqlite3_step(statement)) == SQLITE_ROW) ok = row(statement);
    if (result != SQLITE_DONE) ok = false;
    const DWORD error = ok ? ERROR_SUCCESS : StorageError(result);
    sqlite3_finalize(statement);
    SetLastError(error);
    return ok;
}
bool ReadText(sqlite3_stmt* row, int column, std::wstring& value) {
    if (sqlite3_column_type(row, column) != SQLITE_TEXT) return false;
    const auto* text = static_cast<const wchar_t*>(sqlite3_column_text16(row, column));
    if (!text) return false;
    const int count = sqlite3_column_bytes16(row, column) / static_cast<int>(sizeof(wchar_t));
    value.assign(text, static_cast<size_t>(count));
    return value.find(L'\0') == std::wstring::npos;
}
bool NormalizeConfig(ContentIndexConfig& config) {
    if (config.roots.size() > 32 || config.excluded_directories.size() > 256 || config.excluded_paths.size() > 256 ||
        !config.maximum_file_bytes || config.maximum_file_bytes > 64ull * 1024 * 1024 ||
        !config.maximum_document_bytes || config.maximum_document_bytes > 512ull * 1024 * 1024 ||
        static_cast<uint32_t>(config.default_encoding) > 3) return false;
    for (auto& root : config.roots) {
        root.path = ContentScopeKey(std::move(root.path));
        if (root.path.size() < 3 || root.path.size() > 32768 || root.path[1] != L':' || root.path[2] != L'\\' ||
            root.path.find(L'\0') != std::wstring::npos || static_cast<uint32_t>(root.encoding) > 3) return false;
    }
    for (const auto& ex : config.excluded_directories) if (ex.empty() || ex.find_first_of(L"\\/:") != std::wstring::npos) return false;
    for (auto& ex : config.excluded_paths) {
        ex = ContentScopeKey(std::move(ex));
        if (ex.size() < 3 || ex.size() > 32768 || ex[1] != L':' || ex[2] != L'\\') return false;
    }
    return true;
}
}
bool LoadContentIndexConfig(const std::wstring& path, ContentIndexConfig& output) {
    if (path.empty()) { SetLastError(ERROR_PATH_NOT_FOUND); return false; }
    ContentIndexConfig config;
    if (config_storage::ReadConfigFile(path, config)) { output = std::move(config); return true; }
    const DWORD sidecar_error = GetLastError();
    if (sidecar_error != ERROR_FILE_NOT_FOUND && sidecar_error != ERROR_PATH_NOT_FOUND) return false;
    // Only an absent sidecar may import old storage. Corruption must never revive
    // obsolete roots or publish a partially read configuration.
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        const DWORD error = GetLastError();
        if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) return false;
        output = {}; SetLastError(ERROR_SUCCESS); return true;
    }
    const int n = WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (!n) return false;
    std::string file(static_cast<size_t>(n), '\0');
    if (!WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, file.data(), n, nullptr, nullptr)) return false;
    sqlite3* db = nullptr;
    int result = sqlite3_open_v2(file.c_str(), &db, SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX, nullptr);
    bool ok = result == SQLITE_OK;
    DWORD error = StorageError(result);
    if (ok) {
        sqlite3_busy_timeout(db, 250);
        result = sqlite3_exec(db, "BEGIN", nullptr, nullptr, nullptr);
        ok = result == SQLITE_OK;
        error = StorageError(result);
    }
    if (ok) {
        ok = ReadRows(db, "SELECT path,encoding FROM roots ORDER BY path", [&](sqlite3_stmt* row) {
            ContentIndexRoot root;
            if (!ReadText(row, 0, root.path) || sqlite3_column_type(row, 1) != SQLITE_INTEGER) return false;
            const auto encoding = sqlite3_column_int64(row, 1);
            if (encoding < 0 || encoding > 3 || config.roots.size() >= 32) return false;
            root.encoding = static_cast<text::Encoding>(encoding);
            config.roots.push_back(std::move(root)); return true;
        });
        error = GetLastError();
    }
    bool configured = false;
    if (ok) {
        ok = ReadRows(db, "SELECT name,value FROM settings", [&](sqlite3_stmt* row) {
            std::wstring name;
            if (!ReadText(row, 0, name)) return false;
            const bool known = name == L"maximum_file_bytes" || name == L"maximum_document_bytes" ||
                name == L"shared_scope" || name == L"default_encoding";
            if (!known) return true;
            if (sqlite3_column_type(row, 1) != SQLITE_INTEGER) return false;
            const auto value = sqlite3_column_int64(row, 1);
            if (name == L"maximum_file_bytes") {
                if (value <= 0 || value > 64ll * 1024 * 1024) return false;
                config.maximum_file_bytes = static_cast<uint64_t>(value); configured = true;
            } else if (name == L"maximum_document_bytes") {
                if (value <= 0 || value > 512ll * 1024 * 1024) return false;
                config.maximum_document_bytes = static_cast<uint64_t>(value);
            } else if (name == L"shared_scope") {
                if (value != 0 && value != 1) return false;
                config.shared_scope = value != 0;
            } else {
                if (value < 0 || value > 3) return false;
                config.default_encoding = static_cast<text::Encoding>(value);
            }
            return true;
        });
        error = GetLastError();
    }
    if (ok && configured) {
        config.excluded_directories.clear();
        ok = ReadRows(db, "SELECT value FROM exclusions", [&](sqlite3_stmt* row) {
            std::wstring value;
            if (!ReadText(row, 0, value)) return false;
            auto& exclusions = value.find_first_of(L"\\/:") == std::wstring::npos ? config.excluded_directories : config.excluded_paths;
            if (exclusions.size() >= 256) return false;
            exclusions.push_back(std::move(value)); return true;
        });
        error = GetLastError();
    }
    if (ok) { ok = NormalizeConfig(config); if (!ok) error = ERROR_INVALID_DATA; }
    if (db) { sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr); sqlite3_close(db); }
    SetLastError(ok ? ERROR_SUCCESS : error);
    if (ok) { FilterKnownRemovableRoots(config, ProbePersistentDevice); output = std::move(config); }
    return ok;
}
bool LoadContentIndexConfig(ContentIndexConfig& output) {
    return LoadContentIndexConfig(config_storage::DatabasePath(), output);
}
ContentIndexConfig LoadContentIndexConfig() {
    ContentIndexConfig config;
    LoadContentIndexConfig(config);
    return config;
}
bool SaveInstantContentConfig(ContentIndexConfig& config) {
    if (!NormalizeConfig(config)) return false;
    return config_storage::WriteConfigFile(config_storage::DatabasePath(), config);
}
}
