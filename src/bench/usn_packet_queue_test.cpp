// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

// Isolated queue and overlapped-I/O regressions; never opens a volume or Pulse service.
#include "../index/usn_stream.h"
#include "../index/index_memory_probe.h"
#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <cwchar>

namespace {
std::atomic<size_t> live_bytes{0};
std::atomic<int> fail_after{-1};
struct alignas(std::max_align_t) Allocation { size_t bytes; };
void* Allocate(size_t bytes) {
    const int remaining = fail_after.load();
    if (remaining >= 0 && fail_after.fetch_sub(1) == 0) throw std::bad_alloc();
    if (bytes > SIZE_MAX - sizeof(Allocation)) throw std::bad_alloc();
    auto* h = static_cast<Allocation*>(std::malloc(sizeof(Allocation) + bytes));
    if (!h) throw std::bad_alloc();
    h->bytes = bytes; live_bytes.fetch_add(bytes); return h + 1;
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

namespace pulse::index {
struct UsnStreamTestAccess {
    static std::unique_ptr<UsnStream> Make(HANDLE signal) {
        return std::unique_ptr<UsnStream>(new UsnStream(signal));
    }
    static bool Push(UsnStream& s, const std::vector<BYTE>& data) { return s.packets_.Push(data.data(), data.size()); }
    static void Fail(UsnStream& s, DWORD error) { s.Failed(error); }
};
}
using namespace pulse::index;
namespace {
int passes = 0, failures = 0;
void Check(bool ok, const char* name) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
    ok ? ++passes : ++failures;
}
std::vector<BYTE> Packet(size_t bytes, int64_t cursor, BYTE tag = 0x5a) {
    std::vector<BYTE> data(bytes, tag);
    if (bytes >= sizeof(cursor)) std::memcpy(data.data(), &cursor, sizeof(cursor));
    return data;
}
// Independent reference to the previous read-buffer/queue/Take behavior.
struct Legacy {
    std::deque<std::vector<BYTE>> packets;
    void Push(const std::vector<BYTE>& data) {
        std::vector<BYTE> packet(UsnPacketQueue::kReadBytes);
        std::memcpy(packet.data(), data.data(), data.size());
        packet.resize(data.size()); packets.push_back(std::move(packet));
    }
    void Take(std::vector<BYTE>& records, int64_t& cursor) {
        while (!packets.empty() && records.size() < UsnPacketQueue::kTakeBytes) {
            auto& packet = packets.front();
            std::memcpy(&cursor, packet.data(), sizeof(cursor));
            records.insert(records.end(), packet.begin() + sizeof(cursor), packet.end());
            packets.pop_front();
        }
    }
};
void TinyAndOwnership() {
    auto input = Packet(160, 1);
    UsnPacketQueue queue; Legacy legacy;
    const auto base = live_bytes.load();
    bool pushed = true;
    for (int64_t i = 1; i <= 257; ++i) {
        std::memcpy(input.data(), &i, sizeof(i)); pushed &= queue.Push(input.data(), input.size());
    }
    const auto state = queue.Memory();
    const auto heap = live_bytes.load() - base;
    for (int64_t i = 1; i <= 257; ++i) { std::memcpy(input.data(), &i, sizeof(i)); legacy.Push(input); }
    size_t old_capacity = 0;
    for (const auto& p : legacy.packets) old_capacity += p.capacity();
    Check(pushed && state.packets == 257 && state.payload_bytes == 41120 && state.capacity_bytes == 41120,
          "257 tiny packets retain exact payload capacity, not 64.25 MiB");
    Check(heap == state.charged_bytes && state.charged_bytes < 65536,
          "charged bytes include every ordinary C++ queue allocation");
    std::printf("METRIC tiny old_capacity=%zu new_capacity=%zu new_charged=%zu reusable_io=%zu\n",
        old_capacity, state.capacity_bytes, state.charged_bytes, UsnPacketQueue::kReadBytes);
    std::fill(input.begin(), input.end(), BYTE{0});
    std::vector<BYTE> a, b; int64_t ca = -1, cb = -1; bool more = true;
    const bool ok = queue.Take(a, ca, more); legacy.Take(b, cb);
    Check(old_capacity == 67371008 && ok && !more && a == b && ca == cb && ca == 257,
          "buffer overwrite cannot alter queued data; FIFO and cursor match old queue");
    const auto drained = queue.Memory();
    Check(!drained.packets && !drained.charged_bytes && !drained.capacity_bytes && !drained.payload_bytes &&
          drained.peak_charged_bytes == state.charged_bytes, "drain releases all packet storage but preserves high-water mark");
    input = Packet(8, 999);
    Check(queue.Push(input.data(), input.size()) && queue.Take(a, ca, more) && ca == 999,
          "reappend after drain and header-only cursor are preserved");
}
void Batching() {
    UsnPacketQueue queue; Legacy legacy;
    for (int64_t i = 1; i <= 6; ++i) {
        auto data = Packet(UsnPacketQueue::kReadBytes, i, static_cast<BYTE>(i));
        queue.Push(data.data(), data.size()); legacy.Push(data);
    }
    std::vector<BYTE> a, b; int64_t ca = 0, cb = 0; bool more = false;
    bool same = queue.Take(a, ca, more); legacy.Take(b, cb);
    Check(same && more && a == b && ca == 5 && ca == cb && a.size() == 5 * (UsnPacketQueue::kReadBytes - 8),
          "1 MiB boundary retains old whole-packet overshoot and remaining signal");
    a.clear(); b.clear(); same = queue.Take(a, ca, more); legacy.Take(b, cb);
    Check(same && !more && a == b && ca == 6 && ca == cb, "second batch resumes FIFO without missing bytes");
    a.assign(UsnPacketQueue::kTakeBytes, 0); auto p = Packet(16, 7); queue.Push(p.data(), p.size());
    Check(queue.Take(a, ca, more) && more && ca == 6 && queue.Memory().packets == 1,
          "already-full output consumes nothing and keeps cursor");
}
void ErrorsAndAllocations() {
    auto p = Packet(160, 42); size_t charge = 0;
    { UsnPacketQueue probe; probe.Push(p.data(), p.size()); charge = probe.Memory().charged_bytes; }
    UsnPacketQueue queue(2 * charge);
    bool ok = queue.Push(p.data(), p.size()) && queue.Push(p.data(), p.size());
    Check(ok && queue.Memory().charged_bytes == 2 * charge && !queue.Push(p.data(), p.size()) &&
          queue.Error() == ERROR_BUFFER_OVERFLOW && queue.Memory().overflows == 1,
          "actual charged budget accepts exact fit and rejects next packet");
    queue.Fail(ERROR_INVALID_DATA);
    std::vector<BYTE> records{1, 2, 3}; int64_t cursor = 99; bool more = true;
    Check(!queue.Take(records, cursor, more) && records == std::vector<BYTE>({1, 2, 3}) && cursor == 99 && !more &&
          !queue.Push(p.data(), p.size()) && queue.Memory().packets == 2 && queue.Memory().overflows == 1 &&
          queue.Error() == ERROR_BUFFER_OVERFLOW, "overflow stays sticky: no silent consume/cursor advance/error overwrite");
    bool invalid = true;
    for (const size_t bytes : {size_t{0}, size_t{7}, UsnPacketQueue::kReadBytes + 1}) {
        UsnPacketQueue bad; invalid &= !bad.Push(p.data(), bytes) && bad.Error() == ERROR_INVALID_DATA;
    }
    { UsnPacketQueue bad; invalid &= !bad.Push(nullptr, 8) && bad.Error() == ERROR_INVALID_DATA; }
    Check(invalid, "malformed lengths and null input rejected before memory access");
    bool oom = true;
    for (int position = 0; position != 2; ++position) {
        UsnPacketQueue q; const auto base = live_bytes.load(); bool threw = false;
        fail_after = position;
        try { q.Push(p.data(), p.size()); } catch (const std::bad_alloc&) { threw = true; }
        fail_after = -1;
        oom &= threw && live_bytes.load() == base && q.Memory().charged_bytes == 0 && q.Memory().packets == 0;
        oom &= q.Push(p.data(), p.size());
    }
    Check(oom, "node/buffer allocation failures do not leak, corrupt counters or link a partial packet");
    UsnPacketQueue q; q.Push(p.data(), p.size()); records.clear(); records.shrink_to_fit(); cursor = 99;
    bool threw = false; fail_after = 0;
    try { q.Take(records, cursor, more); } catch (const std::bad_alloc&) { threw = true; }
    fail_after = -1;
    Check(threw && cursor == 99 && q.Memory().packets == 1 && q.Take(records, cursor, more) && cursor == 42,
          "failed output allocation leaves front packet/cursor retryable");
    const auto base = live_bytes.load(); bool chain = true;
    { UsnPacketQueue long_queue; auto header = Packet(8, 1);
      for (size_t i = 0; i < 200000; ++i) chain &= long_queue.Push(header.data(), header.size()); }
    Check(chain && live_bytes.load() == base, "200000-node destruction is iterative and releases all allocations");
}
void Concurrent() {
    UsnPacketQueue q; std::atomic<bool> done{false}; std::atomic<bool> pushed{true};
    std::thread producer([&] {
        auto p = Packet(16, 0);
        for (int64_t i = 1; i <= 20000; ++i) {
            const int64_t cursor = i * 7;
            std::memcpy(p.data(), &cursor, 8); std::memcpy(p.data() + 8, &i, 8);
            if (!q.Push(p.data(), p.size())) { pushed = false; break; }
        }
        done = true;
    });
    int64_t expected = 1, cursor = 0; bool ok = true, more = false;
    std::vector<BYTE> records;
    while (!done || q.Memory().packets) {
        records.clear(); if (!q.Take(records, cursor, more)) { ok = false; break; }
        ok &= records.size() % 8 == 0;
        for (size_t offset = 0; offset + 8 <= records.size(); offset += 8) {
            int64_t value = 0; std::memcpy(&value, records.data() + offset, 8); ok &= value == expected++;
        }
        ok &= cursor == (expected - 1) * 7;
        if (records.empty()) std::this_thread::yield();
    }
    producer.join();
    Check(ok && pushed && expected == 20001 && !q.Memory().charged_bytes, "concurrent producer/consumer preserves every record and cursor");
}
void StreamSignals() {
    HANDLE signal = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    bool ok = signal != nullptr;
    {
        auto stream = UsnStreamTestAccess::Make(signal);
        auto p = Packet(UsnPacketQueue::kReadBytes, 77);
        for (int i = 0; i < 6; ++i) ok &= UsnStreamTestAccess::Push(*stream, p);
        std::vector<BYTE> records; int64_t cursor = 0;
        ok &= stream->Take(records, cursor) && cursor == 77 && WaitForSingleObject(signal, 0) == WAIT_OBJECT_0;
        records.clear(); ok &= stream->Take(records, cursor) && WaitForSingleObject(signal, 0) == WAIT_TIMEOUT;
        Check(ok, "production UsnStream::Take re-signals only when a batch leaves packets");
        UsnStreamTestAccess::Fail(*stream, ERROR_NOT_ENOUGH_MEMORY);
        Check(WaitForSingleObject(signal, 0) == WAIT_OBJECT_0 && stream->Error() == ERROR_NOT_ENOUGH_MEMORY &&
              !stream->Take(records, cursor), "stream failure wakes consumer and propagates error");
    }
    if (signal) CloseHandle(signal);
}
struct Pipe {
    HANDLE server = INVALID_HANDLE_VALUE, client = INVALID_HANDLE_VALUE;
    HANDLE stop = CreateEventW(nullptr, TRUE, FALSE, nullptr), completed = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    ~Pipe() {
        if (client != INVALID_HANDLE_VALUE) CloseHandle(client);
        if (server != INVALID_HANDLE_VALUE) CloseHandle(server);
        if (stop) CloseHandle(stop); if (completed) CloseHandle(completed);
    }
    bool Open(int id) {
        wchar_t name[128]{};
        swprintf_s(name, L"\\\\.\\pipe\\pulse-usn-regression-%lu-%llu-%d", GetCurrentProcessId(), GetTickCount64(), id);
        server = CreateNamedPipeW(name, PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED,
            PIPE_TYPE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 64, 64, 0, nullptr);
        if (server == INVALID_HANDLE_VALUE || !stop || !completed) return false;
        client = CreateFileW(name, GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        return client != INVALID_HANDLE_VALUE;
    }
};
void WaitCase(int mode, const char* name) {
    Pipe p;
    if (!p.Open(mode)) { Check(false, name); return; }
    OVERLAPPED operation{}; operation.hEvent = p.completed;
    BYTE buffer[64]{}; DWORD received = 0;
    const BOOL immediate = ReadFile(p.server, buffer, sizeof(buffer), &received, &operation);
    if (immediate || GetLastError() != ERROR_IO_PENDING) { Check(false, name); return; }
    const BYTE sent = 123; DWORD written = 0;
    bool setup = true;
    if (mode == 0 || mode == 3) setup = WriteFile(p.client, &sent, 1, &written, nullptr) && written == 1;
    if (mode == 1 || mode == 3 || !setup) SetEvent(p.stop);
    if (mode == 4) { CloseHandle(p.client); p.client = INVALID_HANDLE_VALUE; }
    DWORD error = 0;
    const auto result = AwaitUsnRead(p.server, p.stop, mode == 2 ? nullptr : p.completed, operation, received, error);
    DWORD final_bytes = 0;
    const BOOL complete = GetOverlappedResult(p.server, &operation, &final_bytes, FALSE);
    const DWORD final_error = complete ? ERROR_SUCCESS : GetLastError();
    bool ok = setup && final_error != ERROR_IO_INCOMPLETE;
    if (mode == 0) ok &= result == UsnReadWait::Completed && received == 1 && buffer[0] == sent;
    else if (mode == 1 || mode == 3) ok &= result == UsnReadWait::Stopped;
    else if (mode == 2) ok &= result == UsnReadWait::Failed && error == ERROR_INVALID_HANDLE;
    else ok &= result == UsnReadWait::Failed && error == ERROR_BROKEN_PIPE;
    Check(ok, name);
}
void ProbeJson() {
    IndexMemoryProbe probe;
    probe.retained.usn_streams = 2; probe.retained.usn_queue_packets = 257;
    probe.retained.usn_queue_capacity_bytes = 41120; probe.retained.usn_queue_charged_bytes = 49344;
    probe.retained.usn_sum_stream_peak_charged_bytes = 60000;
    const auto text = probe.Json();
    Check(text.find("\"usn_queue_packets\":257") != std::string::npos &&
          text.find("\"usn_queue_capacity_bytes\":41120") != std::string::npos &&
          text.find("\"usn_sum_stream_peak_charged_bytes\":60000") != std::string::npos,
          "memory JSON exposes queue capacity/charge and explicitly named summed stream peaks");
    std::printf("JSON %s\n", text.c_str());
}
}
int main() {
    TinyAndOwnership(); Batching(); ErrorsAndAllocations(); Concurrent(); StreamSignals();
    WaitCase(0, "pending I/O completion retains buffer bytes");
    WaitCase(1, "stop cancels and drains pending I/O before buffer release");
    WaitCase(2, "WAIT_FAILED preserves original error and drains pending I/O");
    WaitCase(3, "stop wins simultaneous completion; operation is fully drained");
    WaitCase(4, "failed overlapped completion preserves error and drains operation");
    ProbeJson();
    std::printf("RESULT %d PASS %d FAIL\n", passes, failures);
    return failures ? 1 : 0;
}
