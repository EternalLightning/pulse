// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "../app/frame_profile.h"
#include <dxgi1_4.h>
#include <psapi.h>

namespace pulse::bench {

using ProfileClock = std::chrono::steady_clock;
inline constexpr const char* vm_csv_columns =
    "vm_ms,vm_base_ms,vm_commands_ms,vm_operations_ms,vm_sidebar_ms,vm_tabs_ms,"
    "vm_status_ms,vm_tray_ms,vm_details_ms,vm_panes_ms,vm_other_ms,vm_builds";

inline void PrintVmProfile(const app::FrameProfileSample& sample) {
    std::printf(",%.6f", sample.vm_ms);
    for (const double value : sample.vm_milliseconds) std::printf(",%.6f", value);
    std::printf(",%u", sample.vm_builds);
}

inline constexpr const char* paint_csv_columns =
    "paint_setup_ms,paint_backdrop_ms,paint_title_ms,paint_toolbar_ms,paint_sidebar_ms,"
    "paint_pane_ms,paint_details_ms,paint_settings_ms,paint_status_ms,paint_overlays_ms,"
    "paint_other_ms,paint_list_setup_ms,paint_list_rows_ms,paint_list_layout_ms,paint_list_slots_ms,"
    "paint_list_motion_ms,paint_list_columns_ms";

inline void PrintPaintProfile(const app::FrameProfileSample& sample) {
    for (const double value : sample.paint_milliseconds) std::printf(",%.6f", value);
}

struct ProfileMemory {
    struct Row {
        double time_ms;
        unsigned long long working_set, private_bytes;
        long long local_usage = -1, local_budget = -1, nonlocal_usage = -1, nonlocal_budget = -1;
        const char* scenario;
        int repeat;
    };
    ProfileClock::time_point started = ProfileClock::now(), previous = started;
    Microsoft::WRL::ComPtr<IDXGIAdapter3> adapter;
    std::vector<Row> rows;
    bool sampled = false;
    int failures = 0;

    void Sample(const char* scenario, int repeat) {
        const auto now = ProfileClock::now();
        if (sampled && now - previous < std::chrono::seconds(1)) return;
        previous = now;
        sampled = true;
        PROCESS_MEMORY_COUNTERS_EX process{};
        process.cb = sizeof(process);
        if (!K32GetProcessMemoryInfo(GetCurrentProcess(),
            reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&process), sizeof(process))) {
            std::printf("[FAIL] process memory query error=%lu\n", GetLastError());
            ++failures;
            return;
        }
        Row row{std::chrono::duration<double, std::milli>(now - started).count(),
            static_cast<unsigned long long>(process.WorkingSetSize),
            static_cast<unsigned long long>(process.PrivateUsage), -1, -1, -1, -1, scenario, repeat};
        if (adapter) {
            DXGI_QUERY_VIDEO_MEMORY_INFO info{};
            if (SUCCEEDED(adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info))) {
                row.local_usage = static_cast<long long>(info.CurrentUsage);
                row.local_budget = static_cast<long long>(info.Budget);
            }
            if (SUCCEEDED(adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &info))) {
                row.nonlocal_usage = static_cast<long long>(info.CurrentUsage);
                row.nonlocal_budget = static_cast<long long>(info.Budget);
            }
        }
        rows.push_back(row);
    }
    void Print() const {
        std::printf("MEMORY_CSV,time_ms,working_set_bytes,private_bytes,gpu_local_usage_bytes,"
            "gpu_local_budget_bytes,gpu_nonlocal_usage_bytes,gpu_nonlocal_budget_bytes,scenario,repeat\n");
        for (const auto& row : rows)
            std::printf("MEMORY_CSV,%.3f,%llu,%llu,%lld,%lld,%lld,%lld,%s,%d\n", row.time_ms,
                row.working_set, row.private_bytes, row.local_usage, row.local_budget,
                row.nonlocal_usage, row.nonlocal_budget, row.scenario, row.repeat);
    }
};

inline void FillProfileSidebar(AppState& s, int count) {
    for (int i = 0; i < count; ++i) {
        app::SidebarEntry entry;
        entry.label = L"Profile folder " + std::to_wstring(i);
        entry.path = L"C:\\pulse-frame-profile\\sidebar-" + std::to_wstring(i);
        s.sidebar.quick_access.push_back(std::move(entry));
    }
}

inline void FillProfileTray(AppState& s, int count) {
    std::vector<std::wstring> paths;
    for (int i = 0; i < count; ++i)
        paths.push_back(L"C:\\pulse-frame-profile\\tray-" + std::to_wstring(i));
    s.tray.Collect(paths, false);
    std::vector<app::TrayMetadataResult> metadata;
    for (auto& request : s.tray.BeginMetadataQueries(static_cast<size_t>(count)))
        metadata.push_back({std::move(request), app::TrayMetadataState::Ready, FILE_ATTRIBUTE_DIRECTORY, 0});
    s.tray.ApplyMetadataResults(metadata);
}

} // namespace pulse::bench
