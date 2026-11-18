// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "../ui/toolbar_customization.h"
#include <windowsx.h>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <functional>
#include <thread>
#include <utility>

namespace pulse::bench::toolbar_customization_checks {
using namespace pulse::ui;
using Findings = std::vector<std::pair<bool, const char*>>;
inline bool Send(HWND hwnd, UINT message, WPARAM wparam = 0, LPARAM lparam = 0, DWORD_PTR* output = nullptr) {
    DWORD_PTR result = 0;
    const bool completed = SendMessageTimeoutW(hwnd, message, wparam, lparam,
        SMTO_ABORTIFHUNG | SMTO_BLOCK, 3000, &result) != 0;
    if (output) *output = result;
    return completed;
}
inline bool Inspect(HWND hwnd, ToolbarCustomizationInspection& info) {
    DWORD_PTR result = 0;
    return Send(hwnd, kToolbarCustomizationInspectMessage, 0, reinterpret_cast<LPARAM>(&info), &result) && result;
}
inline bool WaitScroll(HWND hwnd, bool current) {
    const auto deadline = std::chrono::steady_clock::now()+std::chrono::seconds(3);
    do {
        ToolbarCustomizationInspection info;
        if (!Inspect(hwnd, info)) return false;
        if (!(current ? info.current_scrolling : info.available_scrolling)) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    } while (std::chrono::steady_clock::now() < deadline);
    return false;
}
inline LPARAM Center(const D2D1_RECT_F& bounds) {
    return MAKELPARAM(static_cast<short>(std::lround((bounds.left+bounds.right)*0.5f)),
        static_cast<short>(std::lround((bounds.top+bounds.bottom)*0.5f)));
}
inline bool Click(HWND hwnd, D2D1_RECT_F bounds) {
    return bounds.right > bounds.left && bounds.bottom > bounds.top &&
        Send(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, Center(bounds)) && Send(hwnd, WM_LBUTTONUP, 0, Center(bounds));
}
inline D2D1_RECT_F Row(const ToolbarCustomizationInspection& info, bool current, int command) {
    for (const auto& row : current ? info.current_rows : info.available_rows)
        if (!row.category && row.command == command) return row.bounds;
    return {};
}
inline bool Snapshot(HWND hwnd, const std::filesystem::path& path) {
    DWORD_PTR result = 0;
    return Send(hwnd, kToolbarCustomizationSnapshotMessage, 0, reinterpret_cast<LPARAM>(path.c_str()), &result) && result;
}
inline ToolbarCustomizationSpec Spec(bool english = false) {
    ToolbarCustomizationSpec spec;
    spec.title = english ? L"Customize toolbar" : L"自定义工具栏";
    spec.available_label = english ? L"Available actions" : L"可用操作";
    spec.current_label = english ? L"Current toolbar" : L"当前工具栏";
    spec.add_text = english ? L"Add →" : L"添加 →";
    spec.remove_text = english ? L"← Remove" : L"← 移除";
    spec.restore_text = english ? L"Restore defaults" : L"恢复默认设置";
    spec.save_text = english ? L"Save" : L"保存";
    spec.cancel_text = english ? L"Cancel" : L"取消";
    spec.up_text = english ? L"Move up" : L"上移";
    spec.down_text = english ? L"Move down" : L"下移";
    const wchar_t* names_zh[] = {L"新建", L"剪切", L"复制", L"粘贴", L"重命名", L"删除", L"属性", L"撤销",
        L"全选", L"反向选择", L"通配符选择", L"详细信息", L"大图标", L"分栏布局", L"左右双窗格", L"上下双窗格",
        L"刷新", L"搜索", L"打开终端", L"路径别名", L"应用程序", L"操作中心", L"设置", L"控制面板"};
    const wchar_t* names_en[] = {L"New", L"Cut", L"Copy", L"Paste", L"Rename", L"Delete", L"Properties", L"Undo",
        L"Select all", L"Invert selection", L"Wildcard selection", L"Details", L"Large icons", L"Column layout", L"Side by side panes", L"Stacked panes",
        L"Refresh", L"Search", L"Open terminal", L"Path aliases", L"Applications", L"Operation center", L"Settings", L"Control panel"};
    const wchar_t* groups_zh[] = {L"新建", L"编辑", L"选择", L"视图", L"工具"};
    const wchar_t* groups_en[] = {L"Create", L"Edit", L"Selection", L"View", L"Tools"};
    for (int i = 0; i < 24; ++i) {
        const int group = i == 0 ? 0 : i < 8 ? 1 : i < 11 ? 2 : i < 17 ? 3 : 4;
        spec.actions.push_back({i+1, english ? names_en[i] : names_zh[i],
            i == 0 ? L"\xE710" : i < 8 ? L"\xE8C8" : L"\xE713", english ? groups_en[group] : groups_zh[group]});
    }
    spec.order = {1, 2, 3}; spec.default_order = {1, 2, 3, 4, 5, 6, 7};
    return spec;
}
inline ToolbarCustomizationResult RunDialog(const ToolbarCustomizationSpec& spec, bool dark, Findings& findings,
    const std::function<void(HWND, Findings&)>& driver) {
    const DWORD main_thread = GetCurrentThreadId();
    std::thread worker([&] {
        HWND window = nullptr;
        const auto deadline = std::chrono::steady_clock::now()+std::chrono::seconds(8);
        while (!window && std::chrono::steady_clock::now() < deadline) {
            EnumThreadWindows(main_thread, [](HWND candidate, LPARAM reference) -> BOOL {
                wchar_t name[128]{}; GetClassNameW(candidate, name, 128);
                if (wcscmp(name, kToolbarCustomizationWindowClass) == 0) {
                    *reinterpret_cast<HWND*>(reference) = candidate; return FALSE;
                }
                return TRUE;
            }, reinterpret_cast<LPARAM>(&window));
            if (!window) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        findings.push_back({window != nullptr, "toolbar customization modal creates its production window"});
        if (window) {
            try { driver(window, findings); }
            catch (...) { findings.push_back({false, "toolbar customization UI driver completed without an exception"}); }
            if (IsWindow(window)) PostMessageW(window, WM_CLOSE, 0, 0);
        }
    });
    const auto result = ShowToolbarCustomization(nullptr, spec, dark, HexColor(0x0078D4));
    worker.join();
    return result;
}
} // namespace pulse::bench::toolbar_customization_checks

namespace pulse::bench {
inline void RunToolbarCustomizationChecks(const std::filesystem::path& root,
    const std::function<void(bool, const char*)>& check) {
    namespace test = toolbar_customization_checks;
    using namespace ui;
    std::filesystem::create_directories(root);
    auto spec = test::Spec();
    ToolbarCustomizationModel model(spec);
    check(model.Order() == spec.order && !model.Add(0) && !model.Add(900) && !model.Add(1),
        "toolbar model rejects unknown IDs and duplicate actions");
    check(model.Add(4) && model.Remove(2) && !model.Remove(2) && model.Order() == std::vector<int>({1,3,4}),
        "toolbar model adds and removes commands by stable ID");
    check(model.Move(4,1) && model.Order() == std::vector<int>({4,1,3}) && model.Move(4,0) &&
        model.Order() == std::vector<int>({1,3,4}) && !model.Move(4,900) && !model.Move(4,4),
        "toolbar model moves before a stable ID or to the end and rejects invalid targets");
    model.Reset();
    check(model.Order() == spec.default_order && spec.order == std::vector<int>({1,2,3}),
        "toolbar defaults reset only the working model and preserve caller state");
    auto invalid_spec = spec; invalid_spec.order = {1,999,1,2};
    check(ToolbarCustomizationModel(invalid_spec).Order() == std::vector<int>({1,2}),
        "toolbar model sanitizes stale and duplicate configured IDs");
    test::Findings findings;
    const auto saved = test::RunDialog(spec, false, findings, [&](HWND hwnd, auto& output) {
        ToolbarCustomizationInspection info;
        if (!test::Inspect(hwnd, info)) { output.push_back({false, "toolbar production inspection responds"}); return; }
        output.push_back({info.order == spec.order && info.current.right > info.available.right &&
            info.add.right < info.current.left && info.primary.top > info.restore.bottom,
            "toolbar dialog keeps two lists, actions and footer in separate bounds"});
        output.push_back({test::Snapshot(hwnd, root/L"toolbar-customize-light.png"), "toolbar light theme PNG rendered"});
        test::Click(hwnd, test::Row(info, false, 4)); test::Inspect(hwnd, info); test::Click(hwnd, info.add); test::Inspect(hwnd, info);
        output.push_back({info.order == std::vector<int>({1,2,3,4}), "toolbar Add button updates the working order"});
        test::Click(hwnd, test::Row(info, true, 2)); test::Inspect(hwnd, info); test::Click(hwnd, info.remove); test::Inspect(hwnd, info);
        output.push_back({info.order == std::vector<int>({1,3,4}), "toolbar Remove button removes only the selected stable command"});
        const auto start = test::Row(info, true, 1), target = test::Row(info, true, 3);
        const auto drop = MAKELPARAM(static_cast<short>((target.left+target.right)*0.5f), static_cast<short>(target.bottom-2));
        test::Send(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, test::Center(start));
        test::Send(hwnd, WM_MOUSEMOVE, MK_LBUTTON, drop); test::Send(hwnd, WM_LBUTTONUP, 0, drop); test::Inspect(hwnd, info);
        output.push_back({info.order == std::vector<int>({3,1,4}) && !info.captured,
            "toolbar drag reorders by command identity and releases mouse capture"});
        test::Send(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, test::Center(test::Row(info, true, 1)));
        test::Send(hwnd, WM_MOUSEMOVE, MK_LBUTTON, test::Center(test::Row(info, true, 4)));
        test::Inspect(hwnd, info);
        const bool captured_drag = info.captured && info.dragging_command == 1;
        test::Send(hwnd, WM_KEYDOWN, VK_ESCAPE); test::Inspect(hwnd, info);
        output.push_back({captured_drag && !info.captured && !info.dragging_command && info.order == std::vector<int>({3,1,4}),
            "Escape cancels toolbar drag without closing or changing the order"});
        test::Send(hwnd, WM_KEYDOWN, VK_END); test::Inspect(hwnd, info);
        const bool selected_end = info.current_selection == 4;
        test::Send(hwnd, WM_SYSKEYDOWN, VK_UP); test::Inspect(hwnd, info);
        output.push_back({selected_end && info.order == std::vector<int>({3,4,1}), "toolbar keyboard selection and Alt+Up reorder work"});
        test::Send(hwnd, WM_KEYDOWN, VK_TAB); test::Inspect(hwnd, info);
        output.push_back({info.focus != 101, "Tab moves keyboard focus from the toolbar list to its actions"});
        test::Click(hwnd, info.restore); test::Inspect(hwnd, info);
        output.push_back({info.order == spec.default_order, "toolbar Restore defaults changes the draft before Save"});
        test::Click(hwnd, info.primary);
    });
    check(saved.accepted && saved.order == spec.default_order, "toolbar Save returns exactly the reviewed draft order");
    const auto cancelled = test::RunDialog(spec, false, findings, [&](HWND hwnd, auto& output) {
        ToolbarCustomizationInspection info; test::Inspect(hwnd, info);
        test::Click(hwnd, test::Row(info, false, 4)); test::Inspect(hwnd, info); test::Click(hwnd, info.add); test::Inspect(hwnd, info);
        output.push_back({info.order != spec.order, "toolbar Cancel fixture has an edited draft"});
        test::Click(hwnd, info.cancel);
    });
    check(!cancelled.accepted && cancelled.order.empty() && spec.order == std::vector<int>({1,2,3}),
        "toolbar Cancel discards edits and leaves caller configuration unchanged");
    for (int variant = 0; variant < 4; ++variant) {
        auto visual = test::Spec(variant == 1 || variant == 2);
        visual.order = visual.default_order;
        const auto result = test::RunDialog(visual, variant == 0, findings, [&](HWND hwnd, auto& output) {
            if (variant == 2) SetWindowPos(hwnd, nullptr, 0, 0, 600, 430, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            if (variant == 3) {
                RECT suggested{}; GetWindowRect(hwnd, &suggested);
                test::Send(hwnd, WM_DPICHANGED, MAKELPARAM(168,168), reinterpret_cast<LPARAM>(&suggested));
            }
            ToolbarCustomizationInspection info; test::Inspect(hwnd, info);
            output.push_back({info.available.right < info.current.left && info.current.bottom < info.up.top &&
                info.restore.bottom < info.cancel.top && info.cancel.right < info.primary.left,
                "toolbar localization, narrow layout and DPI keep controls from overlapping"});
            const wchar_t* name = variant == 0 ? L"toolbar-customize-dark.png" : variant == 1 ? L"toolbar-customize-english.png" :
                variant == 2 ? L"toolbar-customize-narrow.png" : L"toolbar-customize-dpi175.png";
            output.push_back({test::Snapshot(hwnd, root/name), "toolbar representative visual PNG rendered"});
            if (variant == 3) output.push_back({std::abs(info.scale-1.75f) < 0.001f, "toolbar DPI change recreates formats and geometry at 175 percent"});
            test::Click(hwnd, info.available);
            test::Send(hwnd, WM_KEYDOWN, VK_END);
            output.push_back({test::WaitScroll(hwnd, false), "toolbar keyboard reveal animation converges"});
            test::Inspect(hwnd, info);
            const auto last = test::Row(info, false, 24);
            output.push_back({info.available_selection == 24 && last.right > last.left,
                "toolbar available list keyboard navigation reveals its final categorized action"});
            test::Send(hwnd, WM_KEYDOWN, VK_ESCAPE);
        });
        check(!result.accepted, "toolbar visual fixture closes without saving any preferences");
    }
    for (const auto& [value, description] : findings) check(value, description);
}
} // namespace pulse::bench
