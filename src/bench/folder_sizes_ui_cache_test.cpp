#include "../app/app_state.h"
#include "../app/folder_sizes_ui.h"
#include "../common/localization.h"
#include <filesystem>
#include <fstream>
#include <iostream>

namespace {
int failures = 0;
void Check(bool passed, const char* text) {
    std::cout << (passed ? "[PASS] " : "[FAIL] ") << text << std::endl;
    failures += !passed;
}
template<class Predicate> bool Wait(Predicate ready) {
    const auto deadline = GetTickCount64() + 6000;
    while (!ready() && GetTickCount64() < deadline) Sleep(10);
    return ready();
}
void Write(const std::filesystem::path& path, size_t bytes) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file << std::string(bytes, 'x');
}
pulse::ui::WindowViewModel View(pulse::AppState& state, bool labels = true) {
    pulse::ui::WindowViewModel vm;
    for (const auto& pane : state.window_tabs.Active()->panes) {
        pulse::ui::PaneSlotView slot;
        slot.rect = D2D1::RectF(240, 120, 900, 550);
        slot.focused = vm.pane_slots.empty();
        slot.pane.path = pane->view.current_path;
        slot.pane.snapshot = pane->view.snapshot;
        slot.pane.is_file_system = true;
        slot.pane.sort_column = pane->view.sort_column;
        slot.pane.sort_direction = pane->view.sort_direction;
        slot.pane.view_mode = labels ? pulse::ui::ViewMode::Tiles : pulse::ui::ViewMode::Details;
        vm.pane_slots.push_back(std::move(slot));
    }
    return vm;
}
}
int main() {
    using namespace pulse;
    namespace disk = std::filesystem;
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    const auto com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(com)) return 2;
    l10n::Initialize(GetModuleHandleW(nullptr), L"zh-CN");
    const auto root = disk::current_path() / L"bench_data" /
        (L"folder-size-ui-cache-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    disk::create_directories(root / L"a-large");
    disk::create_directory(root / L"z-small");
    Write(root / L"a-large" / L"data.bin", 1000);
    Write(root / L"z-small" / L"data.bin", 10);
    auto snapshot = std::make_shared<std::vector<fs::DirEntry>>();
    fs::DirEntry large; large.name = L"a-large"; large.is_dir = true;
    snapshot->push_back(large);
    for (int i = 0; i < 130; ++i) {
        fs::DirEntry entry; entry.name = std::to_wstring(i) + L".bin"; entry.size = 500;
        snapshot->push_back(std::move(entry));
    }
    fs::DirEntry small_folder; small_folder.name = L"z-small"; small_folder.is_dir = true;
    snapshot->push_back(small_folder);
    {
        auto state = std::make_unique<AppState>();
        state->isolatedTest = true;
        state->appPrefs.persist = false;
        state->window_tabs.EnsureDefault();
        auto* layout = state->window_tabs.Active();
        auto& first = layout->panes[0]->view;
        first.current_path = root.wstring(); first.sort_column = ui::SortColumn::Size;
        first.SetSnapshot(snapshot); first.selected_index = 0; first.selection_anchor = 0;
        first.selected.insert(0); first.selected.insert(131);
        auto second_pane = std::make_unique<app::Pane>();
        second_pane->view.current_path = root.wstring();
        second_pane->view.sort_column = ui::SortColumn::Size;
        second_pane->view.sort_direction = ui::SortDirection::Desc;
        second_pane->view.SetSnapshot(snapshot);
        layout->panes.push_back(std::move(second_pane));
        auto vm = View(*state);
        PrepareFolderSizes(*state, vm);
        Check(Wait([&] {
            return state->folderSizes.Get((root / L"a-large").wstring()).state == app::FolderSizeState::Ready &&
                state->folderSizes.Get((root / L"z-small").wstring()).state == app::FolderSizeState::Ready;
        }), "size sort requests offscreen folders in the complete snapshot");
        state->folderSizes.TakeChanged();
        QueueFolderSizeSort(*state);
        state->folder_size_sort_due = 0;
        Check(ApplyFolderSizeSort(*state) && first.snapshot->front().name == L"z-small" &&
            layout->panes[1]->view.snapshot->front().name == L"a-large",
            "two panes independently sort the same directory in opposite directions");
        Check(first.EntryAt(static_cast<size_t>(first.selected_index)).name == L"a-large" &&
            first.EntryAt(static_cast<size_t>(first.selection_anchor)).name == L"a-large" && first.selected.size() == 2,
            "size reorder preserves focused row, anchor and multiple selection");
        const auto before_reordered_snapshot = FolderSizesUiStatsForTest(*state);
        vm = View(*state); PrepareFolderSizes(*state, vm);
        const auto stable = FolderSizesUiStatsForTest(*state);
        Check(stable.request_rebuilds == before_reordered_snapshot.request_rebuilds + 2 &&
            stable.syncs == before_reordered_snapshot.syncs + 1 &&
            stable.sort_passes == before_reordered_snapshot.sort_passes && !state->folder_size_sort_pending,
            "each reordered snapshot refreshes interests once without scheduling another sort");
        Sleep(800);
        for (int i = 0; i < 100; ++i) {
            vm = View(*state);
            PrepareFolderSizes(*state, vm);
            ApplyFolderSizeSort(*state);
        }
        const auto after = FolderSizesUiStatsForTest(*state);
        Check(after.request_rebuilds == stable.request_rebuilds && after.syncs == stable.syncs &&
            after.sort_passes == stable.sort_passes && !state->folder_size_sort_pending,
            "100 stable frames after the 750 ms interval neither rebuild requests nor queue/resort");
        FillFolderSizes(*state, vm);
        Check(FolderSizesUiStatsForTest(*state).syncs == stable.syncs &&
            FolderSizesUiStatsForTest(*state).request_rebuilds == stable.request_rebuilds,
            "read-only label fills never synchronize service interests");
        vm.pane_slots[0].pane.scroll_y = 3000;
        PrepareFolderSizes(*state, vm);
        Check(FolderSizesUiStatsForTest(*state).request_rebuilds == stable.request_rebuilds,
            "scrolling size-sorted views retains complete directory interests");
        state->renameIndex = 0;
        Write(root / L"z-small" / L"data.bin", 2000);
        state->folderSizes.Invalidate((root / L"z-small").wstring());
        Check(Wait([&] { return state->folderSizes.Get((root / L"z-small").wstring()).bytes == 2000; }),
            "asynchronous refresh publishes the changed total");
        if (state->folderSizes.TakeChanged()) QueueFolderSizeSort(*state);
        state->folder_size_sort_due = 0;
        Check(!ApplyFolderSizeSort(*state) && state->folder_size_sort_pending,
            "asynchronous sort stays pending during inline rename");
        state->renameIndex = -1;
        Check(ApplyFolderSizeSort(*state) && first.snapshot->front().name == L"a-large" &&
            layout->panes[1]->view.snapshot->front().name == L"z-small",
            "a total change reorders both panes after the gesture ends");
        const auto before_direction = FolderSizesUiStatsForTest(*state);
        first.sort_direction = ui::SortDirection::Desc;
        vm = View(*state); PrepareFolderSizes(*state, vm); state->folder_size_sort_due = 0;
        Check(ApplyFolderSizeSort(*state) && first.snapshot->front().name == L"z-small" &&
            FolderSizesUiStatsForTest(*state).sort_passes == before_direction.sort_passes + 1,
            "sort direction invalidates only the affected pane");
        first.all_selected = true;
        first.sort_direction = ui::SortDirection::Asc;
        vm = View(*state); PrepareFolderSizes(*state, vm); state->folder_size_sort_due = 0;
        Check(ApplyFolderSizeSort(*state) && first.all_selected,
            "size reorder preserves select-all semantics");
        const auto before_snapshot = FolderSizesUiStatsForTest(*state);
        first.SetSnapshot(std::make_shared<std::vector<fs::DirEntry>>(*first.snapshot));
        vm = View(*state); PrepareFolderSizes(*state, vm); state->folder_size_sort_due = 0;
        ApplyFolderSizeSort(*state);
        Check(FolderSizesUiStatsForTest(*state).request_rebuilds == before_snapshot.request_rebuilds + 1 &&
            FolderSizesUiStatsForTest(*state).sort_passes == before_snapshot.sort_passes + 1,
            "a replacement immutable snapshot rebuilds and checks only its pane");
        const auto before_details = FolderSizesUiStatsForTest(*state);
        vm = View(*state); vm.details.is_dir = true; vm.details.path = (root / L"z-small").wstring();
        PrepareFolderSizes(*state, vm);
        Check(FolderSizesUiStatsForTest(*state).syncs == before_details.syncs + 1 &&
            FolderSizesUiStatsForTest(*state).request_rebuilds == before_details.request_rebuilds,
            "details scope changes synchronize counts without rebuilding pane requests");
        first.sort_column = ui::SortColumn::Name;
        layout->panes[1]->view.sort_column = ui::SortColumn::Name;
        const auto before_remove = FolderSizesUiStatsForTest(*state);
        vm = View(*state, false); PrepareFolderSizes(*state, vm);
        Check(FolderSizesUiStatsForTest(*state).syncs == before_remove.syncs + 1 &&
            vm.pane_slots[0].pane.folder_size_labels.empty(),
            "leaving size sorting in a layout without labels releases all interests");
        state->folderSizes.Stop();
    }
    disk::remove_all(root);
    CoUninitialize();
    return failures ? 1 : 0;
}

