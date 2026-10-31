// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

// ui_renderer.cpp — Chrome orchestration (title, toolbar, status, render).
#include "legacy_icons.h"
#include "ui_renderer.h"
#include "command_icons.h"
#include "toolbar_layout.h"
#include "ui_renderer_internal.h"
#include "../common/localization.h"
#include "../common/display_path.h"
#include "../common/known_folder_labels.h"
#include "tab_shape.h"
#include "animation_clock.h"
#include "typography.h"
#include "../model/places.h"
#include "../common/resource_ids.h"
#include "../common/text_format.h"
#include <windowsx.h>
#include <d2d1effects.h>
#include <shlwapi.h>
#include <algorithm>
#include <cmath>
#include <cwchar>
#include <cwctype>
#include <string_view>

namespace pulse::ui {
MainRenderer::MainRenderer()
    : owned_caches_(std::make_unique<RenderCaches>()), caches_(*owned_caches_) {}
MainRenderer::MainRenderer(RenderCaches& caches) : caches_(caches) {}
SIZE MainRenderer::BackgroundImageSize(const std::wstring& path) {
    if (path.empty()) return {};
    auto* bitmap = material_.SourceBitmap(path);
    if (!bitmap) return {};
    const auto size = bitmap->GetPixelSize();
    return {static_cast<LONG>(size.width), static_cast<LONG>(size.height)};
}

MainRenderer::~MainRenderer() {
    icon_cache_.RemoveNotifyWindow(notify_hwnd_);
    thumbnail_cache_.SetNotifyWindow(nullptr);
    thumbnail_cache_.Evict();
    thumbnail_cache_.SetDeviceContext(nullptr);
}

void MainRenderer::SetCompositor(Compositor* comp) {
    details_open_motion_ = {};
    details_motion_window_width_ = 0;
    details_exit_commands_.reset();
    page_surface_.Reset();
    address_path_surface_.Reset();
    settings_content_surface_.Reset();
    for (auto& surface : pane_surfaces_) surface.Reset();
    for (auto& surface : pane_title_surfaces_) surface.Reset();
    for (auto& surface : item_reflow_surfaces_) surface.Reset();
    for (auto& surface : view_mode_transitions_) surface.Reset();
    hover_tracks_.clear();
    RefreshMotionPreferences();
    paneHeaderStroke_.reset();
    ClearTextWidthCache();
    name_layout_cache_.clear();
    name_layout_order_.clear();
    name_layout_format_ = nullptr;
    sized_icon_formats_.clear();
    empty_state_svg_.reset();
    no_selection_svg_.reset();
    recent_empty_svg_.reset();
    starred_empty_svg_.reset();
    exclude_empty_svg_.reset();
    fluent_svgs_.clear();
    fluent_svg_failed_.clear();
    empty_state_svg_dc_.reset();
    compositor_ = comp;
    material_.SetCompositor(comp);
    painter_.SetCompositor(comp);
    painter_.SetHoverSampler([this](const D2D1_RECT_F& bounds, bool hovered) {
        return ControlHoverOpacity(bounds, hovered);
    });
    if (!comp) {
        // Shared icons outlive this window; its context is never retained.
        thumbnail_cache_.Evict();
        thumbnail_cache_.SetNotifyWindow(nullptr);
        thumbnail_cache_.SetDeviceContext(nullptr);
        preview_handler_.Reset();
        preview_mono_format_.reset();
    }
    else { icon_cache_.SetDeviceContext(comp->Dc()); thumbnail_cache_.SetDeviceContext(comp->Dc()); }
}

void MainRenderer::RefreshMotionPreferences() {
    BOOL animations = TRUE;
    motion_enabled_ =
        SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animations, 0) &&
        animations && !IsHighContrast();
}

void MainRenderer::InvalidateTypography() {
    for (auto& surface : view_mode_transitions_) surface.Reset();
    for (auto& surface : item_reflow_surfaces_) surface.Reset();
    ClearTextWidthCache();
    auto_widths_scale_ = 0.0f;
    details_text_layout_.reset();
    name_layout_cache_.clear();
    name_layout_order_.clear();
    name_layout_format_ = nullptr;
    sized_icon_formats_.clear();
    preview_mono_format_.reset();
    painter_.InvalidateTypography();
}
void MainRenderer::SetIconNotifyWindow(HWND hwnd) {
    if (notify_hwnd_ && notify_hwnd_ != hwnd) icon_cache_.RemoveNotifyWindow(notify_hwnd_);
    notify_hwnd_ = hwnd;
    icon_cache_.AddNotifyWindow(hwnd);
    thumbnail_cache_.SetNotifyWindow(hwnd);
    preview_handler_.SetNotifyWindow(hwnd);
}

void MainRenderer::SetScale(float scale) {
    if (scale_ != scale) {
        details_open_motion_ = {};
        details_exit_commands_.reset();
        details_scroll_x_.Reset();
        details_scroll_y_.Reset();
        preview_mono_format_.reset();
        sized_icon_formats_.clear();
        ClearTextWidthCache();
        name_layout_cache_.clear();
        name_layout_order_.clear();
        name_layout_format_ = nullptr;
    }
    scale_ = scale;
    title_bar_height_ = kTitleBarHeight * scale;
    toolbar_height_ = 88.0f * scale;
    status_height_ = 28.0f * scale;
    sidebar_width_ = sidebar_width_dip_ * scale;
    pane_header_height_ = 40.0f * scale;
    column_header_height_ = 32.0f * scale;
    row_height_ = row_height_dip_ * scale;
    margin_ = 4.0f * scale;
    control_gap_ = 4.0f * scale;
    painter_.SetScale(scale);
    icon_cache_.SetScale(scale);
}

float MainRenderer::EffectiveSidebarWidth(float window_width) const {
    const float window_dip = window_width / scale_;
    if (window_dip < kSidebarRailWindowDip) return kSidebarRailWidthDip * scale_;
    const bool details_open = DetailsPanelWidth(window_width) > 0;
    const float max_dip = MaxSidebarWidthDip(window_dip, details_width_, details_open,
                                             2.0f * margin_ / scale_);
    return (std::min)(sidebar_width_dip_, max_dip) * scale_;
}

// The preferred width is only capped while drawing, so narrowing the window pushes
// the panel back temporarily and widening it restores what the user dragged.
float MainRenderer::DetailsPanelWidth(float window_w) const {
    const float progress = details_open_motion_.initialized ? details_open_motion_.value : (details_visible_ ? 1.0f : 0.0f);
    return DetailsPanelTargetWidth(window_w) * progress;
}

float MainRenderer::DetailsPanelTargetWidth(float window_w) const {
    const float window_dip = window_w / scale_;
    if (window_dip < kDetailsVisibleWindowDip) return 0.0f;
    const float max_dip = MaxDetailsWidthDip(window_dip, sidebar_width_dip_,
                                             window_dip < kSidebarRailWindowDip,
                                             2.0f * margin_ / scale_);
    return (std::min)(details_width_, max_dip) * scale_ + margin_;
}

float MainRenderer::SidebarMaxWidthDip(float window_w) const {
    const float window_dip = window_w / scale_;
    const bool details_open = DetailsPanelWidth(window_w) > 0;
    return MaxSidebarWidthDip(window_dip, details_width_, details_open, 2.0f * margin_ / scale_);
}

float MainRenderer::DetailsMaxWidthDip(float window_w) const {
    const float window_dip = window_w / scale_;
    return MaxDetailsWidthDip(window_dip, sidebar_width_dip_,
                              window_dip < kSidebarRailWindowDip, 2.0f * margin_ / scale_);
}

D2D1_RECT_F MainRenderer::ContentRect(float w, float h) const {
    float left = EffectiveSidebarWidth(w) + margin_;
    float top = title_bar_height_ + toolbar_height_ + margin_;
    float bottom = h - status_height_ - margin_;
    return D2D1::RectF(left, top, w - margin_ - DetailsPanelWidth(w), bottom);
}
D2D1_RECT_F MainRenderer::ToolbarRect(float w) const {
    return D2D1::RectF(EffectiveSidebarWidth(w), title_bar_height_, w, title_bar_height_ + toolbar_height_);
}

D2D1_RECT_F MainRenderer::AddressBarRect(float w) const {
    return MakeToolbarLayout(w,scale_,title_bar_height_,margin_,NewButtonWidthPx(w-EffectiveSidebarWidth(w)<600*scale_),EffectiveSidebarWidth(w)).address;
}
D2D1_RECT_F MainRenderer::SearchBarRect(float w) const {
    if (w-EffectiveSidebarWidth(w)<480*scale_)
        return D2D1::RectF(EffectiveSidebarWidth(w)+margin_,title_bar_height_+4*scale_,
            w-margin_-44*scale_,title_bar_height_+40*scale_);
    return MakeToolbarLayout(w,scale_,title_bar_height_,margin_,NewButtonWidthPx(w-EffectiveSidebarWidth(w)<600*scale_),EffectiveSidebarWidth(w)).search;
}
D2D1_RECT_F MainRenderer::NewCommandRect(float w) const {
    return MakeToolbarLayout(w,scale_,title_bar_height_,margin_,NewButtonWidthPx(w-EffectiveSidebarWidth(w)<600*scale_),EffectiveSidebarWidth(w)).create;
}
D2D1_RECT_F MainRenderer::SplitCommandRect(float w) const {
    return MakeToolbarLayout(w,scale_,title_bar_height_,margin_,NewButtonWidthPx(w-EffectiveSidebarWidth(w)<600*scale_),EffectiveSidebarWidth(w)).commands[6];
}
float MainRenderer::NewButtonWidthPx(bool compact) const {
    if (compact) return kCommandIconButtonDip * scale_;
    return painter_.MeasureButtonWidth(
        pulse::l10n::Get(pulse::l10n::StringId::New), kIconAdd, true);
}

void MainRenderer::BreadcrumbLayout(const PaneViewModel& vm, float w,
                                    std::vector<BreadcrumbPlaced>& out) const {
    out.clear();
    auto segments = SplitBreadcrumb(vm.path, vm.header_text);
    if (segments.empty()) return;
    D2D1_RECT_F addr = AddressBarRect(w);
    const float segPad = 8.0f * scale_;
    const float chevronW = 14.0f * scale_;
    const float hint = 0.0f;
    const float avail = std::max(0.0f, addr.right - addr.left - 2 * margin_ - hint);

    IDWriteFactory2* dwrite = compositor_ ? compositor_->DwriteFactory() : nullptr;
    IDWriteTextFormat* fmt = compositor_ ? compositor_->AddressFormat() : nullptr;

    struct Measured { float w; };
    std::vector<float> widths(segments.size(), 0.0f);
    float total = 0.0f;
    for (size_t i = 0; i < segments.size(); ++i) {
        widths[i] = MeasureLayoutText(compositor_, dwrite, fmt, segments[i].text) + segPad * 2;
        total += widths[i] + (i ? chevronW : 0.0f);
    }
    // Collapse leading segments until the rest fits (last segment always kept).
    size_t first = 0;
    while (total > avail && first + 1 < segments.size()) {
        total -= widths[first] + chevronW;
        ++first;
    }

    float x = addr.left + margin_;
    for (size_t i = first; i < segments.size(); ++i) {
        if (i > first) x += chevronW;
        BreadcrumbPlaced p;
        const float segment_width = std::max(0.0f, std::min(widths[i], addr.right - margin_ - x));
        if (segment_width <= 0.0f) break;
        p.rc = D2D1::RectF(x, addr.top + 2 * scale_, x + segment_width, addr.bottom - 2 * scale_);
        p.full_text = segments[i].text;
        p.text = FitEndEllipsis(p.full_text, std::max(0.0f, segment_width - 2 * segPad),
            [&](const std::wstring& value) { return MeasureLayoutText(compositor_, dwrite, fmt, value); });
        p.path = segments[i].path;
        out.push_back(p);
        x += widths[i];
    }
}
void MainRenderer::DrawTextRect(ID2D1DeviceContext* dc, IDWriteTextFormat* fmt,
    ID2D1SolidColorBrush* br, std::wstring_view text, float x, float y, float w, float h,
    D2D1_DRAW_TEXT_OPTIONS opts, bool native_text) {
    D2D1_RECT_F rc = typography::SnapVerticalBounds(D2D1::RectF(x, y, x + w, y + h));
    if (native_text) {
        rc.left = std::round(rc.left);
        rc.right = rc.left + w;
    }
    if (compositor_ && DrawLegacyIcon(dc, compositor_->DwriteFactory(), text, rc, br, fmt->GetFontSize())) return;
    if (!native_text && !IsHighContrast() && compositor_ && br && compositor_->DrawLumaText(
            text, fmt, rc, br->GetColor(), text_background_, fmt->GetTextAlignment())) {
        return;
    }
    dc->DrawText(text.data(), (UINT32)text.size(), fmt, &rc, br, opts, typography::MeasuringMode());
}
void MainRenderer::UpdateBrushes(const Theme& theme) {
    ID2D1DeviceContext* dc = compositor_->Dc();
    MakeBrush(dc, theme.bg, brBg_);
    MakeBrush(dc, theme.text, brText_);
    MakeBrush(dc, theme.text_secondary, brTextSecondary_);
    MakeBrush(dc, theme.text_disabled, brTextDisabled_);
    MakeBrush(dc, theme.fill_hover, brFillHover_);
    MakeBrush(dc, theme.fill_pressed, brFillPressed_);
    MakeBrush(dc, theme.fill_selected, brFillSelected_);
    MakeBrush(dc, theme.fill_input, brFillInput_);
    MakeBrush(dc, theme.stroke_card, brStrokeCard_);
    MakeBrush(dc, theme.stroke_divider, brStrokeDivider_);
    MakeBrush(dc, theme.accent, brAccent_);
    MakeBrush(dc, theme.accent_hover, brAccentHover_);
    MakeBrush(dc, theme.accent_text, brAccentText_);
    MakeBrush(dc, theme.danger, brDanger_);
    MakeBrush(dc, theme.danger_hover, brDangerHover_);
    MakeBrush(dc, theme.scrollbar_thumb, brScrollbar_);
    MakeBrush(dc, theme.icon_folder, brIconFolder_);
    MakeBrush(dc, theme.icon_file, brIconFile_);
    MakeBrush(dc, theme.fps_bg, brFpsBg_);
    MakeBrush(dc, theme.fps_text, brFpsText_);
}

void MainRenderer::DrawIconText(float x, float y, float w, float h,
    const std::wstring& glyph, const std::wstring& fallback,
    const D2D1_COLOR_F& color, float size_factor) {
    ID2D1DeviceContext* dc = compositor_->Dc();
    const auto command = command_icons::FromGlyph(glyph);
    if (command != command_icons::Icon::None) {
        if (!paneHeaderStroke_.get()) command_icons::CreateStrokeStyle(dc, &paneHeaderStroke_);
        MakeBrush(dc, color, brText_);
        const auto bounds = command_icons::CenteredBounds(D2D1::RectF(x, y, x + w, y + h),
                                                           20.0f * scale_ * size_factor);
        if (command_icons::Draw(dc, brText_.get(), paneHeaderStroke_.get(), command, bounds)) return;
    }
    IDWriteTextFormat* iconFmt = compositor_->IconFormat();
    std::wstring txt = glyph;
    IDWriteTextFormat* fmt = iconFmt;
    if (!fmt) {
        fmt = compositor_->TextFormat();
        txt = fallback;
    }
    if (iconFmt && size_factor != 1.0f) {
        const int key = static_cast<int>(std::lround(size_factor * 1000.0f));
        auto cached = sized_icon_formats_.find(key);
        if (cached == sized_icon_formats_.end()) {
            ComPtr<IDWriteTextFormat> created;
            const float size = 16.0f * scale_ * size_factor;
            typography::CreateTextFormat(compositor_->DwriteFactory(),
                {typography::FontRole::Icon, size}, &created);
            if (created.get()) {
                created->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                created->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                cached = sized_icon_formats_.emplace(key, std::move(created)).first;
            }
        }
        if (cached != sized_icon_formats_.end()) fmt = cached->second.get();
    }
    MakeBrush(dc, color, brText_);
    DrawTextRect(dc, fmt, brText_.get(), txt, x, y, w, h);
}

void MainRenderer::DrawButton(const D2D1_RECT_F& rc, const Theme& theme, const D2D1_COLOR_F& bg,
    const std::wstring& glyph, const std::wstring& fallback,
    const D2D1_COLOR_F& fg, bool /*round_right*/, bool /*round_left*/, float size_factor) {
    ID2D1DeviceContext* dc = compositor_->Dc();
    const bool hover_fill = bg.a == 0.0f ||
        (bg.r == theme.fill_hover.r && bg.g == theme.fill_hover.g &&
         bg.b == theme.fill_hover.b && bg.a == theme.fill_hover.a);
    const float hover = ControlHoverOpacity(rc, bg.a > 0.0f && hover_fill);
    auto fill = hover_fill ? theme.fill_hover : bg;
    if (hover_fill) fill.a *= hover;
    if (fill.a > 0.0f) {
        MakeBrush(dc, fill, brFillHover_);
        float r = theme.radius_control * scale_;
        FillRoundedRect(dc, brFillHover_.get(), rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top, r);
    }
    DrawIconText(rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top, glyph, fallback, fg,
        size_factor);
}

void MainRenderer::Render(const WindowViewModel& vm, const D2D1_RECT_F& rect,
                          const Theme& theme) {
    if (!compositor_ || !compositor_->Dc()) return;
    details_content_animating_ = false;
    surface_animating_ = false;
    if (vm.settings_open || vm.dark != item_reflow_dark_) {
        for (auto& surface : item_reflow_surfaces_) surface.Reset();
        for (auto& surface : view_mode_transitions_) surface.Reset();
    }
    item_reflow_dark_ = vm.dark;
    for (auto it = hover_tracks_.begin(); it != hover_tracks_.end();) {
        if (it->second.frame != settings_switch_frame_) it = hover_tracks_.erase(it);
        else ++it;
    }
    settings_switch_animating_ = false;
    if (!vm.settings_open || settings_switch_page_ != vm.settings_page) settings_switch_tracks_.clear();
    settings_switch_page_ = vm.settings_open ? vm.settings_page : -1;
    for (auto it = settings_switch_tracks_.begin(); it != settings_switch_tracks_.end();) {
        if (it->second.frame != settings_switch_frame_) it = settings_switch_tracks_.erase(it);
        else ++it;
    }
    ++settings_switch_frame_;
    settings_switch_now_ = AnimationTimeMs();
    if (!vm.details_motion_sampled)
        TickDetailsPanelAnimation(settings_switch_now_, rect.right, motion_enabled_ && !IsHighContrast());
    surface_animating_ |= details_open_motion_.Pending();
    if (vm.settings_open || !vm.details_visible) details_content_reveal_.Reset();
    ThumbnailCache::FrameScope thumbnail_frame(thumbnail_cache_);
    if (vm.settings_open) for (auto& names : painted_name_truncation_) names.clear();
    ID2D1DeviceContext* dc = compositor_->Dc();
    icon_cache_.SetDeviceContext(compositor_->Dc());
    icon_cache_.SetScale(scale_);
    UpdateBrushes(theme);
    text_background_ = theme.bg;
    painter_.BeginFrame(theme, IsHighContrast());

    // None effect + selected image: the (optionally blurred) wallpaper covers
    // the window base; the title scrim, one shared sheet and the pane cards
    // stack over it with the opacities from ComputeLayerAlphas.
    const bool image_mode = vm.window_effect == WindowEffect::None &&
                            !vm.background_image.empty() && vm.wallpaper_visibility > 0 &&
                            !IsHighContrast();
    bool backdrop_drawn = false;
    if (image_mode) {
        backdrop_drawn = material_.DrawSourceCover(dc, rect, vm.background_image,
                                                   WallpaperBlurDip(vm.wallpaper_blur) * scale_);
    } else {
        backdrop_drawn = material_.DrawBackdrop(
            dc, rect, vm.window_effect, vm.dark,
            (vm.window_effect == WindowEffect::None) ? std::wstring{} : vm.background_image);
    }
    const LayerAlphas layers = ComputeLayerAlphas(image_mode, backdrop_drawn,
        vm.backdrop_enabled, vm.dark, vm.wallpaper_visibility, vm.wallpaper_blur);
    if (!backdrop_drawn && !vm.backdrop_enabled) {
        // Lower pane opacity must reveal the theme canvas when no material or
        // wallpaper exists, instead of exposing an unpainted transparent base.
        MakeBrush(dc, theme.bg, brBg_);
        dc->FillRectangle(rect, brBg_.get());
    }
    sheet_alpha_ = layers.sheet;
    card_alpha_ = layers.card;
    auto tint_background = [&](D2D1_RECT_F bounds, D2D1_COLOR_F color, float alpha) {
        if (bounds.right <= bounds.left || bounds.bottom <= bounds.top || alpha <= 0) return;
        MakeBrush(dc, WithAlpha(color, alpha), brBg_);
        dc->FillRectangle(bounds, brBg_.get());
    };
    if (layers.title > 0.0f) {
        D2D1_RECT_F title_bounds = rect;
        if (image_mode && !vm.dark) {
            title_bounds.bottom = (std::min)(rect.bottom, std::round(title_bar_height_));
        }
        tint_background(title_bounds, theme.surface_title, layers.title);
    }
    // One sheet below the title strip carries the toolbar, sidebar and status
    // bar; the active tab uses the same fill and meets it on a whole pixel so
    // translucent layers never double up into a seam.
    if (sheet_alpha_ > 0.0f) {
        const float sheetTop = std::round(title_bar_height_);
        tint_background({rect.left, sheetTop, rect.right, rect.bottom}, theme.surface_sheet, sheet_alpha_);
    }

    DrawTitleBar(vm, rect, theme);
    const auto page_bounds = D2D1::RectF(rect.left, title_bar_height_, rect.right,
                                       rect.bottom - status_height_);
    page_surface_.Paint(dc, page_bounds, vm.settings_open ? L"settings" : L"files", true,
        settings_switch_now_, motion_enabled_ && !IsHighContrast(),
        (vm.settings_open ? 18 : -18) * scale_, 240, [&] {
    if (vm.settings_open) {
        preview_handler_.Sync(notify_hwnd_, {}, L"", 0, 0, 0, 0, vm.dark,
                              theme.bg, theme.text, false);
        DrawSettings(vm, rect, theme);
    } else {
        DrawToolbar(vm, rect, theme);
        DrawSidebar(vm, rect, theme);
        DrawPane(vm, rect, theme);
        if (DetailsPanelWidth(rect.right) > 0) DrawAnimatedDetailsPanel(vm, rect, theme);
        else preview_handler_.Sync(notify_hwnd_, {}, L"", 0, 0, 0, 0, vm.dark,
                                   theme.bg, theme.text, false);
    }
    });
    surface_animating_ |= page_surface_.pending;
    DrawStatusBar(vm, rect, theme);

    // Drag action badge (ui.md §7.8): tooltip-style flyout near the cursor.
    if (!vm.drag_badge.empty()) {
        IDWriteTextFormat* fmt = compositor_->SmallFormat();
        float tw = MeasureLayoutText(compositor_, compositor_->DwriteFactory(), fmt,
                                     vm.drag_badge);
        float bw = tw + 20 * scale_;
        float bh = 24 * scale_;
        float bx = std::min(vm.drag_badge_x + 14 * scale_, rect.right - bw - margin_);
        float by = std::min(vm.drag_badge_y + 16 * scale_, rect.bottom - bh - margin_);
        D2D1_RECT_F brc = D2D1::RectF(bx, by, bx + bw, by + bh);
        MakeBrush(dc, theme.surface_flyout, brFillHover_);
        FillRoundedRect(dc, brFillHover_.get(), brc.left, brc.top, bw, bh, 4 * scale_);
        MakeBrush(dc, theme.stroke_card, brStrokeCard_);
        dc->DrawRoundedRectangle(D2D1::RoundedRect(brc, 4 * scale_, 4 * scale_), brStrokeCard_.get(), 1.0f);
        MakeBrush(dc, theme.text, brText_);
        const D2D1_COLOR_F saved_badge_bg = text_background_;
        text_background_ = BlendOver(theme.surface_flyout, theme.bg);
        DrawTextRect(dc, fmt, brText_.get(), vm.drag_badge, bx + 10 * scale_, by, tw, bh);
        text_background_ = saved_badge_bg;
    }

    if (vm.change_popover.visible) {
        const auto rc = ChangePopoverRect(vm.change_popover, rect, scale_);
        MakeBrush(dc, theme.surface_flyout, brFillHover_);
        FillRoundedRect(dc, brFillHover_.get(), rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top, 8 * scale_);
        auto content = rc; content.left += 14 * scale_; content.right -= 14 * scale_;
        content.top += 10 * scale_; content.bottom -= 42 * scale_;
        size_t begin = 0;
        for (int line = 0; line < ChangePopoverLineCount(vm.change_popover) && begin < vm.change_popover.summary.size(); ++line) {
            const auto end = vm.change_popover.summary.find(L'\n', begin);
            auto line_rc = content; line_rc.top += line * 24 * scale_; line_rc.bottom = line_rc.top + 24 * scale_;
            painter_.DrawText(std::wstring_view(vm.change_popover.summary).substr(begin,
                end == std::wstring::npos ? end : end - begin), line_rc, compositor_->SmallFormat(), theme.text);
            if (end == std::wstring::npos) break;
            begin = end + 1;
        }
        content.top = rc.bottom - 38 * scale_; content.bottom = rc.bottom - 8 * scale_;
        painter_.DrawText(pulse::l10n::Get(pulse::l10n::StringId::ChangeView), content, compositor_->TextFormat(), theme.accent);
    }
    DrawTooltip(vm, rect, theme);

}

void MainRenderer::DrawTitleBar(const WindowViewModel& vm, const D2D1_RECT_F& rect, const Theme& theme) {
    ID2D1DeviceContext* dc = compositor_->Dc();
    const float y = 0.0f;
    const float h = title_bar_height_;
    const float right = rect.right;

    const float ctrlW = 46.0f * scale_;
    const TabStripMetrics strip = ComputeTabStrip(vm, rect.right);
    const float tabH = strip.h;
    const float tabY = strip.y;
    dc->PushAxisAlignedClip(strip.viewport, D2D1_ANTIALIAS_MODE_ALIASED);
    auto drawTab = [&](size_t i, float left, bool raised) {
        const bool active = vm.tabs[i].active;
        const bool pinned = vm.tabs[i].pinned;
        const float tabW = strip.items[i].right - strip.items[i].left;
        if (tabW <= 0.0f || left + tabW <= strip.viewport.left || left >= strip.viewport.right) return;
        const bool hovered = IsHovered(vm, HitTestResult::Tab, static_cast<int>(i)) ||
                             IsHovered(vm, HitTestResult::TabClose, static_cast<int>(i));
        const bool connect = active || raised;
        ChromeTabShape shape;
        shape.top_radius = theme.radius_control * scale_;
        shape.bottom_radius = 8.0f * scale_;
        shape.connect_bottom = connect;
        const float tabTop = tabY;
        // Opaque tabs overlap the sheet by a pixel; translucent ones must abut it.
        const float tabBottom = connect ? (sheet_alpha_ < 1.0f ? std::round(h) : h + 1.0f)
                                        : (tabY + tabH);
        const D2D1_RECT_F tabRc = D2D1::RectF(left, tabTop, left + tabW, tabBottom);
        if (raised) {
            D2D1_RECT_F shadow = tabRc;
            shadow.left += 1.0f * scale_;
            shadow.top += 2.0f * scale_;
            shadow.right -= 1.0f * scale_;
            shadow.bottom += 1.0f * scale_;
            MakeBrush(dc, D2D1::ColorF(0.0f, 0.0f, 0.0f, vm.dark ? 0.09f : 0.07f), brFillPressed_);
            FillChromeTab(dc, brFillPressed_.get(), shadow, shape);
            MakeBrush(dc, WithAlpha(theme.surface_sheet, sheet_alpha_), brFillSelected_);
            FillChromeTab(dc, brFillSelected_.get(), tabRc, shape);
        } else if (active) {
            // A solid light plate remains identifiable over pale chrome and wallpapers.
            MakeBrush(dc, vm.dark ? WithAlpha(theme.surface_sheet, sheet_alpha_) : HexColor(0xFFFFFF), brFillSelected_);
            FillChromeTab(dc, brFillSelected_.get(), tabRc, shape);
        } else {
            // Grouped tabs get a tinted body; ungrouped keep the stock look.
            const float hot = ControlHoverOpacity(tabRc, hovered);
            const bool has_color = vm.tabs[i].color_rgb != 0;
            if (has_color) {
                D2D1_COLOR_F tint = HexColor(vm.tabs[i].color_rgb);
                tint.a *= 0.10f + 0.06f * hot;
                MakeBrush(dc, tint, brFillHover_);
            } else {
                auto fill = theme.fill_hover; fill.a *= hot;
                MakeBrush(dc, fill, brFillHover_);
            }
            FillChromeTab(dc, brFillHover_.get(), tabRc, shape);
        }
        // Grouped tabs draw the same top strip as the ungrouped active tab,
        // just in their group color instead of the accent.
        const float r = shape.top_radius;
        const bool has_color = vm.tabs[i].color_rgb != 0;
        if (has_color) {
            D2D1_COLOR_F line = HexColor(vm.tabs[i].color_rgb);
            if (!active) line.a *= 0.55f;
            MakeBrush(dc, line, brAccent_);
            FillChromeTabAccent(dc, brAccent_.get(), tabRc, shape, 2.0f * scale_);
        }
        if (pinned && !vm.show_pinned_tab_names) {
            // Chrome pinned tab: centered icon, no title, no close button.
            const float icon_size = 20.0f * scale_;
            const auto home_icon = D2D1::RectF(left + (tabW - icon_size) * 0.5f,
                tabY + (tabH - icon_size) * 0.5f,
                left + (tabW + icon_size) * 0.5f, tabY + (tabH + icon_size) * 0.5f);
            if (!vm.tabs[i].is_home || IsHighContrast() ||
                !DrawFluentSvg(IDR_FLUENT_HOME_SVG, home_icon, 1.0f, nullptr, true)) {
                DrawIconText(left, tabY, tabW, tabH,
                    vm.tabs[i].is_home ? kIconHome :
                    vm.tabs[i].is_settings ? kIconSettings : kIconFolder, L"[]",
                    active ? theme.icon_folder : theme.text_secondary, 0.85f);
            }
            return;
        }
        const float markerReserve = vm.tabs[i].marker_rgb != 0 ? 12.0f * scale_ : 0.0f;
        if (markerReserve > 0.0f) {
            MakeBrush(dc, HexColor(vm.tabs[i].marker_rgb), brAccent_);
            dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(left + 10.0f * scale_,
                tabY + tabH * 0.5f), 3.0f * scale_, 3.0f * scale_), brAccent_.get());
        }
        const float icon_left = left + 6.0f * scale_ + markerReserve;
        const auto home_icon = D2D1::RectF(icon_left, tabY + (tabH - 16.0f * scale_) * 0.5f,
            icon_left + 16.0f * scale_, tabY + (tabH + 16.0f * scale_) * 0.5f);
        if (!vm.tabs[i].is_home || IsHighContrast() ||
            !DrawFluentSvg(IDR_FLUENT_HOME_SVG, home_icon, 1.0f, nullptr, true)) {
            DrawIconText(icon_left, tabY, 16.0f * scale_, tabH,
                vm.tabs[i].is_home ? kIconHome :
                vm.tabs[i].is_settings ? kIconSettings : kIconFolder, L"[]",
                active ? theme.icon_folder : theme.text_secondary, 0.85f);
        }
        MakeBrush(dc, theme.text, brText_);
        const bool show_close = TabCloseVisible(vm, static_cast<int>(i), tabW, scale_);
        const float closeSz = kTabCloseSizeDip * scale_;
        const float closePad = kTabClosePadDip * scale_;
        const float closeReserve = show_close ? (closePad + closeSz + 6.0f * scale_)
                                              : 6.0f * scale_;
        const float titleLeft = left + 24.0f * scale_ + markerReserve;
        DrawTabTitle(compositor_, theme.bg, dc, compositor_->DwriteFactory(), compositor_->TabFormat(), brText_.get(),
                     vm.tabs[i].title, titleLeft, tabY,
                     std::max(0.0f, tabW - 24.0f * scale_ - markerReserve - closeReserve), tabH, scale_);
        if (show_close) {
            const float closeY = tabY + (tabH - closeSz) * 0.5f;
            const float closeX = left + tabW - closePad - closeSz;
            auto close_fill = theme.fill_hover;
            close_fill.a *= ControlHoverOpacity(D2D1::RectF(closeX, closeY, closeX + closeSz, closeY + closeSz),
                IsHovered(vm, HitTestResult::TabClose, static_cast<int>(i)));
            if (close_fill.a > 0.0f) {
                MakeBrush(dc, close_fill, brFillPressed_);
                FillRoundedRect(dc, brFillPressed_.get(), closeX, closeY, closeSz, closeSz, r);
            }
            DrawIconText(closeX, closeY, closeSz, closeSz,
                kIconCloseSmall, L"x", theme.text_secondary, 0.85f);
        }
    };
    const int dragI = vm.tab_drag_index;
    // Group chips sit at run starts: Edge-style solid blocks, not pill badges.
    auto brighten = [&](const D2D1_COLOR_F& c) {
        // Toward white (dark theme) or black (light) for readable chip text.
        D2D1_COLOR_F out = c;
        const float t = 0.35f;
        const float target = vm.dark ? 1.0f : 0.0f;
        out.r += (target - out.r) * t;
        out.g += (target - out.g) * t;
        out.b += (target - out.b) * t;
        out.a = 1.0f;
        return out;
    };
    for (const auto& chip : strip.chips) {
        if (chip.group < 0 || chip.group >= static_cast<int>(vm.tab_groups.size())) continue;
        const TabGroupView& gv = vm.tab_groups[static_cast<size_t>(chip.group)];
        const D2D1_COLOR_F gc = HexColor(gv.color_rgb);
        const float ch = strip.h - 8.0f * scale_;
        // Chip drag: the group's chip floats with its run (alone if collapsed).
        float chipLeft = chip.left + gv.x_offset;
        if (vm.tab_drag_chip && dragI >= 0 && dragI < static_cast<int>(vm.tabs.size()) &&
            vm.tabs[static_cast<size_t>(dragI)].group == chip.group) {
            chipLeft = gv.collapsed ? vm.tab_drag_x
                                    : vm.tab_drag_x - chip.width - 4.0f * scale_;
        }
        const D2D1_RECT_F rc = D2D1::RectF(chipLeft, strip.y + 4.0f * scale_,
                                           chipLeft + chip.width,
                                           strip.y + 4.0f * scale_ + ch);
        const bool chip_hovered = IsHovered(vm, HitTestResult::TabGroup, chip.group);
        const bool named = !gv.name.empty();
        D2D1_COLOR_F fill = gc;
        const float chip_hot = ControlHoverOpacity(rc, chip_hovered);
        fill.a *= named ? (0.32f + 0.10f * chip_hot) : (0.85f + 0.15f * chip_hot);
        MakeBrush(dc, fill, brFillHover_);
        FillRoundedRect(dc, brFillHover_.get(), rc.left, rc.top, chip.width, ch,
                        5.0f * scale_);
        if (named) {
            MakeBrush(dc, brighten(gc), brText_);
            DrawTextRect(dc, compositor_->SmallFormat(), brText_.get(), gv.name,
                rc.left + 8.0f * scale_, rc.top, chip.width - 16.0f * scale_, ch);
        }
    }
    int activeI = -1;
    const int dragN = std::max(1, vm.tab_drag_count);
    auto inDragRun = [&](int i) { return dragI >= 0 && i >= dragI && i < dragI + dragN; };
    for (size_t i = 0; i < vm.tabs.size(); ++i) {
        if (vm.tabs[i].hidden) continue;
        if (inDragRun(static_cast<int>(i))) continue;
        if (vm.tabs[i].active) {
            activeI = static_cast<int>(i);
            continue;
        }
        const float left = strip.items[i].left + vm.tabs[i].x_offset * strip.pitch;
        drawTab(i, left, false);
    }
    if (activeI >= 0 && !inDragRun(activeI) && !vm.tabs[static_cast<size_t>(activeI)].hidden) {
        const size_t i = static_cast<size_t>(activeI);
        const float left = strip.items[i].left + vm.tabs[i].x_offset * strip.pitch;
        drawTab(i, left, false);
    }
    // The dragged run floats as one block (browser group drag); collapsed
    // members stay hidden and do not take float width.
    if (dragI >= 0 && dragI < static_cast<int>(vm.tabs.size())) {
        float floatX = vm.tab_drag_x;
        for (int k = 0; k < dragN && dragI + k < static_cast<int>(vm.tabs.size()); ++k) {
            if (vm.tabs[static_cast<size_t>(dragI + k)].hidden) continue;
            drawTab(static_cast<size_t>(dragI + k), floatX, true);
            floatX += strip.pitch;
        }
    }

    auto tab_left_at = [&](int i) -> float {
        if (inDragRun(i)) {
            float left = vm.tab_drag_x;
            for (int k = dragI; k < i; ++k)
                if (!vm.tabs[static_cast<size_t>(k)].hidden) left += strip.pitch;
            return left;
        }
        return strip.items[static_cast<size_t>(i)].left
            + vm.tabs[static_cast<size_t>(i)].x_offset * strip.pitch;
    };
    const auto active = std::find_if(vm.tabs.begin(), vm.tabs.end(),
        [](const TabView& tab) { return tab.active && !tab.hidden; });
    if (active != vm.tabs.end()) {
        const int index = static_cast<int>(active - vm.tabs.begin());
        const float width = strip.items[static_cast<size_t>(index)].right - strip.items[static_cast<size_t>(index)].left;
        const float inset = active->pinned && !vm.show_pinned_tab_names ? 8.0f * scale_ : 16.0f * scale_;
        const float left = tab_left_at(index);
        const float viewport_width = strip.viewport.right - strip.viewport.left;
        // Renderer-only fixtures may omit identity; production always supplies it.
        const void* identity = active->identity ? active->identity :
            reinterpret_cast<const void*>(static_cast<uintptr_t>(index) + 1);
        const bool selection_changed = tab_indicator_.identity != identity;
        const bool animate = motion_enabled_ && vm.tab_drag_index < 0 && !tab_close_.Pending() &&
            tab_indicator_scale_ == scale_ && tab_indicator_window_width_ == rect.right &&
            (selection_changed || (tab_indicator_viewport_width_ == viewport_width &&
                                  tab_indicator_scroll_offset_ == strip.scroll_offset));
        tab_indicator_.Tick(identity, left + std::min(inset, width * 0.5f),
            left + std::max(width * 0.5f, width - inset), AnimationTimeMs(), animate);
        tab_indicator_scale_ = scale_;
        tab_indicator_window_width_ = rect.right;
        tab_indicator_viewport_width_ = viewport_width;
        tab_indicator_scroll_offset_ = strip.scroll_offset;
    } else {
        tab_indicator_.Reset();
    }
    dc->PopAxisAlignedClip();

    const int connected = dragI >= 0 ? dragI : activeI;
    if (connected >= 0 && connected < static_cast<int>(vm.tabs.size())) {
        const auto& connected_item = strip.items[static_cast<size_t>(connected)];
        const float connW = connected_item.right - connected_item.left;
        const float shoulder = 8.0f * scale_;
        const float cut_l = std::clamp(tab_left_at(connected) - shoulder,
            strip.viewport.left, strip.viewport.right);
        const float cut_r = std::clamp(tab_left_at(connected) + connW + shoulder,
            strip.viewport.left, strip.viewport.right);
        if (cut_l > 0.0f)
            FillRect(dc, brStrokeDivider_.get(), 0.0f, h - 1.0f, cut_l, 1.0f);
        if (cut_r < rect.right)
            FillRect(dc, brStrokeDivider_.get(), cut_r, h - 1.0f, rect.right - cut_r, 1.0f);
    } else {
        FillRect(dc, brStrokeDivider_.get(), 0.0f, h - 1.0f, rect.right, 1.0f);
    }

    if (active != vm.tabs.end()) {
        // Draw above the connection divider so the stretched bridge retains its full height.
        dc->PushAxisAlignedClip(strip.viewport, D2D1_ANTIALIAS_MODE_ALIASED);
        MakeBrush(dc, active->color_rgb ? HexColor(active->color_rgb) : theme.accent, brAccent_);
        const float bottom = sheet_alpha_ < 1.0f ? std::round(h) : h + 1.0f;
        FillRoundedRect(dc, brAccent_.get(), tab_indicator_.left, bottom - 3.0f * scale_,
            tab_indicator_.right - tab_indicator_.left, 2.0f * scale_, scale_);
        dc->PopAxisAlignedClip();
    }

    auto drawScroll = [&](const D2D1_RECT_F& bounds, bool left, bool enabled) {
        if (bounds.right <= bounds.left) return;
        const auto region = left ? HitTestResult::TabScrollLeft : HitTestResult::TabScrollRight;
        if (enabled && IsHovered(vm, region)) {
            MakeBrush(dc, theme.fill_hover, brFillHover_);
            FillRoundedRect(dc, brFillHover_.get(), bounds.left, bounds.top,
                bounds.right - bounds.left, bounds.bottom - bounds.top, theme.radius_control * scale_);
        }
        MakeBrush(dc, enabled ? theme.text_secondary : WithAlpha(theme.text_secondary, 0.35f), brText_);
        const float cx = (bounds.left + bounds.right) * 0.5f;
        const float cy = (bounds.top + bounds.bottom) * 0.5f;
        const float dx = std::min(3.0f * scale_, (bounds.right - bounds.left) * 0.20f);
        const float side = left ? 1.0f : -1.0f;
        dc->DrawLine(D2D1::Point2F(cx + side * dx, cy - 4.0f * scale_),
            D2D1::Point2F(cx - side * dx, cy), brText_.get(), 1.5f * scale_);
        dc->DrawLine(D2D1::Point2F(cx - side * dx, cy),
            D2D1::Point2F(cx + side * dx, cy + 4.0f * scale_), brText_.get(), 1.5f * scale_);
    };
    drawScroll(strip.scroll_left, true, strip.scroll_offset > 0.0f);
    drawScroll(strip.scroll_right, false, strip.scroll_offset < strip.scroll_max);
    const D2D1_RECT_F newRc = strip.new_button;
    dc->PushAxisAlignedClip(newRc, D2D1_ANTIALIAS_MODE_ALIASED);
    auto newRcFill = theme.fill_hover;
    newRcFill.a *= ControlHoverOpacity(newRc, IsHovered(vm, HitTestResult::TabNew));
    if (newRcFill.a > 0.0f) {
        MakeBrush(dc, newRcFill, brFillHover_);
        FillRoundedRect(dc, brFillHover_.get(), newRc.left, newRc.top,
            newRc.right - newRc.left, newRc.bottom - newRc.top, theme.radius_control * scale_);
    }
    // Snap the straight strokes to physical pixels instead of scaling a thin outline.
    const float addStroke = std::max(2.0f, std::round(1.5f * scale_));
    const float addOffset = std::fmod(addStroke, 2.0f) * 0.5f;
    const float addX = std::floor((newRc.left + newRc.right) * 0.5f) + addOffset;
    const float addY = std::floor((newRc.top + newRc.bottom) * 0.5f) + addOffset;
    const float addHalf = std::round(6.0f * scale_);
    MakeBrush(dc, theme.text, brText_);
    dc->DrawLine(D2D1::Point2F(addX - addHalf, addY), D2D1::Point2F(addX + addHalf, addY),
        brText_.get(), addStroke);
    dc->DrawLine(D2D1::Point2F(addX, addY - addHalf), D2D1::Point2F(addX, addY + addHalf),
        brText_.get(), addStroke);

    dc->PopAxisAlignedClip();

    // Window controls, right-aligned in Win11 order: min, max/restore, close.
    const float ctrlY = y;
    const float ctrlH = h;
    float cx = right;
    cx -= ctrlW;
    D2D1_RECT_F closeRc = D2D1::RectF(cx, ctrlY, cx + ctrlW, ctrlY + ctrlH);
    auto closeRcFill = theme.danger;
    closeRcFill.a *= ControlHoverOpacity(closeRc, IsHovered(vm, HitTestResult::Close));
    if (closeRcFill.a > 0.0f) {
        MakeBrush(dc, closeRcFill, brDanger_);
        FillRect(dc, brDanger_.get(), closeRc.left, closeRc.top, ctrlW, ctrlH);
    }
    DrawIconText(closeRc.left, closeRc.top, ctrlW, ctrlH, kIconClose, L"x",
        IsHovered(vm, HitTestResult::Close) ? HexColor(0xFFFFFF) : theme.text, 0.85f);
    cx -= ctrlW;
    D2D1_RECT_F maxRc = D2D1::RectF(cx, ctrlY, cx + ctrlW, ctrlY + ctrlH);
    auto maxRcFill = theme.fill_hover;
    maxRcFill.a *= ControlHoverOpacity(maxRc, IsHovered(vm, HitTestResult::Maximize));
    if (maxRcFill.a > 0.0f) {
        MakeBrush(dc, maxRcFill, brFillHover_);
        FillRect(dc, brFillHover_.get(), maxRc.left, maxRc.top, ctrlW, ctrlH);
    }
    DrawIconText(maxRc.left, maxRc.top, ctrlW, ctrlH,
        vm.maximized ? kIconRestore : kIconMaximize, vm.maximized ? L"[]" : L"\u25A1",
        theme.text, 0.85f);
    cx -= ctrlW;
    D2D1_RECT_F minRc = D2D1::RectF(cx, ctrlY, cx + ctrlW, ctrlY + ctrlH);
    auto minRcFill = theme.fill_hover;
    minRcFill.a *= ControlHoverOpacity(minRc, IsHovered(vm, HitTestResult::Minimize));
    if (minRcFill.a > 0.0f) {
        MakeBrush(dc, minRcFill, brFillHover_);
        FillRect(dc, brFillHover_.get(), minRc.left, minRc.top, ctrlW, ctrlH);
    }
    DrawIconText(minRc.left, minRc.top, ctrlW, ctrlH, kIconMinimize, L"_", theme.text, 0.85f);
}

void MainRenderer::DrawStatusBar(const WindowViewModel& vm, const D2D1_RECT_F& rect, const Theme& theme) {
    ID2D1DeviceContext* dc = compositor_->Dc();
    IDWriteFactory2* factory = compositor_->DwriteFactory();
    IDWriteTextFormat* small_fmt = compositor_->SmallFormat();
    const StatusBarMetrics sb = MakeStatusBarMetrics(
        vm, rect, scale_, status_height_, factory, small_fmt);
    const bool centered_progress = vm.status.query_active;
    float y = sb.bar.top;
    // Sits on the shared sheet painted by Render(); no own fill or top rule.
    MakeBrush(dc, theme.text_secondary, brTextSecondary_);
    const float gap = 16.0f * scale_;
    const bool hasTask = vm.status.query_active || !vm.status.task_text.empty() || vm.status.task_progress >= 0.0f;
    const float statusLimit = hasTask ? std::min(rect.right * 0.30f, sb.task.left - gap) : rect.right * 0.30f;
    const std::wstring status_text = path::FriendlyPathText(vm.status.status_text);
    const float statusWidth = std::min(std::max(0.0f, statusLimit - sb.pad),
        MeasureTextWidth(factory, small_fmt, status_text));
    DrawTextEndEllipsis(compositor_, theme.bg, dc, factory, small_fmt, brTextSecondary_.get(), status_text,
        sb.pad, y, statusWidth, status_height_);
    const float selectionLeft = sb.pad + statusWidth + gap;
    const float selectionRight = hasTask ? sb.task.left - gap : rect.right - sb.right_reserved - gap;
    DrawTextEndEllipsis(compositor_, theme.bg, dc, factory, small_fmt, brTextSecondary_.get(), vm.status.selection_text,
        selectionLeft, y, std::max(0.0f, selectionRight - selectionLeft), status_height_);

    const float rightReserved = sb.right_reserved;
    if (vm.status.query_cancellable) {
        DrawButton(sb.cancel_search, theme,
            IsHovered(vm, HitTestResult::StatusBarCancelSearch) ? theme.fill_hover : kTransparent,
            kIconCloseSmall, L"×", theme.text_secondary, true, true, 0.62f);
    }
    if (!centered_progress && !vm.status.performance_text.empty()) {
        const std::wstring& perfText = rect.right < 1100.0f * scale_
            ? vm.status.performance_compact_text : vm.status.performance_text;
        const float perfWidth = std::min(rect.right * 0.50f,
            MeasureTextWidth(factory, small_fmt, perfText) + 16.0f * scale_);
        small_fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
        MakeBrush(dc, theme.text_secondary, brTextSecondary_);
        DrawTextRect(dc, small_fmt, brTextSecondary_.get(), perfText,
            rect.right - rightReserved, y, perfWidth, status_height_);
        small_fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    } else if (!centered_progress && !vm.status.hint_text.empty()) {
        const float cancelWidth = vm.status.query_cancellable ? sb.cancel_search.right - sb.cancel_search.left + 8.0f * scale_ : 0.0f;
        const float hintWidth = std::max(0.0f, rightReserved - sb.pad - cancelWidth);
        small_fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
        DrawTextRect(dc, small_fmt, brTextSecondary_.get(), vm.status.hint_text,
            rect.right - rightReserved, y, hintWidth, status_height_);
        small_fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    }

    // Query activity shares the compact status area with operation summaries.
    const float taskX = sb.task.left;
    const float taskRight = sb.task.right;
    if (centered_progress) {
        const auto& progress_text = vm.status.query_active ? vm.status.query_text : vm.status.task_text;
        const float progress_value = vm.status.query_active ? vm.status.query_progress : vm.status.task_progress / 100.0f;
        const float taskWidth=std::max(0.0f,taskRight-taskX);
        const float trackWidth=std::min(100*scale_,taskWidth*0.30f);
        const float queryGap=std::min(8*scale_,taskWidth-trackWidth);
        const float textWidth=std::min(MeasureTextWidth(factory,small_fmt,progress_text),
            std::max(0.0f,taskWidth-trackWidth-queryGap));
        const float trackX=taskRight-trackWidth;
        const float textX=std::max(taskX,trackX-queryGap-textWidth);
        MakeBrush(dc,theme.accent,brAccentText_);
        DrawTextEndEllipsis(compositor_, theme.bg, dc,factory,small_fmt,brAccentText_.get(),progress_text,
            textX,y,textWidth,status_height_);
        fluent::ProgressSpec progress;
        progress.bounds=D2D1::RectF(trackX,y,std::min(taskRight,trackX+trackWidth),y+status_height_);
        progress.value=std::clamp(progress_value,0.0f,1.0f);
        progress.indeterminate=progress_value<0.0f;
        progress.animation_progress=static_cast<float>(GetTickCount64()%1952)/1952.0f;
        painter_.DrawProgressBar(progress);
    } else if (!vm.status.task_text.empty() || vm.status.task_progress >= 0.0f) {
        const float taskWidth = std::max(0.0f, taskRight - taskX);
        const float trackWidth = vm.status.task_progress >= 0.0f ? std::min(100 * scale_, taskWidth) : 0.0f;
        const float barW = trackWidth > 0.0f ? trackWidth + std::min(8 * scale_, taskWidth - trackWidth) : 0.0f;
        const float textWidth = std::max(0.0f, taskWidth - barW);
        MakeBrush(dc, theme.accent, brAccentText_);
        if (MeasureTextWidth(factory, small_fmt, vm.status.task_text) <= textWidth) {
            DrawTextRect(dc, small_fmt, brAccentText_.get(), vm.status.task_text,
                taskX, y, textWidth, status_height_);
        } else {
            DrawTextEndEllipsis(compositor_, theme.bg, dc, factory, small_fmt, brAccentText_.get(), vm.status.task_text,
                taskX, y, textWidth, status_height_);
        }
        if (barW > 0.0f) {
            float trackH = 4 * scale_;
            float trackX = taskRight - trackWidth;
            float trackY = y + (status_height_ - trackH) * 0.5f;
            MakeBrush(dc, theme.stroke_card, brStrokeCard_);
            FillRoundedRect(dc, brStrokeCard_.get(), trackX, trackY, trackWidth, trackH, trackH * 0.5f);
            float fillW = trackWidth * std::clamp(vm.status.task_progress / 100.0f, 0.0f, 1.0f);
            if (fillW > trackH) {
                MakeBrush(dc, theme.accent, brAccent_);
                FillRoundedRect(dc, brAccent_.get(), trackX, trackY, fillW, trackH, trackH * 0.5f);
            }
        }
    }

}
MainRenderer::TabStripMetrics MainRenderer::ComputeTabStrip(
    const WindowViewModel& vm, float window_w) const {
    TabStripMetrics m;
    const TitleChrome chrome = MakeTitleChrome(window_w, scale_, title_bar_height_);
    const float tabs_right = std::max(0.0f, chrome.chrome_left - 8.0f * scale_);
    m.x0 = std::min(12.0f * scale_, tabs_right);
    m.y = 4.0f * scale_;
    m.h = title_bar_height_ - 8.0f * scale_;
    const float new_w = std::min(32.0f * scale_, tabs_right - m.x0);
    const float new_gap = std::min(4.0f * scale_, tabs_right - m.x0 - new_w);
    const float available = std::max(0.0f, tabs_right - m.x0 - new_w - new_gap);
    const float chip_gap = 4.0f * scale_;
    float chips_total = 0.0f;
    IDWriteTextFormat* chip_fmt = compositor_ ? compositor_->SmallFormat() : nullptr;
    for (size_t i = 0; i < vm.tabs.size(); ++i) {
        const int g = vm.tabs[i].group;
        if (g < 0 || (i > 0 && vm.tabs[i - 1].group == g)) continue;
        float width = 8.0f * scale_;
        if (g < static_cast<int>(vm.tab_groups.size()) &&
            !vm.tab_groups[static_cast<size_t>(g)].name.empty()) {
            const float text_w = std::min(88.0f * scale_,
                MeasureTextWidth(compositor_ ? compositor_->DwriteFactory() : nullptr,
                    chip_fmt, vm.tab_groups[static_cast<size_t>(g)].name));
            width = text_w + 16.0f * scale_;
        }
        TabStripMetrics::Chip chip;
        chip.width = width;
        chip.group = g;
        chips_total += width + chip_gap;
        m.chips.push_back(chip);
    }
    m.extra.assign(vm.tabs.size(), 0.0f);
    size_t ordinary_count = 0, pinned_count = 0;
    int active = -1;
    for (size_t i = 0; i < vm.tabs.size(); ++i) {
        if (vm.tabs[i].active) active = static_cast<int>(i);
        if (vm.tabs[i].hidden) continue;
        if (vm.tabs[i].pinned) ++pinned_count; else ++ordinary_count;
    }
    m.pinned_w = (vm.show_pinned_tab_names ? kTabPinnedNamedW : kTabPinnedW) * scale_;
    const float pinned_total = static_cast<float>(pinned_count) * (m.pinned_w + control_gap_);
    m.w = ordinary_count == 0 ? 0.0f
        : std::clamp(std::max(0.0f, available - chips_total - pinned_total)
            / static_cast<float>(ordinary_count) - control_gap_,
            kTabMinW * scale_, kTabMaxW * scale_);
    m.w = std::min(m.w, available);
    m.pinned_w = std::min(m.pinned_w, available);
    auto content_width = [&] {
        const float trailing_gap = ordinary_count + pinned_count > 0 ? control_gap_
            : m.chips.empty() ? 0.0f : chip_gap;
        return std::max(0.0f, chips_total + static_cast<float>(pinned_count) * (m.pinned_w + control_gap_)
            + static_cast<float>(ordinary_count) * (m.w + control_gap_) - trailing_gap);
    };
    const bool overflow = content_width() > available;
    const float scroll_w = overflow ? std::min(24.0f * scale_, available * 0.20f) : 0.0f;
    const float viewport_left = m.x0 + scroll_w;
    const float viewport_right = m.x0 + available - scroll_w;
    const float viewport_w = std::max(0.0f, viewport_right - viewport_left);
    m.viewport = D2D1::RectF(viewport_left, 0.0f, viewport_right, title_bar_height_ + 1.0f);
    if (overflow) {
        m.scroll_left = D2D1::RectF(m.x0, m.y, viewport_left, m.y + m.h);
        m.scroll_right = D2D1::RectF(viewport_right, m.y,
            viewport_right + scroll_w, m.y + m.h);
        // A single tab must still fit when the viewport is narrower than its
        // usual minimum; scroll keeps every other tab reachable at its size.
        m.w = std::min(m.w, viewport_w);
        m.pinned_w = std::min(m.pinned_w, viewport_w);
    }
    m.pitch = m.w + control_gap_;
    float acc = 0.0f;
    size_t chip_index = 0;
    for (size_t i = 0; i < vm.tabs.size(); ++i) {
        const int g = vm.tabs[i].group;
        if (g >= 0 && (i == 0 || vm.tabs[i - 1].group != g) && chip_index < m.chips.size()) {
            auto& chip = m.chips[chip_index++];
            chip.left = viewport_left + static_cast<float>(i) * m.pitch + acc;
            acc += chip.width + chip_gap;
        }
        m.extra[i] = acc;
        if (vm.tabs[i].hidden) acc -= m.pitch;
        else if (vm.tabs[i].pinned) acc += m.pinned_w - m.w;
    }
    const float content_w = content_width();
    m.scroll_max = overflow ? std::max(0.0f, content_w - viewport_w) : 0.0f;
    auto& scroll = tab_strip_scroll_;
    scroll.offset = std::clamp(scroll.offset, 0.0f, m.scroll_max);
    const bool layout_changed = scroll.active != active || scroll.count != vm.tabs.size() ||
        scroll.viewport_w != viewport_w || scroll.content_w != content_w || scroll.scale != scale_;
    if (layout_changed || vm.tab_drag_index >= 0) scroll.animation.Reset();
    if (layout_changed && vm.tab_drag_index < 0) {
        float reveal_left = 0.0f, reveal_right = 0.0f;
        if (active >= 0) {
            const size_t i = static_cast<size_t>(active);
            reveal_left = static_cast<float>(active) * m.pitch + m.extra[i];
            reveal_right = reveal_left + (vm.tabs[i].pinned ? m.pinned_w : m.w);
            for (const auto& chip : m.chips) {
                if (chip.group != vm.tabs[i].group) continue;
                const float chip_left = chip.left - viewport_left;
                if (vm.tabs[i].hidden) {
                    reveal_left = chip_left;
                    reveal_right = chip_left + std::min(chip.width, viewport_w);
                } else if (reveal_right - chip_left <= viewport_w) {
                    reveal_left = chip_left;
                }
                break;
            }
            if (reveal_left < scroll.offset) scroll.offset = reveal_left;
            if (reveal_right > scroll.offset + viewport_w) scroll.offset = reveal_right - viewport_w;
            scroll.offset = std::clamp(scroll.offset, 0.0f, m.scroll_max);
        }
        scroll.active = active;
        scroll.count = vm.tabs.size();
        scroll.viewport_w = viewport_w;
        scroll.content_w = content_w;
        scroll.scale = scale_;
    }
    m.scroll_offset = scroll.offset;
    m.x0 = viewport_left - m.scroll_offset;
    for (auto& chip : m.chips) chip.left -= m.scroll_offset;
    m.end_x = overflow ? viewport_right + scroll_w + new_gap : m.x0 + content_w + new_gap;
    m.new_button = D2D1::RectF(m.end_x, m.y, m.end_x + new_w, m.y + m.h);
    m.items.reserve(vm.tabs.size() + m.chips.size() + 1);
    for (size_t i = 0; i < vm.tabs.size(); ++i) {
        const float left = m.x0 + static_cast<float>(i) * m.pitch + m.extra[i];
        const void* identity = vm.tabs[i].identity ? vm.tabs[i].identity :
            reinterpret_cast<const void*>(static_cast<uintptr_t>(i) + 1);
        m.items.push_back({identity, -1, left, left + (vm.tabs[i].pinned ? m.pinned_w : m.w)});
    }
    for (const auto& chip : m.chips) {
        const int group_id = chip.group < static_cast<int>(vm.tab_groups.size())
            ? vm.tab_groups[static_cast<size_t>(chip.group)].id : chip.group;
        m.items.push_back({nullptr, group_id, chip.left, chip.left + chip.width});
    }
    m.items.push_back({nullptr, -1, m.new_button.left, m.new_button.right});
    const bool reordering = vm.tab_drag_index >= 0 || std::any_of(vm.tabs.begin(), vm.tabs.end(),
        [](const TabView& tab) { return tab.x_offset != 0; }) ||
        std::any_of(vm.tab_groups.begin(), vm.tab_groups.end(),
            [](const TabGroupView& group) { return group.x_offset != 0; });
    tab_close_.Apply(m.items, AnimationTimeMs(), window_w, scale_, motion_enabled_ && !reordering);
    for (size_t i = 0; i < m.chips.size(); ++i) {
        const auto& item = m.items[vm.tabs.size() + i];
        m.chips[i].left = item.left;
        m.chips[i].width = item.right - item.left;
    }
    m.new_button.left = m.items.back().left;
    m.new_button.right = m.items.back().right;
    return m;
}

void MainRenderer::CaptureTabCloseLayout(const WindowViewModel& vm, float window_w) {
    auto strip = ComputeTabStrip(vm, window_w);
    tab_close_.Capture(std::move(strip.items), window_w, scale_);
}

bool MainRenderer::ScrollTabStrip(const WindowViewModel& vm, float window_w, float steps) {
    if (steps == 0.0f || vm.tab_drag_index >= 0) return false;
    const auto strip = ComputeTabStrip(vm, window_w);
    const float step = std::max(1.0f,
        (strip.viewport.right - strip.viewport.left) * 0.75f);
    const float previous = tab_strip_scroll_.offset;
    tab_strip_scroll_.animation.ScrollBy(tab_strip_scroll_.offset, step * steps,
        strip.scroll_max, strip.viewport.right - strip.viewport.left,
        AnimationTimeMs(), MotionEnabled());
    return previous != tab_strip_scroll_.offset || tab_strip_scroll_.animation.Pending();
}

bool MainRenderer::TickSurfaceScroll(uint64_t now) {
    auto& strip = tab_strip_scroll_;
    bool changed = strip.animation.Tick(strip.offset,
        std::max(0.0f, strip.content_w - strip.viewport_w), now, MotionEnabled());
    const bool preview_scrollable = details_preview_ready_ && !details_viewport_.image &&
        !DetailsPreviewPointerActive();
    if (!preview_scrollable) {
        details_scroll_x_.Reset();
        details_scroll_y_.Reset();
    } else {
        changed |= details_scroll_x_.Tick(details_viewport_.x, details_viewport_.MaxX(),
            now, MotionEnabled());
        changed |= details_scroll_y_.Tick(details_viewport_.y, details_viewport_.MaxY(),
            now, MotionEnabled());
    }
    return changed;
}

D2D1_RECT_F MainRenderer::TabStripViewportRect(const WindowViewModel& vm, float window_w) const {
    return ComputeTabStrip(vm, window_w).viewport;
}

} // namespace pulse::ui
