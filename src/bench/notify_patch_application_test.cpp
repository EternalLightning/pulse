#include "../app/app_internal.h"
#include "../app/app_column_view.h"
#include "../common/localization.h"
#include "../app/directory_projection.h"
#include <chrono>
#include <thread>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>

int main(int argc, char** argv) {
    using namespace pulse;
    OleInitialize(nullptr);
    l10n::Initialize(GetModuleHandleW(nullptr), L"en-US");
    int failures = 0;
    const auto check = [&](bool passed, const char* label) {
        std::printf("[%s] %s\n", passed ? "PASS" : "FAIL", label);
        failures += !passed;
    };
    auto owned = std::make_unique<AppState>();
    auto& s = *owned;
    s.isolatedTest = true;
    s.appPrefs.persist = s.places.persist = false;
    s.hwnd = CreateWindowExW(WS_EX_NOACTIVATE, L"STATIC", L"Notify patch fixture",
        WS_OVERLAPPEDWINDOW, -20000, -20000, 1000, 700, nullptr, nullptr,
        GetModuleHandleW(nullptr), nullptr);
    if (!s.hwnd || !s.compositor.Init(s.hwnd)) {
        std::puts("[FAIL] initialize isolated notification compositor");
        if (s.hwnd) DestroyWindow(s.hwnd);
        s.hwnd = nullptr;
        owned.reset(); OleUninitialize();
        return 2;
    }
    s.renderer.SetCompositor(&s.compositor);
    s.window_tabs.EnsureDefault();
    s.pane = s.window_tabs.Active()->FocusedPane();
    auto& first = *ActiveTab(s);
    auto pane = std::make_unique<app::Pane>();
    auto& second = pane->view;
    s.window_tabs.Active()->panes.push_back(std::move(pane));
    app::RebuildLayoutRoot(*s.window_tabs.Active());
    wchar_t cwd[MAX_PATH]{};
    GetCurrentDirectoryW(MAX_PATH, cwd);
    const auto path = fs::NormalizePath(std::wstring(cwd) + L"\\bench_data\\notify-patch-" +
        std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    const auto added = path + L"\\b.txt";
    const bool created = CreateDirectoryW(path.c_str(), nullptr) != FALSE;
    HANDLE file = created ? CreateFileW(added.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL, nullptr) : INVALID_HANDLE_VALUE;
    check(file != INVALID_HANDLE_VALUE, "create isolated notification fixture");
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    const auto listing = [] {
        auto rows = std::make_shared<std::vector<fs::DirEntry>>(1);
        rows->front().name = L"a.txt";
        rows->front().attrs = FILE_ATTRIBUTE_NORMAL;
        return rows;
    };
    const auto reset = [&] {
        s.worker.Stop();
        s.notify_patches.pending.clear();
        {
            std::lock_guard lock(s.notify_patches.mailbox->mutex);
            s.notify_patches.mailbox->completed.clear();
        }
        s.explicit_entry_refreshes.clear();
        first.current_path = second.current_path = path;
        first.pending_generation = second.pending_generation = 0;
        first.applied_generation = second.applied_generation = 0;
        first.loading = second.loading = false;
        first.sort_direction = second.sort_direction = ui::SortDirection::Asc;
        first.SetSnapshot(listing()); second.SetSnapshot(first.snapshot);
        first.SelectOnly(0); second.SelectOnly(0);
        s.store.Put(path, first.snapshot);
        s.worker.Start([](app::WorkResult) {});
    };
    const auto wait_result = [&] {
        const auto deadline = GetTickCount64() + 5000;
        do {
            {
                std::lock_guard lock(s.notify_patches.mailbox->mutex);
                if (!s.notify_patches.mailbox->completed.empty()) return true;
            }
            Sleep(5);
        } while (GetTickCount64() < deadline);
        return false;
    };
    const bool projection_only = argc == 2 && strcmp(argv[1], "--projection-only") == 0;
    const bool case_names_only = argc == 2 && strcmp(argv[1], "--case-sensitive-names-only") == 0;
    if (case_names_only) {
        s.worker.Stop();
        s.worker.Start([](app::WorkResult) {});
        const auto entry = [](const wchar_t* name, uint64_t size) {
            fs::DirEntry value;
            value.name = name; value.size = size; value.attrs = FILE_ATTRIBUTE_NORMAL;
            return value;
        };
        const auto prepare = [&](std::vector<fs::DirEntry> entries) {
            s.notify_patches.pending.clear();
            s.explicit_entry_refreshes.clear();
            first.current_path = second.current_path = path;
            first.pending_generation = second.pending_generation = 0;
            first.loading = second.loading = false;
            first.sort_direction = second.sort_direction = ui::SortDirection::Asc;
            first.SetSnapshot(std::make_shared<std::vector<fs::DirEntry>>(std::move(entries)));
            second.SetSnapshot(first.snapshot);
            first.SelectOnly(0); second.SelectOnly(0);
            s.store.Put(path, first.snapshot);
        };
        const auto publish = [&](fs::DirNotifyEvent event, fs::DirEntry fact) {
            auto request = std::make_shared<app::NotifyPatchRequest>();
            request->path = path;
            request->generation = ++s.notify_patches.generation;
            request->resolved = {app::NotifyPatch::Applied, {std::move(event)}, {std::move(fact)}};
            for (auto* tab : {&first, &second}) {
                tab->notify_patch_generation = request->generation;
                request->targets.push_back({tab, tab->snapshot, tab->view_generation,
                                            tab->applied_generation, tab->sort_column, tab->sort_direction});
            }
            request->folders = app::FolderSortModeFromInt(s.appPrefs.folder_sort_mode);
            app::BuildNotifyProjections(*request);
            s.notify_patches.pending.emplace(path, app::PendingNotifyPatch{request, {}});
            {
                std::lock_guard lock(s.notify_patches.mailbox->mutex);
                s.notify_patches.mailbox->completed.push_back(request);
            }
            DrainNotifyPatches(s);
        };
        prepare({entry(L"A.txt", 1), entry(L"a.txt", 2), entry(L"keep.txt", 3)});
        second.SelectOnly(1);
        publish({FILE_ACTION_RENAMED_NEW_NAME, L"B.txt", L"A.txt"}, entry(L"B.txt", 10));
        check(first.EntryCount() == 3 && second.EntryCount() == 3 &&
              first.EntryAt(first.selected_index).name == L"B.txt" &&
              second.EntryAt(second.selected_index).name == L"a.txt" &&
              second.EntryAt(second.selected_index).size == 2,
              "notification publication renames the exact selection without moving sibling focus");
        prepare({entry(L"A.txt", 1), entry(L"a.txt", 2), entry(L"keep.txt", 3)});
        first.RemapSelection({L"A.txt", L"a.txt"}, L"a.txt");
        publish({FILE_ACTION_RENAMED_NEW_NAME, L"B.txt", L"A.txt"}, entry(L"B.txt", 10));
        check(first.SelectedCount() == 2 && first.EntryAt(first.selected_index).name == L"a.txt",
              "notification rename preserves both selected identities and lowercase focus");
        prepare({entry(L"A.txt", 1), entry(L"a.txt", 2), entry(L"keep.txt", 3)});
        const bool removed = QueueNotifiesToVisible(s, path, {{FILE_ACTION_REMOVED, L"a.txt", {}}}) && wait_result();
        DrainNotifyPatches(s);
        check(removed && first.EntryCount() == 2 && first.EntryAt(first.selected_index).name == L"A.txt" &&
              second.EntryAt(second.selected_index).name == L"A.txt",
              "background removal publication leaves the exact uppercase selection intact");
        prepare({entry(L"A.txt", 1), entry(L"keep.txt", 3)});
        publish({FILE_ACTION_RENAMED_NEW_NAME, L"a.txt", L"A.txt"}, entry(L"a.txt", 10));
        check(first.EntryCount() == 2 && first.EntryAt(first.selected_index).name == L"a.txt" &&
              second.EntryAt(second.selected_index).name == L"a.txt",
              "case-only notification rename carries both pane selections to the new physical name");
        s.size_patches.clear();
        const auto due = GetTickCount64() + 60000;
        QueueSizePatch(s, path, L"A.txt", due);
        QueueSizePatch(s, path, L"a.txt", due + 1);
        check(s.size_patches.size() == 2 && s.size_patches[0].name == L"A.txt" &&
              s.size_patches[1].name == L"a.txt",
              "single modification coalescing keeps distinct physical case names");
        s.size_patches.clear();
        prepare({entry(L"A.txt", 1), entry(L"a.txt", 2)});
        AppState::DirNotifyBatch batch;
        batch.path = path;
        for (int index = 0; index < 9; ++index)
            batch.events.push_back({FILE_ACTION_MODIFIED, index % 2 ? L"a.txt" : L"A.txt", {}});
        {
            std::lock_guard lock(s.notify_mu);
            s.notify_queue.push_back(std::move(batch));
        }
        DrainDirNotifies(s);
        check(s.size_patches.size() == 2 && s.size_patches[0].name == L"A.txt" &&
              s.size_patches[1].name == L"a.txt",
              "large modification burst coalescing keeps independent case identities");
        s.size_patches.clear();
    }
    if (file != INVALID_HANDLE_VALUE && projection_only) {
        constexpr size_t count = 50000;
        auto source = std::make_shared<std::vector<fs::DirEntry>>(count);
        for (size_t i = 0; i < count; ++i) {
            wchar_t name[32]{};
            swprintf_s(name, L"item-%05zu", count - i - 1);
            source->at(i).name = name;
            source->at(i).attrs = FILE_ATTRIBUTE_NORMAL;
        }
        std::vector<double> publish_ms;
        bool prepared = true, published = true;
        for (unsigned iteration = 0; iteration < 7; ++iteration) {
            reset();
            first.sort_column = second.sort_column = ui::SortColumn::Name;
            first.SetSnapshot(source); second.SetSnapshot(source);
            first.SelectOnly(1); second.SelectOnly(1);
            s.store.Put(path, source);
            const bool queued = QueueNotifiesToVisible(s, path, {{FILE_ACTION_REMOVED, L"item-00000", {}}});
            const bool completed = wait_result();
            {
                std::lock_guard lock(s.notify_patches.mailbox->mutex);
                if (s.notify_patches.mailbox->completed.empty()) prepared = false;
                else {
                    const auto& request = *s.notify_patches.mailbox->completed.front();
                    prepared &= request.projections.size() == 1 && request.projections.front().source == source &&
                        request.projections.front().snapshot->size() == count - 1 &&
                        request.projections.front().snapshot->front().name == L"item-00001";
                }
            }
            prepared &= queued && completed && first.snapshot == source && second.snapshot == source;
            const auto start = std::chrono::steady_clock::now();
            DrainNotifyPatches(s);
            publish_ms.push_back(std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - start).count());
            published &= first.snapshot == second.snapshot && first.snapshot != source &&
                first.EntryCount() == count - 1 && source->size() == count &&
                first.EntryAt(first.selected_index).name == L"item-49998";
        }
        check(prepared, "large removal builds one sorted shared projection before any UI publication");
        check(published, "UI publishes immutable prepared rows and preserves both pane selections");
        std::sort(publish_ms.begin(), publish_ms.end());
        std::printf("[METRIC] 50000 rows, 2 panes, 7 publishes: median %.3f ms, p95 %.3f ms\n",
            publish_ms[publish_ms.size() / 2], publish_ms.back());
        reset();
        const auto original = first.snapshot;
        const bool queued = QueueNotifiesToVisible(s, path, {{FILE_ACTION_REMOVED, L"a.txt", {}}}) && wait_result();
        second.sort_direction = ui::SortDirection::Desc;
        DrainNotifyPatches(s);
        check(queued && first.snapshot == original && second.snapshot == original && first.pending_generation,
            "changed projection settings reject the entire stale notification batch before swapping rows");
    }
    if (file != INVALID_HANDLE_VALUE && !case_names_only && !projection_only) {
        reset();
        const auto before_sort = first.snapshot;
        second.sort_direction = ui::SortDirection::Desc;
        check(QueueNotifiesToVisible(s, path, {{FILE_ACTION_ADDED, L"b.txt", {}}}) && wait_result(),
            "resolve addition for panes sharing facts with opposite sort directions");
        DrainNotifyPatches(s);
        check(first.EntryAt(0).name == L"a.txt" && second.EntryAt(0).name == L"b.txt" &&
            first.snapshot != second.snapshot && before_sort->size() == 1 &&
            first.EntryAt(first.selected_index).name == L"a.txt" &&
            second.EntryAt(second.selected_index).name == L"a.txt",
            "watch addition immediately sorts each pane and preserves existing selections");
        reset();
        std::mutex gate_mutex;
        std::condition_variable gate_changed;
        bool release = false;
        unsigned entered = 0;
        for (unsigned i = 0; i < 4; ++i) {
            s.worker.EnqueueIo([&] {
                std::unique_lock lock(gate_mutex);
                ++entered; gate_changed.notify_all();
                gate_changed.wait(lock, [&] { return release; });
            });
        }
        {
            std::unique_lock lock(gate_mutex);
            check(gate_changed.wait_for(lock, std::chrono::seconds(5), [&] { return entered >= 2; }),
                "existing I/O workers reach the gate");
        }
        const auto initial = first.snapshot;
        first.column_strip.child.path = path;
        first.column_strip.child.snapshot = initial;
        first.column_strip.child.requested = true;
        const auto old_column_generation = s.worker.Refresh(path, ui::SortColumn::Name, ui::SortDirection::Asc);
        const auto other_old_generation = s.worker.Refresh(path, ui::SortColumn::Size, ui::SortDirection::Asc);
        first.column_strip.child.generation = old_column_generation;
        first.column_strip.child.rows = std::make_shared<std::vector<int>>(1, 0);
        const bool queued = QueueNotifiesToVisible(s, path, {{FILE_ACTION_ADDED, L"b.txt", {}}});
        const auto pending = s.notify_patches.pending.find(path);
        check(queued && pending != s.notify_patches.pending.end() && first.snapshot == initial &&
              second.snapshot == initial && pending->second.request->resolved.events.empty(),
              "blocked worker leaves notification queries and pane publication pending without blocking UI");
        check(QueueNotifiesToVisible(s, path, {{FILE_ACTION_REMOVED, L"a.txt", {}}}) &&
              s.notify_patches.pending.at(path).queued.size() == 1,
              "a consecutive batch queues behind the same directory request");
        {
            std::lock_guard lock(gate_mutex);
            release = true;
        }
        gate_changed.notify_all();
        const bool completed = wait_result();
        DrainNotifyPatches(s);
        const bool removal_completed = wait_result();
        DrainNotifyPatches(s);
        check(completed && removal_completed && first.EntryCount() == 1 && first.EntryAt(0).name == L"b.txt" &&
              first.snapshot == second.snapshot && first.snapshot == s.store.Peek(path) &&
              initial->front().name == L"a.txt" && s.notify_patches.pending.empty(),
              "successive add and remove publish in order with one shared immutable pane listing");
        check(first.column_strip.child.snapshot == first.snapshot &&
              !first.column_strip.child.rows && first.column_strip.child.requested,
              "notification publication refreshes column facts and invalidates old source-index rows");
        const auto fresh_column = first.column_strip.child.snapshot;
        app::WorkResult stale_column{};
        stale_column.path = path;
        stale_column.generation = old_column_generation;
        stale_column.snapshot = initial;
        NoteColumnStripResult(s, stale_column);
        check(first.column_strip.child.snapshot == fresh_column &&
              first.column_strip.child.notify_watermark >= other_old_generation,
              "enumeration issued before notification publication cannot restore old column facts");
        stale_column.error = true;
        stale_column.generation = other_old_generation;
        const auto new_column_generation = s.worker.Refresh(path, ui::SortColumn::Name, ui::SortDirection::Asc);
        first.column_strip.child.generation = new_column_generation;
        NoteColumnStripResult(s, stale_column);
        check(!first.column_strip.child.error && first.column_strip.child.generation == new_column_generation,
              "old column error cannot invalidate a notification patch or consume a newer request");
        stale_column.generation = new_column_generation;
        stale_column.error = false;
        auto later_column = std::make_shared<std::vector<fs::DirEntry>>(*fresh_column);
        stale_column.snapshot = later_column;
        NoteColumnStripResult(s, stale_column);
        check(first.column_strip.child.snapshot == later_column &&
              first.column_strip.child.applied_generation == new_column_generation,
              "a column enumeration issued after notification publication can update its facts");

        reset();
        second.SetSnapshot(listing());
        auto other_rows = std::make_shared<std::vector<fs::DirEntry>>(*second.snapshot);
        fs::DirEntry extra; extra.name = L"z.txt"; extra.attrs = FILE_ATTRIBUTE_NORMAL;
        other_rows->insert(other_rows->begin(), extra); second.SetSnapshot(other_rows);
        second.SelectOnly(1);
        second.sort_direction = ui::SortDirection::Desc;
        check(QueueNotifiesToVisible(s, path, {{FILE_ACTION_RENAMED_NEW_NAME, L"b.txt", L"a.txt"}}) &&
              wait_result(), "worker resolves a shared rename fact once for distinct pane orders");
        DrainNotifyPatches(s);
        check(first.EntryAt(0).name == L"b.txt" && second.EntryAt(0).name == L"z.txt" &&
              second.EntryAt(1).name == L"b.txt" && first.snapshot != second.snapshot &&
              first.EntryAt(first.selected_index).name == L"b.txt" &&
              second.EntryAt(second.selected_index).name == L"b.txt",
              "resolved rename sorts each pane by its direction and remaps independent selection");

        for (unsigned stale = 0; stale < 5; ++stale) {
            reset();
            const auto original = first.snapshot;
            check(QueueNotifiesToVisible(s, path, {{FILE_ACTION_ADDED, L"b.txt", {}}}) && wait_result(),
                  "stage a completed background patch for stale-result verification");
            if (stale == 0) {
                first.NavigateTo(path + L"\\elsewhere");
                first.NavigateTo(path);
                first.SetSnapshot(original);
            } else if (stale == 1) {
                first.SetSnapshot(listing());
            } else if (stale == 2) {
                RefreshPath(s, path, RefreshReason::Explicit);
            } else if (stale == 3) {
                first = app::Tab{};
                first.current_path = path;
                first.SetSnapshot(original);
            } else {
                ++first.applied_generation;
            }
            const auto current = first.snapshot;
            DrainNotifyPatches(s);
            check(first.snapshot == current && second.snapshot == original &&
                  first.EntryCount() == 1 && s.notify_patches.pending.empty(),
                  stale == 0 ? "return to same path and snapshot rejects the old view generation" :
                  stale == 1 ? "snapshot replacement rejects the old resolved facts for every pane" :
                  stale == 2 ? "explicit refresh invalidates the pending patch generation" :
                  stale == 3 ? "replacing a tab at the same address and snapshot rejects the old request" :
                               "a newer applied enumeration generation rejects the old resolved facts");
        }

        reset();
        const auto retained = first.snapshot;
        check(QueueNotifiesToVisible(s, path, {{FILE_ACTION_MODIFIED, L"a.txt", {}}}) &&
              QueueNotifiesToVisible(s, path, {{FILE_ACTION_REMOVED, L"a.txt", {}}}) && wait_result(),
              "stage an inconclusive stat followed by a removal");
        DrainNotifyPatches(s);
        check(first.snapshot == retained && second.snapshot == retained && first.EntryCount() == 1 &&
              first.pending_generation != 0 && s.notify_patches.pending.empty(),
              "failed stat keeps all old rows until fallback enumeration rather than applying later removals");
        s.worker.Stop();
    }
    check(file == INVALID_HANDLE_VALUE || DeleteFileW(added.c_str()), "remove isolated fixture file");
    check(!created || RemoveDirectoryW(path.c_str()), "remove isolated fixture directory");
    s.watches.Stop(); s.worker.Stop();
    DestroyWindow(s.hwnd); s.hwnd = nullptr;
    owned.reset(); OleUninitialize();
    return failures ? 1 : 0;
}
