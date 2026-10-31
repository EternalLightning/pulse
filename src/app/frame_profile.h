// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <array>
#include <chrono>
#include <cstdint>

namespace pulse::app {

enum class FramePhase : size_t { Prepare, Paint, Submit, Present, AfterPresent, Count };
struct FrameProfileSample {
    std::array<double, static_cast<size_t>(FramePhase::Count)> milliseconds{};
    double total_ms = 0;
    int32_t draw_result = 0;
    bool presented = false;
    bool complete = false;
};
inline thread_local FrameProfileSample* frame_profile_sink = nullptr;

// Opt-in observation; normal frames do not allocate, read the clock, or log here.
class FrameProfileProbe {
public:
    FrameProfileProbe() : sink_(frame_profile_sink) {
        if (sink_) { *sink_ = {}; first_ = last_ = Clock::now(); }
    }
    void Mark(FramePhase phase) {
        if (!sink_) return;
        const auto now = Clock::now();
        sink_->milliseconds[static_cast<size_t>(phase)] = std::chrono::duration<double, std::milli>(now - last_).count();
        last_ = now;
    }
    void Submitted(int32_t result) { if (sink_) sink_->draw_result = result; }
    void Presented(bool presented) { if (sink_) sink_->presented = presented; }
    void Finish() {
        if (!sink_) return;
        Mark(FramePhase::AfterPresent);
        sink_->total_ms = std::chrono::duration<double, std::milli>(last_ - first_).count();
        sink_->complete = true;
    }
private:
    using Clock = std::chrono::steady_clock;
    FrameProfileSample* sink_;
    Clock::time_point first_{}, last_{};
};

class ObserveFrameProfile {
public:
    explicit ObserveFrameProfile(FrameProfileSample& sample) : previous_(frame_profile_sink) { frame_profile_sink = &sample; }
    ~ObserveFrameProfile() { frame_profile_sink = previous_; }
    ObserveFrameProfile(const ObserveFrameProfile&) = delete;
    ObserveFrameProfile& operator=(const ObserveFrameProfile&) = delete;
private:
    FrameProfileSample* previous_;
};

} // namespace pulse::app
