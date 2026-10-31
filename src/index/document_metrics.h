// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once
#include <windows.h>
#include <cstdint>

namespace pulse::index {
struct DocumentReadMetrics {
    uint64_t package_us = 0, enumerate_us = 0, parts_inclusive_us = 0, part_open_us = 0;
    uint64_t stream_read_us = 0, stream_bytes = 0, stream_reads = 0;
    uint64_t filter_us = 0, cpu_us = 0;
    uint64_t pdf_load_us = 0, pdf_page_us = 0, pdf_text_us = 0, pdf_pages = 0, pdf_fallbacks = 0;
    uint64_t reader_queue_us = 0, admission_wait_us = 0, process_start_us = 0;
    uint64_t response_us = 0, process_stop_us = 0, process_starts = 0;
    // Callback and Win32 read wall times overlap page/load timing; bytes are
    // logical reads, not physical disk traffic. Map counters are experimental.
    uint64_t pdf_read_calls = 0, pdf_requested_bytes = 0, pdf_reader_us = 0;
    uint64_t pdf_file_reads = 0, pdf_file_bytes = 0, pdf_file_io_us = 0, pdf_cache_hits = 0;
    uint64_t pdf_map_views = 0, pdf_mapped_bytes = 0, pdf_map_fallbacks = 0;
    // Pre-termination exit code: STILL_ACTIVE means no exit was observed.
    // memory_known bits: 1=job peak, 2=process counters.
    uint64_t child_exit_code = 0, child_exit_known = 0, child_failure_stage = 0;
    uint64_t child_peak_private_bytes = 0, child_private_bytes = 0, child_memory_known = 0;
    uint64_t read_attempts = 0, memory_recycles = 0;
    uint64_t fresh_filter_fallbacks = 0, fresh_filter_recoveries = 0, fresh_filter_skips = 0;
    uint64_t fresh_filter_first_error = 0, fresh_filter_first_exit = 0;
    // Full per-attempt child CPU, independent from (overlapping) Extract cpu_us.
    uint64_t attempt_cpu_us = 0, released_private_bytes = 0, released_memory_known = 0;
};
inline void AddReaderMetrics(DocumentReadMetrics& sum, const DocumentReadMetrics& value) noexcept {
    sum.reader_queue_us += value.reader_queue_us; sum.admission_wait_us += value.admission_wait_us;
    sum.process_start_us += value.process_start_us; sum.response_us += value.response_us;
    sum.process_stop_us += value.process_stop_us; sum.process_starts += value.process_starts;
    sum.read_attempts += value.read_attempts; sum.memory_recycles += value.memory_recycles;
    sum.fresh_filter_fallbacks += value.fresh_filter_fallbacks;
    sum.fresh_filter_recoveries += value.fresh_filter_recoveries; sum.fresh_filter_skips += value.fresh_filter_skips;
    if (value.fresh_filter_first_error) {
        sum.fresh_filter_first_error = value.fresh_filter_first_error;
        sum.fresh_filter_first_exit = value.fresh_filter_first_exit;
    }
    sum.attempt_cpu_us += value.attempt_cpu_us;
    if (value.released_memory_known) {
        sum.released_memory_known = value.released_memory_known; sum.released_private_bytes = value.released_private_bytes;
    }
    if (value.child_failure_stage) {
        sum.child_exit_code = value.child_exit_code; sum.child_exit_known = value.child_exit_known;
        sum.child_failure_stage = value.child_failure_stage;
        sum.child_private_bytes = value.child_private_bytes; sum.child_memory_known = value.child_memory_known;
    }
    if (value.child_peak_private_bytes > sum.child_peak_private_bytes) sum.child_peak_private_bytes = value.child_peak_private_bytes;
}
inline void AddExtractionMetrics(DocumentReadMetrics& sum, const DocumentReadMetrics& value) noexcept {
    sum.package_us += value.package_us; sum.enumerate_us += value.enumerate_us;
    sum.parts_inclusive_us += value.parts_inclusive_us; sum.part_open_us += value.part_open_us;
    sum.stream_read_us += value.stream_read_us; sum.stream_bytes += value.stream_bytes; sum.stream_reads += value.stream_reads;
    sum.filter_us += value.filter_us; sum.cpu_us += value.cpu_us;
    sum.pdf_load_us += value.pdf_load_us; sum.pdf_page_us += value.pdf_page_us; sum.pdf_text_us += value.pdf_text_us;
    sum.pdf_pages += value.pdf_pages; sum.pdf_fallbacks += value.pdf_fallbacks;
    sum.pdf_read_calls += value.pdf_read_calls; sum.pdf_requested_bytes += value.pdf_requested_bytes; sum.pdf_reader_us += value.pdf_reader_us;
    sum.pdf_file_reads += value.pdf_file_reads; sum.pdf_file_bytes += value.pdf_file_bytes; sum.pdf_file_io_us += value.pdf_file_io_us;
    sum.pdf_cache_hits += value.pdf_cache_hits; sum.pdf_map_views += value.pdf_map_views;
    sum.pdf_mapped_bytes += value.pdf_mapped_bytes; sum.pdf_map_fallbacks += value.pdf_map_fallbacks;
}
inline uint64_t DocumentMicros() noexcept {
    static const uint64_t frequency = [] {
        LARGE_INTEGER f{}; QueryPerformanceFrequency(&f); return static_cast<uint64_t>(f.QuadPart);
    }();
    LARGE_INTEGER now{}; QueryPerformanceCounter(&now);
    const auto ticks = static_cast<uint64_t>(now.QuadPart);
    return frequency ? ticks / frequency * 1000000 + ticks % frequency * 1000000 / frequency : GetTickCount64() * 1000;
}
inline uint64_t DocumentCpuMicros() noexcept {
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) return 0;
    return (((uint64_t{kernel.dwHighDateTime} << 32) | kernel.dwLowDateTime) +
        ((uint64_t{user.dwHighDateTime} << 32) | user.dwLowDateTime)) / 10;
}
class DocumentMetricScope {
public:
    explicit DocumentMetricScope(uint64_t& sum) : sum_(sum), start_(DocumentMicros()) {}
    ~DocumentMetricScope() { Finish(); }
    void Finish() { if (!finished_) { sum_ += DocumentMicros() - start_; finished_ = true; } }
private:
    uint64_t& sum_;
    uint64_t start_;
    bool finished_ = false;
};
}
