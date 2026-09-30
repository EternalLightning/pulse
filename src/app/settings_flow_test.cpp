#include "app_internal.h"
#include "app_input.h"
#include "../ui/ui_renderer_internal.h"
#include <fstream>
#include <filesystem>

#ifdef PULSE_WITH_SELFTEST
namespace pulse { bool TestSettingsFilter(const std::wstring&,l10n::StringId); }
using namespace pulse;
int RunSettingsWidthBorderTest(AppState& s, const wchar_t* output) {
    using H = ui::HitTestResult;
    std::ofstream log{std::filesystem::path(output)};
    int failures = 0;
    auto check = [&](bool ok, const char* label) {
        log << (ok ? "[PASS] " : "[FAIL] ") << label << '\n';
        if (!ok) ++failures;
    };
    check((s.settingsExpanded & ui::kSettingsContextExpandedMask) == ui::kSettingsContextExpandedMask,
        "all context-menu groups start expanded");
    const float original_scale = s.scale;
    for (float scale : {1.0f, 1.25f, 1.5f, 2.0f}) {
        s.compositor.RecreateTextFormats(scale);
        s.renderer.SetScale(scale);
        for (float width : {760.0f, 1280.0f, 3045.0f}) {
            auto vm = BuildVm(s, false);
            vm.settings_open = true;
            vm.settings_scroll = 0;
            const auto window = D2D1::RectF(0, 0, width*scale, 1464*scale);
            for (int page = 0; page < 5; ++page) {
                vm.settings_page = page;
                const auto l = ui::MakeSettingsLayout(vm, window, scale,
                    s.renderer.TitleBarHeight(), 28*scale, nullptr);
                check(l.content.left == l.nav.right && l.content.right == window.right,
                    "settings use available width at every window size and DPI");
                const auto hit = s.renderer.HitTest(vm, window,
                    (l.nav_row[page].left+l.nav_row[page].right)/2,
                    (l.nav_row[page].top+l.nav_row[page].bottom)/2);
                check(hit.region == H::SettingsNav && hit.index == page,
                    "page navigation matches the resized layout");
            }
        }
    }
    s.compositor.RecreateTextFormats(original_scale);
    s.renderer.SetScale(original_scale);
    OpenSettingsTab(s, 2);
    const auto group = ipc::CtxMenuGroup::Software;
    const bool enabled = s.ctxMenuPrefs.GroupEnabled(group);
    H hit; hit.region = H::SettingsDisclosure; hit.index = 8;
    HandleSettingsControl(s, hit);
    check(!(s.settingsExpanded & (1u<<8)), "default-expanded group can be collapsed");
    HandleSettingsControl(s, hit);
    check((s.settingsExpanded & ui::kSettingsContextExpandedMask) == ui::kSettingsContextExpandedMask &&
        s.ctxMenuPrefs.GroupEnabled(group) == enabled, "expansion preserves context-menu preferences");

    // Render the real switch painter at fractional offsets and common DPI scales.
    s.compositor.Resize(640, 420);
    auto* dc = s.compositor.Dc();
    dc->BeginDraw();
    const auto theme = ui::MakeTheme(true, s.accentColor);
    dc->Clear(theme.bg);
    ui::fluent::Painter painter(&s.compositor);
    int row = 0;
    for (float scale : {1.0f, 1.25f, 1.5f, 2.0f}) {
        s.compositor.RecreateTextFormats(scale);
        painter.SetScale(scale);
        painter.BeginFrame(theme);
        const float top = 20.25f + row*94.0f;
        painter.DrawText(std::to_wstring(static_cast<int>(scale*100)) + L"%",
            D2D1::RectF(16, top, 100, top+40), s.compositor.TextFormat(), theme.text);
        for (int state = 0; state < 4; ++state) {
            ui::fluent::ControlState control{};
            control.checked = state==1;
            control.hovered = state==2;
            control.enabled = state!=3;
            painter.DrawSwitch(D2D1::RectF(120.25f+state*126,top,232.0f+state*126.0f,top+40*scale),L"",control);
        }
        ++row;
    }
    check(SUCCEEDED(dc->EndDraw()), "switch states render without Direct2D errors");
    const auto png = std::filesystem::path(output).parent_path()/L"switch-borders.png";
    check(s.compositor.SaveSnapshot(png.c_str()), "switch border DPI comparison captured");
    log << "failures=" << failures << '\n';
    return failures ? 1 : 0;
}

void Render(pulse::AppState&);
int RunSettingsDropdownTest(AppState& s,const wchar_t* output) {
    using H=ui::HitTestResult;
    std::ofstream log{std::filesystem::path(output)};int failures=0;
    auto check=[&](bool ok,const char* label) {log<<(ok ? "[PASS] " : "[FAIL] ")<<label<<std::endl;if(!ok)++failures;};
    const auto base=std::filesystem::path(output).parent_path();
    s.appPrefs.persist=false;
    OpenSettingsTab(s,0);s.settings.SetScroll(0,0);
    s.settings.WindowEffect(ui::WindowEffectId(ui::WindowEffect::None));
    s.settings.Language(L"en-US");
    Render(s);check(s.compositor.SaveSnapshot((base/L"closed.png").c_str()),"closed dropdown screenshot captured");
    for(int index=0;index<2;++index) {
        const auto vm=BuildVm(s,false);
        const auto control=s.renderer.SettingsDropdownBounds(vm,index,
            static_cast<float>(s.compositor.Width()),static_cast<float>(s.compositor.Height()));
        const auto hit=s.renderer.HitTest(vm,D2D1::RectF(0,0,static_cast<float>(s.compositor.Width()),static_cast<float>(s.compositor.Height())),
            control.left+4*s.scale,(control.top+control.bottom)/2);
        check(hit.region==H::SettingsDropdown && hit.index==index,"dropdown hit target matches the rendered control");
        POINT origin{static_cast<LONG>(std::lround(control.left)),static_cast<LONG>(std::lround(control.bottom))};
        ClientToScreen(s.hwnd,&origin);
        static RECT popup{};popup={};
        static int down=0;down=index;
        const auto png=base/(index==0 ? L"effect-menu.png" : L"language-menu.png");
        SetEnvironmentVariableW(L"PULSE_TEST_DROPDOWN_SHOT",png.c_str());
        SetTimer(s.hwnd,0x5347,350,[](HWND hwnd,UINT,UINT_PTR id,DWORD) {
            KillTimer(hwnd,id);
            EnumThreadWindows(GetCurrentThreadId(),[](HWND window,LPARAM)->BOOL {
                wchar_t title[64]{};GetWindowTextW(window,title,64);
                if(IsWindowVisible(window) && wcscmp(title,L"PulseMenu")==0) GetWindowRect(window,&popup);
                return TRUE;
            },0);
            for(int i=0;i<down;++i) PostThreadMessageW(GetCurrentThreadId(),WM_KEYDOWN,VK_DOWN,0);
            PostThreadMessageW(GetCurrentThreadId(),WM_KEYDOWN,VK_RETURN,0);
        });
        HandleSettingsControl(s,hit);
        SetEnvironmentVariableW(L"PULSE_TEST_DROPDOWN_SHOT",nullptr);
        log<<"control left="<<origin.x<<" bottom="<<origin.y<<" popup="<<popup.left<<","<<popup.top<<","<<popup.right<<","<<popup.bottom<<std::endl;
        check(popup.left+ui::FluentMenu::kShadowMargin==origin.x,"popup left edge aligns with control instead of click position");
        check(popup.top+ui::FluentMenu::kShadowMargin==origin.y+static_cast<int>(std::lround(4*s.scale)),"popup opens below control with a consistent gap");
        check(popup.right-popup.left-2*ui::FluentMenu::kShadowMargin>=static_cast<int>(std::lround(control.right-control.left)),"popup is at least as wide as the control");
    }
    check(s.appPrefs.language==L"zh-CN","language selection still applies");
    check(s.appPrefs.window_effect==ui::WindowEffectId(ui::WindowEffect::None),"window effect selection still applies");
    Render(s);check(s.compositor.SaveSnapshot((base/L"closed-zh.png").c_str()),"Chinese selected value screenshot captured");
    log<<"failures="<<failures<<std::endl;return failures ? 1 : 0;
}

int RunSearchColumnsTest(AppState& s,const wchar_t* output) {
    using H=ui::HitTestResult;
    std::ofstream log{std::filesystem::path(output)};int failures=0;
    auto check=[&](bool ok,const char* label) {log<<(ok ? "[PASS] " : "[FAIL] ")<<label<<std::endl;if(!ok)++failures;};
    const auto base=std::filesystem::path(output).parent_path();
    check((s.settingsExpanded&2u)!=0,"storage and maintenance starts expanded");
    OpenSettingsTab(s,1);
    H disclosure;disclosure.region=H::SettingsDisclosure;disclosure.index=1;
    HandleSettingsControl(s,disclosure);check(!(s.settingsExpanded&2u),"storage can still collapse");
    HandleSettingsControl(s,disclosure);check((s.settingsExpanded&2u)!=0,"storage can reopen");
    Render(s);check(s.compositor.SaveSnapshot((base/L"storage-expanded.png").c_str()),"default expanded storage screenshot captured");
    // Pre-1.0.39 sessions stored divider ratios; they read back as automatic widths.
    const std::array<float,4> saved{0.31f,0.43f,0.62f,0.82f};
    using K=ui::MainRenderer::ColumnKind;
    for(float scale:{1.0f,1.25f,1.5f,2.0f}) {
        s.renderer.SetScale(scale);
        bool compact=true,positive=true,filled=true,dragged=true,legacy=true;
        for(float width:{500.0f,800.0f,1440.0f,2400.0f}) {
            const auto pane=D2D1::RectF(0,0,width*scale,600*scale);
            const auto automatic=s.renderer.DetailsColumns(pane,{},true,{});
            for(const auto& dividers:{std::array<float,4>{},saved}) {
                const auto c=s.renderer.DetailsColumns(pane,{},true,dividers);
                compact &= c.Width(K::Date)<=176*scale+0.01f && c.Width(K::Type)<=196*scale+0.01f && c.Width(K::Size)<=112*scale+0.01f;
                float sum=0;for(int i=0;i<c.count;++i){positive &= c.widths[i]>0;sum+=c.widths[i];}
                filled &= std::abs(sum-(c.right-c.left))<0.05f;
                legacy &= c.count==automatic.count && std::abs(c.widths[0]-automatic.widths[0])<0.05f;
                if(c.Has(K::Path)) {
                    const auto moved=s.renderer.ResizeSearchColumnDivider(pane,dividers,0,c.DividerX(0)-40*scale);
                    const auto after=s.renderer.DetailsColumns(pane,{},true,moved);
                    dragged &= std::abs(after.widths[1]-c.widths[1]-40*scale)<0.05f;
                }
                if(width>=1440) compact &= c.widths[0]+c.widths[1]>c.Width(K::Date)+c.Width(K::Type)+c.Width(K::Size);
            }
        }
        check(compact,"fitted metadata columns stay compact and leave room for name/path");
        check(positive && filled,"columns stay positive and fill the available width");
        check(dragged,"name/path divider drag keeps its requested width");
        check(legacy,"legacy divider ratios fall back to fitted widths");
    }
    s.renderer.SetScale(s.scale);
    ui::WindowViewModel vm;vm.dark=s.darkMode;vm.window_effect=ui::WindowEffect::None;
    vm.tabs.push_back({L"Search results",true});
    const float width=static_cast<float>(s.compositor.Width()),height=static_cast<float>(s.compositor.Height());
    ui::PaneSlotView slot;slot.rect=D2D1::RectF(220*s.scale,100*s.scale,width-8*s.scale,height-32*s.scale);slot.focused=true;
    slot.pane.header_text=L"Search results";slot.pane.is_search=true;slot.pane.search_query=L"setup";
    slot.pane.search_column_dividers=saved;
    for(int i=0;i<5;++i) {
        ui::ListEntryView row;row.name=L"setup-diagnostic-025d13-project-report-"+std::to_wstring(i)+L".log";
        row.path=L"C:\\Program Files\\Pulse\\Diagnostics\\September\\"+row.name;
        row.type_text=L"Text document";row.date_text=L"2026-09-11 12:50";row.size_text=L"539 B";
        slot.pane.entries.push_back(std::move(row));
    }
    vm.pane_slots.push_back(std::move(slot));
    auto* dc=s.compositor.Dc();dc->BeginDraw();
    s.renderer.Render(vm,D2D1::RectF(0,0,width,height),ui::MakeTheme(s.darkMode,s.accentColor));
    check(SUCCEEDED(dc->EndDraw()),"search result columns render successfully");
    check(s.compositor.SaveSnapshot((base/L"search-columns.png").c_str()),"long name/path and compact metadata screenshot captured");
    log<<"failures="<<failures<<std::endl;return failures ? 1 : 0;
}

int RunAdaptiveColumnsTest(AppState& s, const wchar_t* output) {
    std::ofstream log{std::filesystem::path(output)};
    int failures = 0;
    auto check = [&](bool ok, const char* label) {
        log << (ok ? "[PASS] " : "[FAIL] ") << label << std::endl;
        if (!ok) ++failures;
    };
    const auto base = std::filesystem::path(output).parent_path();
    const float original_scale = s.scale;
    if (auto* tab = ActiveTab(s)) {
        const auto saved_path = tab->current_path;
        const auto saved_view = tab->view_mode;
        const auto saved_details = tab->details_column_dividers;
        const auto saved_search = tab->search_column_dividers;
        // Reuse the isolated application's tab; no navigation or disk fixture is needed.
        tab->current_path = L"C:\\";
        tab->view_mode = ui::ViewMode::Details;
        tab->details_column_dividers = {0.48f, 0.70f, 0.86f};
        tab->search_column_dividers = {0.28f, 0.48f, 0.70f, 0.86f};
        const auto vm = BuildVm(s, false);
        const auto window = D2D1::RectF(0, 0, static_cast<float>(s.compositor.Width()),
            static_cast<float>(s.compositor.Height()));
        bool exercised = false;
        for (size_t i = 0; i < vm.pane_slots.size(); ++i) {
            if (PaneAtSlot(s, static_cast<int>(i)) != s.pane) continue;
            const auto& slot = vm.pane_slots[i];
            const auto columns = s.renderer.DetailsColumns(slot.rect, slot.pane);
            const auto list = s.renderer.PaneListRect(slot.pane, slot.rect);
            const int x = static_cast<int>(std::lround(columns.DividerX(0)));
            const int y = static_cast<int>(std::lround(list.top - 8 * s.scale));
            const auto hit = s.renderer.HitTest(vm, window, static_cast<float>(x), static_cast<float>(y));
            check(hit.region == ui::HitTestResult::ColumnDivider && hit.pane_index == static_cast<int>(i),
                "double-click fixture hits the active tab's actual column divider");
            if (hit.region != ui::HitTestResult::ColumnDivider) break;
            s.columnResizing = true;
            s.columnResizeIndex = hit.index;
            s.columnResizePane = hit.pane_index;
            SetCapture(s.hwnd);
            check(GetCapture() == s.hwnd, "double-click fixture begins with mouse capture");
            HandleLButtonDblClk(&s, s.hwnd, WM_LBUTTONDBLCLK, MK_LBUTTON, MAKELPARAM(x, y));
            check(tab->details_column_dividers[0] == 0.0f &&
                  tab->search_column_dividers == std::array<float, 4>{0.28f, 0.48f, 0.70f, 0.86f},
                "actual divider double-click refits adjacent columns without changing the other layout");
            check(!s.columnResizing && s.columnResizeIndex == -1 && s.columnResizePane == -1 &&
                  GetCapture() != s.hwnd,
                "actual divider double-click stops resizing and releases mouse capture");
            exercised = true;
            break;
        }
        check(exercised, "actual divider double-click regression was exercised");
        if (GetCapture() == s.hwnd) ReleaseCapture();
        s.columnResizing = false;
        s.columnResizeIndex = -1;
        s.columnResizePane = -1;
        tab->current_path = saved_path;
        tab->view_mode = saved_view;
        tab->details_column_dividers = saved_details;
        tab->search_column_dividers = saved_search;
    } else {
        check(false, "isolated application supplies a tab for the double-click regression");
    }
    auto fixture = [](bool search, bool long_type) {
        ui::PaneViewModel pane;
        pane.is_search = search;
        pane.header_text = search ? L"搜索结果 · 自适应列宽" : L"文件夹 · 自适应列宽";
        const wchar_t* types[] = { L"文件", L"应用程序", L"压缩文件",
            L"Microsoft PowerPoint Presentation", L"带有较长中文类型名称的项目归档文件" };
        for (int i = 0; i < (long_type ? 5 : 3); ++i) {
            ui::ListEntryView row;
            row.name = L"2026-项目资料-Quarterly-report-" + std::to_wstring(i) + L".dat";
            row.path = L"C:\\Projects\\季度资料\\Archive\\" + row.name;
            row.type_text = types[i];
            row.date_text = L"2026-09-21 07:44";
            row.size_text = i == 0 ? L"321.9 MB" : (i == 1 ? L"321.9MB" : L"999.99 GB");
            pane.entries.push_back(std::move(row));
        }
        return pane;
    };
    for (float scale : {1.0f, 1.5f, 2.0f}) {
        s.compositor.RecreateTextFormats(scale);
        s.renderer.SetScale(scale);
        auto measure = [&](const std::wstring& value) {
            ui::ComPtr<IDWriteTextLayout> layout;
            const HRESULT hr = s.compositor.DwriteFactory()->CreateTextLayout(value.c_str(),
                static_cast<UINT32>(value.size()), s.compositor.TextFormat(),
                10000.0f * scale, 100.0f * scale, &layout);
            DWRITE_TEXT_METRICS metrics{};
            if (FAILED(hr) || !layout.get() || FAILED(layout->GetMetrics(&metrics))) return 100000.0f;
            float luma = 0.0f;
            s.compositor.MeasureLumaText(value, s.compositor.TextFormat(), luma);
            return std::max(metrics.widthIncludingTrailingWhitespace, luma);
        };
        for (bool search : {false, true}) {
            log << "scale=" << scale << " search=" << search << std::endl;
            for (float width : {280.0f, 480.0f, 600.0f, 800.0f, 1440.0f}) {
                const auto bounds = D2D1::RectF(0, 0, width * scale, 600 * scale);
                auto pane = fixture(search, true);
                const auto columns = s.renderer.DetailsColumns(bounds, pane);
                float sum = 0.0f;
                bool positive = true;
                for (int i = 0; i < columns.count; ++i) {
                    positive &= std::isfinite(columns.widths[i]) && columns.widths[i] > 0;
                    sum += columns.widths[i];
                }
                check(positive && columns.Has(ui::MainRenderer::ColumnKind::Name) &&
                    columns.Has(ui::MainRenderer::ColumnKind::Size),
                    "normal and search columns remain positive at narrow widths and every DPI");
                check(std::abs(sum - (columns.right - columns.left)) < 0.1f,
                    "adaptive columns exactly fill the available pane width");
                if (width >= 600 && columns.Has(ui::MainRenderer::ColumnKind::Date)) {
                    bool compact_fields_fit = true;
                    for (const auto& row : pane.entries) {
                        compact_fields_fit &= measure(row.date_text) + 20 * scale <=
                            columns.Width(ui::MainRenderer::ColumnKind::Date) + 0.1f;
                        compact_fields_fit &= measure(row.size_text) + 20 * scale <=
                            columns.widths[columns.count - 1] + 0.1f;
                    }
                    check(compact_fields_fit, "long type descriptions do not squeeze dates or sizes");
                }
                if (width >= 800 && columns.Has(ui::MainRenderer::ColumnKind::Type)) {
                    bool fits = true;
                    for (const auto& row : pane.entries) {
                        const std::wstring* values[] = { &row.date_text, &row.type_text, &row.size_text };
                        for (int i = 0; i < 3; ++i)
                            fits &= measure(*values[i]) + 20 * scale <=
                                columns.widths[columns.count - 3 + i] + 0.1f;
                    }
                    check(fits, "actual rendered font fits full dates, sizes, and long Chinese/English types");
                }
                const auto short_columns = s.renderer.DetailsColumns(bounds, fixture(search, false));
                if (width >= 800 && columns.Has(ui::MainRenderer::ColumnKind::Type))
                    check(columns.widths[columns.count - 2] > short_columns.widths[columns.count - 2],
                        "long type descriptions expand the type column beyond short labels");
                if (width >= 800 && columns.Has(ui::MainRenderer::ColumnKind::Type)) {
                    auto captured = pane;
                    using K = ui::MainRenderer::ColumnKind;
                    captured.details_column_dividers = {
                        columns.Width(K::Date) / scale, columns.Width(K::Type) / scale,
                        columns.Width(K::Size) / scale};
                    captured.search_column_dividers = {
                        columns.Has(K::Path) ? columns.Width(K::Name) / scale : 0.0f,
                        columns.Width(K::Date) / scale, columns.Width(K::Type) / scale,
                        columns.Width(K::Size) / scale};
                    const auto restored = s.renderer.DetailsColumns(bounds, captured);
                    bool stable = true;
                    for (int i = 0; i < columns.count; ++i)
                        stable &= std::abs(restored.widths[i] - columns.widths[i]) < 0.1f;
                    check(stable, "capturing measured layout as manual DIP widths preserves every column without a jump");
                }
                pane.details_column_dividers = {140.0f, 150.0f, 90.0f};
                pane.search_column_dividers = {200.0f, 140.0f, 150.0f, 90.0f};
                const auto manual = s.renderer.DetailsColumns(bounds, pane);
                const auto expected = s.renderer.DetailsColumns(bounds, pane.details_column_dividers,
                    search, pane.search_column_dividers);
                bool preserved = true;
                for (int i = 0; i < manual.count; ++i)
                    preserved &= std::abs(manual.widths[i] - expected.widths[i]) < 0.01f;
                check(preserved, "explicit manual divider layout overrides content measurement");
            }
        }
        for (bool dark : {false, true}) {
            for (bool narrow : {false, true}) {
                const float width = (narrow ? 760.0f : 1500.0f) * scale;
                const float height = 820.0f * scale;
                s.compositor.Resize(static_cast<UINT>(width), static_cast<UINT>(height));
                ui::WindowViewModel vm;
                vm.dark = dark;
                vm.window_effect = ui::WindowEffect::None;
                vm.tabs.push_back({L"自适应列宽 / Adaptive columns", true});
                for (int i = 0; i < 2; ++i) {
                    ui::PaneSlotView slot;
                    slot.rect = D2D1::RectF(220 * scale, (100 + i * 338) * scale,
                        width - 12 * scale, (420 + i * 338) * scale);
                    slot.focused = i == 0;
                    slot.pane = fixture(i == 1, true);
                    vm.pane_slots.push_back(std::move(slot));
                }
                auto* dc = s.compositor.Dc();
                dc->BeginDraw();
                s.renderer.Render(vm, D2D1::RectF(0, 0, width, height), ui::MakeTheme(dark, s.accentColor));
                check(SUCCEEDED(dc->EndDraw()), "adaptive columns render without Direct2D errors");
                const auto name = std::wstring(L"adaptive-columns-") + (dark ? L"dark-" : L"light-") +
                    (narrow ? L"narrow-" : L"wide-") + std::to_wstring(static_cast<int>(scale * 100)) + L".png";
                check(s.compositor.SaveSnapshot((base / name).c_str()), "adaptive column theme/DPI screenshot captured");
                auto& strip = vm.pane_slots[0].pane.column_strip;
                strip.enabled = strip.eligible = strip.has_child = true;
                auto entries = std::make_shared<std::vector<fs::DirEntry>>(3);
                auto rows = std::make_shared<std::vector<int>>(std::initializer_list<int>{0, 1, 2});
                for (size_t i = 0; i < entries->size(); ++i) {
                    (*entries)[i].name = L"项目文件夹 / Folder " + std::to_wstring(i + 1);
                    (*entries)[i].is_dir = true;
                }
                ui::ColumnStripColumnView column;
                column.title = L"父目录 / Parent";
                column.snapshot = entries;
                column.rows = rows;
                column.highlight_row = 1;
                strip.ancestors = {column, column, column};
                strip.child = column;
                strip.child.title = L"子目录 / Child";
                dc->BeginDraw();
                s.renderer.Render(vm, D2D1::RectF(0, 0, width, height), ui::MakeTheme(dark, s.accentColor));
                check(SUCCEEDED(dc->EndDraw()), "ancestor/current/child columns render across themes and DPI");
                check(s.compositor.SaveSnapshot((base / (L"strip-" + name)).c_str()),
                    "column strip theme/DPI screenshot captured");
            }
        }
    }
    s.compositor.RecreateTextFormats(original_scale);
    s.renderer.SetScale(original_scale);
    log << "failures=" << failures << std::endl;
    return failures ? 1 : 0;
}

int RunSettingsSmoothScrollTest(AppState& s, const wchar_t* output) {
    std::ofstream log{std::filesystem::path(output)};
    int failures = 0;
    auto check = [&](bool ok, const char* label) {
        log << (ok ? "[PASS] " : "[FAIL] ") << label << '\n';
        if (!ok) ++failures;
    };
    s.compositor.Resize(1000, 700);
    OpenSettingsTab(s, 0);
    const float file_offset = ActiveTab(s)->scroll_y;
    auto maximum = [&] {
        return s.renderer.SettingsMaxScroll(BuildVm(s, false),
            static_cast<float>(s.compositor.Width()), static_cast<float>(s.compositor.Height()));
    };
    auto wheel = [&](short delta) {
        HandleMouseWheel(&s, s.hwnd, WM_MOUSEWHEEL, MAKEWPARAM(0, delta), 0);
    };
    auto frame = [&] {
        s.scrollLastUpdateTime = std::chrono::steady_clock::now() - std::chrono::milliseconds(16);
        UpdateSmoothScroll(s);
    };
    for (float scale : {1.0f, 1.5f, 2.0f}) {
        s.scale = scale;
        s.renderer.SetScale(scale);
        s.compositor.RecreateTextFormats(scale);
        s.settings.SetScroll(0, maximum());
        wheel(-120);
        check(s.scrollAnimating && s.settings.scroll() == 0,
            "wheel starts animation without jumping at common DPI scales");
        frame();
        check(s.settings.scroll() > 0 && s.settings.scroll() < 48*scale,
            "first frame advances only part of the wheel distance");
        wheel(-120);
        check(s.scrollVelocityY > 48*scale / AppState::kScrollResponseMs,
            "repeated wheel input accumulates momentum");
        wheel(120);
        check(s.scrollVelocityY < 0, "reverse wheel input changes direction immediately");
        for (int i = 0; i < 100 && s.scrollAnimating; ++i) frame();
        check(!s.scrollAnimating && s.settings.scroll() >= 0 && s.settings.scroll() <= maximum(),
            "animation settles within settings bounds");
        s.settings.SetScroll(maximum() - 1, maximum());
        wheel(-120); frame();
        check(!s.scrollAnimating && s.settings.scroll() == maximum(), "bottom edge stops momentum");
        s.settings.SetScroll(1, maximum());
        wheel(120); frame();
        check(!s.scrollAnimating && s.settings.scroll() == 0, "top edge stops momentum");
        check(ActiveTab(s)->scroll_y == file_offset, "settings animation preserves file scroll offset");
    }
    wheel(-120);
    OpenSettingsTab(s, 3);
    check(!s.scrollAnimating && s.settings.scroll() == 0, "page switch cancels old animation");
    s.compositor.Resize(1000, 4000);
    check(maximum() == 0, "large viewport has no settings overflow");
    wheel(-120);
    check(!s.scrollAnimating && s.settings.scroll() == 0, "non-scrollable page stays idle");
    s.compositor.Resize(1000, 700);
    OpenSettingsTab(s, 0);
    wheel(-120);
    ui::HitTestResult disclosure;
    disclosure.region = ui::HitTestResult::SettingsDisclosure;
    disclosure.index = 0;
    HandleSettingsControl(s, disclosure);
    check(!s.scrollAnimating && s.settings.scroll() <= maximum(), "disclosure cancels and clamps animation");
    s.settings.SetScroll(150, maximum());
    Render(s);
    check(s.compositor.SaveSnapshot((std::filesystem::path(output).parent_path() /
        L"settings-smooth-scroll.png").c_str()), "scrolled settings frame captured");
    log << "failures=" << failures << '\n';
    return failures ? 1 : 0;
}

int RunSettingsFlowTest(AppState& s,const wchar_t* output) {
    if (GetEnvironmentVariableW(L"PULSE_TEST_SETTINGS_SMOOTH_SCROLL", nullptr, 0))
        return RunSettingsSmoothScrollTest(s, output);
    if (GetEnvironmentVariableW(L"PULSE_TEST_ADAPTIVE_COLUMNS", nullptr, 0))
        return RunAdaptiveColumnsTest(s, output);
    if (GetEnvironmentVariableW(L"PULSE_TEST_SEARCH_COLUMNS", nullptr, 0))
        return RunSearchColumnsTest(s, output);
    if (GetEnvironmentVariableW(L"PULSE_TEST_SETTINGS_DROPDOWN", nullptr, 0))
        return RunSettingsDropdownTest(s, output);
    if (GetEnvironmentVariableW(L"PULSE_TEST_SETTINGS_WIDTH_BORDER", nullptr, 0))
        return RunSettingsWidthBorderTest(s, output);
    using H=ui::HitTestResult;using I=l10n::StringId;
    std::ofstream log{std::filesystem::path(output)};int failures=0;
    auto check=[&](bool ok,const char* label){log<<(ok ? "[PASS] " : "[FAIL] ")<<label<<'\n';if(!ok)++failures;};
    s.appPrefs.persist=false;
    app::AppPrefs prefs; prefs.persist=false;
    const auto color_close=[](float a,float b) { return std::fabs(a-b)<0.001f; };
    auto pine=ResolveAccentColor(prefs,false);
    check(color_close(pine.r,82/255.0f) && color_close(pine.g,125/255.0f) && color_close(pine.b,112/255.0f),
        "light theme defaults to concept pine green");
    const auto charcoal=ui::MakeTheme(true,pine);
    check(color_close(charcoal.bg.r,26/255.0f) && color_close(charcoal.bg.r,charcoal.bg.g) &&
        color_close(charcoal.surface_sheet.r,charcoal.surface_sheet.b),"dark theme restores neutral charcoal backgrounds");
    const auto warm=ui::MakeTheme(false,pine);
    check(color_close(warm.bg.r,243/255.0f) && color_close(warm.bg.g,245/255.0f) && color_close(warm.bg.b,241/255.0f),
        "light theme uses concept warm canvas");
    prefs.accent_follow_system=true;
    app::AppPrefs accent_roundtrip; accent_roundtrip.persist=false;
    check(accent_roundtrip.FromJson(prefs.ToJson()) && accent_roundtrip.accent_follow_system,
        "explicit Windows accent survives preference round trip");
    prefs.accent_rgb=L"8861AA";
    auto custom=ResolveAccentColor(prefs,false);
    check(color_close(custom.r,136/255.0f) && color_close(custom.b,170/255.0f),"custom accent overrides theme palette");
    prefs.FromJson(L"{\"language\":\"en-US\"}");check(prefs.theme_mode==-1,"old profiles retain session theme");
    for(int mode=0;mode<3;++mode) {
        prefs.theme_mode=mode;const auto json=prefs.ToJson();app::AppPrefs loaded;loaded.persist=false;
        check(loaded.FromJson(json) && loaded.theme_mode==mode,"theme preference round trip");
    }
    prefs.FromJson(L"{\"theme_mode\":99}");check(prefs.theme_mode==-1,"invalid theme preference has legacy fallback");
    for(const auto* language:{L"zh-CN",L"en-US"}) {
        l10n::SetLanguage(language);bool complete=true;
        for(int id=1900;id<=1929;++id) complete &= !l10n::Get(static_cast<I>(id)).empty();
        check(complete,"new settings labels exist in both languages");
        check(TestSettingsFilter(l10n::Get(I::SettingsWallpaper),I::SettingsWallpaper),"settings search finds hidden advanced settings");
        check(TestSettingsFilter(l10n::Get(I::SettingsSearchIndex),I::IndexLocation),"settings search finds page and subsettings");
        check(!TestSettingsFilter(L"no-such-setting-123",I::SettingsTheme),"settings search has empty results");
    }
    l10n::SetLanguage(L"zh-CN");
    const float original_scale=s.scale;
    ui::fluent::Painter painter(&s.compositor);
    for(float scale:{1.0f,1.5f,2.0f}) {
        s.compositor.RecreateTextFormats(scale);s.renderer.SetScale(scale);painter.SetScale(scale);
        for(float width:{720.0f,820.0f,1280.0f,1920.0f}) {
            const auto window=D2D1::RectF(0,0,width*scale,1000*scale);
            auto vm=BuildVm(s,false);vm.settings_open=true;vm.settings_scroll=0;vm.settings_bloom=&s.bloom_accent;
            vm.settings_content_folders={{LR"(C:\Projects\Very long project folder name)",L"Ready",false},{LR"(D:\Unavailable)",L"Unavailable (3)",true}};
            for(int page=0;page<5;++page) {
                vm.settings_page=page;vm.settings_expanded=0;
                const auto layout=ui::MakeSettingsLayout(vm,window,scale,s.renderer.TitleBarHeight(),28*scale,&painter);
                auto hit=[&](D2D1_RECT_F r){return s.renderer.HitTest(vm,window,(r.left+r.right)/2,(r.top+r.bottom)/2);};
                bool nav=true;for(int i=0;i<5;++i) {auto h=hit(layout.nav_row[i]);nav &= h.region==H::SettingsNav && h.index==i;}
                check(nav,"fixed navigation preserves all page destinations across widths and DPI");
                check(layout.nav_row[0].top == layout.nav.top + 20*scale,
                    "settings navigation starts without a search-box gap");
                check(layout.nav_row[4].bottom<layout.nav_row[3].top,"about is pinned below primary navigation");
                if(page==0) {
                    check(hit(layout.theme_tile[0]).region==H::SettingsTheme && hit(layout.theme_tile[0]).index==1,"light theme preview hit target");
                    check(hit(layout.accent_picker).region==H::SettingsAccent && hit(layout.accent_picker).index==0,
                        "accent value input remains reachable");
                    check(hit(layout.accent_system).region==H::SettingsAccent && hit(layout.accent_system).index==1,
                        "system accent button remains reachable");
                    check(layout.effect_choice.right == 0 && hit(layout.language_choice).index==1,"window effect has no duplicate dropdown; language dropdown remains");
                    auto scrolled=vm;
                    scrolled.settings_scroll=layout.density_card.top-layout.content.top;
                    const auto density_layout=ui::MakeSettingsLayout(scrolled,window,scale,s.renderer.TitleBarHeight(),28*scale,&painter);
                    const auto density=density_layout.density_row[2];
                    check(s.renderer.HitTest(scrolled,window,(density.left+density.right)/2,(density.top+density.bottom)/2).region==H::SettingsDensity,"density segments remain reachable after scrolling into view");
                    check(layout.wallpaper_card.bottom==0 && layout.startup_row[2].bottom>0,
                        "default file manager remains visible when advanced settings are collapsed");
                    for(const auto* locale:{L"zh-CN",L"en-US"}) {
                        l10n::SetLanguage(locale);bool fits=true;
                        const I labels[]={I::SettingsDensityCompact,I::SettingsDensityStandard,I::SettingsDensityRoomy,I::SettingsTraySmall,I::SettingsTrayStandard,I::SettingsTrayLarge};
                        for(I id:labels) {
                            const auto& label=l10n::Get(id);ui::ComPtr<IDWriteTextLayout> measured;
                            s.compositor.DwriteFactory()->CreateTextLayout(label.c_str(),static_cast<UINT32>(label.size()),s.compositor.TextFormat(),2000*scale,100*scale,&measured);
                            DWRITE_TEXT_METRICS metrics{};if(measured.get()) measured->GetMetrics(&metrics);
                            fits &= measured.get() && metrics.widthIncludingTrailingWhitespace<=layout.density_row[0].right-layout.density_row[0].left-6*scale;
                        }
                        check(fits,"density and tray labels fit their segments in both languages");
                    }
                    l10n::SetLanguage(L"zh-CN");
                }
                if(page==2) {
                    bool headers=true;
                    for(int g=0;g<5;++g) {
                        auto h=hit(layout.context_header[g]);headers &= h.region==H::SettingsDisclosure && h.index==g+8;
                        h=hit(layout.context_toggle[g]);headers &= h.region==H::SettingsToggle && h.index==g+10;
                    }
                    check(headers,"context disclosure and group toggle have distinct hit targets");
                    bool hidden=true;for(const auto& r:layout.context_rows) hidden &= r.bottom==0;
                    check(hidden,"collapsed context entries do not receive clicks");
                }
                if(page==1) {
                    check(layout.index_action[0].bottom==0 && layout.index_volume_rows.empty(),"collapsed maintenance has no invisible hit targets");
                    check(hit(layout.disclosure[1]).region==H::SettingsDisclosure,"maintenance disclosure hit target");
                    check(hit(layout.content_header).region!=H::SettingsContentAction,"shared scope header has no maintenance hit target");
                    check(hit(layout.content_options).region==H::SettingsContentAction && hit(layout.content_options).index==2,"shared text encoding hit target");
                    check(layout.content_rebuild.bottom<=layout.group[1].bottom && layout.section[2].bottom==0,"rebuild lives inside content card without a redundant more-options section");
                }
            }
        }
    }
    s.compositor.RecreateTextFormats(original_scale);s.renderer.SetScale(original_scale);painter.SetScale(original_scale);
    OpenSettingsTab(s,0);s.settingsExpanded=0;s.settings.SetScroll(0,0);
    auto window=D2D1::RectF(0,0,static_cast<float>(s.compositor.Width()),static_cast<float>(s.compositor.Height()));
    auto layout=[&] {return ui::MakeSettingsLayout(BuildVm(s,false),window,s.scale,s.renderer.TitleBarHeight(),28*s.scale,&painter);};
    auto click=[&](D2D1_RECT_F r) {
        const int x=static_cast<int>((r.left+r.right)/2),y=static_cast<int>((r.top+r.bottom)/2);
        SendMessageW(s.hwnd,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(x,y));
        SendMessageW(s.hwnd,WM_LBUTTONUP,0,MAKELPARAM(x,y));
    };
    click(layout().theme_tile[0]);check(!s.darkMode && s.appPrefs.theme_mode==1,"mouse click applies and saves light theme");
    click(layout().theme_tile[1]);check(s.darkMode && s.appPrefs.theme_mode==2,"mouse click applies and saves dark theme");
    click(layout().theme_tile[2]);check(s.themeOverride==ui::ThemeMode::Auto && s.appPrefs.theme_mode==0,"system theme is a persistent explicit selection");
    s.settings.SetScroll(layout().density_card.top-layout().content.top,
        s.renderer.SettingsMaxScroll(BuildVm(s,false),window.right,window.bottom));
    click(layout().density_row[0]);check(s.appPrefs.row_height==28,"mouse click changes density through existing controller");
    s.settings.SetScroll(0,0);
    auto vm=BuildVm(s,false);
    const auto collapsed_max=s.renderer.SettingsMaxScroll(vm,window.right,window.bottom);
    H toggle;toggle.region=H::SettingsDisclosure;toggle.index=0;HandleSettingsControl(s,toggle);
    vm=BuildVm(s,false);const auto expanded_max=s.renderer.SettingsMaxScroll(vm,window.right,window.bottom);
    check((s.settingsExpanded&1u) && expanded_max>collapsed_max,"expanding advanced settings updates scroll range");
    vm.settings_scroll = expanded_max;
    const auto advanced_layout = ui::MakeSettingsLayout(vm,window,s.scale,
        s.renderer.TitleBarHeight(),28*s.scale,&painter);
    const auto percent_rect = advanced_layout.wallpaper_look_value;
    check(percent_rect.right>percent_rect.left,
        "background percentage input is laid out in expanded settings");
    {
        // The advanced group owns both the hidden-files switch and the protected
        // operating system files switch; both need a reachable row and fitting text.
        auto advanced=BuildVm(s,false);advanced.settings_expanded|=1u;advanced.settings_scroll=0;
        const auto lay=ui::MakeSettingsLayout(advanced,window,s.scale,s.renderer.TitleBarHeight(),28*s.scale,&painter);
        check(lay.protected_files_row.top>=lay.hidden_files_row.bottom &&
            lay.protected_files_row.bottom-lay.protected_files_row.top>=64*s.scale-1.0f &&
            lay.protected_files_row.bottom<=lay.footer.top,
            "protected system files row sits between the hidden row and the footer");
        const float text_width=lay.hidden_files_row.right-lay.hidden_files_row.left-126*s.scale;
        for(const auto* locale:{L"zh-CN",L"en-US"}) {
            l10n::SetLanguage(locale);bool fits=true;
            // An out-of-range id resolves to an empty string, which would also fit.
            check(!l10n::Get(I::SettingsShowProtected).empty() &&
                !l10n::Get(I::SettingsShowProtectedDesc).empty(),
                "protected system files labels exist in both languages");
            const I descriptions[]={I::SettingsShowHiddenDesc,I::SettingsShowProtectedDesc};
            for(I id:descriptions) {
                const auto& value=l10n::Get(id);ui::ComPtr<IDWriteTextLayout> measured;
                s.compositor.DwriteFactory()->CreateTextLayout(value.c_str(),static_cast<UINT32>(value.size()),
                    s.compositor.SmallFormat(),4000*s.scale,100*s.scale,&measured);
                DWRITE_TEXT_METRICS metrics{};if(measured.get()) measured->GetMetrics(&metrics);
                fits &= measured.get() && metrics.widthIncludingTrailingWhitespace<=text_width;
            }
            check(fits,"hidden and protected descriptions fit the row in both languages");
        }
        l10n::SetLanguage(L"zh-CN");
        // Scrolled into view, both rows must answer with their own toggle target.
        s.settings.SetScroll(lay.hidden_files_row.top-lay.content.top,10000);
        auto scrolled=BuildVm(s,false);
        const auto visible=ui::MakeSettingsLayout(scrolled,window,s.scale,s.renderer.TitleBarHeight(),28*s.scale,&painter);
        auto row_hit=[&](D2D1_RECT_F r) {
            return s.renderer.HitTest(scrolled,window,(r.left+r.right)/2,(r.top+r.bottom)/2); };
        const auto hidden=row_hit(visible.hidden_files_row);
        const auto guarded=row_hit(visible.protected_files_row);
        check(hidden.region==H::SettingsToggle && hidden.index==5,"hidden files row keeps its toggle target");
        check(guarded.region==H::SettingsToggle && guarded.index==16,"protected system files row exposes its own toggle");
        Render(s);
        check(s.compositor.SaveSnapshot((std::filesystem::path(output).parent_path()/L"settings-advanced.png").c_str()),
            "advanced settings screenshot captured");
    }
    s.settings.SetScroll(expanded_max,expanded_max);HandleSettingsControl(s,toggle);
    check(s.settings.scroll()<=collapsed_max,"collapsing advanced settings clamps existing scroll");
    OpenSettingsTab(s,1);s.settings.SetScroll(0,0);
    const bool before=s.appPrefs.search_pinyin;click(layout().search_pinyin_row);
    check(s.appPrefs.search_pinyin!=before,"mouse click changes pinyin setting");
    toggle.index=1;HandleSettingsControl(s,toggle);vm=BuildVm(s,false);
    const float offset=s.renderer.SettingsDestinationOffset(vm,static_cast<int>(I::IndexLocation),window.right,window.bottom);
    check(offset>0 && (s.settingsExpanded&2u),"search destination locates expanded index storage section");
    log<<"failures="<<failures<<'\n';return failures ? 1 : 0;
}
#endif
