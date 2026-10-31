// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

// Isolated requested-heap/algorithm comparison; no service, pipes or user files.
#include "../index/change_feed_history.h"
#include "../index/index_memory_probe.h"
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <string_view>

namespace {
std::atomic<size_t> live_bytes{0}, peak_bytes{0}, allocations{0};
struct alignas(std::max_align_t) Allocation { size_t bytes; };
void* Allocate(size_t bytes) {
    if (bytes > SIZE_MAX - sizeof(Allocation)) throw std::bad_alloc();
    auto* h = static_cast<Allocation*>(std::malloc(sizeof(Allocation) + bytes));
    if (!h) throw std::bad_alloc();
    h->bytes = bytes;
    const size_t live = live_bytes.fetch_add(bytes) + bytes;
    auto peak = peak_bytes.load();
    while (peak < live && !peak_bytes.compare_exchange_weak(peak, live)) {}
    ++allocations;
    return h + 1;
}
void Release(void* p) noexcept {
    if (!p) return;
    auto* h = static_cast<Allocation*>(p) - 1;
    live_bytes.fetch_sub(h->bytes); std::free(h);
}
}
void* operator new(size_t n) { return Allocate(n); }
void* operator new[](size_t n) { return Allocate(n); }
void operator delete(void* p) noexcept { Release(p); }
void operator delete[](void* p) noexcept { Release(p); }
void operator delete(void* p, size_t) noexcept { Release(p); }
void operator delete[](void* p, size_t) noexcept { Release(p); }

using namespace pulse::index;
namespace {
// Original Engine::RecordFeed and ReadFeed storage/scan, kept as an independent reference.
struct LegacyFeed {
    explicit LegacyFeed(size_t n = 100000) : limit(n) {}
    void Append(ChangeRecord record, uint64_t id) {
        record.id = id; records.push_back(std::move(record));
        if (records.size() > limit) records.pop_front();
    }
    void ReadAfter(uint64_t cursor, size_t limit_out, std::vector<ChangeRecord>& out) const {
        if (!limit_out) return;
        for (const auto& record : records) {
            if (record.id <= cursor) continue;
            out.push_back(record);
            if (!--limit_out) break;
        }
    }
    void Clear() { records.clear(); }
    size_t Size() const { return records.size(); }
    uint64_t FirstId() const { return records.empty() ? 0 : records.front().id; }
    size_t limit;
    std::deque<ChangeRecord> records;
};
bool Equal(const ChangeRecord& a, const ChangeRecord& b) {
    return a.id == b.id && a.time == b.time && a.file_id == b.file_id && a.kind == b.kind &&
        a.is_dir == b.is_dir && a.source == b.source && a.path == b.path && a.old_path == b.old_path;
}
bool SamePage(const ChangeFeedHistory& a, const LegacyFeed& b, uint64_t cursor, size_t limit) {
    std::vector<ChangeRecord> x, y;
    a.ReadAfter(cursor, limit, x); b.ReadAfter(cursor, limit, y);
    return x.size() == y.size() && std::equal(x.begin(), x.end(), y.begin(), Equal);
}
ChangeRecord Fixture(uint64_t i) {
    ChangeRecord r;
    r.id = i; r.file_id = 0xABCD00000000ull + i; r.time = 1800000000 + i;
    r.kind = static_cast<ChangeKind>(i % 6); r.is_dir = i % 11 == 0;
    r.source = i % 7 ? ChangeSource::Event : ChangeSource::InitialMtime;
    r.path = L"C:\\feed-fixture\\" + std::wstring(100, L'x') + L"\\目录\\file-" + std::to_wstring(i) + L".txt";
    if (i % 5 == 0) r.old_path = L"\\\\server\\share\\旧目录\\" + std::wstring(60, L'y') + std::to_wstring(i);
    return r;
}
template<class Feed> void Benchmark(const char* label, bool short_paths) {
    std::vector<ChangeRecord> input;
    input.reserve(200000);
    for (uint64_t i = 1; i <= 200000; ++i) {
        auto r = Fixture(i);
        if (short_paths) { r.path = L"C:\\x"; r.old_path.clear(); }
        input.push_back(std::move(r));
    }
    const size_t base = live_bytes.load(), begin_allocs = allocations.load();
    peak_bytes = base;
    const auto begin = std::chrono::steady_clock::now();
    Feed feed;
    for (size_t i = 0; i < 100000; ++i) feed.Append(input[i], input[i].id);
    const double append_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now()-begin).count();
    const size_t retained = live_bytes.load()-base, peak = peak_bytes.load()-base;
    const auto append_allocs = allocations.load()-begin_allocs;
    const auto read_begin = std::chrono::steady_clock::now();
    size_t checksum = 0;
    for (int i = 0; i < 2000; ++i) {
        std::vector<ChangeRecord> page;
        feed.ReadAfter(100000, 512, page);
        checksum += page.size();
    }
    const double caught_up_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now()-read_begin).count();
    const auto page_begin = std::chrono::steady_clock::now();
    for (int i = 0; i < 200; ++i) {
        std::vector<ChangeRecord> page;
        feed.ReadAfter(99488, 512, page);
        checksum += page.size();
    }
    const double page_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now()-page_begin).count();
    const auto rollover_begin = std::chrono::steady_clock::now();
    for (size_t i = 100000; i < input.size(); ++i) feed.Append(input[i], input[i].id);
    const double rollover_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now()-rollover_begin).count();
    const size_t rollover_retained = live_bytes.load()-base;
    feed.Clear();
    std::printf("%s short=%d retained=%zu peak=%zu allocs=%zu append_ms=%.3f caught_up_2000_ms=%.3f page_200_ms=%.3f rollover_ms=%.3f rollover_retained=%zu cleared=%zu checksum=%zu\n",
        label, short_paths ? 1 : 0, retained, peak, append_allocs, append_ms, caught_up_ms, page_ms,
        rollover_ms, rollover_retained, live_bytes.load()-base, checksum);
}
}
int main(int argc, char** argv) {
    if (argc > 1 && std::string_view(argv[1]) == "--probe") {
        if (!SetEnvironmentVariableW(L"PULSE_INDEX_DIAGNOSTICS", L"1")) return 2;
        IndexMemoryProbe probe;
        const auto count = allocations.load();
        const auto begin = std::chrono::steady_clock::now();
        for (unsigned i = 0; i < 5000; ++i) probe.Capture(IndexMemoryPoint::TimingFlush);
        const auto extra = allocations.load() - count;
        const auto& point = probe.At(IndexMemoryPoint::TimingFlush);
        const bool ok = !extra && point.calls == 5000 && !point.failures && point.private_commit_bytes > 0;
        std::printf("[%s] 5000 memory samples allocations=%zu elapsed_ms=%.3f\n", ok ? "PASS" : "FAIL", extra,
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now()-begin).count());
        const auto json = probe.Json();
        for (const auto c : json) { std::putchar(c); if (c == '{' || c == '}' || c == ',') std::putchar('\n'); }
        std::putchar('\n');
        return ok ? 0 : 1;
    }
    if (argc > 1 && std::string_view(argv[1]) == "--legacy") { Benchmark<LegacyFeed>("legacy", argc > 2); return 0; }
    if (argc > 1 && std::string_view(argv[1]) == "--packed") { Benchmark<ChangeFeedHistory>("packed", argc > 2); return 0; }
    bool ok = true;
    auto check = [&](bool valid, const char* label) { std::printf("[%s] %s\n", valid ? "PASS" : "FAIL", label); ok &= valid; };
    for (size_t limit : {size_t(0), size_t(1), size_t(61), size_t(1000)}) {
        ChangeFeedHistory packed(limit); LegacyFeed legacy(limit);
        bool same = SamePage(packed, legacy, 0, 512);
        for (uint64_t i = 1; i <= 6000; ++i) {
            auto r = Fixture(i);
            if (i % 997 == 0) { r.path.assign(32767, L'长'); r.old_path.assign(32767, L'旧'); }
            if (i % 101 == 0) { r.path = L""; r.old_path = std::wstring(L"x\0y", 3); }
            packed.Append(r, i * 3); legacy.Append(r, i * 3);
            same &= packed.Size() == legacy.Size() && packed.FirstId() == legacy.FirstId();
            if (i % 37 == 0) {
                for (size_t page : {size_t(0), size_t(1), size_t(17), size_t(512)})
                    for (uint64_t cursor : {uint64_t(0), i * 3 - 1, i * 3, UINT64_MAX, (i > 100 ? i - 100 : 0) * 3})
                        same &= SamePage(packed, legacy, cursor, page);
            }
        }
        check(same, "paged feed equals deque through eviction, long UTF-16, empty and embedded-NUL paths");
        packed.Clear(); legacy.Clear();
        check(packed.Empty() && SamePage(packed, legacy, 0, 512), "clear drops all events");
        auto r = Fixture(90000); packed.Append(r, r.id); legacy.Append(r, r.id);
        check(SamePage(packed, legacy, 0, 512), "append after clear preserves IDs and metadata");
    }
    {
        ChangeFeedHistory packed; LegacyFeed legacy;
        for (uint64_t i = 1; i <= 200500; ++i) { auto r = Fixture(i); packed.Append(r, i); legacy.Append(r, i); }
        check(packed.Size() == 100000 && packed.FirstId() == 100501 &&
            SamePage(packed, legacy, 100500, 512) && SamePage(packed, legacy, 200490, 512),
            "default retention remains exactly 100000 events across repeated page recycling");
    }
    return ok ? 0 : 1;
}
