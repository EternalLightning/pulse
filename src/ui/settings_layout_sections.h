// Included inside ui_renderer_internal.h's anonymous namespace.
constexpr float kSettingsRowMinDip = 64.0f;
constexpr float kSettingsRowPaddingDip = 10.0f;
constexpr float kSettingsTextGapDip = 2.0f;

struct SettingsToggleGeometry {
    D2D1_RECT_F title{}, description{}, control{}, icon{};
};
float SettingsTitleHeight(float scale, const fluent::Painter* painter) {
    return std::max(24.0f * scale, painter ? painter->MeasureButtonHeight() - 8.0f * scale : 0.0f);
}
float SettingsCaptionHeight(std::wstring_view description, float width, float scale,
                           const fluent::Painter* painter) {
    return description.empty() ? 0.0f : std::max(18.0f * scale,
        painter ? painter->MeasureWrappedCaptionHeight(description, std::max(1.0f, width)) : 18.0f * scale);
}
float SettingsToggleHeight(float width, std::wstring_view description, float scale,
                           const fluent::Painter* painter, float control_reserve_dip = 72.0f) {
    const float caption = SettingsCaptionHeight(description, width - (54.0f + control_reserve_dip) * scale, scale, painter);
    const float text = SettingsTitleHeight(scale, painter) + (caption > 0 ? kSettingsTextGapDip * scale + caption : 0);
    return std::max(kSettingsRowMinDip * scale, text + 2.0f * kSettingsRowPaddingDip * scale);
}
SettingsToggleGeometry SettingsToggleBounds(const D2D1_RECT_F& row, std::wstring_view description,
                                           float scale, const fluent::Painter* painter,
                                           float control_reserve_dip = 72.0f) {
    SettingsToggleGeometry result;
    const float left = row.left + 54.0f * scale, right = row.right - control_reserve_dip * scale;
    const float title = SettingsTitleHeight(scale, painter);
    const float caption = SettingsCaptionHeight(description, right - left, scale, painter);
    const float text = title + (caption > 0 ? kSettingsTextGapDip * scale + caption : 0);
    const float top = row.top + std::max(kSettingsRowPaddingDip * scale, (row.bottom - row.top - text) * 0.5f);
    result.title = D2D1::RectF(left, top, right, top + title);
    result.description = D2D1::RectF(left, result.title.bottom + kSettingsTextGapDip * scale,
        right, result.title.bottom + kSettingsTextGapDip * scale + caption);
    const float center = (row.top + row.bottom) * 0.5f;
    const float switch_right = row.right - (control_reserve_dip - 56.0f) * scale;
    result.control = D2D1::RectF(switch_right - 44.0f * scale, center - 16.0f * scale,
        switch_right, center + 16.0f * scale);
    result.icon = D2D1::RectF(row.left + 16.0f * scale, center - 12.0f * scale,
        row.left + 40.0f * scale, center + 12.0f * scale);
    return result;
}
float LayoutSettingsActions(float left, float right, float top, float scale,
                            const std::wstring_view* labels, int count, D2D1_RECT_F* output,
                            const fluent::Painter* painter, bool wrap_text = false) {
    const float gap = 8 * scale;
    std::vector<float> widths;
    std::vector<float> heights;
    for (int i = 0; i < count; ++i) {
        const float width = std::min(right - left, std::max(96 * scale,
            painter ? painter->MeasureButtonWidth(labels[i]) : 120 * scale));
        widths.push_back(width);
        heights.push_back(wrap_text && painter
            ? painter->MeasureWrappedButtonHeight(labels[i], width) : 32 * scale);
    }
    for (int first = 0; first < count;) {
        int end = first + 1;
        float width = widths[first];
        while (end < count && width + gap + widths[end] <= right - left) {
            width += gap + widths[end++];
        }
        float row_height = 32 * scale;
        for (int i = first; i < end; ++i) row_height = std::max(row_height, heights[i]);
        float x = right - width;
        for (int i = first; i < end; ++i) {
            output[i] = D2D1::RectF(x, top, x + widths[i], top + row_height);
            x += widths[i] + gap;
        }
        first = end;
        top += row_height + gap;
    }
    return top;
}

float LayoutSettingsActions(float left, float right, float top, float scale,
                            const l10n::StringId* labels, int count, D2D1_RECT_F* output,
                            const fluent::Painter* painter) {
    std::vector<std::wstring_view> text;
    for (int i = 0; i < count; ++i) text.push_back(l10n::Get(labels[i]));
    return LayoutSettingsActions(left, right, top, scale, text.data(), count, output, painter);
}

float LayoutSettingsDuplicates(SettingsLayout& l, const WindowViewModel& vm, float scale,
                               float y, const fluent::Painter* painter) {
    using I = l10n::StringId;
    const float card_left = l.content.left + 20 * scale;
    const float card_right = l.content.right - 20 * scale;
    const float left = card_left + 16 * scale, right = card_right - 16 * scale;
    const float available = std::max(1.0f, right - left);
    const float text_left = card_left + 54 * scale;
    const auto button_width = [&](std::wstring_view text) {
        return painter ? painter->MeasureButtonWidth(text) : 120 * scale;
    };
    const auto caption_height = [&](std::wstring_view text, float width) {
        return text.empty() ? 0.0f : std::max(18 * scale, painter
            ? painter->MeasureWrappedCaptionHeight(text, width) : 36 * scale);
    };
    const auto button_height = [&](std::wstring_view text, float width) {
        return painter ? painter->MeasureWrappedButtonHeight(text, width) : 32 * scale;
    };
    y += 18 * scale;
    const float options_top = y;
    const I scopes[] = {I::DupScopeFolder, I::DupScopeDrive, I::DupScopeAll};
    float scope_item_width = 80 * scale;
    for (const I id : scopes)
        scope_item_width = std::max(scope_item_width, button_width(l10n::Get(id)) + 6 * scale);
    const float scope_width = std::max(300 * scale, scope_item_width * 3);
    l.dup_scope_stacked = scope_width > available;
    const bool scope_below = card_right - card_left < 560 * scale ||
        available - scope_width < 180 * scale;
    const float scope_label_right = scope_below ? right : right - scope_width - 12 * scale;
    l.dup_scope_label = D2D1::RectF(text_left, y + 10 * scale, scope_label_right, y + 34 * scale);
    const float hint_height = caption_height(vm.dup_hint, scope_label_right - text_left);
    l.dup_hint = D2D1::RectF(text_left, y + 35 * scale, scope_label_right,
        y + 35 * scale + hint_height);
    const float scope_top = scope_below ? l.dup_hint.bottom + 12 * scale : y + 16 * scale;
    for (int i = 0; i < 3; ++i) {
        const float width = scope_width / 3;
        const float x = l.dup_scope_stacked ? left : right - scope_width + i * width;
        const float top = scope_top + (l.dup_scope_stacked ? i * 40 * scale : 0);
        l.dup_scope[i] = D2D1::RectF(x, top,
            l.dup_scope_stacked ? right : x + width, top + 32 * scale);
    }
    y = std::max(l.dup_hint.bottom, l.dup_scope[2].bottom) + 12 * scale;
    l.dup_scope_row = D2D1::RectF(card_left, options_top, card_right, y);

    if (vm.dup_scope == 0 || vm.dup_scope == 1) {
        const float target_top = y;
        l.dup_target_label = D2D1::RectF(text_left, y + 10 * scale, right, y + 34 * scale);
        if (vm.dup_scope == 0) {
            const auto& label = l10n::Get(I::DupBrowse);
            const float width = std::min(available, std::max(96 * scale, button_width(label)));
            const bool below = card_right - card_left < 560 * scale;
            const float top = y + (below ? 66 : 16) * scale;
            l.dup_browse = D2D1::RectF(right - width, top, right, top + button_height(label, width));
            l.dup_target_label.right = below ? right : l.dup_browse.left - 12 * scale;
            l.dup_target_summary = D2D1::RectF(below ? left : text_left, y + 35 * scale,
                below ? right : l.dup_browse.left - 12 * scale, y + 56 * scale);
            y = std::max(y + 64 * scale, l.dup_browse.bottom + 12 * scale);
        } else {
            const bool below = card_right - card_left < 560 * scale;
            const float choices_left = below ? left : left + 220 * scale;
            if (!below) l.dup_target_label.right = choices_left - 12 * scale;
            std::vector<std::wstring_view> labels;
            for (const auto& drive : vm.dup_drives) labels.push_back(drive.label);
            l.dup_drives.resize(labels.size());
            const float bottom = LayoutSettingsActions(choices_left, right,
                y + (below ? 48 : 16) * scale, scale, labels.data(),
                static_cast<int>(labels.size()), l.dup_drives.data(), painter, true);
            y = std::max(y + 64 * scale, bottom + 4 * scale);
        }
        l.dup_target_row = D2D1::RectF(card_left, target_top, card_right, y);
    }

    const float min_top = y;
    const bool min_below = card_right - card_left < 560 * scale;
    const float min_width = std::min(300 * scale, available);
    const float min_label_right = min_below ? right : right - min_width - 12 * scale;
    l.dup_min_label = D2D1::RectF(text_left, y + 10 * scale, min_label_right, y + 34 * scale);
    const float desc_height = caption_height(l10n::Get(I::DupMinSizeDesc), min_label_right - text_left);
    l.dup_min_description = D2D1::RectF(text_left, y + 35 * scale, min_label_right,
        y + 35 * scale + desc_height);
    const float min_choice_top = min_below ? l.dup_min_description.bottom + 12 * scale : y + 16 * scale;
    for (int i = 0; i < 3; ++i)
        l.dup_min_size[i] = D2D1::RectF(right - min_width + i * min_width / 3, min_choice_top,
            right - min_width + (i + 1) * min_width / 3, min_choice_top + 32 * scale);
    y = std::max(l.dup_min_description.bottom, l.dup_min_size[2].bottom) + 12 * scale;
    l.dup_min_row = D2D1::RectF(card_left, min_top, card_right, y);

    const float actions_top = y;
    const std::wstring_view actions[] = {l10n::Get(I::Cancel), l10n::Get(I::DupScan)};
    D2D1_RECT_F action_rects[2]{};
    y = LayoutSettingsActions(left, right, y + 12 * scale, scale, actions, 2, action_rects, painter, true) + 4 * scale;
    l.dup_cancel = action_rects[0]; l.dup_scan = action_rects[1];
    l.dup_actions_row = D2D1::RectF(card_left, actions_top, card_right, y);
    l.duplicate_options = D2D1::RectF(card_left, options_top, card_right, y);
    y += 24 * scale;
    if (vm.dup_show_progress) {
        l.dup_progress = D2D1::RectF(card_left, y, card_right, y + 72 * scale);
        y += 84 * scale;
    }
    if (!vm.dup_empty.empty()) {
        l.dup_empty = D2D1::RectF(card_left, y, card_right,
            y + caption_height(vm.dup_empty, card_right - card_left));
        y = l.dup_empty.bottom + 12 * scale;
    }

    const bool files_below = available < 300 * scale;
    const float file_height = (files_below ? 72.0f : 40.0f) * scale;
    const float keep_width = std::min(available,
        std::max(88 * scale, button_width(l10n::Get(I::DupKeep)) + 16 * scale));
    const auto& delete_text = l10n::Get(I::DupDeleteExtras);
    const float delete_width = std::min(available, std::max(96 * scale, button_width(delete_text)));
    for (size_t g = 0; g < vm.dup_groups.size(); ++g) {
        const float files_top = y + 48 * scale;
        const float delete_top = files_top + static_cast<float>(vm.dup_groups[g].files.size()) * file_height + 8 * scale;
        const auto delete_rect = D2D1::RectF(right - delete_width, delete_top, right,
            delete_top + button_height(delete_text, delete_width));
        const auto card = D2D1::RectF(card_left, y, card_right, delete_rect.bottom + 12 * scale);
        l.dup_group_cards.push_back(card);
        l.dup_group_delete.push_back(delete_rect);
        if (VisibleInContent(card, l.content, 64 * scale)) {
            for (size_t f = 0; f < vm.dup_groups[g].files.size(); ++f) {
                const float top = files_top + static_cast<float>(f) * file_height;
                l.dup_keep.push_back(D2D1::RectF(right - keep_width,
                    top + (files_below ? 36 : 4) * scale, right,
                    top + (files_below ? 68 : 36) * scale));
                l.dup_open.push_back(D2D1::RectF(left, top,
                    files_below ? right : right - keep_width - 12 * scale, top + 36 * scale));
                l.dup_keep_group.push_back(static_cast<int>(g));
                l.dup_keep_file.push_back(static_cast<int>(f));
                l.dup_open_group.push_back(static_cast<int>(g));
                l.dup_open_file.push_back(static_cast<int>(f));
            }
        }
        y = card.bottom + 12 * scale;
    }
    if (vm.dup_show_delete_all) {
        const std::wstring_view label = vm.dup_delete_all.empty()
            ? std::wstring_view(l10n::Get(I::DupDeleteAllExtras)) : std::wstring_view(vm.dup_delete_all);
        y = LayoutSettingsActions(left, right, y, scale, &label, 1, &l.dup_delete_all, painter, true) + 8 * scale;
    }
    return y;
}

float LayoutSettingsGeneral(SettingsLayout& l, const WindowViewModel& vm, float scale,
                            float y, const fluent::Painter* painter) {
    const float left = l.content.left + 20*scale, right = l.content.right - 20*scale;
    const bool narrow = right - left < 560*scale;
    auto row = [&](float h) { auto r = D2D1::RectF(left, y, right, y+h*scale); y=r.bottom; return r; };
    auto toggle_row = [&](l10n::StringId description) {
        return row(SettingsToggleHeight(right - left, l10n::Get(description), scale, painter) / scale);
    };
    auto section = [&](int i) { y+=24*scale; l.section[i]=row(28); };
    auto choice = [&](D2D1_RECT_F r, float width) {
        return D2D1::RectF(narrow ? r.left+16*scale : r.right-(width+16)*scale,
            r.bottom-44*scale, r.right-16*scale, r.bottom-12*scale);
    };
    auto segments = [&](D2D1_RECT_F r, D2D1_RECT_F* output, int count, float width) {
        auto c=choice(r,width); const float w=(c.right-c.left)/count;
        for(int i=0;i<count;++i) output[i]=D2D1::RectF(c.left+i*w,c.top,c.left+(i+1)*w,c.bottom);
    };
    section(0);
    l.theme_row=row(narrow ? 142.0f : 112.0f);
    const float tw=(std::min)(96*scale,(right-left-32*scale)/3);
    const float tile_left=narrow ? left+16*scale : right-16*scale-3*tw;
    for(int i=0;i<3;++i) l.theme_tile[i]=D2D1::RectF(tile_left+i*tw+4*scale,
        l.theme_row.bottom-90*scale,tile_left+(i+1)*tw-4*scale,l.theme_row.bottom-12*scale);
    l.accent_card=row(narrow ? 116.0f : 96.0f);
    const float accent_top=l.accent_card.bottom-(narrow ? 44.0f : 64.0f)*scale;
    l.accent_system=D2D1::RectF(right-116*scale,accent_top,right-16*scale,accent_top+32*scale);
    l.accent_picker=D2D1::RectF(right-232*scale,accent_top,right-124*scale,accent_top+32*scale);
    // The tiles reuse SettingsEffect's existing hit regions and controller.
    // A single tile selector avoids duplicate controls for the same setting.
    const int effect_columns = right-left < 440*scale ? 2 : 4;
    const int effect_rows = kWindowEffectCount/effect_columns;
    const float effect_top = 66.0f;
    l.effect_card=row(effect_top+82.0f*effect_rows+12.0f);
    l.effect_choice = {};
    const float effect_gap=8*scale;
    const float effect_width=(right-left-32*scale-effect_gap*(effect_columns-1))/effect_columns;
    for(int i=0;i<kWindowEffectCount;++i) {
        const float tile_x=left+16*scale+(i%effect_columns)*(effect_width+effect_gap);
        const float tile_y=l.effect_card.top+effect_top*scale+(i/effect_columns)*82*scale;
        l.effect_row[i]=D2D1::RectF(tile_x,tile_y,tile_x+effect_width,tile_y+74*scale);
    }
    l.language_card=row(narrow ? 98.0f : 64.0f); l.language_choice=choice(l.language_card,176);
    l.group[0]=D2D1::RectF(left,l.theme_row.top,right,y);
    section(1);
    using I = l10n::StringId;
    l.startup_row[0]=toggle_row(I::SettingsLaunchDesc); l.startup_tray_row=toggle_row(I::StartToTrayDesc);
    l.startup_row[1]=toggle_row(I::SettingsKeepRunningDesc); l.last_tab_row=toggle_row(I::CloseLastTabWindowDesc);
    l.startup_row[2]=toggle_row(I::SettingsDefaultManagerDesc);
    const I manager_descriptions[] = {I::SettingsOpenFoldersDesc, I::SettingsTakeoverWinEDesc,
        I::SettingsTakeoverThisPcDesc, I::SettingsExplorerTakeoverDesc};
    for(size_t i=0;i<std::size(l.default_manager_rows);++i)
        l.default_manager_rows[i]=toggle_row(manager_descriptions[i]);
    if(!vm.settings_system_pending && !vm.settings_system_status.empty())
        l.system_status=row((SettingsCaptionHeight(vm.settings_system_status, right-left-70*scale,
            scale, painter)+2*kSettingsRowPaddingDip*scale)/scale);
    l.new_tab_row=toggle_row(I::SettingsNewTabHomeDesc);
    l.group[1]=D2D1::RectF(left,l.startup_row[0].top,right,y);
    section(2);
    l.density_card=row(narrow ? 98.0f : 64.0f); segments(l.density_card,l.density_row,3,282);
    l.performance_row=toggle_row(I::SettingsShowPerformanceDesc);
    const I list_descriptions[] = {I::ListSmartDateDesc, I::ListZebraRowsDesc, I::ListSizeBarDesc};
    for(size_t i=0;i<std::size(l.list_style_row);++i) l.list_style_row[i]=toggle_row(list_descriptions[i]);
    l.folder_sort_card=row(narrow ? 98.0f : 64.0f); segments(l.folder_sort_card,l.folder_sort_row,3,282);
    l.group[2]=D2D1::RectF(left,l.density_card.top,right,y);
    y+=18*scale;
    l.disclosure[0]=row(64);
    if(vm.settings_expanded & 1u) {
        y+=10*scale;
        l.wallpaper_card=row(124);
        l.wallpaper_preview=D2D1::RectF(left+16*scale,l.wallpaper_card.top+12*scale,left+112*scale,l.wallpaper_card.top+68*scale);
        const float cw=painter ? painter->MeasureButtonWidth(l10n::Get(l10n::StringId::Clear)) : 80*scale;
        const float bw=painter ? painter->MeasureButtonWidth(l10n::Get(l10n::StringId::ChooseImage)) : 120*scale;
        l.wallpaper_clear=D2D1::RectF(right-16*scale-cw,y-44*scale,right-16*scale,y-12*scale);
        l.wallpaper_choose=D2D1::RectF(l.wallpaper_clear.left-8*scale-bw,y-44*scale,l.wallpaper_clear.left-8*scale,y-12*scale);
        y+=8*scale; l.wallpaper_look_card=row(narrow ? 98.0f : 64.0f); l.wallpaper_look_value=choice(l.wallpaper_look_card,112);
        y+=8*scale; l.wallpaper_blur_card=row(narrow ? 98.0f : 64.0f); segments(l.wallpaper_blur_card,l.wallpaper_blur_row,3,282);
        y+=8*scale; l.tray_icon_card=row(narrow ? 98.0f : 64.0f); segments(l.tray_icon_card,l.tray_icon_row,3,282);
        y+=8*scale; l.hidden_files_row=toggle_row(I::SettingsShowHiddenDesc);
        y+=8*scale; l.protected_files_row=toggle_row(I::SettingsShowProtectedDesc);
        y+=8*scale; l.pinned_names_row=toggle_row(I::PinnedNamesDesc);
        y+=8*scale; l.blank_click_row=toggle_row(I::SettingsBlankClickBackDesc);
        y+=8*scale; l.change_tracking_row=toggle_row(I::SettingsChangeTrackingDesc);
        l.change_days_row=row(narrow ? 98.0f : 64.0f); segments(l.change_days_row,l.change_days,3,282);
    }
    y+=18*scale; l.configuration_path=row(152);
    const l10n::StringId config_labels[] = {l10n::StringId::OpenLocation, l10n::StringId::ChangeLocation};
    const float config_bottom = LayoutSettingsActions(left+16*scale, right-16*scale,
        l.configuration_path.top+108*scale, scale, config_labels, 2, l.configuration_action, painter);
    y = std::max(y, config_bottom+4*scale);
    l.configuration_path.bottom = y;
    y+=12*scale; l.footer=row(44); y+=16*scale;
    return y;
}

float LayoutSettingsContent(SettingsLayout& l, const WindowViewModel& vm, float scale,
                           float y, const fluent::Painter* painter) {
    const float left=l.content.left+20*scale,right=l.content.right-20*scale;
    const bool narrow=right-left<560*scale;
    auto row=[&](float h) { auto r=D2D1::RectF(left,y,right,y+h*scale);y=r.bottom;return r; };
    y+=14*scale; l.section[1]=row(28);
    l.content_header=row(72);
    l.content_types=row(narrow ? 236.0f : 160.0f);
    if(vm.settings_content_folders.empty()) l.content_empty=row(84);
    else if (!vm.settings_content_instant) {
        auto r=row(52);
        const float bw=painter ? painter->MeasureButtonWidth(l10n::Get(vm.settings_content_paused ? l10n::StringId::ContentIndexResume : l10n::StringId::ContentIndexPause)) : 150*scale;
        l.content_pause=D2D1::RectF(left+16*scale,r.top+10*scale,left+16*scale+bw,r.bottom-10*scale);
    }
    l.content_options=row(64);
    if(!vm.settings_content_instant && !vm.settings_content_folders.empty()) l.content_rebuild=row(64);
    l.group[1]=D2D1::RectF(left,l.content_header.top,right,y);
    y+=8*scale; l.footer=row(40);
    y+=24*scale;
    return y;
}
