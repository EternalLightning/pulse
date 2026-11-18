// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "typography.h"
#include "ui_renderer.h"
#include "ui_renderer_internal.h"
#include "../common/localization.h"

namespace pulse::ui {
void MainRenderer::DrawSettingsCore(const WindowViewModel& vm, const D2D1_RECT_F& rect, const Theme& theme) {
    using I = l10n::StringId;
    using H = HitTestResult;
    auto* dc=compositor_->Dc();
    const auto lay=MakeSettingsLayout(vm,rect,scale_,title_bar_height_,status_height_,&painter_, EffectiveSidebarWidth(rect.right));
    auto text=[&](const std::wstring& value,D2D1_RECT_F r,bool is_small=false) {
        painter_.DrawText(value,r,is_small ? compositor_->SmallFormat() : compositor_->TextFormat(),is_small ? theme.text_secondary : theme.text);
    };
    auto draw_card=[&](D2D1_RECT_F r) {
        if(r.bottom<=r.top) return;
        MakeBrush(dc,(vm.dark && !IsHighContrast() ? theme.fill_input : WithAlpha(theme.surface_card,std::max(card_alpha_,0.65f))),brFillInput_); MakeBrush(dc,theme.stroke_card,brStrokeCard_);
        dc->FillRoundedRectangle(D2D1::RoundedRect(r,8*scale_,8*scale_),brFillInput_.get());
        dc->DrawRoundedRectangle(D2D1::RoundedRect(r,8*scale_,8*scale_),brStrokeCard_.get(),1);
    };
    auto label=[&](D2D1_RECT_F r,const std::wstring& title,const std::wstring& desc,const wchar_t* icon,float text_right=0.0f,float description_height=21.0f) {
        const float right=text_right>0 ? text_right : r.right-72*scale_;
        const bool inline_control = right < r.right-60*scale_;
        const float text_height = (24.0f + (desc.empty() ? 0.0f : 1.0f+description_height))*scale_;
        const float top = inline_control ? (r.top+r.bottom-text_height)*0.5f : r.top+10*scale_;
        const float icon_top = inline_control ? (r.top+r.bottom-24*scale_)*0.5f : r.top+17*scale_;
        DrawIconText(r.left+16*scale_,icon_top,24*scale_,24*scale_,icon,L"",theme.text_secondary,0.85f);
        text(title,D2D1::RectF(r.left+54*scale_,top,right,top+24*scale_));
        if(!desc.empty()) {
            const auto bounds=D2D1::RectF(r.left+54*scale_,top+25*scale_,right,top+(25+description_height)*scale_);
            if(description_height<=21.0f) text(desc,bounds,true);
            else {
                ComPtr<IDWriteTextLayout> description;
                if(SUCCEEDED(typography::CreateTextLayout(compositor_->DwriteFactory(), desc.c_str(),static_cast<UINT32>(desc.size()),
                    compositor_->SmallFormat(),bounds.right-bounds.left,bounds.bottom-bounds.top,&description))) {
                    description->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
                    description->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
                    MakeBrush(dc,theme.text_secondary,brTextSecondary_);
                    compositor_->DrawTextLayout(D2D1::Point2F(bounds.left,bounds.top),description.get(),brTextSecondary_.get(),D2D1_DRAW_TEXT_OPTIONS_CLIP);
                }
            }
        }
    };
    auto toggle=[&](D2D1_RECT_F r,I title,I desc,const wchar_t* icon,bool on,int hit) {
        if(IsHovered(vm,H::SettingsToggle,hit)) {
            if (IsHighContrast()) painter_.StrokeRoundedRect(r, 8*scale_, theme.stroke_card);
            else {
                MakeBrush(dc,SettingsPressed(vm,r) ? theme.fill_pressed : theme.fill_hover,brFillHover_);
                FillRoundedRect(dc,brFillHover_.get(),r.left+2*scale_,r.top+2*scale_,r.right-r.left-4*scale_,r.bottom-r.top-4*scale_,6*scale_);
            }
        }
        const auto bounds=SettingsToggleBounds(r,l10n::Get(desc),scale_,&painter_);
        DrawIconText(bounds.icon.left,bounds.icon.top,bounds.icon.right-bounds.icon.left,
            bounds.icon.bottom-bounds.icon.top,icon,L"",theme.text_secondary,0.85f);
        const auto& heading=l10n::Get(title);
        const auto fitted=FitEndEllipsis(heading,bounds.title.right-bounds.title.left,[&](const std::wstring& value) {
            return MeasureTextWidth(compositor_->DwriteFactory(),compositor_->TextFormat(),value);
        });
        painter_.DrawText(fitted,bounds.title,compositor_->TextFormat(),theme.text);
        painter_.DrawWrappedCaption(l10n::Get(desc),D2D1::Point2F(bounds.description.left,bounds.description.top),
            bounds.description.right-bounds.description.left,theme.text_secondary);
        // Busy is enforced in HitTest/controller, not by recoloring every sibling
        // switch. Its geometry and checked appearance stay stable until result.
        fluent::ControlState state{}; state.checked=on; state.hovered=IsHovered(vm,H::SettingsToggle,hit);
        DrawSettingsSwitch(vm, L"setting:"+std::to_wstring(hit),bounds.control,state);
    };
    auto button=[&](D2D1_RECT_F r,const std::wstring& value,H::Region region,int index,bool primary=false,bool enabled=true,const wchar_t* glyph=L"") {
        fluent::ControlState st{}; st.enabled=enabled; st.hovered=enabled && IsHovered(vm,region,index);
        DrawSettingsButton(vm, {r,value,glyph,primary ? fluent::ButtonKind::Primary : fluent::ButtonKind::Standard,st});
    };
    auto disclosure=[&](D2D1_RECT_F r,I title,I desc,const wchar_t* icon,int id,bool own_card) {
        if(own_card) draw_card(r);
        const float hover = ControlHoverOpacity(r, IsHovered(vm,H::SettingsDisclosure,id));
        if(hover > 0.0f && IsHighContrast()) painter_.StrokeRoundedRect(r, 8*scale_, theme.stroke_card);
        if(hover > 0.0f && !IsHighContrast()) {
            auto fill = theme.fill_hover; fill.a *= hover;
            MakeBrush(dc,fill,brFillHover_);
            FillRoundedRect(dc,brFillHover_.get(),r.left+2*scale_,r.top+2*scale_,r.right-r.left-4*scale_,r.bottom-r.top-4*scale_,6*scale_);
        }
        label(r,l10n::Get(title),l10n::Get(desc),icon);
        const float expansion = DisclosureProgress(L"settings-disclosure:" + std::to_wstring(vm.settings_page) +
            L":" + std::to_wstring(id), (vm.settings_expanded&(1u<<id)) != 0);
        D2D1_MATRIX_3X2_F transform; dc->GetTransform(&transform);
        dc->SetTransform(D2D1::Matrix3x2F::Rotation(90.0f * expansion,
            D2D1::Point2F(r.right-29*scale_, r.top+31*scale_)) * transform);
        DrawIconText(r.right-38*scale_,r.top+22*scale_,18*scale_,18*scale_,
            L"\xE76C",L"",theme.text_secondary,0.75f);
        dc->SetTransform(transform);
    };
    auto section=[&](int i,I title) { text(l10n::Get(title),lay.section[i]); };
    auto segmented=[&](D2D1_RECT_F card,D2D1_RECT_F const* choices,const I* labels,const int* values,int current,H::Region hit,I title,const std::wstring& desc,float description_height=21.0f) {
        const bool stacked=choices[0].left<card.left+100*scale_;
        label(card,l10n::Get(title),desc,L"\xE8A4",stacked ? card.right-16*scale_ : choices[0].left-12*scale_,description_height);
        painter_.DrawSegmentedTrack(D2D1::RectF(choices[0].left,choices[0].top,choices[2].right,choices[2].bottom));
        for(int i=0;i<3;++i) {
            fluent::SegmentedItemSpec item{};item.bounds=choices[i];item.text=l10n::Get(labels[i]);
            item.state.checked=current==values[i];item.state.hovered=IsHovered(vm,hit,i);
            item.shared_track=true; item.position=i==0 ? fluent::SegmentPosition::First : i==2 ? fluent::SegmentPosition::Last : fluent::SegmentPosition::Middle;
            DrawSettingsSegment(vm, item);
        }
    };
    if (IsSettingsCategory(vm.settings_page)) {
        for (int i = 0; i < lay.section_count; ++i) section(i, lay.section_title[i]);
        for (const auto &row : lay.category_rows) draw_card(row);
        const I languages[] = {I::LanguageSystem, I::LanguageZhCN, I::LanguageEnUS};
        const auto dropdown = [&](D2D1_RECT_F r, D2D1_RECT_F c, I title, I desc, const std::wstring &value, int id) {
            label(r, l10n::Get(title), l10n::Get(desc), id == 0 ? L"\xE790" : L"\xE8C1",
                  c.left < r.left + 100 * scale_ ? r.right - 16 * scale_ : c.left - 12 * scale_);
            fluent::ButtonSpec control{};
            control.bounds = c;
            control.text = value;
            control.drop_down = true;
            control.state.hovered = IsHovered(vm, H::SettingsDropdown, id);
            DrawSettingsButton(vm, control);
        };
        if (vm.settings_page == 0) {
            const bool small_theme = lay.theme_tile[0].left < lay.theme_row.left + 100 * scale_;
            label(lay.theme_row, l10n::Get(I::SettingsTheme), small_theme ? L"" : l10n::Get(I::SettingsThemeDesc),
                  L"\xE790", small_theme ? lay.theme_row.right - 16 * scale_ : lay.theme_tile[0].left - 12 * scale_);
            const I theme_names[] = {I::SettingsThemeLight, I::SettingsThemeDark, I::SettingsThemeSystem};
            const int theme_values[] = {1, 2, 0};
            for (int i = 0; i < 3; ++i) {
                const auto r = lay.theme_tile[i];
                auto preview = r;
                preview.bottom -= 22 * scale_;
                const bool selected = vm.settings_theme == theme_values[i];
                MakeBrush(dc, i == 0 ? HexColor(0xF3F5F8) : HexColor(0x1A1A1A), brFillHover_);
                dc->FillRoundedRectangle(D2D1::RoundedRect(preview, 5 * scale_, 5 * scale_), brFillHover_.get());
                MakeBrush(dc, i == 0 ? HexColor(0xFFFFFF) : HexColor(0x333333), brFillHover_);
                FillRoundedRect(dc, brFillHover_.get(), preview.left + 7 * scale_, preview.top + 8 * scale_,
                                preview.right - preview.left - 14 * scale_, 8 * scale_, 2 * scale_);
                FillRoundedRect(dc, brFillHover_.get(), preview.left + 7 * scale_, preview.top + 21 * scale_,
                                14 * scale_, preview.bottom - preview.top - 28 * scale_, 2 * scale_);
                if (i == 2) {
                    MakeBrush(dc, HexColor(0xF3F5F8), brFillHover_);
                    FillRect(dc, brFillHover_.get(), preview.left + 7 * scale_, preview.top + 8 * scale_,
                             (preview.right - preview.left) / 2 - 7 * scale_,
                             preview.bottom - preview.top - 15 * scale_);
                }
                MakeBrush(dc,
                          selected                                           ? theme.accent
                          : IsHovered(vm, H::SettingsTheme, theme_values[i]) ? theme.text_secondary
                                                                             : theme.stroke_card,
                          brStrokeCard_);
                dc->DrawRoundedRectangle(D2D1::RoundedRect(preview, 5 * scale_, 5 * scale_), brStrokeCard_.get(),
                                         selected ? 2 * scale_ : 1);
                if (selected)
                    DrawIconText(preview.right - 23 * scale_, preview.top + 3 * scale_, 20 * scale_, 20 * scale_,
                                 L"\xE73E", L"", !vm.dark && i != 0 ? HexColor(0xFFFFFF) : theme.accent_ink, 0.7f);
                painter_.DrawText(l10n::Get(theme_names[i]),
                                  D2D1::RectF(r.left, preview.bottom + 2 * scale_, r.right, r.bottom),
                                  compositor_->SmallFormat(), theme.text, fluent::HorizontalAlignment::Center);
            }

            const bool narrow_accent = lay.accent_picker.left < lay.accent_card.left + 250 * scale_;
            label(lay.accent_card, l10n::Get(I::SettingsThemeColor), l10n::Get(I::SettingsThemeColorDesc), L"\xE790",
                  narrow_accent ? lay.accent_card.right - 16 * scale_ : lay.accent_picker.left - 12 * scale_);
            button(lay.accent_picker, L"#" + vm.settings_accent_hex, H::SettingsAccent, 0);
            button(lay.accent_system, l10n::Get(I::SettingsThemeSystem), H::SettingsAccent, 1, false, true,
                   vm.settings_accent_system ? L"\xE73E" : L"");

            const I effects[] = {I::EffectNone, I::EffectAcrylic, I::EffectMica, I::EffectMicaAlt};
            label(lay.effect_card, l10n::Get(I::SettingsWindowEffect), l10n::Get(I::SettingsWindowEffectDesc),
                  L"\xE790", lay.effect_card.right - 16 * scale_);
            for (int i = 0; i < kWindowEffectCount; ++i) {
                const auto r = lay.effect_row[i];
                auto preview = r;
                preview.bottom -= 22 * scale_;
                const bool selected = i == static_cast<int>(vm.window_effect);
                const bool hovered = IsHovered(vm, H::SettingsEffect, i);
                // One fixed wallpaper makes the static material illustrations comparable.
                const auto wallpaper_blue = HexColor(0x547BA5);
                const auto wallpaper_purple = HexColor(0xAE80B6);
                const auto wallpaper_teal = HexColor(0x54A8A1);
                MakeBrush(dc, wallpaper_blue, brFillHover_);
                dc->FillRoundedRectangle(D2D1::RoundedRect(preview, 6 * scale_, 6 * scale_), brFillHover_.get());
                dc->PushAxisAlignedClip(preview, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
                const float wallpaper_rx = (std::min)(36 * scale_, (preview.right - preview.left) * 0.32f);
                const float wallpaper_ry = (preview.bottom - preview.top - 12 * scale_) * 0.5f;
                MakeBrush(dc, wallpaper_purple, brFillHover_);
                dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(preview.right - 6 * scale_ - wallpaper_rx,
                    preview.top + 6 * scale_ + wallpaper_ry), wallpaper_rx, wallpaper_ry), brFillHover_.get());
                MakeBrush(dc, wallpaper_teal, brFillHover_);
                dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(preview.left + 6 * scale_ + wallpaper_rx,
                    preview.bottom - 6 * scale_ - wallpaper_ry), wallpaper_rx, wallpaper_ry), brFillHover_.get());
                const float inset = 10 * scale_;
                auto window = D2D1::RectF(preview.left + inset, preview.top + 9 * scale_, preview.right - inset,
                                          preview.bottom - 9 * scale_);
                MakeBrush(dc, theme.surface_title, brFillInput_);
                dc->FillRoundedRectangle(D2D1::RoundedRect(window, 3 * scale_, 3 * scale_), brFillInput_.get());
                // Acrylic shows softened background colors; opaque Mica uses wallpaper tint,
                // with a stronger tint for Mica Alt. These are illustrations, not live DWM.
                if (i != 0) {
                    const float tint = i == 1 ? 0.52f : i == 2 ? 0.10f : 0.26f;
                    const D2D1_GRADIENT_STOP stops[] = {
                        {0.0f, BlendOver(WithAlpha(wallpaper_teal, tint), theme.surface_title)},
                        {0.5f, BlendOver(WithAlpha(wallpaper_blue, tint), theme.surface_title)},
                        {1.0f, BlendOver(WithAlpha(wallpaper_purple, tint), theme.surface_title)}};
                    ComPtr<ID2D1GradientStopCollection> colors;
                    ComPtr<ID2D1LinearGradientBrush> material;
                    if (SUCCEEDED(dc->CreateGradientStopCollection(stops, ARRAYSIZE(stops), &colors)) &&
                        SUCCEEDED(dc->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(
                            D2D1::Point2F(window.left, window.bottom), D2D1::Point2F(window.right, window.top)),
                            colors.get(), &material))) {
                        dc->FillRoundedRectangle(D2D1::RoundedRect(window, 3 * scale_, 3 * scale_), material.get());
                    }
                }
                MakeBrush(dc, WithAlpha(theme.text_secondary, 0.30f), brTextSecondary_);
                FillRoundedRect(dc, brTextSecondary_.get(), window.left + 6 * scale_, window.top + 6 * scale_,
                                (std::min)(18 * scale_, (window.right - window.left) * 0.18f), 2 * scale_, scale_);
                MakeBrush(dc, WithAlpha(theme.accent, 0.10f), brFillHover_);
                FillRoundedRect(dc, brFillHover_.get(), window.right - 22 * scale_, window.top + 5 * scale_,
                                16 * scale_, window.bottom - window.top - 10 * scale_, 2 * scale_);
                dc->PopAxisAlignedClip();
                MakeBrush(dc,
                          selected  ? theme.accent
                          : hovered ? theme.text_secondary
                                    : theme.stroke_card,
                          brStrokeCard_);
                dc->DrawRoundedRectangle(D2D1::RoundedRect(preview, 6 * scale_, 6 * scale_), brStrokeCard_.get(),
                                         selected ? 1.5f * scale_ : scale_);
                if (selected) {
                    const auto center = D2D1::Point2F(preview.right - 9 * scale_, preview.bottom - 9 * scale_);
                    MakeBrush(dc, theme.accent, brAccent_);
                    dc->FillEllipse(D2D1::Ellipse(center, 7 * scale_, 7 * scale_), brAccent_.get());
                    DrawIconText(center.x - 6 * scale_, center.y - 6 * scale_, 12 * scale_, 12 * scale_, L"\xE73E", L"",
                                 theme.accent_text, 0.6f);
                }
                painter_.DrawText(l10n::Get(effects[i]),
                                  D2D1::RectF(r.left, preview.bottom + 3 * scale_, r.right, r.bottom),
                                  compositor_->SmallFormat(), selected ? theme.accent_ink : theme.text_secondary,
                                  fluent::HorizontalAlignment::Center);
            }

            const auto &preview = lay.wallpaper_preview;
            MakeBrush(dc, theme.fill_hover, brFillHover_);
            FillRoundedRect(dc, brFillHover_.get(), preview.left, preview.top, preview.right - preview.left,
                            preview.bottom - preview.top, 6.0f * scale_);
            if (!vm.background_image.empty()) material_.DrawSourceCover(dc, preview, vm.background_image);
            MakeBrush(dc, theme.stroke_card, brStrokeCard_);
            dc->DrawRoundedRectangle(D2D1::RoundedRect(preview, 6.0f * scale_, 6.0f * scale_), brStrokeCard_.get(),
                                     1.0f);
            const float text_left = preview.right + 12.0f * scale_;
            const bool compact_wallpaper = lay.wallpaper_choose.top > lay.wallpaper_card.top + 64.0f * scale_;
            const float text_right = compact_wallpaper ? lay.wallpaper_card.right - 16.0f * scale_
                                                       : lay.wallpaper_choose.left - 12.0f * scale_;
            MakeBrush(dc, theme.text, brText_);
            DrawTextRect(dc, compositor_->TextFormat(), brText_.get(),
                         pulse::l10n::Get(pulse::l10n::StringId::SettingsWallpaper), text_left,
                         lay.wallpaper_card.top + 18.0f * scale_, (std::max)(40.0f * scale_, text_right - text_left),
                         22.0f * scale_);
            const std::wstring wallpaper_desc = vm.background_image.empty()
                                                    ? pulse::l10n::Get(pulse::l10n::StringId::SettingsWallpaperDesc)
                                                    : FileNameOf(vm.background_image);
            MakeBrush(dc, theme.text_secondary, brTextSecondary_);
            DrawTextRect(dc, compositor_->SmallFormat(), brTextSecondary_.get(), wallpaper_desc, text_left,
                         lay.wallpaper_card.top + 42.0f * scale_, (std::max)(40.0f * scale_, text_right - text_left),
                         18.0f * scale_);
            fluent::ControlState choose{};
            choose.hovered = IsHovered(vm, HitTestResult::SettingsWallpaper, 0);
            DrawSettingsButton(vm, {lay.wallpaper_choose,
                                 pulse::l10n::Get(pulse::l10n::StringId::ChooseImage),
                                 {},
                                 fluent::ButtonKind::Standard,
                                 choose});
            fluent::ControlState clear{};
            clear.enabled = !vm.background_image.empty();
            clear.hovered = clear.enabled && IsHovered(vm, HitTestResult::SettingsWallpaper, 1);
            DrawSettingsButton(vm, {lay.wallpaper_clear,
                                 pulse::l10n::Get(pulse::l10n::StringId::Clear),
                                 {},
                                 fluent::ButtonKind::Standard,
                                 clear});

            const int levels[] = {0, 1, 2};

            label(lay.wallpaper_look_card, l10n::Get(I::SettingsWallpaperLook), l10n::Get(I::SettingsWallpaperLookDesc),
                  L"\xE8A4",
                  lay.wallpaper_look_value.left < lay.wallpaper_look_card.left + 100 * scale_
                      ? lay.wallpaper_look_card.right - 16 * scale_
                      : lay.wallpaper_look_value.left - 12 * scale_);
            button(lay.wallpaper_look_value, std::to_wstring(vm.wallpaper_visibility) + L"%", H::SettingsWallpaperLook,
                   0);
            const I blurs[] = {I::WallpaperBlurOff, I::WallpaperBlurLight, I::WallpaperBlurStrong};
            segmented(lay.wallpaper_blur_card, lay.wallpaper_blur_row, blurs, levels, vm.wallpaper_blur,
                      H::SettingsWallpaperBlur, I::SettingsWallpaperBlur, l10n::Get(I::SettingsWallpaperBlurDesc));
            dropdown(lay.language_card, lay.language_choice, I::SettingsLanguage, I::SettingsLanguageDesc,
                     l10n::Get(languages[vm.settings_language]), 1);
            const I text_render[] = {I::TextRenderAuto, I::TextRenderSharp, I::TextRenderSmooth};
            const int text_render_values[] = {0, 1, 2};
            const bool text_render_stacked = lay.text_render_row[0].left < lay.text_render_card.left + 100 * scale_;
            const float text_render_right = text_render_stacked ? lay.text_render_card.right - 16 * scale_
                                                                : lay.text_render_row[0].left - 12 * scale_;
            const float text_render_caption =
                SettingsCaptionHeight(l10n::Get(I::SettingsTextRenderDesc),
                                      text_render_right - lay.text_render_card.left - 54 * scale_, scale_, &painter_) /
                scale_;
            segmented(lay.text_render_card, lay.text_render_row, text_render, text_render_values,
                      vm.settings_text_render, H::SettingsTextRender, I::SettingsTextRender, l10n::Get(I::SettingsTextRenderDesc),
                      std::max(22.0f, text_render_caption));
        } else if (vm.settings_page == 6) {
            toggle(lay.blank_click_row, I::SettingsBlankClickBack, I::SettingsBlankClickBackDesc, L"\xE72B",
                   vm.settings_blank_click_go_back, 7);
            toggle(lay.hidden_files_row, I::SettingsShowHidden, I::SettingsShowHiddenDesc, L"\xE890",
                   vm.settings_show_hidden_files, 5);
            // Hidden + system entries: File Explorer keeps these behind a second option.
            toggle(lay.protected_files_row, I::SettingsShowProtected, I::SettingsShowProtectedDesc, L"\xE72E",
                   vm.settings_show_protected_os_files, 16);
            const I density[] = {I::SettingsDensityCompact, I::SettingsDensityStandard, I::SettingsDensityRoomy};
            const int heights[] = {28, 34, 40};
            segmented(lay.density_card, lay.density_row, density, heights, vm.settings_row_height, H::SettingsDensity,
                      I::SettingsRowHeight, l10n::Get(I::SettingsRowHeightDesc));
            toggle(lay.list_style_row[0], I::ListSmartDate, I::ListSmartDateDesc, L"\xE787",
                   vm.settings_list_smart_date, 17);
            toggle(lay.list_style_row[1], I::ListZebraRows, I::ListZebraRowsDesc, L"\xE8FD",
                   vm.settings_list_zebra_rows, 18);
            toggle(lay.list_style_row[2], I::ListSizeBar, I::ListSizeBarDesc, L"\xE9D2", vm.settings_list_size_bar, 19);
            const I folder_sort[] = {I::FolderSortTop, I::FolderSortFollow, I::FolderSortMixed};
            const int folder_sort_values[] = {0, 1, 2};
            segmented(lay.folder_sort_card, lay.folder_sort_row, folder_sort, folder_sort_values,
                      vm.settings_folder_sort, H::SettingsFolderSort, I::SettingsFolderSort, l10n::Get(I::SettingsFolderSortDesc));
            toggle(lay.auto_folder_size_row, I::AutoFolderSize, I::AutoFolderSizeDesc, L"\xE8B7",
                   vm.settings_auto_folder_size, 35);
            toggle(lay.performance_row, I::SettingsShowPerformance, I::SettingsShowPerformanceDesc, L"\xE946",
                   vm.settings_show_performance, 4);
        } else if (vm.settings_page == 7) {
            toggle(lay.new_tab_row, I::SettingsNewTabHome, I::SettingsNewTabHomeDesc, L"\xE80F",
                   vm.settings_new_tab_home, 20);

            const I external_modes[] = {I::ExternalOpenReuse, I::ExternalOpenTab, I::ExternalOpenWindow};
            dropdown(lay.external_open_card, lay.external_open_choice, I::SettingsExternalOpen,
                     I::SettingsExternalOpenDesc,
                     l10n::Get(external_modes[std::clamp(vm.settings_external_open_mode, 0, 2)]), 2);
            toggle(lay.pinned_names_row, I::PinnedNames, I::PinnedNamesDesc, L"\xE718", vm.show_pinned_tab_names, 6);
            toggle(lay.last_tab_row, I::CloseLastTabWindow, I::CloseLastTabWindowDesc, L"\xE8BB",
                   vm.settings_close_last_tab_window, 32);

            label(lay.wallpaper_fit_card, l10n::Get(I::SettingsMatchBackground),
                  l10n::Get(I::SettingsMatchBackgroundDesc), L"\xE740",
                  lay.wallpaper_fit.left < lay.wallpaper_fit_card.left + 100 * scale_
                      ? lay.wallpaper_fit_card.right - 16 * scale_
                      : lay.wallpaper_fit.left - 12 * scale_);
            const auto background_size = BackgroundImageSize(vm.background_image);
            button(lay.wallpaper_fit, l10n::Get(I::MatchWindowSize), H::SettingsWallpaper, 2, false,
                   !vm.settings_lock_window_size && background_size.cx > 0 && background_size.cy > 0);

            toggle(lay.window_size_lock_row, I::SettingsLockWindowSize, I::SettingsLockWindowSizeDesc, L"\xE72E",
                   vm.settings_lock_window_size, 34);
        } else if (vm.settings_page == 8) {
            toggle(lay.startup_row[2], I::SettingsDefaultManager, I::SettingsDefaultManagerDesc, L"\xE8B7",
                   vm.settings_open_folders && vm.settings_take_over_win_e && vm.settings_take_over_this_pc, 21);

            toggle(lay.explorer_takeover_row, I::SettingsExplorerTakeover, I::SettingsExplorerTakeoverDesc, L"\xE8B7",
                   vm.settings_explorer_takeover, 24);
            toggle(lay.file_dialog_bridge_row, I::SettingsFileDialogBridge, I::SettingsFileDialogBridgeDesc,
                   L"\xE8E5", vm.settings_file_dialog_bridge, 38);
            toggle(lay.file_dialog_takeover_row, I::SettingsFileDialogTakeover, I::SettingsFileDialogTakeoverDesc,
                   L"\xE8A7", vm.settings_file_dialog_takeover, 37);

            if (lay.system_status.bottom > lay.system_status.top) {
                const auto r = lay.system_status;
                DrawIconText(r.left + 16 * scale_, r.top + 12 * scale_, 24 * scale_, 24 * scale_, L"\xE946", L"",
                             theme.text_secondary, 0.85f);
                painter_.DrawWrappedCaption(
                    vm.settings_system_status,
                    D2D1::Point2F(r.left + 54 * scale_, r.top + kSettingsRowPaddingDip * scale_),
                    r.right - r.left - 70 * scale_, theme.text_secondary);
            }
            toggle(lay.startup_row[0], I::SettingsLaunch, I::SettingsLaunchDesc, L"\xE7E8",
                   vm.settings_launch_on_startup, 1);
            toggle(lay.startup_tray_row, I::StartToTray, I::StartToTrayDesc, L"\xE73F", vm.settings_start_to_tray, 31);
            toggle(lay.startup_row[1], I::SettingsKeepRunning, I::SettingsKeepRunningDesc, L"\xE737",
                   vm.settings_keep_running, 2);

            const auto config = lay.configuration_path;
            text(l10n::Get(I::ConfigurationLocation),
                 D2D1::RectF(config.left + 16 * scale_, config.top + 8 * scale_, config.right - 16 * scale_,
                             config.top + 32 * scale_));
            text(vm.settings_configuration_path,
                 D2D1::RectF(config.left + 16 * scale_, config.top + 34 * scale_, config.right - 16 * scale_,
                             config.top + 56 * scale_),
                 true);
            const auto status_bounds = D2D1::RectF(config.left + 16 * scale_, config.top + 60 * scale_,
                                                   config.right - 16 * scale_, config.top + 100 * scale_);
            dc->PushAxisAlignedClip(status_bounds, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
            painter_.DrawWrappedCaption(vm.settings_configuration_status.empty()
                                            ? l10n::Get(I::StorageRestartDescription)
                                            : vm.settings_configuration_status,
                                        D2D1::Point2F(status_bounds.left, status_bounds.top),
                                        status_bounds.right - status_bounds.left, theme.text_secondary);
            dc->PopAxisAlignedClip();
            button(lay.configuration_action[0], l10n::Get(I::OpenLocation), H::SettingsConfigurationAction, 0);
            button(lay.configuration_action[1], l10n::Get(I::ChangeLocation), H::SettingsConfigurationAction, 1, true,
                   !vm.settings_storage_pending);
            painter_.DrawWrappedCaption(vm.settings_transfer_status,
                D2D1::Point2F(lay.transfer_description.left, lay.transfer_description.top),
                lay.transfer_description.right - lay.transfer_description.left, theme.text_secondary);
            const l10n::StringId transfer_labels[] = {l10n::StringId::ExportSettingsTitle,
                l10n::StringId::ImportSettingsTitle, l10n::StringId::RestartNow};
            for (int i = 0; i < 2 + static_cast<int>(vm.settings_transfer_pending); ++i) {
                fluent::ControlState state{};
                state.enabled = !vm.settings_transfer_busy;
                state.hovered = state.enabled && IsHovered(vm, HitTestResult::SettingsDiagnosticsAction, i + 3);
                DrawSettingsButton(vm, {lay.transfer_action[i], l10n::Get(transfer_labels[i]) + (i < 2 ? L"…" : L""), {},
                    fluent::ButtonKind::Standard, state});
            }

            dc->PushAxisAlignedClip(lay.footer, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
            painter_.DrawWrappedCaption(l10n::Get(I::SettingsStorageFooter),
                                        D2D1::Point2F(lay.footer.left, lay.footer.top),
                                        lay.footer.right - lay.footer.left, theme.text_secondary);
            dc->PopAxisAlignedClip();
        } else if (vm.settings_page == 9) {
            toggle(lay.staging_tray_row, I::SettingsStagingTray, I::SettingsStagingTrayDesc, L"\xE7B8",
                   vm.staging_tray_enabled, 36);
            toggle(lay.text_compare_row, I::SettingsTextCompare, I::SettingsTextCompareDesc, L"\xE8A5",
                   vm.settings_text_compare, 33);
            toggle(lay.change_tracking_row, I::SettingsChangeTracking, I::SettingsChangeTrackingDesc, L"\xE823",
                   vm.settings_change_tracking, 8);
            const I days[] = {I::ChangeToday, I::ChangeLast3Days, I::ChangeLast7Days};
            const int day_values[] = {1, 3, 7};
            segmented(lay.change_days_row, lay.change_days, days, day_values, vm.settings_change_days,
                      H::SettingsChangeDays, I::SettingsChangeDays, L"");
        }
    } else {
        section(0,I::SearchModeName);section(1,I::SearchModeContent);
        for (auto row : {lay.global_search_row, lay.search_pinyin_row,
                lay.filename_status, lay.disclosure[1], lay.content_header, lay.content_types,
                lay.content_empty, lay.content_options, lay.content_rebuild}) {
            if (row.bottom <= row.top) continue;
            row.top += 2*scale_; row.bottom -= 2*scale_;
            draw_card(row);
        }
        toggle(lay.global_search_row,I::GlobalSearch,I::GlobalSearchDesc,L"\xE721",vm.settings_global_search_enabled,15);
        toggle(lay.search_pinyin_row,I::SearchPinyin,I::SearchPinyinDesc,L"\xE721",vm.settings_search_pinyin,9);
        label(lay.filename_status,l10n::Get(I::SettingsFilenameIndex),vm.settings_index_status,L"\xE8A5",lay.filename_status.right-16*scale_);
        disclosure(lay.disclosure[1],I::SettingsMaintenance,I::SettingsMaintenanceDesc,L"\xE713",1,false);
        label(lay.content_header,l10n::Get(I::SettingsContentFolders),vm.settings_content_summary,L"\xE8B7",
            lay.content_header.right-16*scale_);

        label(lay.content_types,l10n::Get(I::SettingsContentTypes),l10n::Get(I::SettingsContentTypesDesc),L"\xE8A5",
            lay.content_types.right-16*scale_,(lay.content_types.bottom-lay.content_types.top)/scale_-45);

        if(vm.settings_content_folders.empty()) {
            label(lay.content_empty,l10n::Get(I::ContentIndexEmpty),l10n::Get(I::SettingsContentEmptyHelp),L"\xE8B7",lay.content_empty.right-16*scale_);
        }
        if(!vm.settings_content_instant && !vm.settings_content_folders.empty()) button(lay.content_pause,l10n::Get(vm.settings_content_paused ? I::ContentIndexResume : I::ContentIndexPause),H::SettingsContentAction,1);
        label(lay.content_options,l10n::Get(I::SettingsContentOptions),l10n::Get(I::SettingsContentOptionsDesc),L"\xE8A5");
        DrawIconText(lay.content_options.right-38*scale_,lay.content_options.top+22*scale_,18*scale_,18*scale_,L"\xE76C",L"",theme.text_secondary,0.75f);
        text(l10n::Get(vm.settings_content_instant ? I::ContentInstantReady : I::SettingsContentHelp),lay.footer,true);
        if(!vm.settings_content_instant && !vm.settings_content_folders.empty()) {
        label(lay.content_rebuild,l10n::Get(I::ContentIndexRebuild),l10n::Get(I::SettingsContentRebuildHelp),L"\xE72C");
        DrawIconText(lay.content_rebuild.right-38*scale_,lay.content_rebuild.top+22*scale_,18*scale_,18*scale_,L"\xE76C",L"",theme.text_secondary,0.75f);
        }
    }
}
} // namespace pulse::ui
