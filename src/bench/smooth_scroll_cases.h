#pragma once
#include "../ui/smooth_scroll.h"
#include <cstdio>
#include <cmath>

inline int RunSmoothScrollChecks() {
    int failures = 0;
    const auto check = [&](bool passed, const char* label) {
        std::printf("[%s] %s\n", passed ? "PASS" : "FAIL", label);
        failures += !passed;
    };
    pulse::ui::SmoothScroll motion;
    float value = 0;
    motion.ScrollBy(value, 72, 1000, 300, 1000, true);
    check(value == 0 && motion.Pending(), "wheel input schedules movement without jumping");
    motion.Tick(value, 1000, 1032, true);
    check(value > 0 && value < 72, "wheel has an intermediate pixel position");
    const float forward = value;
    motion.ScrollBy(value, -30, 1000, 300, 1032, true);
    motion.Tick(value, 1000, 1048, true);
    check(value < forward, "direction reversal responds immediately");
    for (uint64_t now = 1064; motion.Pending() && now < 4000; now += 16) motion.Tick(value, 1000, now, true);
    check(!motion.Pending() && std::abs(value - std::max(0.0f, forward - 30)) < 0.6f,
        "wheel settles at the proportional endpoint and stops");
    motion.ScrollBy(value, 100, 1000, 300, 4000, true);
    motion.Tick(value, 20, 4016, true);
    check(!motion.Pending() && value <= 20, "shrinking content cancels residual momentum");
    motion.Reset();
    value = 40;
    motion.ScrollBy(value, 80, 1000, 300, 5000, true);
    value = 0;
    motion.Tick(value, 1000, 5016, true);
    check(value == 0 && !motion.Pending(), "external positioning cancels a pending gesture");
    motion.ScrollBy(value, 9, 1000, 300, 6000, false);
    check(value == 9 && !motion.Pending(), "disabled motion immediately reaches the endpoint");
    value = 1000;
    motion.ScrollBy(value, 50, 1000, 300, 7000, true);
    check(!motion.Pending() && value == 1000, "wheel at the boundary schedules no frames");
    motion.Reset();
    value = 50;
    motion.ScrollBy(value, 10, 1000, 300, 8000, true);
    motion.ScrollBy(value, 20, 1000, 300, 8000, true);
    motion.Tick(value, 1000, 8016, true);
    check(value > 50 && value < 80, "successive wheel input combines into continuous momentum");
    motion.Tick(value, 1000, 8016, false);
    check(value == 80 && !motion.Pending(), "disabling motion during a gesture applies its endpoint");
    motion.ScrollBy(value, -100, 0, 300, 9000, true);
    check(value == 0 && !motion.Pending(), "empty content clamps without scheduling frames");
    motion.ScrollBy(value, 30, 100, 300, 10000, true);
    motion.Tick(value, 1000, 10016, true);
    check(value > 0 && motion.Pending(), "growing content preserves an active gesture");
    motion.Reset();
    const float reset = value;
    motion.Tick(value, 1000, 10032, true);
    check(value == reset && !motion.Pending(), "explicit cancellation leaves the last presented offset stable");
    pulse::ui::SmoothScroll fast, slow;
    float a = 0, b = 0;
    fast.ScrollBy(a, 72, 1000, 300, 11000, true);
    slow.ScrollBy(b, 72, 1000, 300, 11000, true);
    for (uint64_t now = 11008; now <= 11096; now += 8) fast.Tick(a, 1000, now, true);
    slow.Tick(b, 1000, 11096, true);
    check(std::abs(a - b) < 0.01f, "equal elapsed time gives the same position across frame rates");
    std::printf("[RESULT] smooth scroll failures=%d\n", failures);
    return failures;
}
