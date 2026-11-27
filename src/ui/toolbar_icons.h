// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "command_icons.h"
#include "fluent_components.h"

namespace pulse::ui::toolbar_icons {
command_icons::Icon Resolve(std::wstring_view glyph, fluent::MenuPictogram pictogram);
void DrawNavigationButton(fluent::Painter& painter, D2D1_RECT_F bounds, std::wstring_view glyph,
                          const Theme& theme, fluent::ControlState state, bool high_contrast);
bool Draw(fluent::Painter& painter, ID2D1RenderTarget* target, ID2D1SolidColorBrush* brush, ID2D1StrokeStyle* stroke,
          command_icons::Icon icon, D2D1_RECT_F bounds, float scale, float slot_dip = 16.0f,
          bool monochrome = true);
}
