// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "../app/app_internal.h"

inline int pulse::ui::ItemReflowUiTest::RunSearchContinuity(const std::filesystem::path& output) {
    using namespace pulse;
    int failures = 0;
    const auto check = [&](bool passed, const char* label) {
        std::printf("[%s] %s\n", passed ? "PASS" : "FAIL", label);
        failures += !passed;
    };
    std::filesystem::create_directories(output);
    SetEnvironmentVariableW(L"PULSE_TEST_HIDDEN_SHOT", L"1");
    auto owned = std::make_unique<AppState>();
    auto& s = *owned;
    s.isolatedTest = true;
    s.appPrefs.persist = s.places.persist = s.searchHistory.persist = false;
    s.appPrefs.search_pinyin = false;
    s.hwnd = CreateWindowExW(WS_EX_NOACTIVATE, L"STATIC", L"Search continuity",
        WS_POPUP, -30000, -30000, 1100, 820, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!s.hwnd || !s.compositor.Init(s.hwnd)) {
        check(false, "initialize search continuity compositor");
        if (s.hwnd) DestroyWindow(s.hwnd);
        s.hwnd = nullptr;
        return 1;
    }
    s.renderer.SetCompositor(&s.compositor);
    s.renderer.SetDetailsPanelVisible(false);
    s.compositor.RecreateTextFormats(1);
    s.window_tabs.EnsureDefault();
    s.pane = s.window_tabs.Active()->FocusedPane();
    auto& tab = *ActiveTab(s);
    s.hwndAddressEdit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | ES_AUTOHSCROLL,
        0, 0, 300, 30, s.hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
    s.addressSearching = s.addressEditing = true;
    const std::wstring root = L"C:\\pulse-search-continuity";
    const auto submit = [&](const wchar_t* text) {
        SetWindowTextW(s.hwndAddressEdit, text);
        QueueAddressSearch(s);
        TickAddressSearchDeadlines(s, s.addressLiveDue);
        return static_cast<uint32_t>(tab.pending_generation);
    };
    const auto publish = [&](uint32_t id, const std::vector<std::wstring>& names) {
        index::SearchResult result;
        for (const auto& name : names) result.hits.push_back({root + L"\\" + name, name});
        result.total = result.hits.size();
        AcceptIndexProviderResult(s, id, std::move(result), false);
    };
    const auto pane = [&] {
        PaneViewModel vm;
        app::FillPaneViewModel(vm, *s.pane, &s.places);
        return vm;
    };
    const auto ink = [&](D2D1_RECT_F bounds) -> size_t {
        using Microsoft::WRL::ComPtr;
        ComPtr<ID2D1Image> image;
        ComPtr<ID2D1Bitmap1> target, sample;
        auto* dc = s.compositor.Dc();
        dc->GetTarget(image.GetAddressOf());
        if (!image || FAILED(image.As(&target))) return 0;
        const auto size = target->GetPixelSize();
        const UINT left = static_cast<UINT>(std::max(0.0f, std::floor(bounds.left)));
        const UINT top = static_cast<UINT>(std::max(0.0f, std::floor(bounds.top)));
        const UINT right = std::min(size.width, static_cast<UINT>(std::max(0.0f, std::ceil(bounds.right))));
        const UINT bottom = std::min(size.height, static_cast<UINT>(std::max(0.0f, std::ceil(bounds.bottom))));
        if (right <= left || bottom <= top) return 0;
        const auto properties = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW, target->GetPixelFormat());
        const D2D1_RECT_U region{left, top, right, bottom};
        if (FAILED(dc->CreateBitmap(D2D1::SizeU(right - left, bottom - top), nullptr, 0, properties, &sample)) ||
            FAILED(sample->CopyFromBitmap(nullptr, target.Get(), &region))) return 0;
        D2D1_MAPPED_RECT mapped{};
        if (FAILED(sample->Map(D2D1_MAP_OPTIONS_READ, &mapped))) return 0;
        size_t count = 0;
        for (UINT y = 0; y < bottom - top; ++y) for (UINT x = 0; x < right - left; ++x) {
            const auto* pixel = mapped.bits + static_cast<size_t>(y) * mapped.pitch + x * 4;
            if (pixel[0] + pixel[1] + pixel[2] < 500) ++count;
        }
        sample->Unmap();
        return count;
    };
    for (const auto mode : {ViewMode::Details, ViewMode::List, ViewMode::MediumIcons}) {
        std::printf("[CASE] sx->s mode=%d\n", static_cast<int>(mode));
        publish(submit(L"sx"), {L"sx_alpha.txt", L"sx_beta.txt", L"sx_gamma.txt", L"sx_delta.txt"});
        tab.view_mode = mode;
        tab.ClearSelection();
        tab.scroll_y = mode == ViewMode::List ? 0.0f : 12.0f;
        const float previous_scroll = tab.scroll_y;
        for (auto& surface : s.renderer.pane_surfaces_) surface.Reset();
        s.renderer.page_surface_.Reset();
        Reset(s.renderer);
        uint64_t last_frame = 0;
        const auto frame = [&](uint64_t now, const wchar_t* label) {
            if (last_frame && now > last_frame) Sleep(static_cast<DWORD>(now - last_frame));
            last_frame = now;
            WindowViewModel vm;
            vm.pane = pane();
            const auto theme = MakeTheme(false, HexColor(0x0078D4));
            auto* dc = s.compositor.Dc();
            dc->BeginDraw(); dc->Clear(theme.bg);
            s.renderer.motion_enabled_ = true;
            s.renderer.Render(vm, D2D1::RectF(0, 0, 1100, 820), theme);
            check(SUCCEEDED(dc->EndDraw()), "sx->s production pane renders");
            check(!s.renderer.pane_surfaces_[0].pending, "query edit never starts a full-pane fade");
            check(!s.renderer.page_surface_.pending, "query edit never starts a full-window fade");
            if (label) check(WriteAnimationSnapshot(s.compositor, output /
                (std::to_wstring(static_cast<int>(mode)) + L"-" + label + L".png")), "save sx->s frame");
        };
        const auto visual = [&](const wchar_t* name) {
            const std::wstring key = root + L"\\" + name + L"\n" + name;
            return Surface(s.renderer).BackgroundVisual(key, {}, 0);
        };
        frame(1000, L"sx");
        const auto old_alpha = visual(L"sx_alpha.txt");
        const auto old_beta = visual(L"sx_beta.txt");
        const size_t beta_ink = ink(old_beta.bounds);
        check(beta_ink > 20, "initial sx row has measurable foreground pixels");
        const auto old_snapshot = tab.snapshot;
        const auto request = submit(L"s");
        check(pane().search_query == L"sx", "pending s keeps the published sx highlight and filter chips");
        check(tab.view_mode == mode && tab.scroll_y == previous_scroll,
            "editing the query preserves the current view and scroll position");
        check(tab.snapshot == old_snapshot && tab.search_retaining_results,
            "pending s keeps all published sx identities");
        frame(1100, L"pending-s");
        std::vector<std::wstring> expanded;
        for (int i = 0; i < 320; ++i) expanded.push_back(L"s" + std::to_wstring(i) + L".txt");
        expanded[3] = L"sx_alpha.txt";
        expanded[319] = L"sx_beta.txt";
        publish(request, expanded);
        int beta_source = -1;
        for (size_t i = 0; i < tab.EntryCount(); ++i)
            if (tab.EntryAt(i).name == L"sx_beta.txt") beta_source = static_cast<int>(i);
        check(beta_source >= 300, "provider relevance merging places the retained sx identity beyond neighboring pages");
        check(pane().search_query == L"s", "s highlights publish together with the s result snapshot");
        frame(1200, L"s-0");
        const auto alpha = visual(L"sx_alpha.txt");
        const auto beta = visual(L"sx_beta.txt");
        check(alpha.opacity == 1 && !alpha.exiting && alpha.bounds.top == old_alpha.bounds.top &&
            alpha.bounds.left == old_alpha.bounds.left, "retained sx starts from its actual painted position");
        check(beta.opacity == 1 && !beta.exiting && beta.source == beta_source &&
            beta.bounds.top == old_beta.bounds.top && beta.bounds.left == old_beta.bounds.left,
            "sx retained beyond the new neighboring page remains a moving live identity");
        check(ink(old_beta.bounds) * 2 >= beta_ink,
            "sx beyond the new page is actually painted at its previous screen position");
        frame(1280, L"s-80");
        const auto middle = visual(L"sx_alpha.txt");
        check(middle.opacity == 1 && !middle.exiting &&
            (middle.bounds.top != alpha.bounds.top || middle.bounds.left != alpha.bounds.left),
            "retained sx remains opaque while visibly moving to its new rank");
        frame(1440, L"s-settled");
        check(!Surface(s.renderer).Pending(), "sx->s motion releases its frame deadline");

        std::printf("[CASE] folder bulk update mode=%d\n", static_cast<int>(mode));
        tab.current_path = root;
        tab.search_retaining_results = false;
        tab.loading = false;
        tab.scroll_y = 0;
        tab.ClearSelection();
        auto folder_before = std::make_shared<std::vector<fs::DirEntry>>();
        for (const auto* name : {L"sx_alpha.txt", L"sx_beta.txt", L"sx_gamma.txt", L"sx_delta.txt"}) {
            fs::DirEntry entry;
            entry.name = name;
            entry.attrs = FILE_ATTRIBUTE_NORMAL;
            folder_before->push_back(std::move(entry));
        }
        tab.SetSnapshot(folder_before);
        for (auto& surface : s.renderer.pane_surfaces_) surface.Reset();
        Reset(s.renderer);
        frame(2000, L"folder-before");
        const auto folder_beta = visual(L"sx_beta.txt");
        const size_t folder_ink = ink(folder_beta.bounds);
        auto folder_after = std::make_shared<std::vector<fs::DirEntry>>();
        for (const auto& name : expanded) {
            fs::DirEntry entry;
            entry.name = name;
            entry.attrs = FILE_ATTRIBUTE_NORMAL;
            folder_after->push_back(std::move(entry));
        }
        tab.SetSnapshot(folder_after);
        frame(2100, L"folder-after-0");
        const auto folder_moving = visual(L"sx_beta.txt");
        check(folder_moving.source == 319 && !folder_moving.exiting && folder_moving.opacity == 1 &&
            folder_moving.bounds.left == folder_beta.bounds.left &&
            folder_moving.bounds.top == folder_beta.bounds.top,
            "ordinary folder snapshots retain off-page identities with implicit child paths");
        check(folder_ink > 20 && ink(folder_beta.bounds) * 2 >= folder_ink,
            "ordinary folder retained rows remain painted during a large replacement");
        frame(2180, L"folder-after-80");
        frame(2400, nullptr);
        check(!Surface(s.renderer).Pending(), "ordinary folder bulk movement settles");
    }
    s.watches.Stop();
    DestroyWindow(s.hwnd);
    s.hwndAddressEdit = s.hwnd = nullptr;
    std::printf("search_continuity_failures=%d\n", failures);
    return failures ? 1 : 0;
}
