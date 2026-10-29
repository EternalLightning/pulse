#pragma once
#include "ui_renderer.h"
#include <cmath>

namespace pulse::ui {
struct HomeLayout {
    struct Card { D2D1_RECT_F rect; int index; };
    std::vector<Card> cards;
    std::array<D2D1_RECT_F, 3> headings{};
    std::array<D2D1_RECT_F, 3> content_clips{};
    std::array<bool, 3> empty{};
    D2D1_RECT_F network_notice{};
    float height = 0.0f;
};
inline float HomeGroupExpansion(const PaneViewModel& pane, int group) {
    return pane.home_expansion[group] < 0.0f
        ? ((pane.home_collapsed_mask & (1u << group)) ? 0.0f : 1.0f)
        : std::clamp(pane.home_expansion[group], 0.0f, 1.0f);
}
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
        const float expansion = HomeGroupExpansion(pane, group);
        if (expansion <= 0.0f) { y += 12 * scale; continue; }
        const float content_top = y;
        if (group == 2 && pane.network_discovery_disabled) {
            out.network_notice = D2D1::RectF(left, y, left + width, y + 52 * scale);
            y += 64 * scale;
        }
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
        const float content_height = y - content_top;
        out.content_clips[group] = D2D1::RectF(bounds.left, content_top - scale, bounds.right, content_top + content_height * expansion);
        y = content_top + 12 * scale + (content_height + 12 * scale) * expansion;
    }
    out.height = y + pane.scroll_y - bounds.top;
    return out;
}
inline int HitHomeCard(const PaneViewModel& pane, D2D1_RECT_F bounds, float scale, float x, float y) {
    if (x < bounds.left || x >= bounds.right || y < bounds.top || y >= bounds.bottom) return -1;
    const auto layout = MakeHomeLayout(pane, bounds, scale);
    for (const auto& card : layout.cards) {
        const int group = pane.home_cards[card.index].group;
        const auto& clip = layout.content_clips[group];
        if (!(pane.home_collapsed_mask & (1u << group)) && y >= clip.top && y < clip.bottom &&
            x >= card.rect.left && x < card.rect.right && y >= card.rect.top && y < card.rect.bottom) return card.index;
    }
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
inline bool HitHomeNetworkSettings(const PaneViewModel& pane, D2D1_RECT_F bounds, float scale, float x, float y) {
    if (!pane.network_discovery_disabled || x < bounds.left || x >= bounds.right ||
        y < bounds.top || y >= bounds.bottom) return false;
    const auto layout = MakeHomeLayout(pane, bounds, scale);
    const auto rect = layout.network_notice;
    return !(pane.home_collapsed_mask & 4u) && rect.right > rect.left &&
        x >= rect.left && x < rect.right && y >= rect.top && y < rect.bottom && y < layout.content_clips[2].bottom;
}
}
