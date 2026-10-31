// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once
#include <windows.h>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

namespace pulse::index {
// One queue per volume. Own exact-length copies; never retain the I/O buffer.
class UsnPacketQueue {
public:
    static constexpr size_t kReadBytes = 256 * 1024;
    static constexpr size_t kTakeBytes = 1024 * 1024;
    static constexpr size_t kBudgetBytes = 64 * 1024 * 1024;
    struct Stats {
        size_t packets = 0, payload_bytes = 0, capacity_bytes = 0, charged_bytes = 0;
        size_t peak_packets = 0, peak_capacity_bytes = 0, peak_charged_bytes = 0;
        uint64_t overflows = 0;
    };
    explicit UsnPacketQueue(size_t budget = kBudgetBytes) : budget_(budget) {}
    ~UsnPacketQueue() {
        // Avoid recursive destruction of a long chain after overflow/shutdown.
        while (head_) { auto packet = std::move(head_); head_ = std::move(packet->next); }
    }
    bool Push(const BYTE* data, size_t bytes) {
        std::lock_guard lock(mutex_);
        if (error_) return false;
        if (!data || bytes < sizeof(int64_t) || bytes > kReadBytes) {
            error_ = ERROR_INVALID_DATA; return false;
        }
        if (bytes > budget_ - stats_.charged_bytes ||
            sizeof(Packet) > budget_ - stats_.charged_bytes - bytes) return Overflow();
        auto packet = std::make_unique<Packet>(data, bytes);
        const size_t capacity = packet->data.capacity();
        // Charge actual buffer capacity plus the packet object. Allocator
        // bookkeeping is excluded, but there is no unaccounted container map.
        if (capacity > budget_ - stats_.charged_bytes ||
            sizeof(Packet) > budget_ - stats_.charged_bytes - capacity) return Overflow();
        Packet* tail = packet.get();
        if (tail_) tail_->next = std::move(packet);
        else head_ = std::move(packet);
        tail_ = tail;
        ++stats_.packets;
        stats_.payload_bytes += bytes;
        stats_.capacity_bytes += capacity;
        stats_.charged_bytes += capacity + sizeof(Packet);
        if (stats_.packets > stats_.peak_packets) stats_.peak_packets = stats_.packets;
        if (stats_.capacity_bytes > stats_.peak_capacity_bytes) stats_.peak_capacity_bytes = stats_.capacity_bytes;
        if (stats_.charged_bytes > stats_.peak_charged_bytes) stats_.peak_charged_bytes = stats_.charged_bytes;
        return true;
    }
    bool Take(std::vector<BYTE>& records, int64_t& cursor, bool& more) {
        std::lock_guard lock(mutex_);
        more = false;
        if (error_) return false;
        while (head_ && records.size() < kTakeBytes) {
            const auto& data = head_->data;
            records.insert(records.end(), data.begin() + sizeof(int64_t), data.end());
            std::memcpy(&cursor, data.data(), sizeof(cursor));
            --stats_.packets;
            stats_.payload_bytes -= data.size();
            stats_.capacity_bytes -= data.capacity();
            stats_.charged_bytes -= data.capacity() + sizeof(Packet);
            auto packet = std::move(head_);
            head_ = std::move(packet->next);
            if (!head_) tail_ = nullptr;
        }
        more = head_ != nullptr;
        return true;
    }
    void Fail(DWORD error) {
        std::lock_guard lock(mutex_);
        if (!error_) error_ = error;
    }
    DWORD Error() const { return error_.load(); }
    Stats Memory() const { std::lock_guard lock(mutex_); return stats_; }
private:
    struct Packet {
        Packet(const BYTE* bytes, size_t size) : data(bytes, bytes + size) {}
        std::vector<BYTE> data;
        std::unique_ptr<Packet> next;
    };
    bool Overflow() {
        error_ = ERROR_BUFFER_OVERFLOW; ++stats_.overflows; return false;
    }
    const size_t budget_;
    mutable std::mutex mutex_;
    std::unique_ptr<Packet> head_;
    Packet* tail_ = nullptr;
    Stats stats_;
    std::atomic<DWORD> error_{ERROR_SUCCESS};
};
}
