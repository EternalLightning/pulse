// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "../ui/item_reflow_animation.h"
#include <cmath>
#include <cstdio>

namespace {

using pulse::ui::ItemReflowAnimation;
using pulse::ui::ItemReflowSlot;

int failures = 0;

void Check(bool condition, const char* name) {
    std::printf("[%s] %s\n", condition ? "PASS" : "FAIL", name);
    if (!condition) ++failures;
}

bool Near(float actual, float expected) {
    return std::fabs(actual - expected) < 0.001f;
}

ItemReflowSlot Slot(const wchar_t* key, float left, float top, int source = 0) {
    return {key, {left, top, left + 40.0f, top + 20.0f}, source};
}

void MiddleInsertAndDelete() {
    ItemReflowAnimation animation;
    const std::vector<ItemReflowSlot> original{
        Slot(L"a", 0, 0), Slot(L"b", 0, 20, 1), Slot(L"c", 0, 40, 2)};
    const std::vector<ItemReflowSlot> inserted{
        Slot(L"a", 0, 0), Slot(L"new", 0, 20, 1),
        Slot(L"b", 0, 40, 2), Slot(L"c", 0, 60, 3)};
    animation.Update(original, 100, true);
    Check(!animation.Pending() && Near(animation.Visual(L"b").bounds.top, 20),
          "initial snapshot snaps without animation");
    animation.Update(inserted, 110, true);
    Check(animation.Pending() && Near(animation.Visual(L"b").bounds.top, 20) &&
          Near(animation.Visual(L"new").opacity, 0) &&
          Near(animation.Visual(L"new").bounds.top, 20),
          "middle insertion begins at old sibling positions and fades new item in place");
    animation.Update(inserted, 230, true);
    Check(Near(animation.Visual(L"b").bounds.top, 30) &&
          Near(animation.Visual(L"c").bounds.top, 50) &&
          Near(animation.Visual(L"new").opacity, 0),
          "middle insertion moves later siblings before revealing the new label");
    animation.Update(inserted, 290, true);
    Check(Near(animation.Visual(L"new").opacity, 0.5f) &&
          Near(animation.Visual(L"new").bounds.top, 20),
          "new label fades in place over the final 120 ms");
    animation.Update(inserted, 350, true);
    Check(!animation.Pending() && Near(animation.Visual(L"b").bounds.top, 40) &&
          Near(animation.Visual(L"new").opacity, 1),
          "movement and entry reach exact endpoints at 240 ms");

    animation.Update(original, 360, true);
    Check(animation.Pending() && animation.Visual(L"new").exiting &&
          animation.Visual(L"new").source == 1 &&
          animation.Visual(L"b").source == 1 &&
          Near(animation.Visual(L"new").bounds.top, 20),
          "delete retains outgoing rectangle and source while live source updates");
    animation.Update(original, 400, true);
    Check(Near(animation.Visual(L"new").opacity, 0.5f) &&
          animation.Visual(L"b").bounds.top < 40 &&
          animation.Visual(L"b").bounds.top > 20,
          "deleted item fades quickly while later siblings move upward");
    animation.Update(original, 440, true);
    Check(animation.Visuals().count(L"new") == 0 && animation.Pending(),
          "exit is removed after 80 ms while sibling movement continues");
    animation.Update(original, 600, true);
    Check(!animation.Pending() && Near(animation.Visual(L"b").bounds.top, 20),
          "deletion settles at the compacted layout");
}

void NonadjacentChanges() {
    const std::vector<ItemReflowSlot> original{
        Slot(L"a", 0, 0), Slot(L"b", 0, 20, 1), Slot(L"c", 0, 40, 2),
        Slot(L"d", 0, 60, 3), Slot(L"e", 0, 80, 4), Slot(L"f", 0, 100, 5)};
    const std::vector<ItemReflowSlot> compacted{
        Slot(L"a", 0, 0), Slot(L"c", 0, 20, 1),
        Slot(L"e", 0, 40, 2), Slot(L"f", 0, 60, 3)};
    ItemReflowAnimation deletion;
    deletion.Update(original, 0, true);
    deletion.Update(compacted, 10, true);
    Check(deletion.Pending() && Near(deletion.Visual(L"a").bounds.top, 0) &&
          Near(deletion.Visual(L"c").bounds.top, 40) &&
          Near(deletion.Visual(L"e").bounds.top, 80) &&
          Near(deletion.Visual(L"f").bounds.top, 100) &&
          deletion.Visual(L"b").exiting && deletion.Visual(L"d").exiting,
          "nonadjacent B/D deletion starts every surviving identity at its old position");
    deletion.Update(compacted, 130, true);
    Check(Near(deletion.Visual(L"c").bounds.top, 30) &&
          Near(deletion.Visual(L"e").bounds.top, 60) &&
          Near(deletion.Visual(L"f").bounds.top, 80) &&
          deletion.Visuals().count(L"b") == 0 && deletion.Visuals().count(L"d") == 0,
          "nonadjacent deletion moves C one row and E/F two rows with shared progress");
    deletion.Update(compacted, 250, true);
    Check(!deletion.Pending() && Near(deletion.Visual(L"c").bounds.top, 20) &&
          Near(deletion.Visual(L"e").bounds.top, 40) &&
          Near(deletion.Visual(L"f").bounds.top, 60),
          "nonadjacent deletion reaches each identity's accumulated row offset");

    auto mixed_original = original;
    mixed_original.push_back(Slot(L"g", 0, 120, 6));
    mixed_original.push_back(Slot(L"h", 0, 140, 7));
    const std::vector<ItemReflowSlot> mixed_final{
        Slot(L"x", 0, 0), Slot(L"a", 0, 20, 1), Slot(L"c", 0, 40, 2),
        Slot(L"e", 0, 60, 3), Slot(L"f", 0, 80, 4), Slot(L"y", 0, 100, 5),
        Slot(L"h", 0, 120, 6), Slot(L"z", 0, 140, 7)};
    ItemReflowAnimation mixed;
    mixed.Update(mixed_original, 0, true);
    mixed.Update(mixed_final, 10, true);
    Check(mixed.Pending() && Near(mixed.Visual(L"a").bounds.top, 0) &&
          Near(mixed.Visual(L"c").bounds.top, 40) &&
          Near(mixed.Visual(L"e").bounds.top, 80) &&
          Near(mixed.Visual(L"f").bounds.top, 100) &&
          Near(mixed.Visual(L"h").bounds.top, 140) &&
          mixed.Visual(L"b").exiting && mixed.Visual(L"d").exiting &&
          mixed.Visual(L"g").exiting,
          "mixed nonadjacent inserts/deletes preserve all stable identities at change time");
    mixed.Update(mixed_final, 130, true);
    Check(Near(mixed.Visual(L"a").bounds.top, 10) &&
          Near(mixed.Visual(L"c").bounds.top, 40) &&
          Near(mixed.Visual(L"e").bounds.top, 70) &&
          Near(mixed.Visual(L"f").bounds.top, 90) &&
          Near(mixed.Visual(L"h").bounds.top, 130) &&
          Near(mixed.Visual(L"x").opacity, 0) && Near(mixed.Visual(L"y").opacity, 0) &&
          Near(mixed.Visual(L"z").opacity, 0),
          "mixed changes derive positive zero and negative net offsets per identity");
    mixed.Update(mixed_final, 190, true);
    Check(Near(mixed.Visual(L"x").opacity, 0.5f) &&
          Near(mixed.Visual(L"y").opacity, 0.5f) &&
          Near(mixed.Visual(L"z").opacity, 0.5f) &&
          Near(mixed.Visual(L"x").bounds.top, 0) &&
          Near(mixed.Visual(L"y").bounds.top, 100) &&
          Near(mixed.Visual(L"z").bounds.top, 140),
          "multiple nonadjacent insertions reveal in their own target slots late in reflow");
    mixed.Update(mixed_final, 250, true);
    bool endpoints = !mixed.Pending() && mixed.Visuals().size() == mixed_final.size();
    for (const auto& slot : mixed_final) {
        const auto visual = mixed.Visual(slot.key);
        endpoints = endpoints && Near(visual.bounds.top, slot.bounds.top) &&
                    Near(visual.opacity, 1) && visual.source == slot.source && !visual.exiting;
    }
    Check(endpoints, "mixed nonadjacent changes settle at the complete final identity layout");
}

void GridAndRapidChanges() {
    ItemReflowAnimation animation;
    animation.Update({Slot(L"a", 80, 0)}, 1000, true);
    animation.Update({Slot(L"a", 0, 20)}, 1010, true);
    animation.Update({Slot(L"a", 0, 20)}, 1130, true);
    const auto before = animation.Visual(L"a");
    Check(Near(before.bounds.left, 40) && Near(before.bounds.top, 10) &&
          Near(before.bounds.right - before.bounds.left, 40),
          "grid wrap interpolates horizontal and vertical position without resizing");
    animation.Update({Slot(L"a", 120, 40)}, 1130, true);
    Check(Near(animation.Visual(L"a").bounds.left, before.bounds.left) &&
          Near(animation.Visual(L"a").bounds.top, before.bounds.top),
          "interrupted movement retargets continuously from the sampled position");
    animation.Update({}, 1170, true);
    const auto outgoing = animation.Visual(L"a");
    Check(outgoing.exiting && outgoing.bounds.left > before.bounds.left &&
          outgoing.bounds.left < 120,
          "deletion during movement freezes the current sampled rectangle");
    animation.Update({}, 1210, true);
    const auto faded = animation.Visual(L"a");
    animation.Update({Slot(L"a", 40, 60, 7)}, 1210, true);
    Check(!animation.Visual(L"a").exiting && animation.Visual(L"a").source == 7 &&
          Near(animation.Visual(L"a").bounds.left, faded.bounds.left) &&
          Near(animation.Visual(L"a").opacity, faded.opacity),
          "rapid re-add preserves both current position and current opacity");
    animation.Update({Slot(L"a", 40, 60, 7)}, 1450, true);
    Check(!animation.Pending() && Near(animation.Visual(L"a").bounds.left, 40) &&
          Near(animation.Visual(L"a").bounds.top, 60) &&
          Near(animation.Visual(L"a").opacity, 1),
          "re-added item reaches its new endpoint");
}

void GridRowWrap() {
    auto before = Slot(L"wrap", 80, 0);
    auto after = Slot(L"wrap", 0, 20);
    before.wrap_rows = true;
    after.wrap_rows = true;
    ItemReflowAnimation insertion;
    insertion.Update({before}, 0, true);
    insertion.Update({after}, 10, true);
    Check(insertion.Pending() && Near(insertion.Visual(L"wrap").bounds.left, 80) &&
          Near(insertion.Visual(L"wrap").bounds.top, 0) &&
          Near(insertion.Visual(L"wrap").opacity, 1),
          "grid insertion wrap starts continuously at the old row edge");
    insertion.Update({after}, 70, true);
    Check(Near(insertion.Visual(L"wrap").bounds.left, 100) &&
          Near(insertion.Visual(L"wrap").bounds.top, 0) &&
          Near(insertion.Visual(L"wrap").opacity, 0.5f),
          "grid insertion wrap exits horizontally on the old row without a diagonal");
    insertion.Update({after}, 130, true);
    Check(insertion.Pending() && Near(insertion.Visual(L"wrap").bounds.left, -40) &&
          Near(insertion.Visual(L"wrap").bounds.top, 20) &&
          Near(insertion.Visual(L"wrap").opacity, 0),
          "grid insertion wrap changes rows invisibly at 120 ms");
    insertion.Update({after}, 190, true);
    Check(Near(insertion.Visual(L"wrap").bounds.left, -20) &&
          Near(insertion.Visual(L"wrap").bounds.top, 20) &&
          Near(insertion.Visual(L"wrap").opacity, 0.5f),
          "grid insertion wrap enters horizontally along the target row");
    insertion.Update({after}, 250, true);
    Check(!insertion.Pending() && Near(insertion.Visual(L"wrap").bounds.left, 0) &&
          Near(insertion.Visual(L"wrap").bounds.top, 20) &&
          Near(insertion.Visual(L"wrap").opacity, 1),
          "grid wrap repeated frames retain the exact 240 ms endpoint");

    ItemReflowAnimation deletion;
    deletion.Update({after}, 0, true);
    deletion.Update({before}, 10, true);
    deletion.Update({before}, 70, true);
    Check(Near(deletion.Visual(L"wrap").bounds.left, -20) &&
          Near(deletion.Visual(L"wrap").bounds.top, 20) &&
          Near(deletion.Visual(L"wrap").opacity, 0.5f),
          "grid deletion wrap exits left while remaining on its original row");
    deletion.Update({before}, 130, true);
    Check(Near(deletion.Visual(L"wrap").bounds.left, 120) &&
          Near(deletion.Visual(L"wrap").bounds.top, 0) &&
          Near(deletion.Visual(L"wrap").opacity, 0),
          "grid deletion wrap changes to the previous row only while invisible");
    deletion.Update({before}, 190, true);
    Check(Near(deletion.Visual(L"wrap").bounds.left, 100) &&
          Near(deletion.Visual(L"wrap").bounds.top, 0) &&
          Near(deletion.Visual(L"wrap").opacity, 0.5f),
          "grid deletion wrap enters from the target row's right edge");
    deletion.Update({before}, 250, true);
    Check(!deletion.Pending() && Near(deletion.Visual(L"wrap").bounds.left, 80) &&
          Near(deletion.Visual(L"wrap").bounds.top, 0) &&
          Near(deletion.Visual(L"wrap").opacity, 1),
          "grid deletion wrap reaches its exact endpoint");

    ItemReflowAnimation interrupted;
    interrupted.Update({before}, 0, true);
    interrupted.Update({after}, 10, true);
    interrupted.Update({after}, 70, true);
    const auto sampled = interrupted.Visual(L"wrap");
    auto retarget = Slot(L"wrap", 0, 40);
    retarget.wrap_rows = true;
    interrupted.Update({retarget}, 70, true);
    Check(Near(interrupted.Visual(L"wrap").bounds.left, sampled.bounds.left) &&
          Near(interrupted.Visual(L"wrap").bounds.top, sampled.bounds.top) &&
          Near(interrupted.Visual(L"wrap").opacity, sampled.opacity),
          "interrupted grid wrap preserves the current position and opacity");
    interrupted.Update({retarget}, 130, true);
    Check(Near(interrupted.Visual(L"wrap").bounds.left, -40) &&
          Near(interrupted.Visual(L"wrap").bounds.top, 40) &&
          Near(interrupted.Visual(L"wrap").opacity, 0),
          "retargeted wrap uses the original invisible row-change deadline");
    interrupted.Update({retarget}, 190, true);
    const auto reentry = interrupted.Visual(L"wrap");
    interrupted.Update({}, 190, true);
    Check(interrupted.Visual(L"wrap").exiting &&
          Near(interrupted.Visual(L"wrap").bounds.left, reentry.bounds.left) &&
          Near(interrupted.Visual(L"wrap").bounds.top, reentry.bounds.top) &&
          Near(interrupted.Visual(L"wrap").opacity, reentry.opacity),
          "deletion during wrap reentry freezes its sampled rectangle and opacity");
    interrupted.Update({}, 210, true);
    const auto faded = interrupted.Visual(L"wrap");
    interrupted.Update({retarget}, 210, true);
    Check(!interrupted.Visual(L"wrap").exiting &&
          Near(interrupted.Visual(L"wrap").bounds.left, faded.bounds.left) &&
          Near(interrupted.Visual(L"wrap").bounds.top, faded.bounds.top) &&
          Near(interrupted.Visual(L"wrap").opacity, faded.opacity),
          "re-add during interrupted wrap stays continuous from the exit sample");
    interrupted.Update({retarget}, 450, true);
    Check(!interrupted.Pending() && Near(interrupted.Visual(L"wrap").bounds.left, 0) &&
          Near(interrupted.Visual(L"wrap").bounds.top, 40) &&
          Near(interrupted.Visual(L"wrap").opacity, 1),
          "interrupted wrap and re-add eventually settle at the final slot");
}

void FrameCadence() {
    ItemReflowAnimation sparse;
    ItemReflowAnimation frequent;
    const std::vector<ItemReflowSlot> from{Slot(L"a", 0, 0)};
    const std::vector<ItemReflowSlot> to{Slot(L"a", 180, 0)};
    sparse.Update(from, 0, true);
    frequent.Update(from, 0, true);
    sparse.Update(to, 10, true);
    frequent.Update(to, 10, true);
    for (uint64_t now = 20; now < 130; now += 10) frequent.Update(to, now, true);
    sparse.Update(to, 130, true);
    frequent.Update(to, 130, true);
    Check(Near(sparse.Visual(L"a").bounds.left, 90) &&
          Near(sparse.Visual(L"a").bounds.left, frequent.Visual(L"a").bounds.left),
          "unchanged target does not restart animation on every frame");
    frequent.Update(to, 250, true);
    Check(!frequent.Pending() && Near(frequent.Visual(L"a").bounds.left, 180),
          "frequent frames settle on the original deadline");
}

void VelocityAndBounds() {
    ItemReflowAnimation fresh;
    fresh.Update({Slot(L"a", 0, 0)}, 0, true);
    fresh.Update({Slot(L"a", 20, 0)}, 10, true);
    fresh.Update({Slot(L"a", 20, 0)}, 11, true);
    const float first_step = fresh.Visual(L"a").bounds.left;
    fresh.Update({Slot(L"a", 20, 0)}, 249, true);
    const float last_step = 20.0f - fresh.Visual(L"a").bounds.left;
    fresh.Update({Slot(L"a", 20, 0)}, 250, true);
    Check(first_step > 0 && first_step < 0.005f &&
          last_step > 0 && last_step < 0.005f && !fresh.Pending(),
          "fresh movement starts and ends near zero velocity with a finite endpoint");

    ItemReflowAnimation continued;
    continued.Update({Slot(L"a", 0, 0)}, 0, true);
    continued.Update({Slot(L"a", 40, 0)}, 10, true);
    continued.Update({Slot(L"a", 40, 0)}, 129, true);
    const float previous = continued.Visual(L"a").bounds.left;
    continued.Update({Slot(L"a", 40, 0)}, 130, true);
    const float position = continued.Visual(L"a").bounds.left;
    continued.Update({Slot(L"a", 80, 0)}, 130, true);
    Check(Near(continued.Visual(L"a").bounds.left, position),
          "same-direction retarget preserves the exact sampled position");
    continued.Update({Slot(L"a", 80, 0)}, 131, true);
    const float before_velocity = position - previous;
    const float after_velocity = continued.Visual(L"a").bounds.left - position;
    Check(std::fabs(before_velocity - 0.25f) < 0.005f &&
          std::fabs(after_velocity - before_velocity) < 0.005f,
          "same-direction retarget carries velocity when its slope bound is not binding");
    continued.Update({Slot(L"a", 80, 0)}, 370, true);
    Check(!continued.Pending() && Near(continued.Visual(L"a").bounds.left, 80),
          "retarget reaches its exact endpoint 240 ms after the actual change");

    ItemReflowAnimation reversed;
    reversed.Update({Slot(L"a", 0, 0)}, 0, true);
    reversed.Update({Slot(L"a", 40, 0)}, 10, true);
    reversed.Update({Slot(L"a", 0, 0)}, 130, true);
    float last = reversed.Visual(L"a").bounds.left;
    bool reversal_bounded = Near(last, 20);
    for (uint64_t now = 131; now <= 370; ++now) {
        reversed.Update({Slot(L"a", 0, 0)}, now, true);
        const float current = reversed.Visual(L"a").bounds.left;
        reversal_bounded = reversal_bounded && current >= 0 && current <= last;
        last = current;
    }
    Check(reversal_bounded && !reversed.Pending() && Near(last, 0),
          "reversal brakes outward velocity instead of overshooting the sampled bounds");

    ItemReflowAnimation capped;
    capped.Update({Slot(L"a", 0, 0)}, 0, true);
    capped.Update({Slot(L"a", 200, 0)}, 10, true);
    capped.Update({Slot(L"a", 101, 0)}, 130, true);
    bool cap_bounded = Near(capped.Visual(L"a").bounds.left, 100);
    last = 100;
    for (uint64_t now = 131; now <= 370; ++now) {
        capped.Update({Slot(L"a", 101, 0)}, now, true);
        const float current = capped.Visual(L"a").bounds.left;
        cap_bounded = cap_bounded && current >= last && current <= 101;
        last = current;
    }
    Check(cap_bounded && !capped.Pending() && Near(last, 101),
          "short same-direction retarget caps carried velocity to prevent overshoot");
}

void IndependentEntryAndWrapDeadlines() {
    ItemReflowAnimation entry;
    entry.Update({}, 0, true);
    entry.Update({Slot(L"new", 0, 20)}, 10, true);
    entry.Update({Slot(L"new", 40, 20)}, 70, true);
    entry.Update({Slot(L"new", 40, 20)}, 130, true);
    Check(Near(entry.Visual(L"new").opacity, 0) &&
          entry.Visual(L"new").bounds.left > 0,
          "geometric retarget during entry hold does not reveal the new item early");
    entry.Update({Slot(L"new", 40, 20)}, 190, true);
    Check(Near(entry.Visual(L"new").opacity, 0.5f),
          "entry fade keeps its creation clock after a geometric retarget");
    entry.Update({Slot(L"new", 40, 20)}, 250, true);
    Check(Near(entry.Visual(L"new").opacity, 1) && entry.Pending(),
          "entry opacity settles at creation plus 240 ms while retargeted geometry continues");
    entry.Update({Slot(L"new", 40, 20)}, 310, true);
    Check(!entry.Pending() && Near(entry.Visual(L"new").bounds.left, 40),
          "independent entry and geometry tracks both reach finite deadlines");

    auto before = Slot(L"wrap", 80, 0);
    auto target = Slot(L"wrap", 0, 20);
    before.wrap_rows = target.wrap_rows = true;
    ItemReflowAnimation wrap;
    wrap.Update({before}, 0, true);
    wrap.Update({target}, 10, true);
    for (uint64_t now = 30; now < 130; now += 20) {
        target = Slot(L"wrap", 0, static_cast<float>(now));
        target.wrap_rows = true;
        wrap.Update({target}, now, true);
    }
    wrap.Update({target}, 130, true);
    Check(Near(wrap.Visual(L"wrap").bounds.top, target.bounds.top) &&
          Near(wrap.Visual(L"wrap").opacity, 0),
          "frequent outgoing wrap retargets keep the original invisible row-change time");
    wrap.Update({target}, 190, true);
    const auto incoming = wrap.Visual(L"wrap");
    target = Slot(L"wrap", 40, target.bounds.top);
    target.wrap_rows = true;
    wrap.Update({target}, 190, true);
    Check(Near(wrap.Visual(L"wrap").bounds.left, incoming.bounds.left) &&
          Near(wrap.Visual(L"wrap").opacity, 0.5f),
          "incoming wrap retarget starts at the current position without another fade-out");
    for (uint64_t now = 200; now < 250; now += 10) {
        target.bounds.left += 1;
        target.bounds.right += 1;
        wrap.Update({target}, now, true);
    }
    wrap.Update({target}, 250, true);
    Check(!wrap.Pending() && Near(wrap.Visual(L"wrap").bounds.left, target.bounds.left) &&
          Near(wrap.Visual(L"wrap").opacity, 1),
          "frequent incoming wrap retargets still finish on the original wrap deadline");
}

void ResetAndDisabledMotion() {
    ItemReflowAnimation animation;
    animation.Update({Slot(L"old", 0, 0)}, 0, true);
    animation.Update({Slot(L"new", 40, 20)}, 10, true);
    animation.Update({Slot(L"destination", 80, 40)}, 20, true, true);
    Check(!animation.Pending() && animation.Visuals().size() == 1 &&
          Near(animation.Visual(L"destination").opacity, 1),
          "navigation reset discards outgoing items and snaps destination");
    animation.Update({Slot(L"destination", 120, 40)}, 30, true);
    animation.Update({Slot(L"replacement", 160, 40)}, 40, false);
    Check(!animation.Pending() && animation.Visuals().size() == 1 &&
          Near(animation.Visual(L"replacement").bounds.left, 160) &&
          Near(animation.Visual(L"replacement").opacity, 1),
          "disabled motion or high contrast snaps and discards outgoing items");
    animation.Reset();
    Check(!animation.Pending() && animation.Visuals().empty(),
          "explicit reset clears all retained visuals");
    animation.Update({Slot(L"first", 20, 20)}, 50, true);
    Check(!animation.Pending() && Near(animation.Visual(L"first").opacity, 1),
          "first update after reset snaps");
    animation.Update({}, 60, true);
    animation.Update({}, 140, true);
    Check(!animation.Pending() && animation.Visuals().empty(),
          "empty layout settles when its last outgoing item disappears");
    animation.Update({Slot(L"brief", 0, 0)}, 150, true);
    animation.Update({}, 150, true);
    Check(!animation.Pending() && animation.Visuals().empty(),
          "zero-opacity insertion removed immediately leaves no stranded exit");
}

void ZebraTransitions() {
    const auto striped = [](const wchar_t* key, int row, float shade) {
        auto slot = Slot(key, 0, static_cast<float>(row * 20), row);
        slot.stripe = shade;
        return slot;
    };
    ItemReflowAnimation animation;
    const std::vector<ItemReflowSlot> before{striped(L"a", 0, 0), striped(L"b", 1, 1),
        striped(L"c", 2, 0), striped(L"d", 3, 1), striped(L"e", 4, 0)};
    auto after = std::vector<ItemReflowSlot>{striped(L"a", 0, 0), striped(L"c", 1, 1),
        striped(L"d", 2, 0), striped(L"e", 3, 1)};
    animation.Update(before, 1000, true);
    animation.Update(after, 1100, true);
    Check(Near(animation.Visual(L"c").stripe, 0) && Near(animation.Visual(L"d").stripe, 1),
        "delete parity changes begin at each surviving row's old shade");
    animation.Update(after, 1220, true);
    Check(Near(animation.Visual(L"c").stripe, 0.5f) && Near(animation.Visual(L"d").stripe, 0.5f),
        "opposite zebra shades crossfade while their rows move");
    animation.Update(after, 1340, true);
    Check(!animation.Pending() && Near(animation.Visual(L"c").stripe, 1) &&
        Near(animation.Visual(L"d").stripe, 0) && Near(animation.Visual(L"e").stripe, 1),
        "delete endpoint restores alternating shades instead of retaining file identity colors");
    after[1].stripe = 0;
    animation.Update(after, 1400, true);
    Check(animation.Pending() && Near(animation.Visual(L"c").stripe, 1),
        "shade-only changes schedule frames even when geometry is unchanged");
    animation.Update(after, 1520, true);
    const float middle = animation.Visual(L"c").stripe;
    after[1].stripe = 1;
    animation.Update(after, 1520, true);
    Check(Near(middle, 0.5f) && Near(animation.Visual(L"c").stripe, middle),
        "rapid zebra reversal continues from the sampled shade without flashing");
    animation.Update(after, 1640, true);
    Check(Near(animation.Visual(L"c").stripe, 0.75f), "reversed shade approaches its new endpoint smoothly");
    animation.Update(after, 1641, false);
    Check(!animation.Pending() && Near(animation.Visual(L"c").stripe, 1),
        "disabled motion snaps row shades and releases their frame scheduling");
    animation.Update(before, 1700, true, true);
    Check(!animation.Pending() && Near(animation.Visual(L"b").stripe, 1) &&
        Near(animation.Visual(L"c").stripe, 0), "navigation reset immediately applies destination zebra parity");
    ItemReflowAnimation grouped;
    grouped.Update({striped(L"g1a", 0, 0), striped(L"g1b", 1, 1), striped(L"g1c", 2, 0),
        striped(L"g2a", 3, 0), striped(L"g2b", 4, 1)}, 2000, true);
    const std::vector<ItemReflowSlot> reduced{striped(L"g1a", 0, 0), striped(L"g1c", 1, 1),
        striped(L"g2a", 2, 0), striped(L"g2b", 3, 1)};
    grouped.Update(reduced, 2100, true);
    grouped.Update(reduced, 2220, true);
    Check(Near(grouped.Visual(L"g1c").stripe, 0.5f) && Near(grouped.Visual(L"g2a").stripe, 0) &&
        Near(grouped.Visual(L"g2b").stripe, 1),
        "odd deletion within one group leaves the next group's shades unchanged");
    ItemReflowAnimation separated;
    auto all = before;
    all.push_back(striped(L"f", 5, 1));
    separated.Update(all, 3000, true);
    const std::vector<ItemReflowSlot> sparse{striped(L"a", 0, 0), striped(L"c", 1, 1),
        striped(L"e", 2, 0), striped(L"f", 3, 1)};
    separated.Update(sparse, 3100, true);
    separated.Update(sparse, 3220, true);
    Check(Near(separated.Visual(L"c").stripe, 0.5f) && Near(separated.Visual(L"e").stripe, 0) &&
        Near(separated.Visual(L"f").stripe, 1),
        "nonadjacent deletions crossfade only rows whose net parity changes");
}

void Capacity() {
    ItemReflowAnimation animation;
    std::vector<ItemReflowSlot> original;
    std::vector<ItemReflowSlot> replacement;
    for (int i = 0; i < 512; ++i) {
        original.push_back({L"old" + std::to_wstring(i), {0, 0, 40, 20}, i});
        replacement.push_back({L"new" + std::to_wstring(i), {40, 20, 80, 40}, i});
    }
    animation.Update(original, 0, true);
    animation.Update(replacement, 10, true);
    Check(animation.Pending() && animation.Visuals().size() == 512 &&
          animation.Visuals().count(L"new511") == 1 &&
          animation.Visuals().count(L"old0") == 0,
          "visible items take capacity priority over outgoing visuals");
    replacement.push_back(Slot(L"overflow", 80, 40));
    animation.Update(replacement, 20, true);
    Check(!animation.Pending() && animation.Visuals().size() == 512 &&
          Near(animation.Visual(L"new511").opacity, 1),
          "oversized input snaps at most 512 visuals without pending animation");
}

void ReplacementPage() {
    const D2D1_RECT_F viewport{0, 0, 400, 200};
    std::vector<ItemReflowSlot> before, after;
    for (int i = 0; i < 30; ++i) {
        before.push_back({L"old" + std::to_wstring(i),
            {0, i * 20.0f, 400, (i + 1) * 20.0f}, i});
        after.push_back({L"new" + std::to_wstring(i),
            {0, i * 20.0f, 400, (i + 1) * 20.0f}, i});
    }
    ItemReflowAnimation animation;
    animation.Update(before, 0, true, false, &viewport);
    animation.Update(after, 10, true, false, &viewport);
    Check(animation.Pending() && Near(animation.Visual(L"new0").opacity, 0) &&
        Near(animation.Visual(L"new0").bounds.top, 0),
        "replacement starts a real-slot reveal without a fabricated slide");
    animation.Update(after, 130, true, false, &viewport);
    Check(Near(animation.Visual(L"new0").opacity, 0.5f) &&
        Near(animation.Visual(L"new0").bounds.top, 0),
        "vacant slots fade immediately without a global entry hold");
    const auto interrupted = animation.Visual(L"new0");
    after[0].bounds.top += 20; after[0].bounds.bottom += 20;
    animation.Update(after, 130, true, false, &viewport);
    Check(Near(animation.Visual(L"new0").bounds.top, interrupted.bounds.top),
        "rapid bulk retarget continues from the sampled visible position");
    animation.Update(after, 370, true, false, &viewport);
    Check(!animation.Pending() && Near(animation.Visual(L"new0").bounds.top, 20),
        "bulk retarget settles and releases its animation deadline");

    // The survivors existed in the neighboring page, but none was on screen.
    std::vector<ItemReflowSlot> filtered;
    for (int i = 0; i < 10; ++i) {
        auto slot = before[static_cast<size_t>(i + 15)];
        slot.bounds = {0, i * 20.0f, 400, (i + 1) * 20.0f};
        filtered.push_back(slot);
    }
    animation.Reset();
    animation.Update(before, 0, true, false, &viewport);
    animation.Update(filtered, 10, true, false, &viewport);
    Check(animation.Pending() && Near(animation.Visual(L"old15").bounds.top, 300) &&
        Near(animation.Visual(L"old15").opacity, 1),
        "distant survivors retain their actual old positions instead of teleporting");
    animation.Update(filtered, 250, true, false, &viewport);
    Check(!animation.Pending() && Near(animation.Visual(L"old15").bounds.top, 0),
        "distant survivors reach their exact final positions");
    filtered.resize(1);
    animation.Reset();
    animation.Update(before, 0, true, false, &viewport);
    animation.Update(filtered, 10, true, false, &viewport);
    Check(Near(animation.Visual(L"old15").opacity, 1) &&
        Near(animation.Visual(L"old15").bounds.top, 300),
        "narrowing to one distant match preserves that identity's real starting position");

    // Off-screen unchanged identities must not mask a visible page replacement.
    auto mixed = before;
    for (int i = 0; i < 10; ++i) mixed[static_cast<size_t>(i)] = after[static_cast<size_t>(i)];
    animation.Reset();
    animation.Update(before, 0, true, false, &viewport);
    animation.Update(mixed, 10, true, false, &viewport);
    animation.Update(mixed, 130, true, false, &viewport);
    Check(Near(animation.Visual(L"new1").opacity, 0.5f),
        "unoccupied visible slots reveal even when neighboring pages retain old identities");
}

} // namespace

int main() {
    MiddleInsertAndDelete();
    NonadjacentChanges();
    GridAndRapidChanges();
    GridRowWrap();
    FrameCadence();
    VelocityAndBounds();
    IndependentEntryAndWrapDeadlines();
    ResetAndDisabledMotion();
    ZebraTransitions();
    Capacity();
    ReplacementPage();
    return failures == 0 ? 0 : 1;
}
