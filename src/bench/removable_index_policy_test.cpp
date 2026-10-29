#include "../index/persistent_index_policy.h"
#include "../index/content_scope.h"
#include "../index/live_filename_search.h"
#include <filesystem>
#include <fstream>
#include <iostream>

using namespace pulse::index;
namespace {
int failures = 0, checks = 0;
void Check(bool ok, const char* name) {
    ++checks;
    std::cout << (ok ? "[PASS] " : "[FAIL] ") << name << '\n';
    if (!ok) ++failures;
}
}
int main() {
    const PersistentDeviceFacts internal{DRIVE_FIXED, true, true, false, false};
    const PersistentDeviceFacts removable{DRIVE_REMOVABLE, true, true, false, false};
    const PersistentDeviceFacts usb_fixed{DRIVE_FIXED, true, true, true, false};
    Check(AllowsPersistentIndex(internal), "internal SATA/NVMe/RAID/virtual non-removable bus allowed");
    Check(!AllowsPersistentIndex(removable), "removable NTFS and exFAT excluded independently of filesystem");
    Check(!AllowsPersistentIndex(usb_fixed), "USB fixed NTFS and exFAT excluded independently of filesystem");
    Check(!AllowsPersistentIndex({DRIVE_FIXED, false, true, false, false}), "offline fixed excluded");
    Check(!AllowsPersistentIndex({DRIVE_FIXED, true, false, false, false}), "unknown non-system fixed fails closed");
    Check(AllowsPersistentIndex({DRIVE_FIXED, true, false, false, true}), "online system volume tolerates unavailable bus query");
    Check(!AllowsPersistentIndex({DRIVE_FIXED, true, true, true, true}), "positive USB detection overrides system fallback");
    Check(!AllowsPersistentIndex({DRIVE_UNKNOWN, false, false, false, false}), "unknown and unreachable devices excluded");
    Check(!AllowsPersistentIndex({DRIVE_REMOTE, true, true, false, false}), "network root not a local persistent device");
    Check(!AllowsPersistentIndex({DRIVE_CDROM, true, true, false, false}), "optical device excluded");
    Check(AllowsPersistentIndex({DRIVE_RAMDISK, true, false, false, false}), "RAM disk retains local walk compatibility");
    Check(LocalDevicePath(L"::{phone}").empty() && LocalDevicePath(L"\\\\server\\share").empty(), "MTP and UNC rejected by local device parser");
    Check(LocalDevicePath(L"G:/folder/../child") == L"G:\\child", "local subdirectory normalized without disk access");
    ContentIndexConfig config;
    config.roots = {{L"C:\\data"}, {L"G:\\subdirectory"}, {L"C:\\mounted-usb\\child"}, {L"Z:\\offline"}};
    const auto probe = [&](const std::wstring& path) {
        return path == L"C:\\data" ? internal : path == L"G:\\subdirectory" ? removable :
            path == L"C:\\mounted-usb\\child" ? usb_fixed : PersistentDeviceFacts{};
    };
    Check(!PersistentRootsAllowed(config, probe), "explicit roots reject removable subdirectory/mounted USB/offline roots");
    FilterPersistentRoots(config, probe);
    Check(config.roots.size() == 1 && config.roots[0].path == L"C:\\data", "legacy configured roots filtered in memory");
    Check(PersistentRootsAllowed(config, probe), "internal configured root remains valid");
    ContentIndexConfig previous;
    previous.roots = {{L"C:\\data"}, {L"G:\\subdirectory"}, {L"C:\\mounted-usb\\child"}, {L"Z:\\offline"}};
    FilterKnownRemovableRoots(previous, probe);
    Check(previous.roots.size() == 2 && previous.roots[1].path == L"Z:\\offline", "old offline/unknown internal roots survive configuration load");
    Check(!PersistentRootsAllowed(previous, probe), "retained offline root remains ineligible for persistent work");
    FilterPersistentRoots(previous, probe);
    Check(previous.roots.size() == 1 && previous.roots[0].path == L"C:\\data", "execution snapshot pauses offline root while preserving original configuration");
    VolumeInfo volume;
    volume.mount_point = L"G:\\"; volume.kind = VolumeKind::Removable; volume.online = true;
    volume.supported = volume.enabled = true; volume.indexed_items = 999;
    IndexConfig legacy; legacy.include_removable_ntfs = true;
    Check(SharedContentScope({}, {volume}, {}).roots.empty() && legacy.include_removable_ntfs,
        "legacy removable true cannot reintroduce shared content root");
    CacheSearchDevices({volume});
    Check(CachedTransientLocalSearch(L"G:\\child"), "cached removable subdirectory uses temporary search");
    Check(!CachedTransientLocalSearch(L"C:\\child"), "internal path does not use removable fallback");
    VolumeInfo internal_mount;
    internal_mount.mount_point = L"C:\\"; internal_mount.kind = VolumeKind::Fixed; internal_mount.online = true;
    VolumeInfo usb_mount = volume;
    usb_mount.mount_point = L"C:\\mounted-usb\\";
    VolumeInfo nested_internal = internal_mount;
    nested_internal.mount_point = L"C:\\mounted-usb\\internal\\";
    CacheSearchDevices({internal_mount, volume}, {usb_mount, nested_internal});
    Check(CachedTransientLocalSearch(L"c:\\mounted-usb") && CachedTransientLocalSearch(L"C:\\mounted-usb\\child"),
        "directory mount alias and its root route to removable scan from cached background aliases");
    Check(!CachedTransientLocalSearch(L"C:\\mounted-usb-other\\child"), "mount alias matches directory boundaries");
    Check(!CachedTransientLocalSearch(L"C:\\mounted-usb\\internal\\child"), "nested internal mount overrides enclosing USB classification");
    Check(TransientFilenameSearchRoot(L"C:\\mounted-usb\\child") == L"C:\\mounted-usb\\child", "mounted USB alias selects temporary filename root");
    CacheSearchDevices({internal_mount, volume});
    Check(!CachedTransientLocalSearch(L"C:\\mounted-usb\\child"), "refresh discards removed directory mount alias");
    Check(TransientFilenameSearchRoot(L"G:\\child") == L"G:\\child", "temporary filename route accepts excluded local path");
    Check(FilenameSearchRoot(L"G:\\child").empty(), "UNC parser keeps local path rejection");
    Check(TransientFilenameSearchRoot(L"\\\\server\\share\\child") == L"\\\\server\\share\\child", "UNC fallback preserved");
    CacheSearchDevices({});
    Check(!CachedTransientLocalSearch(L"G:\\child"), "refreshed volume list forgets removed drive letter");
    const auto actual_mounts = EnumerateSearchDeviceMounts();
    Check(!actual_mounts.empty() && std::any_of(actual_mounts.begin(), actual_mounts.end(), [](const auto& device) {
        return CompareStringOrdinal(device.mount_point.c_str(), -1, L"C:\\", -1, TRUE) == CSTR_EQUAL;
    }), "background enumeration returns actual system-volume mount path");
    if (!actual_mounts.empty()) {
        auto known_device = actual_mounts.front();
        known_device.kind = VolumeKind::Removable; known_device.online = false;
        const auto aliases = EnumerateSearchDeviceMounts({known_device});
        Check(std::any_of(aliases.begin(), aliases.end(), [&](const auto& alias) {
            return alias.id == known_device.id;
        }) && std::all_of(aliases.begin(), aliases.end(), [&](const auto& alias) {
            return alias.id != known_device.id || (alias.kind == known_device.kind && alias.online == known_device.online);
        }), "known volume aliases reuse IPC classification and online state rather than probing again");
    }
    for (const auto* path : {L"C:\\", L"E:\\"}) {
        const auto facts = ProbePersistentDevice(path);
        std::wcout << L"[PROBE] " << path << L" type=" << facts.drive_type << L" online=" << facts.online
            << L" bus_known=" << facts.bus_known << L" removable_bus=" << facts.removable_bus
            << L" system=" << facts.system_volume << L" allowed=" << AllowsPersistentIndex(facts) << L'\n';
        Check(AllowsPersistentIndex(facts), "actual machine system/project disk remains eligible (read-only probe)");
    }
    const auto root = std::filesystem::absolute(std::filesystem::path(L"bench_data/issue-fixes-removable/fixture") /
        std::to_wstring(GetCurrentProcessId()));
    std::filesystem::create_directories(root / L"child");
    for (int i = 0; i < 340; ++i) {
        std::ofstream file(root / (L"sample" + std::to_wstring(i) + L".txt")); file << "temporary sample";
    }
    { std::ofstream file(root / L"child" / L"nested.txt"); file << "nested"; }
    volume.mount_point = root.wstring(); CacheSearchDevices({volume});
    Check(TransientFilenameSearchRoot(root.wstring()) == root.wstring(), "real private NTFS directory can inject removable cached policy");
    Query query; query.needle = L"ext:txt"; query.path_prefix = root.wstring(); query.limit = 5;
    std::atomic<bool> cancel{false}; SearchResult last; size_t publications = 0;
    ScanLiveFilenames(query, TransientFilenameSearchRoot(root.wstring()), cancel, [&](SearchResult result) { last = std::move(result); ++publications; });
    Check(!last.scanning && !last.incomplete && last.total == 341 && last.hits.size() == 5, "temporary recursive filename scan preserves filter and bounded page");
    Check(publications >= 2, "temporary scan publishes loading and completion states");
    ScanLiveFilenames(query, root.wstring() + L"\\", cancel, [&](SearchResult result) { last = std::move(result); });
    Check(last.total == 341 && !last.incomplete, "temporary scan handles trailing slash used by drive roots");
    query.offset = 340; ScanLiveFilenames(query, root.wstring(), cancel, [&](SearchResult result) { last = std::move(result); });
    Check(last.total == 341 && last.hits.size() == 1, "temporary scan preserves page offset");
    query.offset = 0; query.needle = L"nested";
    ScanLiveFilenames(query, root.wstring(), cancel, [&](SearchResult result) { last = std::move(result); });
    Check(last.total == 1 && last.hits[0].name == L"nested.txt", "temporary scanner searches child directories");
    cancel = true; publications = 0;
    ScanLiveFilenames(query, root.wstring(), cancel, [&](SearchResult) { ++publications; });
    Check(publications == 1, "cancelled temporary scan never publishes final results");
    cancel = false; ScanLiveFilenames(query, L"", cancel, [&](SearchResult result) { last = std::move(result); });
    Check(last.error == ERROR_PATH_NOT_FOUND && last.incomplete && !last.scanning, "expired cached root cannot scan process current directory");
    CacheSearchDevices({});
    std::cout << checks - failures << " PASS / " << failures << " FAIL\n";
    return failures ? 1 : 0;
}
