// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#ifdef PULSE_WITH_SELFTEST
#include "../ui/ui_renderer_internal.h"
#include "../common/windows_compat.h"
#include <filesystem>
#include <fstream>

namespace pulse::ui {
struct ColumnResizeUiTest {
    static bool Run() {
        const std::filesystem::path root = L"bench_data/column-resize";
        std::filesystem::create_directories(root);
        std::ofstream log(root / L"results.log");
        bool ok = true;
        auto check = [&](bool pass, const char* message) {
            log << (pass ? "[PASS] " : "[FAIL] ") << message << std::endl; ok &= pass;
        };
        HWND hwnd = CreateWindowExW(0, L"STATIC", L"Column resize", WS_POPUP,
            0, 0, 1200, 700, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        {
            Compositor compositor;
            if (!hwnd || !compositor.Init(hwnd)) { if (hwnd) DestroyWindow(hwnd); return false; }
            MainRenderer renderer; renderer.SetCompositor(&compositor);
            for (float scale : {1.0f, 1.5f, 2.0f}) for (bool dark : {false, true}) for (int width : {540, 760}) {
                renderer.SetScale(scale); compositor.RecreateTextFormats(scale);
                compositor.Resize(static_cast<UINT>(width*scale), static_cast<UINT>(700*scale));
                const auto theme = MakeTheme(dark, HexColor(0x0078D4));
                renderer.UpdateBrushes(theme); renderer.text_background_ = theme.bg;
                renderer.icon_cache_.SetDeviceContext(compositor.Dc()); renderer.painter_.BeginFrame(theme, false);
                PaneViewModel pane; pane.view_mode = ViewMode::Details; pane.path = L"C:\\资料";
                pane.header_text = L"列宽拖动：原始 / 向右 / 向左";
                for (const wchar_t* name : {L"季度数据.xlsx", L"Drawing2.dwg", L"设计说明.pdf"}) {
                    ListEntryView entry; entry.name = name; entry.path = pane.path + L"\\" + name;
                    entry.type_text = L"设计文档"; entry.date_text = L"2026-09-27 18:20"; entry.size_text = L"128.5 KB";
                    pane.entries.push_back(entry);
                }
                WindowViewModel vm;
                auto* dc = compositor.Dc(); dc->BeginDraw(); dc->Clear(theme.bg);
                for (int phase = 0; phase < 3; ++phase) {
                    const auto bounds = D2D1::RectF(8*scale, (8+phase*232.0f)*scale, (width-8.0f)*scale, (232+phase*232.0f)*scale);
                    const auto body = renderer.PaneBodyBounds(pane, bounds);
                    const auto initial = renderer.DetailsColumns(body, pane);
                    for (int i = 0; i < initial.count; ++i) {
                        using K = MainRenderer::ColumnKind;
                        const int slot = initial.kinds[i] == K::Date ? 0 : initial.kinds[i] == K::Type ? 1 : initial.kinds[i] == K::Size ? 2 : -1;
                        if (slot >= 0) pane.details_column_dividers[slot] = initial.widths[i] / scale;
                    }
                    const int divider = initial.count >= 3 ? 1 : 0;
                    const float cursor = initial.DividerX(divider) + (phase == 1 ? 60.0f : phase == 2 ? -120.0f : 0.0f)*scale;
                    pane.details_column_dividers = renderer.ResizeDetailsColumnDivider(body, pane.details_column_dividers, divider, cursor);
                    const auto after = renderer.DetailsColumns(body, pane);
                    check(initial.kinds == after.kinds && initial.count == after.count, "measured production columns retain visibility through dragging");
                    renderer.DrawSinglePane(vm, pane, bounds, 0, true, false, theme);
                }
                check(SUCCEEDED(dc->EndDraw()), "render before/after column resize");
                const auto shot = root / (std::to_wstring(width) + L"-" + std::to_wstring(static_cast<int>(scale*100)) + (dark ? L"-dark.png" : L"-light.png"));
                check(compositor.SaveSnapshot(shot.c_str()), "save actual column renderer screenshot");
            }
        }
        DestroyWindow(hwnd); return ok;
    }
};
}
namespace pulse::app {
bool RunColumnResizeUiTest() {
    const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr)) return false;
    compat::EnableDpiAwareness(); l10n::Initialize(GetModuleHandleW(nullptr), L"zh-CN");
    const bool ok = ui::ColumnResizeUiTest::Run();
    CoUninitialize(); return ok;
}
}
#endif
