// fs_net_cache.h — Persistent UNC directory snapshots + one-shot connectivity probe.
#pragma once
#include "fs_enum.h"
#include "fs_snapshot.h"
#include <cstdint>
#include <string>
#include <functional>
#include <windows.h>

namespace pulse::fs {

using UncProbeId = uint64_t;

struct UncProbeResult {
    UncProbeId probe_id = 0;
    std::wstring unc;
    NetStatus status = NetStatus::Unknown;
    DWORD rtt_ms = 0;
};

struct NetSnapshotWriteState;
class NetSnapshotWriteTicket {
    std::shared_ptr<NetSnapshotWriteState> state_;
    uint64_t sequence_ = 0;
    friend NetSnapshotWriteTicket ReserveNetSnapshotWrite(const std::wstring&);
    friend bool SaveNetSnapshot(const std::wstring&, const SnapshotPtr&,
        const std::function<bool()>&, const NetSnapshotWriteTicket&);
};
// Reserve when enumeration is requested, so a late old result cannot replace
// a newer committed snapshot. Reservation performs no filesystem work.
NetSnapshotWriteTicket ReserveNetSnapshotWrite(const std::wstring& path);

bool SaveNetSnapshot(const std::wstring& path, const SnapshotPtr& snapshot,
                     const std::function<bool()>& cancelled = {},
                     const NetSnapshotWriteTicket& ticket = {});
SnapshotPtr LoadNetSnapshot(const std::wstring& path, uint64_t* unix_sec = nullptr);
std::wstring FormatCacheAge(uint64_t unix_sec);

// Background probe: posts `msg` to `hwnd` with lParam = heap UncProbeResult*.
// The 1500ms timeout reports Offline; it does not cancel the blocking filesystem
// call. A late completion is posted separately with the same probe ID.
// Live and timed-out calls share four physical slots until their IO returns.
// False means no request was admitted; no result will be posted for that ID.
bool StartUncProbe(HWND hwnd, UINT msg, std::wstring unc, UncProbeId probe_id);

#ifdef PULSE_NET_CACHE_TESTING
struct UncProbeResourceUsage {
    uint32_t live = 0;
    uint32_t retired = 0;
    uint32_t limit = 0;
};
UncProbeResourceUsage UncProbeResourcesForTest();
void SetUncProbeHookForTest(std::function<bool(const std::wstring&)> hook, DWORD timeout_ms = 1500);
#endif

} // namespace pulse::fs
