// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "../ui/tab_indicator_animation.h"
#include "../ui/tab_close_animation.h"
#include <cmath>
#include <cstdio>

namespace {
int failures = 0;
int checks = 0;
void Check(bool condition, const char* message) {
    ++checks;
    std::printf("[%s] %s\n", condition ? "PASS" : "FAIL", message);
    if (!condition) ++failures;
}
}

int main() {
    using pulse::ui::TabIndicatorAnimation;
    int tab_a = 0, tab_b = 0, tab_c = 0;
    for (const bool rightward : {false, true}) {
        auto leading = [&](uint64_t elapsed) {
            TabIndicatorAnimation sample;
            sample.Tick(&tab_a, rightward ? 16.0f : 616.0f, rightward ? 184.0f : 784.0f, 1000, true);
            sample.Tick(&tab_b, rightward ? 616.0f : 16.0f, rightward ? 784.0f : 184.0f, 1100, true);
            sample.Tick(&tab_b, rightward ? 616.0f : 16.0f, rightward ? 784.0f : 184.0f, 1100 + elapsed, true);
            return rightward ? sample.right : -sample.left;
        };
        const float moving_acceleration = std::abs(leading(44) - 2 * leading(42) + leading(40));
        Check(std::abs(leading(4) - 2 * leading(2) + leading(0)) < moving_acceleration * 0.25f,
            "leading edge builds acceleration gradually from rest in either direction");
        Check(std::abs(leading(178) - 2 * leading(176) + leading(174)) < moving_acceleration * 0.25f,
            "leading edge eases acceleration back to rest before its stretch ends");
        Check(leading(4) - leading(0) < leading(44) - leading(40) &&
              leading(178) - leading(174) < leading(44) - leading(40),
            "leading edge starts and stops more slowly than its moving middle");
    }
    TabIndicatorAnimation line;
    line.Tick(&tab_a, 16, 184, 1000, true);
    Check(!line.pending && line.left == 16 && line.right == 184,
        "first tab appears immediately without requesting frames");
    line.Tick(&tab_b, 216, 384, 1100, true);
    Check(line.pending && line.left == 16 && line.right == 184,
        "switch begins at the currently displayed underline");
    line.Tick(&tab_b, 216, 384, 1124, true);
    Check(line.left == 16 && line.right > 184 && line.right < 384,
        "rightward switch extends the leading edge while holding the trailing edge");
    line.Tick(&tab_b, 216, 384, 1208, true);
    const float overlap_left = line.left, overlap_right = line.right;
    Check(line.left > 16 && line.left < 216 && line.right > 184 && line.right < 384 &&
          line.right - line.left > 168,
        "both edges overlap in motion while the underline remains stretched");
    line.Tick(&tab_b, 216, 384, 1212, true);
    Check(line.left > overlap_left && line.right > overlap_right,
        "the former phase boundary has no all-edge pause");
    line.Tick(&tab_b, 216, 384, 1280, true);
    Check(line.left > 16 && line.left < 216 && line.right == 384,
        "contraction catches up after the leading edge arrives");
    line.Tick(&tab_b, 216, 384, 1340, true);
    Check(!line.pending && line.left == 216 && line.right == 384,
        "switch finishes exactly at the target and stops requesting frames");

    line.Tick(&tab_a, 16, 184, 1400, true);
    line.Tick(&tab_a, 16, 184, 1424, true);
    Check(line.left > 16 && line.left < 216 && line.right == 384,
        "leftward switch extends the left edge first");
    const float reverse_left = line.left, reverse_right = line.right;
    line.Tick(&tab_b, 216, 384, 1424, true);
    Check(line.left == reverse_left && line.right == reverse_right,
        "rapid reversal preserves the sampled span without a jump");
    line.Tick(&tab_b, 216, 384, 1432, true);
    Check(line.left > reverse_left && line.right == reverse_right,
        "reversal contracts immediately when the target is already inside the span");
    line.Tick(&tab_b, 216, 384, 1564, true);
    Check(!line.pending && line.left == 216 && line.right == 384,
        "reversal settles at the requested tab");

    line.Tick(&tab_a, 16, 184, 1600, true);
    line.Tick(&tab_a, 16, 184, 1640, true);
    const float sampled_left = line.left, sampled_right = line.right;
    line.Tick(&tab_c, 416, 448, 1640, true);
    Check(line.left == sampled_left && line.right == sampled_right,
        "retargeting to a narrow pinned tab preserves both displayed edges");
    bool bounded = true;
    for (uint64_t now = 1644; now <= 1880; now += 4) {
        line.Tick(&tab_c, 416, 448, now, true);
        bounded &= line.left >= sampled_left && line.right <= 448 && line.left <= line.right;
    }
    Check(bounded && !line.pending && line.left == 416 && line.right == 448,
        "high-refresh samples stay bounded and settle on a narrower target");

    line.Tick(&tab_b, 216, 384, 1900, true);
    line.Tick(&tab_b, 216, 384, 1950, true);
    line.Tick(&tab_b, 216, 384, 1950, false);
    Check(!line.pending && line.left == 216 && line.right == 384,
        "disabling animations during a transition snaps to its final underline");
    line.Tick(&tab_a, 16, 184, 2000, false);
    Check(!line.pending && line.left == 16 && line.right == 184,
        "disabled animation switches immediately without requesting frames");
    line.Tick(&tab_b, 216, 384, 2100, true);
    line.Tick(&tab_b, 316, 484, 2150, true);
    Check(!line.pending && line.left == 316 && line.right == 484,
        "layout changes follow the active tab immediately instead of chasing stale geometry");
    line.Reset();
    line.Tick(&tab_c, 832, 896, 50000, true);
    Check(!line.pending && line.left == 832 && line.right == 896,
        "new visible strip starts at current DPI geometry without stale motion");

    line.Tick(&tab_a, 32, 368, 51000, true);
    line.Tick(&tab_a, 32, 368, 51080, true);
    const float before_clock_reset = line.left;
    line.Tick(&tab_b, 432, 768, 51080, true);
    line.Tick(&tab_b, 432, 768, 51079, true);
    Check(line.left == before_clock_reset && line.left <= line.right,
        "non-increasing timestamps cannot underflow or invert the span");
    line.Tick(&tab_b, 432, 768, 100000, true);
    Check(!line.pending && line.left == 432 && line.right == 768,
        "long idle or minimized intervals settle instead of replaying old frames");
    for (const int hz : {30, 60, 144, 240}) {
        line.Reset();
        line.Tick(&tab_a, 16, 184, 1000, true);
        line.Tick(&tab_b, 616, 784, 1100, true);
        bool continuous = true;
        float previous_left = line.left, previous_right = line.right;
        const int max_frames = hz * 2;
        for (int frame = 1; frame <= max_frames; ++frame) {
            const uint64_t now = 1100 + static_cast<uint64_t>(frame) * 1000 / hz;
            line.Tick(&tab_b, 616, 784, now, true);
            continuous &= line.left >= previous_left && line.right >= previous_right &&
                line.left <= line.right && (line.left > previous_left || line.right > previous_right);
            previous_left = line.left;
            previous_right = line.right;
            if (!line.pending) break;
        }
        if (line.pending) std::printf("[DIAG] hz=%d left=%f right=%f pending=1 after 2000 ms\n", hz, line.left, line.right);
        Check(continuous && !line.pending && line.left == 616 && line.right == 784,
            "display-paced samples advance continuously and stop exactly at the target");
    }
    using pulse::ui::TabCloseAnimation;
    TabCloseAnimation close;
    using Items = std::vector<TabCloseAnimation::Item>;
    const Items four{{&tab_a, -1, 12, 212}, {&tab_b, -1, 216, 416},
        {&tab_c, -1, 420, 620}, {nullptr, -1, 624, 656}};
    const Items rest{{&tab_b, -1, 12, 252}, {&tab_c, -1, 256, 496}, {nullptr, -1, 500, 532}};
    close.Capture(four, 960, 1);
    auto spans = rest;
    close.Apply(spans, 1000, 960, 1, true);
    Check(close.Pending() && spans[0] == four[1] && spans[1] == four[2] && spans.back() == four.back(),
        "closing a left tab preserves surviving spans and the plus button on the first frame");
    spans = rest; close.Apply(spans, 1100, 960, 1, true);
    Check(spans[0].left == 114 && spans[0].right == 334 && spans.back().left == 562,
        "closing interpolates tab positions, widths and plus button together");
    const auto halfway = spans;
    close.Capture(spans, 960, 1);
    const Items last{{&tab_c, -1, 12, 252}, {nullptr, -1, 256, 288}};
    spans = last; close.Apply(spans, 1100, 960, 1, true);
    Check(spans[0] == halfway[1] && spans.back() == halfway.back(),
        "a consecutive close retargets from the displayed positions without jumping");
    spans = last; close.Apply(spans, 1300, 960, 1, true);
    Check(!close.Pending() && spans == last, "closing settles exactly after elapsed time and stops requesting frames");
    for (const int hz : {30, 60, 144, 240}) {
        close.Capture(four, 960, 1);
        spans = rest; close.Apply(spans, 1000, 960, 1, true);
        float previous = spans[1].left;
        bool monotonic = true;
        for (int frame = 1; close.Pending() && frame <= hz * 2; ++frame) {
            spans = rest;
            close.Apply(spans, 1000 + static_cast<uint64_t>(frame) * 1000 / hz, 960, 1, true);
            monotonic &= spans[1].left < previous && spans[1].right >= spans[1].left;
            previous = spans[1].left;
        }
        if (close.Pending()) std::printf("[DIAG] close hz=%d remains pending after 2000 ms\n", hz);
        Check(monotonic && !close.Pending() && spans == rest, "close timing is independent of sampling frequency");
    }
    for (int cancel = 0; cancel != 5; ++cancel) {
        close.Capture(four, 960, 1);
        spans = rest; close.Apply(spans, 1000, 960, 1, true);
        auto target = rest;
        if (cancel == 3) target.push_back({&tab_a, -1, 600, 800});
        if (cancel == 4) { target[0].left -= 10; target[0].right -= 10; }
        spans = target;
        close.Apply(spans, 1050, cancel == 0 ? 1100.0f : 960.0f,
            cancel == 1 ? 1.5f : 1.0f, cancel != 2);
        Check(!close.Pending() && spans == target,
            "resize, DPI, disabled motion or unrelated layout changes discard stale close coordinates");
    }
    close.Capture(four, 960, 1);
    spans = rest; close.Apply(spans, 1000, 960, 1, true);
    spans = rest; close.Apply(spans, 999, 960, 1, true);
    Check(spans[0] == four[1], "non-increasing close timestamps cannot underflow");
    spans = rest; close.Apply(spans, 99999, 960, 1, true);
    Check(!close.Pending() && spans == rest, "a hidden or idle interval settles the close without replaying frames");
    const Items grouped{{&tab_a, -1, 12, 60}, {nullptr, 7, 68, 100},
        {&tab_b, -1, 104, 304}, {&tab_c, -1, 308, 508}, {nullptr, -1, 512, 544}};
    const Items grouped_rest{{&tab_a, -1, 12, 60}, {nullptr, 7, 68, 100},
        {&tab_c, -1, 104, 344}, {nullptr, -1, 348, 380}};
    close.Capture(grouped, 960, 1);
    spans = grouped_rest; close.Apply(spans, 1000, 960, 1, true);
    Check(spans[0] == grouped[0] && spans[1] == grouped[1] && spans[2] == grouped[3],
        "pinned tabs and stable group chips retain their own geometry while members close");
    close.Capture(rest, 960, 1);
    spans = rest; close.Apply(spans, 1000, 960, 1, true);
    Check(!close.Pending() && spans == rest, "an unchanged strip cannot start an empty animation loop");
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
