// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "../ui/ui_compositor.h"
#include <array>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace pulse::bench {

enum class PresentStatisticsState : size_t {
    Unsupported, Error, Disjoint, Zero, Incomplete, Baseline, Unchanged, Advanced, Reset, Count
};
inline constexpr const char* present_statistics_states[] = {
    "unsupported", "error", "disjoint", "zero", "incomplete", "baseline", "unchanged", "advanced", "reset"
};

class PresentStatisticsSequence {
public:
    PresentStatisticsState Observe(HRESULT result, const DXGI_FRAME_STATISTICS& value) {
        using State = PresentStatisticsState;
        if (result != S_OK) {
            previous_valid_ = false;
            if (result == DXGI_ERROR_FRAME_STATISTICS_DISJOINT) { ++epoch_; return State::Disjoint; }
            if (result == DXGI_ERROR_UNSUPPORTED || result == E_NOINTERFACE || result == E_NOTIMPL)
                return State::Unsupported;
            return State::Error;
        }
        if (!value.PresentCount || !value.PresentRefreshCount || !value.SyncRefreshCount || value.SyncQPCTime.QuadPart <= 0) {
            previous_valid_ = false;
            return !value.PresentCount && !value.PresentRefreshCount && !value.SyncRefreshCount &&
                !value.SyncQPCTime.QuadPart ? State::Zero : State::Incomplete;
        }
        State state = State::Baseline;
        if (previous_valid_) {
            if (value.PresentCount < previous_.PresentCount || value.PresentRefreshCount < previous_.PresentRefreshCount ||
                value.SyncRefreshCount < previous_.SyncRefreshCount || value.SyncQPCTime.QuadPart < previous_.SyncQPCTime.QuadPart) {
                ++epoch_;
                state = State::Reset;
            } else state = value.PresentCount == previous_.PresentCount ? State::Unchanged : State::Advanced;
        }
        previous_ = value;
        previous_valid_ = true;
        return state;
    }
    uint64_t Epoch() const { return epoch_; }
private:
    DXGI_FRAME_STATISTICS previous_{};
    uint64_t epoch_ = 0;
    bool previous_valid_ = false;
};

class PresentationStatisticsProfile {
public:
    static bool Requested() {
        wchar_t value[2]{};
        return GetEnvironmentVariableW(L"PULSE_PROFILE_PRESENT_STATISTICS", value, 2) == 1 && value[0] == L'1';
    }
    void Start(const ui::Compositor& compositor) {
        rows_.reserve(16000);
        LARGE_INTEGER frequency{};
        QueryPerformanceFrequency(&frequency);
        Sample(compositor, "initial", -1, -1, -1, -1, false, false);
        const auto& snapshot = rows_.back().snapshot;
        std::printf("[PRESENT_INFO] qpc_frequency=%lld composition=%d description_hr=%08lx swap_effect=%u flags=%u "
            "limit=asynchronous-snapshots-not-display-intervals-or-drop-counts no_flush=1\n",
            frequency.QuadPart, compositor.UsesTransparentComposition(),
            static_cast<unsigned long>(snapshot.description_result), static_cast<unsigned>(snapshot.description.SwapEffect),
            snapshot.description.Flags);
    }
    void Sample(const ui::Compositor& compositor, const char* kind, int64_t render_id, int scenario, int repeat,
                int frame, bool measurement, bool cpu_presented) {
        Row row{};
        row.kind = kind; row.render_id = render_id; row.scenario = scenario; row.repeat = repeat; row.frame = frame;
        row.measurement = measurement; row.cpu_presented = cpu_presented;
        QueryPerformanceCounter(&row.sample_qpc);
        row.snapshot = compositor.ReadPresentationStatistics();
        row.frame_state = frame_sequence_.Observe(row.snapshot.frame_result, row.snapshot.frame);
        row.frame_epoch = frame_sequence_.Epoch();
        const auto& media = row.snapshot.media;
        const DXGI_FRAME_STATISTICS media_frame{media.PresentCount, media.PresentRefreshCount,
            media.SyncRefreshCount, media.SyncQPCTime, media.SyncGPUTime};
        row.media_state = media_sequence_.Observe(row.snapshot.media_result, media_frame);
        row.media_epoch = media_sequence_.Epoch();
        rows_.push_back(row);
    }
    void Print(const char* const* scenarios, size_t scenario_count) const {
        std::printf("PRESENT_CSV,sample_id,kind,render_id,scenario,repeat,frame,measurement,cpu_presented,sample_qpc,"
            "last_present_hr,last_present_id,frame_hr,frame_state,frame_epoch,present_id,present_refresh_count,"
            "sync_refresh_count,sync_qpc,sync_gpu_time,media_qi_hr,media_hr,media_state,media_epoch,media_present_id,"
            "media_present_refresh_count,media_sync_refresh_count,media_sync_qpc,media_composition_mode,media_approved_present_duration\n");
        for (size_t i = 0; i < rows_.size(); ++i) {
            const auto& row = rows_[i];
            const auto& snapshot = row.snapshot;
            const auto& stats = snapshot.frame;
            const auto& media = snapshot.media;
            std::printf("PRESENT_CSV,%zu,%s,%lld,%s,%d,%d,%d,%d,%lld,%08lx,%u,%08lx,%s,%llu,%u,%u,%u,%lld,%lld,"
                "%08lx,%08lx,%s,%llu,%u,%u,%u,%lld,%u,%u\n", i, row.kind, static_cast<long long>(row.render_id),
                row.scenario < 0 ? "boundary" : scenarios[row.scenario], row.repeat, row.frame, row.measurement,
                row.cpu_presented, row.sample_qpc.QuadPart, static_cast<unsigned long>(snapshot.last_present_result),
                snapshot.last_present_count, static_cast<unsigned long>(snapshot.frame_result), Name(row.frame_state),
                static_cast<unsigned long long>(row.frame_epoch), stats.PresentCount, stats.PresentRefreshCount,
                stats.SyncRefreshCount, stats.SyncQPCTime.QuadPart, stats.SyncGPUTime.QuadPart,
                static_cast<unsigned long>(snapshot.media_interface_result), static_cast<unsigned long>(snapshot.media_result),
                Name(row.media_state), static_cast<unsigned long long>(row.media_epoch), media.PresentCount,
                media.PresentRefreshCount, media.SyncRefreshCount, media.SyncQPCTime.QuadPart,
                static_cast<unsigned>(media.CompositionMode), media.ApprovedPresentDuration);
        }
        for (size_t scenario = 0; scenario < scenario_count; ++scenario) {
            std::array<size_t, static_cast<size_t>(PresentStatisticsState::Count)> frame{}, media{};
            size_t samples = 0;
            for (const auto& row : rows_) if (row.measurement && row.scenario == static_cast<int>(scenario)) {
                ++samples;
                ++frame[static_cast<size_t>(row.frame_state)];
                ++media[static_cast<size_t>(row.media_state)];
            }
            const auto valid = [](const auto& counts) {
                return counts[static_cast<size_t>(PresentStatisticsState::Baseline)] +
                    counts[static_cast<size_t>(PresentStatisticsState::Unchanged)] +
                    counts[static_cast<size_t>(PresentStatisticsState::Advanced)] +
                    counts[static_cast<size_t>(PresentStatisticsState::Reset)];
            };
            std::printf("[PRESENT_PROFILE] scenario=%s samples=%zu frame_valid=%zu media_valid=%zu",
                scenarios[scenario], samples, valid(frame), valid(media));
            for (size_t state = 0; state < frame.size(); ++state)
                std::printf(" frame_%s=%zu media_%s=%zu", present_statistics_states[state], frame[state],
                    present_statistics_states[state], media[state]);
            std::printf("\n");
        }
    }
private:
    static const char* Name(PresentStatisticsState state) { return present_statistics_states[static_cast<size_t>(state)]; }
    struct Row {
        const char* kind = nullptr;
        int64_t render_id = -1;
        int scenario = -1, repeat = -1, frame = -1;
        bool measurement = false, cpu_presented = false;
        LARGE_INTEGER sample_qpc{};
        ui::PresentationStatistics snapshot;
        PresentStatisticsState frame_state{}, media_state{};
        uint64_t frame_epoch = 0, media_epoch = 0;
    };
    std::vector<Row> rows_;
    PresentStatisticsSequence frame_sequence_, media_sequence_;
};

} // namespace pulse::bench
