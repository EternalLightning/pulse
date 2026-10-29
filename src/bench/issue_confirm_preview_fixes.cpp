#include "../app/app_state.h"
#include "../app/folder_sizes_ui.h"
#include "../app/entry_sort.h"
#include "../preview_host/shell_thumbnail_bitmap.h"
#include <filesystem>
#include <fstream>
#include <cstdio>
#include <cstring>
#include <functional>

namespace {
using namespace pulse;
namespace disk = std::filesystem;
int failures = 0;
void Check(bool ok, const char* label) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
    if (!ok) ++failures;
}
bool Wait(const std::function<bool()>& ready) {
    const auto due = GetTickCount64() + 10000;
    while (GetTickCount64() < due) { if (ready()) return true; Sleep(10); }
    return ready();
}
void Alpha() {
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = 2;
    info.bmiHeader.biHeight = -1;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    void* memory = nullptr;
    HBITMAP bitmap = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &memory, nullptr, 0);
    Check(bitmap && memory, "#213 create real RGB HBITMAP with zero alpha");
    if (!bitmap) return;
    const unsigned char input[] = {10, 20, 40, 0, 15, 25, 45, 0};
    std::memcpy(memory, input, sizeof(input));
    std::vector<uint8_t> old, rgb, argb;
    UINT width = 0, height = 0, stride = 0;
    Check(preview::HbitmapToBgra(bitmap, false, old, width, height, stride, WTSAT_UNKNOWN) &&
        old.size() == 8 && old[3] == 0 && old[7] == 0,
        "#213 control reproduces the former discarded-alpha conversion as transparent pixels");
    Check(preview::HbitmapToBgra(bitmap, false, rgb, width, height, stride, WTSAT_RGB) &&
        width == 2 && height == 1 && stride == 8 && rgb.size() == 8 &&
        rgb[0] == 10 && rgb[1] == 20 && rgb[2] == 40 && rgb[3] == 255 && rgb[7] == 255,
        "#213 declared RGB preserves color and becomes opaque through production WIC conversion");
    auto* pixels = static_cast<unsigned char*>(memory);
    pixels[3] = 128;
    Check(preview::HbitmapToBgra(bitmap, true, argb, width, height, stride, WTSAT_ARGB) &&
        argb.size() == 8 && argb[3] == 128 && argb[7] == 0 && argb[0] == 10,
        "#213 declared ARGB retains partial and full transparency");
}
#ifndef PULSE_ALPHA_ONLY
int Find(const app::Tab& tab, const std::wstring& name) {
    for (size_t i = 0; i < tab.EntryCount(); ++i) if (tab.EntryAt(i).name == name) return static_cast<int>(i);
    return -1;
}
std::wstring Order(const app::Tab& tab) {
    std::wstring result;
    for (const auto& entry : *tab.snapshot) { if (!result.empty()) result += L","; result += entry.name; }
    return result;
}
void Sizes() {
    const auto root = disk::current_path() / L"bench_data" / L"issue-confirm-preview-fixes" /
        (L"sizes-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    for (const auto* name : {L"a-large", L"b-small", L"c-empty"}) disk::create_directories(root / name);
    std::ofstream(root / L"a-large" / L"payload", std::ios::binary) << std::string(1000, 'x');
    std::ofstream(root / L"b-small" / L"payload", std::ios::binary) << std::string(10, 'x');
    std::ofstream(root / L"middle.bin", std::ios::binary) << std::string(20, 'x');
    auto state = std::make_unique<AppState>();
    auto& s = *state;
    s.isolatedTest = true;
    s.appPrefs.persist = s.ctxMenuPrefs.persist = s.places.persist = false;
    s.window_tabs.EnsureDefault();
    s.pane = s.window_tabs.Active()->FocusedPane();
    auto& tab = s.pane->view;
    tab.current_path = root.wstring();
    tab.sort_column = ui::SortColumn::Size;
    tab.sort_direction = ui::SortDirection::Asc;
    auto entries = std::make_shared<std::vector<fs::DirEntry>>();
    Check(fs::EnumerateDirectory(tab.current_path, *entries), "#58 enumerate private real-size directories");
    std::sort(entries->begin(), entries->end(), [&](const auto& a, const auto& b) {
        return app::EntryLess(a, b, ui::SortColumn::Size, ui::SortDirection::Asc);
    });
    app::Tab control;
    control.current_path = tab.current_path;
    control.SetSnapshot(entries);
    Check(Find(control, L"a-large") < Find(control, L"b-small") &&
        control.EntryAt(static_cast<size_t>(Find(control, L"a-large"))).size == 0,
        "#58 control reproduces name order when raw enumerated directory sizes are zero");
    fs::DirEntry unknown;
    unknown.name = L"z-unknown"; unknown.is_dir = true; unknown.attrs = FILE_ATTRIBUTE_DIRECTORY;
    entries->push_back(unknown);
    tab.SetSnapshot(entries);
    ui::WindowViewModel vm;
    ui::PaneSlotView slot;
    slot.focused = true;
    slot.rect = D2D1::RectF(0, 0, 800, 10);
    slot.pane.path = tab.current_path;
    slot.pane.is_file_system = true;
    slot.pane.view_mode = ui::ViewMode::Details;
    slot.pane.sort_column = ui::SortColumn::Size;
    slot.pane.snapshot = tab.snapshot;
    vm.pane_slots.push_back(slot);
    PrepareFolderSizes(s, vm);
    const auto ready = [&](const wchar_t* name, uint64_t bytes) {
        const auto value = s.folderSizes.Get((root / name).wstring());
        return value.state == app::FolderSizeState::Ready && value.has_value && value.bytes == bytes;
    };
    Check(Wait([&] { return ready(L"a-large", 1000) && ready(L"b-small", 10) && ready(L"c-empty", 0); }),
        "#58 production preparation scans all directories despite offscreen details rows");
    Check(vm.pane_slots.front().pane.folder_size_labels.empty(),
        "#58 requesting size sort does not add unsupported details labels");
    const wchar_t* expected[3][2] = {
        {L"c-empty,b-small,a-large,z-unknown,middle.bin", L"a-large,b-small,c-empty,z-unknown,middle.bin"},
        {L"c-empty,b-small,a-large,z-unknown,middle.bin", L"middle.bin,a-large,b-small,c-empty,z-unknown"},
        {L"c-empty,b-small,middle.bin,a-large,z-unknown", L"a-large,middle.bin,b-small,c-empty,z-unknown"}
    };
    for (int mode = 0; mode < 3; ++mode) for (int descending = 0; descending < 2; ++descending) {
        tab.SetSnapshot(entries);
        tab.sort_direction = descending ? ui::SortDirection::Desc : ui::SortDirection::Asc;
        s.appPrefs.folder_sort_mode = mode;
        tab.SelectIndices({Find(tab, L"a-large"), Find(tab, L"middle.bin")});
        tab.selected_index = Find(tab, L"a-large");
        tab.selection_anchor = Find(tab, L"middle.bin");
        tab.scroll_y = 73.5f;
        tab.scroll_x = 12.0f;
        const auto generation = tab.view_generation;
        s.folder_size_sort_due = 0;
        QueueFolderSizeSort(s);
        const bool applied = ApplyFolderSizeSort(s);
        if (!applied || Order(tab) != expected[mode][descending])
            std::printf("[OBS] mode=%d descending=%d order=%ls expected=%ls\n", mode, descending,
                Order(tab).c_str(), expected[mode][descending]);
        Check(applied && Order(tab) == expected[mode][descending],
            "#58 actual totals sort correctly for folder policy and direction; unknown is last");
        Check(tab.SelectedCount() == 2 && tab.IsSelected(Find(tab, L"a-large")) && tab.IsSelected(Find(tab, L"middle.bin")) &&
            tab.EntryAt(static_cast<size_t>(tab.selected_index)).name == L"a-large" &&
            tab.EntryAt(static_cast<size_t>(tab.selection_anchor)).name == L"middle.bin" &&
            tab.scroll_y == 73.5f && tab.scroll_x == 12.0f && tab.view_generation == generation + 1,
            "#58 sort preserves selected identities, focus, anchor and scroll while advancing view generation");
        Check(std::all_of(tab.snapshot->begin(), tab.snapshot->end(), [](const auto& e) { return !e.is_dir || e.size == 0; }),
            "#58 aggregate sort keys never overwrite enumerated directory entry sizes");
    }
    s.appPrefs.folder_sort_mode = 0;
    tab.sort_direction = ui::SortDirection::Asc;
    for (bool all : {false, true}) {
        tab.SetSnapshot(entries);
        if (all) tab.SelectAll(); else tab.ClearSelection();
        s.folder_size_sort_due = 0;
        QueueFolderSizeSort(s);
        Check(ApplyFolderSizeSort(s) && (all ? tab.SelectedCount() == static_cast<int>(entries->size()) : tab.SelectedCount() == 0),
            "#58 empty and select-all selections retain their original meaning after sorting");
    }
    tab.SetSnapshot(entries);
    s.folder_size_sort_due = 0;
    QueueFolderSizeSort(s);
    s.renameIndex = 0;
    Check(!ApplyFolderSizeSort(s) && s.folder_size_sort_pending && tab.snapshot == entries,
        "#58 inline rename defers sorting and retains a pending update");
    s.renameIndex = -1;
    s.dragPending = true;
    Check(!ApplyFolderSizeSort(s) && s.folder_size_sort_pending && tab.snapshot == entries,
        "#58 pointer drag defers sorting and retains a pending update");
    s.dragPending = false;
    Check(ApplyFolderSizeSort(s), "#58 deferred size sort applies after interaction ends");
    QueueFolderSizeSort(s);
    const auto sorted = tab.snapshot;
    Check(!ApplyFolderSizeSort(s) && s.folder_size_sort_pending && tab.snapshot == sorted,
        "#58 750ms throttle preserves pending results instead of discarding them");
    tab.current_path = (root / L"a-large").wstring();
    s.folder_size_sort_due = 0;
    Check(!ApplyFolderSizeSort(s) && tab.snapshot == sorted,
        "#58 a retained old snapshot is not resorted after directory switching");
    vm.pane_slots.front().pane.path = tab.current_path;
    vm.pane_slots.front().pane.snapshot = std::make_shared<const std::vector<fs::DirEntry>>();
    PrepareFolderSizes(s, vm);
    s.folderSizes.Stop();
    Check(tab.snapshot == sorted, "#58 cancelling old folder interests does not publish a replacement snapshot");
    disk::remove_all(root);
}
#endif
}
int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr)) return 2;
    if (argc == 1 || std::strcmp(argv[1], "--alpha") == 0) Alpha();
#ifndef PULSE_ALPHA_ONLY
    if (argc == 1 || std::strcmp(argv[1], "--folder-sizes") == 0) Sizes();
#endif
    std::printf("RESULT failures=%d\n", failures);
    CoUninitialize();
    return failures ? 1 : 0;
}
