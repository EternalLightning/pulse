#include "../common/windows_compat.h"
#include "../model/batch_rename_preview.h"
#include <chrono>
#include <cstdio>
#include <future>

namespace {
int failures = 0;
void Check(bool ok, const char* name) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++failures;
}
auto WaitResult(pulse::model::BatchRenamePreview& preview) {
    std::optional<std::vector<pulse::model::BatchRenameItem>> result;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!(result = preview.Take()) && std::chrono::steady_clock::now() < deadline)
        Sleep(1);
    return result;
}
}

int main() {
    using namespace pulse::model;
    BatchRenameRule rule;
    rule.prefix = L"new-";
    std::promise<void> entered, release, finished;
    auto blocked = release.get_future().share();
    std::atomic<int> probes{0};
    BatchRenamePreview preview([&](const std::wstring&) {
        if (++probes == 1) { entered.set_value(); blocked.wait(); }
        return false;
    });
    std::vector<std::wstring> large_selection;
    for (int i = 0; i < 10000; ++i)
        large_selection.push_back(L"C:\\fixture\\file-" + std::to_wstring(i));
    const auto begin = std::chrono::steady_clock::now();
    preview.Request(std::move(large_selection), rule);
    const auto request_us = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - begin).count();
    std::printf("[INFO] 10000-item request returned in %lld us; provider held by gate\n",
        static_cast<long long>(request_us));
    Check(entered.get_future().wait_for(std::chrono::seconds(5)) == std::future_status::ready,
        "controlled slow filesystem probe entered on worker");
    Check(std::chrono::steady_clock::now() - begin < std::chrono::seconds(2) && !preview.Take(),
        "request returns while filesystem probe is blocked; no partial plan");
    rule.prefix = L"latest-";
    preview.Request({L"C:\\fixture\\latest"}, rule);
    release.set_value();
    const auto result = WaitResult(preview);
    Check(result && result->size() == 1 && result->front().new_name == L"latest-latest",
        "new generation supersedes blocked preview");
    Check(probes == 2, "obsolete preview cancels before probing second item");
    preview.Close();

    std::promise<void> closing_entered, closing_release;
    auto closing_gate = closing_release.get_future().share();
    auto completed = finished.get_future();
    auto delayed = std::make_unique<BatchRenamePreview>([&](const std::wstring&) {
        closing_entered.set_value();
        closing_gate.wait();
        finished.set_value();
        return true;
    });
    delayed->Request({L"C:\\fixture\\a"}, rule);
    Check(closing_entered.get_future().wait_for(std::chrono::seconds(5)) == std::future_status::ready,
        "close fixture enters blocked provider");
    const auto close_begin = std::chrono::steady_clock::now();
    delayed.reset();
    std::printf("[INFO] blocked-provider preview destruction returned in %lld us\n",
        static_cast<long long>(std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - close_begin).count()));
    Check(std::chrono::steady_clock::now() - close_begin < std::chrono::seconds(1),
        "dialog preview destruction does not wait for blocked provider");
    closing_release.set_value();
    Check(completed.wait_for(std::chrono::seconds(5)) == std::future_status::ready,
        "worker safely finishes after preview owner destruction");

    auto conflicts = PreviewBatchRename({L"C:\\fixture\\a", L"C:\\fixture\\b"}, rule,
        {}, [](const std::wstring&) { return true; });
    Check(conflicts.size() == 2 && conflicts[0].status == BatchRenameStatus::Collision &&
        conflicts[1].status == BatchRenameStatus::Collision, "existing destinations remain collisions");
    rule.pattern = L"same";
    int duplicate_probes = 0;
    auto duplicates = PreviewBatchRename({L"C:\\fixture\\a", L"C:\\fixture\\b"}, rule,
        {}, [&](const std::wstring&) { ++duplicate_probes; return false; });
    Check(duplicates.size() == 2 && duplicates[0].status == BatchRenameStatus::Collision &&
        duplicates[1].status == BatchRenameStatus::Collision && duplicate_probes == 0,
        "duplicate plan destinations reject without filesystem probes");
    return failures ? 1 : 0;
}
