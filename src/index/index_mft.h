// index_mft.h — Sequential NTFS $MFT reader (plan.md 阶段 2.5 C).
// Names, parent FRN, directory flag, data size and last-write time from
// STANDARD_INFORMATION + FILE_NAME + unnamed $DATA. Used instead of
// FSCTL_ENUM_USN_DATA so size:/dm: work after an admin MFT build.
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <windows.h>
#include <winioctl.h>

namespace pulse::index {

struct MftFile {
    uint64_t frn = 0;
    uint64_t parent = 0;
    uint64_t size = 0;
    uint64_t mtime = 0; // FILETIME as u64
    std::wstring name;
    bool is_dir = false;
    uint8_t name_type = 0; // NTFS FILE_NAME.NameType
};

enum class MftReadStatus { Complete, Failed, Cancelled, Capped };

struct MftReadIo {
    // Optional IO overrides permit read-failure fixtures without a live volume.
    std::function<bool(NTFS_VOLUME_DATA_BUFFER&)> query;
    std::function<bool(uint64_t, void*, DWORD)> read;
};

// Walk $MFT on an already-opened volume handle (FILE_READ_DATA).
// `emit` returns false to stop. `progress` is invoked roughly every 50k records.
MftReadStatus EnumerateMft(HANDLE volume,
                  std::atomic<bool>* running,
                  const std::function<void(size_t)>& progress,
                  const std::function<bool(MftFile&&)>& emit,
                  const MftReadIo* io = nullptr);

} // namespace pulse::index
