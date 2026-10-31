// SPDX-License-Identifier: Apache-2.0
#include "../ui/item_reflow_rows.h"
#include <cstdio>

namespace {
using namespace pulse::ui;
int failures = 0;

void Check(const PaneViewModel& vm, const ItemReflowSurface& surface,
        const std::vector<int>& expected, const char* label) {
    const auto actual = FindRetainedReflowRows(vm, surface, {0, 0, 400, 400});
    const bool passed = actual == expected;
    std::printf("[%s] %s\n", passed ? "PASS" : "FAIL", label);
    failures += !passed;
}
}

int main() {
    PaneViewModel vm;
    vm.is_file_system = true;
    vm.path = L"C:\\fixture";
    auto snapshot = std::make_shared<std::vector<pulse::fs::DirEntry>>(100000);
    for (size_t i = 0; i < snapshot->size(); ++i)
        (*snapshot)[i].name = L"row-" + std::to_wstring(i);
    (*snapshot)[99990].name = L"same.txt";
    (*snapshot)[99990].full_path = L"C:\\first\\same.txt";
    (*snapshot)[99995].name = L"same.txt";
    (*snapshot)[99995].full_path = L"C:\\second\\same.txt";
    vm.snapshot = snapshot;
    std::vector<ItemReflowSlot> slots{
        {L"C:\\first\\same.txt\nsame.txt", {0, 0, 400, 20}, 99990},
        {L"C:\\second\\same.txt\nsame.txt", {0, 20, 400, 40}, 99995},
        {L"C:\\fixture\\row-99999\nrow-99999", {0, 40, 400, 60}, 99999},
        {L"C:\\fixture\\row-1\nrow-1", {0, 500, 400, 520}, 1}};
    ItemReflowSurface surface;
    surface.Begin(slots, L"fixture", {0, 0, 400, 400}, 0, true);
    Check(vm, surface, {99990, 99995, 99999}, "large snapshot resolves exact paths and joined filesystem paths");
    vm.filter_text = L"same";
    vm.filter_map = std::make_shared<const PaneViewModel::FilterMap>(
        PaneViewModel::FilterMap{99990, 99995, 99999});
    Check(vm, surface, {0, 1, 2}, "sparse filter resolves only its resident source rows");
    vm.filter_map = std::make_shared<const PaneViewModel::FilterMap>(
        PaneViewModel::FilterMap{99999, 99995, 99990});
    auto inverse = std::make_shared<PaneViewModel::FilterMap>(100000, -1);
    (*inverse)[99990] = 2; (*inverse)[99995] = 1; (*inverse)[99999] = 0;
    vm.source_to_view = inverse;
    Check(vm, surface, {2, 1, 0}, "reordered filter preserves source-order retained rows");
    vm.filter_map.reset();
    vm.source_to_view.reset();
    Check(vm, surface, {}, "pending filter has no retained source rows");
    vm.filter_text.clear();
    vm.snapshot.reset();
    for (size_t i : {99990u, 99995u, 99999u}) {
        ListEntryView entry;
        entry.name = (*snapshot)[i].name;
        entry.path = (*snapshot)[i].full_path;
        vm.entries.push_back(std::move(entry));
    }
    Check(vm, surface, {0, 1, 2}, "entry-backed search rows use the same identity rules");
    return failures ? 1 : 0;
}
