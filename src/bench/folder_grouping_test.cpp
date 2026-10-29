#include "../app/app_model.h"
#include "../app/date_groups.h"
#include "../app/entry_sort.h"
#include "../app/folder_view_prefs.h"
#include "../common/json_utils.h"
#include <d2d1helper.h>
#include <algorithm>
#include <cstdio>
#include <limits>

namespace pulse::app {
std::wstring GetPulseDataDir() { return {}; }
}

namespace {
int failures = 0;
void Check(bool value, const char* message) {
    std::printf("[%s] %s\n", value ? "PASS" : "FAIL", message);
    if (!value) ++failures;
}

FILETIME LocalDate(WORD day) {
    SYSTEMTIME date{};
    date.wYear = 2026;
    date.wMonth = 10;
    date.wDay = day;
    date.wHour = 12;
    SYSTEMTIME utc{};
    FILETIME value{};
    TzSpecificLocalTimeToSystemTime(nullptr, &date, &utc);
    SystemTimeToFileTime(&utc, &value);
    return value;
}

std::shared_ptr<const std::vector<pulse::fs::DirEntry>> Entries(pulse::ui::SortColumn column,
    pulse::ui::SortDirection direction) {
    auto entries = std::make_shared<std::vector<pulse::fs::DirEntry>>();
    auto add = [&](const wchar_t* name, uint64_t size, WORD day, bool directory = false) {
        pulse::fs::DirEntry entry;
        entry.name = name;
        entry.size = size;
        entry.mtime = LocalDate(day);
        entry.is_dir = directory;
        entry.attrs = directory ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
        entries->push_back(std::move(entry));
    };
    add(L"a.txt", 100, 22);
    add(L"b.png", 2 * 1024 * 1024, 21);
    add(L"c.TXT", 3 * 1024 * 1024, 22);
    add(L"d.png", 50, 5);
    add(L"e", 0, 21);
    add(L"folder", 0, 5, true);
    std::stable_sort(entries->begin(), entries->end(), [&](const auto& a, const auto& b) {
        return pulse::app::EntryLess(a, b, column, direction);
    });
    return entries;
}

bool SourceSortPreserved(const pulse::app::GroupedView& view) {
    for (const auto& group : view.groups) {
        const auto first = view.indices.begin() + group.first;
        if (!std::is_sorted(first, first + group.count)) return false;
    }
    return true;
}
}

int main() {
    using namespace pulse;
    SYSTEMTIME today{};
    today.wYear = 2026;
    today.wMonth = 10;
    today.wDay = 22;
    for (const auto sort : {ui::SortColumn::Name, ui::SortColumn::Size, ui::SortColumn::Mtime}) {
        for (const auto sort_direction : {ui::SortDirection::Asc, ui::SortDirection::Desc}) {
            const auto snapshot = Entries(sort, sort_direction);
            bool independent = true;
            for (const auto mode : {ui::GroupMode::DateModified, ui::GroupMode::Type, ui::GroupMode::Size}) {
                const auto asc = app::BuildGroupedView(snapshot, nullptr, mode, ui::SortDirection::Asc, today);
                const auto desc = app::BuildGroupedView(snapshot, nullptr, mode, ui::SortDirection::Desc, today);
                independent &= asc.indices.size() == snapshot->size() && asc.groups.size() > 1 &&
                    SourceSortPreserved(asc) && SourceSortPreserved(desc) && asc.groups.size() == desc.groups.size();
                for (size_t i = 0; i < asc.groups.size(); ++i) {
                    const auto& a = asc.groups[i];
                    const auto& b = desc.groups[desc.groups.size() - 1 - i];
                    independent &= a.bucket == b.bucket && a.label == b.label && a.count == b.count &&
                        std::equal(asc.indices.begin() + a.first, asc.indices.begin() + a.first + a.count,
                            desc.indices.begin() + b.first);
                }
            }
            Check(independent, "independent group order preserves ascending/descending user sort within each group");
        }
    }

    const auto snapshot = Entries(ui::SortColumn::Name, ui::SortDirection::Asc);
    const std::vector<int> visible{1, 2, 3, 4};
    const auto filtered = app::BuildGroupedView(snapshot, &visible, ui::GroupMode::Type, ui::SortDirection::Asc, today);
    bool mapping = filtered.indices == std::vector<int>({2, 4, 1, 3});
    for (size_t i = 0; i < filtered.indices.size(); ++i)
        mapping &= filtered.source_to_view[static_cast<size_t>(filtered.indices[i])] == static_cast<int>(i);
    mapping &= filtered.source_to_view[0] == -1 && filtered.source_to_view[5] == -1;
    Check(mapping, "filtered group order maps view rows and immutable source indices both ways");
    const std::vector<int> invalid{-1, 999, 2};
    const auto sanitized = app::BuildGroupedView(snapshot, &invalid, ui::GroupMode::Size, ui::SortDirection::Asc, today);
    Check(sanitized.indices == std::vector<int>({2}) && sanitized.groups[0].first == 0,
        "invalid source indices cannot create sparse group headers");

    app::Pane pane;
    auto& tab = pane.view;
    tab.current_path = L"C:\\fixture";
    tab.sort_column = ui::SortColumn::Name;
    tab.SetSnapshot(snapshot);
    tab.SelectOnly(1);
    const auto original_selection = tab.selected;
    const auto original_revision = tab.selection_revision;
    tab.group_mode = ui::GroupMode::Type;
    tab.group_direction = ui::SortDirection::Asc;
    ui::PaneViewModel vm;
    app::FillPaneViewModel(vm, pane);
    Check(tab.snapshot == snapshot && tab.selected == original_selection &&
        tab.selected_index == 1 && tab.selection_revision == original_revision &&
        vm.ViewIndex(1) == 4 && vm.SourceIndex(4) == 1 && vm.date_groups && vm.date_groups->size() == 4,
        "grouping leaves shared snapshot, selection and focus identity intact");
    tab.SelectOnly(vm.SourceIndex(2));
    tab.MoveFocus(vm.SourceIndex(4), true);
    Check(tab.selected == std::unordered_set<int>({2, 4, 1}),
        "shift navigation selects the displayed grouped range rather than the source-index interval");
    tab.filter_text = L"*.txt";
    app::FillPaneViewModel(vm, pane);
    Check(vm.EntryCount() == 2 && vm.SourceIndex(0) == 1 && vm.SourceIndex(1) == 3 &&
        vm.ViewIndex(1) == 0 && vm.ViewIndex(3) == 1 && vm.ViewIndex(2) == -1 &&
        vm.date_groups && vm.date_groups->size() == 1 && vm.date_groups->front().count == 2,
        "filter changes rebuild the group index map and contiguous headers");
    tab.filter_text.clear();
    tab.group_mode = ui::GroupMode::None;
    app::FillPaneViewModel(vm, pane);
    Check(!vm.date_groups && !vm.source_to_view && vm.SourceIndex(1) == 1,
        "None restores the original sorted source projection");
    tab.group_mode = ui::GroupMode::Auto;
    app::FillPaneViewModel(vm, pane);
    const bool auto_name = !vm.date_groups;
    tab.sort_column = ui::SortColumn::Mtime;
    app::FillPaneViewModel(vm, pane);
    Check(auto_name && vm.date_groups && !vm.date_groups->empty(),
        "legacy Auto groups modified-date sorting only");
    tab.group_mode = ui::GroupMode::Type;
    tab.filter_text = L"not-present";
    app::FillPaneViewModel(vm, pane);
    Check(vm.EntryCount() == 0 && !vm.date_groups, "empty filtered view has no orphan headers");

    bool layout_ok = true;
    for (float scale : {1.0f, 1.5f, 2.0f}) for (float width : {120.0f, 600.0f}) {
        const auto bounds = D2D1::RectF(0, 0, width * scale, 110 * scale);
        ui::ViewLayout layout(ui::ViewMode::Details, bounds, filtered.indices.size(),
            0, 0, scale, 28, &filtered.groups);
        const auto first = layout.ItemRect(0);
        layout_ok &= first.right > first.left && layout.HitTest(first.left + 1, first.top - scale) == -1 &&
            layout.HitTest(first.left + 1, first.top + scale) == 0 && layout.MaxScrollY() > 0;
        ui::ViewLayout scrolled(ui::ViewMode::Details, bounds, filtered.indices.size(),
            0, layout.MaxScrollY(), scale, 28, &filtered.groups);
        const auto last = scrolled.ItemRect(static_cast<int>(filtered.indices.size()) - 1);
        layout_ok &= scrolled.HitTest(last.left + scale, last.top + scale) == static_cast<int>(filtered.indices.size()) - 1;
    }
    Check(layout_ok, "group header hit testing and scrolling fit narrow panes at 100/150/200 percent DPI");

    app::FolderViewPrefs prefs;
    const std::wstring unc = L"\\\\server\\share\\Folder";
    Check(prefs.Set(unc, ui::ViewMode::Tiles) && prefs.SetSort(unc, ui::SortColumn::Size, ui::SortDirection::Desc) &&
        prefs.SetGroup(unc, ui::GroupMode::Type, ui::SortDirection::Asc) && prefs.SetColumns(unc, {160, 120, 96}),
        "UNC directories store explicit view, sort, grouping and manual column widths");
    Check(!prefs.SetGroup(L"relative", ui::GroupMode::Type, ui::SortDirection::Asc) &&
        !prefs.SetGroup(unc, static_cast<ui::GroupMode>(999), ui::SortDirection::Asc) &&
        !prefs.SetGroup(unc, ui::GroupMode::Type, static_cast<ui::SortDirection>(999)) &&
        !prefs.SetColumns(unc, {-1, 0, 0}) && !prefs.SetColumns(unc, {1.5f, 0, 0}) &&
        !prefs.SetColumns(unc, {4097, 0, 0}) &&
        !prefs.SetColumns(unc, {std::numeric_limits<float>::infinity(), 0, 0}) &&
        !prefs.SetColumns(L"pulse:home", {100, 100, 100}), "invalid values and virtual paths cannot enter preferences");
    std::wstring json = L"{\"version\":1";
    prefs.AppendJson(json);
    json += L"}";
    app::FolderViewPrefs restored;
    restored.ReadJson(json);
    const std::wstring unc_alias = L"\\\\?\\UNC\\SERVER\\SHARE\\folder\\";
    Check(json::Document(json).Valid() && restored.Find(unc_alias) == ui::ViewMode::Tiles &&
        restored.FindSort(unc_alias) == prefs.FindSort(unc) && restored.FindGroup(unc_alias) == prefs.FindGroup(unc) &&
        restored.FindColumns(unc_alias) == prefs.FindColumns(unc), "new JSON round trips preferences across UNC spelling and case");
    restored.ReadJson(L"{\"folder_view_tiles\":[\"C:\\\\old\"],\"folder_sort_size_desc\":[\"C:\\\\old\"]}");
    Check(restored.Find(L"C:\\old") == ui::ViewMode::Tiles && restored.FindSort(L"C:\\old").has_value() &&
        !restored.FindGroup(L"C:\\old") && !restored.FindColumns(L"C:\\old"), "old JSON retains explicit view and sort with default grouping and columns");
    restored.ReadJson(L"{\"folder_columns\":[{\"path\":\"C:\\\\legacy\",\"widths\":[0.3,0.6,0.9]},"
        L"{\"path\":\"C:\\\\bad\",\"widths\":[1e100,0,0]},"
        L"{\"path\":\"C:\\\\bad2\",\"widths\":[100,\"200\",300]}]}");
    Check(restored.FindColumns(L"C:\\legacy").has_value() && !restored.FindColumns(L"C:\\bad") &&
        !restored.FindColumns(L"C:\\bad2"), "legacy normalized widths remain readable and corrupt persisted widths are rejected");
    prefs.SetColumns(unc, {160, 120, 96, 88});
    std::wstring cloud_json = L"{\"version\":1";
    prefs.AppendJson(cloud_json);
    cloud_json += L"}";
    restored.ReadJson(cloud_json);
    Check(restored.FindColumns(unc) == prefs.FindColumns(unc), "cloud status width round trips with existing metadata widths");
    std::printf("%d failures\n", failures);
    return failures ? 1 : 0;
}
