#include "../ui/ui_renderer.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <wincodec.h>

using namespace pulse::ui;

namespace {
struct Image {
    UINT width = 0, height = 0;
    std::vector<BYTE> pixels;
};

Image ReadImage(const std::filesystem::path& path) {
    Image image;
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&factory))) ||
        FAILED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
            WICDecodeMetadataCacheOnLoad, &decoder)) ||
        FAILED(decoder->GetFrame(0, &frame)) || FAILED(factory->CreateFormatConverter(&converter)) ||
        FAILED(converter->Initialize(frame.get(), GUID_WICPixelFormat32bppBGRA,
            WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)) ||
        FAILED(converter->GetSize(&image.width, &image.height))) return {};
    image.pixels.resize(static_cast<size_t>(image.width) * image.height * 4);
    if (FAILED(converter->CopyPixels(nullptr, image.width * 4,
            static_cast<UINT>(image.pixels.size()), image.pixels.data()))) return {};
    return image;
}

size_t Changed(const Image& a, const Image& b, D2D1_RECT_F rect) {
    if (a.pixels.empty() || a.width != b.width || a.height != b.height) return SIZE_MAX;
    size_t changed = 0;
    for (int y = std::max(0, static_cast<int>(std::ceil(rect.top)));
         y < std::min(static_cast<int>(a.height), static_cast<int>(rect.bottom)); ++y) {
        for (int x = std::max(0, static_cast<int>(std::ceil(rect.left)));
             x < std::min(static_cast<int>(a.width), static_cast<int>(rect.right)); ++x) {
            const auto offset = (static_cast<size_t>(y) * a.width + x) * 4;
            if (!std::equal(a.pixels.begin() + offset, a.pixels.begin() + offset + 4,
                    b.pixels.begin() + offset)) ++changed;
        }
    }
    return changed;
}
}

int main() {
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return 2;
    int failures = 0;
    const auto check = [&](bool ok, const char* message) {
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", message);
        if (!ok) ++failures;
    };
    wchar_t executable[MAX_PATH]{};
    GetModuleFileNameW(nullptr, executable, MAX_PATH);
    const auto output = std::filesystem::path(executable).parent_path().parent_path() /
        L"bench_data" / L"hidden-file-appearance";
    std::filesystem::create_directories(output);
    const auto hwnd = CreateWindowExW(WS_EX_NOACTIVATE, L"STATIC", L"Hidden appearance fixture",
        WS_OVERLAPPEDWINDOW, -30000, -30000, 1400, 1000, nullptr, nullptr, nullptr, nullptr);
    {
        Compositor compositor;
        if (!hwnd || !compositor.Init(hwnd)) return 2;
        MainRenderer renderer;
        renderer.SetCompositor(&compositor);
        for (float scale : {1.0f, 1.5f}) for (bool dark : {false, true}) {
            compositor.Resize(static_cast<UINT>(1100 * scale), static_cast<UINT>(740 * scale));
            compositor.RecreateTextFormats(scale);
            renderer.SetScale(scale);
            const auto bounds = D2D1::RectF(0, 0, 1100 * scale, 740 * scale);
            const auto theme = MakeTheme(dark, HexColor(0x0078D4));
            auto* dc = compositor.Dc();
            dc->BeginDraw();
            dc->Clear(HexColor(0xD050B0));
            if (FAILED(dc->EndDraw()) || !compositor.SaveSnapshot((output / L"thumbnail.png").c_str())) return 2;
            for (int mode = 0; mode < 10; ++mode) {
                std::printf("[CASE] scale=%.1f dark=%d mode=%d\n", scale, dark, mode);
                WindowViewModel vm;
                vm.window_effect = WindowEffect::None;
                vm.dark = dark;
                vm.tabs.push_back({L"Hidden appearance", true});
                PaneSlotView slot;
                slot.rect = D2D1::RectF(220 * scale, 94 * scale, 1092 * scale, 700 * scale);
                slot.focused = true;
                auto& pane = slot.pane;
                pane.header_controls_opacity = 0;
                pane.view_mode = mode < 8 ? ViewModeFromIndex(mode) :
                    mode == 9 ? ViewMode::LargeIcons : ViewMode::Details;
                pane.selected_index = 0;
                pane.selected_count = 1;
                pane.row_action_mask = 0;
                ListEntryView entry;
                entry.name = L"Sample.txt";
                if (mode == 9) {
                    entry.name = L"thumbnail.png";
                    entry.path = (output / entry.name).wstring();
                    entry.size_value = std::filesystem::file_size(entry.path);
                }
                pane.entries.push_back(entry);
                auto column_entries = std::make_shared<std::vector<pulse::fs::DirEntry>>(1);
                column_entries->front().name = entry.name;
                if (mode == 8) {
                    pane.column_strip.enabled = pane.column_strip.eligible = true;
                    ColumnStripColumnView column;
                    column.title = L"Ancestor";
                    column.snapshot = column_entries;
                    column.rows = std::make_shared<std::vector<int>>(1, 0);
                    column.highlight_row = 0;
                    pane.column_strip.ancestors.push_back(column);
                }
                vm.pane_slots.push_back(std::move(slot));
                auto& view = vm.pane_slots.front();
                const auto body = renderer.PaneBodyBounds(view.pane, view.rect);
                const auto list = renderer.PaneListRect(view.pane, body);
                ViewLayout layout(view.pane.view_mode, list, 1, 0, 0, scale,
                    renderer.ListRowHeightDip(view.pane, list));
                auto icon = layout.IconRect(0), name = layout.NameRect(0), cell = layout.ItemRect(0);
                if (mode == 8) {
                    cell = renderer.ColumnStripGeometry(view.pane, view.rect).columns.front().rect;
                    cell.top += 28 * scale;
                    cell.bottom = cell.top + renderer.RowHeight();
                    icon = D2D1::RectF(cell.left + 14 * scale, cell.top,
                        cell.left + 30 * scale, cell.bottom);
                    name = D2D1::RectF(cell.left + 38 * scale, cell.top, cell.right - 10 * scale, cell.bottom);
                }
                const auto capture = [&](DWORD attrs, const wchar_t* label, bool cut = false) {
                    view.pane.entries.front().attrs = attrs;
                    column_entries->front().attrs = attrs;
                    view.pane.cut_names.clear();
                    if (cut) view.pane.cut_names.insert(entry.name);
                    dc->BeginDraw();
                    dc->Clear(theme.bg);
                    renderer.Render(vm, bounds, theme);
                    if (FAILED(dc->EndDraw())) return Image{};
                    const auto path = output / (std::to_wstring(static_cast<int>(scale * 100)) +
                        (dark ? L"-dark-" : L"-light-") + std::to_wstring(mode) + L"-" + label + L".png");
                    if (!compositor.SaveSnapshot(path.c_str())) return Image{};
                    return ReadImage(path);
                };
                // Let the generic Shell icon request settle before comparing attributes.
                for (int i = 0; i < 4; ++i) { capture(0, L"warmup"); Sleep(50); }
                if (mode == 9) {
                    bool thumbnail_ready = false;
                    for (int i = 0; i < 60 && !thumbnail_ready; ++i) {
                        const auto image = capture(0, L"warmup");
                        if (!image.pixels.empty()) {
                            const auto offset = (static_cast<size_t>((icon.top + icon.bottom) / 2) * image.width +
                                static_cast<size_t>((icon.left + icon.right) / 2)) * 4;
                            thumbnail_ready = image.pixels[offset] == 0xB0 &&
                                image.pixels[offset + 1] == 0x50 && image.pixels[offset + 2] == 0xD0;
                        }
                        if (!thumbnail_ready) Sleep(50);
                    }
                    check(thumbnail_ready, "actual image thumbnail is ready before opacity comparison");
                }
                const auto normal = capture(0, L"normal");
                const auto hidden = capture(FILE_ATTRIBUTE_HIDDEN, L"hidden");
                const auto system = capture(FILE_ATTRIBUTE_SYSTEM, L"system");
                const auto both = capture(FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM, L"protected");
                check(!normal.pixels.empty() && !hidden.pixels.empty(), "renderer captures normal and hidden items");
                const auto icon_changes = Changed(normal, hidden, icon);
                const auto name_changes = Changed(normal, hidden, name);
                check(icon_changes != SIZE_MAX && icon_changes > 5, "hidden icon is visibly translucent");
                check(name_changes != SIZE_MAX && name_changes > 5, "hidden filename is visibly translucent");
                check(Changed(hidden, system, cell) == 0 && Changed(hidden, both, cell) == 0,
                    "system and protected items receive the same opacity without stacking");
                const auto background = D2D1::RectF(cell.right - 14 * scale, cell.top + 3 * scale,
                    cell.right - 10 * scale, cell.top + 4 * scale);
                check(Changed(normal, hidden, background) == 0, "selection background keeps its opacity");
                if (mode != 8) {
                    const auto cut = capture(0, L"cut", true);
                    const auto hidden_cut = capture(FILE_ATTRIBUTE_HIDDEN, L"hidden-cut", true);
                    check(Changed(cut, hidden_cut, name) == 0, "hidden cut filename is not dimmed twice");
                }
            }
        }
    }
    DestroyWindow(hwnd);
    CoUninitialize();
    return failures ? 1 : 0;
}
