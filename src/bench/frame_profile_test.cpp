// SPDX-License-Identifier: Apache-2.0
#include <cstddef>
#include "../app/frame_profile.h"
#include "presentation_statistics_profile.h"
#include <cmath>
#include <cstdio>
#include <numeric>
#include <thread>

int main() {
    using namespace pulse::app;
    int failures = 0;
    const auto check = [&](bool passed, const char* label) {
        std::printf("[%s] %s\n", passed ? "PASS" : "FAIL", label);
        failures += !passed;
    };
    {
        using State = pulse::bench::PresentStatisticsState;
        pulse::bench::PresentStatisticsSequence sequence;
        DXGI_FRAME_STATISTICS stats{};
        check(sequence.Observe(DXGI_ERROR_UNSUPPORTED, stats) == State::Unsupported &&
            sequence.Observe(E_NOINTERFACE, stats) == State::Unsupported &&
            sequence.Observe(E_FAIL, stats) == State::Error &&
            sequence.Observe(S_OK, stats) == State::Zero,
            "presentation statistics distinguish unsupported, error and successful zero values");
        stats.SyncQPCTime.QuadPart = 100;
        check(sequence.Observe(S_OK, stats) == State::Incomplete,
            "a scheduler timestamp without presentation counters is incomplete");
        stats.PresentCount = 10; stats.PresentRefreshCount = 20; stats.SyncRefreshCount = 21;
        check(sequence.Observe(S_OK, stats) == State::Baseline && sequence.Epoch() == 0,
            "the first usable presentation snapshot establishes a baseline");
        ++stats.SyncRefreshCount; ++stats.SyncQPCTime.QuadPart;
        check(sequence.Observe(S_OK, stats) == State::Unchanged,
            "scheduler refresh and QPC changes alone do not imply a new presentation");
        ++stats.PresentCount; ++stats.PresentRefreshCount;
        check(sequence.Observe(S_OK, stats) == State::Advanced,
            "a later presentation identifier advances the observed sequence");
        stats.PresentCount = 1;
        check(sequence.Observe(S_OK, stats) == State::Reset && sequence.Epoch() == 1 &&
            sequence.Observe(S_OK, stats) == State::Unchanged,
            "counter rollback begins a new epoch without reporting forward progress");
        check(sequence.Observe(DXGI_ERROR_FRAME_STATISTICS_DISJOINT, stats) == State::Disjoint &&
            sequence.Epoch() == 2 && sequence.Observe(S_OK, stats) == State::Baseline,
            "disjoint statistics require a fresh presentation baseline");
    }
    {
        VmProfileProbe disabled;
        disabled.Mark(VmPhase::Base);
    }
    check(frame_profile_sink == nullptr, "disabled observation leaves the thread unbound");
    struct DrawCounts { int begins = 0, ends = 0; int32_t result = 0; } outer, inner_draw;
    const auto hooks = [](DrawCounts& counts) {
        return FrameDrawProfileHooks{&counts,
            [](void* value) noexcept { ++static_cast<DrawCounts*>(value)->begins; },
            [](void* value, int32_t result) noexcept {
                auto& count = *static_cast<DrawCounts*>(value);
                ++count.ends; count.result = result;
            }};
    };
    {
        ObserveFrameDrawProfile draw(hooks(outer));
        BeginFrameDrawProfile(); EndFrameDrawProfile(-1);
        check(outer.begins == 0 && outer.ends == 0, "draw callbacks require an observed frame");
        FrameProfileSample observed;
        ObserveFrameProfile observe(observed);
        BeginFrameDrawProfile(); EndFrameDrawProfile(-7);
        {
            ObserveFrameDrawProfile nested(hooks(inner_draw));
            BeginFrameDrawProfile(); EndFrameDrawProfile(0);
        }
        BeginFrameDrawProfile(); EndFrameDrawProfile(-8);
        check(outer.begins == 2 && outer.ends == 2 && outer.result == -8 &&
            inner_draw.begins == 1 && inner_draw.ends == 1 && inner_draw.result == 0,
            "draw callbacks forward draw results and restore the outer observer");
    }
    check(frame_draw_profile_hooks.context == nullptr && !frame_draw_profile_hooks.begin && !frame_draw_profile_hooks.end,
        "draw observation restores disabled callbacks");
    FrameProfileSample sample;
    {
        ObserveFrameProfile observe(sample);
        FrameProfileProbe frame;
        {
            VmProfileProbe vm;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            vm.Mark(VmPhase::Base);
        }
        const double first_total = sample.vm_ms;
        {
            VmProfileProbe vm;
            vm.Mark(VmPhase::Panes);
        }
        check(sample.vm_builds == 2 && sample.vm_ms >= first_total,
            "all VM builds within one observed frame accumulate");
        check(sample.vm_milliseconds[static_cast<size_t>(VmPhase::Base)] >= 1,
            "VM sections measure actual elapsed time");
        const double sections = std::accumulate(sample.vm_milliseconds.begin(), sample.vm_milliseconds.end(), 0.0);
        check(std::abs(sections - sample.vm_ms) < 0.000001,
            "VM sections partition total VM time without double counting");
        FrameProfileSample nested;
        {
            ObserveFrameProfile inner(nested);
            VmProfileProbe vm;
        }
        check(nested.vm_builds == 1 && frame_profile_sink == &sample,
            "nested observers restore the outer sample");
        {
            PaintProfileProbe pane;
            {
                PaintProfileProbe list;
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                list.Mark(PaintPhase::ListSetup);
                list.Mark(PaintPhase::ListRows);
            }
            pane.Mark(PaintPhase::Pane);
        }
        const auto paint = [&](PaintPhase phase) { return sample.paint_milliseconds[static_cast<size_t>(phase)]; };
        check(paint(PaintPhase::ListSetup) >= 1 &&
            paint(PaintPhase::Pane) >= paint(PaintPhase::ListSetup) + paint(PaintPhase::ListRows),
            "nested list timings are a subset of their enclosing pane timing");
        frame.Mark(FramePhase::Prepare);
        frame.Finish();
        check(sample.complete && sample.total_ms >= sample.vm_ms,
            "VM measurements remain inside the observed frame lifetime");
        FrameProfileProbe next_frame;
        check(sample.vm_builds == 0 && sample.vm_ms == 0,
            "a new frame clears previous VM measurements");
        check(std::accumulate(sample.paint_milliseconds.begin(), sample.paint_milliseconds.end(), 0.0) == 0,
            "a new frame clears previous paint measurements");
    }
    check(frame_profile_sink == nullptr, "observation restores the disabled state");
    return failures ? 1 : 0;
}
