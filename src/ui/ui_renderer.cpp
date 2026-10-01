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
#include "bloom_accent_picker.h"
#include "typography.h"
#include "../app/places.h"
#include "../app/resource.h"
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
MainRenderer::MainRenderer() = default;

void MainRenderer::SetCompositor(Compositor* comp) {
    paneHeaderStroke_.reset();
    tray_shadows_.clear();
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
    if (!comp) {
        icon_cache_.Reset();
        thumbnail_cache_.Reset();
        preview_handler_.Reset();
        preview_mono_format_.reset();
    }
    else { icon_cache_.SetDeviceContext(comp->Dc()); thumbnail_cache_.SetDeviceContext(comp->Dc()); }
}

void MainRenderer::InvalidateTypography() {
    ClearTextWidthCache();
    name_layout_cache_.clear();
    name_layout_order_.clear();
    name_layout_format_ = nullptr;
    sized_icon_formats_.clear();
    preview_mono_format_.reset();
    painter_.InvalidateTypography();
}
void MainRenderer::SetIconNotifyWindow(HWND hwnd) {
    notify_hwnd_ = hwnd;
    icon_cache_.SetNotifyWindow(hwnd);
    thumbnail_cache_.SetNotifyWindow(hwnd);
    preview_handler_.SetNotifyWindow(hwnd);
}

void MainRenderer::SetScale(float scale) {
    if (scale_ != scale) {
        tray_shadows_.clear();
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
    const bool details_open = details_visible_ && window_dip >= kDetailsVisibleWindowDip;
    const float max_dip = MaxSidebarWidthDip(window_dip, details_width_, details_open,
                                             2.0f * margin_ / scale_);
    return (std::min)(sidebar_width_dip_, max_dip) * scale_;
}

// The preferred width is only capped while drawing, so narrowing the window pushes
// the panel back temporarily and widening it restores what the user dragged.
float MainRenderer::DetailsPanelWidth(float window_w) const {
    const float window_dip = window_w / scale_;
    if (!details_visible_ || window_dip < kDetailsVisibleWindowDip) return 0.0f;
    const float max_dip = MaxDetailsWidthDip(window_dip, sidebar_width_dip_,
                                             window_dip < kSidebarRailWindowDip,
                                             2.0f * margin_ / scale_);
    return (std::min)(details_width_, max_dip) * scale_ + margin_;
}

float MainRenderer::SidebarMaxWidthDip(float window_w) const {
    const float window_dip = window_w / scale_;
    const bool details_open = details_visible_ && window_dip >= kDetailsVisibleWindowDip;
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
D2D1_RECT_F MainRenderer::TitleBarRect(float w) const {
    return D2D1::RectF(0, 0, w, title_bar_height_);
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
            w-margin_,title_bar_height_+40*scale_);
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

std::vector<BreadcrumbSegment> SplitBreadcrumb(const std::wstring& path) {
    std::vector<BreadcrumbSegment> out;
    std::wstring p = path;
    if (p.starts_with(L"\\\\?\\UNC\\")) p = L"\\\\" + p.substr(8);
    else if (p.starts_with(L"\\\\?\\")) p = p.substr(4);
    while (p.size() > 1 && p.back() == L'\\') p.pop_back();
    if (p.empty()) {
        // Empty path = This PC: a single segment that navigates to "".
        BreadcrumbSegment seg;
        seg.text = pulse::l10n::Get(pulse::l10n::StringId::ThisPc);
        seg.path = L"";
        out.push_back(seg);
        return out;
    }
    std::wstring pulse_kind, pulse_rest;
    if (app::ParsePulsePath(p, &pulse_kind, &pulse_rest)) {
        if (pulse_kind == L"recycle") {
            BreadcrumbSegment pc;
            pc.text = pulse::l10n::Get(pulse::l10n::StringId::ThisPc);
            pc.path = L"";
            out.push_back(pc);
            BreadcrumbSegment bin;
            bin.text = pulse::l10n::Get(pulse::l10n::StringId::RecycleBin);
            bin.path = L"pulse:recycle";
            out.push_back(bin);
            return out;
        }
        BreadcrumbSegment seg;
        seg.path = path;
        if (pulse_kind == L"home")
            seg.text = pulse::l10n::Get(pulse::l10n::StringId::Home);
        else if (pulse_kind == L"search" || pulse_kind == L"saved-search")
            seg.text = pulse::l10n::Get(pulse::l10n::StringId::Search);
        else if (pulse_kind == L"starred")
            seg.text = pulse::l10n::Get(pulse::l10n::StringId::StarredItems);
        else if (pulse_kind == L"recent")
            seg.text = pulse::l10n::Get(pulse::l10n::StringId::Recent);
        else if (pulse_kind == L"settings")
            seg.text = pulse::l10n::Get(pulse::l10n::StringId::Settings);
        else if (pulse_kind == L"tag")
            seg.text = pulse_rest.empty()
                ? pulse::l10n::Get(pulse::l10n::StringId::Tag) : pulse_rest;
        else
            seg.text = pulse_kind;
        out.push_back(seg);
        return out;
    }

    std::wstring prefix; // full path of the segments emitted so far
    size_t i = 0;
    if (p.size() >= 2 && p[1] == L':') {
        // Explorer-style: drives live under This PC.
        BreadcrumbSegment pc;
        pc.text = pulse::l10n::Get(pulse::l10n::StringId::ThisPc);
        pc.path = L"";
        out.push_back(pc);
        // Drive root, e.g. "C:\": single segment with the drive icon text.
        prefix = p.substr(0, 2);
        BreadcrumbSegment seg;
        seg.text = prefix;
        seg.path = prefix + L"\\";
        out.push_back(seg);
        i = 2;
        while (i < p.size() && p[i] == L'\\') ++i;
        prefix += L"\\";
    } else if (p.size() >= 2 && p[0] == L'\\' && p[1] == L'\\') {
        // UNC: split the root into a server segment and a share segment.
        auto s3 = p.find(L'\\', 2);           // after server
        std::wstring server = (s3 == std::wstring::npos) ? p.substr(2)
                                                         : p.substr(2, s3 - 2);
        BreadcrumbSegment srv;
        srv.text = server;
        srv.path = L"\\\\" + server;
        out.push_back(srv);
        prefix = srv.path;
        if (s3 == std::wstring::npos) {
            i = p.size();
        } else {
            auto s4 = p.find(L'\\', s3 + 1);  // after share
            std::wstring share = (s4 == std::wstring::npos) ? p.substr(s3 + 1)
                                                            : p.substr(s3 + 1, s4 - s3 - 1);
            if (!share.empty()) {
                BreadcrumbSegment seg;
                seg.text = share;
                seg.path = prefix + L"\\" + share;
                out.push_back(seg);
                prefix = seg.path;
            }
            i = (s4 == std::wstring::npos) ? p.size() : s4 + 1;
        }
    }
    while (i <= p.size() && i < p.size()) {
        auto sep = p.find(L'\\', i);
        std::wstring part = (sep == std::wstring::npos) ? p.substr(i)
                                                        : p.substr(i, sep - i);
        if (!part.empty()) {
            if (!prefix.empty() && prefix.back() != L'\\') prefix += L'\\';
            prefix += part;
            BreadcrumbSegment seg;
            seg.text = pulse::path::KnownFolderDisplayName(prefix, part);
            seg.path = prefix;
            out.push_back(seg);
        }
        if (sep == std::wstring::npos) break;
        i = sep + 1;
    }
    return out;
}

void MainRenderer::BreadcrumbLayout(const PaneViewModel& vm, float w,
                                    std::vector<BreadcrumbPlaced>& out) const {
    out.clear();
    auto segments = SplitBreadcrumb(vm.path);
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
        widths[i] = MeasureTextWidth(dwrite, fmt, segments[i].text) + segPad * 2;
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
        p.rc = D2D1::RectF(x, addr.top + 2 * scale_, x + widths[i], addr.bottom - 2 * scale_);
        p.text = segments[i].text;
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
    dc->DrawText(text.data(), (UINT32)text.size(), fmt, &rc, br, opts, DWRITE_MEASURING_MODE_NATURAL);
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
    if (bg.a > 0.0f) { // transparent = resting state; hover fill is drawn on interaction only
        MakeBrush(dc, bg, brFillHover_);
        float r = theme.radius_control * scale_;
        FillRoundedRect(dc, brFillHover_.get(), rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top, r);
    }
    DrawIconText(rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top, glyph, fallback, fg,
        size_factor);
}

void MainRenderer::Render(const WindowViewModel& vm, const D2D1_RECT_F& rect,
                          const Theme& theme) {
    if (!compositor_ || !compositor_->Dc()) return;
    ID2D1DeviceContext* dc = compositor_->Dc();
    icon_cache_.SetDeviceContext(compositor_->Dc());
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
        tint_background(rect, theme.surface_title, layers.title);
    }
    // One sheet below the title strip carries the toolbar, sidebar and status
    // bar; the active tab uses the same fill and meets it on a whole pixel so
    // translucent layers never double up into a seam.
    if (sheet_alpha_ > 0.0f) {
        const float sheetTop = std::round(title_bar_height_);
        tint_background({rect.left, sheetTop, rect.right, rect.bottom}, theme.surface_sheet, sheet_alpha_);
    }

    DrawTitleBar(vm, rect, theme);
    if (vm.settings_open) {
        preview_handler_.Sync(notify_hwnd_, {}, L"", 0, 0, 0, 0, vm.dark,
                              theme.bg, theme.text, false);
        DrawSettings(vm, rect, theme);
        DrawStatusBar(vm, rect, theme);
    } else {
        DrawToolbar(vm, rect, theme);
        DrawSidebar(vm, rect, theme);
        DrawPane(vm, rect, theme);
        if (vm.details_visible) DrawDetailsPanel(vm, rect, theme);
        else preview_handler_.Sync(notify_hwnd_, {}, L"", 0, 0, 0, 0, vm.dark,
                                   theme.bg, theme.text, false);
        DrawStatusBar(vm, rect, theme);
    }

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
    if (!vm.change_popover.visible && vm.drag_badge.empty() && !vm.tooltip_text.empty()) {
        IDWriteTextFormat* fmt = compositor_->SmallFormat();
        const float tw = MeasureLayoutText(compositor_, compositor_->DwriteFactory(), fmt,
                                           vm.tooltip_text);
        const float bw = std::min(tw + 20.0f * scale_, rect.right - 16.0f * scale_);
        const float bh = 28.0f * scale_;
        const float bx = std::clamp(vm.tooltip_x + 12.0f * scale_, 8.0f * scale_,
            std::max(8.0f * scale_, rect.right - bw - 8.0f * scale_));
        const float tipY = vm.hover_region == HitTestResult::StatusBarCancelSearch
            ? rect.bottom - status_height_ - bh - 8.0f * scale_ : vm.tooltip_y + 18.0f * scale_;
        const float by = std::clamp(tipY, 8.0f * scale_,
            std::max(8.0f * scale_, rect.bottom - bh - 8.0f * scale_));
        const D2D1_RECT_F tipRc = D2D1::RectF(bx, by, bx + bw, by + bh);
        MakeBrush(dc, theme.surface_flyout, brFillHover_);
        FillRoundedRect(dc, brFillHover_.get(), bx, by, bw, bh, 4.0f * scale_);
        MakeBrush(dc, theme.stroke_card, brStrokeCard_);
        dc->DrawRoundedRectangle(D2D1::RoundedRect(tipRc, 4.0f * scale_, 4.0f * scale_),
            brStrokeCard_.get(), 1.0f);
        MakeBrush(dc, theme.text, brText_);
        const D2D1_COLOR_F saved_tip_bg = text_background_;
        text_background_ = BlendOver(theme.surface_flyout, theme.bg);
        DrawTextRect(dc, fmt, brText_.get(), vm.tooltip_text,
            bx + 10.0f * scale_, by, bw - 20.0f * scale_, bh);
        text_background_ = saved_tip_bg;
    }

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
    auto drawTab = [&](size_t i, float left, bool raised) {
        const bool active = vm.tabs[i].active;
        const bool pinned = vm.tabs[i].pinned;
        const float tabW = pinned ? (vm.show_pinned_tab_names ? kTabPinnedNamedW : kTabPinnedW) * scale_ : strip.w;
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
            const bool has_color = vm.tabs[i].color_rgb != 0;
            if (has_color) {
                D2D1_COLOR_F tint = HexColor(vm.tabs[i].color_rgb);
                tint.a *= hovered ? 0.16f : 0.10f;
                MakeBrush(dc, tint, brFillHover_);
            } else {
                MakeBrush(dc, hovered ? theme.fill_hover : kTransparent, brFillHover_);
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
        if (active) {
            MakeBrush(dc, vm.tabs[i].color_rgb ? HexColor(vm.tabs[i].color_rgb) : theme.accent, brAccent_);
            const float inset = pinned && !vm.show_pinned_tab_names ? 8.0f * scale_ : 16.0f * scale_;
            FillRoundedRect(dc, brAccent_.get(), tabRc.left + inset, tabRc.bottom - 3*scale_,
                (std::max)(0.0f, tabW - 2.0f * inset), 2*scale_, scale_);
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
                    vm.tabs[i].title.empty() ? kIconFolder
                        : vm.tabs[i].title == pulse::l10n::Get(pulse::l10n::StringId::Settings)
                            ? kIconSettings : kIconFolder, L"[]",
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
                vm.tabs[i].title == pulse::l10n::Get(pulse::l10n::StringId::Settings)
                    ? kIconSettings : kIconFolder, L"[]",
                active ? theme.icon_folder : theme.text_secondary, 0.85f);
        }
        MakeBrush(dc, theme.text, brText_);
        const bool show_close = TabCloseVisible(vm, static_cast<int>(i), tabW, scale_);
        const float closeSz = kTabCloseSizeDip * scale_;
        const float closePad = kTabClosePadDip * scale_;
        const float closeReserve = show_close ? (closePad + closeSz + 6.0f * scale_)
                                              : 6.0f * scale_;
        const float titleLeft = left + 24.0f * scale_ + markerReserve;
        DrawTabTitle(dc, compositor_->DwriteFactory(), compositor_->TabFormat(), brText_.get(),
                     vm.tabs[i].title, titleLeft, tabY,
                     std::max(0.0f, tabW - 24.0f * scale_ - markerReserve - closeReserve), tabH, scale_);
        if (show_close) {
            const float closeY = tabY + (tabH - closeSz) * 0.5f;
            const float closeX = left + tabW - closePad - closeSz;
            if (IsHovered(vm, HitTestResult::TabClose, static_cast<int>(i))) {
                MakeBrush(dc, theme.fill_hover, brFillPressed_);
                FillRoundedRect(dc, brFillPressed_.get(), closeX, closeY, closeSz, closeSz, r);
            }
            DrawIconText(closeX, closeY, closeSz, closeSz,
                kIconCloseSmall, L"x", theme.text_secondary, 0.62f);
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
        fill.a *= named ? (chip_hovered ? 0.42f : 0.32f)
                        : (chip_hovered ? 1.0f : 0.85f);
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
        const float extra = i < strip.extra.size() ? strip.extra[i] : 0.0f;
        const float left = strip.x0
            + (static_cast<float>(i) + vm.tabs[i].x_offset) * strip.pitch + extra;
        drawTab(i, left, false);
    }
    if (activeI >= 0 && !inDragRun(activeI) && !vm.tabs[static_cast<size_t>(activeI)].hidden) {
        const size_t i = static_cast<size_t>(activeI);
        const float extra = i < strip.extra.size() ? strip.extra[i] : 0.0f;
        const float left = strip.x0
            + (static_cast<float>(i) + vm.tabs[i].x_offset) * strip.pitch + extra;
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
        if (inDragRun(i)) return vm.tab_drag_x + static_cast<float>(i - dragI) * strip.pitch;
        const float extra = i < static_cast<int>(strip.extra.size())
            ? strip.extra[static_cast<size_t>(i)] : 0.0f;
        return strip.x0
            + (static_cast<float>(i) + vm.tabs[static_cast<size_t>(i)].x_offset) * strip.pitch
            + extra;
    };
    const int connected = dragI >= 0 ? dragI : activeI;
    if (connected >= 0 && connected < static_cast<int>(vm.tabs.size())) {
        const float connW = vm.tabs[static_cast<size_t>(connected)].pinned
            ? (vm.show_pinned_tab_names ? kTabPinnedNamedW : kTabPinnedW) * scale_ : strip.w;
        const float shoulder = 8.0f * scale_;
        const float cut_l = tab_left_at(connected) - shoulder;
        const float cut_r = tab_left_at(connected) + connW + shoulder;
        if (cut_l > 0.0f)
            FillRect(dc, brStrokeDivider_.get(), 0.0f, h - 1.0f, cut_l, 1.0f);
        if (cut_r < rect.right)
            FillRect(dc, brStrokeDivider_.get(), cut_r, h - 1.0f, rect.right - cut_r, 1.0f);
    } else {
        FillRect(dc, brStrokeDivider_.get(), 0.0f, h - 1.0f, rect.right, 1.0f);
    }

    const float new_tab_x = strip.end_x;
    // New tab button follows the final rest slot (not the sliding tabs).
    D2D1_RECT_F newRc = D2D1::RectF(new_tab_x, tabY, new_tab_x + 32 * scale_, tabY + tabH);
    DrawButton(newRc, theme, IsHovered(vm, HitTestResult::TabNew) ? theme.fill_hover : kTransparent,
        kIconAdd, L"+", theme.text_secondary, true, true);

    // Window controls, right-aligned in Win11 order: min, max/restore, close.
    const float ctrlY = y;
    const float ctrlH = h;
    float cx = right;
    cx -= ctrlW;
    D2D1_RECT_F closeRc = D2D1::RectF(cx, ctrlY, cx + ctrlW, ctrlY + ctrlH);
    if (IsHovered(vm, HitTestResult::Close)) {
        MakeBrush(dc, theme.danger, brDanger_);
        FillRect(dc, brDanger_.get(), closeRc.left, closeRc.top, ctrlW, ctrlH);
    }
    DrawIconText(closeRc.left, closeRc.top, ctrlW, ctrlH, kIconClose, L"x",
        IsHovered(vm, HitTestResult::Close) ? HexColor(0xFFFFFF) : theme.text, 0.66f);
    cx -= ctrlW;
    D2D1_RECT_F maxRc = D2D1::RectF(cx, ctrlY, cx + ctrlW, ctrlY + ctrlH);
    if (IsHovered(vm, HitTestResult::Maximize)) {
        MakeBrush(dc, theme.fill_hover, brFillHover_);
        FillRect(dc, brFillHover_.get(), maxRc.left, maxRc.top, ctrlW, ctrlH);
    }
    DrawIconText(maxRc.left, maxRc.top, ctrlW, ctrlH,
        vm.maximized ? kIconRestore : kIconMaximize, vm.maximized ? L"[]" : L"\u25A1",
        theme.text, 0.66f);
    cx -= ctrlW;
    D2D1_RECT_F minRc = D2D1::RectF(cx, ctrlY, cx + ctrlW, ctrlY + ctrlH);
    if (IsHovered(vm, HitTestResult::Minimize)) {
        MakeBrush(dc, theme.fill_hover, brFillHover_);
        FillRect(dc, brFillHover_.get(), minRc.left, minRc.top, ctrlW, ctrlH);
    }
    DrawIconText(minRc.left, minRc.top, ctrlW, ctrlH, kIconMinimize, L"_", theme.text, 0.66f);
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
    const float statusLimit = centered_progress ? std::min(rect.right * 0.30f, sb.task.left - gap) : rect.right * 0.30f;
    const std::wstring status_text = path::FriendlyPathText(vm.status.status_text);
    const float statusWidth = std::min(std::max(0.0f, statusLimit - sb.pad),
        MeasureTextWidth(factory, small_fmt, status_text));
    DrawTextEndEllipsis(dc, factory, small_fmt, brTextSecondary_.get(), status_text,
        sb.pad, y, statusWidth, status_height_);
    const float selectionLeft = sb.pad + statusWidth + gap;
    const bool hasTask = vm.status.query_active || !vm.status.task_text.empty() || vm.status.task_progress >= 0.0f;
    const float selectionRight = hasTask ? sb.task.left - gap : rect.right - sb.right_reserved - gap;
    DrawTextEndEllipsis(dc, factory, small_fmt, brTextSecondary_.get(), vm.status.selection_text,
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
        DrawTextEndEllipsis(dc,factory,small_fmt,brAccentText_.get(),progress_text,
            textX,y,textWidth,status_height_);
        fluent::ProgressSpec progress;
        progress.bounds=D2D1::RectF(trackX,y,std::min(taskRight,trackX+trackWidth),y+status_height_);
        progress.value=std::clamp(progress_value,0.0f,1.0f);
        progress.indeterminate=progress_value<0.0f;
        progress.animation_progress=static_cast<float>(GetTickCount64()%1952)/1952.0f;
        painter_.DrawProgressBar(progress);
    } else if (!vm.status.task_text.empty() || vm.status.task_progress >= 0.0f) {
        float barW = (vm.status.task_progress >= 0.0f) ? (100 * scale_ + margin_ * 2) : 0.0f;
        MakeBrush(dc, theme.accent, brAccentText_);
        DrawTextRect(dc, small_fmt, brAccentText_.get(), vm.status.task_text,
            taskX, y, std::max(0.0f, taskRight - taskX - barW), status_height_);
        if (barW > 0.0f) {
            float trackH = 4 * scale_;
            float trackX = taskRight - 100 * scale_;
            float trackY = y + (status_height_ - trackH) * 0.5f;
            MakeBrush(dc, theme.stroke_card, brStrokeCard_);
            FillRoundedRect(dc, brStrokeCard_.get(), trackX, trackY, 100 * scale_, trackH, trackH * 0.5f);
            float fillW = 100 * scale_ * std::clamp(vm.status.task_progress / 100.0f, 0.0f, 1.0f);
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
    m.x0 = 12.0f * scale_;
    const float tabsRight = chrome.chrome_left - 8.0f * scale_;

    // Group chips: one at the start of each consecutive same-group run. Their
    // widths come out of the strip budget before tabs are sized; positions
    // are resolved in the second pass once the tab pitch is known.
    const float chipGap = 4.0f * scale_;
    float chipsTotal = 0.0f;
    IDWriteTextFormat* chipFmt = compositor_ ? compositor_->SmallFormat() : nullptr;
    for (size_t i = 0; i < vm.tabs.size(); ++i) {
        const int g = vm.tabs[i].group;
        const bool runStart = g >= 0 && (i == 0 || vm.tabs[i - 1].group != g);
        if (!runStart) continue;
        float cw = 8.0f * scale_; // unnamed group: slim color bar
        if (g < static_cast<int>(vm.tab_groups.size()) &&
            !vm.tab_groups[static_cast<size_t>(g)].name.empty()) {
            const float tw = std::min(88.0f * scale_,
                MeasureTextWidth(compositor_->DwriteFactory(), chipFmt,
                                 vm.tab_groups[static_cast<size_t>(g)].name));
            cw = tw + 16.0f * scale_; // Edge-style block: text + side padding
        }
        TabStripMetrics::Chip chip;
        chip.width = cw;
        chip.group = g;
        chipsTotal += cw + chipGap;
        m.chips.push_back(chip);
    }
    m.extra.assign(vm.tabs.size(), 0.0f);

    const float available = std::max(0.0f, tabsRight - m.x0 - 36.0f * scale_ - chipsTotal);
    size_t visibleCount = 0;
    size_t pinnedCount = 0; // visible pinned tabs get a fixed narrow slot
    for (const auto& t : vm.tabs) {
        if (t.hidden) continue;
        if (t.pinned) ++pinnedCount; else ++visibleCount;
    }
    const float pinnedW = (vm.show_pinned_tab_names ? kTabPinnedNamedW : kTabPinnedW) * scale_;
    const float pinnedTotal = static_cast<float>(pinnedCount) * (pinnedW + control_gap_);
    m.w = visibleCount == 0 ? 0.0f
        : std::min(kTabMaxW * scale_, std::max(kTabMinW * scale_,
            std::max(0.0f, available - pinnedTotal)
                / static_cast<float>(visibleCount) - control_gap_));
    m.y = 4.0f * scale_;
    m.h = title_bar_height_ - 8.0f * scale_;
    m.pitch = m.w + control_gap_;

    // Final pass: per-tab extra offset + definitive chip positions. Collapsed
    // members contribute zero width (chip stays visible at the fold point);
    // pinned tabs use the fixed narrow slot.
    float acc = 0.0f;
    size_t chipIdx = 0;
    for (size_t i = 0; i < vm.tabs.size(); ++i) {
        const int g = vm.tabs[i].group;
        const bool runStart = g >= 0 && (i == 0 || vm.tabs[i - 1].group != g);
        if (runStart && chipIdx < m.chips.size()) {
            TabStripMetrics::Chip& chip = m.chips[chipIdx++];
            chip.left = m.x0 + static_cast<float>(i) * m.pitch + acc;
            acc += chip.width + chipGap;
        }
        m.extra[i] = acc;
        if (vm.tabs[i].hidden) acc -= m.pitch;
        else if (vm.tabs[i].pinned) acc += pinnedW - m.w;
    }
    m.end_x = m.x0 + chipsTotal + static_cast<float>(pinnedCount) *
        (pinnedW + control_gap_) + static_cast<float>(visibleCount) * m.pitch;
    return m;
}

} // namespace pulse::ui
