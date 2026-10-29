#pragma once

#include "transfer_rate.h"
#include <algorithm>
#include <functional>
#include <string>
#include <string_view>
#include <utility>

namespace pulse::ops {

enum class ArchivePhase { Preparing, Extracting, Completed };

struct ArchiveProgress {
    ArchivePhase phase = ArchivePhase::Preparing;
    int percent = -1; // Completion still requires a successful extractor exit.
    uint64_t total_bytes = 0;
    uint64_t processed_bytes = 0;
    uint64_t total_items = 0;
    uint64_t completed_items = 0;
    double bytes_per_second = 0;
    uint64_t eta_seconds = 0;
    std::wstring current_item;
};
using ArchiveProgressCallback = std::function<void(const ArchiveProgress&)>;

// Worker-owned. 7-Zip emits integer percentages, so extraction byte rates are estimates.
class ArchiveProgressTracker {
public:
    void Begin(ArchivePhase phase, uint64_t bytes, uint64_t items, ULONGLONG tick) {
        status_ = {};
        status_.phase = phase;
        status_.percent = 0;
        status_.total_bytes = bytes;
        status_.total_items = items;
        rate_.Reset(tick, 0);
        digits_ = value_ = 0;
        ignore_line_ = false;
        percent_sample_ready_ = false;
        percent_sample_tick_ = percent_interval_ms_ = 0;
    }

    void Feed(std::string_view chunk, ULONGLONG tick) {
        const auto previous_bytes = status_.processed_bytes;
        bool percent_received = false;
        for (const char c : chunk) {
            if (c == '\r' || c == '\n' || c == '\b') {
                digits_ = value_ = 0;
                ignore_line_ = false;
            } else if (!ignore_line_) {
                if (c >= '0' && c <= '9' && digits_ < 3) {
                    value_ = value_ * 10 + c - '0';
                    ++digits_;
                } else if (c == '%' && digits_ && value_ <= 100) {
                    percent_received = percent_received || value_ >= status_.percent;
                    status_.percent = (std::max)(status_.percent, value_);
                    const auto percent = static_cast<uint64_t>(status_.percent);
                    // Avoid overflow for large archive totals.
                    status_.processed_bytes = status_.total_bytes / 100 * percent
                        + status_.total_bytes % 100 * percent / 100;
                    ignore_line_ = true;
                } else if (c != ' ' || digits_) {
                    ignore_line_ = true;
                }
            }
        }
        // Integer percentages are sparse measurements, not byte counters. Empty
        // polls and repeated percentages must not become zero-throughput samples.
        if (percent_received && !percent_sample_ready_) {
            rate_.Reset(tick, status_.processed_bytes);
            percent_sample_ready_ = true;
            percent_sample_tick_ = tick;
        } else if (percent_sample_ready_ && status_.processed_bytes > previous_bytes) {
            percent_interval_ms_ = tick - percent_sample_tick_;
            percent_sample_tick_ = tick;
            Observe(tick);
        }
        // Allow slow archives enough time to cross the next one-percent bucket,
        // but stop displaying an old estimate when the extractor stalls.
        const auto stale_ms = (std::max)(ULONGLONG{4000}, percent_interval_ms_ * 2);
        if (percent_sample_ready_ && tick - percent_sample_tick_ >= stale_ms) {
            status_.bytes_per_second = 0;
            status_.eta_seconds = 0;
        }
    }

    void Advance(uint64_t bytes, uint64_t items, std::wstring item, ULONGLONG tick) {
        status_.processed_bytes = (std::min)(bytes, status_.total_bytes);
        status_.completed_items = (std::min)(items, status_.total_items);
        status_.current_item = std::move(item);
        const auto total = status_.total_bytes ? status_.total_bytes : status_.total_items;
        const auto done = status_.total_bytes ? status_.processed_bytes : status_.completed_items;
        status_.percent = status_.phase == ArchivePhase::Completed ? 100
            : total ? static_cast<int>(static_cast<long double>(done) * 100 / total) : 0;
        Observe(tick);
    }

    const ArchiveProgress& Status() const { return status_; }

private:
    void Observe(ULONGLONG tick) {
        rate_.Observe(tick, status_.processed_bytes, status_.total_bytes);
        status_.bytes_per_second = rate_.speed();
        status_.eta_seconds = rate_.eta_seconds();
    }
    ArchiveProgress status_;
    TransferRateEstimator rate_;
    int digits_ = 0, value_ = 0;
    bool ignore_line_ = false;
    bool percent_sample_ready_ = false;
    ULONGLONG percent_sample_tick_ = 0;
    ULONGLONG percent_interval_ms_ = 0;
};

} // namespace pulse::ops
