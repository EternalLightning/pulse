#include "../app/app_internal.h"
#include "../common/windows_compat.h"
#include "../ui/ui_renderer_internal.h"
#include "../ui/lumatext_renderer.h"
#include "snapshot_output.h"
#include <exception>
#include <filesystem>
#include <iostream>
#include <string_view>

LRESULT CALLBACK WndProcImpl(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

namespace {
int checks = 0, failures = 0;
void Check(bool ok, const char* label) {
    ++checks;
    failures += !ok;
    std::cout << (ok ? "[PASS] " : "[FAIL] ") << label << '\n';
}
LRESULT CALLBACK BackendMessages(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                                UINT_PTR, DWORD_PTR count) {
    if (msg == pulse::ui::typography::TextBackendChangedMessage()) {
        ++*reinterpret_cast<int*>(count);
        return WndProcImpl(hwnd, msg, wp, lp);
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

int PlainTextBackendTest() {
    using namespace pulse::ui;
    const auto output = std::filesystem::absolute(L"docs/validation/font-popup-20261024/plain-text");
    if (!pulse::bench::StreamSnapshots()) std::filesystem::create_directories(output);
    HWND window = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"STATIC", L"Plain text fixture",
        WS_POPUP, -30000, -30000, 900, 620, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    Compositor compositor;
    Check(window && compositor.Init(window), "plain text fixture initializes the production compositor");
    if (!compositor.Dc()) {
        if (window) DestroyWindow(window);
        return 1;
    }
    Check(compositor.LumaTextAvailable(), "plain text fixture loads the real LumaText backend");
    fluent::Painter painter(&compositor);
    MainRenderer renderer;
    renderer.SetCompositor(&compositor);
    renderer.SetDetailsPanelVisible(false);
    for (float scale : {1.0f, 1.5f}) for (bool dark : {false, true}) for (int mode : {0, 1, 2}) {
        typography::SetTextRenderMode(static_cast<typography::TextRenderMode>(mode));
        compositor.Resize(static_cast<int>(900 * scale), static_cast<int>(620 * scale));
        compositor.RecreateTextFormats(scale);
        painter.SetScale(scale);
        const auto theme = MakeTheme(dark, HexColor(0x0078D4));
        auto* dc = compositor.Dc();
        dc->BeginDraw();
        dc->Clear(theme.bg);
        painter.BeginFrame(theme);
        ComPtr<ID2D1SolidColorBrush> brush;
        dc->CreateSolidColorBrush(theme.text, &brush);
        auto check_backend = [&](auto draw, const char* label) {
            const auto* stats = compositor.GetLumaTextStats();
            const auto before = stats ? stats->draw_calls : 0;
            draw();
            stats = compositor.GetLumaTextStats();
            const auto after = stats ? stats->draw_calls : 0;
            Check(mode == 0 ? after > before : after == before, label);
        };
        auto rect = [scale](float x, float y, float w, float h) {
            return D2D1::RectF(x * scale, y * scale, (x + w) * scale, (y + h) * scale);
        };
        check_backend([&] {
            DrawTextEndEllipsis(&compositor, theme.bg, dc, compositor.DwriteFactory(), compositor.TextFormat(),
                brush.get(), L"Short status 短状态", 20 * scale, 20 * scale, 440 * scale, 28 * scale);
        }, "short status uses LumaText only in Auto");
        check_backend([&] {
            DrawTextEndEllipsis(&compositor, theme.bg, dc, compositor.DwriteFactory(), compositor.TextFormat(),
                brush.get(), L"A long localized operation status 操作状态 must retain a complete ellipsis", 20 * scale,
                60 * scale, 190 * scale, 28 * scale);
        }, "ellipsized status uses LumaText only in Auto");
        check_backend([&] {
            DrawTabTitle(&compositor, theme.bg, dc, compositor.DwriteFactory(), compositor.TabFormat(),
                brush.get(), L"Documents 文档", 20 * scale, 100 * scale, 440 * scale, 32 * scale, scale);
        }, "short tab title uses LumaText only in Auto");
        check_backend([&] {
            DrawTabTitle(&compositor, theme.bg, dc, compositor.DwriteFactory(), compositor.TabFormat(),
                brush.get(), L"Very long report 文件名 with a preserved extension.pdf", 20 * scale,
                140 * scale, 210 * scale, 32 * scale, scale);
        }, "long tab title and extension use LumaText only in Auto");
        check_backend([&] {
            fluent::SidebarItemSpec item;
            item.bounds = rect(20, 190, 420, 36);
            item.text = L"Downloads 下载";
            item.detail = L"12 items";
            item.show_count = true;
            item.badge_count = 8;
            painter.DrawSidebarItem(item);
        }, "sidebar name, detail and count follow the selected backend");
        check_backend([&] {
            fluent::SidebarSectionHeaderSpec section;
            section.bounds = rect(20, 235, 420, 32);
            section.text = L"Pinned folders 固定文件夹";
            painter.DrawSidebarSectionHeader(section);
        }, "sidebar section heading follows the selected backend");
        check_backend([&] {
            fluent::DriveSidebarItemSpec drive;
            drive.bounds = rect(20, 280, 420, 70);
            drive.name = L"Workspace 工作盘";
            drive.detail = L"128 GB free";
            drive.skip_glyph = true;
            painter.DrawDriveSidebarItem(drive);
        }, "drive name and capacity follow the selected backend");
        for (const auto paragraph : {DWRITE_PARAGRAPH_ALIGNMENT_NEAR, DWRITE_PARAGRAPH_ALIGNMENT_CENTER,
                                     DWRITE_PARAGRAPH_ALIGNMENT_FAR}) {
            const float x = 20.0f + static_cast<int>(paragraph) * 260.0f;
            const auto bounds = rect(x, 390, 240, 90);
            brush->SetColor(theme.stroke_card);
            dc->DrawRectangle(bounds, brush.get());
            compositor.TextFormat()->SetParagraphAlignment(paragraph);
            check_backend([&] {
                constexpr std::wstring_view text = L"Same text 相同文字";
                if (!compositor.DrawLumaText(text, compositor.TextFormat(), bounds, theme.text, theme.bg)) {
                    brush->SetColor(theme.text);
                    dc->DrawText(text.data(), static_cast<UINT32>(text.size()), compositor.TextFormat(),
                        bounds, brush.get(), D2D1_DRAW_TEXT_OPTIONS_CLIP, typography::MeasuringMode());
                }
            }, "near, center and far paragraph placement follow the selected backend");
        }
        compositor.TextFormat()->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        brush->SetColor(theme.text);
        float unicode_y = 510.0f;
        for (const std::wstring& text : {std::wstring(L"A\u0301A\u0301A\u0301A\u0301A\u0301A\u0301A\u0301A\u0301A\u0301A\u0301A\u0301A\u0301"),
                std::wstring(L"\U00020164\U00020164\U00020164\U00020164\U00020164\U00020164\U00020164\U00020164\U00020164\U00020164\U00020164\U00020164")}) {
            const auto* stats = compositor.GetLumaTextStats();
            const auto repairs_before = stats ? stats->end_ellipsis_repairs : 0;
            check_backend([&] {
                DrawTextEndEllipsis(&compositor, theme.bg, dc, compositor.DwriteFactory(), compositor.TextFormat(),
                    brush.get(), text, 20 * scale, unicode_y * scale, 86 * scale, 28 * scale);
            }, "combining and supported supplementary-plane ellipsis preserve the selected text backend");
            stats = compositor.GetLumaTextStats();
            if (mode == 0 && stats && stats->end_ellipsis_repairs > repairs_before) {
                const size_t prefix = stats->last_ellipsis_prefix_length;
                Check(prefix < text.size() &&
                    (prefix == 0 || !IS_HIGH_SURROGATE(text[prefix - 1])) &&
                    !IS_LOW_SURROGATE(text[prefix]) && text[prefix] != L'\u0301',
                    "explicit Luma ellipsis cuts at a complete surrogate pair or combining cluster");
            }
            unicode_y += 38.0f;
        }
        const auto* emoji_stats = compositor.GetLumaTextStats();
        const auto emoji_draws_before = emoji_stats ? emoji_stats->draw_calls : 0;
        const auto emoji_fallbacks_before = emoji_stats ? emoji_stats->fallback_draws : 0;
        const std::wstring emoji = L"\U0001F600\U0001F600\U0001F600\U0001F600\U0001F600\U0001F600\U0001F600\U0001F600";
        DrawTextEndEllipsis(&compositor, theme.bg, dc, compositor.DwriteFactory(), compositor.TextFormat(),
            brush.get(), emoji, 20 * scale, unicode_y * scale, 86 * scale, 28 * scale);
        emoji_stats = compositor.GetLumaTextStats();
        Check(emoji_stats && emoji_stats->draw_calls == emoji_draws_before &&
            (mode == 0 ? emoji_stats->fallback_draws > emoji_fallbacks_before :
                         emoji_stats->fallback_draws == emoji_fallbacks_before),
            "emoji outside the configured Luma font cascades retains the real DirectWrite fallback");
        Check(SUCCEEDED(dc->EndDraw()), "plain text frame completes without a D2D error");
        const auto name = L"plain-" + std::to_wstring(static_cast<int>(scale * 100)) +
            (dark ? L"-dark-" : L"-light-") + std::to_wstring(mode) + L".png";
        Check(pulse::bench::SaveSnapshot(compositor, output / name), "plain text backend snapshot is saved");

        renderer.SetScale(scale);
        WindowViewModel breadcrumb_vm;
        breadcrumb_vm.dark = dark;
        breadcrumb_vm.safe_mode = true;
        breadcrumb_vm.window_effect = WindowEffect::None;
        breadcrumb_vm.pane.is_file_system = true;
        const std::wstring long_path = L"\\\\server\\share\\资料\\" + std::wstring(100, L'x') + L"-项目报告";
        for (const auto& path : {std::wstring(L"E:\\Projects\\资料"), long_path,
                std::wstring(L"pulse:archive:E:\\fixtures\\资料.zip|assets/images/")})
        for (float width : {620.0f, 1100.0f}) {
            breadcrumb_vm.pane.path = path;
            const auto bounds = D2D1::RectF(0, 0, width * scale, 620 * scale);
            const auto address = renderer.AddressBarRect(bounds.right);
            std::vector<MainRenderer::BreadcrumbPlaced> placed;
            renderer.BreadcrumbLayout(breadcrumb_vm.pane, bounds.right, placed);
            Check(!placed.empty(), "local, UNC and archive breadcrumbs keep a visible segment at both widths");
            const auto source = SplitBreadcrumb(path);
            Check(!placed.empty() && !source.empty() && placed.back().path == source.back().path,
                "breadcrumb fitting preserves the final navigation target");
            bool truncated = false;
            for (size_t i = 0; i < placed.size(); ++i) {
                const auto& segment = placed[i];
                const float width_available = std::max(0.0f, segment.rc.right - segment.rc.left - 16 * scale);
                Check(segment.rc.left >= address.left && segment.rc.right <= address.right &&
                    typography::MeasureLine(&compositor, compositor.AddressFormat(), segment.text) <= width_available + 0.5f,
                    "fitted breadcrumb ink and padding remain within the segment bounds");
                const float x = std::min((segment.rc.left + segment.rc.right) * 0.5f, address.right - 29 * scale);
                const auto hit = renderer.HitTest(breadcrumb_vm, bounds, x, (segment.rc.top + segment.rc.bottom) * 0.5f);
                Check(x >= segment.rc.left && hit.region == HitTestResult::BreadcrumbSegment &&
                    hit.index == static_cast<int>(i) && hit.path == segment.path,
                    "breadcrumb drawing and production hit testing share the fitted segment and original path");
                if (segment.text != segment.full_text) {
                    truncated = true;
                    Check(hit.label == segment.full_text, "truncated breadcrumb hit testing retains its full tooltip label");
                }
            }
            if (path == long_path) Check(truncated, "long UNC breadcrumb exercises actual end ellipsis");
        }
        breadcrumb_vm.pane.path = long_path;
        const auto breadcrumb_bounds = D2D1::RectF(0, 0, 900 * scale, 620 * scale);
        const auto draw_breadcrumb_frame = [&](bool editing) {
            breadcrumb_vm.address_editing = editing;
            const auto* stats = compositor.GetLumaTextStats();
            const auto before = stats ? stats->draw_calls : 0;
            dc->BeginDraw();
            renderer.Render(breadcrumb_vm, breadcrumb_bounds, theme);
            Check(SUCCEEDED(dc->EndDraw()), "production breadcrumb frame completes without a D2D error");
            stats = compositor.GetLumaTextStats();
            return (stats ? stats->draw_calls : 0) - before;
        };
        const auto editing_draws = draw_breadcrumb_frame(true);
        const auto breadcrumb_draws = draw_breadcrumb_frame(false);
        Check(mode == 0 ? breadcrumb_draws > editing_draws : breadcrumb_draws == editing_draws,
            "visible production breadcrumbs add LumaText draws only in Auto");
        Check(pulse::bench::SaveSnapshot(compositor, output / (L"breadcrumb-" + name)),
            "production breadcrumb backend snapshot is saved");
    }
    compositor.Shutdown();
    DestroyWindow(window);
    std::cout << "[SUMMARY] " << checks-failures << " PASS / " << failures << " FAIL\n";
    return failures ? 1 : 0;
}

pulse::ui::WindowViewModel FileListFixture(pulse::ui::MainRenderer& renderer,
                                         float scale, bool chinese, bool dark) {
    using namespace pulse;
    ui::WindowViewModel vm;
    vm.dark = dark;
    vm.safe_mode = true;
    vm.window_effect = ui::WindowEffect::None;
    vm.window_title = chinese ? L"文字渲染对照" : L"Text rendering comparison";
    vm.tabs.push_back({vm.window_title, true});
    vm.status.hint_text = chinese ? L"左侧：普通列表  ·  右侧：搜索高亮" :
        L"Left: ordinary file list  ·  Right: search highlights";
    const auto content = renderer.ContentRect(1640 * scale, 900 * scale);
    const float middle = (content.left + content.right) * 0.5f;
    const wchar_t* names[] = {L"appcompat", L"Downloaded Program Files", L"Program",
        L"README.txt", L"ordinary-file.txt", L"report-report.txt", L"中国报告-报告.txt",
        L"Program-report-very-long-file-name-with-many-characters-that-must-be-truncated.txt",
        L"Drawing1234567890-very-long-file-name-设计图纸-final-version.dwg"};
    for (int side : {0, 1}) {
        ui::PaneSlotView slot;
        slot.rect = D2D1::RectF(side == 0 ? content.left : middle + 4 * scale,
            content.top, side == 0 ? middle - 4 * scale : content.right, content.bottom);
        slot.focused = side == 0;
        auto& pane = slot.pane;
        pane.header_text = chinese ? (side == 0 ? L"普通文件列表" : L"搜索：Program report 报告") :
            (side == 0 ? L"Ordinary file list" : L"Search: Program report 报告");
        pane.view_mode = ui::ViewMode::Details;
        pane.group_mode = ui::GroupMode::None;
        pane.is_file_system = side == 0;
        pane.is_search = side == 1;
        pane.search_query = side == 1 ? L"Program report 报告" : L"";
        // Resident presentation rows have no source paths, so the production
        // renderer uses generic icons without resolving any user directory.
        for (size_t i = 0; i < std::size(names); ++i) {
            ui::ListEntryView entry;
            entry.name = names[i];
            entry.is_dir = i < 3;
            entry.attrs = entry.is_dir ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
            entry.type_text = l10n::Get(entry.is_dir ? l10n::StringId::TypeFolder :
                l10n::StringId::TypeTextDocument);
            entry.size_text = entry.is_dir ? L"" : L"12.5 KB";
            entry.date_text = L"2026-10-24 09:30";
            pane.entries.push_back(std::move(entry));
        }
        vm.pane_slots.push_back(std::move(slot));
    }
    vm.pane = vm.pane_slots.front().pane;
    return vm;
}
}

int main(int argc, char** argv) try {
    using namespace pulse;
    using I = l10n::StringId;
    using H = ui::HitTestResult;
    std::cout << std::unitbuf;
    OleInitialize(nullptr);
    compat::EnableDpiAwareness();
    l10n::Initialize(GetModuleHandleW(nullptr), L"en-US");
    const auto previous_mode = ui::typography::CurrentTextRenderMode();
    if (argc == 2 && std::string_view(argv[1]) == "--plain-text-only") {
        const int result = PlainTextBackendTest();
        ui::typography::SetTextRenderMode(previous_mode);
        OleUninitialize();
        return result;
    }
    ui::typography::SetTextRenderMode(ui::typography::TextRenderMode::Auto);
    app::AppPrefs prefs;
    prefs.persist = false;
    Check(prefs.text_render == 0, "new preferences default to automatic rendering");
    for (int mode : {0, 1, 2}) {
        prefs.text_render = mode;
        Check(prefs.Save(), "isolated preference save succeeds without touching user storage");
        app::AppPrefs restored;
        restored.persist = false;
        Check(restored.FromJson(prefs.ToJson()) && restored.text_render == mode,
            "all three render modes survive the production JSON round trip");
    }
    for (const wchar_t* json : {L"{}", L"{\"text_render\":-1}", L"{\"text_render\":3}",
                               L"{\"text_render\":2147483647}", L"{\"text_render\":\"bad\"}"}) {
        prefs.text_render = 2;
        Check(prefs.FromJson(json) && prefs.text_render == 0,
            "missing or invalid persisted rendering values fall back to Auto");
    }
    prefs.text_render = 42;
    app::AppPrefs restored;
    restored.persist = false;
    Check(restored.FromJson(prefs.ToJson()) && restored.text_render == 0,
        "serialization refuses an invalid in-memory rendering mode");
    prefs.text_render = 2;
    prefs.ResetToDefaults();
    Check(prefs.text_render == 0, "reset restores automatic rendering");

    const auto output = std::filesystem::absolute(L"bench_data/text-render-settings-20261024");
    std::filesystem::create_directories(output);
    auto first = std::make_unique<AppState>();
    auto second = std::make_unique<AppState>(first->core);
    int first_messages = 0, second_messages = 0;
    int* counters[] = {&first_messages, &second_messages};
    AppState* windows[] = {first.get(), second.get()};
    bool ready = true;
    for (size_t i = 0; i < std::size(windows); ++i) {
        auto& s = *windows[i];
        s.isolatedTest = true;
        s.appPrefs.persist = s.places.persist = s.ctxMenuPrefs.persist = s.searchHistory.persist = false;
        s.accentColor = ui::HexColor(0x0078D4);
        s.hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"STATIC", L"Text rendering fixture",
            WS_POPUP, -30000, -30000, 1100, 900, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        const bool initialized = s.hwnd && s.compositor.Init(s.hwnd);
        Check(initialized, "isolated window initializes its production compositor");
        if (!initialized) { ready = false; break; }
        s.renderer.SetCompositor(&s.compositor);
        s.core.windows.Add(s.hwnd, &s);
        SetWindowLongPtrW(s.hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&s));
        SetWindowSubclass(s.hwnd, BackendMessages, 1, reinterpret_cast<DWORD_PTR>(counters[i]));
        s.window_tabs.EnsureDefault();
        s.pane = s.window_tabs.Active()->FocusedPane();
        s.pane->NewTab(app::MakeSettingsPath(L"general"));
        ActiveTab(s)->loading = false;
        ActiveTab(s)->SetSnapshot(std::make_shared<std::vector<fs::DirEntry>>());
    }
    if (ready) {
        auto& s = *first;
        s.compositor.RecreateTextFormats(s.scale);
        Check(s.compositor.LumaTextAvailable(),"the production compositor loads the updated LumaText SDK");
        for(const wchar_t* language:{L"zh-CN",L"en-US"}) {
            l10n::SetLanguage(language);
            ui::ComPtr<IDWriteTextFormat> format;
            Check(SUCCEEDED(ui::typography::CreateTextFormat(s.compositor.DwriteFactory(),{},&format)),
                "shared typography creates a UI font in both languages");
            wchar_t family[128]{};
            if(format.get()) format->GetFontFamilyName(family,128);
            Check(std::wstring_view(family).starts_with(L"Microsoft YaHei"),
                "UI glyphs use Microsoft YaHei independently of the selected UI language");
        }
        for(int mode:{0,1,2}) {
            ui::typography::SetTextRenderMode(static_cast<ui::typography::TextRenderMode>(mode));
            ui::ComPtr<IDWriteTextLayout> layout;
            const wchar_t sample[]=L"Mixed 中文 filename-report.txt";
            const auto* stats=s.compositor.GetLumaTextStats();
            const auto before=stats?stats->draw_calls:0;
            const auto hr=ui::typography::CreateTextLayout(s.compositor.DwriteFactory(),sample,
                static_cast<UINT32>(std::size(sample)-1),s.compositor.TextFormat(),600,50,&layout);
            Check(SUCCEEDED(hr),"colored text layout preserves DirectWrite shaping");
            ui::ComPtr<ID2D1SolidColorBrush> brush,effect;
            s.compositor.Dc()->CreateSolidColorBrush(D2D1::ColorF(0.1f,0.1f,0.1f),&brush);
            s.compositor.Dc()->CreateSolidColorBrush(D2D1::ColorF(0.1f,0.4f,0.8f),&effect);
            if(layout.get()) layout->SetDrawingEffect(effect.get(),{6,2});
            s.compositor.Dc()->BeginDraw();
            s.compositor.Dc()->Clear(D2D1::ColorF(D2D1::ColorF::White));
            s.compositor.DrawTextLayout(D2D1::Point2F(10,10),layout.get(),brush.get(),D2D1_DRAW_TEXT_OPTIONS_CLIP);
            Check(SUCCEEDED(s.compositor.Dc()->EndDraw()),"colored layout bridge renders successfully");
            stats=s.compositor.GetLumaTextStats();
            const auto after=stats?stats->draw_calls:0;
            Check(mode==0?after>before:after==before,
                "colored layouts use the CoreText-style backend only in Auto mode");
            HWND edit=CreateWindowExW(0,L"EDIT",sample,WS_CHILD|ES_AUTOHSCROLL,0,0,520,32,
                s.hwnd,nullptr,GetModuleHandleW(nullptr),nullptr);
            ui::ComPtr<ID2D1Bitmap1> bitmap;
            ui::LumaTextRenderer edit_renderer;
            Check(edit_renderer.Init(s.compositor.DwriteFactory(),s.compositor.Dc()),"isolated edit renderer initializes the updated SDK");
            const auto before_edit=edit_renderer.Stats().edit_lumatext_draws;
            Check(edit && edit_renderer.PaintEditBitmap(edit,s.compositor.TextFormat(),
                D2D1::ColorF(0.1f,0.1f,0.1f),D2D1::ColorF(D2D1::ColorF::White),&bitmap),
                "native edit text is painted while preserving its original layout and contents");
            const auto after_edit=edit_renderer.Stats().edit_lumatext_draws;
            Check(mode==0?after_edit>before_edit:after_edit==before_edit,
                "edit rasterization follows Auto while Sharp and Smooth keep DirectWrite");
            ui::ComPtr<IDWriteTextLayout> caret_layout;
            const auto caret_hr=ui::typography::CreateTextLayout(s.compositor.DwriteFactory(),sample,
                static_cast<UINT32>(std::size(sample)-1),s.compositor.TextFormat(),10000,32,&caret_layout);
            float caret_x=0,caret_y=0;DWRITE_HIT_TEST_METRICS caret_metrics{};
            const bool caret_ready=SUCCEEDED(caret_hr) && SUCCEEDED(caret_layout->HitTestTextPosition(
                6,FALSE,&caret_x,&caret_y,&caret_metrics));
            Check(caret_ready,"native edit caret has a position in the preserved shaping layout");
            if(edit && caret_ready) {
                const auto point=MAKELPARAM(static_cast<int>(std::lround(caret_x+3)),16);
                edit_renderer.CallEditDefaultMouse(edit,WM_LBUTTONDOWN,MK_LBUTTON,point,s.compositor.TextFormat());
                edit_renderer.CallEditDefaultMouse(edit,WM_LBUTTONUP,0,point,s.compositor.TextFormat());
                DWORD begin=0,end=0;SendMessageW(edit,EM_GETSEL,reinterpret_cast<WPARAM>(&begin),reinterpret_cast<LPARAM>(&end));
                Check(begin==6 && end==6,"clicking the rendered Chinese character puts the native caret at that character");
            }
            wchar_t text[128]{};if(edit) GetWindowTextW(edit,text,128);
            Check(std::wstring_view(text)==sample,"custom rasterization leaves the native editor text unchanged");
            edit_renderer.Shutdown();
            if(edit) DestroyWindow(edit);
        }
        ui::typography::SetTextRenderMode(ui::typography::TextRenderMode::Auto);
        first_messages=second_messages=0;
        app::SettingsController::UiCallbacks callbacks;
        callbacks.apply_effects = [&](app::SettingsEffect effect) { ApplySettingsEffects(s, effect); };
        s.settings.BindUi(s.appPrefs, s.ctxMenuPrefs, s.index, s.networkIndex, std::move(callbacks));
        s.settings.TextRendering(-1);
        s.settings.TextRendering(3);
        Check(s.appPrefs.text_render == 0 && first_messages == 0 && second_messages == 0,
            "out-of-range settings indices leave preferences and both windows unchanged");
        ui::fluent::Painter measuring(&s.compositor);
        for (const wchar_t* language : {L"zh-CN", L"en-US"})
        for (float scale : {1.0f, 1.5f}) for (bool dark : {false, true})
        for (int width : {620, 1100}) {
            s.appPrefs.language = language;
            l10n::SetLanguage(language);
            ui::typography::InvalidateCaches();
            s.scale = scale;
            s.darkMode = dark;
            s.renderer.SetScale(scale);
            s.compositor.RecreateTextFormats(scale);
            s.compositor.Resize(static_cast<UINT>(width * scale), static_cast<UINT>(900 * scale));
            const auto theme = ui::MakeTheme(dark, s.accentColor);
            const auto bounds = D2D1::RectF(0, 0, width * scale, 900 * scale);
            measuring.SetScale(scale);
            measuring.BeginFrame(theme);
            s.settings.SetScroll(0, 100000);
            auto vm = BuildVm(s, false);
            auto layout = ui::MakeSettingsLayout(vm, bounds, scale, s.renderer.TitleBarHeight(), 28 * scale, &measuring);
            s.settings.SetScroll(layout.text_render_card.top - layout.content.top - 20 * scale, 100000);
            vm = BuildVm(s, false);
            layout = ui::MakeSettingsLayout(vm, bounds, scale, s.renderer.TitleBarHeight(), 28 * scale, &measuring);
            Check(layout.text_render_card.top >= layout.content.top &&
                layout.text_render_card.bottom <= layout.content.bottom,
                "the complete rendering row scrolls into the visible settings viewport");
            const bool stacked = layout.text_render_row[0].left < layout.text_render_card.left + 100 * scale;
            const float right = stacked ? layout.text_render_card.right - 16 * scale : layout.text_render_row[0].left - 12 * scale;
            const float caption = ui::SettingsCaptionHeight(l10n::Get(I::SettingsTextRenderDesc),
                right - layout.text_render_card.left - 54 * scale, scale, &measuring);
            Check(layout.text_render_card.top + 35 * scale + caption <=
                (stacked ? layout.text_render_row[0].top - 10 * scale : layout.text_render_card.bottom - 9 * scale),
                "localized rendering description fits without overlapping the segments");
            for (int mode : {1, 2, 0}) {
                const auto choice = layout.text_render_row[mode];
                const float x = (choice.left + choice.right) * 0.5f, y = (choice.top + choice.bottom) * 0.5f;
                const auto hit = s.renderer.HitTest(vm, bounds, x, y);
                Check(hit.region == H::SettingsTextRender && hit.index == mode,
                    "production hit testing reaches the intended rendering segment");
                Check(measuring.MeasureButtonWidth(l10n::Get(mode == 0 ? I::TextRenderAuto :
                    mode == 1 ? I::TextRenderSharp : I::TextRenderSmooth)) <= choice.right - choice.left,
                    "the localized segment label fits its control");
                const int before_first = first_messages, before_second = second_messages;
                const auto generation = ui::typography::Generation();
                HandleLButtonDown(&s, s.hwnd, WM_LBUTTONDOWN, MK_LBUTTON,
                    MAKELPARAM(static_cast<int>(x), static_cast<int>(y)));
                Check(s.appPrefs.text_render == mode && static_cast<int>(ui::typography::CurrentTextRenderMode()) == mode &&
                    ui::typography::Generation() > generation,
                    "native settings input immediately applies the selected rendering mode");
                Check(first_messages > before_first && second_messages > before_second,
                    "a settings change refreshes both production window message handlers");
                vm = BuildVm(s, false);
                Check(vm.settings_text_render == mode, "the view model immediately reflects the selected rendering mode");
                s.compositor.Dc()->BeginDraw();
                s.renderer.Render(vm, bounds, theme);
                Check(SUCCEEDED(s.compositor.Dc()->EndDraw()), "the production settings frame renders in every selected mode");
                const auto name = L"settings-" + std::wstring(language) + L"-" + std::to_wstring(width) + L"-" +
                    std::to_wstring(static_cast<int>(scale * 100)) + (dark ? L"-dark-" : L"-light-") +
                    std::to_wstring(mode) + L".png";
                Check(bench::SaveSnapshot(s.compositor,output/name), "each rendering mode has a settings snapshot for visual review");
            }
        }
        s.settings.ResetUi();
        s.renderer.SetDetailsPanelVisible(false);
        s.renderer.SetListStyle(false, true, false);
        for (const wchar_t* language : {L"zh-CN", L"en-US"})
        for (float scale : {1.0f, 1.25f, 1.5f}) for (bool dark : {false, true})
        for (int mode : {0, 1, 2}) {
            l10n::SetLanguage(language);
            ui::typography::InvalidateCaches();
            s.scale = scale;
            s.darkMode = dark;
            s.renderer.SetScale(scale);
            s.compositor.Resize(static_cast<UINT>(1640 * scale), static_cast<UINT>(900 * scale));
            ui::typography::SetTextRenderMode(static_cast<ui::typography::TextRenderMode>(mode));
            s.compositor.RecreateTextFormats(scale);
            s.renderer.InvalidateTypography();
            auto vm = FileListFixture(s.renderer, scale, std::wstring_view(language) == L"zh-CN", dark);
            const auto& search = vm.pane_slots[1].pane;
            const auto terms = ui::NameHighlightTerms(L"", search.search_query);
            Check(!ui::NameMatchRanges(L"report-report.txt", terms).empty() &&
                !ui::NameMatchRanges(L"中国报告-报告.txt", terms).empty(),
                "the actual search pane contains English and Chinese filename highlight spans");
            const auto bounds = D2D1::RectF(0, 0, 1640 * scale, 900 * scale);
            s.compositor.Dc()->BeginDraw();
            s.renderer.Render(vm, bounds, ui::MakeTheme(dark, s.accentColor));
            Check(SUCCEEDED(s.compositor.Dc()->EndDraw()),
                "the production file list and search-highlight panes render in every mode, theme and DPI");
            const auto name = L"file-list-" + std::wstring(language) + L"-" +
                std::to_wstring(static_cast<int>(scale * 100)) + (dark ? L"-dark-" : L"-light-") +
                std::to_wstring(mode) + L".png";
            Check(bench::SaveSnapshot(s.compositor,output/name),
                "a real MainRenderer file-list and search-highlight snapshot is saved for review");
        }
    }
    ui::typography::SetTextRenderMode(previous_mode);
    for (auto* window : windows) {
        if (!window->hwnd) continue;
        window->core.windows.Remove(window->hwnd);
        RemoveWindowSubclass(window->hwnd, BackendMessages, 1);
        SetWindowLongPtrW(window->hwnd, GWLP_USERDATA, 0);
        window->renderer.SetCompositor(nullptr);
        window->compositor.Shutdown();
        DestroyWindow(window->hwnd);
        window->hwnd = nullptr;
    }
    second.reset();
    first.reset();
    OleUninitialize();
    std::cout << "[SUMMARY] " << checks - failures << " PASS / " << failures << " FAIL\n";
    return failures ? 1 : 0;
} catch (const std::exception& error) {
    std::cerr << "[FAIL] text rendering fixture exception: " << error.what() << '\n';
    return 1;
}
