#pragma once
#include <d2d1.h>
#include <algorithm>
#include <array>

namespace pulse::ui {
struct ToolbarLayout {
    std::array<D2D1_RECT_F, 4> navigation{};
    D2D1_RECT_F address{}, search{}, create{}, extract{}, extract_all{};
    D2D1_RECT_F sort{}, filter{}, overflow{};
    std::array<D2D1_RECT_F, 9> commands{};
};

inline ToolbarLayout MakeToolbarLayout(float width, float scale, float top, float margin, float create_width,
                                      float left, float filter_expand = 0.0f, bool archive = false) {
    ToolbarLayout out;
    const float available = width - left;
    const float nav_step = available < 500*scale ? 26*scale : 34*scale;
    float x = left + margin;
    for (int i=0;i<4;++i) {
        out.navigation[i] = {x,top+6*scale,x+nav_step-4*scale,top+38*scale};
        x += nav_step;
    }
    const float search_width = std::clamp(available*0.27f, 140*scale, 360*scale);
    out.search = {width-margin-search_width,top+4*scale,width-margin,top+40*scale};
    out.address = {x+6*scale,top+4*scale,out.search.left-8*scale,top+40*scale};
    if (available < 480*scale) {
        out.search.left=out.search.right-32*scale;
    }
    out.address.right = out.search.left-8*scale;
    out.create = {left+margin,top+50*scale,left+margin+create_width,top+82*scale};
    const float sort_width = available >= 700*scale ? 88*scale : 32*scale;
    const float filter_base = available >= 700*scale ? 88*scale : 32*scale;
    out.overflow = {width-margin-32*scale,top+50*scale,width-margin,top+82*scale};
    const float filter_right = out.overflow.left-8*scale;
    const float left_limit = filter_right-filter_base-6*scale-sort_width-8*scale;
    x = out.create.right + 12*scale;
    for (int i=0;i<6;++i) {
        out.commands[i] = {x,top+50*scale,x+30*scale,top+82*scale};
        x += 34*scale;
    }
    if (available < 540*scale || x-4*scale > left_limit) {
        for (auto& command : out.commands) command = {};
        x = out.create.right;
    } else {
        x -= 4*scale;
        if (archive && x+8*scale+64*scale+6*scale+128*scale <= left_limit) {
            out.extract = {x+8*scale,top+50*scale,x+72*scale,top+82*scale};
            out.extract_all = {out.extract.right+6*scale,top+50*scale,
                               out.extract.right+134*scale,top+82*scale};
            x = out.extract_all.right;
        }
    }
    const float filter_max = std::clamp(filter_right-x-8*scale-sort_width-6*scale,
                                        filter_base,220*scale);
    const float filter_width = filter_base + (filter_max-filter_base)*std::clamp(filter_expand,0.0f,1.0f);
    out.filter = {filter_right-filter_width,top+50*scale,filter_right,top+82*scale};
    out.sort = {out.filter.left-6*scale-sort_width,top+50*scale,
                out.filter.left-6*scale,top+82*scale};
    return out;
}
}
