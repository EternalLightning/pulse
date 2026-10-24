#pragma once

#include "index_config.h"
#include <algorithm>
#include <mutex>
#include <windows.h>
#include <winioctl.h>

namespace pulse::index {

struct PersistentDeviceFacts {
    UINT drive_type = DRIVE_UNKNOWN;
    bool online = false;
    bool bus_known = false;
    bool removable_bus = false;
    bool system_volume = false;
};

inline bool AllowsPersistentIndex(const PersistentDeviceFacts& facts) {
    return facts.online && (facts.drive_type == DRIVE_FIXED || facts.drive_type == DRIVE_RAMDISK) &&
        !facts.removable_bus && (facts.bus_known || facts.system_volume || facts.drive_type == DRIVE_RAMDISK);
}

inline bool KnownRemovableDevice(const PersistentDeviceFacts& facts) {
    return facts.drive_type == DRIVE_REMOVABLE || facts.drive_type == DRIVE_CDROM || facts.removable_bus;
}

inline std::wstring LocalDevicePath(std::wstring path) {
    std::replace(path.begin(), path.end(), L'/', L'\\');
    if (path.rfind(L"\\\\?\\", 0) == 0) path.erase(0, 4);
    if (path.size() < 3 || path[1] != L':' || path[2] != L'\\' ||
        !((path[0] >= L'A' && path[0] <= L'Z') || (path[0] >= L'a' && path[0] <= L'z'))) return {};
    wchar_t absolute[32768]{};
    const DWORD n = GetFullPathNameW(path.c_str(), ARRAYSIZE(absolute), absolute, nullptr);
    if (!n || n >= ARRAYSIZE(absolute)) return {};
    path.assign(absolute, n);
    while (path.size() > 3 && path.back() == L'\\') path.pop_back();
    return path;
}

// Probe only from index/configuration workers. The UI consumes volume replies.
inline PersistentDeviceFacts ProbePersistentDevice(const std::wstring& input) {
    PersistentDeviceFacts facts;
    const auto path = LocalDevicePath(input);
    wchar_t root[32768]{};
    if (path.empty() || !GetVolumePathNameW(path.c_str(), root, ARRAYSIZE(root))) return facts;
    facts.drive_type = GetDriveTypeW(root);
    facts.online = GetVolumeInformationW(root, nullptr, 0, nullptr, nullptr, nullptr, nullptr, 0) != FALSE;
    if (!facts.online) return facts;
    wchar_t system[MAX_PATH]{}, system_root[32768]{};
    if (GetWindowsDirectoryW(system, ARRAYSIZE(system)) && GetVolumePathNameW(system, system_root, ARRAYSIZE(system_root)))
        facts.system_volume = CompareStringOrdinal(root, -1, system_root, -1, TRUE) == CSTR_EQUAL;
    wchar_t volume[MAX_PATH]{};
    if (!GetVolumeNameForVolumeMountPointW(root, volume, ARRAYSIZE(volume))) return facts;
    std::wstring device(volume);
    if (!device.empty() && device.back() == L'\\') device.pop_back();
    HANDLE handle = CreateFileW(device.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, 0, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return facts;
    STORAGE_PROPERTY_QUERY query{};
    query.PropertyId = StorageDeviceProperty;
    query.QueryType = PropertyStandardQuery;
    alignas(STORAGE_DEVICE_DESCRIPTOR) BYTE buffer[4096]{};
    DWORD returned = 0;
    if (DeviceIoControl(handle, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query), buffer, sizeof(buffer), &returned, nullptr) &&
        returned >= sizeof(STORAGE_DEVICE_DESCRIPTOR)) {
        const auto* descriptor = reinterpret_cast<const STORAGE_DEVICE_DESCRIPTOR*>(buffer);
        facts.bus_known = descriptor->BusType != BusTypeUnknown;
        facts.removable_bus = descriptor->RemovableMedia || descriptor->BusType == BusTypeUsb ||
            descriptor->BusType == BusTypeSd || descriptor->BusType == BusTypeMmc || descriptor->BusType == BusType1394;
    }
    CloseHandle(handle);
    return facts;
}

inline bool PersistentIndexPathAllowed(const std::wstring& path) {
    return AllowsPersistentIndex(ProbePersistentDevice(path));
}

inline PersistentDeviceFacts WorkerPersistentDeviceFacts(const std::wstring& path) {
    struct Entry { std::wstring path; PersistentDeviceFacts facts; ULONGLONG checked; };
    static std::mutex mutex;
    static std::vector<Entry> entries;
    std::lock_guard lock(mutex);
    const auto now = GetTickCount64();
    for (auto& entry : entries) {
        if (CompareStringOrdinal(entry.path.c_str(), -1, path.c_str(), -1, TRUE) != CSTR_EQUAL) continue;
        if (now - entry.checked >= 1000) { entry.facts = ProbePersistentDevice(path); entry.checked = now; }
        return entry.facts;
    }
    auto facts = ProbePersistentDevice(path);
    if (entries.size() >= 128) entries.erase(entries.begin());
    entries.push_back({path, facts, now});
    return facts;
}

template<class Config, class Probe>
inline void FilterPersistentRoots(Config& config, Probe probe) {
    std::erase_if(config.roots, [&](const auto& root) { return !AllowsPersistentIndex(probe(root.path)); });
}

template<class Config, class Probe>
inline bool PersistentRootsAllowed(const Config& config, Probe probe) {
    return std::all_of(config.roots.begin(), config.roots.end(),
        [&](const auto& root) { return AllowsPersistentIndex(probe(root.path)); });
}

template<class Config, class Probe>
inline void FilterKnownRemovableRoots(Config& config, Probe probe) {
    std::erase_if(config.roots, [&](const auto& root) { return KnownRemovableDevice(probe(root.path)); });
}

struct DeviceSearchCache {
    std::mutex mutex;
    std::vector<VolumeInfo> volumes;
};
inline DeviceSearchCache& SearchDeviceCache() { static DeviceSearchCache cache; return cache; }
inline void CacheSearchDevices(const std::vector<VolumeInfo>& volumes,
                               const std::vector<VolumeInfo>& aliases = {}) {
    auto& cache = SearchDeviceCache();
    std::lock_guard lock(cache.mutex);
    cache.volumes = volumes;
    cache.volumes.insert(cache.volumes.end(), aliases.begin(), aliases.end());
    for (auto& volume : cache.volumes) volume.mount_point = LocalDevicePath(volume.mount_point);
}
inline bool CachedTransientLocalSearch(const std::wstring& path) {
    const auto local = LocalDevicePath(path);
    if (local.empty()) return false;
    auto& cache = SearchDeviceCache();
    std::lock_guard lock(cache.mutex);
    const VolumeInfo* closest = nullptr;
    size_t longest = 0;
    for (const auto& volume : cache.volumes) {
        const auto& root = volume.mount_point;
        if (!root.empty() && root.size() > longest && local.size() >= root.size() &&
            CompareStringOrdinal(local.data(), static_cast<int>(root.size()), root.data(), static_cast<int>(root.size()), TRUE) == CSTR_EQUAL &&
            (root.back() == L'\\' || local.size() == root.size() || local[root.size()] == L'\\')) {
            closest = &volume;
            longest = root.size();
        }
    }
    return closest && closest->kind == VolumeKind::Removable && closest->online;
}

} // namespace pulse::index
