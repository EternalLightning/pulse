#include "../ui/fluent_menu.h"
#include "../common/localization.h"
#include <cstdio>
#include <cmath>
#include <filesystem>

namespace {
int failures = 0;
void Check(bool ok, const char* label) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
    if (!ok) ++failures;
}
pulse::ui::FluentMenuItem Item(int command, const wchar_t* text) {
    pulse::ui::FluentMenuItem item;
    item.command = command;
    item.text = text;
    return item;
}
}

int wmain(int argc, wchar_t** argv) {
    using namespace pulse::ui;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    pulse::l10n::Initialize(GetModuleHandleW(nullptr), L"zh-CN");
    const HWND foreground = GetForegroundWindow();
    HWND owner = CreateWindowExW(WS_EX_NOACTIVATE, L"STATIC", L"", WS_POPUP,
        -30000, -30000, 700, 400, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    {
        Compositor compositor;
        Check(owner && compositor.Init(owner), "create menu rendering host");
        const auto long_item = Item(2,
            L"使用 Microsoft 画图进行编辑，带有很长文件名称的 Explorer 操作测试.png");
        for (const float scale : {1.0f, 1.25f, 1.5f}) {
            compositor.RecreateTextFormats(scale);
            FluentMenuModel model;
            auto inline_item = Item(3, L"Long filename in compact search column.txt");
            inline_item.shortcut = L"C:\\Search";
            inline_item.shortcut_inline = true;
            auto explicit_tip = long_item;
            explicit_tip.tooltip = L"Explicit command explanation";
            auto shortcut_item = long_item;
            shortcut_item.shortcut = L"Ctrl+Shift+E";
            auto swatches = long_item;
            swatches.quick_swatches.push_back({5, D2D1::ColorF(1, 0, 0)});
            model.SetItems({Item(1, L"打开"), long_item, inline_item, explicit_tip, shortcut_item, swatches});
            model.Layout(compositor.DwriteFactory(), scale);
            Check(model.WidthPx() <= static_cast<int>(std::ceil(320 * scale)),
                "long menu verbs retain the compact width cap");
            Check(!model.LabelTruncated(0) && model.TooltipText(0).empty(),
                "short labels do not gain a redundant tooltip");
            Check(model.LabelTruncated(1) && model.TooltipText(1) == long_item.text,
                "clamped label exposes its full Unicode text");
            Check(model.LabelTruncated(2) && model.TooltipText(2) == inline_item.text,
                "inline search column exposes its truncated filename");
            Check(model.TooltipText(3) == explicit_tip.tooltip,
                "explicit command tooltip takes precedence");
            Check(model.LabelTruncated(4), "shortcut chrome is reserved before checking labels");
            Check(!model.LabelTruncated(5), "swatch strip is not treated as a label");
            Check(!model.LabelTruncated(-1) && model.TooltipText(999).empty(),
                "invalid rows have no tooltip");
            const int fixed_width = model.WidthPx(), fixed_count = model.Count();
            Check(model.UpdateCommandState(2, L"Undo", false) && model.At(1)->text == L"Undo" &&
                !model.At(1)->enabled && model.Count() == fixed_count,
                "invalidated undo command changes its label and enabled state without leaving stale delete text");
            model.Layout(compositor.DwriteFactory(), scale, static_cast<float>(fixed_width));
            Check(model.WidthPx() == fixed_width && model.Count() == fixed_count && model.TooltipText(1).empty(),
                "live undo refresh preserves menu geometry and clears its obsolete tooltip");
            Check(!model.UpdateCommandState(999, L"Absent", false), "updating absent commands cannot add popup rows");
            model.SetItems({Item(1, L"Open")});
            Check(!model.LabelTruncated(0), "replacing rows clears previous truncation state");
            model.Layout(compositor.DwriteFactory(), scale);
            Check(!model.LabelTruncated(0), "relayout clears old long-label results");

            if (argc == 2) {
                std::filesystem::create_directories(argv[1]);
                FluentMenu menu;
                Check(menu.Create(owner, &compositor, scale), "create snapshot menu");
                auto submenu = long_item;
                submenu.children.push_back(Item(6, L"Open"));
                auto badge = long_item;
                badge.badge_text = L"提示";
                for (const bool dark : {false, true}) {
                    menu.SetTheme(dark, D2D1::ColorF(0.0f, 0.47f, 0.83f));
                    const auto filename = std::wstring(L"menu_text_") +
                        std::to_wstring(static_cast<int>(scale * 100)) +
                        (dark ? L"_dark.png" : L"_light.png");
                    const auto output = std::filesystem::path(argv[1]) / filename;
                    Check(menu.SaveDebugSnapshot(output.c_str(),
                        {Item(1, L"使用 Microsoft 画图进行编辑"), long_item, shortcut_item,
                         inline_item, submenu, badge}, 1), "save menu text visual evidence");
                }
            }
        }
    }
    DestroyWindow(owner);
    Check(GetForegroundWindow() == foreground, "tests preserve foreground activation");
    CoUninitialize();
    return failures ? 1 : 0;
}
