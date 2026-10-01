#include "ui_renderer_internal.h"
#include "toolbar_layout.h"
#include "pane_header_icons.h"
#include "../app/resource.h"

namespace pulse::ui {
void MainRenderer::DrawToolbar(const WindowViewModel& vm, const D2D1_RECT_F& rect, const Theme& theme) {
    auto* dc = compositor_->Dc();
    const float left = EffectiveSidebarWidth(rect.right);
    const bool compact = rect.right-left < 600*scale_;
    const auto layout = MakeToolbarLayout(rect.right,scale_,title_bar_height_,margin_,NewButtonWidthPx(compact),left,vm.pane.filter_expand,vm.archive_view);
    const bool searchOverlay=vm.address_searching && rect.right-left<480*scale_;
    if (!searchOverlay) {
        const wchar_t* nav_glyphs[] = {kIconBack,kIconForward,kIconUp,kIconRefresh};
        const HitTestResult::Region nav_hits[] = {HitTestResult::NavBack,HitTestResult::NavForward,HitTestResult::NavUp,HitTestResult::NavRefresh};
        for (int i=0;i<4;++i) {
            const auto r=layout.navigation[i];
            if (r.right<=r.left) continue;
            const bool enabled=i==0 ? vm.can_go_back : i==1 ? vm.can_go_forward : true;
            DrawButton(r,theme,enabled && IsHovered(vm,nav_hits[i]) ? theme.fill_hover : kTransparent,
                nav_glyphs[i],L"",vm.dark ? HexColor(0xFFFFFF) : enabled ? theme.text_secondary : theme.text_disabled,true,true);
        }
        // Breadcrumb address bar: segments clickable, empty area -> edit mode.
        D2D1_RECT_F addrRc = AddressBarRect(rect.right);
        fluent::ControlState addrState{};
        addrState.focused = vm.address_editing && !vm.address_searching;
        addrState.hovered = !vm.address_editing && IsHovered(vm, HitTestResult::AddressBar);
        painter_.DrawTextFieldFrame(addrRc, addrState);
        dc->PushAxisAlignedClip(addrRc, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        if (!vm.address_editing || vm.address_searching) {
            std::vector<BreadcrumbPlaced> placed;
            BreadcrumbLayout(vm.pane, rect.right, placed);
            for (size_t i = 0; i < placed.size(); ++i) {
                const auto& seg = placed[i];
                if ((int)i == vm.breadcrumb_drop) {
                    // Drop target: accent 2px stroke (ui.md §5.2 rule 7).
                    dc->DrawRoundedRectangle(
                        D2D1::RoundedRect(seg.rc, theme.radius_control * scale_, theme.radius_control * scale_),
                        brAccent_.get(), 2.0f * scale_);
                } else if ((int)i == vm.breadcrumb_hover) {
                    MakeBrush(dc, theme.fill_hover, brFillHover_);
                    FillRoundedRect(dc, brFillHover_.get(), seg.rc.left, seg.rc.top,
                        seg.rc.right - seg.rc.left, seg.rc.bottom - seg.rc.top,
                        theme.radius_control * scale_);
                }
                if (i > 0) {
                    // Chevron separator.
                    float chX = seg.rc.left - 14.0f * scale_;
                    DrawIconText(chX, addrRc.top, 14.0f * scale_, addrRc.bottom - addrRc.top,
                        kIconChevronRight, L">", theme.text_secondary, 0.55f);
                }
                MakeBrush(dc, theme.text, brText_);
                // Width measured exactly; let the ink use the right padding as slack
                // so the trailing glyph is not shaved by the clip rect.
                DrawTextRect(dc, compositor_->AddressFormat(), brText_.get(), seg.text,
                    seg.rc.left + 8 * scale_, seg.rc.top, seg.rc.right - seg.rc.left - 8 * scale_,
                    seg.rc.bottom - seg.rc.top, D2D1_DRAW_TEXT_OPTIONS_CLIP, true);
            }
            if (placed.empty() && !vm.pane.path.empty()) {
                MakeBrush(dc, theme.text, brText_);
                DrawTextRect(dc, compositor_->AddressFormat(), brText_.get(), vm.pane.path,
                    addrRc.left + 12.0f * scale_, addrRc.top,
                    addrRc.right - addrRc.left - 18.0f * scale_,
                    addrRc.bottom - addrRc.top, D2D1_DRAW_TEXT_OPTIONS_CLIP, true);
            }
        }
        dc->PopAxisAlignedClip();
    }
    DrawAddressSearchChrome(vm, rect.right, theme);
    MakeBrush(dc,theme.stroke_divider,brStrokeDivider_);
    FillRect(dc,brStrokeDivider_.get(),left+margin_,title_bar_height_+44*scale_,rect.right-left-2*margin_,scale_);
    const std::wstring label=l10n::Get(l10n::StringId::New);
    fluent::ButtonSpec create;
    create.bounds=layout.create; create.text=compact ? std::wstring_view{} : std::wstring_view{label};
    create.glyph=kIconAdd; create.kind=fluent::ButtonKind::Transparent;
    create.bordered=false;
    create.icon_only=compact; create.drop_down=!compact;
    create.skip_glyph=true;
    create.state.enabled=!vm.archive_view;
    create.state.hovered=create.state.enabled && IsHovered(vm,HitTestResult::NewButton);
    painter_.DrawButton(create);
    const float icon_x=compact ? (layout.create.left+layout.create.right)*0.5f
                               : layout.create.left+18*scale_;
    const float icon_y=(layout.create.top+layout.create.bottom)*0.5f;
    const auto icon_bounds=[&](float cx,float cy) {
        const float radius=8*scale_;
        return D2D1::RectF(cx-radius,cy-radius,cx+radius,cy+radius);
    };
    const auto enabled_color=vm.dark ? HexColor(0x8CCBFF) : theme.accent;
    const auto& create_color=create.state.enabled ? enabled_color : theme.text_disabled;
    const int create_icon=vm.dark ? IDR_FILES_NEW_COLOR_DARK_SVG : IDR_FILES_NEW_COLOR_SVG;
    auto create_bounds=icon_bounds(icon_x,icon_y);
    // Move the complete 1px icon onto pixel centers, keeping its layers concentric.
    const float shift_x=std::floor(create_bounds.left)+0.5f-create_bounds.left;
    const float shift_y=std::floor(create_bounds.top)+0.5f-create_bounds.top;
    create_bounds.left+=shift_x; create_bounds.right+=shift_x;
    create_bounds.top+=shift_y; create_bounds.bottom+=shift_y;
    if (!DrawFluentSvg(create.state.enabled ? create_icon : IDR_FILES_NEW_SVG,
        create_bounds,1.0f,create.state.enabled ? nullptr : &create_color)) {
        const auto icon=create_bounds;
        DrawIconText(icon.left,icon.top,icon.right-icon.left,icon.bottom-icon.top,
            kIconAdd,L"",create_color,0.82f);
    }
    if (layout.commands[0].right > layout.commands[0].left) {
        MakeBrush(dc,theme.stroke_divider,brStrokeDivider_);
        FillRect(dc,brStrokeDivider_.get(),layout.create.right+6*scale_,
            layout.create.top+7*scale_,scale_,18*scale_);
    }
    const wchar_t* glyphs[]={kIconCut,kIconCopy,kIconPaste,kIconRename,kIconDelete,kIconSettings,kIconSplit,
        vm.details_visible ? kIconDetailsClose : kIconDetailsOpen,L""};
    const int files_icons[]={IDR_FILES_CUT_SVG,IDR_FILES_COPY_SVG,IDR_FILES_PASTE_SVG,
        IDR_FILES_RENAME_SVG,IDR_FILES_DELETE_SVG,IDR_FILES_PROPERTIES_SVG};
    const int color_icons[]={IDR_FILES_CUT_COLOR_SVG,IDR_FILES_COPY_COLOR_SVG,IDR_FILES_PASTE_COLOR_SVG,
        IDR_FILES_RENAME_COLOR_SVG,IDR_FILES_DELETE_COLOR_SVG,IDR_FILES_PROPERTIES_COLOR_SVG};
    const int dark_color_icons[]={IDR_FILES_CUT_COLOR_DARK_SVG,IDR_FILES_COPY_COLOR_DARK_SVG,
        IDR_FILES_PASTE_COLOR_DARK_SVG,IDR_FILES_RENAME_COLOR_DARK_SVG,
        IDR_FILES_DELETE_COLOR_DARK_SVG,IDR_FILES_PROPERTIES_COLOR_DARK_SVG};
    const HitTestResult::Region hits[]={HitTestResult::Cut,HitTestResult::Copy,HitTestResult::Paste,
        HitTestResult::Rename,HitTestResult::Delete,HitTestResult::Properties,
        HitTestResult::SplitButton,HitTestResult::DetailsToggle,HitTestResult::PaneColumnLayout};
    for(int i=0;i<9;++i) {
        if (layout.commands[i].right <= layout.commands[i].left) continue;
        const bool enabled=vm.archive_view ? i>=5 : i==2 || i>=6 || vm.pane.selected_count>0;
        const bool selected=(i==6 && vm.pane_slots.size()>1) || (i==7 && vm.details_visible) ||
            (i==8 && vm.pane.column_strip.enabled);
        DrawButton(layout.commands[i],theme,enabled && IsHovered(vm,hits[i]) ? theme.fill_hover :
            selected ? theme.fill_selected : kTransparent,
            i<=6 ? L"" : glyphs[i],L"",!enabled ? theme.text_disabled : selected ? theme.accent : theme.text_secondary,true,true,0.82f);
        if (i<6) {
            const auto& r=layout.commands[i];
            const float cx=(r.left+r.right)*0.5f, cy=(r.top+r.bottom)*0.5f;
            const D2D1_COLOR_F color=enabled ? enabled_color : theme.text_disabled;
            const int color_icon=vm.dark ? dark_color_icons[i] : color_icons[i];
            if (!DrawFluentSvg(enabled ? color_icon : files_icons[i],icon_bounds(cx,cy),
                1.0f,enabled ? nullptr : &color)) {
                const auto icon=icon_bounds(cx,cy);
                DrawIconText(icon.left,icon.top,icon.right-icon.left,icon.bottom-icon.top,
                    glyphs[i],L"",color,0.82f);
            }
        }
        if (i==6) DrawPaneHeaderIcon(layout.commands[i],PaneHeaderIcon::Split,selected ? theme.accent : theme.text_secondary);
        if (i==8) DrawPaneHeaderIcon(layout.commands[i],PaneHeaderIcon::Columns,selected ? theme.accent : theme.text_secondary);
    }
    if (vm.archive_view) {
        const auto extract_button = [&](const D2D1_RECT_F& bounds, HitTestResult::Region hit,
                                        const wchar_t* text, bool enabled) {
            if (bounds.right <= bounds.left) return;
            fluent::ButtonSpec button;
            button.bounds=bounds; button.text=text;
            button.kind=fluent::ButtonKind::Transparent; button.bordered=false;
            button.state.hovered=enabled && IsHovered(vm,hit);
            button.state.enabled=enabled;
            painter_.DrawButton(button);
        };
        const bool english=l10n::effective_language()==l10n::Language::EnUS;
        extract_button(layout.extract,HitTestResult::Extract,english ? L"Extract" : L"解压",vm.pane.selected_count>0);
        extract_button(layout.extract_all,HitTestResult::ExtractAll,english ? L"Extract all" : L"解压全部项目",!vm.pane.loading);
    }
    if (layout.overflow.right > layout.overflow.left) {
        DrawButton(layout.overflow,theme,IsHovered(vm,HitTestResult::ToolbarMore) ? theme.fill_hover : kTransparent,
            L"\xE712",L"",theme.text_secondary,true,true);
    }
    const auto command = [&](D2D1_RECT_F bounds, const wchar_t* glyph, int files_icon,
                             l10n::StringId label, HitTestResult::Region region, bool dropdown) {
        fluent::ButtonSpec button;
        button.bounds=bounds;
        button.glyph=glyph;
        button.icon_only=bounds.right-bounds.left < 60*scale_;
        button.text=button.icon_only ? std::wstring_view{} : std::wstring_view{l10n::Get(label)};
        button.kind=fluent::ButtonKind::Transparent;
        button.bordered=false;
        button.drop_down=dropdown && !button.icon_only;
        button.skip_glyph=true;
        button.state.hovered=IsHovered(vm,region);
        painter_.DrawButton(button);
        const float cx=button.icon_only ? (bounds.left+bounds.right)*0.5f : bounds.left+18*scale_;
        const float cy=(bounds.top+bounds.bottom)*0.5f;
        if (!DrawFluentSvg(files_icon,icon_bounds(cx,cy),1.0f,&theme.text)) {
            const auto icon=icon_bounds(cx,cy);
            DrawIconText(icon.left,icon.top,icon.right-icon.left,icon.bottom-icon.top,
                glyph,L"",theme.text_secondary,0.82f);
        }
    };
    MakeBrush(dc,theme.stroke_divider,brStrokeDivider_);
    FillRect(dc,brStrokeDivider_.get(),layout.sort.left-6*scale_,layout.sort.top+7*scale_,scale_,18*scale_);
    command(layout.sort,L"\xE8CB",IDR_FILES_SORT_SVG,l10n::StringId::ToolbarSort,HitTestResult::ToolbarSort,true);
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
