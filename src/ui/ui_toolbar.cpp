// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "ui_renderer_internal.h"
#include "toolbar_layout.h"
#include "toolbar_icons.h"
#include "address_search_layout.h"
#include "toolbar_command_state.h"
#include "pane_header_icons.h"
#include "command_icons.h"
#include "../common/resource_ids.h"

namespace pulse::ui {
void MainRenderer::DrawToolbar(const WindowViewModel& vm, const D2D1_RECT_F& rect, const Theme& theme) {
    DrawNavigationBar(vm, rect, theme);
    DrawCommandBar(vm, rect, theme);
}

void MainRenderer::DrawNavigationBar(const WindowViewModel& vm, const D2D1_RECT_F& rect, const Theme& theme) {
    auto* dc = compositor_->Dc();
    const float left = EffectiveSidebarWidth(rect.right);
    const bool compact = rect.right-left < 600*scale_;
    const auto layout = MakeToolbarLayout(rect.right,scale_,title_bar_height_,margin_,NewButtonWidthPx(compact),left,vm.pane.filter_expand,vm.archive_view,vm.pane.is_recycle,vm.toolbar_order);
    const float address_opacity = rect.right - left < 480 * scale_
        ? AddressSearchContentOpacity(1.0f - vm.address_search_animation) : 1.0f;
    if (address_opacity > 0.0f) {
        if (address_opacity < 1.0f) dc->PushLayer(D2D1::LayerParameters(rect, nullptr,
            D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::Matrix3x2F::Identity(), address_opacity), nullptr);
        const wchar_t* nav_glyphs[] = {kIconBack,kIconForward,kIconUp,kIconRefresh};
        const HitTestResult::Region nav_hits[] = {HitTestResult::NavBack,HitTestResult::NavForward,HitTestResult::NavUp,HitTestResult::NavRefresh};
        for (int i=0;i<4;++i) {
            const auto r=layout.navigation[i];
            if (r.right<=r.left) continue;
            const bool enabled=i==0 ? vm.can_go_back : i==1 ? vm.can_go_forward : true;
            DrawButton(r,theme,enabled && IsHovered(vm,nav_hits[i]) ? theme.fill_hover : kTransparent,
                nav_glyphs[i],L"",enabled ? (vm.dark && !IsHighContrast() ? HexColor(0xFFFFFF) : theme.text_secondary) : theme.text_disabled,true,true);
        }
        // Breadcrumb address bar: segments clickable, empty area -> edit mode.
        D2D1_RECT_F addrRc = AddressBarRect(rect.right);
        fluent::ControlState addrState{};
        addrState.hovered = !vm.address_editing && IsHovered(vm, HitTestResult::AddressBar);
        painter_.DrawTextFieldFrame(addrRc, addrState);
        const float path_progress = std::clamp(vm.address_path_animation, 0.0f, 1.0f);
        const float breadcrumb_opacity = AddressSearchContentOpacity(1.0f - path_progress);
        const float editor_opacity = AddressSearchContentOpacity(path_progress);
        if (editor_opacity > 0.0f) {
            dc->PushLayer(D2D1::LayerParameters(addrRc, nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                D2D1::Matrix3x2F::Identity(), editor_opacity), nullptr);
            addrState.focused = true;
            painter_.DrawTextFieldFrame(addrRc, addrState, true);
            if (!vm.address_editing || vm.address_searching) {
                MakeBrush(dc, theme.text, brText_);
                const float offset = 6.0f * scale_ * (1.0f - path_progress);
                DrawTextRect(dc, compositor_->AddressFormat(), brText_.get(), vm.address_path_closing_text,
                    addrRc.left + 10 * scale_ + offset, addrRc.top + 2 * scale_,
                    std::max(0.0f, addrRc.right - addrRc.left - 20 * scale_ - offset),
                    addrRc.bottom - addrRc.top - 4 * scale_, D2D1_DRAW_TEXT_OPTIONS_CLIP);
            }
            dc->PopLayer();
        }
        dc->PushAxisAlignedClip(addrRc, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        if (breadcrumb_opacity > 0.0f) {
            D2D1_MATRIX_3X2_F transform;
            dc->GetTransform(&transform);
            dc->PushLayer(D2D1::LayerParameters(addrRc, nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                D2D1::Matrix3x2F::Identity(), breadcrumb_opacity), nullptr);
            dc->SetTransform(D2D1::Matrix3x2F::Translation(-6.0f * scale_ * path_progress, 0) * transform);
            std::vector<BreadcrumbPlaced> placed;
            BreadcrumbLayout(vm.pane, rect.right, placed);
            for (size_t i = 0; i < placed.size(); ++i) {
                const auto& seg = placed[i];
                if ((int)i == vm.breadcrumb_drop) {
                    // Drop target: accent 2px stroke (ui.md §5.2 rule 7).
                    dc->DrawRoundedRectangle(
                        D2D1::RoundedRect(seg.rc, theme.radius_control * scale_, theme.radius_control * scale_),
                        brAccent_.get(), 2.0f * scale_);
                }
                auto hover_fill = theme.fill_hover;
                hover_fill.a *= ControlHoverOpacity(seg.rc, (int)i == vm.breadcrumb_hover);
                if ((int)i != vm.breadcrumb_drop && hover_fill.a > 0.0f) {
                    MakeBrush(dc, hover_fill, brFillHover_);
                    FillRoundedRect(dc, brFillHover_.get(), seg.rc.left, seg.rc.top,
                        seg.rc.right - seg.rc.left, seg.rc.bottom - seg.rc.top,
                        theme.radius_control * scale_);
                }
            }
            dc->SetTransform(transform);
            dc->PopLayer();
            const std::wstring path_key = vm.pane.is_home ? L"pulse:home" :
                vm.pane.is_query_search ? L"pulse:search" : vm.pane.path;
            address_path_surface_.Paint(dc, addrRc, path_key, true, settings_switch_now_,
                motion_enabled_ && !IsHighContrast() && !vm.address_editing &&
                    path_progress == 0.0f && address_opacity == 1.0f,
                8 * scale_, 180, [&] {
                    dc->PushLayer(D2D1::LayerParameters(addrRc, nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                        D2D1::Matrix3x2F::Identity(), breadcrumb_opacity), nullptr);
                    dc->SetTransform(D2D1::Matrix3x2F::Translation(-6.0f * scale_ * path_progress, 0) * transform);
                    for (size_t i = 0; i < placed.size(); ++i) {
                        const auto& seg = placed[i];
                        if (i > 0) {
                            const float chX = seg.rc.left - 14.0f * scale_;
                            DrawIconText(chX, addrRc.top, 14.0f * scale_, addrRc.bottom - addrRc.top,
                                kIconChevronRight, L">", theme.text_secondary, 0.55f);
                        }
                        MakeBrush(dc, theme.text, brText_);
                        // Keep the right padding available for the final glyph.
                        DrawTextRect(dc, compositor_->AddressFormat(), brText_.get(), seg.text,
                            seg.rc.left + 8 * scale_, seg.rc.top, seg.rc.right - seg.rc.left - 8 * scale_,
                            seg.rc.bottom - seg.rc.top, D2D1_DRAW_TEXT_OPTIONS_CLIP);
                    }
                    if (placed.empty() && !vm.pane.path.empty()) {
                        MakeBrush(dc, theme.text, brText_);
                        DrawTextRect(dc, compositor_->AddressFormat(), brText_.get(), vm.pane.path,
                            addrRc.left + 12.0f * scale_, addrRc.top,
                            addrRc.right - addrRc.left - 18.0f * scale_,
                            addrRc.bottom - addrRc.top, D2D1_DRAW_TEXT_OPTIONS_CLIP);
                    }
                    dc->SetTransform(transform);
                    dc->PopLayer();
            }, {}, false, vm.pane.navigation_motion, vm.pane.navigation_owner, vm.pane.navigation_revision);
            surface_animating_ |= address_path_surface_.pending;
        } else address_path_surface_.Reset();
        dc->PopAxisAlignedClip();
        if (address_opacity < 1.0f) dc->PopLayer();
    } else address_path_surface_.Reset();
    DrawAddressSearchChrome(vm, rect.right, theme);
    const auto operation_color = IsHighContrast() &&
        (IsHovered(vm, HitTestResult::OperationCenter) || vm.operation_center_open)
        ? theme.text_selected : vm.operation_count > 0 || vm.operation_center_open
        ? theme.accent_ink : theme.text_secondary;
    DrawButton(layout.operations,theme,
        IsHovered(vm,HitTestResult::OperationCenter) ? theme.fill_hover :
        vm.operation_center_open ? theme.fill_selected : kTransparent,
        L"",L"",operation_color,true,true);
    const float operation_x = (layout.operations.left+layout.operations.right)*0.5f;
    const float operation_y = (layout.operations.top+layout.operations.bottom)*0.5f;
    DrawFluentSvg(IDR_FILES_STATUS_CENTER_SVG,
        D2D1::RectF(operation_x-8*scale_,operation_y-8*scale_,
                    operation_x+8*scale_,operation_y+8*scale_),1.0f,&operation_color);
    if (vm.operation_count > 0) {
        const auto count = vm.operation_count > 9 ? L"9+" : std::to_wstring(vm.operation_count);
        const float badge_width = std::max(19*scale_,
            typography::MeasureLine(compositor_, compositor_->SmallFormat(), count) + 4*scale_);
        const auto badge = D2D1::RectF(layout.operations.right+2*scale_-badge_width,
            layout.operations.top,layout.operations.right+2*scale_,layout.operations.top+14*scale_);
        MakeBrush(dc,theme.accent,brAccent_);
        FillRoundedRect(dc,brAccent_.get(),badge.left,badge.top,badge.right-badge.left,
            badge.bottom-badge.top,7*scale_);
        painter_.DrawText(count,badge,compositor_->SmallFormat(),theme.accent_text,
            fluent::HorizontalAlignment::Center,theme.accent);
    }
    if (vm.operation_failed) {
        ComPtr<ID2D1SolidColorBrush> badge;
        if (SUCCEEDED(dc->CreateSolidColorBrush(theme.danger,&badge)))
            dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(layout.operations.right-5*scale_,
                vm.operation_count > 0 ? layout.operations.bottom-6*scale_
                                       : layout.operations.top+6*scale_),3*scale_,3*scale_),badge.get());
    }
}

void MainRenderer::DrawCommandBar(const WindowViewModel& vm, const D2D1_RECT_F& rect, const Theme& theme) {
    if (vm.settings_open) return;
    auto* dc = compositor_->Dc();
    const float left = EffectiveSidebarWidth(rect.right);
    const bool compact = rect.right-left < 600*scale_;
    const auto layout = MakeToolbarLayout(rect.right,scale_,title_bar_height_,margin_,NewButtonWidthPx(compact),left,vm.pane.filter_expand,vm.archive_view,vm.pane.is_recycle,vm.toolbar_order);
    MakeBrush(dc,theme.stroke_divider,brStrokeDivider_);
    FillRect(dc,brStrokeDivider_.get(),left+margin_,title_bar_height_+44*scale_,rect.right-left-2*margin_,scale_);
    constexpr float icon_slot_dip = 18.0f;
    const auto icon_bounds=[&](float cx,float cy) {
        const float radius = icon_slot_dip * scale_ * 0.5f;
        return D2D1::RectF(cx-radius,cy-radius,cx+radius,cy+radius);
    };
    const bool high_contrast = IsHighContrast();
    const auto draw_command_icon = [&](int command, command_icons::Icon icon, D2D1_RECT_F bounds,
                                       bool enabled, D2D1_COLOR_F color) {
        if (toolbar_icons::DrawArtwork(painter_, command, bounds, scale_, vm.dark,
            high_contrast || !enabled, color, icon_slot_dip)) return;
        if (!paneHeaderStroke_.get()) command_icons::CreateStrokeStyle(dc, &paneHeaderStroke_);
        MakeBrush(dc, color, brText_);
        toolbar_icons::Draw(painter_, dc, brText_.get(), paneHeaderStroke_.get(), icon, bounds, scale_, icon_slot_dip);
    };
    if (layout.create.right > layout.create.left) {
        const std::wstring label=l10n::Get(l10n::StringId::New);
        fluent::ButtonSpec create;
        create.bounds=layout.create; create.text=compact ? std::wstring_view{} : std::wstring_view{label};
        create.glyph=kIconAdd; create.kind=fluent::ButtonKind::Transparent;
        create.bordered=false;
        create.icon_only=compact; create.drop_down=!compact;
        create.skip_glyph=true;
        create.state.enabled=!vm.archive_view && (vm.toolbar_commands.empty() ||
            std::any_of(vm.toolbar_commands.begin(), vm.toolbar_commands.end(), [](const auto& command) {
                return command.command == 186 && command.enabled;
            }));
        create.state.hovered=create.state.enabled && IsHovered(vm,HitTestResult::NewButton);
        painter_.DrawButton(create);
        const float icon_x=compact ? (layout.create.left+layout.create.right)*0.5f
                                   : layout.create.left+18*scale_;
        const float icon_y=(layout.create.top+layout.create.bottom)*0.5f;
        const auto create_color=!create.state.enabled ? theme.text_disabled
            : IsHighContrast() && create.state.hovered ? theme.text_selected : theme.text;
        draw_command_icon(186,command_icons::Icon::Add,icon_bounds(icon_x,icon_y),create.state.enabled,create_color);
    }
    if (layout.create.right > layout.create.left && layout.commands[0].right > layout.create.right) {
        MakeBrush(dc,theme.stroke_divider,brStrokeDivider_);
        FillRect(dc,brStrokeDivider_.get(),layout.create.right+6*scale_,
            layout.create.top+7*scale_,scale_,18*scale_);
    }
    using Icon = command_icons::Icon;
    const Icon icons[]={Icon::Cut,Icon::Copy,Icon::Paste,Icon::Rename,Icon::Delete,Icon::Info,
        Icon::Split,vm.details_visible ? Icon::PanelClose : Icon::Panel,Icon::Columns};
    const HitTestResult::Region hits[]={HitTestResult::Cut,HitTestResult::Copy,HitTestResult::Paste,
        HitTestResult::Rename,HitTestResult::Delete,HitTestResult::Properties,
        HitTestResult::SplitButton,HitTestResult::DetailsToggle,HitTestResult::PaneColumnLayout};
    const ToolbarCommandState command_state{vm.archive_view, vm.pane.selected_count,
        vm.selection_has_drive_root, vm.can_paste};
    for(int i=0;i<9;++i) {
        if (layout.commands[i].right <= layout.commands[i].left) continue;
        const bool enabled = i < 6 && vm.toolbar_commands_enabled
            ? (*vm.toolbar_commands_enabled)[i] : command_state.Enabled(i);
        const bool selected=(i==6 && vm.pane_slots.size()>1) || (i==7 && vm.details_visible) ||
            (i==8 && vm.pane.column_strip.enabled);
        DrawButton(layout.commands[i],theme,enabled && IsHovered(vm,hits[i]) ? theme.fill_hover :
            selected ? theme.fill_selected : kTransparent,L"",L"",theme.text,true,true);
        const auto foreground = !enabled ? theme.text_disabled
            : IsHighContrast() && (selected || IsHovered(vm,hits[i])) ? theme.text_selected
            : selected ? theme.accent_ink : theme.text;
        draw_command_icon(kLegacyToolbarCommands[i],icons[i],layout.commands[i],enabled,foreground);
    }
    for (const auto& placed : layout.placed_commands) {
        if (placed.command == 186 || std::find(kLegacyToolbarCommands.begin(), kLegacyToolbarCommands.end(),
            placed.command) != kLegacyToolbarCommands.end()) continue;
        const auto item = std::find_if(vm.toolbar_commands.begin(), vm.toolbar_commands.end(),
            [&](const auto& command) { return command.command == placed.command; });
        if (item == vm.toolbar_commands.end()) continue;
        const bool hovered = IsHovered(vm, HitTestResult::CustomToolbarCommand, placed.command);
        fluent::ButtonSpec button;
        button.bounds = placed.bounds; button.glyph = item->glyph;
        button.icon_only = true; button.kind = fluent::ButtonKind::Transparent;
        button.bordered = false;
        button.skip_glyph = true;
        button.state.enabled = item->enabled; button.state.hovered = hovered;
        if (item->selected) DrawButton(placed.bounds, theme, theme.fill_selected, L"", L"",
            theme.text, true, true);
        painter_.DrawButton(button);
        draw_command_icon(placed.command,toolbar_icons::Resolve(item->glyph,item->pictogram,item->command),placed.bounds,item->enabled,
            !item->enabled ? theme.text_disabled : IsHighContrast() && (hovered || item->selected)
                ? theme.text_selected : item->selected ? theme.accent_ink : theme.text);
    }
    const auto contextual_button = [&](const D2D1_RECT_F& bounds, HitTestResult::Region hit,
                                       const wchar_t* text, bool enabled) {
        if (bounds.right <= bounds.left) return;
        fluent::ButtonSpec button;
        button.bounds=bounds; button.text=text;
        button.kind=fluent::ButtonKind::Transparent; button.bordered=false;
        button.state.hovered=enabled && IsHovered(vm,hit);
        button.state.enabled=enabled;
        painter_.DrawButton(button);
    };
    if (vm.archive_view) {
        contextual_button(layout.extract,HitTestResult::Extract,l10n::Get(l10n::StringId::ArchiveExtract).c_str(),vm.pane.selected_count>0);
        contextual_button(layout.extract_all,HitTestResult::ExtractAll,l10n::Get(l10n::StringId::ArchiveExtractAll).c_str(),!vm.pane.loading);
    }
    if (vm.pane.is_recycle) {
        contextual_button(layout.empty_recycle,HitTestResult::EmptyRecycle,
            l10n::Get(l10n::StringId::EmptyRecycleBin).c_str(),vm.can_empty_recycle);
    }
    if (layout.has_overflow && layout.overflow.right > layout.overflow.left) {
        const bool hovered = IsHovered(vm, HitTestResult::ToolbarMore);
        DrawButton(layout.overflow, theme, hovered ? theme.fill_hover : kTransparent, L"", L"", theme.text, true, true);
        draw_command_icon(0, Icon::More, layout.overflow, true, high_contrast && hovered ? theme.text_selected : theme.text);
    }
    const auto command = [&](D2D1_RECT_F bounds, const wchar_t* glyph, int files_icon,
                             l10n::StringId label, HitTestResult::Region region, bool dropdown, bool enabled = true) {
        fluent::ButtonSpec button;
        button.bounds=bounds;
        button.glyph=glyph;
        button.icon_only=bounds.right-bounds.left < 60*scale_;
        button.text=button.icon_only ? std::wstring_view{} : std::wstring_view{l10n::Get(label)};
        button.kind=fluent::ButtonKind::Transparent;
        button.bordered=false;
        button.drop_down=dropdown && !button.icon_only;
        button.skip_glyph=true;
        button.state.hovered=enabled && IsHovered(vm,region);
        button.state.enabled=enabled;
        painter_.DrawButton(button);
        const float cx=button.icon_only ? (bounds.left+bounds.right)*0.5f : bounds.left+18*scale_;
        const float cy=(bounds.top+bounds.bottom)*0.5f;
        const auto foreground=!enabled ? theme.text_disabled
            : IsHighContrast() && button.state.hovered ? theme.text_selected : theme.text;
        auto icon=icon_bounds(cx,cy);
        if (files_icon == IDR_TOOLBAR_CUSTOMIZE_SVG) {
            // Keep the 16 DIP artwork's one-pixel horizontal strokes aligned at 100%.
            const float side = 16.0f * scale_;
            const float left = std::round(cx - side * 0.5f);
            const float top = std::round(cy - side * 0.5f);
            icon = D2D1::RectF(left, top, left + side, top + side);
        }
        if (!DrawFluentSvg(files_icon,icon,1.0f,&foreground)) {
            DrawIconText(icon.left,icon.top,icon.right-icon.left,icon.bottom-icon.top,
                glyph,L"",foreground,0.82f);
        }
    };
    if (!layout.has_overflow && layout.overflow.right > layout.overflow.left) {
        command(layout.overflow,L"\xE9E9",IDR_TOOLBAR_CUSTOMIZE_SVG,l10n::StringId::CustomizeCommands,HitTestResult::ToolbarMore,false);
    }
    MakeBrush(dc,theme.stroke_divider,brStrokeDivider_);
    FillRect(dc,brStrokeDivider_.get(),layout.sort.left-4*scale_,layout.sort.top+7*scale_,scale_,18*scale_);
    command(layout.sort,L"\xE8CB",IDR_FILES_SORT_SVG,l10n::StringId::ToolbarSort,HitTestResult::ToolbarSort,true);
    command(layout.group,L"\xE8FD",IDR_FILES_GROUP_SVG,l10n::StringId::ToolbarGroup,HitTestResult::ToolbarGroup,true,vm.can_group);
    if (vm.pane.filter_expand <= 0.015f && !vm.filter_editing) {
        command(layout.filter,kIconFilter,IDR_FILES_FILTER_SVG,l10n::StringId::ToolbarFilter,HitTestResult::FilterBox,false);
    } else {
        fluent::TextFieldSpec field;
        field.bounds=layout.filter;
        field.placeholder=l10n::Get(l10n::StringId::FilterPlaceholder);
        field.text=vm.pane.filter_text;
        field.leading_glyph=kIconFilter;
        field.compact_leading_glyph=true;
        field.suppress_leading_glyph=true;
        field.suppress_text=vm.filter_editing;
        field.state.focused=vm.filter_editing;
        field.state.hovered=IsHovered(vm,HitTestResult::FilterBox);
        if (!vm.pane.filter_text.empty()) field.trailing_width=30;
        painter_.DrawTextField(field);
        const float cx=layout.filter.left+19*scale_;
        const float cy=(layout.filter.top+layout.filter.bottom)*0.5f;
        if (!DrawFluentSvg(IDR_FILES_FILTER_SVG,icon_bounds(cx,cy),1.0f,&theme.text_secondary))
            DrawIconText(layout.filter.left,layout.filter.top,30*scale_,
                layout.filter.bottom-layout.filter.top,kIconFilter,L"",theme.text_secondary,0.82f);
        if (!vm.pane.filter_text.empty() && vm.pane.filter_expand >= 0.985f) {
            DrawButton(FilterClearRect(rect,vm.pane.filter_expand),theme,
                IsHovered(vm,HitTestResult::FilterClear) ? theme.fill_hover : kTransparent,
                L"\xE711",L"",theme.text_secondary,true,true,0.65f);
        }
    }
}
}
