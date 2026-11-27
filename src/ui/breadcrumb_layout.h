// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "ui_renderer.h"
#include <span>

namespace pulse::ui {

void LayoutBreadcrumbs(Compositor* compositor, const D2D1_RECT_F& bounds, float scale, float margin,
    const std::wstring& path, std::wstring_view title, std::vector<MainRenderer::BreadcrumbPlaced>& out);
int HitBreadcrumbSegment(std::span<const MainRenderer::BreadcrumbPlaced> segments, float x, float y);
void DrawBreadcrumbSegmentHighlight(fluent::Painter& painter, const Theme& theme,
    const MainRenderer::BreadcrumbPlaced& segment, float opacity, bool pressed = false);
void DrawBreadcrumbSegmentLabel(fluent::Painter& painter, Compositor& compositor, const Theme& theme,
    const MainRenderer::BreadcrumbPlaced& segment, bool separator, D2D1_COLOR_F ink);

} // namespace pulse::ui
