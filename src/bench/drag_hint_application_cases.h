#pragma once
#include "../app/app_internal.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

namespace pulse::bench::drag_hint {
struct ProviderGate {
    struct State {
        std::mutex mutex;
        std::condition_variable changed;
        size_t entered = 0, finished = 0;
        bool release = false;
    };
    std::shared_ptr<State> state = std::make_shared<State>();
    size_t owners = 0;
    bool Block(app::WorkerPool& pool) {
        owners = app::WorkerCancellationTestProbe::ProviderOwners(pool);
        bool accepted = owners != 0;
        for (size_t i = 0; i < owners; ++i) accepted &= pool.EnqueueIo([shared = state] {
            std::unique_lock lock(shared->mutex);
            ++shared->entered; shared->changed.notify_all();
            shared->changed.wait(lock, [&] { return shared->release; });
            ++shared->finished; shared->changed.notify_all();
        });
        std::unique_lock lock(state->mutex);
        return accepted && state->changed.wait_for(lock, std::chrono::seconds(5), [&] { return state->entered == owners; });
    }
    void Release() {
        { std::lock_guard lock(state->mutex); state->release = true; }
        state->changed.notify_all();
    }
    ~ProviderGate() { Release(); }
};
inline bool Wait(const std::function<bool()>& condition) {
    const auto deadline = GetTickCount64() + 5000;
    do {
        if (condition()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    } while (GetTickCount64() < deadline);
    return false;
}
inline size_t DrainMessages(AppState& s, bool deliver = false) {
    size_t count = 0;
    MSG message{};
    while (PeekMessageW(&message, s.hwnd, WM_HEADER_DROP_RESULT, WM_HEADER_DROP_RESULT, PM_REMOVE)) {
        ++count;
        if (deliver) HandleHeaderDropResult(s);
    }
    return count;
}
}

inline int RunDragHintApplicationCases(pulse::AppState& s) {
    using namespace pulse;
    using namespace pulse::bench::drag_hint;
    int failures = 0;
    const auto check = [&](bool ok, const char* label) {
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
        failures += !ok;
    };
    const auto root = std::filesystem::current_path() / L"bench_data" /
        (L"drag-hint-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    std::filesystem::create_directories(root / L"folder");
    std::filesystem::create_directories(root / L"replacement");
    const auto file = (root / L"ordinary.txt").wstring();
    const auto link = (root / L"unresolved.lnk").wstring();
    const auto folder = (root / L"folder").wstring();
    const auto replacement = (root / L"replacement").wstring();
    { std::ofstream stream(file); stream << "ordinary"; }
    { std::ofstream stream(link); stream << "not a real Shell link"; }
    const std::vector<std::wstring> sources{file, folder};
    const auto ready = [&] {
        std::lock_guard lock(s.header_drop_query->mutex);
        return s.header_drop_query->hint_ready;
    };
    check(s.worker.Start([](app::WorkResult) {}), "isolated header hint worker starts");
    {
        ProviderGate gate;
        check(gate.Block(s.worker), "all metadata provider owners pause before header hover");
        check(HeaderDropHint(s, sources) == file && !ready(),
              "pending folder metadata immediately yields the cheap first-source hint");
        uint64_t generation = 0;
        { std::lock_guard lock(s.header_drop_query->mutex); generation = s.header_drop_query->hint_generation; }
        bool stable = true;
        for (int i = 0; i < 8; ++i) stable &= HeaderDropHint(s, sources) == file;
        {
            std::lock_guard lock(s.header_drop_query->mutex);
            stable &= !s.header_drop_query->hint_ready && s.header_drop_query->hint_generation == generation;
        }
        check(stable, "repeated pending hover keeps one hint generation");
        size_t overs = 0;
        std::wstring observed_hint;
        POINT observed_point{};
        DWORD observed_keys = 0, observed_allowed = 0;
        ui::DropTargetCallbacks callbacks;
        callbacks.drag_over = [&](const auto& paths, POINT point, DWORD keys, DWORD allowed, DWORD) {
            ++overs; observed_hint = HeaderDropHint(s, paths); observed_point = point;
            observed_keys = keys; observed_allowed = allowed; return DROPEFFECT_COPY;
        };
        callbacks.drag_leave = [&] { ClearDropFeedback(s); };
        auto* target = new ui::WindowDropTarget(s.hwnd, std::move(callbacks));
        s.dropTarget = target;
        auto* data = ui::FileDataObject::Create(sources);
        POINT initial{10, 10}, latest{23, 31};
        ClientToScreen(s.hwnd, &initial); ClientToScreen(s.hwnd, &latest);
        DWORD effect = DROPEFFECT_COPY | DROPEFFECT_MOVE;
        target->DragEnter(data, 0, {initial.x, initial.y}, &effect);
        effect = DROPEFFECT_COPY;
        target->DragOver(MK_CONTROL, {latest.x, latest.y}, &effect);
        const auto before_completion = overs;
        gate.Release();
        check(Wait(ready) && HeaderDropHint(s, sources) == folder,
              "background metadata replaces the cheap file hint with the actual folder");
        check(DrainMessages(s, true) == 1 && overs == before_completion + 1 && observed_hint == folder &&
              observed_point.x == 23 && observed_point.y == 31 && observed_keys == MK_CONTROL &&
              observed_allowed == DROPEFFECT_COPY,
              "one metadata completion refreshes feedback using the latest active hover context");
        target->DragLeave();
        s.dropTarget = nullptr;
        target->Release(); data->Release();
    }
    for (int scenario = 0; scenario < 3; ++scenario) {
        ClearHeaderDropHint(s); DrainMessages(s);
        ProviderGate gate;
        check(gate.Block(s.worker), "provider owners pause before obsolete hint scenarios");
        HeaderDropHint(s, sources);
        uint64_t old_generation = 0;
        { std::lock_guard lock(s.header_drop_query->mutex); old_generation = s.header_drop_query->hint_generation; }
        const std::vector<std::wstring> next{file, replacement};
        if (scenario == 0) ClearDropFeedback(s);
        else {
            if (scenario == 2) ClearHeaderDropHint(s);
            HeaderDropHint(s, next);
        }
        gate.Release();
        if (scenario != 0) check(Wait(ready) && HeaderDropHint(s, next) == replacement,
                               "new sources obtain their own completed metadata hint");
        if (scenario == 0) {
            ProviderGate drained;
            check(drained.Block(s.worker), "cancelled hint job drains before worker shutdown can suppress its result");
            drained.Release();
        }
        s.worker.Stop();
        {
            std::lock_guard lock(s.header_drop_query->mutex);
            check(s.header_drop_query->hint_generation > old_generation &&
                  (scenario == 0 ? s.header_drop_query->hint_sources.empty() &&
                    !s.header_drop_query->hint_ready && s.header_drop_query->folder_hint.empty() :
                    s.header_drop_query->hint_sources == next && s.header_drop_query->folder_hint == replacement),
                  scenario == 0 ? "cancelled hover cannot restore its cleared hint" :
                  scenario == 1 ? "old source metadata cannot overwrite a replacement hint" :
                                  "reset invalidates old metadata before the next hover session");
        }
        check(DrainMessages(s) == (scenario == 0 ? 0u : 1u),
              "obsolete hint jobs never emit feedback completion messages");
        check(s.worker.Start([](app::WorkResult) {}), "header hint worker restarts between isolated races");
    }
    ClearHeaderDropHint(s);
    HeaderDropHint(s, {file});
    check(Wait(ready) && HeaderDropHint(s, {file}).empty(),
          "completed ordinary-file metadata clears the navigation hint");
    DrainMessages(s);
    ClearHeaderDropHint(s);
    HeaderDropHint(s, {file, link});
    check(Wait(ready) && HeaderDropHint(s, {file, link}) == link,
          "no-directory metadata retains the cheap shortcut candidate for final validation");
    DrainMessages(s);
    ClearHeaderDropHint(s);
    HeaderDropHint(s, {folder});
    check(Wait(ready) && HeaderDropHint(s, {folder}) == folder, "header folder hint is ready before the final drop");
    DrainMessages(s);
    {
        ProviderGate gate;
        check(gate.Block(s.worker), "provider owners pause while final drop metadata is queued");
        auto* tab = ActiveTab(s);
        tab->current_path = fs::NormalizePath(root.wstring());
        const auto original_path = tab->current_path;
        const auto vm = BuildVm(s);
        const auto rect = vm.pane_slots.empty()
            ? s.renderer.ContentRect(static_cast<float>(s.compositor.Width()), static_cast<float>(s.compositor.Height()))
            : vm.pane_slots.front().rect;
        POINT point{static_cast<LONG>(rect.left + 8), static_cast<LONG>(rect.top + s.renderer.PaneHeaderHeight() * 0.5f)};
        s.trayDrag.active = true;
        const auto effect = DropExecute(s, {folder}, point, 0, DROPEFFECT_LINK, DROPEFFECT_LINK);
        s.trayDrag.active = false;
        check(effect == DROPEFFECT_LINK, "real pane-header drop queues final validation without reporting a copy");
        std::error_code error;
        std::filesystem::remove(std::filesystem::path(folder), error);
        check(!error && !std::filesystem::exists(folder), "dropped fixture directory disappears before final metadata validation");
        gate.Release();
        const bool completed = Wait([&] {
            std::lock_guard lock(s.header_drop_query->mutex);
            return s.header_drop_query->result.has_value();
        });
        bool rejected_directory = false;
        {
            std::lock_guard lock(s.header_drop_query->mutex);
            if (s.header_drop_query->result) rejected_directory = s.header_drop_query->result->folder.empty() &&
                s.header_drop_query->result->accepted_effect == DROPEFFECT_LINK;
        }
        check(completed && rejected_directory, "final metadata refuses the deleted directory instead of navigating to stale hint facts");
        DrainMessages(s, true);
        check(tab->current_path == original_path && s.notification_toast.IsVisible() &&
              !std::filesystem::exists(root / L"folder"),
              "deleted header target produces the failure UI without navigation or a claimed copy fallback");
    }
    s.worker.Stop();
    ClearHeaderDropHint(s);
    const std::vector<std::wstring> retry_sources{file, replacement};
    const auto refused_hint = HeaderDropHint(s, retry_sources);
    {
        std::lock_guard lock(s.header_drop_query->mutex);
        check(refused_hint == file && s.header_drop_query->hint_sources.empty() &&
              !s.header_drop_query->hint_ready,
              "rejected metadata admission releases source ownership and retains only the cheap navigation candidate");
    }
    check(s.worker.Start([](app::WorkResult) {}), "metadata worker restarts after refused hint admission");
    HeaderDropHint(s, retry_sources);
    check(Wait(ready) && HeaderDropHint(s, retry_sources) == replacement,
          "a later hover retries refused metadata and obtains a real folder hint");
    s.worker.Stop();
    ClearDropFeedback(s); DrainMessages(s);
    std::error_code cleanup;
    std::filesystem::remove_all(root, cleanup);
    check(!cleanup && !std::filesystem::exists(root), "workers stop before the isolated drag-hint fixture is removed");
    return failures ? 1 : 0;
}
