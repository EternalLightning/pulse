#pragma once
#include "ui_renderer.h"
#include <cmath>

namespace pulse::ui {
struct HomeLayout {
    struct Card { D2D1_RECT_F rect; int index; };
    std::vector<Card> cards;
    std::array<D2D1_RECT_F, 3> headings{};
    std::array<bool, 3> empty{};
    float height = 0.0f;
};
inline D2D1_RECT_F HomeButtonRect(float sidebar_width, float top, float scale) {
    return D2D1::RectF(8 * scale, top + 4 * scale, sidebar_width - 8 * scale, top + 44 * scale);
}
inline HomeLayout MakeHomeLayout(const PaneViewModel& pane, D2D1_RECT_F bounds, float scale) {
    HomeLayout out;
    const float pad = 20 * scale, gap = 12 * scale;
    const float left = bounds.left + pad;
    const float width = std::max(1.0f, bounds.right - left - pad - 8 * scale);
    const int columns = std::max(1, static_cast<int>((width + gap) / (220 * scale + gap)));
    const float card_width = (width - gap * static_cast<float>(columns - 1)) / static_cast<float>(columns);
    float y = bounds.top + 16 * scale - pane.scroll_y;
    for (int group = 0; group < 3; ++group) {
        const float card_height = (group == 0 ? 60 : 88) * scale;
        out.headings[group] = D2D1::RectF(left, y, left + width, y + 28 * scale);
        y += 36 * scale;
        if (pane.home_collapsed_mask & (1u << group)) { y += 12 * scale; continue; }
        int count = 0;
        for (int i = 0; i < static_cast<int>(pane.home_cards.size()); ++i) {
            if (pane.home_cards[static_cast<size_t>(i)].group != group) continue;
            const float x = left + static_cast<float>(count % columns) * (card_width + gap);
            const float top = y + static_cast<float>(count / columns) * (card_height + gap);
            out.cards.push_back({D2D1::RectF(x, top, x + card_width, top + card_height), i});
            ++count;
        }
        out.empty[group] = count == 0;
        const int rows = (count + columns - 1) / columns;
        y += rows ? static_cast<float>(rows) * (card_height + gap) : 34 * scale;
        y += 24 * scale;
    }
    out.height = y + pane.scroll_y - bounds.top;
    return out;
}
inline int HitHomeCard(const PaneViewModel& pane, D2D1_RECT_F bounds, float scale, float x, float y) {
    if (x < bounds.left || x >= bounds.right || y < bounds.top || y >= bounds.bottom) return -1;
    for (const auto& card : MakeHomeLayout(pane, bounds, scale).cards)
        if (x >= card.rect.left && x < card.rect.right && y >= card.rect.top && y < card.rect.bottom) return card.index;
    return -1;
}
inline int HitHomeGroup(const PaneViewModel& pane, D2D1_RECT_F bounds, float scale, float x, float y) {
    if (x < bounds.left || x >= bounds.right || y < bounds.top || y >= bounds.bottom) return -1;
    const auto layout = MakeHomeLayout(pane, bounds, scale);
    for (int group = 0; group < 3; ++group) {
        const auto& r = layout.headings[group];
        if (x >= r.left && x < r.right && y >= r.top && y < r.bottom) return group;
    }
    return -1;
}
}
