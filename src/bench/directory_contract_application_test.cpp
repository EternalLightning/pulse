#include "../app/app_internal.h"
#include "../app/app_column_view.h"
#include "../app/folder_sizes_ui.h"
#include "../common/localization.h"
#include <cstdio>
#include <condition_variable>
#include <mutex>
#include <thread>

int main() {
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
    s.hwnd = CreateWindowExW(WS_EX_NOACTIVATE, L"STATIC", L"Directory contract fixture",
        WS_OVERLAPPEDWINDOW, -20000, -20000, 1000, 700, nullptr, nullptr,
        GetModuleHandleW(nullptr), nullptr);
    if (!s.hwnd || !s.compositor.Init(s.hwnd)) {
        std::puts("[FAIL] initialize isolated directory-contract compositor");
        if (s.hwnd) DestroyWindow(s.hwnd);
        s.hwnd = nullptr;
        owned.reset(); OleUninitialize();
        return 2;
    }
    s.renderer.SetCompositor(&s.compositor);
    s.window_tabs.EnsureDefault();
    s.pane = s.window_tabs.Active()->FocusedPane();
    auto& name_tab = *ActiveTab(s);
    auto second = std::make_unique<app::Pane>();
    auto& size_tab = second->view;
    s.window_tabs.Active()->panes.push_back(std::move(second));
    app::RebuildLayoutRoot(*s.window_tabs.Active());
    const std::wstring path = fs::NormalizePath(L"C:\\pulse-directory-contract-memory");
    name_tab.current_path = size_tab.current_path = path;
    size_tab.sort_column = ui::SortColumn::Size;
    auto facts = std::make_shared<std::vector<fs::DirEntry>>(2);
    (*facts)[0].name = L"b.txt"; (*facts)[0].size = 1;
    (*facts)[1].name = L"a.txt"; (*facts)[1].size = 10;
    for (auto& entry : *facts) entry.attrs = FILE_ATTRIBUTE_NORMAL;
    name_tab.pending_generation = 1; size_tab.pending_generation = 2;
    app::WorkResult result{};
    result.path = path; result.generation = 1; result.snapshot = facts;
    ApplyWorkerResult(s, result);
    name_tab.SelectOnly(1);
    result.generation = 2; result.sort_column = ui::SortColumn::Size;
    ApplyWorkerResult(s, result);
    check(name_tab.EntryAt(0).name == L"a.txt" && size_tab.EntryAt(0).name == L"b.txt" &&
          name_tab.EntryAt(name_tab.selected_index).name == L"b.txt" && facts->at(0).name == L"b.txt",
          "shared worker completion preserves each pane's order and existing selection");
    const auto before_sort_generation = name_tab.pending_generation;
    SetSort(s, ui::SortColumn::Size, ui::SortDirection::Asc);
    check(name_tab.EntryAt(0).name == L"b.txt" && !name_tab.order_held &&
          name_tab.EntryAt(name_tab.selected_index).name == L"b.txt" &&
          name_tab.pending_generation == before_sort_generation && !name_tab.loading,
          "sort command projects current facts without enumeration or selection loss");
    name_tab.sort_column = ui::SortColumn::Name;
    for (auto* tab : {&name_tab, &size_tab}) {
        tab->column_layout = true;
        tab->column_strip.for_path = path;
        app::ColumnStripColumn column;
        column.path = path; column.snapshot = facts; column.requested = true;
        tab->column_strip.ancestors.push_back(std::move(column));
    }
    SyncColumnStrips(s);
    auto& name_column = name_tab.column_strip.ancestors.front();
    auto& size_column = size_tab.column_strip.ancestors.front();
    check(name_column.snapshot->at(static_cast<size_t>(name_column.rows->at(0))).name == L"a.txt" &&
          size_column.snapshot->at(static_cast<size_t>(size_column.rows->at(0))).name == L"b.txt",
          "column source indices use the owning pane's sort projection");
    result.generation = 10;
    NoteColumnStripResult(s, result);
    const auto accepted = name_column.snapshot;
    result.generation = 9;
    result.snapshot = std::make_shared<std::vector<fs::DirEntry>>();
    NoteColumnStripResult(s, result);
    check(name_column.applied_generation == 10 && name_column.generation == 0 &&
          name_column.snapshot == accepted && size_column.snapshot == accepted,
          "older column completion cannot overwrite a newer applied generation");
    result.generation = 11; result.error = true;
    name_column.generation = size_column.generation = 11;
    NoteColumnStripResult(s, result);
    result.generation = 10; result.error = false; result.snapshot = facts;
    NoteColumnStripResult(s, result);
    check(name_column.error && name_column.applied_generation == 11,
          "newer column error also rejects stale successful completion");
    const auto sharing_case = [&](bool reload) {
        wchar_t cwd[MAX_PATH]{};
        GetCurrentDirectoryW(MAX_PATH, cwd);
        const auto fixture = std::wstring(cwd) + L"\\bench_data\\shared-enumeration-" +
            std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
        const auto file_path = fixture + L"\\shared.txt";
        const bool created = CreateDirectoryW(fixture.c_str(), nullptr) != FALSE;
        HANDLE file = created ? CreateFileW(file_path.c_str(), GENERIC_WRITE, 0, nullptr,
            CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr) : INVALID_HANDLE_VALUE;
        if (file == INVALID_HANDLE_VALUE) {
            check(false, "create isolated shared enumeration fixture");
            if (created) RemoveDirectoryW(fixture.c_str());
            return;
        }
        CloseHandle(file);
        std::mutex mutex;
        std::condition_variable changed;
        bool release = false;
        unsigned entered = 0;
        std::vector<app::WorkResult> results;
        s.worker.Start([&](app::WorkResult result) {
            std::lock_guard lock(mutex);
            results.push_back(std::move(result)); changed.notify_all();
        });
        const auto count = (std::min)(4u, (std::max)(2u, std::thread::hardware_concurrency()));
        for (unsigned i = 0; i < count; ++i) s.worker.EnqueueIo([&] {
            std::unique_lock lock(mutex);
            ++entered; changed.notify_all();
            changed.wait(lock, [&] { return release; });
        });
        bool blocked = false;
        {
            std::unique_lock lock(mutex);
            blocked = changed.wait_for(lock, std::chrono::seconds(5), [&] { return entered == count; });
        }
        s.shot.active = true;
        name_tab.sort_column = size_tab.sort_column = ui::SortColumn::Name;
        name_tab.sort_direction = size_tab.sort_direction = ui::SortDirection::Asc;
        name_tab.pending_generation = size_tab.pending_generation = 0;
        StartLoadingPath(s, name_tab, fixture);
        StartLoadingPath(s, size_tab, fixture);
        const auto shared = size_tab.pending_generation;
        const bool shared_before = shared && name_tab.pending_generation == shared;
        StartLoadingPath(s, name_tab, reload ? fixture : L"pulse:fixture-empty");
        {
            std::lock_guard lock(mutex); release = true;
        }
        changed.notify_all();
        {
            std::unique_lock lock(mutex);
            changed.wait_for(lock, std::chrono::seconds(5), [&] { return !results.empty(); });
        }
        s.worker.Stop();
        for (auto& result : results) ApplyWorkerResult(s, result);
        check(blocked && shared_before && !size_tab.loading && !size_tab.pending_generation &&
              size_tab.EntryCount() == 1 && size_tab.EntryAt(0).name == L"shared.txt" &&
              (!reload || (!name_tab.loading && name_tab.EntryCount() == 1)),
              reload ? "same-path reload preserves a shared pending enumeration" :
                       "one pane navigating away preserves the other pane's pending enumeration");
        s.shot.active = false;
        s.watches.Stop();
        check(DeleteFileW(file_path.c_str()) && RemoveDirectoryW(fixture.c_str()),
              "shared enumeration fixture cleaned");
    };
    sharing_case(false);
    sharing_case(true);
    name_tab.current_path = L"pulse:search:fixture";
    name_tab.pending_generation = 900;
    index::SearchResult partial;
    partial.incomplete = true; partial.error = ERROR_BAD_NETPATH; partial.total = 1;
    index::Hit hit;
    hit.name = L"local.txt"; hit.path = L"C:\\fixture\\local.txt";
    partial.hits.push_back(std::move(hit));
    DeliverIndexSearchResult(s, 900, std::move(partial));
    check(name_tab.EntryCount() == 1 && name_tab.banner_type == app::BannerType::SearchIncomplete &&
          !name_tab.banner_message.empty() && !name_tab.loading,
          "incomplete filename search displays local rows and an error banner without remaining pending");
    name_tab.filename_live_generation = 900;
    index::SearchResult complete;
    DeliverIndexSearchResult(s, 900, std::move(complete));
    check(name_tab.EntryCount() == 0 && name_tab.banner_type != app::BannerType::SearchIncomplete,
          "complete filename refresh clears the earlier failure banner");
    s.detailsSelPath = L"C:\\fixture\\same.txt";
    s.detailsProbeGeneration = name_tab.applied_generation;
    s.detailsProbeViewGeneration = name_tab.view_generation;
    s.detailsProbeSnapshot = name_tab.snapshot;
    s.detailsProbeTick = GetTickCount64();
    check(!ShouldRequestDetailsMeta(s, name_tab, s.detailsSelPath),
          "an unchanged current details version reuses its metadata");
    name_tab.SetSnapshot(std::make_shared<std::vector<fs::DirEntry>>());
    check(ShouldRequestDetailsMeta(s, name_tab, s.detailsSelPath),
          "same-path snapshot replacement requests fresh details metadata");
    s.detailsRequestId = 50;
    DetailsMetaResult stale;
    stale.path = s.detailsSelPath; stale.window_epoch = s.detailsWindowEpoch; stale.request_id = 49;
    stale.meta.owner = L"stale";
    s.detailsOwner = L"current";
    check(!ApplyDetailsMetaResult(s, stale) && s.detailsOwner == L"current",
          "same-path old metadata request cannot overwrite current facts");
    stale.request_id = 50; stale.window_epoch = app::NextDetailsWindowEpoch();
    check(!ApplyDetailsMetaResult(s, stale) && s.detailsOwner == L"current",
          "a reused window handle cannot accept metadata from an old window epoch");
    stale.window_epoch = s.detailsWindowEpoch; stale.attrs_valid = true;
    check(ApplyDetailsMetaResult(s, stale) && s.detailsOwner == L"stale" && s.detailsSelValid,
          "matching window and request epochs apply details metadata normally");
    {
        wchar_t cwd[MAX_PATH]{};
        GetCurrentDirectoryW(MAX_PATH, cwd);
        const auto root = std::wstring(cwd) + L"\\bench_data\\shared-size-" +
            std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
        const auto file_path = root + L"\\data.bin";
        const auto offline = root + L"\\offline";
        const bool created = CreateDirectoryW(root.c_str(), nullptr) != FALSE;
        HANDLE file = created ? CreateFileW(file_path.c_str(), GENERIC_WRITE, 0, nullptr,
            CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr) : INVALID_HANDLE_VALUE;
        DWORD written = 0;
        const bool wrote = file != INVALID_HANDLE_VALUE && WriteFile(file, "abc", 3, &written, nullptr) && written == 3;
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
        check(wrote, "create isolated shared folder-size application fixture");
        if (wrote) {
            name_tab.current_path = fs::NormalizePath(root);
            auto selected = std::make_shared<std::vector<fs::DirEntry>>(1);
            selected->front().name = L"data.bin"; selected->front().size = 3;
            selected->front().attrs = FILE_ATTRIBUTE_NORMAL;
            name_tab.SetSnapshot(selected); name_tab.SelectOnly(0);
            ++name_tab.applied_generation;
            s.showDetailsPanel = true;
            (void)BuildVm(s, true);
            bool received_meta = false;
            const auto meta_deadline = GetTickCount64() + 5000;
            while (!received_meta && GetTickCount64() < meta_deadline) {
                MSG message{};
                if (PeekMessageW(&message, s.hwnd, WM_DETAILS_META, WM_DETAILS_META, PM_REMOVE)) {
                    std::unique_ptr<DetailsMetaResult> meta(reinterpret_cast<DetailsMetaResult*>(message.lParam));
                    received_meta = meta && ApplyDetailsMetaResult(s, *meta);
                } else Sleep(10);
            }
            check(received_meta && s.detailsSelValid && !s.detailsTypeName.empty() && !s.detailsOwner.empty() &&
                  !s.detailsPermissions.empty(),
                  "actual COM-initialized details worker returns handle-backed metadata with matching epochs");
            s.showDetailsPanel = false;
            ui::WindowViewModel folders;
            folders.details.path = root; folders.details.is_dir = true;
            ui::PaneSlotView slot;
            slot.rect = D2D1::RectF(240, 120, 900, 550); slot.focused = true;
            slot.pane.path = std::wstring(cwd) + L"\\bench_data";
            slot.pane.is_file_system = true; slot.pane.view_mode = ui::ViewMode::Tiles;
            ui::ListEntryView entry; entry.name = L"shared-size"; entry.path = root; entry.is_dir = true;
            slot.pane.entries.push_back(std::move(entry)); folders.pane_slots.push_back(std::move(slot));
            const auto wait_size = [&](app::FolderSizeState expected) {
                const auto deadline = GetTickCount64() + 5000;
                do {
                    FillFolderSizes(s, folders);
                    if (s.folderSizes.Get(root).state == expected) return true;
                    Sleep(10);
                } while (GetTickCount64() < deadline);
                return false;
            };
            const bool ready = wait_size(app::FolderSizeState::Ready);
            check(ready && s.folderSizes.Get(root).bytes == 3 && s.folderSizes.Get(root).files == 1 &&
                  !folders.details.size_pending && !folders.details.contains_text.empty() &&
                  !folders.pane_slots[0].pane.folder_size_labels.at(0).text.empty(),
                  "details and visible folder row consume one shared bytes-and-counts result");
            const bool marked = CreateDirectoryW(offline.c_str(), nullptr) &&
                SetFileAttributesW(offline.c_str(), FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_OFFLINE);
            check(marked, "create unreadable-content boundary via isolated offline subtree");
            if (marked) {
                s.folderSizes.Invalidate(root);
                const bool partial_size = wait_size(app::FolderSizeState::Partial);
                const auto label = l10n::Get(l10n::StringId::FolderSizePartial);
                check(partial_size && !folders.details.size_pending &&
                      folders.details.size_text.find(label) != std::wstring::npos &&
                      folders.details.contains_text.find(label) != std::wstring::npos &&
                      folders.pane_slots[0].pane.folder_size_labels.at(0).text.find(label) != std::wstring::npos,
                      "partial bytes and counts are explicitly labelled in details and folder rows");
                auto refreshing = s.folderSizes.Get(root);
                refreshing.state = app::FolderSizeState::Updating;
                FillDetailsFolderSize(folders.details, refreshing);
                check(folders.details.size_pending && refreshing.incomplete &&
                      folders.details.contains_text.find(label) != std::wstring::npos,
                      "an incomplete count keeps its partial label throughout a slow refresh");
            }
            s.folderSizes.Stop();
            if (GetFileAttributesW(offline.c_str()) != INVALID_FILE_ATTRIBUTES) {
                SetFileAttributesW(offline.c_str(), FILE_ATTRIBUTE_DIRECTORY); RemoveDirectoryW(offline.c_str());
            }
        }
        const bool cleaned_file = file == INVALID_HANDLE_VALUE || DeleteFileW(file_path.c_str());
        check(cleaned_file && (!created || RemoveDirectoryW(root.c_str())), "shared size application fixture cleaned");
    }
    s.watches.Stop();
    DestroyWindow(s.hwnd); s.hwnd = nullptr;
    owned.reset(); OleUninitialize();
    return failures ? 1 : 0;
}
