#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace pulse::index {
struct MigratedIndexFile {
    std::wstring relative;
    uint64_t size = 0;
    uint64_t hash = 0;
    bool copied = false;
};
struct IndexMigration {
    std::wstring source;
    std::wstring target;
    std::vector<MigratedIndexFile> files;
    uint32_t failure = 0;
};
// Only recognized index artifacts are copied/deleted; config and user files stay put.
std::wstring ResolveIndexMigrationTarget(const std::wstring& target);
bool SameIndexLocation(const std::wstring& source, const std::wstring& target);
// Validate without side effects before preparing privileged service storage.
// Source protection is file-only; unrelated source files and directory ACLs stay put.
bool PrepareServiceIndexMigration(const std::wstring& source, const std::wstring& target,
    const std::function<bool(const std::wstring&)>& protect_file,
    const std::function<bool(const std::wstring&)>& protect_tree,
    IndexMigration& preparation, std::wstring& error);
bool CopyIndexForMigration(const std::wstring& source, const std::wstring& target,
                           IndexMigration& migration, std::wstring& error);
bool RemoveMigratedIndexSource(const IndexMigration& migration, std::wstring& error);
void DiscardIndexMigrationCopies(const IndexMigration& migration);
}
