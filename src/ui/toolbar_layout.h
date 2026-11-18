// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once
#include <d2d1.h>
#include <algorithm>
#include <array>
#include <span>
#include <vector>

namespace pulse::ui {
// Stable MenuCmd IDs; this geometry header does not link the application catalog.
inline constexpr std::array<int, 8> kDefaultToolbarOrder{186, 2, 3, 4, 6, 5, 7, 306};
inline constexpr std::array<int, 9> kLegacyToolbarCommands{2, 3, 4, 6, 5, 7, 306, 101, 181};
struct ToolbarCommandPlacement {
    int command = 0;
    D2D1_RECT_F bounds{};
};
struct ToolbarLayout {
    std::array<D2D1_RECT_F, 4> navigation{};
    D2D1_RECT_F address{}, search{}, operations{}, create{}, extract{}, extract_all{}, empty_recycle{};
    D2D1_RECT_F sort{}, group{}, filter{}, overflow{};
    std::array<D2D1_RECT_F, 9> commands{};
    std::vector<ToolbarCommandPlacement> placed_commands;
    bool has_overflow = false;
};

inline ToolbarLayout MakeToolbarLayout(float width, float scale, float top, float margin, float create_width,
                                      float left, float filter_expand = 0.0f, bool archive = false,
                                      bool recycle = false,
                                      std::span<const int> command_order = kDefaultToolbarOrder) {
    ToolbarLayout out;
    const float available = width - left;
    const float nav_step = available < 500*scale ? 28*scale : 34*scale;
    float x = left + margin;
    for (int i=0;i<4;++i) {
        out.navigation[i] = {x,top+6*scale,x+nav_step-4*scale,top+38*scale};
        x += nav_step;
    }
    const float search_width = std::clamp(available*0.27f, 140*scale, 360*scale);
    out.operations = {width-margin-36*scale,top+4*scale,width-margin,top+40*scale};
    const float search_right = out.operations.left-8*scale;
    out.search = {search_right-search_width,top+4*scale,search_right,top+40*scale};
    out.address = {x+6*scale,top+4*scale,out.search.left-8*scale,top+40*scale};
    if (available < 480*scale) {
        out.search.left=out.search.right-32*scale;
    }
    out.address.right = out.search.left-8*scale;
    // Leave room for localized text ink after the icons, chevrons and insets.
    const float dropdown_width = available >= 700*scale ? 120*scale : 32*scale;
    const float filter_base = available >= 700*scale ? 88*scale : 32*scale;
    const float action_gap = 8*scale;
    out.overflow = {width-margin-32*scale,top+50*scale,width-margin,top+82*scale};
    const float filter_right = out.overflow.left-action_gap;
    const float dropdown_span = 2*(dropdown_width+action_gap);
    const float left_limit = filter_right-filter_base-dropdown_span-action_gap;
    x = left + margin;
    for (const int command : command_order) {
        if (command <= 0 || std::any_of(out.placed_commands.begin(), out.placed_commands.end(),
            [=](const auto& item) { return item.command == command; })) continue;
        const float command_width = command == 186 ? create_width : 30*scale;
        if (x + command_width > left_limit) { out.has_overflow = true; break; }
        const D2D1_RECT_F bounds{x, top+50*scale, x+command_width, top+82*scale};
        out.placed_commands.push_back({command, bounds});
        if (command == 186) out.create = bounds;
        for (size_t i = 0; i < kLegacyToolbarCommands.size(); ++i)
            if (command == kLegacyToolbarCommands[i]) out.commands[i] = bounds;
        x += command_width + (command == 186 ? 12*scale : 4*scale);
    }
    if (!out.placed_commands.empty()) x -= 4*scale;
    if (archive && x+8*scale+64*scale+6*scale+128*scale <= left_limit) {
        out.extract = {x+8*scale,top+50*scale,x+72*scale,top+82*scale};
        out.extract_all = {out.extract.right+6*scale,top+50*scale,
                           out.extract.right+134*scale,top+82*scale};
        x = out.extract_all.right;
    } else if (archive) out.has_overflow = true;
    if (recycle && x+8*scale+144*scale <= left_limit) {
        out.empty_recycle = {x+8*scale,top+50*scale,x+152*scale,top+82*scale};
        x = out.empty_recycle.right;
    } else if (recycle) out.has_overflow = true;
    const float filter_max = std::clamp(filter_right-x-dropdown_span-action_gap,
                                        filter_base,220*scale);
    const float filter_width = filter_base + (filter_max-filter_base)*std::clamp(filter_expand,0.0f,1.0f);
    out.filter = {filter_right-filter_width,top+50*scale,filter_right,top+82*scale};
    out.group = {out.filter.left-action_gap-dropdown_width,top+50*scale,
                 out.filter.left-action_gap,top+82*scale};
    out.sort = {out.group.left-action_gap-dropdown_width,top+50*scale,
                out.group.left-action_gap,top+82*scale};
    return out;
}
}
