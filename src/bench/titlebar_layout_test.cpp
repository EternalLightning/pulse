#include "../ui/ui_renderer.h"
#include "../ui/ui_compositor.h"
#include "../ui/text_hit_ranges.h"
#include "../common/localization.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <vector>

namespace {
int failures = 0;
void Check(bool ok, const char* text) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", text);
    if (!ok) ++failures;
}
pulse::ui::WindowViewModel MakeView(int count, bool grouped = false, bool named_pins = false) {
    pulse::ui::WindowViewModel vm;
    vm.show_pinned_tab_names = named_pins;
    for (int i = 0; i < count; ++i) {
        pulse::ui::TabView tab;
        tab.title = L"A long folder tab " + std::to_wstring(i + 1);
        tab.active = i == count - 1;
        tab.is_home = i % 3 == 0;
        tab.pinned = grouped && i < 3;
        if (grouped && i >= 3 && i < 11) { tab.group = 0; tab.color_rgb = 0x1685E8; }
        if (grouped && i >= 14 && i < 19) {
            tab.group = 1; tab.color_rgb = 0xA54FD2; tab.hidden = true;
        }
        vm.tabs.push_back(std::move(tab));
    }
    if (grouped) {
        pulse::ui::TabGroupView group;
        group.id = 1; group.name = L"Project files"; group.color_rgb = 0x1685E8;
        vm.tab_groups.push_back(group);
        group.id = 2; group.name = L"Collapsed group"; group.color_rgb = 0xA54FD2; group.collapsed = true;
        vm.tab_groups.push_back(group);
    }
    return vm;
}
bool CaptionHits(pulse::ui::MainRenderer& renderer, const pulse::ui::WindowViewModel& vm,
                 const D2D1_RECT_F& bounds, float scale) {
    using Hit = pulse::ui::HitTestResult;
    const Hit::Region regions[] = {Hit::Minimize, Hit::Maximize, Hit::Close};
    const float left = bounds.right - 138.0f * scale;
    for (int button = 0; button < 3; ++button)
        for (const float y : {1.0f, renderer.TitleBarHeight() * 0.5f, renderer.TitleBarHeight() - 1.0f})
            for (int sample = 0; sample < 5; ++sample) {
                const float x = left + (46.0f * button + 1.0f + 9.0f * sample) * scale;
                if (renderer.HitTest(vm, bounds, x, y).region != regions[button]) return false;
            }
    return true;
}
bool ActiveFits(pulse::ui::MainRenderer& renderer, const pulse::ui::WindowViewModel& vm,
                float width) {
    const auto viewport = renderer.TabStripViewportRect(vm, width);
    for (int i = 0; i < static_cast<int>(vm.tabs.size()); ++i) {
        if (!vm.tabs[static_cast<size_t>(i)].active || vm.tabs[static_cast<size_t>(i)].hidden) continue;
        D2D1_RECT_F tab{};
        return renderer.TabItemRect(vm, width, i, &tab) && tab.left >= viewport.left - 0.1f &&
            tab.right <= viewport.right + 0.1f;
    }
    return true;
}
void GeometryCases(pulse::ui::MainRenderer& renderer) {
    using Hit = pulse::ui::HitTestResult;
    for (const float scale : {1.0f, 1.25f, 1.5f}) {
        renderer.SetScale(scale);
        for (const float width : {240.0f, 480.0f, 640.0f, 1280.0f}) {
            const auto bounds = D2D1::RectF(0, 0, width * scale, 600.0f * scale);
            for (const int count : {1, 6, 24, 72}) {
                auto vm = MakeView(count, count >= 24, count == 72);
                Check(CaptionHits(renderer, vm, bounds, scale), "caption areas win at narrow/wide widths and every DPI");
                Check(ActiveFits(renderer, vm, bounds.right), "active tab fits the shared viewport after a layout change");
                const auto viewport = renderer.TabStripViewportRect(vm, bounds.right);
                Check(viewport.right <= bounds.right - 146.0f * scale + 0.1f &&
                    viewport.left <= viewport.right, "tab viewport stays outside the fixed caption budget");
                const auto active_tab = static_cast<int>(vm.tabs.size()) - 1;
                D2D1_RECT_F active{};
                Check(renderer.TabItemRect(vm, bounds.right, active_tab, &active) &&
                    renderer.HitTest(vm, bounds, (active.left + active.right) * 0.5f,
                        renderer.TitleBarHeight() * 0.5f).index == active_tab,
                    "active tab remains directly hit-testable");
                std::vector<bool> seen(vm.tabs.size(), false);
                std::vector<bool> groups_seen(vm.tab_groups.size(), false);
                auto record_visible = [&] {
                    const auto clip = renderer.TabStripViewportRect(vm, bounds.right);
                    for (int i = 0; i < static_cast<int>(vm.tabs.size()); ++i) {
                        D2D1_RECT_F tab{};
                        if (!renderer.TabItemRect(vm, bounds.right, i, &tab)) continue;
                        const float left = std::max(tab.left, clip.left), right = std::min(tab.right, clip.right);
                        if (right - left < scale) continue;
                        const auto hit = renderer.HitTest(vm, bounds, (left + right) * 0.5f,
                            renderer.TitleBarHeight() * 0.5f);
                        if ((hit.region == Hit::Tab || hit.region == Hit::TabClose) && hit.index == i)
                            seen[static_cast<size_t>(i)] = true;
                    }
                    for (int g = 0; g < static_cast<int>(vm.tab_groups.size()); ++g) {
                        D2D1_RECT_F chip{};
                        if (!renderer.TabGroupChipRect(vm, bounds.right, g, &chip)) continue;
                        const float left = std::max(chip.left, clip.left), right = std::min(chip.right, clip.right);
                        if (right - left < scale) continue;
                        const auto hit = renderer.HitTest(vm, bounds, (left + right) * 0.5f,
                            (chip.top + chip.bottom) * 0.5f);
                        if (hit.region == Hit::TabGroup && hit.index == g) groups_seen[static_cast<size_t>(g)] = true;
                    }
                };
                record_visible();
                for (int step = 0; step < 300 && renderer.ScrollTabStrip(vm, bounds.right, -1); ++step)
                    record_visible();
                for (int step = 0; step < 300 && renderer.ScrollTabStrip(vm, bounds.right, 1); ++step)
                    record_visible();
                bool all_seen = true;
                for (size_t i = 0; i < seen.size(); ++i) if (!vm.tabs[i].hidden && !seen[i]) all_seen = false;
                Check(all_seen, "scrolling reaches every ordinary and pinned tab");
                Check(std::all_of(groups_seen.begin(), groups_seen.end(), [](bool seen_group) { return seen_group; }),
                    "scrolling reaches expanded and collapsed group chips");
                int add_hits = 0;
                for (int x = 0; x < static_cast<int>(bounds.right); ++x)
                    if (renderer.HitTest(vm, bounds, static_cast<float>(x), renderer.TitleBarHeight() * 0.5f).region == Hit::TabNew)
                        ++add_hits;
                Check(add_hits >= static_cast<int>(31.0f * scale), "new-tab button keeps its own clickable budget");
                for (auto& tab : vm.tabs) tab.active = false;
                vm.tabs[0].active = true;
                Check(ActiveFits(renderer, vm, bounds.right), "switching to the first pinned or ordinary tab reveals it");
                vm.tab_drag_index = 0; vm.tab_drag_count = std::min(3, count);
                vm.tab_drag_x = bounds.right - 120.0f * scale;
                Check(CaptionHits(renderer, vm, bounds, scale), "floating drag runs cannot steal caption hits");
                Check(!renderer.ScrollTabStrip(vm, bounds.right, 1), "scrolling does not move the drag coordinate system");
                vm.tab_drag_index = -1;
                vm.tabs[0].x_offset = 1000.0f;
                Check(CaptionHits(renderer, vm, bounds, scale), "animated sibling positions cannot steal caption hits");
            }
        }
    }
    renderer.SetScale(1.0f);
    auto wheel_vm = MakeView(24, true);
    const float wheel_width = 480.0f;
    renderer.ScrollTabStrip(wheel_vm, wheel_width, -1000.0f);
    D2D1_RECT_F before{}, after{};
    renderer.TabItemRect(wheel_vm, wheel_width, 0, &before);
    const auto wheel_viewport = renderer.TabStripViewportRect(wheel_vm, wheel_width);
    const float step = std::max(1.0f, (wheel_viewport.right - wheel_viewport.left) * 0.75f);
    const bool fraction_moved = renderer.ScrollTabStrip(wheel_vm, wheel_width, 0.125f);
    renderer.TabItemRect(wheel_vm, wheel_width, 0, &after);
    Check(fraction_moved && std::abs(before.left - after.left - step * 0.125f) < 0.1f,
        "partial wheel deltas move the strip proportionally without requiring a full notch");
    Check(!renderer.ScrollTabStrip(wheel_vm, wheel_width, 0.0f),
        "a zero wheel delta cannot move the strip");
}
}

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    using namespace pulse;
    if (argc == 2 && std::strcmp(argv[1], "--preview-text-ranges") == 0) {
        ui::ComPtr<IDWriteFactory> factory;
        const HRESULT created = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
            __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(&factory));
        Check(SUCCEEDED(created), "create DirectWrite factory for text hit range regression");
        if (FAILED(created)) return 1;
        ui::ComPtr<IDWriteTextFormat> format;
        factory->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 14.0f, L"en-US", &format);
        Check(format.get() != nullptr, "create hit range font");
        if (!format.get()) return 1;
        std::wstring text;
        for (int line = 0; line < 140; ++line) text += L"line\n";
        ui::ComPtr<IDWriteTextLayout> layout;
        factory->CreateTextLayout(text.data(), static_cast<UINT32>(text.size()), format.get(),
            300.0f, 10000.0f, &layout);
        Check(layout.get() != nullptr, "create multiline hit range layout");
        if (!layout.get()) return 1;
        const auto ranges = ui::TextHitRanges(layout.get(), 0, static_cast<UINT32>(text.size()), 7, 11);
        Check(ranges.size() > 96 && ranges.back().textPosition >= 139 * 5 &&
            ranges.back().top > ranges.front().top,
            "selection spanning more than 96 lines retains final line highlight");
        Check(ui::TextHitRanges(layout.get(), 0, 4, 0, 0).size() == 1,
            "short selection retains single hit range");
        Check(ui::TextHitRanges(layout.get(), 0, 0, 0, 0).empty() &&
            ui::TextHitRanges(nullptr, 0, 4, 0, 0).empty(), "empty and missing layout are safe");
        return failures ? 1 : 0;
    }
    OleInitialize(nullptr);
    l10n::Initialize(GetModuleHandleW(nullptr), L"zh-CN");
    ui::MainRenderer renderer;
    if (argc == 2 && std::strcmp(argv[1], "--details-panel") == 0) {
        renderer.SetDetailsPanelVisible(true);
        renderer.SetSidebarWidthDip(224.0f);
        renderer.SetDetailsPanelWidth(900.0f);
        for (const float scale : {1.0f, 1.25f, 1.5f, 2.0f}) {
            renderer.SetScale(scale);
            for (const float width : {1600.0f, 1000.0f, 1600.0f}) {
                const float w = width * scale, h = 720.0f * scale;
                const auto panel = renderer.DetailsPanelRect(w, h);
                const auto content = renderer.ContentRect(w, h);
                Check(std::abs(panel.left - content.right - renderer.Margin()) < 0.01f,
                    "details drawing and hit bounds match the width reserved beside the list");
                Check(std::abs((panel.right - panel.left) / scale -
                    (width == 1000.0f ? renderer.DetailsMaxWidthDip(w) : 900.0f)) < 0.01f,
                    "narrowing caps details width and widening restores the preferred width");
            }
            Check(renderer.DetailsPanelWidth(999.0f * scale) == 0.0f &&
                renderer.DetailsPanelRect(999.0f * scale, 720.0f * scale).right == 0.0f,
                "details panel and reserved space disappear together below the visibility threshold");
            ui::WindowViewModel vm;
            vm.details.has_selection = true;
            vm.details.multi_count = 1;
            vm.details.preview_enabled = false;
            vm.details.preview_expansion = 0.0f;
            const float off = renderer.DetailsContentHeightDip(vm, 1600 * scale, 720 * scale);
            vm.details.preview_expansion = 1.0f;
            Check(off > 0.0f && std::abs(off -
                renderer.DetailsContentHeightDip(vm, 1600 * scale, 720 * scale)) < 0.01f,
                "disabled preview contributes no height regardless of saved expansion");
            vm.details.preview_enabled = true;
            Check(renderer.DetailsContentHeightDip(vm, 1600 * scale, 720 * scale) > off,
                "enabling preview restores its contribution to scroll content height");
        }
        OleUninitialize();
        return failures == 0 ? 0 : 1;
    }
    GeometryCases(renderer);
    if (argc == 2 && std::strcmp(argv[1], "--geometry-only") == 0) {
        std::printf("titlebar geometry: %d failures\n", failures);
        OleUninitialize();
        return failures == 0 ? 0 : 1;
    }
    Check(true, "geometry cases completed; initializing the visual compositor");
    HWND window = CreateWindowExW(WS_EX_NOACTIVATE, L"STATIC", L"titlebar fixture", WS_OVERLAPPEDWINDOW,
        -30000, -30000, 960, 600, nullptr, nullptr, nullptr, nullptr);
    ui::Compositor compositor;
    if (!window || !compositor.Init(window)) { Check(false, "visual fixture initializes compositor"); return 2; }
    renderer.SetCompositor(&compositor);
    GeometryCases(renderer);
    const auto output = std::filesystem::absolute(L"bench_data/titlebar-layout");
    std::filesystem::create_directories(output);
    for (const float scale : {1.0f, 1.25f, 1.5f}) {
        renderer.SetScale(scale);
        compositor.RecreateTextFormats(scale);
        for (const float width : {480.0f, 960.0f}) {
            compositor.Resize(static_cast<UINT>(width * scale), static_cast<UINT>(420.0f * scale));
            const auto bounds = D2D1::RectF(0, 0, width * scale, 420.0f * scale);
            for (const bool dark : {false, true}) {
                auto vm = MakeView(24, true);
                vm.dark = dark;
                for (const int active : {1, 7, 23}) {
                    for (auto& tab : vm.tabs) tab.active = false;
                    vm.tabs[static_cast<size_t>(active)].active = true;
                    Check(ActiveFits(renderer, vm, bounds.right), "visual example keeps active pinned/group/last tab visible");
                    compositor.Dc()->BeginDraw();
                    renderer.Render(vm, bounds, ui::MakeTheme(dark, ui::HexColor(0x0078D4)));
                    Check(SUCCEEDED(compositor.Dc()->EndDraw()), "titlebar example renders directly to the snapshot buffer");
                    const auto name = std::to_wstring(static_cast<int>(width)) + L"-" +
                        std::to_wstring(static_cast<int>(scale * 100)) + (dark ? L"-dark-" : L"-light-") +
                        std::to_wstring(active) + L".png";
                    Check(compositor.SaveSnapshot((output / name).c_str()), "titlebar visual evidence saved before Present");
                }
            }
        }
    }
    renderer.SetCompositor(nullptr);
    compositor.Shutdown();
    DestroyWindow(window);
    OleUninitialize();
    std::printf("titlebar layout: %d failures\n", failures);
    return failures == 0 ? 0 : 1;
}
