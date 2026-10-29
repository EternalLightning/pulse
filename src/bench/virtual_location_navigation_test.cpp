#include "../app/app_internal.h"
#include "../common/localization.h"
#include "../common/windows_compat.h"
#include <cstdio>

int main() {
    using namespace pulse;
    OleInitialize(nullptr);
    compat::EnableDpiAwareness();
    int checks = 0, failures = 0;
    const auto check = [&](bool ok, const char* label) {
        ++checks;
        failures += !ok;
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
    };
    auto owned = std::make_unique<AppState>();
    auto& s = *owned;
    s.isolatedTest = true;
    s.appPrefs.persist = s.places.persist = s.ctxMenuPrefs.persist = s.searchHistory.persist = false;
    s.hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"STATIC", L"Navigation fixture",
        WS_POPUP, -30000, -30000, 1100, 720, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    check(s.hwnd && s.compositor.Init(s.hwnd), "hidden production compositor initializes");
    if (!s.compositor.Dc()) return 2;
    s.renderer.SetCompositor(&s.compositor);
    s.window_tabs.NewTab(L"");
    s.pane = s.window_tabs.Active()->FocusedPane();
    const std::vector<std::wstring> addresses{L"", app::MakeHomePath(), app::MakeStarredPath(),
        app::MakeRecentPath(), app::MakeRecyclePath(), app::MakeTagPath(L"tag-identity"),
        app::MakeSearchPath(L"content:needle"), L"pulse:saved-search:7", app::MakeSettingsPath(),
        L"pulse:changes:workspace-identity", L"pulse:archive:E:\\fixture\\sample.zip|folder/"};
    for (const auto* locale : {L"zh-CN", L"en-US"}) {
        l10n::Initialize(GetModuleHandleW(nullptr), locale);
        for (const float scale : {1.0f, 1.5f}) for (const int width : {720, 1100}) {
            s.scale = scale;
            s.renderer.SetScale(scale);
            s.compositor.RecreateTextFormats(scale);
            s.compositor.Resize(static_cast<UINT>(width * scale), static_cast<UINT>(720 * scale));
            for (const auto& address : addresses) {
                s.pane->NewTab(address);
                auto& tab = *ActiveTab(s);
                tab.virtual_title = address.empty() ? std::wstring{} : L"Custom title / 自定义名称";
                tab.loading = false;
                tab.SetSnapshot(std::make_shared<std::vector<fs::DirEntry>>());
                tab.pending_generation = 42;
                const auto generation = tab.view_generation;
                const auto vm = BuildVm(s, false);
                std::vector<ui::MainRenderer::BreadcrumbPlaced> placed;
                s.renderer.BreadcrumbLayout(vm.pane, static_cast<float>(s.compositor.Width()), placed);
                check(!placed.empty() && placed.back().path == address &&
                      (address.empty() || placed.back().full_text == tab.virtual_title),
                    "production breadcrumb layout separates the label from the navigation address");
                if (placed.empty()) continue;
                const auto& rc = placed.back().rc;
                const LPARAM point = MAKELPARAM(static_cast<int>((rc.left + rc.right) * .5f),
                    static_cast<int>((rc.top + rc.bottom) * .5f));
                if (!vm.settings_open) {
                    const auto hit = s.renderer.HitTest(vm,
                        D2D1::RectF(0, 0, static_cast<float>(s.compositor.Width()),
                            static_cast<float>(s.compositor.Height())),
                        static_cast<float>(LOWORD(point)), static_cast<float>(HIWORD(point)));
                    check(hit.region == ui::HitTestResult::BreadcrumbSegment && hit.path == address,
                        "visible breadcrumb hit testing returns the original navigation address");
                    for (int repeat = 0; repeat < 3; ++repeat) {
                        HandleLButtonDown(&s, s.hwnd, WM_LBUTTONDOWN, MK_LBUTTON, point);
                        HandleLButtonUp(&s, s.hwnd, WM_LBUTTONUP, 0, point);
                    }
                    check(tab.current_path == address && tab.view_generation == generation &&
                          tab.back_stack.empty() && tab.pending_generation == 42,
                        "actual breadcrumb mouse handlers preserve virtual location, history and pending work");
                }
                ShowAddressEditor(s);
                wchar_t text[1024]{};
                GetWindowTextW(s.hwndAddressEdit, text, ARRAYSIZE(text));
                check(s.addressEditing && std::wstring(text) == ClipboardPath(address),
                    "native address editor contains the address rather than the localized title");
                HideAddressEditor(s, true);
                check(tab.current_path == address && tab.view_generation == generation &&
                      tab.back_stack.empty() && tab.pending_generation == 42,
                    "submitting the unchanged native address editor keeps the original destination");
            }
        }
    }
    check(FormatAddressPath(L" \"\" ").empty(), "an empty submitted address retains This PC identity");
    check(FormatAddressPath(L" \"C:\" ") == L"C:\\", "typed drive roots retain their existing formatting");
    s.watches.Stop();
    if (s.hwndAddressEdit) DestroyWindow(s.hwndAddressEdit);
    s.hwndAddressEdit = nullptr;
    s.renderer.SetCompositor(nullptr);
    s.compositor.Shutdown();
    DestroyWindow(s.hwnd);
    s.hwnd = nullptr;
    owned.reset();
    OleUninitialize();
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
