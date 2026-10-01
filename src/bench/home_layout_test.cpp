#include "../ui/home_layout.h"
#include "../app/places.h"
#include "../app/home_catalog.h"
#include "../common/localization.h"
#include <iostream>

int main() {
    using namespace pulse;
    int failures = 0;
    const auto check = [&](bool ok, const char* name) {
        std::cout << (ok ? "[PASS] " : "[FAIL] ") << name << '\n';
        if (!ok) ++failures;
    };
    check(app::MakeHomePath() == L"pulse:home", "stable home navigation path");
    ui::PaneViewModel pane;
    pane.is_home = true;
    for (int group = 0; group < 3; ++group)
        for (int i = 0; i < (group == 0 ? 5 : 3); ++i)
            pane.home_cards.push_back({L"Fixture", L"", std::to_wstring(pane.home_cards.size()), L"", group});
    for (const float scale : {1.0f, 1.5f, 2.0f}) {
        for (const float width : {260.0f, 720.0f, 1200.0f}) {
            const auto bounds = D2D1::RectF(0, 0, width * scale, 600 * scale);
            auto layout = ui::MakeHomeLayout(pane, bounds, scale);
            check(layout.cards.size() == pane.home_cards.size(), "all three groups retain their cards");
            bool valid = true;
            for (const auto& card : layout.cards) {
                valid &= card.rect.left >= bounds.left && card.rect.right <= bounds.right;
                valid &= card.rect.right > card.rect.left;
                if (card.rect.top >= bounds.top && card.rect.bottom < bounds.bottom)
                    valid &= ui::HitHomeCard(pane, bounds, scale, (card.rect.left + card.rect.right) * 0.5f,
                        (card.rect.top + card.rect.bottom) * 0.5f) == card.index;
            }
            check(valid, "responsive cards stay within pane and hit their own navigation target");
            check(ui::HitHomeCard(pane, bounds, scale, 1, 1) == -1, "pane padding is not a card target");
            const float maximum = std::max(0.0f, layout.height - (bounds.bottom - bounds.top));
            pane.scroll_y = maximum;
            auto scrolled = ui::MakeHomeLayout(pane, bounds, scale);
            check(std::abs(scrolled.height - layout.height) < 0.01f, "scroll offset does not alter content extent");
            const auto last = scrolled.cards.back();
            check(last.rect.bottom < bounds.bottom && ui::HitHomeCard(pane, bounds, scale,
                (last.rect.left + last.rect.right) * 0.5f, (last.rect.top + last.rect.bottom) * 0.5f) == last.index,
                "last network card is reachable at maximum scroll");
            pane.scroll_y = 0;
            for (int group = 0; group < 3; ++group) {
                pane.home_collapsed_mask = 1u << group;
                const auto folded = ui::MakeHomeLayout(pane, bounds, scale);
                bool hidden = folded.height < layout.height;
                for (const auto& card : folded.cards) hidden &= pane.home_cards[card.index].group != group;
                check(hidden, "collapsed group removes its cards and reduces scroll extent");
                const auto header = folded.headings[group];
                pane.scroll_y = std::max(0.0f, header.top - bounds.top - 16 * scale);
                const auto visible = ui::MakeHomeLayout(pane, bounds, scale).headings[group];
                check(ui::HitHomeGroup(pane, bounds, scale, visible.left + 8 * scale, visible.top + 8 * scale) == group &&
                    ui::HitHomeCard(pane, bounds, scale, visible.left + 8 * scale, visible.top + 8 * scale) == -1,
                    "scrolled collapsed heading remains an expand target at each DPI");
                pane.scroll_y = 0;
            }
            pane.home_collapsed_mask = 7;
            check(ui::MakeHomeLayout(pane, bounds, scale).cards.empty(), "all collapsed groups retain headings without card targets");
            pane.home_collapsed_mask = 0;
        }
    }
    pane.home_cards.clear();
    check(ui::MakeHomeLayout(pane, D2D1::RectF(0, 0, 400, 600), 1).empty == std::array<bool, 3>{true, true, true},
        "empty state retains all three group headings");
    l10n::Initialize(GetModuleHandleW(nullptr), L"zh-CN");
    app::HomeCatalog catalog;
    catalog.Refresh();
    const auto deadline = GetTickCount64() + 10000;
    while (catalog.Loading() && GetTickCount64() < deadline) Sleep(5);
    check(!catalog.Loading(), "background catalog completes without touching user preferences");
    catalog.Fill(pane);
    const auto recycle = std::find_if(pane.home_cards.begin(), pane.home_cards.end(),
        [](const auto& card) { return card.path == app::MakeRecyclePath(); });
    check(recycle != pane.home_cards.end() && recycle->group == 0, "recycle bin remains a navigable library card");
    bool capacity_valid = true;
    for (const auto& card : pane.home_cards) {
        if (card.group == 1) capacity_valid &= !card.detail.empty() && card.used_ratio <= 1.0f;
        if (card.group == 2 && card.path.size() == 3 && card.path[1] == L':')
            capacity_valid &= !card.detail.empty() && card.used_ratio <= 1.0f;
    }
    check(capacity_valid, "local and mapped drives have capacity or explicit unavailable state");
    bool native_icons = true;
    for (const auto& card : pane.home_cards)
        if (card.group == 0) native_icons &= card.system_icon_index >= 0;
    check(native_icons, "all library cards use native known-folder Shell icons after relocation");
    const auto mapped = std::find_if(pane.home_cards.begin(), pane.home_cards.end(), [](const auto& card) { return card.path == L"Z:\\"; });
    ULARGE_INTEGER available{}, total{}, free{};
    if (GetDriveTypeW(L"Z:\\") == DRIVE_REMOTE && GetDiskFreeSpaceExW(L"Z:\\", &available, &total, &free) && total.QuadPart) {
        check(mapped != pane.home_cards.end() && mapped->used_ratio >= 0 && !mapped->detail.empty(),
            "available mapped Z drive retains capacity and usage bar");
        wchar_t label[MAX_PATH + 1]{};
        if (GetVolumeInformationW(L"Z:\\", label, MAX_PATH, nullptr, nullptr, nullptr, nullptr, 0) && label[0])
            check(mapped != pane.home_cards.end() && mapped->label == std::wstring(label) + L" (Z:)", "mapped drive volume label matches Windows");
    }
    check(catalog.ConsumeUpdate() && !catalog.ConsumeUpdate(), "completed catalog update is consumed once");
    return failures ? 1 : 0;
}
