// SPDX-License-Identifier: Apache-2.0
#include "../ui/ui_renderer_internal.h"
#include "../ui/toolbar_icons.h"
#include "../ui/toolbar_customization.h"
#include "../ui/toolbar_layout.h"
#include "../app/command_catalog.h"
#include "../app/context_menu.h"
#include "../common/resource_ids.h"
#include <filesystem>
#include <fstream>
#include <cstdio>
#include <map>

namespace {
using namespace pulse;
int passed = 0, failed = 0;
void Check(bool ok, const std::string& label) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label.c_str());
    ok ? ++passed : ++failed;
}
struct Pixels {
    int width = 0, height = 0, left = 0, top = 0;
    std::vector<BYTE> bgra;
    const BYTE* At(int x, int y) const { return bgra.data() + (static_cast<size_t>(y) * width + x) * 4; }
};
Pixels ReadCrop(ui::Compositor& compositor, D2D1_RECT_F bounds) {
    Pixels out;
    auto* dc = compositor.Dc();
    ui::ComPtr<ID2D1Image> image; dc->GetTarget(&image);
    ui::ComPtr<ID2D1Bitmap1> target, readable;
    if (!image.get() || FAILED(image->QueryInterface(IID_PPV_ARGS(&target)))) return out;
    const auto size = target->GetPixelSize();
    out.left = std::max(0, static_cast<int>(std::floor(bounds.left)));
    out.top = std::max(0, static_cast<int>(std::floor(bounds.top)));
    const int right = std::min(static_cast<int>(size.width), static_cast<int>(std::ceil(bounds.right)));
    const int bottom = std::min(static_cast<int>(size.height), static_cast<int>(std::ceil(bounds.bottom)));
    out.width = right - out.left; out.height = bottom - out.top;
    if (out.width <= 0 || out.height <= 0) return {};
    const auto props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        target->GetPixelFormat(), 96, 96);
    const D2D1_RECT_U source{static_cast<UINT>(out.left), static_cast<UINT>(out.top),
        static_cast<UINT>(right), static_cast<UINT>(bottom)};
    if (FAILED(dc->CreateBitmap(D2D1::SizeU(out.width, out.height), nullptr, 0, props, &readable)) ||
        FAILED(readable->CopyFromBitmap(nullptr, target.get(), &source))) return {};
    D2D1_MAPPED_RECT mapped{};
    if (FAILED(readable->Map(D2D1_MAP_OPTIONS_READ, &mapped))) return {};
    out.bgra.resize(static_cast<size_t>(out.width) * out.height * 4);
    for (int y = 0; y < out.height; ++y)
        std::copy_n(mapped.bits + static_cast<size_t>(y) * mapped.pitch, out.width * 4,
            out.bgra.data() + static_cast<size_t>(y) * out.width * 4);
    readable->Unmap();
    return out;
}
std::array<BYTE, 4> Background(const Pixels& pixels) {
    if (pixels.bgra.empty()) return {};
    // Midpoints of the four clear margins are inside the rounded button fill.
    const std::array<std::pair<int,int>, 4> points{{{pixels.width / 2, 1},
        {pixels.width / 2, pixels.height - 2}, {1, pixels.height / 2},
        {pixels.width - 2, pixels.height / 2}}};
    std::array<BYTE, 4> color{};
    for (int channel = 0; channel < 4; ++channel) {
        std::array<BYTE, 4> values{};
        for (size_t i = 0; i < points.size(); ++i)
            values[i] = pixels.At(points[i].first, points[i].second)[channel];
        std::sort(values.begin(), values.end());
        color[channel] = static_cast<BYTE>((static_cast<unsigned>(values[1]) + values[2]) / 2);
    }
    return color;
}
struct PairedColor {
    int compared = 0, incorrect = 0;
    float max_error = 0;
};
PairedColor CompareReference(const Pixels& actual, const Pixels& reference) {
    PairedColor result;
    if (actual.bgra.empty() || actual.width != reference.width || actual.height != reference.height) return result;
    for (int y = 0; y < actual.height; ++y) for (int x = 0; x < actual.width; ++x) {
        const auto* a = actual.At(x,y); const auto* r = reference.At(x,y);
        ++result.compared;
        bool incorrect = false;
        for (int c = 0; c < 3; ++c) {
            const float error = static_cast<float>(std::abs(static_cast<int>(a[c]) - r[c]));
            result.max_error = std::max(result.max_error, error);
            incorrect |= error >= 4;
        }
        if (incorrect) ++result.incorrect;
    }
    return result;
}
struct Artwork {
    int outline, light, dark;
    float size;
};
Artwork ExpectedArtwork(int command) {
    using namespace pulse::app;
    switch (command) {
    case CmdNewMenu: return {IDR_FILES_NEW_SVG, IDR_FILES_NEW_COLOR_SVG, IDR_FILES_NEW_COLOR_DARK_SVG, 16};
    case CmdCut: return {IDR_FILES_CUT_SVG, IDR_FILES_CUT_COLOR_SVG, IDR_FILES_CUT_COLOR_DARK_SVG, 17};
    case CmdCopy: return {IDR_FILES_COPY_SVG, IDR_FILES_COPY_COLOR_SVG, IDR_FILES_COPY_COLOR_DARK_SVG, 18};
    case CmdPaste: return {IDR_FILES_PASTE_SVG, IDR_FILES_PASTE_COLOR_SVG, IDR_FILES_PASTE_COLOR_DARK_SVG, 17};
    case CmdRename: return {IDR_FILES_RENAME_SVG, IDR_FILES_RENAME_COLOR_SVG, IDR_FILES_RENAME_COLOR_DARK_SVG, 16};
    case CmdDelete: return {IDR_FILES_DELETE_SVG, IDR_FILES_DELETE_COLOR_SVG, IDR_FILES_DELETE_COLOR_DARK_SVG, 16};
    case CmdProperties: return {IDR_FILES_PROPERTIES_SVG, IDR_FILES_PROPERTIES_COLOR_SVG, IDR_FILES_PROPERTIES_COLOR_DARK_SVG, 16};
    case CmdCopyPath: return {IDR_FILES_COPY_PATH_SVG, 0, 0, 18};
    case CmdAdvancedSearch: return {IDR_FILES_TOOLBAR_ADVANCED_SEARCH_SVG, 0, 0, 18};
    case CmdOperationCenter: return {IDR_FILES_STATUS_CENTER_SVG, 0, 0, 16};
    case CmdCustomizeCommands: return {IDR_TOOLBAR_CUSTOMIZE_SVG, 0, 0, 16};
    default: return {};
    }
}
int ExpectedRoundedArtwork(ui::command_icons::Icon icon) {
    using I = ui::command_icons::Icon;
    switch (icon) {
    case I::Details: return IDR_FILES_LAYOUT_DETAILS_SVG;
    case I::Grid: case I::Applications: return IDR_FILES_LAYOUT_GRID_SVG;
    case I::List: return IDR_FILES_LAYOUT_LIST_SVG;
    case I::Columns: case I::LayoutThree: return IDR_FILES_LAYOUT_COLUMNS_SVG;
    case I::Content: return IDR_FILES_LAYOUT_CONTENT_SVG;
    case I::Split: return IDR_FILES_PANE_VERTICAL_SVG;
    case I::LayoutStacked: return IDR_FILES_PANE_HORIZONTAL_SVG;
    case I::Panel: return IDR_FILES_PANEL_RIGHT_SVG;
    case I::SelectAll: return IDR_FILES_SELECT_ALL_SVG;
    case I::InvertSelection: return IDR_FILES_SELECT_INVERT_SVG;
    case I::Copy: return IDR_FILES_COPY_SVG;
    case I::Cut: return IDR_FILES_CUT_SVG;
    case I::Rename: return IDR_FILES_RENAME_SVG;
    case I::File: return IDR_FILES_TOOLBAR_FILE_SVG;
    case I::Folder: return IDR_FILES_TOOLBAR_FOLDER_SVG;
    case I::NewFolder: return IDR_FILES_TOOLBAR_NEW_FOLDER_SVG;
    case I::Terminal: return IDR_FILES_TOOLBAR_TERMINAL_SVG;
    case I::DisplayAlias: return IDR_WORKSPACE_DISPLAY_ALIAS_SVG;
    case I::LinkedBrowsing: return IDR_WORKSPACE_LINKED_BROWSING_SVG;
    case I::Sync: return IDR_WORKSPACE_DIRECTORY_SYNC_SVG;
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
Pixels ExpectedIcon(ui::Compositor& compositor, const Pixels& actual,
                    D2D1_RECT_F button, ui::command_icons::Icon icon,
                    D2D1_COLOR_F foreground, float scale, bool create, const ui::Theme& theme,
                    float artwork_size = 0, int artwork_resource = 0, bool tinted = false) {
    if (actual.bgra.empty()) return {};
    auto* dc = compositor.Dc();
    ui::ComPtr<ID2D1Image> previous;
    dc->GetTarget(&previous);
    ui::ComPtr<ID2D1Bitmap1> source;
    if (!previous.get() || FAILED(previous->QueryInterface(IID_PPV_ARGS(&source)))) return {};
    ui::ComPtr<ID2D1Bitmap1> reference;
    const auto properties = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET, source->GetPixelFormat(), 96, 96);
    if (FAILED(dc->CreateBitmap(source->GetPixelSize(), nullptr, 0, properties, &reference))) return {};
    ui::ComPtr<ID2D1SolidColorBrush> brush;
    ui::ComPtr<ID2D1StrokeStyle> stroke;
    if (FAILED(dc->CreateSolidColorBrush(foreground, &brush)) ||
        FAILED(ui::command_icons::CreateStrokeStyle(dc, &stroke))) return {};
    D2D1_MATRIX_3X2_F transform{}; dc->GetTransform(&transform);
    const auto antialias = dc->GetAntialiasMode(); const auto blend = dc->GetPrimitiveBlend();
    const auto background = Background(actual);
    const float cx = create ? button.left + 18 * scale : (button.left + button.right) / 2;
    const float cy = (button.top + button.bottom) / 2;
    const auto slot = D2D1::RectF(cx - 9 * scale, cy - 9 * scale,
        cx + 9 * scale, cy + 9 * scale);
    dc->SetTarget(reference.get()); dc->SetTransform(D2D1::Matrix3x2F::Identity());
    dc->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    dc->SetPrimitiveBlend(D2D1_PRIMITIVE_BLEND_SOURCE_OVER);
    dc->BeginDraw(); dc->Clear(D2D1::ColorF(background[2] / 255.0f, background[1] / 255.0f, background[0] / 255.0f));
    // Preserve the production viewport and world coordinates: hardware AA can change on a translated small target.
    // Brush selection is independent of VM state and the UI path.
    ui::fluent::Painter painter(&compositor);
    painter.BeginFrame(theme);
    bool drawn;
    if (artwork_resource) {
        const float size = std::round(artwork_size * scale);
        const float left = create ? std::floor(cx) + 0.5f - size / 2 : std::round(cx - size / 2);
        const float top = create ? std::floor(cy) + 0.5f - size / 2 : std::round(cy - size / 2);
        drawn = painter.DrawSvgResource(artwork_resource, {left, top, left + size, top + size},
                                        tinted ? &foreground : nullptr);
    } else {
        drawn = ui::toolbar_icons::Draw(painter, dc, brush.get(), stroke.get(), icon, slot, scale, 18.0f);
    }
    const HRESULT result = dc->EndDraw();
    Pixels pixels;
    if (drawn && SUCCEEDED(result)) pixels = ReadCrop(compositor,
        D2D1::RectF(static_cast<float>(actual.left), static_cast<float>(actual.top),
            static_cast<float>(actual.left + actual.width), static_cast<float>(actual.top + actual.height)));
    dc->SetTarget(previous.get()); dc->SetTransform(transform);
    dc->SetAntialiasMode(antialias); dc->SetPrimitiveBlend(blend);
    return pixels;
}
struct Ink {
    int left = 0, top = 0, right = 0, bottom = 0, core = 0, count = 0;
    float peak = 0;
    std::array<BYTE, 4> background{}, peak_pixel{};
    std::vector<bool> mask;
    std::vector<float> coverage;
    int Width() const { return right - left; }
    int Height() const { return bottom - top; }
};
bool HasMirrorSymmetry(const Pixels& pixels, const Ink& ink) {
    if (ink.count == 0) return false;
    float horizontal = 0, vertical = 0, total = 0;
    const auto coverage = [&](int x, int y) {
        return ink.coverage[static_cast<size_t>(y) * pixels.width + x];
    };
    for (int y = ink.top; y < ink.bottom; ++y) {
        for (int x = ink.left; x < ink.right; ++x) {
            const float sample = coverage(x, y);
            total += sample;
            horizontal += std::abs(sample - coverage(ink.left + ink.right - 1 - x, y));
            vertical += std::abs(sample - coverage(x, ink.top + ink.bottom - 1 - y));
        }
    }
    // Compare visible ink rather than treating a very faint edge as a solid pixel.
    return horizontal <= total * 0.12f && vertical <= total * 0.12f;
}
Ink Measure(const Pixels& pixels, D2D1_COLOR_F primary, D2D1_COLOR_F foreground) {
    Ink ink;
    if (pixels.bgra.empty()) return ink;
    ink.left = pixels.width; ink.top = pixels.height;
    ink.mask.resize(static_cast<size_t>(pixels.width) * pixels.height);
    ink.coverage.resize(ink.mask.size());
    ink.background = Background(pixels);
    const auto* background = ink.background.data();
    const float base[3]{static_cast<float>(background[0]), static_cast<float>(background[1]), static_cast<float>(background[2])};
    const float primary_rgb[3]{primary.b * 255, primary.g * 255, primary.r * 255};
    const float foreground_rgb[3]{foreground.b * 255, foreground.g * 255, foreground.r * 255};
    float denominator = 0;
    for (int c = 0; c < 3; ++c) denominator += (primary_rgb[c] - base[c]) * (primary_rgb[c] - base[c]);
    for (int y = 0; y < pixels.height; ++y) for (int x = 0; x < pixels.width; ++x) {
        const auto* pixel = pixels.At(x, y);
        float projection = 0; bool core = true;
        for (int c = 0; c < 3; ++c) {
            projection += (pixel[c] - base[c]) * (primary_rgb[c] - base[c]);
            const float expected = foreground_rgb[c] * foreground.a + base[c] * (1 - foreground.a);
            core &= std::abs(pixel[c] - expected) < 4;
        }
        const float alpha = denominator > 1 ? projection / denominator : 0;
        ink.coverage[static_cast<size_t>(y) * pixels.width + x] = std::clamp(alpha, 0.0f, 1.0f);
        if (alpha > ink.peak) {
            ink.peak = alpha;
            std::copy_n(pixel, 4, ink.peak_pixel.begin());
        }
        if (core) ++ink.core;
        if (alpha <= 0.07f) continue;
        ink.mask[static_cast<size_t>(y) * pixels.width + x] = true;
        ++ink.count;
        ink.left = std::min(ink.left, x); ink.top = std::min(ink.top, y);
        ink.right = std::max(ink.right, x + 1); ink.bottom = std::max(ink.bottom, y + 1);
    }
    return ink;
}
ui::HitTestResult::Region Region(int command) {
    using R = ui::HitTestResult;
    const auto found = std::find(ui::kLegacyToolbarCommands.begin(), ui::kLegacyToolbarCommands.end(), command);
    constexpr R::Region legacy[]{R::Cut, R::Copy, R::Paste, R::Rename, R::Delete, R::Properties,
        R::SplitButton, R::DetailsToggle, R::PaneColumnLayout};
    if (found != ui::kLegacyToolbarCommands.end()) return legacy[found - ui::kLegacyToolbarCommands.begin()];
    return command == 186 ? R::NewButton : R::CustomToolbarCommand;
}
int MiddleRuns(const Pixels& pixels, const Ink& ink) {
    if (ink.count == 0) return 0;
    const int y = (ink.top + ink.bottom) / 2;
    int runs = 0; bool previous = false;
    for (int x = ink.left; x < ink.right; ++x) {
        const bool current = ink.mask[static_cast<size_t>(y) * pixels.width + x];
        if (current && !previous) ++runs;
        previous = current;
    }
    return runs;
}
struct Tile { int command; std::wstring text; Pixels pixels; };
bool SaveOverview(ui::Compositor& compositor, const std::filesystem::path& path,
                  const std::vector<Tile>& tiles, float scale, const ui::Theme& theme) {
    constexpr int columns = 3;
    const int cell_width = static_cast<int>(std::round(320 * scale));
    const int row_height = static_cast<int>(std::round(42 * scale));
    const int heading = static_cast<int>(std::round(44 * scale));
    const int width = columns * cell_width;
    const int height = heading + static_cast<int>((tiles.size() + columns - 1) / columns) * row_height;
    compositor.Resize(width, height);
    auto* dc = compositor.Dc(); dc->BeginDraw(); dc->Clear(theme.bg);
    ui::fluent::Painter painter(&compositor); painter.SetScale(scale); painter.BeginFrame(theme);
    const auto caption = std::wstring(L"实际工具栏图标 / ") + (theme.text.r > 0.9f ? L"深色 " : L"浅色 ") +
        std::to_wstring(static_cast<int>(scale * 100)) + L"% / 原始像素 1:1";
    painter.DrawText(caption, D2D1::RectF(12 * scale, 0, static_cast<float>(width), static_cast<float>(heading)),
        compositor.HeaderFormat(), theme.text);
    bool ok = true;
    for (size_t i = 0; i < tiles.size(); ++i) {
        const auto& tile = tiles[i]; const auto& pixels = tile.pixels;
        if (pixels.bgra.empty()) { ok = false; continue; }
        ui::ComPtr<ID2D1Bitmap1> bitmap;
        const auto properties = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
        if (FAILED(dc->CreateBitmap(D2D1::SizeU(pixels.width, pixels.height), pixels.bgra.data(), pixels.width * 4,
            properties, &bitmap))) { ok = false; continue; }
        const float x = static_cast<float>(i % columns * cell_width) + std::round(10 * scale);
        const float y = static_cast<float>(heading + i / columns * row_height);
        const float top = y + std::floor((row_height - pixels.height) / 2.0f);
        dc->DrawBitmap(bitmap.get(), D2D1::RectF(x, top, x + pixels.width, top + pixels.height), 1,
            D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
        painter.DrawText(std::to_wstring(tile.command) + L"  " + tile.text,
            D2D1::RectF(x + pixels.width + 10 * scale, y,
                static_cast<float>((i % columns + 1) * cell_width) - 6 * scale, y + row_height),
            compositor.SmallFormat(), theme.text);
    }
    return SUCCEEDED(dc->EndDraw()) && ok && compositor.SaveSnapshot(path.c_str());
}
}

int main(int argc, char** argv) {
    using namespace pulse;
    const bool pane_tools_only = argc == 2 && std::string_view(argv[1]) == "--pane-tools";
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    OleInitialize(nullptr); l10n::Initialize(GetModuleHandleW(nullptr), L"zh-CN");
    const auto root = std::filesystem::absolute(pane_tools_only ? L"../bench_data/toolbar-pane-tools-artwork"
        : L"../bench_data/toolbar-catalog-artwork");
    std::filesystem::create_directories(root);
    std::ofstream metrics(root / L"metrics.csv");
    metrics << "command,dark,dpi,width,height,pixels,core,peak,hover_core,disabled_core,disabled_peak,negative_core,negative_peak,disabled_background,disabled_peak_pixel,paired_pixels,paired_incorrect,paired_max_error,negative_incorrect\n";
    HWND window = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"STATIC", L"Toolbar catalog icon fixture",
        WS_POPUP, -30000, -30000, 1200, 320, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    ui::Compositor compositor; ui::MainRenderer renderer;
    Check(window && compositor.Init(window), "initialize the production renderer without an application profile");
    if (!compositor.Dc()) return 2;
    renderer.SetCompositor(&compositor); renderer.SetDetailsPanelVisible(false);
    ui::ToolbarCustomizationSpec spec;
    for (const auto& command : app::GetCommandCatalog())
        spec.actions.push_back({command.command, l10n::Get(command.label), command.glyph, {}, command.pictogram});
    for (const bool dark : {true, false}) for (const float scale : {1.0f, 1.25f, 1.5f, 1.75f, 2.0f}) {
        const float width = 1200 * scale, height = 320 * scale;
        compositor.Resize(static_cast<UINT>(width), static_cast<UINT>(height));
        compositor.RecreateTextFormats(scale); renderer.SetScale(scale); renderer.InvalidateTypography();
        const auto bounds = D2D1::RectF(0, 0, width, height);
        const auto theme = ui::MakeTheme(dark, ui::HexColor(0x0078D4));
        Check(!dark || (theme.text.r == 1 && theme.text.g == 1 && theme.text.b == 1), "ordinary dark foreground is white");
        if (!pane_tools_only) {
            ui::WindowViewModel vm;
            vm.dark = dark; vm.safe_mode = true; vm.can_group = true; vm.can_paste = true;
            vm.pane.loading = true; vm.pane.selected_count = 1;
            vm.pane.view_mode = ui::ViewMode::Details;
            vm.toolbar_order.assign(ui::kDefaultToolbarOrder.begin(), ui::kDefaultToolbarOrder.end());
            renderer.SetToolbarOrder(vm.toolbar_order);
            auto* dc = compositor.Dc();
            dc->BeginDraw(); dc->Clear(D2D1::ColorF(0, 0)); renderer.Render(vm, bounds, theme);
            Check(SUCCEEDED(dc->EndDraw()), "render the four pane-header controls");
            const auto pane = renderer.ContentRect(width, height);
            const std::pair<D2D1_RECT_F, const char*> icons[] = {
                {renderer.PaneDetailsRect(pane), "details"},
                {renderer.PaneMediumIconsRect(pane), "grid"},
                {renderer.PaneSplitButtonRect(pane), "split"},
                {renderer.PaneViewButtonRect(pane), "more"},
            };
            for (const auto& [button, name] : icons) {
                const float cx = (button.left + button.right) / 2;
                const float cy = (button.top + button.bottom) / 2;
                const float sample_radius = 10 * scale;
                const auto sample = ReadCrop(compositor, {cx - sample_radius, cy - sample_radius,
                    cx + sample_radius, cy + sample_radius});
                const auto foreground = std::string_view(name) == "details" ? theme.accent_ink : theme.text;
                Check(HasMirrorSymmetry(sample, Measure(sample, foreground, foreground)),
                    std::string("pane ") + name + " is symmetric at " + std::to_string(static_cast<int>(100 * scale)) + "%");
            }
            if (scale == 1 || scale == 1.75f) {
                const auto filename = std::wstring(dark ? L"dark-" : L"light-") +
                    (scale == 1 ? L"toolbar.png" : L"175-toolbar.png");
                Check(compositor.SaveSnapshot((root / filename).c_str()), "save the toolbar and pane header at native resolution");
            }
        }
        std::map<int, Ink> normal_ink;
        std::vector<Tile> tiles, failure_tiles;
        for (const auto& descriptor : app::GetCommandCatalog()) {
            const int id = descriptor.command;
            if (pane_tools_only && id != app::CmdDisplayAlias && id != app::CmdTerminalPanel &&
                id != app::CmdOpenTerminal && id != app::CmdLinkedBrowsing &&
                id != app::CmdDirectorySync) continue;
            const auto artwork = ExpectedArtwork(id);
            const std::string label = "command " + std::to_string(id) + (dark ? " dark " : " light ") +
                std::to_string(static_cast<int>(scale * 100)) + "%";
            ui::ToolbarCustomizationModel model(spec);
            bool ok = true;
            const auto require = [&](bool condition, const char* detail) {
                if (!condition) std::printf("[DIAG] %s: %s\n", label.c_str(), detail);
                ok &= condition;
            };
            require(model.Add(id) && model.Order() == std::vector<int>{id}, "model Add/order");
            const auto icon = ui::toolbar_icons::Resolve(descriptor.glyph, descriptor.pictogram, id);
            const int rounded = artwork.outline ? 0 : ExpectedRoundedArtwork(icon);
            const bool center_divider = rounded == IDR_FILES_PANE_VERTICAL_SVG || rounded == IDR_FILES_PANE_HORIZONTAL_SVG ||
                rounded == IDR_FLUENT_BOOK_OPEN_SVG || rounded == IDR_FLUENT_TABLE_SIMPLE_SVG;
            const int canvas_pixels = static_cast<int>(std::lround(18 * scale));
            const int odd_pixels = (canvas_pixels - 1) | 1;
            const float artwork_size = rounded
                ? static_cast<float>(center_divider ? odd_pixels : canvas_pixels) / scale : artwork.size;
            require(icon != ui::command_icons::Icon::None, "Resolve must return a real icon");
            ui::WindowViewModel vm;
            vm.dark = dark; vm.safe_mode = true; vm.can_group = true; vm.can_paste = true;
            vm.pane.loading = true; vm.pane.selected_count = 1;
            vm.toolbar_order = model.Order();
            vm.toolbar_commands = {{id, l10n::Get(descriptor.label), descriptor.glyph, true, descriptor.pictogram}};
            vm.toolbar_commands_enabled = std::array<bool, 6>{true, true, true, true, true, true};
            renderer.SetToolbarOrder(vm.toolbar_order);
            const auto layout = ui::MakeToolbarLayout(width, scale, renderer.TitleBarHeight(), renderer.Margin(),
                renderer.NewButtonWidthPx(false), renderer.EffectiveSidebarWidth(width), 0, false, false, vm.toolbar_order);
            const auto found = std::find_if(layout.placed_commands.begin(), layout.placed_commands.end(),
                [=](const auto& item) { return item.command == id; });
            if (found == layout.placed_commands.end()) { Check(false, label + " is placed in the actual toolbar"); continue; }
            const auto button = found->bounds;
            const float cx = id == 186 ? button.left + 18 * scale : (button.left + button.right) / 2;
            const float cy = (button.top + button.bottom) / 2;
            const auto crop_bounds = D2D1::RectF(std::floor(cx - 12 * scale), std::floor(cy - 12 * scale),
                std::floor(cx - 12 * scale) + std::round(24 * scale), std::floor(cy - 12 * scale) + std::round(24 * scale));
            const auto render = [&](const ui::Theme& current_theme) {
                auto* dc = compositor.Dc(); dc->BeginDraw(); dc->Clear(D2D1::ColorF(0, 0));
                renderer.Render(vm, bounds, current_theme);
                return SUCCEEDED(dc->EndDraw());
            };
            const auto hit = [&]() { return renderer.HitTest(vm, bounds, cx, cy); };
            require(render(theme), "normal render EndDraw");
            const auto pixels = ReadCrop(compositor, crop_bounds);
            const auto ink = Measure(pixels, theme.text, theme.text);
            normal_ink[id] = ink;
            require(ink.count > 3, "normal ink pixel count > 3");
            if (id == app::CmdLibraries || id == app::CmdLayoutFourGrid)
                require(HasMirrorSymmetry(pixels, ink), "the two book pages or four panes are visually symmetric");
            const auto artwork_matches = [&](const Pixels& actual, int resource, D2D1_COLOR_F foreground, bool tinted) {
                const auto expected = ExpectedIcon(compositor, actual, button, icon,
                    foreground, scale, id == 186, theme, artwork_size, resource, tinted);
                const auto match = CompareReference(actual, expected);
                return match.compared > 0 && match.incorrect == 0;
            };
            const bool tinted_artwork = rounded || (artwork.outline && !artwork.light);
            const int normal_resource = rounded ? rounded
                : tinted_artwork ? artwork.outline : dark ? artwork.dark : artwork.light;
            if (normal_resource) {
                require(artwork_matches(pixels, normal_resource, theme.text, tinted_artwork), "normal renders the expected path and foreground");
            } else {
                require(ink.core > 1, "normal exact primary core > 1");
                require(ink.peak > 0.95f, "normal primary peak > 0.95");
            }
            const float side = std::round(18 * scale);
            const float slot_left = std::round(cx - side / 2), slot_top = std::round(cy - side / 2);
            require(ink.left + pixels.left >= slot_left - 1 && ink.right + pixels.left <= slot_left + side + 1 &&
                ink.top + pixels.top >= slot_top - 1 && ink.bottom + pixels.top <= slot_top + side + 1,
                "normal ink stays within the 18 DIP slot plus 1 physical pixel");
            const auto normal_hit = hit();
            require(normal_hit.region == Region(id) && (Region(id) != ui::HitTestResult::CustomToolbarCommand || normal_hit.index == id),
                "normal hit region and command ID");
            if (scale == 1 || scale == 1.75f) tiles.push_back({id, l10n::Get(descriptor.label), pixels});
            vm.hover_region = static_cast<int>(Region(id)); vm.hover_control_index = id;
            require(render(theme), "hover render EndDraw");
            const auto hover_pixels = ReadCrop(compositor, crop_bounds);
            const auto hover_ink = Measure(hover_pixels, theme.text, theme.text);
            if (normal_resource) {
                require(artwork_matches(hover_pixels, normal_resource, theme.text, tinted_artwork), "hover retains the expected path and foreground");
            } else {
                require(hover_ink.core > 1, "hover exact primary core > 1");
                require(hover_ink.peak > 0.95f, "hover primary peak > 0.95");
            }
            require(hit().region == Region(id), "hover hit region");
            if (artwork.outline && dark && scale == 1) {
                ui::HighContrastFrameScope contrast(true);
                auto hc = ui::MakeHighContrastTheme();
                hc.fill_hover = ui::HexColor(0xFFFFFF);
                hc.text_selected = ui::HexColor(0x000000);
                require(render(hc), "high-contrast hovered render EndDraw");
                require(artwork_matches(ReadCrop(compositor, crop_bounds), artwork.outline, hc.text_selected, true),
                    "high-contrast hover draws the original outline with HighlightText");
            }
            Ink disabled, negative_ink;
            PairedColor paired, negative_paired;
            Pixels disabled_pixels, negative_pixels, reference_pixels;
            if (id != 306 && id != 101 && id != 181) {
                vm.hover_region = 0; vm.hover_control_index = -1;
                vm.toolbar_commands.front().enabled = false; vm.pane.selected_count = 0; vm.can_paste = false;
                vm.toolbar_commands_enabled = std::array<bool, 6>{false, false, false, false, false, false};
                require(render(theme), "disabled render EndDraw");
                disabled_pixels = ReadCrop(compositor, crop_bounds);
                disabled = Measure(disabled_pixels, theme.text, theme.text_disabled);
                require(disabled.count > 0, "disabled ink count > 0");
                reference_pixels = ExpectedIcon(compositor, disabled_pixels, button, icon,
                    theme.text_disabled, scale, id == 186, theme, artwork_size, rounded ? rounded : artwork.outline, true);
                paired = CompareReference(disabled_pixels, reference_pixels);
                require(paired.compared > 0 && paired.incorrect == 0,
                    "disabled RGB matches an independently selected expected brush on the same GPU within the original 4 RGB tolerance");
                if (!normal_resource) require(disabled.peak < ink.peak - 0.15f, "disabled peak dims by more than 0.15");
                require(hit().region != Region(id), "disabled command rejects hit");
                auto incorrect_disabled_theme = theme;
                incorrect_disabled_theme.text_disabled = theme.text;
                require(render(incorrect_disabled_theme), "disabled negative control render EndDraw");
                negative_pixels = ReadCrop(compositor, crop_bounds);
                negative_ink = Measure(negative_pixels, theme.text, theme.text_disabled);
                negative_paired = CompareReference(negative_pixels, reference_pixels);
                require(negative_paired.compared > 0 && negative_paired.incorrect > 0,
                    "disabled negative control: independent expected-brush reference rejects the wrong primary foreground");
                if (!normal_resource) require(!(negative_ink.peak < ink.peak - 0.15f),
                    "disabled negative control: an incorrectly primary-colored disabled icon must fail the dimming predicate");
            }
            if (id == 181) {
                require(icon == ui::command_icons::Icon::Columns && MiddleRuns(pixels, ink) >= 4,
                    "columns resolves correctly and contains four vertical ink runs");
            }
            metrics << id << ',' << dark << ',' << static_cast<int>(scale * 100) << ',' << ink.Width() << ',' << ink.Height() << ','
                << ink.count << ',' << ink.core << ',' << ink.peak << ',' << hover_ink.core << ',' << disabled.core << ',' << disabled.peak << ','
                << negative_ink.core << ',' << negative_ink.peak << ',' << static_cast<int>(disabled.background[0]) << ','
                << static_cast<int>(disabled.peak_pixel[0]) << ',' << paired.compared << ',' << paired.incorrect << ','
                << paired.max_error << ',' << negative_paired.incorrect << '\n';
            if (!ok) {
                std::printf("[DIAG] %s: normal core=%d peak=%.6f; hover core=%d peak=%.6f; disabled core=%d peak=%.6f background=(%u,%u,%u) peak_rgb=(%u,%u,%u); negative core=%d peak=%.6f\n",
                    label.c_str(), ink.core, ink.peak, hover_ink.core, hover_ink.peak, disabled.core, disabled.peak,
                    static_cast<unsigned>(disabled.background[2]), static_cast<unsigned>(disabled.background[1]), static_cast<unsigned>(disabled.background[0]),
                    static_cast<unsigned>(disabled.peak_pixel[2]), static_cast<unsigned>(disabled.peak_pixel[1]), static_cast<unsigned>(disabled.peak_pixel[0]),
                    negative_ink.core, negative_ink.peak);
                std::printf("[DIAG] %s: paired pixels=%d incorrect=%d max_error=%.6f negative_incorrect=%d\n",
                    label.c_str(), paired.compared, paired.incorrect, paired.max_error, negative_paired.incorrect);
                failure_tiles.push_back({id, L"normal", pixels});
                failure_tiles.push_back({id, L"hover", hover_pixels});
                if (!disabled_pixels.bgra.empty()) failure_tiles.push_back({id, L"disabled", disabled_pixels});
                if (!reference_pixels.bgra.empty()) failure_tiles.push_back({id, L"independent expected brush", reference_pixels});
                if (!negative_pixels.bgra.empty()) failure_tiles.push_back({id, L"incorrect primary disabled", negative_pixels});
            }
            Check(ok, label + " resolves, renders within 18 DIP, retains its artwork on hover and dims when disabled");
        }
        const auto split = normal_ink.find(306), large = normal_ink.find(81);
        if (!pane_tools_only) Check(split != normal_ink.end() && large != normal_ink.end() &&
            std::abs(split->second.Width() - large->second.Width()) <= std::ceil(2 * scale) &&
            std::abs(split->second.Height() - large->second.Height()) <= std::ceil(2 * scale),
            "actual split and large-icon ink have comparable physical dimensions at this theme and DPI");
        if (split != normal_ink.end()) {
            for (const auto& [id, ink] : normal_ink) if (ExpectedArtwork(id).light) {
                const int dimension = std::max(ink.Width(), ink.Height());
                const int split_dimension = std::max(split->second.Width(), split->second.Height());
                Check(std::abs(dimension - split_dimension) <= std::ceil(3 * scale),
                    "restored Files artwork keeps comparable visible dimensions to the pane icon");
            }
        }
        if (!failure_tiles.empty()) {
            const auto file = std::wstring(dark ? L"dark-" : L"light-") + std::to_wstring(static_cast<int>(scale * 100)) + L"-failed-states.png";
            Check(SaveOverview(compositor, root / file, failure_tiles, scale, theme), "save failed normal, hover, disabled and negative-control crops without rescaling");
        }
        if (!tiles.empty()) {
            const auto file = std::wstring(dark ? L"dark-" : L"light-") + std::to_wstring(static_cast<int>(scale * 100)) + L"-catalog.png";
            Check(SaveOverview(compositor, root / file, tiles, scale, theme), "save named catalog overview using actual unscaled toolbar pixel crops");
        }
    }
    renderer.SetCompositor(nullptr); compositor.Shutdown(); DestroyWindow(window); OleUninitialize();
    std::printf("%d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
