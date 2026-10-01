#include "home_layout.h"
#include "ui_renderer_internal.h"
#include "../common/localization.h"

namespace pulse::ui {
void MainRenderer::DrawHome(const WindowViewModel& vm, const PaneViewModel& pane,
                            const D2D1_RECT_F& bounds, int pane_index, const Theme& theme) {
    auto* dc = compositor_->Dc();
    painter_.BeginFrame(theme, IsHighContrast());
    dc->PushAxisAlignedClip(bounds, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    const auto pane_fill = vm.dark && !IsHighContrast()
        ? WithAlpha(HexColor(0xFFFFFF), vm.background_image.empty() ? 0.045f : 0.09f)
        : WithAlpha(theme.surface_card, card_alpha_);
    painter_.FillRoundedRect(bounds, theme.radius_control * scale_, pane_fill);
    const D2D1_RECT_F body = bounds;
    dc->PushAxisAlignedClip(body, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    const HomeLayout layout = MakeHomeLayout(pane, bounds, scale_);
    const l10n::StringId headings[] = {l10n::StringId::HomeLibraries, l10n::StringId::HomeDrives, l10n::StringId::SidebarNetworkLocations};
    for (int group = 0; group < 3; ++group) {
        const auto& header = layout.headings[group];
        const bool collapsed = (pane.home_collapsed_mask & (1u << group)) != 0;
        const bool hover = vm.hover_region == HitTestResult::HomeGroup && vm.hover_control_index == group && vm.hover_pane_index == pane_index;
        if (hover) painter_.FillRoundedRect(header, theme.radius_control * scale_, theme.fill_hover);
        DrawIconText(header.left, header.top + 6 * scale_, 16 * scale_, 16 * scale_,
            collapsed ? kIconChevronRight : kIconChevronDown, L"", theme.text_secondary, 1.0f);
        auto text = header;
        text.left += 24 * scale_; text.top += 2 * scale_;
        const auto title = l10n::Get(headings[group]);
        painter_.DrawText(title, text, compositor_->TextFormat(), theme.text);
        const float line_left = text.left + MeasureTextWidth(compositor_->DwriteFactory(), compositor_->TextFormat(), title) + 12 * scale_;
        if (line_left < header.right)
            painter_.FillRoundedRect(D2D1::RectF(line_left, header.top + 15 * scale_, header.right, header.top + 16 * scale_), 0, theme.stroke_divider);
        if (layout.empty[group]) {
            auto empty_text = layout.headings[group];
            empty_text.top += 36 * scale_; empty_text.bottom += 38 * scale_;
            painter_.DrawText(l10n::Get(pane.home_loading ? l10n::StringId::HomeLoading :
                group == 2 ? l10n::StringId::HomeNetworkEmpty : l10n::StringId::HomeCapacityUnavailable),
                empty_text, compositor_->SmallFormat(), theme.text_secondary);
        }
    }
    for (const auto& placed : layout.cards) {
        const auto& card = pane.home_cards[static_cast<size_t>(placed.index)];
        const auto& r = placed.rect;
        if (r.bottom < body.top || r.top > body.bottom) continue;
        const bool hover = vm.hover_region == HitTestResult::HomeCard && vm.hover_control_index == placed.index && vm.hover_pane_index == pane_index;
        painter_.FillRoundedRect(r, theme.radius_control * scale_, hover ? theme.fill_hover : theme.fill_input);
        painter_.StrokeRoundedRect(r, theme.radius_control * scale_, hover ? theme.accent : theme.stroke_card);
        const float icon_top = card.group == 0 ? (r.top + r.bottom - 28 * scale_) * 0.5f : r.top + 8 * scale_;
        const auto icon = D2D1::RectF(r.left + 14 * scale_, icon_top, r.left + 42 * scale_, icon_top + 28 * scale_);
        const int svg = FluentSvgIdForGlyph(card.glyph);
        const bool native = card.group == 0 && icon_cache_.DrawSystemIcon(dc, icon, card.system_icon_index);
        if (!native && (IsHighContrast() || !svg || !DrawFluentSvg(svg, icon, 1.0f, nullptr, true)))
            DrawIconText(icon.left, icon.top, 28 * scale_, 28 * scale_, card.glyph.c_str(), L"", theme.accent, 1.0f);
        painter_.DrawText(card.label, D2D1::RectF(r.left + 52 * scale_, icon_top - 4 * scale_, r.right - 12 * scale_, icon_top + 32 * scale_),
            compositor_->TextFormat(), theme.text);
        const std::wstring detail = card.detail.empty() && card.group == 2 ? card.path : card.detail;
        painter_.DrawText(detail, D2D1::RectF(r.left + 14 * scale_, r.top + 44 * scale_, r.right - 12 * scale_, r.top + 68 * scale_),
            compositor_->SmallFormat(), theme.text_secondary);
        if (card.used_ratio >= 0) {
            const auto bar = D2D1::RectF(r.left + 14 * scale_, r.bottom - 16 * scale_, r.right - 14 * scale_, r.bottom - 11 * scale_);
            painter_.FillRoundedRect(bar, 2 * scale_, theme.stroke_divider);
            auto used = bar; used.right = used.left + (bar.right - bar.left) * std::clamp(card.used_ratio, 0.0f, 1.0f);
            painter_.FillRoundedRect(used, 2 * scale_, card.used_ratio >= 0.9f ? HexColor(0xE45B65) : theme.accent);
        }
    }
    dc->PopAxisAlignedClip();
    D2D1_RECT_F track{}, thumb{}; float max_scroll = 0;
    if (PaneScrollbarGeometry(pane, bounds, track, thumb, max_scroll))
        painter_.FillRoundedRect(D2D1::RectF(thumb.left + 4 * scale_, thumb.top, thumb.right - 4 * scale_, thumb.bottom), 3 * scale_, theme.text_secondary);
    dc->PopAxisAlignedClip();
}
}
