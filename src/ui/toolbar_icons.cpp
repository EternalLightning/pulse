// SPDX-License-Identifier: Apache-2.0
#include "toolbar_icons.h"
#include <map>

namespace pulse::ui::toolbar_icons {
void DrawNavigationButton(fluent::Painter& painter, D2D1_RECT_F bounds, std::wstring_view glyph,
                          const Theme& theme, fluent::ControlState state, bool high_contrast) {
    painter.DrawButton({bounds, {}, {}, fluent::ButtonKind::Transparent, state, true});
    const auto foreground = !state.enabled ? theme.text_disabled
        : high_contrast ? (state.hovered || state.pressed ? theme.text_selected : theme.text_secondary) : theme.text;
    painter.DrawGlyph(glyph, bounds, foreground, true, 20.0f);
}

command_icons::Icon Resolve(std::wstring_view glyph, fluent::MenuPictogram pictogram) {
    if (pictogram != fluent::MenuPictogram::None) return fluent::PaneLayoutIcon(pictogram);
    return command_icons::FromGlyph(glyph);
}

bool Draw(fluent::Painter& painter, ID2D1RenderTarget* target, ID2D1SolidColorBrush* brush, ID2D1StrokeStyle* stroke,
          command_icons::Icon icon, D2D1_RECT_F bounds, float scale, float slot_dip, bool monochrome) {
    if (!target || !brush || icon <= command_icons::Icon::None || icon >= command_icons::Icon::Count) return false;
    const int side = static_cast<int>(std::round((std::min)({slot_dip * scale,
        bounds.right - bounds.left, bounds.bottom - bounds.top})));
    if (side <= 0) return true;
    const float art = static_cast<float>(side);
    const float cx = (bounds.left + bounds.right) * 0.5f;
    const float cy = (bounds.top + bounds.bottom) * 0.5f;
    if (icon == command_icons::Icon::More) {
        // Files' 16-unit More icon uses three circles, centered five units apart.
        const float pitch = std::round(5.0f * art / 16.0f);
        const float radius = 1.25f * art / 16.0f;
        const float x = std::floor(cx) + 0.5f;
        const float y = std::floor(cy) + 0.5f;
        for (const float dx : {-pitch, 0.0f, pitch})
            target->FillEllipse(D2D1::Ellipse({x + dx, y}, radius, radius), brush);
        return true;
    }
    if (painter.DrawCommandSvg(icon, bounds, brush->GetColor(), monochrome, slot_dip)) return true;
    const float weight = (std::max)(1.0f,std::round(command_icons::kStroke*art/command_icons::kCanvas));
    struct Cache {
        ID2D1Factory* factory = nullptr;
        std::map<int,std::array<ID2D1PathGeometry*,static_cast<size_t>(command_icons::Icon::Count)>> sizes;
        void Clear() {
            for (auto& [size, icons] : sizes) {
                (void)size;
                for (auto* geometry : icons) if (geometry) geometry->Release();
            }
            sizes.clear();
        }
        ~Cache() { Clear(); if (factory) factory->Release(); }
    };
    static thread_local Cache cache;
    ID2D1Factory* factory = nullptr; target->GetFactory(&factory);
    if (!factory) return false;
    if (factory != cache.factory) {
        cache.Clear(); if (cache.factory) cache.factory->Release(); cache.factory = factory;
    } else factory->Release();
    if (!cache.sizes.contains(side) && cache.sizes.size() >= 8) cache.Clear();
    auto*& geometry = cache.sizes[side][static_cast<size_t>(icon)];
    if (!geometry) geometry = command_icons::BuildGeometry(cache.factory,icon,art,weight);
    if (!geometry) return false;
    const float phase = std::fmod(weight, 2.0f) * 0.5f;
    const float center = std::round(art * 0.5f - phase) + phase;
    D2D1_MATRIX_3X2_F previous{}; target->GetTransform(&previous);
    target->SetTransform(D2D1::Matrix3x2F::Translation(
        std::round(cx - center), std::round(cy - center))*previous);
    if (icon == command_icons::Icon::StarFilled) target->FillGeometry(geometry,brush);
    target->DrawGeometry(geometry,brush,weight,stroke);
    target->SetTransform(previous);
    return true;
}
}
