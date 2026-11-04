// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <array>
#include <chrono>
#include <cstdint>

namespace pulse::app {

enum class FramePhase : size_t { Prepare, Paint, Submit, Present, AfterPresent, Count };
enum class VmPhase : size_t { Base, Commands, Operations, Sidebar, Tabs, Status, Tray, Details, Panes, Other, Count };
enum class PaintPhase : size_t {
    Setup, Backdrop, Title, Toolbar, Sidebar, Pane, Details, Settings, Status, Overlays, Other,
    ListSetup, ListRows, ListLayout, ListSlots, ListMotion, ListColumns, Count
};
struct FrameProfileSample {
    std::array<double, static_cast<size_t>(FramePhase::Count)> milliseconds{};
    std::array<double, static_cast<size_t>(VmPhase::Count)> vm_milliseconds{};
    std::array<double, static_cast<size_t>(PaintPhase::Count)> paint_milliseconds{};
    double vm_ms = 0;
    uint32_t vm_builds = 0;
    double total_ms = 0;
    int32_t draw_result = 0;
    bool presented = false;
    bool complete = false;
};
inline thread_local FrameProfileSample* frame_profile_sink = nullptr;

struct FrameDrawProfileHooks {
    void* context = nullptr;
    void (*begin)(void*) noexcept = nullptr;
    void (*end)(void*, int32_t) noexcept = nullptr;
};
inline thread_local FrameDrawProfileHooks frame_draw_profile_hooks;

inline void BeginFrameDrawProfile() noexcept {
    if (frame_profile_sink && frame_draw_profile_hooks.begin)
        frame_draw_profile_hooks.begin(frame_draw_profile_hooks.context);
}
inline void EndFrameDrawProfile(int32_t result) noexcept {
    if (frame_profile_sink && frame_draw_profile_hooks.end)
        frame_draw_profile_hooks.end(frame_draw_profile_hooks.context, result);
}

class ObserveFrameDrawProfile {
public:
    explicit ObserveFrameDrawProfile(FrameDrawProfileHooks hooks) : previous_(frame_draw_profile_hooks) {
        frame_draw_profile_hooks = hooks;
    }
    ~ObserveFrameDrawProfile() { frame_draw_profile_hooks = previous_; }
    ObserveFrameDrawProfile(const ObserveFrameDrawProfile&) = delete;
    ObserveFrameDrawProfile& operator=(const ObserveFrameDrawProfile&) = delete;
private:
    FrameDrawProfileHooks previous_;
};

// List phases are nested inside Pane; top-level paint phases are exclusive.
class PaintProfileProbe {
public:
    PaintProfileProbe() : sink_(frame_profile_sink) {
        if (sink_) last_ = Clock::now();
    }
    void Mark(PaintPhase phase) {
        if (!sink_) return;
        const auto now = Clock::now();
        sink_->paint_milliseconds[static_cast<size_t>(phase)] +=
            std::chrono::duration<double, std::milli>(now - last_).count();
        last_ = now;
    }
private:
    using Clock = std::chrono::steady_clock;
    FrameProfileSample* sink_;
    Clock::time_point last_{};
};

// VM sections accumulate across every BuildVm call made by an observed frame.
class VmProfileProbe {
public:
    VmProfileProbe() : sink_(frame_profile_sink) {
        if (sink_) { first_ = last_ = Clock::now(); ++sink_->vm_builds; }
    }
    ~VmProfileProbe() {
        if (!sink_) return;
        Mark(VmPhase::Other);
        sink_->vm_ms += std::chrono::duration<double, std::milli>(last_ - first_).count();
    }
    void Mark(VmPhase phase) {
        if (!sink_) return;
        const auto now = Clock::now();
        sink_->vm_milliseconds[static_cast<size_t>(phase)] +=
            std::chrono::duration<double, std::milli>(now - last_).count();
        last_ = now;
    }
private:
    using Clock = std::chrono::steady_clock;
    FrameProfileSample* sink_;
    Clock::time_point first_{}, last_{};
};

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
