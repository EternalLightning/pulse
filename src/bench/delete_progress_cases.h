#pragma once

#include "../ops/ops_manager.h"
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <vector>

namespace pulse::ops {

inline int RunDeleteProgressRegressions(const std::wstring& root) {
    int failures = 0;
    const auto check = [&](bool ok, const char* text) {
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", text);
        failures += !ok;
    };
    // A selected recycle row owns a recursive $R payload and its $I metadata.
    // Keep both under bench_data; the user's actual Recycle Bin is untouched.
    const auto payload = root + L"\\$Rprogress";
    const auto metadata = root + L"\\$Iprogress";
    std::error_code error;
    std::filesystem::create_directories(payload + L"\\nested", error);
    bool prepared = !error;
    for (int index = 0; prepared && index <= 1500; ++index) {
        const auto path = index == 1500 ? metadata
            : payload + L"\\nested\\file-" + std::to_wstring(index);
        HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, 0, nullptr);
        prepared = file != INVALID_HANDLE_VALUE;
        if (prepared) CloseHandle(file);
    }
    check(prepared, "prepare isolated recycle-shaped directory with 1500 children");
    if (!prepared) return 1;
    HWND owner = CreateWindowExW(0, L"STATIC", L"Delete progress fixture", 0,
        0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr);
    check(owner != nullptr, "create isolated confirmation owner");
    if (!owner) return 1;
    OpsManager manager;
    manager.SetUiWindow(owner);
    std::mutex mutex;
    std::vector<OpStatus> samples;
    manager.Start([&] {
        if (const auto pending = manager.PendingDeleteConfirmation())
            manager.ResolveDeleteConfirmation(pending->token, true);
        const auto status = manager.Status();
        std::lock_guard lock(mutex);
        samples.push_back(status);
    });
    OpRequest request;
    request.type = OpType::RealDelete;
    request.delete_targets.push_back({root + L"\\original-folder", {payload}, true});
    const auto id = manager.Submit(std::move(request));
    const auto deadline = GetTickCount64() + 60000;
    while (manager.Status().completed_ops == 0 && GetTickCount64() < deadline) Sleep(5);
    const auto result = manager.Status();
    manager.Stop();
    DestroyWindow(owner);
    check(id != 0 && result.phase == OpPhase::Completed && result.completed_items == 1 &&
        !std::filesystem::exists(payload) && !std::filesystem::exists(metadata),
        "permanent deletion completes one logical row and both physical roots");
    bool bounded = true, detail_valid = true;
    size_t running_samples = 0, measured_samples = 0;
    for (const auto& status : samples) {
        if (status.task_id != id || status.phase != OpPhase::Running) continue;
        ++running_samples;
        bounded = bounded && std::isfinite(status.percent) &&
            (status.percent == -1.0f || (status.percent >= 0.0f && status.percent <= 100.0f));
        if (status.percent >= 0.0f) ++measured_samples;
        const auto begin = status.summary.find(L"  (");
        if (begin != std::wstring::npos) {
            const float displayed = std::wcstof(status.summary.c_str() + begin + 3, nullptr);
            detail_valid = detail_valid && displayed >= 0.0f && displayed <= 100.0f;
        }
    }
    std::printf("[INFO] running samples=%zu, measured samples=%zu, hr=0x%08lX\n",
        running_samples, measured_samples, static_cast<unsigned long>(result.failure_hr));
    check(running_samples > 0 && measured_samples > 0 && bounded && detail_valid,
        "recursive multi-root Shell deletion keeps status and blue detail text within 0..100 percent");
    if (failures == 0) {
        std::filesystem::remove(root, error);
        check(!error, "remove empty isolated fixture root");
    }
    return failures ? 1 : 0;
}

} // namespace pulse::ops
