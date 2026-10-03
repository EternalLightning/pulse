#include "../app/app_internal.h"
#include "../common/localization.h"
#include "../ui/address_search_layout.h"
#include "../ui/toolbar_layout.h"
#include <filesystem>
#include <iostream>

int main() {
    using namespace pulse;
    OleInitialize(nullptr);
    l10n::Initialize(GetModuleHandleW(nullptr), L"zh-CN");
    auto owned = std::make_unique<AppState>();
    auto& s = *owned;
    s.isolatedTest = true;
    s.appPrefs.persist = false;
    s.searchHistory.persist = false;
    s.places.persist = false;
    s.hwnd = CreateWindowExW(0, L"STATIC", L"", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        -20000, -20000, 1100, 720, nullptr, nullptr, nullptr, nullptr);
    if (!s.hwnd || !s.compositor.Init(s.hwnd)) return 2;
    s.renderer.SetCompositor(&s.compositor);
    s.renderer.SetScale(1);
    s.compositor.RecreateTextFormats(1);
    ShowWindow(s.hwnd, SW_SHOWNOACTIVATE);
    s.window_tabs.EnsureDefault();
    s.pane = s.window_tabs.Active()->FocusedPane();
    auto* tab = ActiveTab(s);
    int failures = 0;
    auto check = [&](bool ok, const char* label) {
        std::cout << (ok ? "[PASS] " : "[FAIL] ") << label << std::endl;
        if (!ok) ++failures;
    };
    auto click = [&](float x, float y) {
        HandleLButtonDown(&s, s.hwnd, WM_LBUTTONDOWN, MK_LBUTTON,
            MAKELPARAM(static_cast<int>(x), static_cast<int>(y)));
    };
    auto outside = [&] { click(static_cast<float>(s.compositor.Width()) - 180, 20); };
    auto text = [&] {
        wchar_t value[256]{};
        GetWindowTextW(s.hwndAddressEdit, value, ARRAYSIZE(value));
        return std::wstring(value);
    };

    tab->current_path = app::MakeHomePath();
    s.appPrefs.address_search_current = true;
    ShowAddressSearch(s);
    check(!s.addressSearchCurrent && s.addressSearchRoot.empty(),
        "Home defaults to all indexed locations regardless of saved folder preference");
    HideAddressEditor(s, false);

    tab->current_path = L"C:\\fixture";
    s.appPrefs.address_search_current = false;
    auto vm = BuildVm(s, false);
    check(vm.address_search_placeholder.find(L"fixture") != std::wstring::npos,
        "folder placeholder reflects the default folder scope");
    ShowAddressSearch(s);
    check(s.addressSearchCurrent && s.addressSearchRoot == tab->current_path,
        "folder search defaults to current folder regardless of saved global preference");
    // Use a result-page draft without submitting filesystem/index work.
    tab->current_path = app::MakeSearchPath(L"draft");
    SetWindowTextW(s.hwndAddressEdit, L"draft");
    s.addressSearchAnimation = 1;
    outside();
    check(!s.addressEditing && !IsWindowVisible(s.hwndAddressEdit),
        "outside click hides search editor");
    const auto bounds = s.renderer.SearchBarRect(static_cast<float>(s.compositor.Width()));
    const auto layout = ui::LayoutAddressSearch(bounds, s.scale);
    const auto hit = s.renderer.HitTest(BuildVm(s, false),
        D2D1::RectF(0, 0, static_cast<float>(s.compositor.Width()), static_cast<float>(s.compositor.Height())),
        (layout.input.left + layout.input.right) / 2, (layout.input.top + layout.input.bottom) / 2);
    check(hit.region == ui::HitTestResult::AddressSearchInput,
        "inactive result-page query remains an input hit target");
    click((layout.input.left + layout.input.right) / 2, (layout.input.top + layout.input.bottom) / 2);
    check(s.addressSearching && s.addressEditing && IsWindowVisible(s.hwndAddressEdit) &&
        GetFocus() == s.hwndAddressEdit && text() == L"draft",
        "clicking result query reopens editor with draft and focus");
    check(s.addressSearchCurrent && s.addressSearchRoot == L"C:\\fixture",
        "reopening result query preserves its scope");
    SendMessageW(s.hwndAddressEdit, EM_SETSEL, 5, 5);
    SendMessageW(s.hwndAddressEdit, WM_CHAR, L'x', 0);
    check(text() == L"draftx", "reopened query accepts edits");
    HideAddressEditor(s, false);

    tab->current_path = L"C:\\fixture";
    ShowAddressEditor(s);
    SetWindowTextW(s.hwndAddressEdit, L"C:\\unsubmitted");
    const auto address = s.renderer.AddressBarRect(static_cast<float>(s.compositor.Width()));
    click(address.left + 5, (address.top + address.bottom) / 2);
    check(s.addressEditing, "click inside path bar keeps path editing active");
    outside();
    check(!s.addressEditing && !IsWindowVisible(s.hwndAddressEdit) &&
        tab->current_path == L"C:\\fixture" && !BuildVm(s, false).address_editing,
        "non-focusable outside click restores breadcrumbs without navigating draft");
    ShowAddressEditor(s);
    SendMessageW(s.hwndAddressEdit, WM_KEYDOWN, VK_ESCAPE, 0);
    check(!s.addressEditing, "Escape still cancels path editing");

    tab->back_stack = {};
    tab->forward_stack = {};
    const std::wstring first = L"pulse:fixture:first";
    const std::wstring second = L"pulse:fixture:second";
    const std::wstring third = L"pulse:fixture:third";
    tab->current_path = first;
    tab->NavigateTo(second);
    tab->NavigateTo(third);
    const auto toolbar = ui::MakeToolbarLayout(static_cast<float>(s.compositor.Width()), 1,
        s.renderer.TitleBarHeight(), s.renderer.Margin(), 84,
        s.renderer.EffectiveSidebarWidth(static_cast<float>(s.compositor.Width())));
    auto nav_press = [&](int index, bool double_press) {
        const auto& button = toolbar.navigation[index];
        const LPARAM point = MAKELPARAM(static_cast<int>((button.left + button.right) / 2),
            static_cast<int>((button.top + button.bottom) / 2));
        if (double_press) HandleLButtonDblClk(&s, s.hwnd, WM_LBUTTONDBLCLK, MK_LBUTTON, point);
        else HandleLButtonDown(&s, s.hwnd, WM_LBUTTONDOWN, MK_LBUTTON, point);
    };
    nav_press(0, false);
    check(tab->current_path == second, "first Back press navigates immediately");
    nav_press(0, true);
    check(tab->current_path == first && !BuildVm(s, false).can_go_back,
        "rapid second Back press navigates immediately and disables at history boundary");
    nav_press(0, false);
    check(tab->current_path == first, "disabled Back press preserves current location");
    nav_press(1, false);
    check(tab->current_path == second, "first Forward press navigates immediately");
    nav_press(1, true);
    check(tab->current_path == third && !BuildVm(s, false).can_go_forward,
        "rapid second Forward press navigates immediately and disables at history boundary");
    nav_press(1, true);
    check(tab->current_path == third, "disabled Forward double press preserves current location");

    const auto output = std::filesystem::absolute(L"../bench_data/navigation-toolbar-ui");
    std::filesystem::create_directories(output);
    for (const bool dark : {false, true}) for (const bool enabled : {false, true}) {
        auto navigation_vm = BuildVm(s, false);
        navigation_vm.dark = dark;
        navigation_vm.can_go_back = navigation_vm.can_go_forward = enabled;
        const auto rect = D2D1::RectF(0, 0, static_cast<float>(s.compositor.Width()),
            static_cast<float>(s.compositor.Height()));
        s.compositor.Dc()->BeginDraw();
        s.renderer.Render(navigation_vm, rect, ui::MakeTheme(dark, ui::HexColor(0x0078D4)));
        check(SUCCEEDED(s.compositor.Dc()->EndDraw()), "navigation toolbar render completes");
        const auto file = output / (std::wstring(dark ? L"dark" : L"light") +
            (enabled ? L"-enabled.png" : L"-disabled.png"));
        check(s.compositor.SaveSnapshot(file.c_str()), "navigation toolbar screenshot saved");
    }

    s.addressIgnoreKillFocus = true;
    if (s.hwndAddressEdit) DestroyWindow(s.hwndAddressEdit);
    s.hwndAddressEdit = nullptr;
    s.renderer.SetCompositor(nullptr);
    s.compositor.Shutdown();
    DestroyWindow(s.hwnd);
    s.hwnd = nullptr;
    OleUninitialize();
    return failures ? 1 : 0;
}
