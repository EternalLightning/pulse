#include "../app/date_groups.h"
#include "../ui/view_layout.h"
#include <d2d1helper.h>
#include <cstdio>
#include <memory>
#include <tuple>

namespace {

FILETIME LocalDate(WORD year, WORD month, WORD day) {
    SYSTEMTIME date{};
    date.wYear = year;
    date.wMonth = month;
    date.wDay = day;
    date.wHour = 12;
    FILETIME local{}, utc{};
    SystemTimeToFileTime(&date, &local);
    LocalFileTimeToFileTime(&local, &utc);
    return utc;
}

} // namespace

int main() {
    using namespace pulse;
    auto entries = std::make_shared<std::vector<fs::DirEntry>>();
    for (const auto& date : {std::tuple{2026, 9, 29}, {2026, 9, 28},
                             {2026, 9, 23}, {2026, 9, 22},
                             {2026, 9, 14}, {2026, 8, 31}}) {
        fs::DirEntry entry;
        entry.mtime = LocalDate(static_cast<WORD>(std::get<0>(date)),
            static_cast<WORD>(std::get<1>(date)), static_cast<WORD>(std::get<2>(date)));
        entries->push_back(entry);
    }
    SYSTEMTIME today{};
    today.wYear = 2026;
    today.wMonth = 9;
    today.wDay = 29;
    const auto groups = app::BuildDateGroups(entries, nullptr, today);
    const bool buckets = groups.size() == 5 && groups[0].bucket == 0 &&
        groups[1].bucket == 1 && groups[2].bucket == 3 &&
        groups[2].count == 2 && groups[3].bucket == 4 && groups[4].bucket == 5;
    ui::ViewLayout layout(ui::ViewMode::Details, D2D1::RectF(0, 0, 600, 200),
        entries->size(), 0, 0, 1.0f, 34.0f, &groups);
    const auto first = layout.ItemRect(0);
    const auto second = layout.ItemRect(1);
    const bool geometry = first.top == ui::kDateGroupHeaderDip &&
        second.top == first.bottom + ui::kDateGroupHeaderDip &&
        layout.IconRect(0).left - first.left == 18.0f &&
        layout.NameRect(0).left - layout.IconRect(0).right == 12.0f &&
        layout.HitTest(80, 10) == -1 &&
        layout.HitTest(80, first.top + 5) == 0 &&
        layout.HitTest(80, second.top + 5) == 1 &&
        layout.ContentHeight() == 6 * 34 + 5 * ui::kDateGroupHeaderDip;
    ui::ViewLayout scrolled(ui::ViewMode::Details, D2D1::RectF(0, 0, 600, 200),
        entries->size(), 0, 75, 1.0f, 34.0f, &groups);
    const auto [visible_first, visible_last] = scrolled.VisibleRange();
    const std::vector<int> filtered{0, 2, 5};
    const auto filtered_groups = app::BuildDateGroups(entries, &filtered, today);
    const bool scrolled_and_filtered = scrolled.HitTest(80, 10) == -1 &&
        scrolled.HitTest(80, 35) == 1 && visible_first <= 1 && visible_last >= 1 &&
        filtered_groups.size() == 3 && filtered_groups[0].first == 0 &&
        filtered_groups[1].first == 1 && filtered_groups[2].first == 2;
    const bool ok = buckets && geometry && scrolled_and_filtered;
    std::printf("[%s] date buckets and grouped row hit testing\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
