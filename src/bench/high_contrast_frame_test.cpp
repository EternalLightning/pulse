// SPDX-License-Identifier: Apache-2.0
#include "../ui/FluentTokens.h"
#include <cstdio>
#include <thread>

int main() {
    using namespace pulse::ui;
    int failures = 0;
    const auto check = [&](bool passed, const char* label) {
        std::printf("[%s] %s\n", passed ? "PASS" : "FAIL", label);
        failures += !passed;
    };
    const bool system_value = IsHighContrast();
    {
        HighContrastFrameScope outer(!system_value);
        check(IsHighContrast() == !system_value, "frame uses its captured contrast value");
        {
            HighContrastFrameScope nested(system_value);
            check(IsHighContrast() == system_value, "nested painting can override its captured value");
        }
        check(IsHighContrast() == !system_value, "nested painting restores the outer frame value");
        bool other_thread = !system_value;
        std::thread worker([&] { other_thread = IsHighContrast(); });
        worker.join();
        check(other_thread == system_value, "frame contrast state stays on the painting thread");
    }
    check(IsHighContrast() == system_value, "leaving a frame restores live system queries");
    {
        HighContrastFrameScope next;
        check(IsHighContrast() == system_value, "next frame captures the current system value");
    }
    return failures ? 1 : 0;
}
