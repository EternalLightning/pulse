// SPDX-License-Identifier: Apache-2.0
#include "toolbar_icons.h"
#include <map>

namespace pulse::ui::toolbar_icons {
command_icons::Icon Resolve(std::wstring_view glyph, fluent::MenuPictogram pictogram, int command) {
    using I = command_icons::Icon;
    using P = fluent::MenuPictogram;
    if (command == 181) return I::Columns;
    if (command == 101) return I::Panel;
    switch (pictogram) {
    case P::LayoutSingle: return I::LayoutSingle;
    case P::LayoutSideBySide: return I::Split;
    case P::LayoutStacked: return I::LayoutStacked;
    case P::LayoutThree: return I::LayoutThree;
    case P::LayoutFour: return I::LayoutFour;
    default: break;
    }
    if (glyph.size() == 1) switch (glyph.front()) {
    case L'\xE7A7': return I::Undo;
    case L'\xE8B3': return I::SelectAll;
    case L'\xE7A1': return I::InvertSelection;
    case L'\xE756': return I::Terminal;
    case L'\xE8F4': return I::NewFolder;
    case L'\xE8A9': return I::Split;
    case L'\xECA5': return I::Grid;
    case L'\xEA37': return I::List;
    case L'\xE9D9': return I::Tray;
    case L'\xE8F1': return I::Library;
    case L'\xE71D': return I::Applications;
    default: break;
    }
    return command_icons::FromGlyph(glyph);
}

bool Draw(ID2D1RenderTarget* target, ID2D1Brush* brush, ID2D1StrokeStyle* stroke,
          command_icons::Icon icon, D2D1_RECT_F bounds, float scale) {
    if (!target || !brush || icon <= command_icons::Icon::None || icon >= command_icons::Icon::Count) return false;
    const int side = static_cast<int>(std::round((std::min)({16*scale, bounds.right-bounds.left, bounds.bottom-bounds.top})));
    if (side <= 0) return true;
    const float art = static_cast<float>(side);
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
    D2D1_MATRIX_3X2_F previous{}; target->GetTransform(&previous);
    target->SetTransform(D2D1::Matrix3x2F::Translation(
        std::round((bounds.left+bounds.right-art)*0.5f),
        std::round((bounds.top+bounds.bottom-art)*0.5f))*previous);
    if (icon == command_icons::Icon::StarFilled || icon == command_icons::Icon::More) target->FillGeometry(geometry,brush);
    if (icon != command_icons::Icon::More) target->DrawGeometry(geometry,brush,weight,stroke);
    target->SetTransform(previous);
    return true;
}
}
