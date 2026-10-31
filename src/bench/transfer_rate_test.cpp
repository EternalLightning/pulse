// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "ops/transfer_rate.h"
#include <cmath>
#include <iostream>

namespace {
int failures = 0;
void Check(bool value, const char* name) {
    std::cout << (value ? "[PASS] " : "[FAIL] ") << name << '\n';
    if (!value) ++failures;
}
bool ConsistentEta(const pulse::ops::TransferRateEstimator& rate, uint64_t bytes,
                   uint64_t total) {
    if (bytes >= total || rate.speed() <= 1.0) return rate.eta_seconds() == 0;
    const double expected = static_cast<double>(total - bytes) / rate.speed();
    return static_cast<double>(rate.eta_seconds()) >= expected
        && static_cast<double>(rate.eta_seconds()) < expected + 1.0;
}
}

int main() {
    using pulse::ops::TransferRateEstimator;
    constexpr uint64_t mib = 1024ull * 1024;
    constexpr uint64_t total = 22ull * 1024 * mib;
    TransferRateEstimator rate;
    rate.Reset(0, 0);
    rate.Observe(500, 50 * mib, total);
    Check(rate.speed() == 0 && rate.eta_seconds() == 0,
          "warmup does not invent speed or ETA");
    uint64_t bytes = 0;
    rate.Reset(0, 0);
    for (ULONGLONG tick = 100; tick <= 5000; tick += 100) {
        bytes += 10 * mib;
        rate.Observe(tick, bytes, total);
    }
    Check(std::abs(rate.speed() / mib - 100.0) < 0.001,
          "steady byte progress yields steady throughput");
    Check(ConsistentEta(rate, bytes, total),
          "steady ETA agrees with remaining bytes and displayed speed");

    bool eta_matches = true;
    for (ULONGLONG tick = 5100; tick <= 12000; tick += 100) {
        bytes += tick < 8000 ? mib : 30 * mib;
        rate.Observe(tick, bytes, total);
        eta_matches = eta_matches && ConsistentEta(rate, bytes, total);
    }
    Check(eta_matches, "slowdown and rapid recovery keep ETA consistent with speed");
    Check(rate.speed() > 200 * mib,
          "four-second window and EMA recover after sustained faster progress");
    std::cout << "[DIAG] recovery speed MiB/s=" << rate.speed() / mib
              << " remaining MiB=" << static_cast<double>(total - bytes) / mib
              << " ETA seconds=" << rate.eta_seconds() << '\n';

    rate.Observe(12100, bytes, total);
    Check(rate.speed() > 0 && ConsistentEta(rate, bytes, total),
          "a repeated byte count between chunks does not imply zero throughput");
    for (ULONGLONG tick = 12200; tick <= 17000; tick += 100)
        rate.Observe(tick, bytes, total);
    Check(rate.speed() == 0 && rate.eta_seconds() == 0,
          "observed full window without byte progress clears stale estimates");
    bytes += 25 * mib;
    rate.Observe(17100, bytes, total);
    Check(rate.speed() > 0 && ConsistentEta(rate, bytes, total),
          "progress after a stall restores a usable consistent estimate");

    rate.Reset(30000, bytes);
    Check(rate.speed() == 0 && rate.eta_seconds() == 0,
          "pause reset discards pre-pause speed and ETA");
    rate.Observe(30400, bytes + 40 * mib, total);
    Check(rate.speed() == 0 && rate.eta_seconds() == 0,
          "resume warms up without counting pause time");
    bytes += 100 * mib;
    rate.Observe(31000, bytes, total);
    Check(std::abs(rate.speed() / mib - 100.0) < 0.001
        && ConsistentEta(rate, bytes, total),
          "resume estimates only new transferred bytes");

    rate.Observe(31001, bytes, bytes);
    Check(rate.eta_seconds() == 0, "total shrinking to processed bytes clears ETA immediately");
    const double speed_before_total_change = rate.speed();
    rate.Observe(31002, bytes, bytes + 100 * mib);
    Check(rate.speed() == speed_before_total_change
        && ConsistentEta(rate, bytes, bytes + 100 * mib),
          "changed totals update ETA even inside sampling throttle");
    rate.Observe(31003, bytes, 0);
    Check(rate.eta_seconds() == 0, "unknown total has no remaining-time estimate");

    rate.Reset(40000, 0);
    rate.Observe(41000, 100 * mib, 200 * mib);
    rate.Observe(41000, 200 * mib, 200 * mib);
    Check(rate.eta_seconds() == 0, "same-tick completion clears ETA");
    rate.Observe(42000, mib, total);
    Check(rate.speed() == 0 && rate.eta_seconds() == 0,
          "byte rollback starts a fresh estimate");
    rate.Observe(42300, 2 * mib, 2 * mib);
    Check(rate.eta_seconds() == 0, "completion during warmup has no remaining time");

    rate.Reset(50000, 0);
    rate.Observe(51000, 100 * mib, total);
    rate.Observe(61000, 1100 * mib, total);
    Check(std::abs(rate.speed() / mib - 100.0) < 0.001
        && ConsistentEta(rate, 1100 * mib, total),
          "sparse callbacks measure transferred bytes across their elapsed interval");
    std::cout << failures << " failure(s)\n";
    return failures ? 1 : 0;
}
