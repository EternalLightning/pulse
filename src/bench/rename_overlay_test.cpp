// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "../app/app_internal.h"
#include "../app/search_query.h"
#include <iostream>
#include <cmath>

int main() {
    using namespace pulse;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    int failures = 0;
    auto check = [&](bool ok, const char* name) {
        std::cout << (ok ? "[PASS] " : "[FAIL] ") << name << std::endl;
        if (!ok) ++failures;
    };
    auto owned = std::make_unique<AppState>(); auto& s = *owned;
    s.appPrefs.persist = false;
    WNDCLASSW wc{}; wc.hInstance = GetModuleHandleW(nullptr); wc.lpfnWndProc = DefWindowProcW;
    wc.lpszClassName = L"PulseRenameFixture"; RegisterClassW(&wc);
    s.hwnd = CreateWindowExW(0, wc.lpszClassName, L"Pulse rename verification", WS_OVERLAPPEDWINDOW,
        100, 100, 1200, 760, nullptr, nullptr, wc.hInstance, nullptr);
    if (!s.hwnd || !s.compositor.Init(s.hwnd)) return 2;
    s.compositor.Resize(1180, 720); s.renderer.SetCompositor(&s.compositor);
    s.window_tabs.EnsureDefault(); s.pane = s.window_tabs.Active()->FocusedPane();
    auto& tab = *ActiveTab(s);
    auto entries = std::make_shared<std::vector<fs::DirEntry>>();
    for (const auto* name : {L"中文合同.docx", L"example.cpp", L"新建 Microsoft Excel 工作表.xlsx", L"notes.txt"}) {
        fs::DirEntry row; row.name = name; row.full_path = L"C:\\fixture\\" + row.name;
        entries->push_back(std::move(row));
    }
    tab.SetSnapshot(entries);
    tab.search_snippets = std::make_shared<std::vector<std::wstring>>(4, L"L1  合同内容匹配");
    auto pump = [] { MSG msg{}; while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); } };
    ShowWindow(s.hwnd, SW_SHOWNOACTIVATE);
    for (const float scale : {1.0f, 1.5f}) for (bool search : {false, true}) {
        s.scale = scale; s.darkMode = search;
        s.compositor.RecreateTextFormats(scale); s.renderer.SetScale(scale);
        if (s.editFont) { DeleteObject(s.editFont); s.editFont = nullptr; }
        if (s.editBrush) { DeleteObject(s.editBrush); s.editBrush = nullptr; }
        tab.current_path = search ? app::MakeSearchPath(L"content:合同") : L"C:\\fixture";
        tab.banner_message = search ? L"Content indexing is up to date" : L"";
        tab.SelectOnly(2); ShowRenameOverlay(s);
        auto vm = BuildVm(s, false);
        for (const auto& slot : vm.pane_slots) if (slot.focused) {
            const auto list = s.renderer.PaneListRect(slot.pane, slot.rect);
            const auto frame = s.renderer.RenameFieldRect(slot.pane, list, s.renameIndex);
            RECT edit{}; GetWindowRect(s.hwndRenameEdit, &edit);
            MapWindowPoints(nullptr, s.hwnd, reinterpret_cast<POINT*>(&edit), 2);
            check(edit.left >= frame.left && edit.right <= frame.right + 1 && edit.top >= frame.top && edit.bottom <= frame.bottom + 1,
                search ? "search rename text is inside the shared frame" : "folder rename text is inside the shared frame");
            const auto row = s.renderer.ItemRectInPane(slot.pane, slot.rect, slot.pane.ViewIndex(s.renameIndex));
            check(frame.top >= row.top && frame.bottom <= row.bottom + 1, "rename frame stays in the selected row");
            const auto hit = s.renderer.HitTest(vm, D2D1::RectF(0, 0, 1180, 720), frame.left + 4, (frame.top + frame.bottom) / 2);
            check(hit.index == 2, "rendered edit position hits the same file row");
        }
        DWORD start = 0, end = 0; SendMessageW(s.hwndRenameEdit, EM_GETSEL, reinterpret_cast<WPARAM>(&start), reinterpret_cast<LPARAM>(&end));
        check(start == 0 && end == entries->at(2).name.find_last_of(L'.'), "shared rename selects the stem and preserves the extension");
        const auto theme = ui::MakeTheme(s.darkMode, ui::HexColor(0x0078D4));
        s.compositor.Dc()->BeginDraw(); s.renderer.Render(vm, D2D1::RectF(0, 0, 1180, 720), theme);
        s.compositor.Dc()->EndDraw(); s.compositor.Present();
        InvalidateRect(s.hwndRenameEdit, nullptr, TRUE); UpdateWindow(s.hwndRenameEdit); pump();
        if (scale == 1.5f && search && GetEnvironmentVariableW(L"PULSE_RENAME_SHOW", nullptr, 0)) {
            const auto until = GetTickCount64() + 120000;
            while (GetTickCount64() < until) { pump(); Sleep(20); }
        }
        SendMessageW(s.hwndRenameEdit, WM_KEYDOWN, VK_ESCAPE, 0);
        check(s.renameIndex == -1 && !IsWindowVisible(s.hwndRenameEdit), "Escape uses the existing cancel path");
    }
    s.renderer.SetCompositor(nullptr); s.compositor.Shutdown(); DestroyWindow(s.hwnd); s.hwnd = nullptr;
    CoUninitialize(); return failures ? 1 : 0;
}
