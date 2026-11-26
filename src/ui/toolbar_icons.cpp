// SPDX-License-Identifier: Apache-2.0
#include "toolbar_icons.h"
#include "../common/resource_ids.h"
#include <map>

namespace pulse::ui::toolbar_icons {
namespace {
int IconResource(command_icons::Icon icon) {
    if (const int resource = command_icons::WorkspaceSvg(icon)) return resource;
    using I = command_icons::Icon;
    switch (icon) {
    case I::Details: return IDR_FILES_LAYOUT_DETAILS_SVG;
    case I::Grid: case I::Applications: return IDR_FILES_LAYOUT_GRID_SVG;
    case I::List: return IDR_FILES_LAYOUT_LIST_SVG;
    case I::Columns: case I::LayoutThree: return IDR_FILES_LAYOUT_COLUMNS_SVG;
    case I::Content: return IDR_FILES_LAYOUT_CONTENT_SVG;
    case I::Split: return IDR_FILES_PANE_VERTICAL_SVG;
    case I::LayoutStacked: return IDR_FILES_PANE_HORIZONTAL_SVG;
    case I::Panel: return IDR_FILES_PANEL_RIGHT_SVG;
    case I::PanelClose: return IDR_FILES_PANEL_RIGHT_FILLED_SVG;
    case I::SelectAll: return IDR_FILES_SELECT_ALL_SVG;
    case I::InvertSelection: return IDR_FILES_SELECT_INVERT_SVG;
    case I::Copy: return IDR_FILES_COPY_SVG;
    case I::Cut: return IDR_FILES_CUT_SVG;
    case I::Rename: return IDR_FILES_RENAME_SVG;
    case I::File: return IDR_FILES_TOOLBAR_FILE_SVG;
    case I::Folder: return IDR_FILES_TOOLBAR_FOLDER_SVG;
    case I::NewFolder: return IDR_FILES_TOOLBAR_NEW_FOLDER_SVG;
    case I::Terminal: return IDR_FILES_TOOLBAR_TERMINAL_SVG;
    case I::Refresh: return IDR_FILES_TOOLBAR_REFRESH_SVG;
    case I::Search: return IDR_FILES_TOOLBAR_SEARCH_SVG;
    case I::Settings: return IDR_FILES_TOOLBAR_SETTINGS_SVG;
    case I::Recycle: return IDR_FILES_TOOLBAR_RECYCLE_SVG;
    case I::Pin: return IDR_FILES_TOOLBAR_PIN_SVG;
    case I::Filter: return IDR_FILES_FILTER_SVG;
    case I::Library: return IDR_FLUENT_BOOK_OPEN_SVG;
    case I::Undo: return IDR_FLUENT_ARROW_UNDO_SVG;
    case I::Link: return IDR_FLUENT_LINK_SVG;
    case I::LayoutSingle: return IDR_FLUENT_SQUARE_SVG;
    case I::LayoutFour: return IDR_FLUENT_TABLE_SIMPLE_SVG;
    default: return 0;
    }
}
}

bool DrawArtwork(fluent::Painter& painter, int command, D2D1_RECT_F bounds, float scale,
                 bool dark, bool monochrome, const D2D1_COLOR_F& foreground, float slot_dip) {
    struct Artwork {
        int command, outline, light, dark;
        float inset;
        float max_dip = 18.0f;
    };
    // SVGs that fill more of the canvas need a larger inset to match the neighboring icon ink.
    constexpr Artwork artwork[] = {
        {186, IDR_FILES_NEW_SVG, IDR_FILES_NEW_COLOR_SVG, IDR_FILES_NEW_COLOR_DARK_SVG, 1},
        {2, IDR_FILES_CUT_SVG, IDR_FILES_CUT_COLOR_SVG, IDR_FILES_CUT_COLOR_DARK_SVG, 0, 17},
        {3, IDR_FILES_COPY_SVG, IDR_FILES_COPY_COLOR_SVG, IDR_FILES_COPY_COLOR_DARK_SVG, 0},
        {4, IDR_FILES_PASTE_SVG, IDR_FILES_PASTE_COLOR_SVG, IDR_FILES_PASTE_COLOR_DARK_SVG, 0, 17},
        {6, IDR_FILES_RENAME_SVG, IDR_FILES_RENAME_COLOR_SVG, IDR_FILES_RENAME_COLOR_DARK_SVG, 1},
        {5, IDR_FILES_DELETE_SVG, IDR_FILES_DELETE_COLOR_SVG, IDR_FILES_DELETE_COLOR_DARK_SVG, 1},
        {7, IDR_FILES_PROPERTIES_SVG, IDR_FILES_PROPERTIES_COLOR_SVG, IDR_FILES_PROPERTIES_COLOR_DARK_SVG, 1},
        {9, IDR_FILES_COPY_PATH_SVG, 0, 0, 0},
        {174, IDR_FILES_TOOLBAR_ADVANCED_SEARCH_SVG, 0, 0, 0},
        {189, IDR_FILES_STATUS_CENTER_SVG, 0, 0, 0, 16},
        {300, IDR_TOOLBAR_CUSTOMIZE_SVG, 0, 0, 0, 16},
    };
    const auto* icon = std::find_if(std::begin(artwork), std::end(artwork),
        [command](const Artwork& item) { return item.command == command; });
    if (icon == std::end(artwork)) return false;
    const float art_dip = (std::min)(slot_dip - 2 * icon->inset, icon->max_dip);
    const float size = std::round((std::min)({art_dip * scale,
        bounds.right - bounds.left, bounds.bottom - bounds.top}));
    const float cx = (bounds.left + bounds.right) * 0.5f;
    const float cy = (bounds.top + bounds.bottom) * 0.5f;
    // The New icon's two plus strokes intersect on the canvas center.
    const float left = command == 186 ? std::floor(cx) + 0.5f - size * 0.5f : std::round(cx - size * 0.5f);
    const float top = command == 186 ? std::floor(cy) + 0.5f - size * 0.5f : std::round(cy - size * 0.5f);
    const auto art = D2D1::RectF(left, top, left + size, top + size);
    const int color_resource = dark ? icon->dark : icon->light;
    if (!monochrome && color_resource && painter.DrawSvgResource(color_resource, art)) return true;
    return painter.DrawSvgResource(icon->outline, art, &foreground);
}

command_icons::Icon Resolve(std::wstring_view glyph, fluent::MenuPictogram pictogram, int command) {
    using I = command_icons::Icon;
    using P = fluent::MenuPictogram;
    if (command == 181) return I::Columns;
    if (command == 101) return I::Panel;
    if (command == 57) return I::Pin;
    if ((command >= 80 && command <= 83) || command == 86) return I::Grid;
    if (command == 85) return I::Details;
    if (command == 87) return I::Content;
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

bool Draw(fluent::Painter& painter, ID2D1RenderTarget* target, ID2D1SolidColorBrush* brush, ID2D1StrokeStyle* stroke,
          command_icons::Icon icon, D2D1_RECT_F bounds, float scale, float slot_dip) {
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
    if (const int resource = IconResource(icon)) {
        // Keep the center line of these symmetric outlines on one physical pixel.
        using I = command_icons::Icon;
        const bool center_line = icon == I::Split || icon == I::LayoutStacked || icon == I::LayoutFour || icon == I::Library;
        const float size = center_line && side > 1 && side % 2 == 0 ? art - 1.0f : art;
        const float x = std::round(cx - size * 0.5f);
        const float y = std::round(cy - size * 0.5f);
        const auto foreground = brush->GetColor();
        if (painter.DrawSvgResource(resource, {x, y, x + size, y + size}, &foreground)) return true;
    }
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
