// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "command_icons.h"
#include "fluent_components.h"

namespace pulse::ui::toolbar_icons {
command_icons::Icon Resolve(std::wstring_view glyph, fluent::MenuPictogram pictogram = fluent::MenuPictogram::None,
                            int command = 0);
bool Draw(ID2D1RenderTarget* target, ID2D1Brush* brush, ID2D1StrokeStyle* stroke,
          command_icons::Icon icon, D2D1_RECT_F bounds, float scale);
}
