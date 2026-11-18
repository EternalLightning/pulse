// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "ui_renderer_internal.h"
#include "pane_header_icons.h"
#include "command_icons.h"
#include "toolbar_icons.h"

namespace pulse::ui {

void MainRenderer::DrawPaneHeaderIcon(const D2D1_RECT_F& rc, PaneHeaderIcon icon,
                                      const D2D1_COLOR_F& color) {
    auto* dc = compositor_->Dc();
    if (!paneHeaderStroke_.get()) command_icons::CreateStrokeStyle(dc, &paneHeaderStroke_);
    using I = command_icons::Icon;
    I command = I::None;
    switch (icon) {
    case PaneHeaderIcon::Back: command = I::Back; break;
    case PaneHeaderIcon::Forward: command = I::Forward; break;
    case PaneHeaderIcon::Up: command = I::Up; break;
    case PaneHeaderIcon::Columns: command = I::Columns; break;
    case PaneHeaderIcon::MediumIcons: command = I::Grid; break;
    case PaneHeaderIcon::More: command = I::More; break;
    case PaneHeaderIcon::Split: command = I::Split; break;
    case PaneHeaderIcon::View: command = I::Details; break;
    case PaneHeaderIcon::Filter: command = I::Filter; break;
    }
    MakeBrush(dc, color, brText_);
    toolbar_icons::Draw(painter_, dc, brText_.get(), paneHeaderStroke_.get(), command, rc, scale_);
}

} // namespace pulse::ui
