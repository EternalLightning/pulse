// Included inside ui_renderer_internal.h's anonymous namespace.
float LayoutSettingsGeneral(SettingsLayout& l, const WindowViewModel& vm, float scale,
                            float y, const fluent::Painter* painter) {
    const float left = l.content.left + 20*scale, right = l.content.right - 20*scale;
    const bool narrow = right - left < 560*scale;
    auto row = [&](float h) { auto r = D2D1::RectF(left, y, right, y+h*scale); y=r.bottom; return r; };
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
    l.startup_row[0]=row(64); l.startup_row[1]=row(64); l.startup_row[2]=row(88);
    l.group[1]=D2D1::RectF(left,l.startup_row[0].top,right,y);
    section(2);
    l.density_card=row(narrow ? 98.0f : 64.0f); segments(l.density_card,l.density_row,3,282);
    l.performance_row=row(64);
    for(auto& list_row : l.list_style_row) list_row=row(64);
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
        y+=8*scale; l.hidden_files_row=row(64);
        y+=8*scale; l.protected_files_row=row(64);
        y+=8*scale; l.pinned_names_row=row(64);
        y+=8*scale; l.blank_click_row=row(64);
        y+=8*scale; l.change_tracking_row=row(64);
        l.change_days_row=row(narrow ? 98.0f : 64.0f); segments(l.change_days_row,l.change_days,3,282);
    }
    y+=18*scale; l.configuration_path=row(152);
    const float config_width=(right-left-40*scale)/2;
    for(int i=0;i<2;++i) {
        const float x=left+16*scale+i*(config_width+8*scale);
        l.configuration_action[i]=D2D1::RectF(x,l.configuration_path.top+108*scale,x+config_width,l.configuration_path.top+140*scale);
    }
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
