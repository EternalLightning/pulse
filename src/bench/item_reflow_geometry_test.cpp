// SPDX-License-Identifier: Apache-2.0
#include "../ui/item_reflow_geometry.h"
#include <cstdio>

namespace {
using namespace pulse::ui;
int failures = 0;

bool Equal(D2D1_RECT_F a, D2D1_RECT_F b) {
    return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
}

D2D1_RECT_F ReferenceVacancy(const std::vector<ItemReflowSlot>& slots,
        const ItemReflowAnimation& animation, const std::wstring& key, D2D1_RECT_F bounds) {
    auto available = bounds;
    for (const auto& slot : slots) {
        if (slot.key == key) continue;
        const auto other = animation.Visual(slot.key);
        if (other.opacity <= 0 || other.exiting) continue;
        const bool same_column = slot.bounds.right > bounds.left && slot.bounds.left < bounds.right;
        const bool same_row = slot.bounds.bottom > bounds.top && slot.bounds.top < bounds.bottom;
        if (same_column && slot.bounds.top >= bounds.bottom)
            available.bottom = (std::min)(available.bottom, other.bounds.top);
        if (same_column && slot.bounds.bottom <= bounds.top)
            available.top = (std::max)(available.top, other.bounds.bottom);
        if (same_row && slot.bounds.left >= bounds.right)
            available.right = (std::min)(available.right, other.bounds.left);
        if (same_row && slot.bounds.right <= bounds.left)
            available.left = (std::max)(available.left, other.bounds.right);
    }
    return available;
}

D2D1_RECT_F ReferenceRemoved(const std::vector<ItemReflowSlot>& slots,
        const ItemReflowAnimation& animation, D2D1_RECT_F bounds) {
    auto vacancy = bounds;
    for (const auto& slot : slots) {
        const auto other = animation.Visual(slot.key);
        if (other.opacity <= 0 || other.bounds.right <= bounds.left ||
            other.bounds.left >= bounds.right) continue;
        if (other.bounds.top >= bounds.top)
            vacancy.bottom = (std::min)(vacancy.bottom, other.bounds.top);
        else if (other.bounds.bottom <= bounds.bottom)
            vacancy.top = (std::max)(vacancy.top, other.bounds.bottom);
        else vacancy.bottom = vacancy.top;
    }
    return vacancy;
}

void CheckFrame(const std::vector<ItemReflowSlot>& slots, const ItemReflowAnimation& animation,
        ItemReflowGeometry& geometry) {
    geometry.Update(slots, animation);
    for (const auto& slot : slots) {
        const auto expected = ReferenceVacancy(slots, animation, slot.key, slot.bounds);
        // Background and foreground must share the same cached answer.
        failures += !Equal(geometry.Vacancy(slot.key, slot.bounds), expected);
        failures += !Equal(geometry.Vacancy(slot.key, slot.bounds), expected);
        auto shifted = slot.bounds;
        shifted.left += 7; shifted.right += 7;
        failures += !Equal(geometry.Vacancy(slot.key, shifted),
            ReferenceVacancy(slots, animation, slot.key, shifted));
    }
    for (const auto& [key, visual] : animation.Visuals()) {
        if (visual.exiting)
            failures += !Equal(geometry.RemovedVacancy(visual.bounds),
                ReferenceRemoved(slots, animation, visual.bounds));
    }
    const D2D1_RECT_F unknown{1, 2, 39, 18};
    failures += !Equal(geometry.Vacancy(L"not present", unknown),
        ReferenceVacancy(slots, animation, L"not present", unknown));
}

void Timeline(int count, int columns, bool irregular) {
    std::vector<ItemReflowSlot> initial, changed;
    for (int i = 0; i < count; ++i) {
        const float x = (i % columns) * 48.0f;
        const float y = (i / columns) * 24.0f;
        initial.push_back({L"item-" + std::to_wstring(i), {x, y, x + 40, y + 20}, i, true});
    }
    changed = initial;
    for (int i = 0; i < count; ++i) {
        auto& slot = changed[static_cast<size_t>(i)];
        if (i % 3 == 0) slot.key = L"new-" + std::to_wstring(i);
        else slot.key = initial[static_cast<size_t>((i + count / 2) % count)].key;
        if (irregular) {
            const float dx = ((i * 17) % 13) * 7.0f;
            const float dy = ((i * 31) % 11) * 5.0f;
            slot.bounds.left += dx; slot.bounds.right += dx;
            slot.bounds.top += dy; slot.bounds.bottom += dy;
        }
    }
    ItemReflowAnimation animation;
    ItemReflowGeometry geometry;
    const D2D1_RECT_F viewport{0, 0, 1000, 900};
    animation.Update(initial, 0, true, false, &viewport);
    CheckFrame(initial, animation, geometry);
    for (uint64_t now : {10ull, 26ull, 74ull, 130ull, 190ull, 250ull}) {
        animation.Update(changed, now, true, false, &viewport);
        CheckFrame(changed, animation, geometry);
    }
    std::reverse(changed.begin(), changed.end());
    for (size_t i = 0; i < changed.size(); ++i) changed[i].bounds = initial[i].bounds;
    for (uint64_t now : {260ull, 276ull, 340ull, 500ull}) {
        animation.Update(changed, now, true, false, &viewport);
        CheckFrame(changed, animation, geometry);
    }
    changed.resize(static_cast<size_t>(count / 2));
    for (uint64_t now : {510ull, 550ull, 590ull, 750ull}) {
        animation.Update(changed, now, true, false, &viewport);
        CheckFrame(changed, animation, geometry);
    }
    geometry.Reset();
    const D2D1_RECT_F unknown{1, 2, 39, 18};
    failures += !Equal(geometry.Vacancy(L"not present", unknown), unknown);
}

void DuplicateKeysAndDistantBlockers() {
    std::vector<ItemReflowSlot> slots{
        {L"same", {0, 0, 40, 20}, 0},
        {L"same", {0, 60, 40, 80}, 1},
        {L"near", {0, 80, 40, 100}, 2},
        {L"far", {0, 10, 40, 30}, 3}};
    ItemReflowAnimation animation;
    ItemReflowGeometry geometry;
    animation.Update(slots, 0, true);
    CheckFrame(slots, animation, geometry);
    slots[2].bounds = {0, 20, 40, 40};
    slots[3].bounds = {0, 40, 40, 60};
    animation.Update(slots, 10, true);
    animation.Update(slots, 50, true);
    CheckFrame(slots, animation, geometry);
}
}

int main() {
    for (int count : {1, 37, 512}) {
        Timeline(count, 1, false);
        Timeline(count, 7, false);
        Timeline(count, 9, true);
    }
    DuplicateKeysAndDistantBlockers();
    std::printf("[%s] exact vacancy equivalence for list/grid/irregular layouts, replacement, "
        "wrap, deletion, retarget and 512-slot capacity (%d mismatches)\n",
        failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
